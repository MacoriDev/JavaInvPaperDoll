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

// Supplied libminecraftpe.so target: LivePlayerRenderer helper that calculates
// two float look offsets from the current cursor position.
constexpr std::uintptr_t kLivePlayerLookOffsetHelperRva = 0x097B8AB4u;
constexpr std::uint32_t kLookOffsetSignature[] = {
    0xFC1B0FEA, 0x6D00A3E9, 0xA901FBFD, 0xF90017F7
};

// Confirmed GameActivityMotionEvent layout from the supplied binary and v3/v4 logs.
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
constexpr std::int32_t kToolFinger = 1;

constexpr std::int32_t kActionDown = 0;
constexpr std::int32_t kActionUp = 1;
constexpr std::int32_t kActionMove = 2;
constexpr std::int32_t kActionCancel = 3;

// v5.2 intentionally accepts touch positions from the entire screen.
// The model renderer uses the most recent touch position and keeps it after release.
constexpr std::int64_t kRenderLogIntervalNs = 250'000'000LL;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

using GHook = void*;
using GlossInitFn = void (*)(bool);
using GlossHookFn = GHook (*)(void*, void*, void**);
using MotionEventFromJavaFn = void (*)(JNIEnv*, jobject, std::byte*);

// ABI observed at LivePlayerRenderer's two call sites:
// x0=float* outputX, x1=float* outputY, x2/x3/x4 opaque render/UI objects,
// first two floating arguments arrive in s0/s1 and are used as model-center X/Y.
using LivePlayerLookOffsetFn = void (*)(float*, float*, void*, void*, void*, float, float);

GlossInitFn g_glossInit = nullptr;
GlossHookFn g_glossHook = nullptr;
MotionEventFromJavaFn g_oldMotionEventFromJava = nullptr;
LivePlayerLookOffsetFn g_oldLivePlayerLookOffset = nullptr;
GHook g_motionHook = nullptr;
GHook g_lookHook = nullptr;

std::atomic<bool> g_followTouch{false};
std::atomic<bool> g_haveLastTouch{false};
std::atomic<std::int32_t> g_followPointerId{-1};
std::atomic<std::uint32_t> g_touchXBits{0};
std::atomic<std::uint32_t> g_touchYBits{0};
std::atomic<std::int64_t> g_lastRenderLogNs{0};
std::atomic<std::uint64_t> g_overrideFrames{0};

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

