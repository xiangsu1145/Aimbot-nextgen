#pragma once

#include <string>

namespace aimbotng {
namespace infer {

/// What the 4-channel box head of a YOLO-family detector emits.
///
/// The model does not advertise this through a portable cross-backend flag;
/// most models either bake `dist2bbox` into the graph (xyxy pixel coords,
/// "end2end" exports) or leave it raw and rely on the engine to do the
/// (cx,cy,w,h) -to-corners math at post-process time. Detection is by
/// graph walk: see analyzeBoxFormat().
enum class BoxFormat {
    Undecided = 0,  // couldn't determine; caller picks the project default
    Cxcywh,         // (cx, cy, w, h) — typically normalised [0,1]
    Xyxy,           // (x1, y1, x2, y2) — typically pixel coordinates
};

/// Walks the model's static operator graph (no runtime Invoke) and figures
/// out which format the box head emits.
///
///   .tflite — subgraph[0].operators walked back from the declared output
///             tensors; Sub/Add/Mul/MatMul on the box-channel chain → Xyxy,
///             Conv/Reshape on a 4-ch box tensor → Cxcywh.
///   .onnx   — Ort::Session::GetModelMetadata() reads the `end2end` custom
///             metadata key Ultralytics writes when `dist2bbox` is baked in.
///             `end2end="True"` (case-insensitive) → Xyxy; anything else
///             (including missing) → Cxcywh. The key is the user-visible
///             string Ultralytics itself sets, so its meaning is one we can
///             rely on across every Ultralytics export.
///
/// Never invokes the runtime; reads bytes only. Returns Undecided when the
/// path is unreadable, the format is unknown, or the analysis cannot
/// conclude — callers fall back to their project default (cxcywh).
BoxFormat analyzeBoxFormat(const std::string& path);

/// Short label for log lines: "cxcywh" / "xyxy" / "undecided".
const char* boxFormatName(BoxFormat f);

}  // namespace infer
}  // namespace aimbotng
