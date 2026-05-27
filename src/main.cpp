#include <android/input.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>

namespace {

constexpr const char* kLogTag = "InventoryTouchFollow";
constexpr const char* kPreloaderLibrary = "libpreloader.so";
constexpr std::int64_t kMotionLogIntervalNs = 120'000'000LL;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

using GHook = void*;
using GlossInitFn = void (*)(bool);
using GlossHookFn = GHook (*)(void*, void*, void**);
using AInputQueueGetEventFn = int32_t (*)(AInputQueue*, AInputEvent**);

GlossInitFn g_glossInit = nullptr;
GlossHookFn g_glossHook = nullptr;
AInputQueueGetEventFn g_oldGetEvent = nullptr;
GHook g_inputHook = nullptr;

std::atomic<std::int64_t> g_lastMotionLogNs{0};
std::atomic<std::uint64_t> g_motionCount{0};

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename T>
T Symbol(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}

const char* ActionName(int32_t action) {
    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN: return "DOWN";
        case AMOTION_EVENT_ACTION_UP: return "UP";
        case AMOTION_EVENT_ACTION_MOVE: return "MOVE";
        case AMOTION_EVENT_ACTION_CANCEL: return "CANCEL";
        case AMOTION_EVENT_ACTION_OUTSIDE: return "OUTSIDE";
        case AMOTION_EVENT_ACTION_POINTER_DOWN: return "POINTER_DOWN";
        case AMOTION_EVENT_ACTION_POINTER_UP: return "POINTER_UP";
        case AMOTION_EVENT_ACTION_HOVER_MOVE: return "HOVER_MOVE";
        case AMOTION_EVENT_ACTION_SCROLL: return "SCROLL";
        case AMOTION_EVENT_ACTION_HOVER_ENTER: return "HOVER_ENTER";
        case AMOTION_EVENT_ACTION_HOVER_EXIT: return "HOVER_EXIT";
        case AMOTION_EVENT_ACTION_BUTTON_PRESS: return "BUTTON_PRESS";
        case AMOTION_EVENT_ACTION_BUTTON_RELEASE: return "BUTTON_RELEASE";
        default: return "OTHER";
    }
}

const char* ToolName(int32_t tool) {
    switch (tool) {
        case AMOTION_EVENT_TOOL_TYPE_FINGER: return "FINGER";
        case AMOTION_EVENT_TOOL_TYPE_STYLUS: return "STYLUS";
        case AMOTION_EVENT_TOOL_TYPE_MOUSE: return "MOUSE";
        case AMOTION_EVENT_TOOL_TYPE_ERASER: return "ERASER";
        default: return "UNKNOWN";
    }
}

bool ShouldLogMotion(int32_t maskedAction) {
    const bool frequent = maskedAction == AMOTION_EVENT_ACTION_MOVE ||
                          maskedAction == AMOTION_EVENT_ACTION_HOVER_MOVE;
    if (!frequent) {
        return true;
    }

    const std::int64_t now = NowNs();
    std::int64_t previous = g_lastMotionLogNs.load(std::memory_order_relaxed);
    if (now - previous < kMotionLogIntervalNs) {
        return false;
    }
    return g_lastMotionLogNs.compare_exchange_strong(
        previous, now, std::memory_order_relaxed);
}

void RecordMotion(AInputEvent* event) {
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t maskedAction = action & AMOTION_EVENT_ACTION_MASK;
    if (!ShouldLogMotion(maskedAction)) {
        return;
    }

    const std::size_t pointerCount = AMotionEvent_getPointerCount(event);
    if (pointerCount == 0) {
        return;
    }

    std::size_t pointerIndex = static_cast<std::size_t>(
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (pointerIndex >= pointerCount) {
        pointerIndex = 0;
    }

    const float x = AMotionEvent_getX(event, pointerIndex);
    const float y = AMotionEvent_getY(event, pointerIndex);
    const int32_t source = AInputEvent_getSource(event);
    const int32_t tool = AMotionEvent_getToolType(event, pointerIndex);
    const int32_t buttons = AMotionEvent_getButtonState(event);
    const int32_t deviceId = AInputEvent_getDeviceId(event);
    const std::uint64_t count = g_motionCount.fetch_add(1, std::memory_order_relaxed) + 1;

    LOGI("INPUT #%" PRIu64 " action=%s(%d) source=0x%08x tool=%s(%d) buttons=0x%x device=%d pointer=%zu/%zu pos=(%.1f,%.1f)",
         count,
         ActionName(maskedAction), maskedAction,
         source,
         ToolName(tool), tool,
         buttons,
         deviceId,
         pointerIndex, pointerCount,
         static_cast<double>(x), static_cast<double>(y));
}

int32_t HookGetEvent(AInputQueue* queue, AInputEvent** outEvent) {
    const int32_t result = g_oldGetEvent ? g_oldGetEvent(queue, outEvent) : -1;
    if (result >= 0 && outEvent != nullptr && *outEvent != nullptr &&
        AInputEvent_getType(*outEvent) == AINPUT_EVENT_TYPE_MOTION) {
        RecordMotion(*outEvent);
    }
    return result;
}

bool ResolvePreloaderApi() {
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
    void* preloader = nullptr;
    for (int attempt = 0; attempt < 120 && preloader == nullptr; ++attempt) {
        preloader = dlopen(kPreloaderLibrary, RTLD_NOW | RTLD_NOLOAD);
        if (preloader == nullptr) {
            usleep(250 * 1000);
        }
    }
    if (preloader == nullptr) {
        LOGE("%s was not already loaded", kPreloaderLibrary);
        return false;
    }

    g_glossInit = Symbol<GlossInitFn>(preloader, "GlossInit");
    g_glossHook = Symbol<GlossHookFn>(preloader, "GlossHook");
    if (!g_glossInit || !g_glossHook) {
        LOGE("required preloader exports missing: Init=%p Hook=%p",
             reinterpret_cast<void*>(g_glossInit), reinterpret_cast<void*>(g_glossHook));
        return false;
    }

    g_glossInit(true);
    return true;
}

void InstallInputProbe() {
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
    void* androidLibrary = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!androidLibrary) {
        androidLibrary = dlopen("libandroid.so", RTLD_NOW);
    }
    if (!androidLibrary) {
        LOGE("could not open libandroid.so");
        return;
    }

    void* inputTarget = dlsym(androidLibrary, "AInputQueue_getEvent");
    if (!inputTarget) {
        LOGE("AInputQueue_getEvent not found");
        return;
    }

    g_inputHook = g_glossHook(inputTarget,
                              reinterpret_cast<void*>(&HookGetEvent),
                              reinterpret_cast<void**>(&g_oldGetEvent));
    if (!g_inputHook || !g_oldGetEvent) {
        LOGE("input hook install failed");
        return;
    }
    LOGI("installed input-only probe: no Minecraft renderer functions are hooked");
    LOGI("test order: open inventory, move mouse over preview, then drag finger in the same area");
}

void* InitThread(void*) {
    LOGI("module loaded: JsonUI pointer input probe v2 (renderer hooks disabled)");
    if (!ResolvePreloaderApi()) {
        return nullptr;
    }
    InstallInputProbe();
    return nullptr;
}

__attribute__((constructor)) void ModConstructor() {
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, &InitThread, nullptr) != 0) {
        LOGE("failed to create initialization thread");
        return;
    }
    pthread_detach(thread);
}

} // namespace
