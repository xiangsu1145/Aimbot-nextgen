// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer::box_format — auto-detect box-coordinate format from the
//  model's static graph.
//
//  The engine has to know whether the 4-channel box head of a YOLO-family
//  detector emits (cx,cy,w,h) (raw head, postprocess does dist2bbox) or
//  (x1,y1,x2,y2) (the export baked dist2bbox in). There is no portable
//  cross-backend flag, so we determine this by reading the model file and
//  walking its operator graph — never invoking the runtime.
//
//  Two paths:
//
//   .tflite — subgraph[0].operators walked back from declared outputs;
//             decoder ops (Sub/Add/Mul/BatchMatMul) on the box-channel
//             chain => Xyxy, Conv-like ops (Conv/DWConv/FullyConnected)
//             reached first => Cxcywh.
//
//   .onnx   — Ort::Session::GetModelMetadata() reads Ultralytics' `end2end`
//             custom metadata key. "True" (case-insensitive) => Xyxy;
//             anything else (including missing) => Cxcywh.
//
//  Returns BoxFormat::Undecided when nothing definitive can be said; the UI
//  defaults the dropdown to Cxcywh in that case but lets the user override.
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/box_format.h"
#include "inference/model_type.h"   // ensureOrtApiReady()

#define ORT_API_MANUAL_INIT
#include "onnxruntime_cxx_api.h"

#include "tensorflow/lite/schema/schema_generated.h"

#include <android/log.h>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#define TAG "AimbotBoxFmt"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {

const char* boxFormatName(BoxFormat f) {
    switch (f) {
        case BoxFormat::Xyxy:    return "xyxy";
        case BoxFormat::Cxcywh:  return "cxcywh";
        default:                 return "undecided";
    }
}

namespace {

bool lowerIsTrue(const char* s) {
    if (s == nullptr) return false;
    std::string t;
    for (; *s != '\0'; ++s) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        t.push_back(c);
    }
    return t == "true" || t == "1";
}

bool fileToBytes(const std::string& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(in.read(out.data(), size));
}

// ── ONNX path: read `end2end` metadata key ─────────────────────────────────

BoxFormat analyzeOnnxBoxFormat(const std::string& path) {
    if (!ensureOrtApiReady()) return BoxFormat::Undecided;
    try {
        static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "boxfmt");
        Ort::SessionOptions so;
        Ort::Session sess(env, path.c_str(), so);
        Ort::ModelMetadata md = sess.GetModelMetadata();
        Ort::AllocatorWithDefaultOptions alloc;
        std::vector<Ort::AllocatedStringPtr> keys = md.GetCustomMetadataMapKeysAllocated(alloc);
        for (auto& keyPtr : keys) {
            const std::string key = keyPtr.get();
            if (key != "end2end") continue;
            Ort::AllocatedStringPtr val = md.LookupCustomMetadataMapAllocated("end2end", alloc);
            if (val == nullptr) return BoxFormat::Cxcywh;
            const std::string v = val.get();
            LOGI("box-format: ONNX end2end=%s -> %s", v.c_str(),
                 lowerIsTrue(v.c_str()) ? "xyxy" : "cxcywh");
            return lowerIsTrue(v.c_str()) ? BoxFormat::Xyxy : BoxFormat::Cxcywh;
        }
        LOGI("box-format: ONNX metadata lacks end2end key -> cxcywh");
        return BoxFormat::Cxcywh;
    } catch (const std::exception& e) {
        LOGW("box-format: ONNX probe failed (%s) -> undecided", e.what());
        return BoxFormat::Undecided;
    } catch (...) {
        return BoxFormat::Undecided;
    }
}

// ── TFLite path: walk subgraph[0].operators ────────────────────────────────

// A tensor qualifies as a "box head" when its channels axis carries between
// 4 and 100 inclusive. The YOLO convention is channels-first (N, C, ...) so
// shp[1] holds the channel count. 4 = pure boxes (split-head), 4+nc = boxes
// plus classes (single-head combined output). Anything outside the window
// is treated as not-a-box-head (a scores-only auxiliary or a classifier
// tensor — both keep the walk out of).
inline bool tensorHasBoxHead(const flatbuffers::Vector<int32_t>* shape) {
    if (shape == nullptr) return false;
    const int ndims = static_cast<int>(shape->size());
    if (ndims < 2) return false;
    const int ch = shape->Get(1);
    return ch >= 4 && ch <= 100;
}

