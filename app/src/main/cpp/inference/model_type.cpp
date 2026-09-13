// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer::detectModelType — implementation
//
//  Two probes live here, one per file kind:
//
//  * .onnx — a real ORT session is created just to read the graph's declared
//    input dtype. The session is destroyed before this returns. Reusing the
//    process-wide Env is what lets a probe cost one dlopen per process rather
//    than per call.
//
//  * .tflite — the C API can read the input tensor's dtype straight after
//    AllocateTensors, no ORT needed. The TFLite runtime is linked in
//    statically from libtensorflowlite_jni.so (see CMakeLists.txt), so the
//    same direct calls work without any dlopen dance.
// ─────────────────────────────────────────────────────────────────────────────
#define ORT_API_MANUAL_INIT
#include "onnxruntime_cxx_api.h"

#include "tensorflow/lite/c/c_api.h"
#include "tensorflow/lite/c/common.h"

#include "inference/model_type.h"
#include "inference/model_store.h"

#include <android/log.h>

#include <cstdio>
#include <string>

#define TAG "AimbotInfer"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace {

// ORT_API_MANUAL_INIT (defined at the top of this file) suppresses the C++ API
// table's automatic static initialisation: the wrapper reaches the C API through
// a process-global function-pointer table that stays NULL until the inference
// engine's ensureOrtApiReady() dlopens libonnxruntime.so and calls
// Ort::InitApi(g_ortApi). That happens inside the app, but this probe also runs
// inside the shell daemon, where it is the FIRST Ort:: call in the process — so
// each detect* entry below must ask ensureOrtApiReady() to initialise the table
// first. If the library cannot be loaded it returns false and we bail out
// WITHOUT constructing Ort::Env, which would otherwise dereference the still-null
// table and SIGSEGV (fault addr 0x18 in Ort::Env::Env — tombstone_25, before the
// daemon ever published its port).

const char* onnxTypeShort(int t) {
    switch (t) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:    return "fp32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:  return "fp16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16: return "bf16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:     return "int8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:    return "uint8";
        default:                                     return "?";
    }
}

std::string detectOnnxType(const std::string& path) {
    if (!aimbotng::infer::ensureOrtApiReady()) return "";
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "probe");
    try {
        Ort::SessionOptions so;
        Ort::Session sess(env, path.c_str(), so);
        Ort::TypeInfo ti = sess.GetInputTypeInfo(0);
        auto tinfo = ti.GetTensorTypeAndShapeInfo();
        return onnxTypeShort(static_cast<int>(tinfo.GetElementType()));
    } catch (const std::exception& e) {
        LOGW("detectOnnxType(%s) failed: %s", path.c_str(), e.what());
        return "";
    } catch (...) {
        return "";
    }
}

const char* tfliteTypeShort(TfLiteType t) {
    switch (t) {
        case kTfLiteFloat32: return "fp32";
        case kTfLiteFloat16: return "fp16";
        case kTfLiteInt8:    return "int8";
        case kTfLiteUInt8:   return "uint8";
        case kTfLiteInt16:   return "int16";
        case kTfLiteInt32:   return "int32";
        case kTfLiteBool:    return "bool";
        default:             return "?";
    }
}

std::string detectTfliteType(const std::string& path) {
    TfLiteModel* model = TfLiteModelCreateFromFile(path.c_str());
    if (model == nullptr) {
        LOGW("detectTfliteType(%s): cannot read model file", path.c_str());
        return "";
    }

    TfLiteInterpreterOptions* opts = TfLiteInterpreterOptionsCreate();
    if (opts == nullptr) {
        TfLiteModelDelete(model);
        return "";
    }
    TfLiteInterpreterOptionsSetNumThreads(opts, 1);

    TfLiteInterpreter* interp = TfLiteInterpreterCreate(model, opts);
    TfLiteInterpreterOptionsDelete(opts);

    std::string result;
    if (interp != nullptr && TfLiteInterpreterAllocateTensors(interp) == kTfLiteOk) {
        if (TfLiteInterpreterGetInputTensorCount(interp) > 0) {
            const TfLiteTensor* in = TfLiteInterpreterGetInputTensor(interp, 0);
            if (in != nullptr) result = tfliteTypeShort(TfLiteTensorType(in));
        }
    } else {
        LOGW("detectTfliteType(%s): interpreter init failed", path.c_str());
    }

    if (interp != nullptr) TfLiteInterpreterDelete(interp);
    TfLiteModelDelete(model);
    return result;
}

