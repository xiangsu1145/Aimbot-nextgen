// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of litert_engine.h
//
//  TensorFlow Lite is shipped as `libtensorflowlite_jni.so` under
//  app/src/main/jniLibs/<abi>/ and linked into libaimbotng.so at build time
//  (see CMakeLists.txt). That means the C API symbols are available directly
//  to the engine — no dlopen dance for the base runtime. The XNNPACK
//  delegate is statically built into the same .so and is therefore also
//  available directly.
//
//  Only one delegate still needs its own dlopen: the GPU one, which ships as
//  `libtensorflowlite_gpu_jni.so` (the name the TFLite GPU AAR uses — and the
//  one libpath.cpp preloads). NNAPI needs none at all: its C API is exported
//  by `libtensorflowlite_jni.so`, the runtime we already link, so it resolves
//  with RTLD_DEFAULT. Both are still resolved through
//  resolveDelegateSymbol(), which tries the loaded process first and then
//  preload()s a shipped .so by absolute path — the daemon is app_process in
//  /system/bin and a bare dlopen of an APK library fails there.
//
//  * Ep::Cpu      — no extra delegate; the LiteRT default CPU path.
//  * Ep::Xnnpack  — explicit XNNPACK delegate on top of the default.
//  * Ep::Nnapi    — NNAPI delegate, out of libtensorflowlite_jni.so.
//  * Ep::Gpu      — OpenCL GPU delegate, out of libtensorflowlite_gpu_jni.so.
//  * Ep::Htp      — Qualcomm's QNN runtime via libQnnTFLiteDelegate.so.
//
//  preprocess() and decode() come from preprocess.h / postprocess.h, the
//  same ones OrtEngine uses. The model_type.cpp probe runs TFLite too, so
//  the menu's fp32 / fp16 / int8 label works for .tflite entries without
//  the UI knowing which engine produced the answer.
// ─────────────────────────────────────────────────────────────────────────────

#include "inference/litert_engine.h"
#include "inference/libpath.h"

#include "tensorflow/lite/c/c_api.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/delegates/gpu/delegate.h"
#include "tensorflow/lite/delegates/nnapi/nnapi_delegate_c_api.h"
#include "tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h"
#include "qnn/TFLiteDelegate/QnnTFLiteDelegate.h"

#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace {

using Clock = std::chrono::steady_clock;

float msSince(Clock::time_point t0) {
    return static_cast<float>(
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
}

const char* tfliteTypeName(TfLiteType t) {
    switch (t) {
        case kTfLiteFloat32: return "fp32";
        case kTfLiteFloat16: return "fp16";
        case kTfLiteInt8:    return "int8";
        case kTfLiteUInt8:   return "uint8";
        case kTfLiteInt16:   return "int16";
        case kTfLiteInt32:   return "int32";
        default:             return "?";
    }
}

/// Candidate .so files for each delegate EP, in load order.
///
/// GPU: the delegate ships as `libtensorflowlite_gpu_jni.so` — that is the name
/// the TensorFlow Lite GPU AAR uses and the one libpath.cpp's kLiteRtGpu
/// preloads. `libtensorflowlite_gpu_delegate.so` is kept as a second guess so a
/// build that renamed it still resolves.
///
/// NNAPI: there is no `libtensorflowlite_nnapi_delegate.so` in any TensorFlow
/// release. Its C API — TfLiteNnapiDelegateCreate/Delete/OptionsDefault — is
/// exported by `libtensorflowlite_jni.so`, which libaimbotng.so already links
/// as DT_NEEDED, so the normal case needs no dlopen at all. The bare name stays
/// as a fallback for a runtime that does split it out.
const char* const kGpuLibs[] = {
    "libtensorflowlite_gpu_jni.so",
    "libtensorflowlite_gpu_delegate.so",
    nullptr,
};
const char* const kNnapiLibs[] = {
    "libtensorflowlite_jni.so",
    "libtensorflowlite_nnapi_delegate.so",
    nullptr,
};

/// Finds `sym` in whichever library provides this EP's delegate.
///
/// Cheapest first:
///   1. Already in the process — covers NNAPI (its symbols live in the TFLite
///      runtime we link) and anything preload() put in with RTLD_GLOBAL.
///   2. A .so we ship, opened by *absolute path* out of libraryDir(). preload()
///      and not dlopen() because the daemon is `app_process` in /system/bin:
///      the dynamic linker does not search the APK's lib dir, so a bare dlopen
///      of a library we definitely shipped fails there while working fine in
///      the app process.
///
/// Declared before unload() because the delegate's deleter has to be found the
/// same way its factory was — a create/delete pair resolved by different rules
/// is how you end up leaking a delegate or calling the wrong destructor.
void* resolveDelegateSymbol(const char* sym, const char* const* libs) {
    if (void* p = dlsym(RTLD_DEFAULT, sym)) return p;
    for (int i = 0; libs[i] != nullptr; ++i) {
        std::string why;
        if (void* lib = preload(libs[i], &why)) {
            if (void* p = dlsym(lib, sym)) return p;
        }
    }
    return nullptr;
}

}  // namespace

struct LiteRtEngine::Impl {
    TfLiteModel*       model     = nullptr;
    TfLiteInterpreter* interp    = nullptr;
    TfLiteDelegate*    delegate  = nullptr;  // owned here

    PreprocessConfig  pre;
    PostprocessConfig post;

    TfLiteType inType  = kTfLiteFloat32;
    TfLiteType outType = kTfLiteFloat32;

    int  inputW = 0;
    int  inputH = 0;
    bool inputNhwc = true;

    int  anchors     = 0;
    int  channels    = 0;
    bool channelsFirst = false;

    // Split-head (dual-output) exports: e.g. best_int8.tflite emits
    //   output A = [1, nc, N] scores  +  output B = [1, 4, N] boxes
    // instead of one concatenated [1, 4+nc, N] tensor. The single-tensor
    // path below can only see output 0, so decoding it as boxes+scores
    // yields zero boxes. When dualHead is true the engine dequantizes
    // BOTH tensors and interleaves them into `decoded` as [4+nc] per
    // anchor, after which the shared decode() works unchanged.
    bool dualHead = false;
    int  boxesOutIdx  = -1;
    int  scoresOutIdx = -1;
    TfLiteType boxesType  = kTfLiteFloat32;
    TfLiteType scoresType = kTfLiteFloat32;
    bool boxesChannelsFirst  = true;
    bool scoresChannelsFirst = true;

