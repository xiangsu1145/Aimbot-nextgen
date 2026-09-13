// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of engine.h's factory and backend.h's table
//
//  The pair table and the factory live in the same translation unit on purpose.
//  They are two views of one fact — "does this backend exist and work" — and the
//  version of this code that kept them apart is the version where the menu
//  offered a backend the factory would not build.
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/engine.h"

#include "inference/capabilities.h"
#include "inference/libpath.h"
#include "inference/litert_engine.h"
#include "inference/ort_engine.h"

#include <android/log.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace {

// ── The matrix ───────────────────────────────────────────────────────────────
//
// Order is menu order. Every entry is a pair the *design* supports; whether it
// runs here is a separate question answered by epAvailable().

const Pair kPairs[] = {
    // ONNX Runtime. One library, four ways to run it.
    {Runtime::OnnxRuntime, Ep::Xnnpack},   // the default: XNNPACK kernels
    {Runtime::OnnxRuntime, Ep::Cpu},       // plain MLAS, for when XNNPACK is wrong
    {Runtime::OnnxRuntime, Ep::Nnapi},     // the platform delegate
    {Runtime::OnnxRuntime, Ep::Htp},       // needs an ORT built with the QNN EP

    // LiteRT. Listed separately so the user can pick between the default
    // CPU path and an explicit XNNPACK delegate, even though the latter is
    // typically a no-op on top of the former.
    {Runtime::LiteRT, Ep::Cpu},
    {Runtime::LiteRT, Ep::Xnnpack},
    {Runtime::LiteRT, Ep::Nnapi},
    {Runtime::LiteRT, Ep::Gpu},
    {Runtime::LiteRT, Ep::Htp},

    // NCNN.
    {Runtime::Ncnn, Ep::Cpu},
    {Runtime::Ncnn, Ep::Vulkan},

    // QNN driven directly, off a pre-compiled context binary.
    {Runtime::Qnn, Ep::Cpu},
    {Runtime::Qnn, Ep::Gpu},
    {Runtime::Qnn, Ep::Htp},

    {Runtime::Unknown, Ep::Default},   // terminator
};

struct Probe {
    bool        done = false;
    bool        ok   = false;
    std::string why;
};

std::mutex g_probeMutex;
Probe      g_probes[static_cast<int>(Runtime::Count)][static_cast<int>(Ep::Count)];

/// Whether an engine for this pair has been written at all. Kept apart from
/// availability so the menu can say "not implemented yet" rather than "not
/// supported on this device" — two different sentences for two different
/// problems, and only one of them is the user's problem.
///
/// QNN HTP under LiteRT is compiled in: a Snapdragon device with the QNN
/// libraries available can run it. Whether a *particular* device can run
/// it is decided by epAvailable() — MTK devices see the row grey at
/// runtime because they have no QNN runtime, even though the engine code
/// is present and links.
bool implemented(Runtime r, Ep e) {
    if (r == Runtime::OnnxRuntime) {
        return e == Ep::Xnnpack || e == Ep::Cpu;
    }
    if (r == Runtime::LiteRT) {
        return e == Ep::Cpu     || e == Ep::Xnnpack ||
               e == Ep::Nnapi   || e == Ep::Gpu     ||
               e == Ep::Htp;
    }
    return false;
}

}  // namespace

// ── Names ───────────────────────────────────────────────────────────────────

const char* runtimeLabel(Runtime r) {
    switch (r) {
        case Runtime::OnnxRuntime: return "ONNX Runtime";
        case Runtime::LiteRT:      return "LiteRT";
        case Runtime::Ncnn:        return "NCNN";
        case Runtime::Qnn:         return "QNN";
        default:                   return "Unknown";
    }
}

const char* runtimeExtension(Runtime r) {
    switch (r) {
        case Runtime::OnnxRuntime: return "onnx";
        case Runtime::LiteRT:      return "tflite";
        case Runtime::Ncnn:        return "param";
        case Runtime::Qnn:         return "bin";
        default:                   return "";
    }
}