bool detectOnnxInputSize(const std::string& path, int& w, int& h) {
    if (!aimbotng::infer::ensureOrtApiReady()) return false;
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "probe");
    try {
        Ort::SessionOptions so;
        Ort::Session sess(env, path.c_str(), so);
        Ort::TypeInfo ti = sess.GetInputTypeInfo(0);
        auto tinfo = ti.GetTensorTypeAndShapeInfo();
        const auto shape = tinfo.GetShape();
        // Need at least 4 dims (N, C, H, W) and a statically-known size.
        if (shape.size() < 4) return false;
        for (int64_t d : shape) {
            if (d <= 0) return false;   // dynamic (-1) dimension -> not known
        }
        // ONNX detection models are NCHW: [N, C, H, W] -> dims[2]=H, dims[3]=W.
        h = static_cast<int>(shape[2]);
        w = static_cast<int>(shape[3]);
        return (w > 0 && h > 0);
    } catch (const std::exception& e) {
        LOGW("detectOnnxInputSize(%s) failed: %s", path.c_str(), e.what());
        return false;
    } catch (...) {
        return false;
    }
}

bool detectTfliteShape(const std::string& path, int& w, int& h, int& classes);

bool detectTfliteInputSize(const std::string& path, int& w, int& h) {
    int classes = 0;
    return detectTfliteShape(path, w, h, classes);
}

/** YOLO-style class count from an output tensor: channels minus the 4 box
 *  fields. The output is [1, C, A] (channels-first) or [1, A, C]; the channel
 *  axis is the *smaller* of the two feature dims, so we can tell the two
 *  orientations apart without knowing the model family. For split-head
 *  exports ([1,4,A] boxes + [1,nc,A] scores) the caller merges outputs, so
 *  this helper stays single-tensor; see detectTfliteShape for the merge. */
int tfliteOutputClasses(const TfLiteTensor* out) {
    if (out == nullptr || out->dims == nullptr || out->dims->size < 3) return 0;
    const int d1 = out->dims->data[1];
    const int d2 = out->dims->data[2];
    const int ch = (d1 < d2) ? d1 : d2;
    return (ch > 4) ? (ch - 4) : 0;
}