    std::vector<float>  decoded;
    std::vector<float>  inputF32;
    std::vector<uint8_t> inputU8;

    std::atomic<float> confidence{0.5f};
    int loggedRuns = 0;

    std::string inShapeText, outShapeText;
};

// ── Lifecycle ───────────────────────────────────────────────────────────────

LiteRtEngine::LiteRtEngine(Ep ep) : ep_(ep) {
    switch (ep) {
        case Ep::Cpu:     activeName_ = "LiteRT / CPU";     break;
        case Ep::Xnnpack: activeName_ = "LiteRT / XNNPACK"; break;
        case Ep::Nnapi:   activeName_ = "LiteRT / NNAPI";   break;
        case Ep::Gpu:     activeName_ = "LiteRT / GPU";     break;
        case Ep::Htp:     activeName_ = "LiteRT / QNN HTP"; break;
        default:          activeName_ = "LiteRT";           break;
    }
    status_ = Status::good();
}

LiteRtEngine::~LiteRtEngine() { unload(); }

void LiteRtEngine::unload() {
    if (!impl_) return;
    if (impl_->interp) { TfLiteInterpreterDelete(impl_->interp); impl_->interp = nullptr; }
    if (impl_->delegate) {
        // One delegate factory — one matching destroyer. The factory name is
        // kept on the engine (activeName_) so the right delete is obvious.
        if (ep_ == Ep::Nnapi) {
            using DeleteFn = void (*)(TfLiteDelegate*);
            DeleteFn del = reinterpret_cast<DeleteFn>(
                resolveDelegateSymbol("TfLiteNnapiDelegateDelete", kNnapiLibs));
            if (del != nullptr) del(impl_->delegate);
        } else if (ep_ == Ep::Xnnpack) {
            TfLiteXNNPackDelegateDelete(impl_->delegate);
        } else if (ep_ == Ep::Gpu) {
            using DeleteFn = void (*)(TfLiteDelegate*);
            DeleteFn del = reinterpret_cast<DeleteFn>(
                resolveDelegateSymbol("TfLiteGpuDelegateV2Delete", kGpuLibs));
            if (del != nullptr) del(impl_->delegate);
        } else if (ep_ == Ep::Htp) {
            // QNN delegate: TfLiteQnnDelegateDelete lives in
            // libQnnTFLiteDelegate.so, already linked (see buildDelegate).
            // It was previously leaked here (no branch); free it like the
            // other delegates so model reloads don't accumulate HTP graphs.
            using DeleteFn = void (*)(TfLiteDelegate*);
            DeleteFn del = reinterpret_cast<DeleteFn>(
                dlsym(RTLD_DEFAULT, "TfLiteQnnDelegateDelete"));
            if (del != nullptr) del(impl_->delegate);
        }
        impl_->delegate = nullptr;
    }
    if (impl_->model) { TfLiteModelDelete(impl_->model); impl_->model = nullptr; }
    impl_.reset();
    status_ = Status::bad("not loaded");
    timing_ = Timing{};
}

bool LiteRtEngine::ready() const {
    return impl_ != nullptr && impl_->interp != nullptr && impl_->anchors > 0;
}

const char* LiteRtEngine::activeName() const { return activeName_.c_str(); }

void LiteRtEngine::setConfidence(float c) {
    if (impl_ == nullptr) return;
    impl_->confidence.store(std::clamp(c, 0.01f, 0.99f), std::memory_order_relaxed);
}

