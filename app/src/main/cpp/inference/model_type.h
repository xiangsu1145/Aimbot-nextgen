// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — model type detection, declared without ORT headers.
//
//  The implementation lives in ort_engine.cpp (the only translation unit that
//  includes the ONNX Runtime C++ API). This header lets the menu label each
//  model with its tensor element type (fp16 / fp32 / int8 / …) without pulling
//  the ORT headers — and their ORT_API_MANUAL_INIT requirement — into the UI.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>

namespace aimbotng {
namespace infer {

/// Initialises the process-global ONNX Runtime API table once.
///
/// The inference engine builds the table lazily on its first use, but
/// model_type.cpp's probes also construct an Ort::Env to read the input tensor's
/// dtype and shape. Those probes run inside the shell daemon too, where they are
/// the FIRST Ort:: call in the process — without this the wrapper dereferences a
/// null function-pointer table and SIGSEGVs (the tombstone one sees when the
/// daemon crashes before publishing its port: fault addr 0x18 in
/// Ort::Env::Env). The table is loaded by dlopen'ing libonnxruntime.so from
/// our own DT_NEEDED path, so no symbol is referenced at link time.
///
/// Returns false if libonnxruntime.so cannot be loaded (e.g. its dependencies
/// are unavailable in the shell process). Callers MUST check the return value
/// and not construct any Ort::* object on failure.
bool ensureOrtApiReady();

/// Peeks the input element type of a model file, as a short label:
/// "fp32", "fp16", "bf16", "int8", "uint8", "?" for anything else, or ""
/// if the file cannot be read. For .onnx the dtype is read from the graph;
/// other kinds return "" (not yet peeked here).
std::string detectModelType(const std::string& path);

/// Peeks the model's square input edge (the height/width of the detection
/// input), e.g. 640 for a 640x640 model. Returns false when the size is not
/// statically known (a dynamic / -1 dimension, or an unreadable file), so the
/// caller can fall back to its own default. Most detection models are square,
/// so a single edge is enough to label the resolution in the UI.
bool detectModelInputSize(const std::string& path, int& w, int& h);

/// Peeks both the input edge and the number of *detection classes* the model
/// outputs, in one probe (so the menu can show "N classes • WxH" without a
/// second model load). `classes` is derived from the output tensor's channel
/// axis minus the 4 box coordinates (YOLO-style): for [1,C,A] or [1,A,C] the
/// smaller of the two feature dims is the channel count. Returns false (and
/// leaves w/h/classes unchanged) when the file cannot be read.
bool detectModelShape(const std::string& path, int& w, int& h, int& classes);

}  // namespace infer
}  // namespace aimbotng
