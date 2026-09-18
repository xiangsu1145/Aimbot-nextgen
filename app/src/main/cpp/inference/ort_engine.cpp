// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of ort_engine.h
//
//  Two things here are worth reading before changing anything:
//
//  * **The library is dlopen'ed, not linked.** The daemon is `app_process`, so
//    the linker resolves a library's DT_NEEDED against /system/bin and the
//    platform paths — neither of which has libonnxruntime.so. Option 1 was to
//    link it and add DT_RUNPATH=$ORIGIN; option 2, taken here, is to load it by
//    absolute path. Option 2 was chosen because it also degrades properly: a
//    missing or stripped library becomes "backend unavailable" in a menu instead
//    of a link failure that stops the whole daemon from starting.
//
//  * **ORT_API_MANUAL_INIT.** The C++ wrapper normally reaches the C API through
//    the extern symbol OrtGetApiBase(), which we do not have at link time. The
//    header provides exactly this escape hatch: with the macro defined, the
//    wrapper stops trying to find the symbol itself and waits for InitApi().
//    The macro must be defined identically in every translation unit that
//    includes the C++ header, which is why the ORT headers are confined to this
//    file and ort_engine.h exposes only a pimpl.
// ─────────────────────────────────────────────────────────────────────────────

#define ORT_API_MANUAL_INIT
#include "onnxruntime_cxx_api.h"
#include "onnxruntime_session_options_config_keys.h"

#include "inference/libpath.h"
#include "inference/ort_engine.h"

#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

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

/// Short label for an ONNX tensor element type, for the model list and probes.
std::string dtypeShort(ONNXTensorElementDataType t) {
    switch (t) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:    return "fp32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:  return "fp16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16: return "bf16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:     return "int8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:    return "uint8";
        default:                                     return "?";
    }
}

using GetApiBaseFn = const OrtApiBase* (*)();

/// The C API pointer, once per process. OnnxRuntime has no per-session global
/// state to speak of, and calling GetApi() repeatedly would be harmless, but
/// InitApi() is documented as once-only and the discovery costs a dlopen.
std::once_flag          g_ortOnce;
const OrtApi*           g_ortApi = nullptr;
std::string             g_ortError;

const OrtApi* ensureOrt() {
    std::call_once(g_ortOnce, [] {
        std::string why;
        void* lib = preload("libonnxruntime.so", &why);
        if (lib == nullptr) {
            g_ortError = "libonnxruntime.so could not be loaded: " + why;
            return;
        }
        auto getApiBase = reinterpret_cast<GetApiBaseFn>(dlsym(lib, "OrtGetApiBase"));
        if (getApiBase == nullptr) {
            g_ortError = "libonnxruntime.so has no OrtGetApiBase — wrong library?";
            return;
        }
        const OrtApiBase* base = getApiBase();
        if (base == nullptr || base->GetApi == nullptr) {
            g_ortError = "OrtGetApiBase() returned nothing usable";
            return;
        }
        g_ortApi = base->GetApi(ORT_API_VERSION);
        if (g_ortApi == nullptr) {
            g_ortError = "this build of libonnxruntime.so does not expose API version " +
                         std::to_string(ORT_API_VERSION) +
                         " — the vendored headers and the runtime disagree";
            return;
        }
        // Hands the wrapper the pointer it would otherwise have looked up.
        Ort::InitApi(g_ortApi);
        LOGI("ONNX Runtime %s initialised (api %d)",
             base->GetVersionString ? base->GetVersionString() : "?", ORT_API_VERSION);
    });
    return g_ortApi;
}

}  // namespace

// ── Public API-table initialiser (see ort_engine.h) ──────────────────────────
bool ensureOrtApiReady() {
    return ensureOrt() != nullptr;
}

// ── Impl ────────────────────────────────────────────────────────────────────
//   Held behind a pimpl so the ORT headers — and the ORT_API_MANUAL_INIT
//   definition they depend on — stay inside this translation unit.

struct OrtEngine::Impl {
    Ort::Env     env{ORT_LOGGING_LEVEL_WARNING, "aimbotng"};
    Ort::Session session{nullptr};