namespace {

/// Builds the delegate for the engine's chosen Ep, or returns nullptr for
/// Ep::Cpu (where "no delegate" is the whole point).
TfLiteDelegate* buildDelegate(Ep ep, const std::string& cacheToken,
                              std::string* why, int htpPerfMode = 1) {
    if (ep == Ep::Cpu) return nullptr;

    if (ep == Ep::Xnnpack) {
        auto opts = TfLiteXNNPackDelegateOptionsDefault();
        return TfLiteXNNPackDelegateCreate(&opts);
    }

    if (ep == Ep::Nnapi) {
        using OptionsDefaultFn = TfLiteNnapiDelegateOptions (*)();
        using CreateFn = TfLiteDelegate* (*)(const TfLiteNnapiDelegateOptions*);
        OptionsDefaultFn optsDefault = reinterpret_cast<OptionsDefaultFn>(
            resolveDelegateSymbol("TfLiteNnapiDelegateOptionsDefault", kNnapiLibs));
        CreateFn create = reinterpret_cast<CreateFn>(
            resolveDelegateSymbol("TfLiteNnapiDelegateCreate", kNnapiLibs));
        if (optsDefault == nullptr || create == nullptr) {
            if (why) *why = "NNAPI delegate symbols missing (TfLiteNnapiDelegateCreate)";
            return nullptr;
        }
        TfLiteNnapiDelegateOptions opts = optsDefault();
        opts.model_token = cacheToken.empty() ? nullptr : cacheToken.c_str();
        TfLiteDelegate* d = create(&opts);
        if (d == nullptr && why) *why = "TfLiteNnapiDelegateCreate returned null";
        return d;
    }

    if (ep == Ep::Gpu) {
        using OptionsDefaultFn = TfLiteGpuDelegateOptionsV2 (*)();
        using CreateFn = TfLiteDelegate* (*)(const TfLiteGpuDelegateOptionsV2*);
        OptionsDefaultFn optsDefault = reinterpret_cast<OptionsDefaultFn>(
            resolveDelegateSymbol("TfLiteGpuDelegateOptionsV2Default", kGpuLibs));
        CreateFn create = reinterpret_cast<CreateFn>(
            resolveDelegateSymbol("TfLiteGpuDelegateV2Create", kGpuLibs));
        if (optsDefault == nullptr || create == nullptr) {
            if (why) *why = "GPU delegate not available: libtensorflowlite_gpu_jni.so is "
                            "missing or has no TfLiteGpuDelegateV2Create";
            return nullptr;
        }
        TfLiteGpuDelegateOptionsV2 opts = optsDefault();
        opts.inference_preference = TFLITE_GPU_INFERENCE_PREFERENCE_FAST_SINGLE_ANSWER;
        opts.inference_priority1 = TFLITE_GPU_INFERENCE_PRIORITY_MIN_LATENCY;
        TfLiteDelegate* d = create(&opts);
        if (d == nullptr && why) *why = "TfLiteGpuDelegateV2Create returned null";
        return d;
    }

    if (ep == Ep::Htp) {
        // Qualcomm's HTP runtime. The factory and the QNN libraries are
        // linked into libaimbotng.so statically, so the call is a direct
        // symbol — no dlopen. The same goes for the unload() path: the
        // deleter symbol comes from the same .so we already have.
        TfLiteQnnDelegateOptions qnn = TfLiteQnnDelegateOptionsDefault();
        qnn.backend_type           = kHtpBackend;
        // skel_library_dir: QNN loads the per-SoC skel from this directory
        // at runtime (libQnnHtpV68Skel.so, libQnnHtpV73Skel.so, …). libraryDir()
        // returns our own libaimbotng.so's directory — the same place the
        // skels live in jniLibs.
        qnn.skel_library_dir       = libraryDir();
        // Cache dir for the compiled HTP graph. /data/local/tmp is writable
        // by the shell daemon (uid 2000) without further setup.
        qnn.cache_dir              = "/data/local/tmp/aimbot_qnn";
        // Per-model token: each model gets a unique token (its basename,
        // propagated via ModelSpec::cacheToken) so the HTP graph cache
        // lives in its own file and warm-up work survives cold launches.
        qnn.model_token            = cacheToken.empty()
                                          ? nullptr
                                          : cacheToken.c_str();
        // Latency-critical real-time workload: vote for the HTP performance
        // mode so the DSP scheduler does not throttle us when the game
        // contends for NPU resources. The mode is the per-model vote the
        // user picks in the Model Settings dialog (default: Sustained High
        // Performance). It is the raw TfLiteQnnDelegateHtpPerformanceMode int.
        const int pm = (htpPerfMode < 0 || htpPerfMode > 9) ? 1 : htpPerfMode;
        qnn.htp_options.performance_mode =
            static_cast<TfLiteQnnDelegateHtpPerformanceMode>(pm);
        TfLiteDelegate* d = TfLiteQnnDelegateCreate(&qnn);
        if (d == nullptr && why) *why = "TfLiteQnnDelegateCreate returned null";
        return d;
    }

    if (why) *why = "no delegate factory for this Ep";
    return nullptr;
}

}  // namespace

