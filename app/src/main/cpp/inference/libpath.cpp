// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of libpath.h
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/libpath.h"

#include "inference/capabilities.h"

#include <android/log.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace {

/// Anchors dladdr to this library. It has to be a real function defined here —
/// taking the address of an external one would report that library's path, and
/// on a device there is no property or API to ask instead: the daemon has no
/// Context.
void selfAnchor() {}

std::string resolveLibraryDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&selfAnchor), &info) == 0 ||
        info.dli_fname == nullptr) {
        LOGW("dladdr on our own symbol failed; vendor libraries cannot be located");
        return {};
    }
    std::string path(info.dli_fname);
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash + 1);   // trailing slash included
}

std::mutex  g_loadMutex;
std::string g_lastError;

}  // namespace

const char* libraryDir() {
    static const std::string dir = resolveLibraryDir();
    return dir.c_str();
}

std::string libraryPath(const char* name) {
    if (name == nullptr) return {};
    const std::string dir = libraryDir();
    if (dir.empty()) return name;
    return dir + name;
}

bool libraryPresent(const char* name) {
    if (name == nullptr) return false;
    const std::string dir = libraryDir();
    if (dir.empty()) return false;
    const std::string full = dir + name;
    FILE* f = fopen(full.c_str(), "rb");
    if (f == nullptr) return false;
    fclose(f);
    return true;
}

const char* lastLoadError() { return g_lastError.c_str(); }