    // Names have to outlive the session and be stable: Run() takes an array of
    // raw pointers, so a vector<string> that reallocates under it is a crash.
    std::vector<std::string> inNames, outNames;
    std::vector<const char*> inNamePtrs, outNamePtrs;

    PreprocessConfig pre;
    PostprocessConfig post;
    std::vector<float> input;          // targetW * targetH * 3 floats

    /// Element types the graph actually declares.
    ///
    /// Not assumed to be float32. A model exported with `--half` — which is what
    /// you get from most YOLO export scripts aimed at an NPU, and what the device
    /// this was developed against is fed — declares float16 tensors, and handing
    /// Ort a float32 buffer for one fails the whole run with "Unexpected input
    /// data type", every frame, with no other symptom. Read from the graph and
    /// honoured.
    ONNXTensorElementDataType inType  = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    ONNXTensorElementDataType outType = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;

    std::vector<Ort::Float16_t> inputF16;   // used when inType is float16
    std::vector<float>          outputF32;  // scratch when outType is float16

    /// The live threshold. Atomic because the menu writes it from the render
    /// thread while the session's worker is mid-run reading it — the one piece
    /// of engine state that genuinely crosses threads.
    std::atomic<float> confidence{0.5f};

    /// Frames are only logged a few times: a per-frame line would be its own
    /// problem, and three is enough to see the shapes settle.
    int loggedRuns = 0;

    // Output geometry, resolved once at load.
    int  anchors = 0;
    int  channels = 0;
    bool channelsFirst = false;

    std::string inShapeText, outShapeText;
};

namespace {

std::string shapeText(const std::vector<int64_t>& d) {
    std::string s = "[";
    for (size_t i = 0; i < d.size(); ++i) {
        if (i != 0) s += ", ";
        s += std::to_string(d[i]);
    }
    return s + "]";
}

/// Short name for a tensor element type, for logs and the menu's readout.
const char* dtypeName(ONNXTensorElementDataType t) {
    switch (t) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:   return "f32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return "f16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:  return "f64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:   return "u8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:    return "i8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:   return "i32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:   return "i64";
        default:                                    return "?";
    }
}

/// Works out which of the known YOLO layouts a tensor is, and how many classes
/// it carries.
///
/// This is the one place the code infers structure from numbers, so it is also
/// the one place a wrong guess is silent. Two rules keep it honest:
///
///  * The declared class count wins when the model entry has one. It is the only
///    fact available from outside the graph, and it is the fact a user can fix.
///    The 4 vs 5 offset then follows: v5 inserts an objectness score between the
///    box and the classes.
///  * Without a declared count, assume 4 + nc — the v8/v11 export. That is the
///    common case and the one the Model page's Add dialog produces. The guess is
///    logged with the shape so a wrong box count is traceable to it.
///
/// The decision itself is shared with the TFLite backends (resolveHeadLayout, in
/// postprocess.h) so all four engines answer the v5-vs-v8 question the same way;
/// this function only reads the ONNX shape into the arguments that one takes.
void resolveLayout(const std::vector<int64_t>& dims, size_t declaredClasses,
                   int inputSize, int& anchors, int& channels,
                   bool& channelsFirst, HeadLayout& layout, int& numClasses) {
    channelsFirst = false;
    layout = HeadLayout::V8ChannelsFirst;

    if (dims.size() >= 3) {
        // (1, C, N) or (1, N, C). Whichever axis is smaller is the channel axis:
        // a detection head has a handful of numbers per anchor and thousands of
        // anchors. That holds for every YOLO export seen so far and would only
        // fail for a model with more classes than anchors, which is not a thing.
        const int64_t a = dims[dims.size() - 2];
        const int64_t b = dims[dims.size() - 1];
        if (a < b) {
            channelsFirst = true;
            channels = static_cast<int>(a);
            anchors  = static_cast<int>(b);
        } else {
            channels = static_cast<int>(b);
            anchors  = static_cast<int>(a);
        }
    } else if (dims.size() == 2) {
        anchors  = static_cast<int>(dims[0]);
        channels = static_cast<int>(dims[1]);
    }

    layout = resolveHeadLayout(anchors, channels, channelsFirst, inputSize,
                               static_cast<int>(declaredClasses), numClasses);
}