inline int channelCountOf(const flatbuffers::Vector<int32_t>* shape) {
    if (shape == nullptr) return 0;
    if (static_cast<int>(shape->size()) < 2) return 0;
    const int ch = shape->Get(1);
    return (ch >= 4 && ch <= 100) ? ch : 0;
}

// Walks back from `startIdx` through producer ops, terminating at:
//   - Sub / Add / Mul / Div / BatchMatMul  -> Xyxy
//   - Conv2D / DWConv2D / Conv3D / FC / TConv -> Cxcywh
//   - pass-through (Reshape / Transpose / Identity / Split / Concat) -> follow
//   - any other op -> Undecided (give up at first unknown)
struct WalkResult { bool decided = false; bool isXyxy = false; };
WalkResult walkChain(const tflite::SubGraph* sg,
                     const std::vector<int>& producer,
                     const std::vector<tflite::BuiltinOperator>& opcodeBuiltin,
                     int startIdx) {
    WalkResult r;
    if (sg == nullptr || sg->tensors() == nullptr || sg->operators() == nullptr) return r;
    int cur = startIdx;
    std::vector<bool> visited(sg->tensors()->size(), false);
    for (int d = 0; d < 32; ++d) {
        if (cur < 0 || static_cast<size_t>(cur) >= visited.size()) break;
        if (visited[static_cast<size_t>(cur)]) break;
        visited[static_cast<size_t>(cur)] = true;
        const int opIdx = producer[static_cast<size_t>(cur)];
        if (opIdx < 0 || static_cast<size_t>(opIdx) >= sg->operators()->size()) break;
        const tflite::Operator* op = sg->operators()->Get(opIdx);
        if (op == nullptr || op->inputs() == nullptr || op->inputs()->size() == 0) break;
        tflite::BuiltinOperator bo = tflite::BuiltinOperator_CUSTOM;
        const uint32_t opcIdx = op->opcode_index();
        if (opcIdx < opcodeBuiltin.size()) {
            bo = opcodeBuiltin[opcIdx];
        }
        switch (bo) {
            case tflite::BuiltinOperator_SUB:
            case tflite::BuiltinOperator_ADD:
            case tflite::BuiltinOperator_MUL:
            case tflite::BuiltinOperator_DIV:
            case tflite::BuiltinOperator_BATCH_MATMUL:
                r.decided = true; r.isXyxy = true; return r;
            case tflite::BuiltinOperator_CONV_2D:
            case tflite::BuiltinOperator_DEPTHWISE_CONV_2D:
            case tflite::BuiltinOperator_FULLY_CONNECTED:
            case tflite::BuiltinOperator_CONV_3D:
            case tflite::BuiltinOperator_TRANSPOSE_CONV:
                r.decided = true; r.isXyxy = false; return r;
            case tflite::BuiltinOperator_RESHAPE:
            case tflite::BuiltinOperator_TRANSPOSE:
            case tflite::BuiltinOperator_SPLIT:
            case tflite::BuiltinOperator_CONCATENATION:
            case tflite::BuiltinOperator_CONCAT_EMBEDDINGS:
                cur = op->inputs()->Get(0);
                continue;
            default:
                return r;
        }
    }
    return r;
}

