#include <android/log.h>
#include <jni.h>
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
constexpr std::int64_t kMotionLogIntervalNs = 100'000'000LL;
constexpr std::int64_t kRewriteLogIntervalNs = 150'000'000LL;

// Confirmed from the supplied libminecraftpe.so / v3 logging.
constexpr std::size_t kEventDeviceIdOffset = 0x00;
constexpr std::size_t kEventSourceOffset = 0x04;
constexpr std::size_t kEventActionOffset = 0x08;
constexpr std::size_t kEventPointerCountOffset = 0x38;
constexpr std::size_t kFirstPointerOffset = 0x3C;
constexpr std::size_t kPointerStride = 0xD0;
constexpr std::size_t kPointerIdOffset = 0x00;
constexpr std::size_t kPointerToolTypeOffset = 0x04;
constexpr std::size_t kPointerAxisXOffset = 0x08;
constexpr std::size_t kPointerAxisYOffset = 0x0C;

constexpr std::int32_t kActionMask = 0xFF;
constexpr std::int32_t kActionPointerIndexMask = 0xFF00;
constexpr std::int32_t kActionPointerIndexShift = 8;
constexpr std::int32_t kSourceTouchscreen = 0x00001002;
constexpr std::int32_t kSourceMouse = 0x00002002;

// S23 Ultra landscape/raw coordinates (3088 x 1440).
// Entire two-panel JsonUI inventory window from the supplied screenshot/log test.
// The close X button stays outside this region so the inventory can still be closed.
constexpr float kInventoryLeft = 385.0f;
constexpr float kInventoryTop = 215.0f;
constexpr float kInventoryRight = 2510.0f;
constexpr float kInventoryBottom = 1225.0f;

enum : std::int32_t {
    ACTION_DOWN = 0,
    ACTION_UP = 1,
    ACTION_MOVE = 2,
    ACTION_CANCEL = 3,
    ACTION_POINTER_DOWN = 5,
    ACTION_POINTER_UP = 6,
    ACTION_HOVER_MOVE = 7,
    ACTION_HOVER_ENTER = 9,
    ACTION_HOVER_EXIT = 10,
};

enum : std::int32_t {
    TOOL_FINGER = 1,
    TOOL_MOUSE = 3,
};

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

using GHook = void*;
using GlossInitFn = void (*)(bool);
using GlossHookFn = GHook (*)(void*, void*, void**);
using MotionEventFromJavaFn = void (*)(JNIEnv*, jobject, std::byte*);

GlossInitFn g_glossInit = nullptr;
GlossHookFn g_glossHook = nullptr;
MotionEventFromJavaFn g_oldMotionEventFromJava = nullptr;
GHook g_motionFromJavaHook = nullptr;

std::atomic<std::int64_t> g_lastMotionLogNs{0};
std::atomic<std::int64_t> g_lastRewriteLogNs{0};
std::atomic<std::uint64_t> g_motionCount{0};
std::atomic<std::uint64_t> g_rewriteCount{0};
std::atomic<bool> g_warnedNoMouse{false};
std::atomic<bool> g_warnedMultiTouch{false};

// GameActivity converts events on one input/JNI path in this build.
bool g_inventoryDragActive = false;
std::int32_t g_inventoryDragPointerId = -1;
bool g_haveRealMouseDevice = false;
std::int32_t g_realMouseDeviceId = -1;

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
void WriteValue(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(T));
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
        case ACTION_POINTER_DOWN: return "POINTER_DOWN";
        case ACTION_POINTER_UP: return "POINTER_UP";
        case ACTION_HOVER_MOVE: return "HOVER_MOVE";
        case ACTION_HOVER_ENTER: return "HOVER_ENTER";
        case ACTION_HOVER_EXIT: return "HOVER_EXIT";
        default: return "OTHER";
    }
}

bool IsInsideInventoryWindow(float x, float y) {
    return x >= kInventoryLeft && x <= kInventoryRight &&
           y >= kInventoryTop && y <= kInventoryBottom;
}

bool ShouldLog(std::atomic<std::int64_t>& last, std::int64_t interval) {
    const std::int64_t now = NowNs();
    std::int64_t previous = last.load(std::memory_order_relaxed);
    if (now - previous < interval) {
        return false;
    }
    return last.compare_exchange_strong(previous, now, std::memory_order_relaxed);
}