Status LiteRtEngine::load(const ModelSpec& spec) {
    unload();
    const auto t0 = Clock::now();

    if (spec.path.empty()) return status_ = Status::bad("no model path");

    // Make sure every .so this (Runtime, Ep) needs is loaded into the
    // namespace before we go further. The list lives in libpath.cpp so
    // adding a new skel there does not require editing this file.
    {
        int n = 0;
        const char* const* names = runtimeLibraries(Runtime::LiteRT, ep_, n);
        if (names != nullptr && n > 0) {
            std::string why;
            preloadAll(names, &why);
        }
    }

    try {
        auto impl = std::make_unique<Impl>();

        impl->model = TfLiteModelCreateFromFile(spec.path.c_str());
        if (impl->model == nullptr) {
            return status_ = Status::bad("cannot read model file: " + spec.path);
        }

        TfLiteInterpreterOptions* opts = TfLiteInterpreterOptionsCreate();
        if (opts == nullptr) {
            return status_ = Status::bad("TfLiteInterpreterOptionsCreate failed");
        }

        const int threads = spec.cpuThreads > 0 ? spec.cpuThreads : 1;
        TfLiteInterpreterOptionsSetNumThreads(opts, threads);

        std::string delWhy;
        impl->delegate = buildDelegate(ep_, spec.cacheToken, &delWhy,
                                         spec.htpPerfMode);
        if (impl->delegate != nullptr) {
            TfLiteInterpreterOptionsAddDelegate(opts, impl->delegate);
            LOGI("LiteRT delegate attached: %s (threads=%d)", activeName_.c_str(), threads);
        } else if (ep_ != Ep::Cpu) {
            TfLiteInterpreterOptionsDelete(opts);
            TfLiteModelDelete(impl->model);
            return status_ = Status::bad(
                std::string("LiteRT delegate failed to build: ") +
                (delWhy.empty() ? "unknown reason" : delWhy));
        } else {
            LOGI("LiteRT CPU path (no extra delegate), threads=%d", threads);
        }

        impl->interp = TfLiteInterpreterCreate(impl->model, opts);
        TfLiteInterpreterOptionsDelete(opts);
        if (impl->interp == nullptr) {
            return status_ = Status::bad("TfLiteInterpreterCreate returned null");
        }

        if (TfLiteInterpreterAllocateTensors(impl->interp) != kTfLiteOk) {
            return status_ = Status::bad("AllocateTensors failed");
        }

        // ── Input shape & dtype ─────────────────────────────────────────────
        if (TfLiteInterpreterGetInputTensorCount(impl->interp) < 1) {
            return status_ = Status::bad("model has no inputs");
        }
        TfLiteTensor* inTensor = TfLiteInterpreterGetInputTensor(impl->interp, 0);
        if (inTensor == nullptr) {
            return status_ = Status::bad("GetInputTensor returned null");
        }

        impl->inType = TfLiteTensorType(inTensor);
        const int ndim = TfLiteTensorNumDims(inTensor);

        bool nhwc = true;
        int inputW = spec.inputSize > 0 ? spec.inputSize : 0;
        int inputH = inputW;

        if (ndim == 4) {
            const int d0 = TfLiteTensorDim(inTensor, 0);
            const int d1 = TfLiteTensorDim(inTensor, 1);
            const int d2 = TfLiteTensorDim(inTensor, 2);
            const int d3 = TfLiteTensorDim(inTensor, 3);
            if (d3 == 3) {                       // (1, H, W, 3) — NHWC
                nhwc = true;
                if (d1 > 0) inputH = d1;
                if (d2 > 0) inputW = d2;
            } else if (d1 == 3) {                // (1, 3, H, W) — NCHW
                nhwc = false;
                if (d2 > 0) inputH = d2;
                if (d3 > 0) inputW = d3;
            } else {
                nhwc = true;
            }
        } else if (ndim == 3) {
            const int d1 = TfLiteTensorDim(inTensor, 1);
            const int d2 = TfLiteTensorDim(inTensor, 2);
            nhwc = (d2 == 3);
            if (d1 > 0) inputH = d1;
            if (inputW == 0) inputW = inputH;
        }

        if (inputW == 0) inputW = spec.inputSize > 0 ? spec.inputSize : 640;
        if (inputH == 0) inputH = inputW;

        impl->inputW = inputW;
        impl->inputH = inputH;
        impl->inputNhwc = nhwc;
        impl->pre = preprocessFor(std::max(inputW, inputH));
        impl->pre.targetW = inputW;
        impl->pre.targetH = inputH;
        impl->pre.nchw = !nhwc;

        // ── Output shape & dtype ────────────────────────────────────────────
        // Single-head: [1, 4+nc, N] or [1, N, 4+nc].
        // Split-head:  [1, nc, N] + [1, 4, N] (either order, e.g.
        // best_int8.tflite emits scores [1,2,1344] as output 0 and boxes
        // [1,4,1344] as output 1). Identify by the 4-channel boxes tensor;
        // never assume output 0 is the concatenated head.
        const int outCount = TfLiteInterpreterGetOutputTensorCount(impl->interp);
        if (outCount < 1) {
            return status_ = Status::bad("model has no outputs");
        }
        auto channelsAnchorsOf = [](const TfLiteTensor* t, int& ch, int& an,
                                    bool& cf) -> bool {
            if (t == nullptr) return false;
            const int nd = TfLiteTensorNumDims(t);
            int64_t x = 0, y = 0;
            if (nd >= 3) {
                x = TfLiteTensorDim(t, nd - 2);
                y = TfLiteTensorDim(t, nd - 1);
            } else if (nd == 2) {
                x = TfLiteTensorDim(t, 0);
                y = TfLiteTensorDim(t, 1);
            } else {
                return false;
            }
            if (x <= 0 || y <= 0) return false;
            if (x < y) { cf = true;  ch = static_cast<int>(x); an = static_cast<int>(y); }
            else       { cf = false; ch = static_cast<int>(y); an = static_cast<int>(x); }
            return true;
        };
        if (outCount == 1) {
            const TfLiteTensor* outTensor = TfLiteInterpreterGetOutputTensor(impl->interp, 0);
            if (outTensor == nullptr) {
                return status_ = Status::bad("GetOutputTensor returned null");
            }
            impl->outType = TfLiteTensorType(outTensor);
            int ch = 0, an = 0;
            if (!channelsAnchorsOf(outTensor, ch, an, impl->channelsFirst)) {
                return status_ = Status::bad("output has too few dims");
            }
            impl->channels = ch;
            impl->anchors  = an;
            impl->dualHead = false;
        } else if (outCount == 2) {
            // Split-head: one tensor must carry the 4 box channels.
            const TfLiteTensor* t0 = TfLiteInterpreterGetOutputTensor(impl->interp, 0);
            const TfLiteTensor* t1 = TfLiteInterpreterGetOutputTensor(impl->interp, 1);
            if (t0 == nullptr || t1 == nullptr) {
                return status_ = Status::bad("GetOutputTensor returned null");
            }
            int ch0 = 0, an0 = 0, ch1 = 0, an1 = 0;
            bool cf0 = true, cf1 = true;
            if (!channelsAnchorsOf(t0, ch0, an0, cf0) ||
                !channelsAnchorsOf(t1, ch1, an1, cf1)) {
                return status_ = Status::bad("split-head output has too few dims");
            }
            if (an0 != an1) {
                return status_ = Status::bad(
                    "split-head anchor mismatch: " + std::to_string(an0) +
                    " vs " + std::to_string(an1));
            }
            int bIdx = -1, sIdx = -1;
            if (ch0 == 4 && ch1 != 4)      { bIdx = 0; sIdx = 1; }
            else if (ch1 == 4 && ch0 != 4) { bIdx = 1; sIdx = 0; }
            else {
                return status_ = Status::bad(
                    "split-head outputs not boxes+scores "
                    "(channels " + std::to_string(ch0) + "/" +
                    std::to_string(ch1) + "); only [4]+[nc] is decoded so far");
            }
            impl->dualHead = true;
            impl->boxesOutIdx  = bIdx;
            impl->scoresOutIdx = sIdx;
            impl->boxesType  = TfLiteTensorType(bIdx == 0 ? t0 : t1);
            impl->scoresType = TfLiteTensorType(sIdx == 0 ? t0 : t1);
            impl->outType    = impl->scoresType;
            impl->boxesChannelsFirst  = (bIdx == 0) ? cf0 : cf1;
            impl->scoresChannelsFirst = (sIdx == 0) ? cf0 : cf1;
            const int nc = (sIdx == 0) ? ch0 : ch1;
            impl->anchors  = an0;
            impl->channels = 4 + nc;
            // The merged buffer is consumed channels-first when the heads
            // are; mixed orientations are normalized on interleave.
            impl->channelsFirst = impl->boxesChannelsFirst && impl->scoresChannelsFirst;
            LOGI("split-head detected: boxes=out%d [4,%d] %s scores=out%d [%d,%d] %s",
                 bIdx, an0, tfliteTypeName(impl->boxesType),
                 sIdx, nc, an0, tfliteTypeName(impl->scoresType));
        } else {
            return status_ = Status::bad(
                "model has " + std::to_string(outCount) +
                " outputs; only single-head and boxes+scores split-head are decoded so far");
        }

        impl->post.layout = impl->channelsFirst
                                ? HeadLayout::V8ChannelsFirst
                                : HeadLayout::V8Rows;
        if (spec.classes.size() == impl->channels - 4) {
            impl->post.numClasses = static_cast<int>(spec.classes.size());
        } else if (spec.classes.size() == impl->channels - 5) {
            impl->post.layout = HeadLayout::V5Objectness;
            impl->post.numClasses = static_cast<int>(spec.classes.size());
        } else if (spec.classes.size() > 0) {
            LOGW("declared %zu classes but output channels=%d — trusting channels",
                 spec.classes.size(), impl->channels);
            impl->post.numClasses = std::max(1, impl->channels - 4);
        } else {
            impl->post.numClasses = std::max(1, impl->channels - 4);
        }
        impl->post.confidence = std::clamp(spec.confidence, 0.01f, 0.99f);
        impl->post.iou = std::clamp(spec.iou, 0.05f, 0.95f);
        impl->confidence.store(impl->post.confidence, std::memory_order_relaxed);

        if (impl->anchors <= 0 || impl->post.numClasses <= 0) {
            return status_ = Status::bad("could not read the detection head");
        }

        // ── Scratch ─────────────────────────────────────────────────────────
        // int8/uint8 tensors need 1 byte/channel, NOT 4. Allocating the buffer
        // at the float (4×) size makes TfLiteTensorCopyFromBuffer reject the
        // copy (byte count mismatch) and silently fail inference.
        const size_t needF = preprocessBufferBytes(impl->pre, sizeof(float));
        const size_t needU = preprocessBufferBytes(impl->pre, 1);
        impl->inputF32.assign(needF / sizeof(float), 0.0f);
        impl->inputU8.assign(needU, 0);
        impl->decoded.assign(
            static_cast<size_t>(impl->anchors) * static_cast<size_t>(impl->channels),
            0.0f);

        {
            char buf[96];
            snprintf(buf, sizeof(buf), "[1, %d, %d, %d] %s",
                     inputH, inputW, 3, nhwc ? "NHWC" : "NCHW");
            impl->inShapeText = buf;
        }
        {
            char buf[160];
            if (impl->dualHead) {
                snprintf(buf, sizeof(buf), "[1, 4, %d] %s + [1, %d, %d] %s (split)",
                         impl->anchors, tfliteTypeName(impl->boxesType),
                         impl->channels - 4, impl->anchors,
                         tfliteTypeName(impl->scoresType));
            } else {
                snprintf(buf, sizeof(buf), "[1, %lld, %lld] %s",
                         impl->channelsFirst ? (long long)impl->channels
                                             : (long long)impl->anchors,
                         impl->channelsFirst ? (long long)impl->anchors
                                             : (long long)impl->channels,
                         tfliteTypeName(impl->outType));
            }
            impl->outShapeText = buf;
        }

        impl_ = std::move(impl);

        const float loadMs = msSince(t0);
        LOGI("loaded %s  in=%s %s out=%s  classes=%d anchors=%d %s  %.0f ms",
             spec.path.c_str(), impl_->inShapeText.c_str(),
             tfliteTypeName(impl_->inType), impl_->outShapeText.c_str(),
             impl_->post.numClasses, impl_->anchors,
             impl_->channelsFirst ? "channels-first" : "rows", loadMs);

        status_ = Status::good();
        timing_ = Timing{};
    } catch (const std::exception& e) {
        return status_ = Status::bad(std::string("LiteRT load failed: ") + e.what());
    } catch (...) {
        return status_ = Status::bad("LiteRT load failed: unknown exception");
    }
    return status_;
}

