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
#include "inference/litert_npu_engine.h"
#if AIMBOTNG_HAVE_NEUROPILOT
#include "inference/neuropilot_engine.h"
#endif
#if AIMBOTNG_HAVE_NEURON
#include "inference/neuron_engine.h"
#endif
#include "inference/ort_engine.h"

#include <android/log.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

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
    // LiteRT 2.x: a different runtime from everything above (libLiteRt.so with
    // vendor dispatch libraries), despite the shared Runtime. Kept last in the
    // LiteRT group because it is the one that compiles on the device and the
    // one a user should reach for deliberately.
    {Runtime::LiteRT, Ep::Npu},
    // Neuron (MediaTek APU). Gated by AIMBOTNG_HAVE_NEURON: this backend needs
    // no MediaTek SDK at build time — the adapter is dlopen'd at runtime and the
    // delegate is ours — but its source is kept out of the public repo, so it is
    // compiled only when that flag is set (developer machines / local builds).
    //
    // This entry was missing for a while, and the symptom was not "the row is
    // absent" but "the row is grey and nothing says why": engineRowDisabled()
    // asks epAvailable(), epAvailable() asks validPair() and returns false on
    // the first line without a word, so neuron::Adapter was never even asked.
    // Every explanation added to the adapter was unreachable code. If a pair
    // is missing here, the engine does not exist as far as the menu is
    // concerned — keep this table and `implemented()` in step.
    // Neuron (MediaTek APU). Gated by AIMBOTNG_HAVE_NEURON: this backend needs
    // no MediaTek SDK at build time — the adapter is dlopen'd at runtime and the
    // delegate is ours — but its source is kept out of the public repo, so it is
    // compiled only when that flag is set (developer machines / local builds).
    //
    // This entry was missing for a while, and the symptom was not "the row is
    // absent" but "the row is grey and nothing says why": engineRowDisabled()
    // asks epAvailable(), epAvailable() asks validPair() and returns false on
    // the first line without a word, so neuron::Adapter was never even asked.
    // Every explanation added to the adapter was unreachable code. If a pair
    // is missing here, the engine does not exist as far as the menu is
    // concerned — keep this table and `implemented()` in step.
    //
    // Temporarily commented out AIMBOTNG_HAVE_NEURON gate so Neuron is wired
    // in unconditionally (matches the backup build that 9400+ users run).
    // {Runtime::LiteRT, Ep::Neuron},
    {Runtime::LiteRT, Ep::Neuron},
#if AIMBOTNG_HAVE_NEUROPILOT
    // MediaTek NeuroPilot: a third runtime again (libtflite_mtk.mtk.so), and
    // the only one here whose library lives on the system rather than in the
    // APK — so it is the one whose row can go grey for a reason the user did
    // nothing to cause.
    {Runtime::LiteRT, Ep::NeuroPilot},
#endif

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
               e == Ep::Htp     || e == Ep::Npu
        // Temporarily commented out the AIMBOTNG_HAVE_NEURON / _NEUROPILOT
        // gates so Neuron and NeuroPilot are wired in unconditionally,
        // matching the backup build.
        // #if AIMBOTNG_HAVE_NEURON
               || e == Ep::Neuron
        // #endif
        // #if AIMBOTNG_HAVE_NEUROPILOT
               || e == Ep::NeuroPilot
        // #endif
        ;
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
        case Ep::Htp:        return "HTP";
        case Ep::Npu:        return "NPU";
#if AIMBOTNG_HAVE_NEURON
        case Ep::Neuron:     return "Neuron";
#endif
#if AIMBOTNG_HAVE_NEUROPILOT
        case Ep::NeuroPilot: return "NeuroPilot";
#endif
        default:             return "?";
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
    // Both of these returns used to be silent, and that is how a greyed-out
    // menu row with no explanation in the log became possible. A row is grey
    // for exactly three reasons — the pair is not in the table, the engine is
    // not written, or the probe failed — and each now says so.
    if (!validPair(r, e)) {
        LOGW("backend unavailable: %s — not a pair this build knows about "
             "(engine.cpp kPairs is the list; a missing entry hides the whole "
             "engine, not just the row)", pairLabel(r, e));
        return false;
    }
    if (!implemented(r, e)) {
        LOGW("backend unavailable: %s — engine not implemented yet", pairLabel(r, e));
        return false;
    }

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
        // LiteRT 2.x NPU: the runtime plus a dispatch/compiler-plugin pair for
        // *this* vendor. Existence only — deliberately no dlopen. Loading
        // libLiteRt.so pulls in 5 MB of runtime and, on some vendors, starts
        // poking at the NPU; doing that to answer a menu question is how a
        // settings screen ends up costing a second of boot. Whether it actually
        // initialises is LiteRtNpuEngine::load()'s problem, and it has a better
        // error message for it.
        if (e == Ep::Npu) {
            const auto& caps = capabilities();
            if (!caps.isQualcomm && !caps.isMediaTek) {
                p.ok  = false;
                p.why = "no MediaTek or Qualcomm SoC detected — LiteRT 2.x NPU "
                        "dispatch only ships for those two";
                return false;
            }
            if (!libraryPresent("libLiteRt.so")) {
                p.ok  = false;
                p.why = "libLiteRt.so is not on this device (LiteRT 2.x AAR)";
                return false;
            }
            const char* dispatch = caps.isQualcomm ? "libLiteRtDispatch_Qualcomm.so"
                                                   : "libLiteRtDispatch_MediaTek.so";
            const char* plugin   = caps.isQualcomm ? "libLiteRtCompilerPlugin_Qualcomm.so"
                                                   : "libLiteRtCompilerPlugin_MediaTek.so";
            if (!libraryPresent(dispatch)) {
                p.ok  = false;
                p.why = std::string(dispatch) + " is not on this device";
                return false;
            }
            // The dispatch library alone is not enough. Without the matching
            // compiler plugin the vendor options are accepted and ignored, and
            // the graph runs on a path an order of magnitude slower with no
            // error anywhere — so this is checked, not assumed.
            if (!libraryPresent(plugin)) {
                p.ok  = false;
                p.why = std::string(plugin) +
                        " is missing — without it the NPU options are silently "
                        "ignored (the non-JIT archive ships dispatch only)";
                return false;
            }
            p.ok = true;
            LOGI("backend available: %s (%s, dispatch + JIT compiler plugin)",
                 pairLabel(r, e), caps.isQualcomm ? "Qualcomm" : "MediaTek");
            return true;
        }