/// Counts the anchors that clear a threshold under one head interpretation,
/// without NMS or geometry. Used only by the load-time auto-detect to tell a
/// V8 head from a V5/objectness head on a blank frame: a correct detector
/// fires ~0 anchors on a blank frame, while the wrong interpretation fires a
/// flood — that divergence is what betrays which layout is right.
int countHead(const float* data, int anchors, int channels, bool channelsFirst,
              bool v5, int numClasses, float conf) {
    if (data == nullptr || anchors <= 0 || channels <= 0 || numClasses <= 0) return 0;
    const int firstClass = v5 ? 5 : 4;
    const int avail = channels - firstClass;
    if (avail < 1) return 0;
    const int classes = numClasses > avail ? avail : numClasses;
    int n = 0;
    for (int i = 0; i < anchors; ++i) {
        float best = -1.0f;
        for (int c = 0; c < classes; ++c) {
            const float s = channelsFirst
                ? data[static_cast<size_t>(firstClass + c) * static_cast<size_t>(anchors) +
                       static_cast<size_t>(i)]
                : data[static_cast<size_t>(i) * static_cast<size_t>(channels) +
                       static_cast<size_t>(firstClass + c)];
            if (s > best) best = s;
        }
        if (v5) {
            const float obj = channelsFirst
                ? data[4 * static_cast<size_t>(anchors) + static_cast<size_t>(i)]
                : data[static_cast<size_t>(i) * static_cast<size_t>(channels) + 4];
            best *= obj;
        }
        if (best >= conf) ++n;
    }
    return n;
}

}  // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

OrtEngine::OrtEngine(Ep ep) : ep_(ep) {
    activeName_ = (ep == Ep::Xnnpack) ? "ONNX Runtime / XNNPACK" : "ONNX Runtime / CPU";
    status_ = Status::good();
}

OrtEngine::~OrtEngine() { unload(); }

void OrtEngine::unload() {
    impl_.reset();       // destroys the session and the env in that order
    status_ = Status::bad("not loaded");
    timing_ = Timing{};
}

bool OrtEngine::ready() const {
    // No operator bool on Ort::Session in this version, and asking it a question
    // would throw on an empty session. The head geometry is set at the end of a
    // successful load and cleared by unload(), which makes it the honest flag.
    return impl_ != nullptr && impl_->anchors > 0;
}

const char* OrtEngine::activeName() const { return activeName_.c_str(); }

void OrtEngine::setConfidence(float c) {
    // No lock and no reload: the threshold is read once per frame by run(), so
    // changing it is a store. The stored config keeps its load-time value and is
    // not consulted for the threshold, which is what keeps this free of a mutex
    // that the render thread and the worker would both have to touch.
    if (impl_ == nullptr) return;
    impl_->confidence.store(std::clamp(c, 0.01f, 0.99f), std::memory_order_relaxed);
}