// ── Inference ───────────────────────────────────────────────────────────────

// Layout-aware dequant: tensor element (anchor a, channel c) -> float,
// honoring per-channel scales with the tensor's own orientation.
static bool dequantHeadTensor(const TfLiteTensor* t, int channels, int anchors,
                              bool cf, std::vector<float>& out) {
    if (t == nullptr || channels <= 0 || anchors <= 0) return false;
    const size_t elems = static_cast<size_t>(channels) * static_cast<size_t>(anchors);
    if (out.size() != elems) out.assign(elems, 0.0f);
    void* raw = const_cast<void*>(TfLiteTensorData(t));
    if (raw == nullptr) return false;
    const TfLiteType ty = TfLiteTensorType(t);
    const TfLiteAffineQuantization* aff = nullptr;
    if (t->quantization.type == kTfLiteAffineQuantization) {
        aff = static_cast<const TfLiteAffineQuantization*>(t->quantization.params);
    }
    const bool perChannel = aff != nullptr && aff->scale != nullptr && aff->scale->size > 1;
    auto srcIdx = [channels, anchors, cf](int a, int c) -> size_t {
        return cf ? static_cast<size_t>(c) * static_cast<size_t>(anchors) + static_cast<size_t>(a)
                  : static_cast<size_t>(a) * static_cast<size_t>(channels) + static_cast<size_t>(c);
    };
    switch (ty) {
        case kTfLiteFloat32: {
            const float* d = static_cast<const float*>(raw);
            for (int a = 0; a < anchors; ++a)
                for (int c = 0; c < channels; ++c) out[srcIdx(a, c)] = d[srcIdx(a, c)];
            return true;
        }
        case kTfLiteInt8: {
            const int8_t* d = static_cast<const int8_t*>(raw);
            if (perChannel) {
                const float* sc = aff->scale->data;
                const int32_t* zp = (aff->zero_point && aff->zero_point->size > 0)
                                        ? aff->zero_point->data : nullptr;
                for (int a = 0; a < anchors; ++a)
                    for (int c = 0; c < channels; ++c) {
                        const size_t i = srcIdx(a, c);
                        out[i] = (static_cast<float>(d[i]) -
                                  static_cast<float>(zp ? zp[c] : 0)) * sc[c];
                    }
            } else {
                TfLiteQuantizationParams qp = TfLiteTensorQuantizationParams(t);
                for (size_t i = 0; i < elems; ++i)
                    out[i] = (static_cast<float>(d[i]) -
                              static_cast<float>(qp.zero_point)) * qp.scale;
            }
            return true;
        }
        case kTfLiteUInt8: {
            const uint8_t* d = static_cast<const uint8_t*>(raw);
            if (perChannel) {
                const float* sc = aff->scale->data;
                const int32_t* zp = (aff->zero_point && aff->zero_point->size > 0)
                                        ? aff->zero_point->data : nullptr;
                for (int a = 0; a < anchors; ++a)
                    for (int c = 0; c < channels; ++c) {
                        const size_t i = srcIdx(a, c);
                        out[i] = (static_cast<float>(d[i]) -
                                  static_cast<float>(zp ? zp[c] : 0)) * sc[c];
                    }
            } else {
                TfLiteQuantizationParams qp = TfLiteTensorQuantizationParams(t);
                for (size_t i = 0; i < elems; ++i)
                    out[i] = (static_cast<float>(d[i]) -
                              static_cast<float>(qp.zero_point)) * qp.scale;
            }
            return true;
        }
        case kTfLiteFloat16: {
            const uint16_t* d = static_cast<const uint16_t*>(raw);
            for (size_t i = 0; i < elems; ++i) {
                const uint32_t sign     = (d[i] & 0x8000) << 16;
                const uint32_t exponent = (d[i] & 0x7C00) >> 10;
                uint32_t mantissa = (d[i] & 0x03FF) << 13;
                uint32_t bits = sign;
                if (exponent == 0) {
                    if (mantissa != 0) {
                        while ((mantissa & 0x00800000) == 0) mantissa <<= 1;
                        mantissa &= 0x007FFFFF;
                        bits |= ((exponent - 14 + 127) << 23) | mantissa;
                    }
                } else if (exponent == 0x1F) {
                    bits |= 0x7F800000 | mantissa;
                } else {
                    bits |= ((exponent - 15 + 127) << 23) | mantissa;
                }
                float v;
                std::memcpy(&v, &bits, sizeof(v));
                out[i] = v;
            }
            return true;
        }
        default:
            return false;
    }
}