#if AIMBOTNG_HAVE_NEUROPILOT
        // NeuroPilot: the one backend whose library is a *system* one rather
        // than something we shipped, so the only honest answer is to try. This
        // really does dlopen several megabytes of MediaTek's TFLite fork, which
        // is expensive enough that it is worth saying why it is worth it:
        //   - every other failure mode here is "the .so is not in the APK",
        //     which libraryPresent() answers without loading anything;
        //   - this one is "the device does not have NeuroPilot", which only
        //     dlopen can distinguish from "the device has it but the shell
        //     namespace cannot reach it" — and those two have different fixes.
        // It runs once: epAvailable() caches per pair.
        if (e == Ep::NeuroPilot) {
            std::string why;
            if (!neuropilotAvailable(&why)) {
                p.ok  = false;
                p.why = why;
                LOGW("backend unavailable: %s — %s", pairLabel(r, e), why.c_str());
                return false;
            }
            p.ok = true;
            LOGI("backend available: %s", pairLabel(r, e));
            return true;
        }
#endif
        // Neuron: unlike NeuroPilot this backend needs no MediaTek SDK at build
        // time — the adapter is dlopen'd at runtime and the whole delegate is
        // ours — so the probe is the same shape but the failure modes are
        // different and each gets its own sentence:
        //   - Android < 12: no NDK AHardwareBuffer/APU path worth taking, and
        //     the adapter refuses to expose a device anyway;
        //   - adapter .so absent: a device with no MediaTek NPU at all;
        //   - adapter present but device count 0: the NPU is there but the
        //     APU driver has not been brought up (or is owned by another
        //     process), which is a different fix again.
        // neuronAvailable() distinguishes them; epAvailable() caches per pair,
        // so the dlopen cost is paid once.
        // Neuron: unlike NeuroPilot this backend needs no MediaTek SDK at build
        // time — the adapter is dlopen'd at runtime and the whole delegate is
        // ours — so the probe is the same shape but the failure modes are
        // different and each gets its own sentence:
        //   - Android < 12: no NDK AHardwareBuffer/APU path worth taking, and
        //     the adapter refuses to expose a device anyway;
        //   - adapter .so absent: a device with no MediaTek NPU at all;
        //   - adapter present but device count 0: the NPU is there but the
        //     APU driver has not been brought up (or is owned by another
        //     process), which is a different fix again.
        // neuronAvailable() distinguishes them; epAvailable() caches per pair,
        // so the dlopen cost is paid once.
        //
        // Temporarily commented out the AIMBOTNG_HAVE_NEURON gate so the
        // Neuron probe runs unconditionally, matching the backup build.
        // #if AIMBOTNG_HAVE_NEURON
        if (e == Ep::Neuron) {
            std::string why;
            if (!neuronAvailable(&why)) {
                p.ok  = false;
                p.why = why;
                // Greyed rows used to be silent here, so a device-side capture
                // showed the option disabled and nothing else. The reason is
                // the whole point of asking.
                LOGW("backend unavailable: %s — %s", pairLabel(r, e), why.c_str());
                return false;
            }
            p.ok = true;
            LOGI("backend available: %s", pairLabel(r, e));
            return true;
        }
        // #endif
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
    LOGW("backend unavailable: %s — %s", pairLabel(r, e), p.why.c_str());
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
        // Ep::Npu is a different runtime in a LiteRT-shaped coat: libLiteRt.so
        // with vendor dispatch, not libtensorflowlite_jni.so with delegates. It
        // gets its own engine class rather than a branch inside LiteRtEngine
        // because almost nothing between them is shared — not the API, not the
        // handles, not the way acceleration is selected.
        if (e == Ep::Npu) {
            auto engine = std::make_unique<LiteRtNpuEngine>();
            if (why != nullptr) why->clear();
            return engine;
        }
#if AIMBOTNG_HAVE_NEUROPILOT
        // NeuroPilot: MediaTek's TFLite fork. Like Ep::Npu it is a whole
        // runtime behind a LiteRT-shaped name, but it is not the same library
        // and shares no handles with it, so it gets its own engine too.
        if (e == Ep::NeuroPilot) {
            auto engine = std::make_unique<NeuroPilotEngine>();
            if (why != nullptr) why->clear();
            return engine;
        }
#endif
        // Neuron: Google's TFLite interpreter plus our own delegate, so it is
        // closer to LiteRtEngine than to the two above — same library, same
        // handles — but it still gets its own class because the delegate, the
        // partition policy and the failure reporting are all different, and
        // folding that into LiteRtEngine would mean two unrelated ways to pick
        // a delegate living in one function.
#if AIMBOTNG_HAVE_NEURON
        if (e == Ep::Neuron) {
            auto engine = std::make_unique<NeuronEngine>();
            if (why != nullptr) why->clear();
            return engine;
        }
#endif
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
