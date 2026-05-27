#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr const char* kLogTag = "InventoryTouchFollow";
constexpr const char* kPreloaderLibrary = "libpreloader.so";
constexpr const char* kMinecraftLibrary = "libminecraftpe.so";
constexpr std::int64_t kMotionLogIntervalNs = 90'000'000LL;

// Confirmed from the supplied libminecraftpe.so disassembly:
// GameActivityMotionEvent size used by android_app_clear_motion_events.
constexpr std::size_t kMotionEventSize = 0x6E0;
constexpr std::size_t kEventDeviceIdOffset = 0x00;
constexpr std::size_t kEventSourceOffset = 0x04;
constexpr std::size_t kEventActionOffset = 0x08;
constexpr std::size_t kEventPointerCountOffset = 0x38;
constexpr std::size_t kFirstPointerOffset = 0x3C;
constexpr std::size_t kPointerStride = 0xD0;
constexpr std::size_t kPointerIdOffset = 0x00;
constexpr std::size_t kPointerToolTypeOffset = 0x04;
constexpr std::size_t kPointerAxisXOffset = 0x08; // AMOTION_EVENT_AXIS_X = 0
constexpr std::size_t kPointerAxisYOffset = 0x0C; // AMOTION_EVENT_AXIS_Y = 1

constexpr std::int32_t kActionMask = 0xFF;
constexpr std::int32_t kActionPointerIndexMask = 0xFF00;
constexpr std::int32_t kActionPointerIndexShift = 8;

enum : std::int32_t {
    ACTION_DOWN = 0,
    ACTION_UP = 1,
    ACTION_MOVE = 2,
    ACTION_CANCEL = 3,
    ACTION_OUTSIDE = 4,
    ACTION_POINTER_DOWN = 5,
    ACTION_POINTER_UP = 6,
    ACTION_HOVER_MOVE = 7,
    ACTION_SCROLL = 8,
    ACTION_HOVER_ENTER = 9,
    ACTION_HOVER_EXIT = 10,
    ACTION_BUTTON_PRESS = 11,
    ACTION_BUTTON_RELEASE = 12,
};

enum : std::int32_t {
    TOOL_UNKNOWN = 0,
    TOOL_FINGER = 1,
    TOOL_STYLUS = 2,
    TOOL_MOUSE = 3,
    TOOL_ERASER = 4,
};

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

using GHook = void*;
using GlossInitFn = void (*)(bool);
using GlossHookFn = GHook (*)(void*, void*, void**);

// Mirrors the beginning of android_input_buffer from GameActivity native_app_glue.
// We only need the first two fields to read motion events before Minecraft processes them.
struct AndroidInputBufferPrefix {
    std::byte* motionEvents;
    std::uint64_t motionEventsCount;
    std::uint64_t motionEventsBufferSize;
};

using SwapInputBuffersFn = AndroidInputBufferPrefix* (*)(void* androidApp);

GlossInitFn g_glossInit = nullptr;
GlossHookFn g_glossHook = nullptr;
SwapInputBuffersFn g_oldSwapInputBuffers = nullptr;
GHook g_swapHook = nullptr;