BoxFormat analyzeTfliteBoxFormat(const std::string& path) {
    std::vector<char> bytes;
    if (!fileToBytes(path, bytes)) {
        LOGW("box-format: cannot read tflite %s", path.c_str());
        return BoxFormat::Undecided;
    }
    const tflite::Model* m = tflite::GetModel(bytes.data());
    if (m == nullptr || m->subgraphs() == nullptr || m->subgraphs()->size() < 1) {
        return BoxFormat::Undecided;
    }
    const tflite::SubGraph* sg = m->subgraphs()->Get(0);
    if (sg == nullptr || sg->tensors() == nullptr || sg->operators() == nullptr) {
        return BoxFormat::Undecided;
    }

    // Opcode index -> BuiltinOperator enum. Custom / unrecognised codes
    // become BuiltinOperator_CUSTOM, which terminates the walk at that step
    // (conservative — Undecided rather than a guess).
    std::vector<tflite::BuiltinOperator> opcodeBuiltin;
    if (m->operator_codes() != nullptr) {
        opcodeBuiltin.reserve(m->operator_codes()->size());
        for (size_t i = 0; i < m->operator_codes()->size(); ++i) {
            const tflite::OperatorCode* c = m->operator_codes()->Get(i);
            // The C++ schema has no NONE sentinel; the schema's default for
            // a missing built-in code is the first enum (BuiltinOperator_ADD
            // = 0). Distinguish custom by `custom_code` being non-empty, in
            // which case we tag as CUSTOM and let the walk bail out.
            if (c == nullptr) {
                opcodeBuiltin.push_back(tflite::BuiltinOperator_CUSTOM);
                continue;
            }
            const bool isCustom =
                (c->custom_code() != nullptr && c->custom_code()->size() > 0) ||
                c->builtin_code() == tflite::BuiltinOperator_CUSTOM;
            tflite::BuiltinOperator bo = c->builtin_code();
            if (static_cast<int32_t>(bo) == 0 && c->deprecated_builtin_code() != 0) {
                bo = static_cast<tflite::BuiltinOperator>(c->deprecated_builtin_code());
            }
            opcodeBuiltin.push_back(isCustom
                                        ? tflite::BuiltinOperator_CUSTOM
                                        : bo);
        }
    }

    // tensor_index -> producer op index (-1 = no producer = input).
    std::vector<int> producer(sg->tensors()->size(), -1);
    for (size_t i = 0; i < sg->operators()->size(); ++i) {
        const tflite::Operator* op = sg->operators()->Get(i);
        if (op == nullptr || op->outputs() == nullptr) continue;
        for (size_t k = 0; k < op->outputs()->size(); ++k) {
            const int idx = op->outputs()->Get(k);
            if (idx >= 0 && static_cast<size_t>(idx) < producer.size()) {
                producer[static_cast<size_t>(idx)] = static_cast<int>(i);
            }
        }
    }

    // Identify which outputs to walk.
    const size_t nOut = (sg->outputs() != nullptr) ? sg->outputs()->size() : 0;
    std::vector<int> outIdx(nOut, -1);
    std::vector<int> outCh(nOut, 0);
    for (size_t i = 0; i < nOut; ++i) {
        outIdx[i] = sg->outputs()->Get(i);
        if (outIdx[i] >= 0 && static_cast<size_t>(outIdx[i]) < sg->tensors()->size()) {
            const tflite::Tensor* t = sg->tensors()->Get(outIdx[i]);
            outCh[i] = (t != nullptr) ? channelCountOf(t->shape()) : 0;
        }
    }
    const int nWith4 = [&]() {
        int n = 0;
        for (size_t i = 0; i < outCh.size(); ++i) if (outCh[i] == 4) ++n;
        return n;
    }();
    const bool isSplitHead = (nWith4 == 1 && nOut >= 2);

    bool anyDecided = false;
    bool allXyxy = true;
    int walksDecided = 0;
    for (size_t i = 0; i < nOut; ++i) {
        if (outCh[i] == 0) continue;
        if (isSplitHead && outCh[i] != 4) continue;  // skip sibling score tensor
        const WalkResult wr = walkChain(sg, producer, opcodeBuiltin, outIdx[i]);
        if (!wr.decided) continue;
        ++walksDecided;
        if (!wr.isXyxy) allXyxy = false;
    }
    if (walksDecided == 0) {
        LOGI("box-format: TFLite graph walk indecisive (%s)", path.c_str());
        return BoxFormat::Undecided;
    }
    anyDecided = (walksDecided > 0);
    (void)anyDecided;
    if (allXyxy) {
        LOGI("box-format: TFLite graph walk -> xyxy (%s)", path.c_str());
        return BoxFormat::Xyxy;
    }
    LOGI("box-format: TFLite graph walk -> cxcywh (%s)", path.c_str());
    return BoxFormat::Cxcywh;
}

}  // namespace

BoxFormat analyzeBoxFormat(const std::string& path) {
    const std::string lower = [&]() {
        std::string s = path;
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".tfl") == 0) {
        return analyzeTfliteBoxFormat(path);
    }
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".onnx") == 0) {
        return analyzeOnnxBoxFormat(path);
    }
    return BoxFormat::Undecided;
}

}  // namespace infer
}  // namespace aimbotng
