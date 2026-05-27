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
constexpr std::int64_t kMotionLogIntervalNs = 100'000'000LL;
constexpr std::int64_t kInjectLogIntervalNs = 150'000'000LL;

// Confirmed for the supplied libminecraftpe.so build.
constexpr std::size_t kMotionEventSize = 0x6E0;
constexpr std::size_t kEventDeviceIdOffset = 0x00;
constexpr std::size_t kEventSourceOffset = 0x04;
constexpr std::size_t kEventActionOffset = 0x08;
constexpr std::size_t kEventPointerCountOffset = 0x38;
constexpr std::size_t kFirstPointerOffset = 0x3C;
constexpr std::size_t kPointerStride = 0xD0;
constexpr std::size_t kPointerIdOffset = 0x00;
constexpr std::size_t kPointerToolTypeOffset = 0x04;
constexpr std::size_t kPointerAxisXOffset = 0x08; // AMOTION_EVENT_AXIS_X
constexpr std::size_t kPointerAxisYOffset = 0x0C; // AMOTION_EVENT_AXIS_Y

constexpr std::int32_t kActionMask = 0xFF;
constexpr std::int32_t kActionPointerIndexMask = 0xFF00;
constexpr std::int32_t kActionPointerIndexShift = 8;
constexpr std::int32_t kSourceTouchscreen = 0x00001002;
constexpr std::int32_t kSourceMouse = 0x00002002;

// Galaxy S23 Ultra landscape/raw input coordinates from the supplied log.
// Only a DOWN beginning in this JsonUI character rectangle starts emulation.
// Movement may then continue outside this area until UP/CANCEL.
constexpr float kPreviewLeft = 1580.0f;
constexpr float kPreviewTop = 220.0f;
constexpr float kPreviewRight = 1950.0f;
constexpr float kPreviewBottom = 760.0f;

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

// Beginning of android_input_buffer from GameActivity native_app_glue.
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
std::atomic<std::int64_t> g_lastInjectLogNs{0};
std::atomic<std::uint64_t> g_motionCount{0};
std::atomic<std::uint64_t> g_syntheticCount{0};
std::atomic<bool> g_loggedFirstBuffer{false};
std::atomic<bool> g_loggedBufferFull{false};

// Hook runs on Minecraft's input consumer thread, so this drag state is only
// read/written from HookSwapInputBuffers.
bool g_previewDragActive = false;
std::int32_t g_previewDragPointerId = -1;
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

bool IsInsidePreview(float x, float y) {
    return x >= kPreviewLeft && x <= kPreviewRight && y >= kPreviewTop && y <= kPreviewBottom;
}