bool LiteRtEngine::readOutputInto(Impl* m, std::vector<float>& dst) {
    if (m->dualHead) {
        const TfLiteTensor* bT = TfLiteInterpreterGetOutputTensor(m->interp, m->boxesOutIdx);
        const TfLiteTensor* sT = TfLiteInterpreterGetOutputTensor(m->interp, m->scoresOutIdx);
        if (bT == nullptr || sT == nullptr) return false;
        const int A = m->anchors;
        const int C = m->channels;          // 4 + nc
        const int nc = C - 4;
        if (A <= 0 || nc <= 0) return false;
        const size_t elems = static_cast<size_t>(A) * static_cast<size_t>(C);
        if (dst.size() != elems) dst.assign(elems, 0.0f);
        std::vector<float> bF, sF;
        if (!dequantHeadTensor(bT, 4, A, m->boxesChannelsFirst, bF)) return false;
        if (!dequantHeadTensor(sT, nc, A, m->scoresChannelsFirst, sF)) return false;
        auto bAt = [&](int a, int k) -> float {
            return m->boxesChannelsFirst
                       ? bF[static_cast<size_t>(k) * static_cast<size_t>(A) + static_cast<size_t>(a)]
                       : bF[static_cast<size_t>(a) * 4 + static_cast<size_t>(k)];
        };
        auto sAt = [&](int a, int c) -> float {
            return m->scoresChannelsFirst
                       ? sF[static_cast<size_t>(c) * static_cast<size_t>(A) + static_cast<size_t>(a)]
                       : sF[static_cast<size_t>(a) * static_cast<size_t>(nc) + static_cast<size_t>(c)];
        };
        if (m->channelsFirst) {
            for (int a = 0; a < A; ++a) {
                for (int k = 0; k < 4; ++k)
                    dst[static_cast<size_t>(k) * static_cast<size_t>(A) + static_cast<size_t>(a)] = bAt(a, k);
                for (int c = 0; c < nc; ++c)
                    dst[static_cast<size_t>(4 + c) * static_cast<size_t>(A) + static_cast<size_t>(a)] = sAt(a, c);
            }
        } else {
            for (int a = 0; a < A; ++a) {
                for (int k = 0; k < 4; ++k)
                    dst[static_cast<size_t>(a) * static_cast<size_t>(C) + static_cast<size_t>(k)] = bAt(a, k);
                for (int c = 0; c < nc; ++c)
                    dst[static_cast<size_t>(a) * static_cast<size_t>(C) + static_cast<size_t>(4 + c)] = sAt(a, c);
            }
        }
        return true;
    }
    const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(m->interp, 0);
    if (out == nullptr) return false;

    const size_t elems = static_cast<size_t>(m->anchors) * static_cast<size_t>(m->channels);
    if (dst.size() != elems) dst.assign(elems, 0.0f);

    void* raw = const_cast<void*>(TfLiteTensorData(out));
    if (raw == nullptr) return false;

    // Per-channel (affine) output quantization is common for detection-head
    // tensors. TfLiteTensorQuantizationParams() only returns a single per-tensor
    // (channel-0) scale, so dequantizing every channel with it collapses all
    // channels onto channel 0's dynamic range — coordinates pin to the origin
    // and class scores collapse toward zero. When the tensor is per-channel we
    // must dequantize each channel with its own scale/zero_point.
    const TfLiteAffineQuantization* aff = nullptr;
    if (out->quantization.type == kTfLiteAffineQuantization) {
        aff = static_cast<const TfLiteAffineQuantization*>(out->quantization.params);
    }
    const bool perChannel = aff != nullptr && aff->scale != nullptr && aff->scale->size > 1;
    if (perChannel && m->loggedRuns < 2) {
        std::string scales;
        for (int c = 0; c < aff->scale->size && c < 16; ++c) {
            scales += std::to_string(aff->scale->data[c]) + " ";
        }
        LOGI("readOutputInto: PER-CHANNEL output quant n=%d qd=%d scales=[%s]",
             aff->scale->size, aff->quantized_dimension, scales.c_str());
    }
    // Linear element -> channel index, matching the storage layout.
    auto channelOf = [m](size_t i) -> int {
        return m->channelsFirst ? static_cast<int>(i / static_cast<size_t>(m->anchors))
                                : static_cast<int>(i % static_cast<size_t>(m->channels));
    };

    switch (m->outType) {
        case kTfLiteFloat32: {
            const float* data = static_cast<const float*>(raw);
            std::memcpy(dst.data(), data, elems * sizeof(float));
            return true;
        }
        case kTfLiteInt8: {
            const int8_t* data = static_cast<const int8_t*>(raw);
            if (perChannel) {
                const float*  sc = aff->scale->data;
                const int32_t* zp = (aff->zero_point && aff->zero_point->size > 0)
                                         ? aff->zero_point->data : nullptr;
                for (size_t i = 0; i < elems; ++i) {
                    const int ch = channelOf(i);
                    dst[i] = (static_cast<float>(data[i]) -
                              static_cast<float>(zp ? zp[ch] : 0)) * sc[ch];
                }
            } else {
                TfLiteQuantizationParams qp = TfLiteTensorQuantizationParams(out);
                const float scale = qp.scale;
                const float zp    = static_cast<float>(qp.zero_point);
                for (size_t i = 0; i < elems; ++i) {
                    dst[i] = (static_cast<float>(data[i]) - zp) * scale;
                }
            }
            return true;
        }
        case kTfLiteUInt8: {
            const uint8_t* data = static_cast<const uint8_t*>(raw);
            if (perChannel) {
                const float*  sc = aff->scale->data;
                const int32_t* zp = (aff->zero_point && aff->zero_point->size > 0)
                                         ? aff->zero_point->data : nullptr;
                for (size_t i = 0; i < elems; ++i) {
                    const int ch = channelOf(i);
                    dst[i] = (static_cast<float>(data[i]) -
                              static_cast<float>(zp ? zp[ch] : 0)) * sc[ch];
                }
            } else {
                TfLiteQuantizationParams qp = TfLiteTensorQuantizationParams(out);
                const float scale = qp.scale;
                const float zp    = static_cast<float>(qp.zero_point);
                for (size_t i = 0; i < elems; ++i) {
                    dst[i] = (static_cast<float>(data[i]) - zp) * scale;
                }
            }
            return true;
        }
        case kTfLiteFloat16: {
            const uint16_t* data = static_cast<const uint16_t*>(raw);
            for (size_t i = 0; i < elems; ++i) {
                const uint32_t sign     = (data[i] & 0x8000) << 16;
                const uint32_t exponent = (data[i] & 0x7C00) >> 10;
                uint32_t mantissa = (data[i] & 0x03FF) << 13;
                uint32_t bits = sign;
                if (exponent == 0) {
                    if (mantissa != 0) {
                        while ((mantissa & 0x00800000) == 0) mantissa <<= 1;
                        mantissa &= 0x007FFFFF;
                        bits |= ((exponent - 14 + 127) << 23) | mantissa;
                    }
                } else if (exponent == 0x1F) {
                    bits |= 0x7F800000 | mantissa;
                } else {
                    bits |= ((exponent - 15 + 127) << 23) | mantissa;
                }
                float v;
                std::memcpy(&v, &bits, sizeof(v));
                dst[i] = v;
            }
            return true;
        }
        default:
            return false;
    }
}