bool detectTfliteShape(const std::string& path, int& w, int& h, int& classes) {
    classes = 0;
    TfLiteModel* model = TfLiteModelCreateFromFile(path.c_str());
    if (model == nullptr) {
        LOGW("detectTfliteShape(%s): cannot read model file", path.c_str());
        return false;
    }

    TfLiteInterpreterOptions* opts = TfLiteInterpreterOptionsCreate();
    if (opts == nullptr) {
        TfLiteModelDelete(model);
        return false;
    }
    TfLiteInterpreterOptionsSetNumThreads(opts, 1);

    TfLiteInterpreter* interp = TfLiteInterpreterCreate(model, opts);
    TfLiteInterpreterOptionsDelete(opts);

    bool ok = false;
    if (interp != nullptr && TfLiteInterpreterAllocateTensors(interp) == kTfLiteOk) {
        if (TfLiteInterpreterGetInputTensorCount(interp) > 0) {
            const TfLiteTensor* in = TfLiteInterpreterGetInputTensor(interp, 0);
            if (in != nullptr && in->dims != nullptr && in->dims->size >= 4) {
                // NHWC: [1, H, W, C] -> dims[1]=H, dims[2]=W.
                h = in->dims->data[1];
                w = in->dims->data[2];
            }
        }
        if (TfLiteInterpreterGetOutputTensorCount(interp) == 1) {
            classes = tfliteOutputClasses(TfLiteInterpreterGetOutputTensor(interp, 0));
        } else if (TfLiteInterpreterGetOutputTensorCount(interp) == 2) {
            // Split-head: [1,4,A] boxes + [1,nc,A] scores (either order).
            // The scores tensor's channel axis IS the class count; the old
            // code only looked at output 0 and reported 0 for best_int8.
            const TfLiteTensor* o0 = TfLiteInterpreterGetOutputTensor(interp, 0);
            const TfLiteTensor* o1 = TfLiteInterpreterGetOutputTensor(interp, 1);
            auto chOf = [](const TfLiteTensor* t) -> int {
                if (t == nullptr || t->dims == nullptr || t->dims->size < 3) return 0;
                const int d1 = t->dims->data[1];
                const int d2 = t->dims->data[2];
                return (d1 < d2) ? d1 : d2;
            };
            const int c0 = chOf(o0), c1 = chOf(o1);
            if (c0 == 4 && c1 != 4)      classes = c1;
            else if (c1 == 4 && c0 != 4) classes = c0;
            else classes = tfliteOutputClasses(o0);
        }
        ok = (w > 0 && h > 0);
    } else {
        LOGW("detectTfliteShape(%s): interpreter init failed", path.c_str());
    }

    if (interp != nullptr) TfLiteInterpreterDelete(interp);
    TfLiteModelDelete(model);
    return ok;
}

int onnxOutputClasses(Ort::Session& sess) {
    try {
        const size_t nout = sess.GetOutputCount();
        if (nout == 0) return 0;
        Ort::TypeInfo ti = sess.GetOutputTypeInfo(0);
        auto shape = ti.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() < 3) return 0;
        const int64_t d1 = shape[1];
        const int64_t d2 = shape[2];
        const int64_t ch = (d1 < d2) ? d1 : d2;
        return (ch > 4) ? static_cast<int>(ch - 4) : 0;
    } catch (...) {
        return 0;
    }
}

bool detectOnnxShape(const std::string& path, int& w, int& h, int& classes) {
    classes = 0;
    if (!aimbotng::infer::ensureOrtApiReady()) return false;
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "probe");
    try {
        Ort::SessionOptions so;
        Ort::Session sess(env, path.c_str(), so);
        Ort::TypeInfo ti = sess.GetInputTypeInfo(0);
        auto shape = ti.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() < 4) return false;
        for (int64_t d : shape) {
            if (d <= 0) return false;   // dynamic (-1) dimension -> not known
        }
        // NCHW: [N, C, H, W] -> dims[2]=H, dims[3]=W.
        h = static_cast<int>(shape[2]);
        w = static_cast<int>(shape[3]);
        classes = onnxOutputClasses(sess);
        return (w > 0 && h > 0);
    } catch (const std::exception& e) {
        LOGW("detectOnnxShape(%s) failed: %s", path.c_str(), e.what());
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace

namespace aimbotng {
namespace infer {

std::string detectModelType(const std::string& path) {
    switch (model::kindOfPath(path)) {
        case model::Kind::Onnx:   return detectOnnxType(path);
        case model::Kind::Tflite: return detectTfliteType(path);
        default:                  return "";
    }
}

bool detectModelInputSize(const std::string& path, int& w, int& h) {
    switch (model::kindOfPath(path)) {
        case model::Kind::Onnx:   return detectOnnxInputSize(path, w, h);
        case model::Kind::Tflite: return detectTfliteInputSize(path, w, h);
        default:                  return false;
    }
}

bool detectModelShape(const std::string& path, int& w, int& h, int& classes) {
    switch (model::kindOfPath(path)) {
        case model::Kind::Onnx:   return detectOnnxShape(path, w, h, classes);
        case model::Kind::Tflite: return detectTfliteShape(path, w, h, classes);
        default:                  return false;
    }
}

}  // namespace infer
}  // namespace aimbotng