void LogMotion(const std::byte* event) {
    const std::int32_t actionRaw = ReadValue<std::int32_t>(event, kEventActionOffset);
    const std::int32_t action = actionRaw & kActionMask;
    if ((action == ACTION_MOVE || action == ACTION_HOVER_MOVE) &&
        !ShouldLog(g_lastMotionLogNs, kMotionLogIntervalNs)) {
        return;
    }

    const std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
    if (pointerCount < 1 || pointerCount > 8) {
        return;
    }
    std::int32_t index = (actionRaw & kActionPointerIndexMask) >> kActionPointerIndexShift;
    if (index < 0 || index >= pointerCount) {
        index = 0;
    }
    const std::size_t pointer = kFirstPointerOffset + static_cast<std::size_t>(index) * kPointerStride;
    const std::int32_t deviceId = ReadValue<std::int32_t>(event, kEventDeviceIdOffset);
    const std::int32_t source = ReadValue<std::int32_t>(event, kEventSourceOffset);
    const std::int32_t tool = ReadValue<std::int32_t>(event, pointer + kPointerToolTypeOffset);
    const std::int32_t pointerId = ReadValue<std::int32_t>(event, pointer + kPointerIdOffset);
    const float x = ReadValue<float>(event, pointer + kPointerAxisXOffset);
    const float y = ReadValue<float>(event, pointer + kPointerAxisYOffset);
    const std::uint64_t count = g_motionCount.fetch_add(1, std::memory_order_relaxed) + 1;

    LOGI("RAW #%" PRIu64 " action=%s(%d) source=0x%08x tool=%d device=%d pointerId=%d pos=(%.1f,%.1f)",
         count, ActionName(action), action, static_cast<unsigned>(source), tool, deviceId,
         pointerId, static_cast<double>(x), static_cast<double>(y));
}

// v4 appended a memcpy-copy of GameActivityMotionEvent into the swapped read buffer.
// That event owns history pointers and gets destroyed later, so copying it can double-free.
// v4.2 keeps the v4.1 crash fix: it never appends or copies an event and rewrites one newly produced event.
bool RewriteSingleFingerAsMouse(std::byte* event, std::int32_t newAction) {
    const std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
    if (pointerCount != 1) {
        if (!g_warnedMultiTouch.exchange(true, std::memory_order_relaxed)) {
            LOGW("touch-follow disabled for multi-touch event: pointerCount=%d", pointerCount);
        }
        return false;
    }
    if (!g_haveRealMouseDevice) {
        if (!g_warnedNoMouse.exchange(true, std::memory_order_relaxed)) {
            LOGW("move the connected mouse once before using touch-follow; real mouse device id not captured");
        }
        return false;
    }

    WriteValue<std::int32_t>(event, kEventDeviceIdOffset, g_realMouseDeviceId);
    WriteValue<std::int32_t>(event, kEventSourceOffset, kSourceMouse);
    WriteValue<std::int32_t>(event, kEventActionOffset, newAction);
    WriteValue<std::int32_t>(event, kFirstPointerOffset + kPointerToolTypeOffset, TOOL_MOUSE);

    const float x = ReadValue<float>(event, kFirstPointerOffset + kPointerAxisXOffset);
    const float y = ReadValue<float>(event, kFirstPointerOffset + kPointerAxisYOffset);
    const std::uint64_t count = g_rewriteCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (newAction != ACTION_HOVER_MOVE || ShouldLog(g_lastRewriteLogNs, kRewriteLogIntervalNs)) {
        LOGI("REWRITE #%" PRIu64 " -> %s device=%d pos=(%.1f,%.1f)",
             count, ActionName(newAction), g_realMouseDeviceId,
             static_cast<double>(x), static_cast<double>(y));
    }
    return true;
}