Status OrtEngine::load(const ModelSpec& spec) {
    unload();
    const auto t0 = Clock::now();

    if (spec.path.empty()) return status_ = Status::bad("no model path");

    // Everything below can throw: the C++ wrapper turns every OrtStatus into an
    // exception, and a malformed model is a user action, not a programming
    // error. The project builds with exceptions on, so this is the one place
    // they are used — and they all stop here.
    try {
        if (ensureOrt() == nullptr) {
            return status_ = Status::bad(g_ortError.empty() ? "ONNX Runtime unavailable"
                                                            : g_ortError);
        }

        auto impl = std::make_unique<Impl>();

        // ── Session options ────────────────────────────────────────────────
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const int threads = spec.cpuThreads > 0 ? spec.cpuThreads : 0;
        if (threads > 0) {
            so.SetIntraOpNumThreads(threads);
            so.SetInterOpNumThreads(1);
        }

        bool xnnpackActive = false;
        if (ep_ == Ep::Xnnpack) {
            // XNNPACK keeps its own thread pool, and the ORT documentation
            // recommends also setting the ORT intra-op pool to a single thread
            // and disabling spinning, so the two do not fight over the same
            // cores. That advice is conditioned on XNNPACK taking the
            // compute-heavy nodes — and when it does not, the cost is not a few
            // percent: every node falls to the CPU provider, which now has one
            // thread. Measured on a 320 YOLO that was ~50 ms a frame against
            // ~10 ms expected.
            //
            // So the ORT pool is left at its default (one thread per core) and
            // only spinning is turned off, which is the part that is safe either
            // way. The oversubscription when XNNPACK *does* claim the graph
            // costs less than the cliff in the case where it does not.
            so.AddConfigEntry(kOrtSessionOptionsConfigAllowIntraOpSpinning, "0");

            const std::string xnnThreads = std::to_string(
                threads > 0 ? threads : 4);
            try {
                so.AppendExecutionProvider("XNNPACK", {{"intra_op_num_threads", xnnThreads}});
                xnnpackActive = true;
            } catch (const std::exception& e) {
                // Not fatal: the CPU provider is always there, so a build of ORT
                // without XNNPACK still runs the model. Said out loud because
                // activeName() would otherwise keep advertising it.
                LOGW("XNNPACK EP refused (%s) — falling back to the CPU provider", e.what());
                activeName_ = "ONNX Runtime / CPU";
            }
        }

        impl->session = Ort::Session(impl->env, spec.path.c_str(), so);

        // ── Input shape ────────────────────────────────────────────────────
        Ort::AllocatorWithDefaultOptions alloc;
        {
            auto name = impl->session.GetInputNameAllocated(0, alloc);
            impl->inNames.emplace_back(name.get());
        }
        auto inInfo = impl->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        std::vector<int64_t> inDims = inInfo.GetShape();
        impl->inShapeText = shapeText(inDims);
        impl->inType = inInfo.GetElementType();

        // NCHW or NHWC, decided by which axis is 3. A dynamic axis (-1) cannot
        // answer that, so the model entry's inputSize is used and NHWC assumed —
        // which is what the mobile exporters produce.
        int inputW = spec.inputSize > 0 ? spec.inputSize : 640;
        int inputH = inputW;
        bool nchw = false;
        if (inDims.size() == 4) {
            if (inDims[1] == 3) {
                nchw = true;
                if (inDims[2] > 0) inputH = static_cast<int>(inDims[2]);
                if (inDims[3] > 0) inputW = static_cast<int>(inDims[3]);
            } else if (inDims[3] == 3) {
                nchw = false;
                if (inDims[1] > 0) inputH = static_cast<int>(inDims[1]);
                if (inDims[2] > 0) inputW = static_cast<int>(inDims[2]);
            }
        }

        impl->pre = preprocessFor(std::max(inputW, inputH));
        impl->pre.targetW = inputW;
        impl->pre.targetH = inputH;
        impl->pre.nchw = nchw;

        // ── Output shape ───────────────────────────────────────────────────
        for (size_t i = 0; i < impl->session.GetOutputCount(); ++i) {
            auto name = impl->session.GetOutputNameAllocated(i, alloc);
            impl->outNames.emplace_back(name.get());
        }
        auto outInfo = impl->session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
        std::vector<int64_t> outDims = outInfo.GetShape();
        impl->outShapeText = shapeText(outDims);
        impl->outType = outInfo.GetElementType();

        // Refused rather than guessed. Feeding the wrong dtype fails every frame
        // with an ORT message about data types, which is a fine message but the
        // wrong place to read it — and a model whose input is, say, int8 needs a
        // different preprocessing path entirely, not a cast.
        auto supported = [](ONNXTensorElementDataType t) {
            return t == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                   t == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
        };
        if (!supported(impl->inType) || !supported(impl->outType)) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "unsupported tensor types: in=%d out=%d (float32 and float16 only)",
                     static_cast<int>(impl->inType), static_cast<int>(impl->outType));
            return status_ = Status::bad(msg);
        }

        if (impl->session.GetOutputCount() != 1) {
            return status_ = Status::bad(
                "model has " + std::to_string(impl->session.GetOutputCount()) +
                " outputs; only single-head exports are decoded so far");
        }

        resolveLayout(outDims, spec.classes.size(),
                      std::max(impl->pre.targetW, impl->pre.targetH),
                      impl->anchors, impl->channels,
                      impl->channelsFirst, impl->post.layout, impl->post.numClasses);
        impl->post.confidence = std::clamp(spec.confidence, 0.01f, 0.99f);
        impl->post.iou = std::clamp(spec.iou, 0.05f, 0.95f);
        impl->confidence.store(impl->post.confidence, std::memory_order_relaxed);

        if (impl->anchors <= 0 || impl->post.numClasses <= 0) {
            return status_ = Status::bad("could not read the detection head: " +
                                         impl->outShapeText);
        }

        // ── Scratch ────────────────────────────────────────────────────────
        for (std::string& s : impl->inNames)  impl->inNamePtrs.push_back(s.c_str());
        for (std::string& s : impl->outNames) impl->outNamePtrs.push_back(s.c_str());

        // ── Head layout auto-detect (V8 vs V5/objectness) ────────────────────
        // When the model entry declares no class count the shape alone cannot
        // tell a V8 head (4+nb) from a V5 head (5+nb, the extra slot being
        // objectness). They decode to wildly different box counts on the same
        // tensor, so a wrong guess is silent and fatal. The one reliable
        // discriminator is behaviour: a correct detector fires ~0 anchors on a
        // blank frame, the wrong interpretation fires a flood. So we run one
        // throwaway forward on a constant input and pick the layout that fires
        // least. Declared class counts still win — this only runs when the entry
        // is silent, which is exactly the case it cannot be answered otherwise.
        if (spec.classes.empty() && impl->channels >= 5 && impl->anchors > 0) {
            try {
                const int ph = impl->pre.targetH, pw = impl->pre.targetW;
                const size_t pn = static_cast<size_t>(3) * ph * pw;
                std::vector<float> blank(pn, 0.5f);  // mid-gray — a "no object" input
                Ort::MemoryInfo pmem =
                    Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
                Ort::Value pin{nullptr};
                if (impl->inType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
                    std::vector<Ort::Float16_t> hf(pn);
                    for (size_t i = 0; i < pn; ++i) hf[i] = Ort::Float16_t(blank[i]);
                    std::vector<int64_t> pshape = {1, 3, ph, pw};
                    pin = Ort::Value::CreateTensor<Ort::Float16_t>(
                        pmem, hf.data(), pn, pshape.data(), pshape.size());
                } else {
                    std::vector<int64_t> pshape = {1, 3, ph, pw};
                    pin = Ort::Value::CreateTensor<float>(
                        pmem, blank.data(), pn, pshape.data(), pshape.size());
                }
                auto pout = impl->session.Run(Ort::RunOptions{nullptr},
                                              impl->inNamePtrs.data(), &pin, 1,
                                              impl->outNamePtrs.data(), 1);
                const float* pdata = nullptr;
                std::vector<float> pf32;
                // Same runtime-type discipline as run(): ORT may have upcast the
                // fp16 output to fp32, in which case reading it as float16 is wrong.
                const ONNXTensorElementDataType pElem =
                    pout[0].GetTensorTypeAndShapeInfo().GetElementType();
                if (pElem == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
                    const Ort::Float16_t* h = pout[0].GetTensorData<Ort::Float16_t>();
                    pf32.resize(static_cast<size_t>(impl->anchors) *
                                static_cast<size_t>(impl->channels));
                    for (size_t i = 0; i < pf32.size(); ++i) pf32[i] = h[i].ToFloat();
                    pdata = pf32.data();
                } else {
                    pdata = pout[0].GetTensorData<float>();
                }
                const float probeConf = 0.25f;
                const int v8Count = countHead(pdata, impl->anchors, impl->channels,
                                              impl->channelsFirst, false,
                                              impl->channels - 4, probeConf);
                const int v5Count = (impl->channels >= 6)
                    ? countHead(pdata, impl->anchors, impl->channels,
                                impl->channelsFirst, true,
                                impl->channels - 5, probeConf)
                    : (1 << 30);
                // Switch to V5 only when it is clearly the quieter interpretation
                // on a blank frame: V8 floods, V5 is near silent. The two guards
                // keep a legitimately-noisy V8 from being flipped — V5 must be
                // essentially silent (<=2 anchors) while V8 clearly floods.
                if (v5Count != (1 << 30) && v5Count <= 2 &&
                    v8Count > impl->anchors / 50) {
                    impl->post.layout = HeadLayout::V5Objectness;
                    impl->post.numClasses = impl->channels - 5;
                    LOGI("head auto-detect: V5/objectness  (blank v8=%d v5=%d, %d classes)",
                         v8Count, v5Count, impl->post.numClasses);
                } else {
                    LOGI("head auto-detect: V8  (blank v8=%d v5=%d)", v8Count, v5Count);
                }
            } catch (const std::exception& e) {
                LOGW("head auto-detect skipped (%s) — keeping V8 default", e.what());
            }
        }

        const size_t need = preprocessBufferBytes(impl->pre, static_cast<int>(sizeof(float)));
        impl->input.assign(need / sizeof(float), 0.0f);
        if (need == 0) return status_ = Status::bad("zero-sized input");

        // Pre-sized so the per-frame path converts in place instead of resizing.
        if (impl->inType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            impl->inputF16.resize(impl->input.size());
        }

        impl_ = std::move(impl);

        const float loadMs = msSince(t0);
        LOGI("loaded %s  in=%s %s out=%s %s  classes=%d anchors=%d %s  %s  (%.0f ms)",
             spec.path.c_str(), impl_->inShapeText.c_str(),
             dtypeName(impl_->inType), impl_->outShapeText.c_str(),
             dtypeName(impl_->outType), impl_->post.numClasses, impl_->anchors,
             impl_->channelsFirst ? "channels-first" : "rows",
             xnnpackActive ? "XNNPACK" : "CPU", loadMs);

        status_ = Status::good();
        timing_ = Timing{};
    } catch (const Ort::Exception& e) {
        return status_ = Status::bad(std::string("ONNX Runtime: ") + e.what());
    } catch (const std::exception& e) {
        return status_ = Status::bad(e.what());
    }
    return status_;
}