std::atomic<std::int64_t> g_lastMotionLogNs{0};
std::atomic<std::uint64_t> g_motionCount{0};
std::atomic<bool> g_loggedFirstBuffer{false};

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename T>
T ReadValue(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

template <typename T>
T Symbol(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}

const char* ActionName(std::int32_t action) {
    switch (action) {
        case ACTION_DOWN: return "DOWN";
        case ACTION_UP: return "UP";
        case ACTION_MOVE: return "MOVE";
        case ACTION_CANCEL: return "CANCEL";
        case ACTION_OUTSIDE: return "OUTSIDE";
        case ACTION_POINTER_DOWN: return "POINTER_DOWN";
        case ACTION_POINTER_UP: return "POINTER_UP";
        case ACTION_HOVER_MOVE: return "HOVER_MOVE";
        case ACTION_SCROLL: return "SCROLL";
        case ACTION_HOVER_ENTER: return "HOVER_ENTER";
        case ACTION_HOVER_EXIT: return "HOVER_EXIT";
        case ACTION_BUTTON_PRESS: return "BUTTON_PRESS";
        case ACTION_BUTTON_RELEASE: return "BUTTON_RELEASE";
        default: return "OTHER";
    }
}

const char* ToolName(std::int32_t tool) {
    switch (tool) {
        case TOOL_FINGER: return "FINGER";
        case TOOL_STYLUS: return "STYLUS";
        case TOOL_MOUSE: return "MOUSE";
        case TOOL_ERASER: return "ERASER";
        default: return "UNKNOWN";
    }
}

bool ShouldLogMotion(std::int32_t maskedAction) {
    const bool frequent = maskedAction == ACTION_MOVE || maskedAction == ACTION_HOVER_MOVE;
    if (!frequent) {
        return true;
    }

    const std::int64_t now = NowNs();
    std::int64_t previous = g_lastMotionLogNs.load(std::memory_order_relaxed);
    if (now - previous < kMotionLogIntervalNs) {
        return false;
    }
    return g_lastMotionLogNs.compare_exchange_strong(previous, now, std::memory_order_relaxed);
}

void RecordMotion(const std::byte* event) {
    const std::int32_t action = ReadValue<std::int32_t>(event, kEventActionOffset);
    const std::int32_t maskedAction = action & kActionMask;
    if (!ShouldLogMotion(maskedAction)) {
        return;
    }

    std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
    if (pointerCount < 1 || pointerCount > 8) {
        LOGW("invalid GameActivity pointerCount=%d action=0x%x", pointerCount, action);
        return;
    }

    std::int32_t pointerIndex = (action & kActionPointerIndexMask) >> kActionPointerIndexShift;
    if (pointerIndex < 0 || pointerIndex >= pointerCount) {
        pointerIndex = 0;
    }

    const std::size_t pointer = kFirstPointerOffset + static_cast<std::size_t>(pointerIndex) * kPointerStride;
    const std::int32_t deviceId = ReadValue<std::int32_t>(event, kEventDeviceIdOffset);
    const std::int32_t source = ReadValue<std::int32_t>(event, kEventSourceOffset);
    const std::int32_t pointerId = ReadValue<std::int32_t>(event, pointer + kPointerIdOffset);
    const std::int32_t tool = ReadValue<std::int32_t>(event, pointer + kPointerToolTypeOffset);
    const float x = ReadValue<float>(event, pointer + kPointerAxisXOffset);
    const float y = ReadValue<float>(event, pointer + kPointerAxisYOffset);
    const std::uint64_t count = g_motionCount.fetch_add(1, std::memory_order_relaxed) + 1;

    LOGI("GA_INPUT #%" PRIu64 " action=%s(%d) source=0x%08x tool=%s(%d) device=%d pointerId=%d index=%d/%d pos=(%.1f,%.1f)",
         count,
         ActionName(maskedAction), maskedAction,
         static_cast<unsigned int>(source),
         ToolName(tool), tool,
         deviceId, pointerId, pointerIndex, pointerCount,
         static_cast<double>(x), static_cast<double>(y));
}

AndroidInputBufferPrefix* HookSwapInputBuffers(void* androidApp) {
    AndroidInputBufferPrefix* buffer = g_oldSwapInputBuffers ? g_oldSwapInputBuffers(androidApp) : nullptr;
    if (!buffer || !buffer->motionEvents || buffer->motionEventsCount == 0) {
        return buffer;
    }

    if (!g_loggedFirstBuffer.exchange(true, std::memory_order_relaxed)) {
        LOGI("first GameActivity motion buffer received: count=%" PRIu64 " capacity=%" PRIu64,
             buffer->motionEventsCount, buffer->motionEventsBufferSize);
    }

    const std::uint64_t count = buffer->motionEventsCount > 32 ? 32 : buffer->motionEventsCount;
    for (std::uint64_t i = 0; i < count; ++i) {
        RecordMotion(buffer->motionEvents + (static_cast<std::size_t>(i) * kMotionEventSize));
    }
    return buffer;
}

bool ResolvePreloaderApi() {
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
    void* preloader = nullptr;
    for (int attempt = 0; attempt < 120 && preloader == nullptr; ++attempt) {
        preloader = dlopen(kPreloaderLibrary, RTLD_NOW | RTLD_NOLOAD);
        if (!preloader) {
            usleep(250 * 1000);
        }
    }
    if (!preloader) {
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

bool InstallGameActivityInputProbe() {
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
    void* minecraft = dlopen(kMinecraftLibrary, RTLD_NOW | RTLD_NOLOAD);
    if (!minecraft) {
        LOGE("%s not loaded", kMinecraftLibrary);
        return false;
    }

    void* target = dlsym(minecraft, "android_app_swap_input_buffers");
    if (!target) {
        LOGE("android_app_swap_input_buffers export not found");
        return false;
    }

    g_swapHook = g_glossHook(target,
                             reinterpret_cast<void*>(&HookSwapInputBuffers),
                             reinterpret_cast<void**>(&g_oldSwapInputBuffers));
    if (!g_swapHook || !g_oldSwapInputBuffers) {
        LOGE("GameActivity swap-input hook install failed target=%p", target);
        return false;
    }

    LOGI("installed GameActivity input-buffer probe target=%p; AInputQueue hook removed", target);
    LOGI("test: inventory open -> move mouse over character -> drag finger in same area");
    return true;
}

void* InitThread(void*) {
    LOGI("module loaded: JsonUI pointer probe v3 (GameActivity input buffer)");
    if (!ResolvePreloaderApi()) {
        return nullptr;
    }
    InstallGameActivityInputProbe();
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