std::uint32_t FloatBits(float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsFloat(std::uint32_t bits) {
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The original mouse branch at RVA 0x097B8BB8 / 0x097B8C9C reads this
// render/UI scale and applies it to the signed-short mouse coordinates.
float ReadNativePointerScale(void* lookContext) {
    if (!lookContext) {
        return 0.0f;
    }
    void* scaleState = nullptr;
    std::memcpy(&scaleState, reinterpret_cast<std::byte*>(lookContext) + 0x10, sizeof(scaleState));
    if (!scaleState) {
        return 0.0f;
    }
    float scale = 0.0f;
    std::memcpy(&scale, reinterpret_cast<std::byte*>(scaleState) + 0x60, sizeof(scale));
    return scale;
}

template <typename T>
T Symbol(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}

void SaveTouch(float x, float y) {
    g_touchXBits.store(FloatBits(x), std::memory_order_relaxed);
    g_touchYBits.store(FloatBits(y), std::memory_order_relaxed);
}

void TrackTouchWithoutModifyingEvent(const std::byte* event) {
    const std::int32_t source = ReadValue<std::int32_t>(event, kEventSourceOffset);
    const std::int32_t actionRaw = ReadValue<std::int32_t>(event, kEventActionOffset);
    const std::int32_t action = actionRaw & kActionMask;
    const std::int32_t pointerCount = ReadValue<std::int32_t>(event, kEventPointerCountOffset);
    if (source != kSourceTouchscreen || pointerCount < 1 || pointerCount > 8) {
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
    if (tool != kToolFinger) {
        return;
    }

    if (action == kActionDown && pointerCount == 1) {
        SaveTouch(x, y);
        g_haveLastTouch.store(true, std::memory_order_release);
        g_followPointerId.store(pointerId, std::memory_order_relaxed);
        g_followTouch.store(true, std::memory_order_release);
        LOGI("FULLSCREEN TOUCH START pointerId=%d pos=(%.1f,%.1f); original TOUCH event preserved",
             pointerId, static_cast<double>(x), static_cast<double>(y));
        return;
    }

    if (!g_followTouch.load(std::memory_order_acquire) ||
        pointerId != g_followPointerId.load(std::memory_order_relaxed) ||
        pointerCount != 1) {
        return;
    }

    if (action == kActionMove) {
        SaveTouch(x, y);
        return;
    }

    if (action == kActionUp || action == kActionCancel) {
        SaveTouch(x, y);
        g_followTouch.store(false, std::memory_order_release);
        g_followPointerId.store(-1, std::memory_order_relaxed);
        LOGI("FULLSCREEN TOUCH END pos=(%.1f,%.1f); gaze is held at last touch and tap/click remains TOUCH",
             static_cast<double>(x), static_cast<double>(y));
    }
}

void HookMotionEventFromJava(JNIEnv* env, jobject javaMotionEvent, std::byte* outEvent) {
    if (!g_oldMotionEventFromJava) {
        return;
    }
    g_oldMotionEventFromJava(env, javaMotionEvent, outEvent);
    if (outEvent) {
        TrackTouchWithoutModifyingEvent(outEvent);
    }
}

bool IsTouchLookActive() {
    // Remain at the final touch position after release until a later touch updates it.
    return g_haveLastTouch.load(std::memory_order_acquire);
}

void HookLivePlayerLookOffset(
    float* outX,
    float* outY,
    void* arg2,
    void* arg3,
    void* arg4,
    float modelCenterX,
    float modelCenterY) {

    if (!g_oldLivePlayerLookOffset) {
        return;
    }

    // Keep all original renderer behavior first. We replace only its final local look offsets.
    g_oldLivePlayerLookOffset(outX, outY, arg2, arg3, arg4, modelCenterX, modelCenterY);

    if (!outX || !outY || !IsTouchLookActive()) {
        return;
    }

    const float touchX = BitsFloat(g_touchXBits.load(std::memory_order_relaxed));
    const float touchY = BitsFloat(g_touchYBits.load(std::memory_order_relaxed));
    const float nativeScale = ReadNativePointerScale(arg2);
    if (!(nativeScale > 0.0f) || !(nativeScale < 16.0f)) {
        return;
    }

    // Device test result:
    // - Horizontal direction needed inversion relative to v5.1.
    // - Vertical direction in v5.1 was already correct.
    // Keep native scaling, invert X only, and restore Y to its v5.1 direction.
    const float nativeTouchX = static_cast<float>(static_cast<std::int16_t>(touchX));
    const float nativeTouchY = static_cast<float>(static_cast<std::int16_t>(touchY));
    const float originalX = *outX;
    const float originalY = *outY;
    *outX = modelCenterX - nativeTouchX * nativeScale;
    *outY = modelCenterY - nativeTouchY * nativeScale;

    const std::uint64_t frame = g_overrideFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::int64_t now = NowNs();
    std::int64_t previous = g_lastRenderLogNs.load(std::memory_order_relaxed);
    if (now - previous >= kRenderLogIntervalNs &&
        g_lastRenderLogNs.compare_exchange_strong(previous, now, std::memory_order_relaxed)) {
        LOGI("NATIVE LOOK X-INVERTED #%" PRIu64 " scale=%.5f center=(%.1f,%.1f) touchRaw=(%.1f,%.1f) original=(%.1f,%.1f) override=(%.1f,%.1f)",
             frame,
             static_cast<double>(nativeScale),
             static_cast<double>(modelCenterX), static_cast<double>(modelCenterY),
             static_cast<double>(touchX), static_cast<double>(touchY),
             static_cast<double>(originalX), static_cast<double>(originalY),
             static_cast<double>(*outX), static_cast<double>(*outY));
    }
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

bool InstallHooks() {
#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif
    void* minecraft = dlopen(kMinecraftLibrary, RTLD_NOW | RTLD_NOLOAD);
    if (!minecraft) {
        LOGE("%s not loaded", kMinecraftLibrary);
        return false;
    }

    void* motionTarget = dlsym(minecraft, "GameActivityMotionEvent_fromJava");
    if (!motionTarget) {
        LOGE("GameActivityMotionEvent_fromJava export not found");
        return false;
    }

    Dl_info moduleInfo{};
    if (dladdr(motionTarget, &moduleInfo) == 0 || !moduleInfo.dli_fbase) {
        LOGE("unable to determine libminecraftpe.so base");
        return false;
    }

    auto* base = reinterpret_cast<std::byte*>(moduleInfo.dli_fbase);
    void* lookTarget = base + kLivePlayerLookOffsetHelperRva;
    if (std::memcmp(lookTarget, kLookOffsetSignature, sizeof(kLookOffsetSignature)) != 0) {
        LOGE("look-offset helper signature mismatch at RVA 0x%" PRIxPTR "; refusing unsafe hook",
             kLivePlayerLookOffsetHelperRva);
        return false;
    }

    g_lookHook = g_glossHook(
        lookTarget,
        reinterpret_cast<void*>(&HookLivePlayerLookOffset),
        reinterpret_cast<void**>(&g_oldLivePlayerLookOffset));
    if (!g_lookHook || !g_oldLivePlayerLookOffset) {
        LOGE("LivePlayerRenderer look-offset helper hook failed target=%p", lookTarget);
        return false;
    }

    g_motionHook = g_glossHook(
        motionTarget,
        reinterpret_cast<void*>(&HookMotionEventFromJava),
        reinterpret_cast<void**>(&g_oldMotionEventFromJava));
    if (!g_motionHook || !g_oldMotionEventFromJava) {
        LOGE("touch observer hook failed target=%p; renderer override will remain idle", motionTarget);
        return false;
    }

    LOGI("installed v5.3 full-screen held-look renderer override; motionTarget=%p lookTarget=%p", motionTarget, lookTarget);
    LOGI("input-mode safe design: TOUCH events preserved; no synthetic mouse input is emitted");
    LOGI("touch follow area: entire screen; gaze hold: persistent after finger release");
    LOGI("axis correction: X inverted; Y restored to v5.1 direction");
    LOGI("test WITHOUT a connected mouse: inventory -> tap/drag anywhere on the screen");
    return true;
}

void* InitThread(void*) {
    LOGI("module loaded: JsonUI inventory touch follow v5.3 (X-only inversion; full-screen held-look)");
    if (ResolvePreloaderApi()) {
        InstallHooks();
    }
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