void ProcessProducedMotion(std::byte* event) {
    const std::int32_t actionRaw = ReadValue<std::int32_t>(event, kEventActionOffset);
    const std::int32_t action = actionRaw & kActionMask;
    const std::int32_t source = ReadValue<std::int32_t>(event, kEventSourceOffset);
    const std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
    if (pointerCount < 1 || pointerCount > 8) {
        return;
    }

    std::int32_t index = (actionRaw & kActionPointerIndexMask) >> kActionPointerIndexShift;
    if (index < 0 || index >= pointerCount) {
        index = 0;
    }
    const std::size_t pointer = kFirstPointerOffset + static_cast<std::size_t>(index) * kPointerStride;
    const std::int32_t tool = ReadValue<std::int32_t>(event, pointer + kPointerToolTypeOffset);
    const std::int32_t pointerId = ReadValue<std::int32_t>(event, pointer + kPointerIdOffset);
    const float x = ReadValue<float>(event, pointer + kPointerAxisXOffset);
    const float y = ReadValue<float>(event, pointer + kPointerAxisYOffset);

    if (source == kSourceMouse && tool == TOOL_MOUSE) {
        const std::int32_t deviceId = ReadValue<std::int32_t>(event, kEventDeviceIdOffset);
        if (!g_haveRealMouseDevice || g_realMouseDeviceId != deviceId) {
            g_haveRealMouseDevice = true;
            g_realMouseDeviceId = deviceId;
            LOGI("captured real absolute mouse device=%d", deviceId);
        }
        return;
    }

    if (source != kSourceTouchscreen || tool != TOOL_FINGER) {
        return;
    }

    if (action == ACTION_DOWN && pointerCount == 1 && IsInsideInventoryWindow(x, y)) {
        if (RewriteSingleFingerAsMouse(event, ACTION_HOVER_ENTER)) {
            g_inventoryDragActive = true;
            g_inventoryDragPointerId = pointerId;
            LOGI("inventory touch-follow START pointerId=%d pos=(%.1f,%.1f)",
                 pointerId, static_cast<double>(x), static_cast<double>(y));
        }
        return;
    }

    if (!g_inventoryDragActive || pointerId != g_inventoryDragPointerId || pointerCount != 1) {
        return;
    }

    if (action == ACTION_MOVE) {
        RewriteSingleFingerAsMouse(event, ACTION_HOVER_MOVE);
        return;
    }

    if (action == ACTION_UP || action == ACTION_CANCEL) {
        RewriteSingleFingerAsMouse(event, ACTION_HOVER_EXIT);
        LOGI("inventory touch-follow END pointerId=%d pos=(%.1f,%.1f)",
             g_inventoryDragPointerId, static_cast<double>(x), static_cast<double>(y));
        g_inventoryDragActive = false;
        g_inventoryDragPointerId = -1;
    }
}

void HookMotionEventFromJava(JNIEnv* env, jobject javaMotionEvent, std::byte* outEvent) {
    if (!g_oldMotionEventFromJava) {
        return;
    }

    g_oldMotionEventFromJava(env, javaMotionEvent, outEvent);
    if (!outEvent) {
        return;
    }

    LogMotion(outEvent);          // log before any change
    ProcessProducedMotion(outEvent);
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
        LOGE("required preloader exports missing");
        return false;
    }
    g_glossInit(true);
    return true;
}

bool InstallMotionProducerRewrite() {
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
    void* minecraft = dlopen(kMinecraftLibrary, RTLD_NOW | RTLD_NOLOAD);
    if (!minecraft) {
        LOGE("%s not loaded", kMinecraftLibrary);
        return false;
    }

    void* target = dlsym(minecraft, "GameActivityMotionEvent_fromJava");
    if (!target) {
        LOGE("GameActivityMotionEvent_fromJava export not found");
        return false;
    }

    g_motionFromJavaHook = g_glossHook(
        target,
        reinterpret_cast<void*>(&HookMotionEventFromJava),
        reinterpret_cast<void**>(&g_oldMotionEventFromJava));
    if (!g_motionFromJavaHook || !g_oldMotionEventFromJava) {
        LOGE("motion producer hook install failed target=%p", target);
        return false;
    }

    LOGI("installed v4.2 GameActivityMotionEvent_fromJava rewrite hook target=%p", target);
    LOGI("safe change: no swapped-buffer append and no GameActivityMotionEvent memcpy");
    LOGI("whole inventory follow bounds raw=(%.0f,%.0f)-(%.0f,%.0f)",
         static_cast<double>(kInventoryLeft), static_cast<double>(kInventoryTop),
         static_cast<double>(kInventoryRight), static_cast<double>(kInventoryBottom));
    LOGW("v4.2 test build: bounds are coordinate-gated, not yet inventory-screen-state gated");
    LOGI("test: inventory -> move connected mouse once -> touch-drag anywhere inside inventory panels");
    return true;
}

void* InitThread(void*) {
    LOGI("module loaded: JsonUI whole-inventory touch follow v4.2 (producer in-place rewrite)");
    if (!ResolvePreloaderApi()) {
        return nullptr;
    }
    InstallMotionProducerRewrite();
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