void* preload(const char* name, std::string* why) {
    if (name == nullptr) {
        if (why) *why = "null library name";
        return nullptr;
    }

    const std::string full = libraryPath(name);

    // RTLD_NOLOAD first: a second dlopen of the same path is cheap but returns a
    // new handle, and more importantly a test dlopen that already succeeded
    // should not be repeated. RTLD_NOLOAD reports whether it is resident without
    // loading it.
    void* handle = dlopen(full.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    if (handle != nullptr) return handle;

    // Drained before the attempt: dlerror() is one-shot, and a stale message
    // from an earlier failure would be attributed to this one.
    (void)dlerror();

    handle = dlopen(full.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char* err = dlerror();
        std::string msg = err ? err : "dlopen failed with no message";
        {
            std::lock_guard<std::mutex> lock(g_loadMutex);
            g_lastError = std::string(name) + ": " + msg;
        }
        if (why) *why = g_lastError;
        LOGW("preload %s failed: %s", full.c_str(), msg.c_str());
        return nullptr;
    }

    LOGI("preloaded %s", full.c_str());
    return handle;
}

bool preloadAll(const char* const* names, std::string* why) {
    if (names == nullptr) return true;
    for (int i = 0; names[i] != nullptr; ++i) {
        if (preload(names[i], why) == nullptr) return false;
    }
    return true;
}

namespace {

// ── The per-pair library lists ───────────────────────────────────────────────
//
// Each list is a static array terminated by nullptr, because the caller walks it
// to the end. The QNN stack is assembled at most once: its last entry is the
// arch-specific skel, whose name depends on the SoC, so it cannot be a constant.

const char* const kNone[] = {nullptr};

/// LiteRT: everything the delegates need that is not the delegate itself.
const char* const kLiteRtBase[] = {"libtensorflowlite_jni.so", nullptr};

/// The GPU delegate lives in its own library and needs the base one first.
const char* const kLiteRtGpu[] = {
    "libtensorflowlite_jni.so",
    "libtensorflowlite_gpu_jni.so",
    nullptr,
};

/// LiteRT 2.x NPU: the runtime plus the MediaTek dispatch and compiler plugin.
///
/// This backend only ships the MediaTek pair. Snapdragon devices run NPU
/// inference through the existing `Ep::Htp` (QNN TFLite delegate) path instead,
/// so no Qualcomm dispatch/plugin is bundled — that keeps the APK smaller and
/// avoids a second, version-matched QNN stack. `epAvailable()` therefore greys
/// the NPU row on a Qualcomm SoC (the `libLiteRtDispatch_Qualcomm.so` it would
/// need is not present) and only offers it on MediaTek. The MediaTek libraries
/// depend only on `libLiteRt.so` plus system libraries, so they load fine even
/// on a Snapdragon where they are never actually exercised.
const char* const kLiteRtNpu[] = {
    "libLiteRt.so",
    "libLiteRtDispatch_MediaTek.so",
    "libLiteRtCompilerPlugin_MediaTek.so",
    nullptr,
};

/// The QNN delegate, which pulls the QNN stack in behind it.
///
/// Order matters. The TFLite JNI runtime is the base — its C API symbols must
/// be resolvable before the delegate is built. The QNN TFLite delegate then
/// loads on top. After that, the full HTP backend stack (libQnnHtp.so +
/// the per-arch V{arch}Skel + libQnnSystem.so) is dlopen'd explicitly:
///
///   * The skel alone is what the delegate looks up under "libQnnHtpSkel.so"
///     (or "libQnnHtpV{arch}Skel.so" on newer QNN SDKs). Without it
///     TfLiteQnnDelegateCreate returns null with no QNN-side error worth
///     showing the user.
///   * libQnnHtp.so and libQnnSystem.so are the runtime + system layer the
///     skel pulls in transitively; preloading them short-circuits the
///     delegate's lazy dlopen and gives a clear log line on success or
///     failure.
///
/// Earlier this list stopped at the delegate and let the delegate itself
/// resolve the backend — which failed silently inside the delegate builder
/// on devices where QNN was not on the classloader namespace. With the
/// full stack listed, preloadDaemonDeps() in ShellServerEntry resolves every
/// SONAME by absolute path before libaimbotng.so is loaded.
const char* const kLiteRtQnn[] = {
    "libtensorflowlite_jni.so",
    "libQnnTFLiteDelegate.so",
    "libqnn_tflite_delegate_jni.so",
    "libQnnHtp.so",
    "libQnnHtpNetRunExtensions.so",
    "libQnnHtpV68Skel.so",
    "libQnnHtpV69Skel.so",
    "libQnnHtpV73Skel.so",
    "libQnnHtpV75Skel.so",
    "libQnnHtpV79Skel.so",
    "libQnnHtpV81Skel.so",
    "libQnnHtpPrepare.so",
    "libQnnHtpOptraceProfilingReader.so",
    "libQnnHtpProfilingReader.so",
    "libQnnSystem.so",
    "libQnnGpu.so",
    "libQnnGpuNetRunExtensions.so",
    "libQnnGpuProfilingReader.so",
    "libQnnDsp.so",
    "libQnnDspNetRunExtensions.so",
    nullptr,
};

/// The QNN stack, assembled once.
///
/// Holds the strings itself and hands out pointers into them, because
/// runtimeLibraries() returns a `const char* const*` that outlives the call and
/// a pointer into a temporary vector would dangle the moment it returned. The
/// two vectors are filled in two passes for the same reason: push_back on
/// `names` may reallocate, so every c_str() is taken after the last one.
struct HtpStack {
    std::vector<std::string> names;
    std::vector<const char*> ptrs;

    HtpStack() {
        names.emplace_back("libQnnHtp.so");

        const int arch = capabilities().htpArch;
        char skel[64];
        if (arch > 0) {
            snprintf(skel, sizeof(skel), "libQnnHtpV%dSkel.so", arch);
        } else {
            // No arch we can name a skel for. The generic backend is still worth
            // attempting: it refuses the graph on its own terms, with a QNN
            // error naming the real problem, which beats our guess at it.
            snprintf(skel, sizeof(skel), "libQnnHtpSkel.so");
        }
        names.emplace_back(skel);

        names.emplace_back("libQnnSystem.so");

        ptrs.reserve(names.size() + 1);
        for (const std::string& n : names) ptrs.push_back(n.c_str());
        ptrs.push_back(nullptr);
    }
};

const HtpStack& htpStack() {
    static const HtpStack stack;
    return stack;
}

}  // namespace

/// Length of a nullptr-terminated list. Every list above ends in nullptr, so
/// counting beats a literal: a literal is one more thing to forget when a
/// library is added, and forgetting it means preload() silently stops after
/// the first N entries.
static int listLen(const char* const* list) {
    int n = 0;
    while (list != nullptr && list[n] != nullptr) ++n;
    return n;
}

const char* const* runtimeLibraries(Runtime runtime, Ep ep, int& count) {
    count = 0;
    switch (runtime) {
        case Runtime::LiteRT:
            switch (ep) {
                case Ep::Gpu:
                    count = listLen(kLiteRtGpu); return kLiteRtGpu;
                case Ep::Htp:
                    count = listLen(kLiteRtQnn); return kLiteRtQnn;
                case Ep::Npu:
                    count = listLen(kLiteRtNpu); return kLiteRtNpu;
                default:
                    count = listLen(kLiteRtBase); return kLiteRtBase;
            }
        case Runtime::Qnn: {
            const HtpStack& s = htpStack();
            count = static_cast<int>(s.ptrs.size()) - 1;
            return s.ptrs.data();
        }
        case Runtime::OnnxRuntime:
        case Runtime::Ncnn:
        default:
            return kNone;
    }
}

}  // namespace infer
}  // namespace aimbotng