const char* epLabel(Ep e) {
    switch (e) {
        case Ep::Default: return "Default";
        case Ep::Cpu:     return "CPU";
        case Ep::Xnnpack: return "XNNPACK";
        case Ep::Nnapi:   return "NNAPI";
        case Ep::Gpu:     return "GPU";
        case Ep::Vulkan:  return "Vulkan";
        case Ep::Htp:     return "HTP";
        default:          return "?";
    }
}

void pairLabelTo(char* out, size_t outSize, Runtime r, Ep e) {
    if (out == nullptr || outSize == 0) return;
    snprintf(out, outSize, "%s / %s", runtimeLabel(r), epLabel(e));
}

const char* pairLabel(Runtime r, Ep e) {
    // A small ring of buffers so two labels can appear in one printf without
    // the second overwriting the first. Four is more than any call site needs
    // and costs nothing.
    static thread_local char bufs[4][64];
    static thread_local int  next = 0;
    char* buf = bufs[next];
    next = (next + 1) & 3;
    pairLabelTo(buf, sizeof(bufs[0]), r, e);
    return buf;
}

// ── The matrix ──────────────────────────────────────────────────────────────

const Pair* allPairs(int& count) {
    count = static_cast<int>(sizeof(kPairs) / sizeof(kPairs[0])) - 1;
    return kPairs;
}

bool validPair(Runtime r, Ep e) {
    int count = 0;
    const Pair* pairs = allPairs(count);
    for (int i = 0; i < count; ++i) {
        if (pairs[i].runtime == r && pairs[i].ep == e) return true;
    }
    return false;
}

const Pair* pairsForRuntime(Runtime r, int& count) {
    // The table is already grouped by runtime, so the matching entries are
    // contiguous. Returned as a pointer into it rather than a copy, which means
    // the caller must use `count` and not walk to the terminator — the
    // terminator is only at the very end.
    count = 0;
    int total = 0;
    const Pair* pairs = allPairs(total);
    int first = -1;
    for (int i = 0; i < total; ++i) {
        if (pairs[i].runtime != r) {
            if (first >= 0) break;
            continue;
        }
        if (first < 0) first = i;
        ++count;
    }
    if (first < 0) {
        count = 0;
        return nullptr;
    }
    return pairs + first;
}

const char* epImplementationState(Runtime r, Ep e) {
    if (!validPair(r, e)) return "not a combination";
    if (implemented(r, e)) return "ready";
    return "not implemented yet";
}

// ── Probing ─────────────────────────────────────────────────────────────────

bool epAvailable(Runtime r, Ep e) {
    if (!validPair(r, e)) return false;
    if (!implemented(r, e)) return false;

    const int ri = static_cast<int>(r);
    const int ei = static_cast<int>(e);
    if (ri < 0 || ri >= static_cast<int>(Runtime::Count) ||
        ei < 0 || ei >= static_cast<int>(Ep::Count)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_probeMutex);
    Probe& p = g_probes[ri][ei];
    if (p.done) return p.ok;
    p.done = true;

    if (r == Runtime::OnnxRuntime) {
        // The one backend whose file is not a sibling directory of anything: it
        // arrives as a single .so from Maven. Loading it here rather than at
        // first inference is what lets the menu grey the option out instead of
        // letting a tap fail seconds later.
        std::string why;
        if (preload("libonnxruntime.so", &why) == nullptr) {
            p.ok  = false;
            p.why = "libonnxruntime.so is not loadable — " + why;
            return false;
        }
        p.ok = true;
        LOGI("backend available: %s", pairLabel(r, e));
        return true;
    }

    if (r == Runtime::LiteRT) {
        // QNN HTP under LiteRT: requires (a) a Qualcomm SoC we ship a skel
        // for, and (b) the QNN .so files on the device. The skel library
        // is selected by htpArch in capabilities(); when that is zero, the
        // device is a Snapdragon we have not packaged a skel for, and the
        // delegate will refuse the graph. Better to grey the row than to
        // let the user tap it and see a runtime failure with no clue.
        if (e == Ep::Htp) {
            const auto& caps = capabilities();
            if (!caps.isQualcomm || caps.htpArch <= 0) {
                p.ok  = false;
                p.why = caps.isQualcomm
                          ? "QNN HTP skel for this SoC is not shipped"
                          : "QNN HTP requires a Qualcomm SoC";
                return false;
            }
            // The delegate .so is loaded lazily — only at model load —
            // because probing would cost an extra dlopen on every device.
            // The probe result is cacheable: once we know it is there, it
            // does not move. We use libraryPresent() rather than dlopen
            // so a flaky preload is not attributed to the backend.
            if (!libraryPresent("libQnnTFLiteDelegate.so")) {
                p.ok  = false;
                p.why = "libQnnTFLiteDelegate.so is not on this device";
                return false;
            }
            p.ok = true;
            LOGI("backend available: %s (htp v%d)", pairLabel(r, e), caps.htpArch);
            return true;
        }
        std::string why;
        if (preload("libtensorflowlite.so", &why) == nullptr) {
            p.ok  = false;
            p.why = "libtensorflowlite.so is not loadable — " + why;
            return false;
        }
        // The runtime library is here, but NNAPI/GPU delegates live in
        // their own .so files. We don't dlopen them here — that would
        // cost a probe on every device, and a missing delegate is also a
        // load-time failure, not an availability failure. Their existence
        // is checked again when LiteRtEngine::load() runs buildDelegate().
        p.ok = true;
        LOGI("backend available: %s", pairLabel(r, e));
        return true;
    }

    p.ok  = false;
    p.why = "engine not implemented yet";
    return false;
}