std::int32_t GetPointerIndex(const std::byte* event, std::int32_t pointerId) {
    const std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
    if (pointerCount < 1 || pointerCount > 8) {
        return -1;
    }
    for (std::int32_t i = 0; i < pointerCount; ++i) {
        const std::size_t pointer = kFirstPointerOffset + static_cast<std::size_t>(i) * kPointerStride;
        if (ReadValue<std::int32_t>(event, pointer + kPointerIdOffset) == pointerId) {
            return i;
        }
    }
    return -1;
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

bool ShouldLogInjection() {
    const std::int64_t now = NowNs();
    std::int64_t previous = g_lastInjectLogNs.load(std::memory_order_relaxed);
    if (now - previous < kInjectLogIntervalNs) {
        return false;
    }
    return g_lastInjectLogNs.compare_exchange_strong(previous, now, std::memory_order_relaxed);
}

void LogOriginalMotion(const std::byte* event) {
    const std::int32_t action = ReadValue<std::int32_t>(event, kEventActionOffset);
    const std::int32_t maskedAction = action & kActionMask;
    if (!ShouldLogMotion(maskedAction)) {
        return;
    }

    const std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
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

    LOGI("INPUT #%" PRIu64 " action=%s(%d) source=0x%08x tool=%s(%d) device=%d pointerId=%d pos=(%.1f,%.1f)",
         count, ActionName(maskedAction), maskedAction, static_cast<unsigned int>(source),
         ToolName(tool), tool, deviceId, pointerId, static_cast<double>(x), static_cast<double>(y));
}

// Append a synthetic absolute mouse hover event while keeping the original touch event.
// This preserves normal touch processing and reuses JsonUI's existing mouse-hover path.
bool AppendSyntheticMouse(AndroidInputBufferPrefix* buffer,
                          const std::byte* touchEvent,
                          std::int32_t touchPointerIndex,
                          std::int32_t syntheticAction) {
    if (!buffer || !buffer->motionEvents) {
        return false;
    }
    if (buffer->motionEventsCount >= buffer->motionEventsBufferSize) {
        if (!g_loggedBufferFull.exchange(true, std::memory_order_relaxed)) {
            LOGW("cannot append synthetic mouse event: GameActivity input buffer full (%" PRIu64 "/%" PRIu64 ")",
                 buffer->motionEventsCount, buffer->motionEventsBufferSize);
        }
        return false;
    }

    const std::size_t touchPointer = kFirstPointerOffset + static_cast<std::size_t>(touchPointerIndex) * kPointerStride;
    const float x = ReadValue<float>(touchEvent, touchPointer + kPointerAxisXOffset);
    const float y = ReadValue<float>(touchEvent, touchPointer + kPointerAxisYOffset);

    std::byte* synthetic = buffer->motionEvents + static_cast<std::size_t>(buffer->motionEventsCount) * kMotionEventSize;
    std::memcpy(synthetic, touchEvent, kMotionEventSize);

    // Normalize multi-touch to a single pointer and copy the selected finger's axes into slot 0.
    if (touchPointerIndex != 0) {
        std::memcpy(synthetic + kFirstPointerOffset,
                    touchEvent + touchPointer,
                    kPointerStride);
    }
    WriteValue<std::int32_t>(synthetic, kEventPointerCountOffset, 1);
    WriteValue<std::int32_t>(synthetic, kEventSourceOffset, kSourceMouse);
    WriteValue<std::int32_t>(synthetic, kEventActionOffset, syntheticAction);
    WriteValue<std::int32_t>(synthetic, kFirstPointerOffset + kPointerToolTypeOffset, TOOL_MOUSE);
    WriteValue<float>(synthetic, kFirstPointerOffset + kPointerAxisXOffset, x);
    WriteValue<float>(synthetic, kFirstPointerOffset + kPointerAxisYOffset, y);

    if (g_haveRealMouseDevice) {
        WriteValue<std::int32_t>(synthetic, kEventDeviceIdOffset, g_realMouseDeviceId);
    }

    ++buffer->motionEventsCount;
    const std::uint64_t injected = g_syntheticCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (syntheticAction != ACTION_HOVER_MOVE || ShouldLogInjection()) {
        LOGI("SYNTH #%" PRIu64 " action=%s device=%d pos=(%.1f,%.1f)%s",
             injected,
             ActionName(syntheticAction),
             g_haveRealMouseDevice ? g_realMouseDeviceId : ReadValue<std::int32_t>(touchEvent, kEventDeviceIdOffset),
             static_cast<double>(x), static_cast<double>(y),
             g_haveRealMouseDevice ? "" : " fallback-touch-device");
    }
    return true;
}

void ProcessMotionForTouchFollow(AndroidInputBufferPrefix* buffer, const std::byte* event) {
    const std::int32_t actionRaw = ReadValue<std::int32_t>(event, kEventActionOffset);
    const std::int32_t action = actionRaw & kActionMask;
    const std::int32_t source = ReadValue<std::int32_t>(event, kEventSourceOffset);
    const std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
    if (pointerCount < 1 || pointerCount > 8) {
        return;
    }

    std::int32_t changedPointerIndex = (actionRaw & kActionPointerIndexMask) >> kActionPointerIndexShift;
    if (changedPointerIndex < 0 || changedPointerIndex >= pointerCount) {
        changedPointerIndex = 0;
    }
    const std::size_t changedPointer = kFirstPointerOffset + static_cast<std::size_t>(changedPointerIndex) * kPointerStride;
    const std::int32_t tool = ReadValue<std::int32_t>(event, changedPointer + kPointerToolTypeOffset);
    const std::int32_t pointerId = ReadValue<std::int32_t>(event, changedPointer + kPointerIdOffset);
    const float x = ReadValue<float>(event, changedPointer + kPointerAxisXOffset);
    const float y = ReadValue<float>(event, changedPointer + kPointerAxisYOffset);

    // Capture the real mouse device id while the mouse is attached. v4 testing should
    // begin by moving the physical mouse once before touching the preview.
    if (tool == TOOL_MOUSE && source == kSourceMouse) {
        const std::int32_t deviceId = ReadValue<std::int32_t>(event, kEventDeviceIdOffset);
        if (!g_haveRealMouseDevice || g_realMouseDeviceId != deviceId) {
            g_haveRealMouseDevice = true;
            g_realMouseDeviceId = deviceId;
            LOGI("captured real absolute mouse device=%d for synthetic hover events", deviceId);
        }
        return;
    }

    if (tool != TOOL_FINGER || source != kSourceTouchscreen) {
        return;
    }

    if (action == ACTION_DOWN && IsInsidePreview(x, y)) {
        g_previewDragActive = true;
        g_previewDragPointerId = pointerId;
        LOGI("preview touch drag START pointerId=%d pos=(%.1f,%.1f) realMouse=%s",
             pointerId, static_cast<double>(x), static_cast<double>(y),
             g_haveRealMouseDevice ? "yes" : "no");
        AppendSyntheticMouse(buffer, event, changedPointerIndex, ACTION_HOVER_ENTER);
        AppendSyntheticMouse(buffer, event, changedPointerIndex, ACTION_HOVER_MOVE);
        return;
    }

    if (!g_previewDragActive) {
        if (action == ACTION_DOWN) {
            LOGI("finger DOWN outside preview pos=(%.1f,%.1f); not emulating mouse", static_cast<double>(x), static_cast<double>(y));
        }
        return;
    }

    const std::int32_t activeIndex = GetPointerIndex(event, g_previewDragPointerId);
    if (action == ACTION_MOVE && activeIndex >= 0) {
        AppendSyntheticMouse(buffer, event, activeIndex, ACTION_HOVER_MOVE);
        return;
    }

    if ((action == ACTION_UP && pointerId == g_previewDragPointerId) || action == ACTION_CANCEL) {
        const std::int32_t finalIndex = activeIndex >= 0 ? activeIndex : changedPointerIndex;
        AppendSyntheticMouse(buffer, event, finalIndex, ACTION_HOVER_EXIT);
        LOGI("preview touch drag END pointerId=%d pos=(%.1f,%.1f)",
             g_previewDragPointerId, static_cast<double>(x), static_cast<double>(y));
        g_previewDragActive = false;
        g_previewDragPointerId = -1;
    }
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

    // Never process events appended by this mod during the same callback.
    const std::uint64_t originalCount = buffer->motionEventsCount;
    const std::uint64_t limitedCount = originalCount > 64 ? 64 : originalCount;
    for (std::uint64_t i = 0; i < limitedCount; ++i) {
        const std::byte* event = buffer->motionEvents + static_cast<std::size_t>(i) * kMotionEventSize;
        LogOriginalMotion(event);
        ProcessMotionForTouchFollow(buffer, event);
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

bool InstallGameActivityTouchFollow() {
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

    LOGI("installed v4 touch->mouse hover injector target=%p; renderer hooks disabled", target);
    LOGI("preview start bounds raw=(%.0f,%.0f)-(%.0f,%.0f)",
         static_cast<double>(kPreviewLeft), static_cast<double>(kPreviewTop),
         static_cast<double>(kPreviewRight), static_cast<double>(kPreviewBottom));
    LOGI("test: keep mouse connected, move it once, then drag finger directly on inventory character");
    return true;
}

void* InitThread(void*) {
    LOGI("module loaded: JsonUI inventory touch follow v4 (GameActivity synthetic mouse hover)");
    if (!ResolvePreloaderApi()) {
        return nullptr;
    }
    InstallGameActivityTouchFollow();
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