// ── Inference ───────────────────────────────────────────────────────────────

std::vector<Box> OrtEngine::run(const FrameView& frame, const Roi& roi) {
    std::vector<Box> none;
    if (!ready()) {
        status_ = Status::bad("no model loaded");
        return none;
    }
    if (!frame.valid()) {
        status_ = Status::bad("frame is not usable");
        return none;
    }

    Impl& m = *impl_;

    try {
        // ── Preprocess ─────────────────────────────────────────────────────
        const auto tPre = Clock::now();
        const LetterboxMap map = letterboxRgba(frame, roi, m.pre, m.input.data());
        timing_.preMs = msSince(tPre);

        // ── Infer ──────────────────────────────────────────────────────────
        const auto tInfer = Clock::now();
        std::vector<int64_t> shape = {1, 3, m.pre.targetH, m.pre.targetW};
        if (!m.pre.nchw) shape = {1, m.pre.targetH, m.pre.targetW, 3};

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // The buffer type follows the graph. The float32 scratch is what the
        // letterbox writes into either way — one resampling implementation, not
        // two — and the float16 case pays one linear conversion on top, about a
        // tenth of the cost of the resample it follows.
        Ort::Value input{nullptr};
        if (m.inType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            const size_t n = m.input.size();
            for (size_t i = 0; i < n; ++i) {
                m.inputF16[i] = Ort::Float16_t(m.input[i]);
            }
            input = Ort::Value::CreateTensor<Ort::Float16_t>(
                mem, m.inputF16.data(), n, shape.data(), shape.size());
        } else {
            input = Ort::Value::CreateTensor<float>(
                mem, m.input.data(), m.input.size(), shape.data(), shape.size());
        }

        auto outputs = m.session.Run(Ort::RunOptions{nullptr},
                                     m.inNamePtrs.data(), &input, 1,
                                     m.outNamePtrs.data(), 1);
        timing_.inferMs = msSince(tInfer);

        // ── Postprocess ────────────────────────────────────────────────────
        const auto tPost = Clock::now();
        std::vector<Box> boxes;
        if (!outputs.empty() && outputs[0].IsTensor()) {
            const size_t elems = m.anchors * static_cast<size_t>(m.channels);
            const float* data = nullptr;

            // Read by the *runtime* element type, not the type the graph
            // declares. ORT inserts a Cast when an execution provider cannot emit
            // fp16 tensors, so a graph that says float16 frequently comes back
            // from Run() as float32 in memory. Trusting the declared type here
            // would reinterpret those fp32 bytes as fp16 and produce garbage —
            // which shows up as a flood of false detections on an otherwise good
            // model. Asking the tensor what it actually is is cheaper than that.
            const ONNXTensorElementDataType outElem =
                outputs[0].GetTensorTypeAndShapeInfo().GetElementType();
            if (outElem == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
                const Ort::Float16_t* half = outputs[0].GetTensorData<Ort::Float16_t>();
                if (m.outputF32.size() != elems) m.outputF32.resize(elems);
                for (size_t i = 0; i < elems; ++i) m.outputF32[i] = half[i].ToFloat();
                data = m.outputF32.data();
            } else {
                data = outputs[0].GetTensorData<float>();
            }

            OutputTensor t;
            t.data = data;
            t.rows = m.anchors;
            t.cols = m.channels;
            t.channelsFirst = m.channelsFirst;

            // Copied so the live threshold can be applied without the decode
            // reading an atomic on every anchor. The struct is small and its one
            // vector is empty in every configuration this engine produces.
            PostprocessConfig cfg = m.post;
            cfg.confidence = m.confidence.load(std::memory_order_relaxed);
            boxes = decode(t, cfg, map, frame);
        }
        timing_.postMs = msSince(tPost);

        // The first few runs are reported whole: shapes, box count and the stage
        // split. Once, at INFO, because "it runs but the numbers are wrong" is a
        // failure mode with no other symptom, and a per-frame log would be its
        // own problem.
        if (m.loggedRuns < 3) {
            ++m.loggedRuns;
            // The best score is in there because a box count that hits
            // maxDetections exactly means little on its own: every anchor passing
            // the threshold and one confident target look identical in the count,
            // and the score tells them apart.
            float best = 0.0f;
            for (const Box& b : boxes) best = std::max(best, b.score);
            LOGI("run %d: %dx%d stride=%d roi=(%d,%d %dx%d) dtypes %s->%s -> %zu boxes "
                 "(best %.3f), pre=%.2f infer=%.2f post=%.2f ms",
                 m.loggedRuns, frame.width, frame.height, frame.rowStrideBytes,
                 roi.x, roi.y, roi.w, roi.h,
                 dtypeName(m.inType), dtypeName(m.outType), boxes.size(), best,
                 static_cast<double>(timing_.preMs),
                 static_cast<double>(timing_.inferMs),
                 static_cast<double>(timing_.postMs));
        }

        status_ = Status::good();
        return boxes;
    } catch (const Ort::Exception& e) {
        // Logged as well as stored: a detector that fails every frame and says
        // nothing is indistinguishable from one that finds nothing, and only one
        // of those is a bug. The stored message reaches the menu's readout.
        status_ = Status::bad(std::string("inference failed: ") + e.what());
        LOGW("%s", status_.message.c_str());
        return none;
    } catch (const std::exception& e) {
        status_ = Status::bad(std::string("inference failed: ") + e.what());
        LOGW("%s", status_.message.c_str());
        return none;
    }
}

std::string OrtEngine::inputDtypeName() const {
    if (impl_ == nullptr) return "";
    return dtypeShort(impl_->inType);
}

std::string OrtEngine::describe() const {
    if (impl_ == nullptr) return std::string("not loaded (") + status_.cstr() + ")";
    const Impl& m = *impl_;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "%s | in %s %s out %s %s | classes %d, anchors %d, %s | input %dx%d %s",
             activeName_.c_str(), m.inShapeText.c_str(), dtypeName(m.inType),
             m.outShapeText.c_str(), dtypeName(m.outType),
             m.post.numClasses, m.anchors,
             m.channelsFirst ? "channels-first" : "rows",
             m.pre.targetW, m.pre.targetH, m.pre.nchw ? "NCHW" : "NHWC");
    return std::string(buf);
}

}  // namespace infer
}  // namespace aimbotng