const char* epUnavailableReason(Runtime r, Ep e) {
    if (!validPair(r, e)) return "not a combination this build knows about";
    if (!implemented(r, e)) return "engine not implemented yet";

    const int ri = static_cast<int>(r);
    const int ei = static_cast<int>(e);
    if (ri < 0 || ri >= static_cast<int>(Runtime::Count) ||
        ei < 0 || ei >= static_cast<int>(Ep::Count)) {
        return "out of range";
    }

    std::lock_guard<std::mutex> lock(g_probeMutex);
    const Probe& p = g_probes[ri][ei];
    if (!p.done) return "not probed yet";
    return p.ok ? "" : p.why.c_str();
}

void resetProbes() {
    std::lock_guard<std::mutex> lock(g_probeMutex);
    for (auto& row : g_probes) {
        for (Probe& p : row) p = Probe{};
    }
}

// ── Factory ─────────────────────────────────────────────────────────────────

std::unique_ptr<Engine> create(Runtime r, Ep e, std::string* why) {
    auto fail = [&](const char* msg) -> std::unique_ptr<Engine> {
        if (why != nullptr) *why = msg;
        return nullptr;
    };

    if (!validPair(r, e)) return fail("no such runtime/accelerator combination");
    if (!implemented(r, e)) {
        if (why != nullptr) {
            *why = std::string(runtimeLabel(r)) + " / " + epLabel(e) +
                   " has no engine in this build yet";
        }
        return nullptr;
    }

    if (r == Runtime::OnnxRuntime) {
        // Deliberately no availability check here: create() builds an engine,
        // it does not run one, and a caller that wants to know whether a backend
        // works asks epAvailable() before offering it. Failing here for a reason
        // that is not about *this* call would make the two answers impossible to
        // tell apart when they disagree.
        auto engine = std::make_unique<OrtEngine>(e);
        if (why != nullptr) why->clear();
        return engine;
    }

    if (r == Runtime::LiteRT) {
        // LiteRtEngine::load() does its own runtime-specific gating (it asks
        // buildDelegate() for a delegate and surfaces a clear error if the
        // device lacks a usable skel). The factory just constructs the
        // object; the constructor is cheap.
        auto engine = std::make_unique<LiteRtEngine>(e);
        if (why != nullptr) why->clear();
        return engine;
    }

    return fail("unreachable");
}

LoadedEngine createAndLoad(const ModelSpec& spec, Runtime r, Ep e) {
    LoadedEngine result;
    result.spec = spec;

    std::string why;
    result.engine = create(r, e, &why);
    if (result.engine == nullptr) {
        result.status = Status::bad(why.empty() ? "could not create engine" : why);
        return result;
    }

    result.status = result.engine->load(spec);
    if (!result.status.ok) {
        result.engine.reset();
    }
    return result;
}

}  // namespace infer
}  // namespace aimbotng