// Quantize raw uint8 [0,255] pixels in-place into the model's integer tensor
// domain using the input tensor's per-tensor scale/zero_point. This is the
// proven path from the reference implementation: q = round(u/255/scale) + zp.
// Without it, float [0,1] pixel values are fed as raw uint8 bytes to an int8
// tensor — producing garbage and zero detections.
static void quantizeInputU8(TfLiteType type, std::vector<uint8_t>& buf,
                            float scale, int zeroPoint) {
    const float invScale = (1.0f / 255.0f) / scale;
    if (type == kTfLiteUInt8) {
        for (size_t i = 0; i < buf.size(); ++i) {
            int v = static_cast<int>(std::lroundf(buf[i] * invScale + zeroPoint));
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            buf[i] = static_cast<uint8_t>(v);
        }
    } else {  // int8
        for (size_t i = 0; i < buf.size(); ++i) {
            int v = static_cast<int>(std::lroundf(buf[i] * invScale + zeroPoint));
            if (v < -128) v = -128;
            else if (v > 127) v = 127;
            buf[i] = static_cast<uint8_t>(static_cast<int8_t>(v));
        }
    }
}

std::vector<Box> LiteRtEngine::run(const FrameView& frame, const Roi& roi) {
    std::vector<Box> none;
    if (!ready()) { LOGW("run: not ready"); status_ = Status::bad("no model loaded"); return none; }
    if (!frame.valid()) { LOGW("run: frame not valid (w=%d h=%d stride=%d px=%p)",
                              frame.width, frame.height, frame.rowStrideBytes, frame.pixels);
                         status_ = Status::bad("frame is not usable"); return none; }

    Impl& m = *impl_;

    try {
        // ── Preprocess ─────────────────────────────────────────────────────
        const auto tPre = Clock::now();
        LetterboxMap map;

        if (m.inType == kTfLiteFloat32 || m.inType == kTfLiteFloat16) {
            map = letterboxRgba(frame, roi, m.pre, m.inputF32.data());
        } else {
            map = letterboxRgbaU8(frame, roi, m.pre, m.inputU8.data());
        }
        timing_.preMs = msSince(tPre);

        // ── Copy into the input tensor ──────────────────────────────────────
        const auto tCopy = Clock::now();
        TfLiteTensor* inTensor = TfLiteInterpreterGetInputTensor(m.interp, 0);
        if (inTensor == nullptr) {
            status_ = Status::bad("GetInputTensor returned null");
            return none;
        }

        const bool okCopy = [&]() {
            switch (m.inType) {
                case kTfLiteFloat32:
                    return TfLiteTensorCopyFromBuffer(inTensor, m.inputF32.data(),
                        m.inputF32.size() * sizeof(float)) == kTfLiteOk;
                case kTfLiteFloat16: {
                    std::vector<uint16_t> h(m.inputF32.size());
                    for (size_t i = 0; i < m.inputF32.size(); ++i) {
                        uint32_t bits;
                        std::memcpy(&bits, &m.inputF32[i], sizeof(bits));
                        const uint32_t sign     = (bits >> 16) & 0x8000;
                        const int32_t exponent  = ((bits >> 23) & 0xFF) - 127 + 15;
                        uint32_t mantissa      = bits & 0x007FFFFF;
                        if (exponent <= 0) {
                            h[i] = static_cast<uint16_t>(sign);
                        } else if (exponent >= 31) {
                            h[i] = static_cast<uint16_t>(sign | 0x7C00);
                        } else {
                            h[i] = static_cast<uint16_t>(sign |
                                                          (exponent << 10) |
                                                          (mantissa >> 13));
                        }
                    }
                    return TfLiteTensorCopyFromBuffer(
                        inTensor, h.data(), h.size() * sizeof(uint16_t)) == kTfLiteOk;
                }
                case kTfLiteInt8:
                case kTfLiteUInt8: {
                    // letterboxRgbaU8 wrote raw uint8 [0,255]; quantize into the
                    // model's integer domain before copying. Honors per-channel
                    // (affine) input quantization too — a single per-tensor scale
                    // would quantize G/B with R's range and feed garbage to HTP.
                    const TfLiteAffineQuantization* iaff =
                        (inTensor->quantization.type == kTfLiteAffineQuantization)
                            ? static_cast<const TfLiteAffineQuantization*>(
                                  inTensor->quantization.params)
                            : nullptr;
                    const bool iPC = iaff && iaff->scale && iaff->scale->size > 1;
                    if (iPC) {
                        const float*  sc = iaff->scale->data;
                        const int32_t* zp = (iaff->zero_point && iaff->zero_point->size > 0)
                                                 ? iaff->zero_point->data : nullptr;
                        const size_t n     = m.inputU8.size();
                        const int    inC   = (m.inputNhwc) ? static_cast<int>(TfLiteTensorDim(inTensor, TfLiteTensorNumDims(inTensor) - 1))
                                                           : static_cast<int>(TfLiteTensorDim(inTensor, 1));
                        const size_t plane = n / static_cast<size_t>(inC > 0 ? inC : 3);
                        for (size_t i = 0; i < n; ++i) {
                            const int ch = m.inputNhwc ? static_cast<int>(i % static_cast<size_t>(inC))
                                                        : static_cast<int>(i / plane);
                            const float invScale = (1.0f / 255.0f) / sc[ch];
                            const float z        = zp ? static_cast<float>(zp[ch]) : 0.0f;
                            int v = static_cast<int>(std::lroundf(static_cast<float>(m.inputU8[i]) * invScale + z));
                            v = (m.inType == kTfLiteUInt8)
                                    ? std::max(0, std::min(255, v))
                                    : std::max(-128, std::min(127, v));
                            m.inputU8[i] = static_cast<uint8_t>(v);
                        }
                        if (m.loggedRuns < 2) {
                            std::string scales;
                            for (int c = 0; c < iaff->scale->size && c < 8; ++c)
                                scales += std::to_string(iaff->scale->data[c]) + " ";
                            LOGI("quantizeInput: PER-CHANNEL input quant n=%d qd=%d scales=[%s]",
                                 iaff->scale->size, iaff->quantized_dimension, scales.c_str());
                        }
                    } else {
                        TfLiteQuantizationParams qp = TfLiteTensorQuantizationParams(inTensor);
                        quantizeInputU8(m.inType, m.inputU8, qp.scale, qp.zero_point);
                    }
                    return TfLiteTensorCopyFromBuffer(inTensor, m.inputU8.data(),
                        m.inputU8.size()) == kTfLiteOk;
                }
                default:
                    return false;
            }
        }();
        if (!okCopy) {
            status_ = Status::bad("TfLiteTensorCopyFromBuffer failed");
            LOGW("%s (inType=%d)", status_.message.c_str(), (int)m.inType);
            return none;
        }

        // ── Infer ──────────────────────────────────────────────────────────
        const auto tInfer = Clock::now();
        if (TfLiteInterpreterInvoke(m.interp) != kTfLiteOk) {
            status_ = Status::bad("TfLiteInterpreterInvoke failed");
            LOGW("%s", status_.message.c_str());
            return none;
        }
        timing_.inferMs = msSince(tInfer);

        // ── Postprocess ────────────────────────────────────────────────────
        const auto tPost = Clock::now();
        std::vector<Box> boxes;
        if (!readOutputInto(&m, m.decoded)) {
            LOGW("run: readOutputInto failed (outType=%d anchors=%d channels=%d)",
                 (int)m.outType, m.anchors, m.channels);
            status_ = Status::bad("unsupported output dtype");
            return none;
        }

        OutputTensor t;
        t.data          = m.decoded.data();
        t.rows          = m.anchors;
        t.cols          = m.channels;
        t.channelsFirst = m.channelsFirst;

        PostprocessConfig cfg = m.post;
        cfg.confidence = m.confidence.load(std::memory_order_relaxed);
        cfg.inputW     = m.pre.targetW;
        cfg.inputH     = m.pre.targetH;
        boxes = decode(t, cfg, map, frame);
        timing_.postMs = msSince(tPost);

        if (m.loggedRuns < 3) {
            ++m.loggedRuns;
            float best = 0.0f;
            for (const Box& b : boxes) best = std::max(best, b.score);
            LOGI("run %d: %dx%d stride=%d roi=(%d,%d %dx%d) %s->%s -> %zu boxes "
                 "(best %.3f), pre=%.2f infer=%.2f post=%.2f ms",
                 m.loggedRuns, frame.width, frame.height, frame.rowStrideBytes,
                 roi.x, roi.y, roi.w, roi.h,
                 tfliteTypeName(m.inType), tfliteTypeName(m.outType),
                 boxes.size(), best,
                 static_cast<double>(timing_.preMs),
                 static_cast<double>(timing_.inferMs),
                 static_cast<double>(timing_.postMs));
        }

        status_ = Status::good();
        return boxes;
    } catch (const std::exception& e) {
        status_ = Status::bad(std::string("inference failed: ") + e.what());
        LOGW("%s", status_.message.c_str());
        return none;
    } catch (...) {
        status_ = Status::bad("inference failed: unknown exception");
        return none;
    }
}

std::string LiteRtEngine::inputDtypeName() const {
    if (impl_ == nullptr) return "";
    return tfliteTypeName(impl_->inType);
}

std::string LiteRtEngine::describe() const {
    if (impl_ == nullptr) return std::string(activeName_) + " (not loaded)";
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s  in=%s %s out=%s  classes=%d anchors=%d %s",
             activeName_.c_str(),
             impl_->inShapeText.c_str(), tfliteTypeName(impl_->inType),
             impl_->outShapeText.c_str(),
             impl_->post.numClasses, impl_->anchors,
             impl_->channelsFirst ? "channels-first" : "rows");
    return buf;
}

}  // namespace infer
}  // namespace aimbotng
