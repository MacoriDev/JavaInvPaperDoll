#include <android/input.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstring>

namespace {

constexpr const char* kLogTag = "InventoryTouchFollow";
constexpr const char* kPreloaderLibrary = "libpreloader.so";
constexpr const char* kMinecraftLibrary = "libminecraftpe.so";

// Located in the uploaded Android ARM64 libminecraftpe.so:
// Build ID: 3e6189ba1d8357ef500d8043bc551406f0aae455
constexpr std::uintptr_t kLivePlayerRenderRva = 0x097B74B0;
constexpr std::uintptr_t kPaperDollRenderRva = 0x097BAF4C;

// The hook refuses to install on a different game build instead of writing to
// an unverified address.
constexpr std::uint8_t kLivePrefix[] = {
    0xef, 0x3b, 0xb6, 0x6d, 0xed, 0x33, 0x01, 0x6d,
    0xeb, 0x2b, 0x02, 0x6d, 0xe9, 0x23, 0x03, 0x6d
};
constexpr std::uint8_t kPaperPrefix[] = {
    0xe4, 0x03, 0x1f, 0xaa, 0x01, 0x00, 0x00, 0x14,
    0xee, 0x0f, 0x16, 0xfc, 0xed, 0x33, 0x01, 0x6d
};

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

using GHook = void*;
struct GlossRegister;

enum InstructionSet : int {
    I_NONE = 0,
    I_THUMB,
    I_ARM,
    I_ARM64
};

using GlossInitFn = void (*)(bool);
using GlossGetLibBiasFn = std::uintptr_t (*)(const char*);
using GlossInternalCallback = void (*)(GlossRegister*, GHook);
using GlossHookInternalFn = GHook (*)(void*, GlossInternalCallback, void*, bool, InstructionSet);
using GlossHookFn = GHook (*)(void*, void*, void**);
using AInputQueueGetEventFn = int32_t (*)(AInputQueue*, AInputEvent**);

GlossInitFn g_glossInit = nullptr;
GlossGetLibBiasFn g_glossGetLibBias = nullptr;
GlossHookInternalFn g_glossHookInternal = nullptr;
GlossHookFn g_glossHook = nullptr;
AInputQueueGetEventFn g_oldGetEvent = nullptr;

GHook g_liveHook = nullptr;
GHook g_paperHook = nullptr;
GHook g_inputHook = nullptr;

std::atomic<float> g_touchX{-1.0f};
std::atomic<float> g_touchY{-1.0f};
std::atomic<int32_t> g_touchAction{-1};
std::atomic<bool> g_touchDown{false};
std::atomic<std::uint64_t> g_liveHits{0};
std::atomic<std::uint64_t> g_paperHits{0};
std::atomic<std::int64_t> g_liveLastLogNs{0};
std::atomic<std::int64_t> g_paperLastLogNs{0};

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename T>
T Symbol(void* library, const char* name) {
    return reinterpret_cast<T>(dlsym(library, name));
}

void LogRendererHit(const char* label,
                    std::atomic<std::uint64_t>& hitCounter,
                    std::atomic<std::int64_t>& lastLogNs) {
    const std::uint64_t hits = hitCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::int64_t now = NowNs();
    std::int64_t previous = lastLogNs.load(std::memory_order_relaxed);

    // Log the first few calls immediately, then at most once per second.
    const bool initial = hits <= 4;
    if (!initial && now - previous < 1'000'000'000LL) {
        return;
    }
    if (!initial && !lastLogNs.compare_exchange_strong(previous, now, std::memory_order_relaxed)) {
        return;
    }
    if (initial) {
        lastLogNs.store(now, std::memory_order_relaxed);
    }

    LOGI("%s renderer hit=%" PRIu64 " touch=(%.1f, %.1f) action=%d down=%d",
         label,
         hits,
         static_cast<double>(g_touchX.load(std::memory_order_relaxed)),
         static_cast<double>(g_touchY.load(std::memory_order_relaxed)),
         g_touchAction.load(std::memory_order_relaxed),
         g_touchDown.load(std::memory_order_relaxed) ? 1 : 0);
}

void OnLivePlayerRendererEntry(GlossRegister*, GHook) {
    LogRendererHit("LIVE", g_liveHits, g_liveLastLogNs);
}

void OnPaperDollRendererEntry(GlossRegister*, GHook) {
    LogRendererHit("PAPER", g_paperHits, g_paperLastLogNs);
}

int32_t HookGetEvent(AInputQueue* queue, AInputEvent** outEvent) {
    const int32_t result = g_oldGetEvent ? g_oldGetEvent(queue, outEvent) : -1;
    if (result < 0 || outEvent == nullptr || *outEvent == nullptr) {
        return result;
    }

    AInputEvent* event = *outEvent;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        return result;
    }

    const int32_t action = AMotionEvent_getAction(event);
    const int32_t maskedAction = action & AMOTION_EVENT_ACTION_MASK;
    std::size_t pointerIndex = static_cast<std::size_t>(
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const std::size_t pointerCount = AMotionEvent_getPointerCount(event);
    if (pointerCount == 0) {
        return result;
    }
    if (pointerIndex >= pointerCount) {
        pointerIndex = 0;
    }

    const float x = AMotionEvent_getX(event, pointerIndex);
    const float y = AMotionEvent_getY(event, pointerIndex);
    g_touchX.store(x, std::memory_order_relaxed);
    g_touchY.store(y, std::memory_order_relaxed);
    g_touchAction.store(maskedAction, std::memory_order_relaxed);

    switch (maskedAction) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_MOVE:
            g_touchDown.store(true, std::memory_order_relaxed);
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            g_touchDown.store(false, std::memory_order_relaxed);
            break;
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (pointerCount <= 1) {
                g_touchDown.store(false, std::memory_order_relaxed);
            }
            break;
        default:
            break;
    }

    if (maskedAction == AMOTION_EVENT_ACTION_DOWN ||
        maskedAction == AMOTION_EVENT_ACTION_UP ||
        maskedAction == AMOTION_EVENT_ACTION_CANCEL) {
        LOGI("TOUCH action=%d position=(%.1f, %.1f)",
             maskedAction, static_cast<double>(x), static_cast<double>(y));
    }
    return result;
}

bool InstallInternalProbe(std::uintptr_t base,
                          std::uintptr_t rva,
                          const std::uint8_t* expected,
                          std::size_t expectedSize,
                          GlossInternalCallback callback,
                          GHook& output,
                          const char* label) {
    auto* target = reinterpret_cast<std::uint8_t*>(base + rva);
    if (std::memcmp(target, expected, expectedSize) != 0) {
        LOGE("%s signature mismatch at RVA 0x%" PRIxPTR "; refusing unsafe hook",
             label, rva);
        return false;
    }

    output = g_glossHookInternal(reinterpret_cast<void*>(target), callback,
                                 nullptr, false, I_ARM64);
    if (output == nullptr) {
        LOGE("failed installing %s internal probe at RVA 0x%" PRIxPTR, label, rva);
        return false;
    }
    LOGI("installed %s internal probe at RVA 0x%" PRIxPTR " target=%p",
         label, rva, reinterpret_cast<void*>(target));
    return true;
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
        LOGE("%s was not already loaded; run this mod through LeviLauncher preloader", kPreloaderLibrary);
        return false;
    }

    g_glossInit = Symbol<GlossInitFn>(preloader, "GlossInit");
    g_glossGetLibBias = Symbol<GlossGetLibBiasFn>(preloader, "GlossGetLibBias");
    g_glossHookInternal = Symbol<GlossHookInternalFn>(preloader, "GlossHookInternal");
    g_glossHook = Symbol<GlossHookFn>(preloader, "GlossHook");

    if (!g_glossInit || !g_glossGetLibBias || !g_glossHookInternal || !g_glossHook) {
        LOGE("required preloader exports missing: Init=%p Bias=%p Internal=%p Hook=%p",
             reinterpret_cast<void*>(g_glossInit),
             reinterpret_cast<void*>(g_glossGetLibBias),
             reinterpret_cast<void*>(g_glossHookInternal),
             reinterpret_cast<void*>(g_glossHook));
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
        LOGW("could not open libandroid.so; renderer logging remains active without touch coordinates");
        return;
    }

    void* inputTarget = dlsym(androidLibrary, "AInputQueue_getEvent");
    if (!inputTarget) {
        LOGW("AInputQueue_getEvent not found; renderer logging remains active without touch coordinates");
        return;
    }

    g_inputHook = g_glossHook(inputTarget,
                              reinterpret_cast<void*>(&HookGetEvent),
                              reinterpret_cast<void**>(&g_oldGetEvent));
    if (!g_inputHook || !g_oldGetEvent) {
        LOGW("touch hook install failed; renderer logging remains active without touch coordinates");
        return;
    }
    LOGI("installed Android touch coordinate probe");
}

void* InitThread(void*) {
    LOGI("module loaded: JsonUI inventory preview renderer probe");
    if (!ResolvePreloaderApi()) {
        return nullptr;
    }

    std::uintptr_t minecraftBase = 0;
    for (int attempt = 0; attempt < 240 && minecraftBase == 0; ++attempt) {
        minecraftBase = g_glossGetLibBias(kMinecraftLibrary);
        if (minecraftBase == 0) {
            usleep(250 * 1000);
        }
    }
    if (minecraftBase == 0) {
        LOGE("unable to locate %s mapping", kMinecraftLibrary);
        return nullptr;
    }

    LOGI("minecraft base=%p build-id target=3e6189ba1d8357ef500d8043bc551406f0aae455",
         reinterpret_cast<void*>(minecraftBase));

    const bool liveInstalled = InstallInternalProbe(
        minecraftBase, kLivePlayerRenderRva, kLivePrefix, sizeof(kLivePrefix),
        &OnLivePlayerRendererEntry, g_liveHook, "LivePlayerRenderer");
    const bool paperInstalled = InstallInternalProbe(
        minecraftBase, kPaperDollRenderRva, kPaperPrefix, sizeof(kPaperPrefix),
        &OnPaperDollRendererEntry, g_paperHook, "PaperDollRenderer");

    if (!liveInstalled && !paperInstalled) {
        LOGE("no preview probes were installed; this .so does not match the analyzed Minecraft build");
        return nullptr;
    }

    InstallInputProbe();
    LOGI("ready: open/close inventory and touch around the character preview; filter logcat by %s", kLogTag);
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
