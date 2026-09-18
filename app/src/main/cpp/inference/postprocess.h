// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — tensor to boxes
//
//  Also written once. What varies between YOLO exports is the *shape* of the
//  output tensor, not the meaning of the numbers in it, so that is the only
//  thing the config carries: pick a layout, and the decode and the suppression
//  are the same code for every runtime and every version.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/inference_types.h"
#include "inference/preprocess.h"

#include <cstddef>
#include <vector>

namespace aimbotng {
namespace infer {

/// The output layouts that have actually been seen coming out of the exporters
/// used with this project. Chosen, not sniffed — guessing the layout from the
/// tensor shape works right up until two layouts share a shape, at which point
/// it fails silently and confidently.
enum class HeadLayout {
    /// (1, 4 + nc, N) — the raw YOLOv8/v11 ONNX export. Channels first, so the
    /// values for one anchor are `stride` apart rather than adjacent.
    V8ChannelsFirst,

    /// (1, N, 4 + nc) — the same model after a transpose, which is what the
    /// NCNN conversion produces.
    V8Rows,

    /// (1, N, 5 + nc) — YOLOv5 and its descendants: the fifth value is an
    /// objectness score that multiplies the class score rather than standing
    /// beside it.
    V5Objectness,

    /// Boxes in frame pixels on one tensor, scores and class ids on others.
    /// Seen from TFLite exports that ship the detector's post-processing.
    Separate,
};

/// One output tensor, described by what it means rather than how it is stored.
struct OutputTensor {
    const float* data = nullptr;
    int  rows = 0;            // anchors
    int  cols = 0;            // values per anchor
    /// True when the memory is (cols, rows) — i.e. `data[channel * rows + i]`.
    /// This is the YOLOv8 ONNX export and it is the single most common way to
    /// get a decoder subtly wrong, so it gets its own field and its own test.
    bool channelsFirst = false;

    bool valid() const { return data != nullptr && rows > 0 && cols > 0; }
};

struct PostprocessConfig {
    HeadLayout layout = HeadLayout::V8ChannelsFirst;

    int   numClasses = 1;
    float confidence = 0.5f;
    float iou        = 0.45f;

    /// Upper bound on what comes back. The point of it is not accuracy: the
    /// consumer draws boxes and picks a target, and past a few dozen the list
    /// costs more than it informs.
    int maxDetections = 64;

    /// Model input size (pixels). Used to rescale *normalized* (0..1) bbox
    /// coordinates back to input pixels before the letterbox inverse-map.
    /// Many YOLOv8 TFLite exports emit normalized coordinates; the decoder
    /// detects this (cx,cy <= 1.5) and multiplies by inputW/H. Passing 0
    /// disables the rescale (assume the model already emits pixel coords).
    int inputW = 0;
    int inputH = 0;

    /// Whether the raw values are logits needing a sigmoid. YOLOv8 exports
    /// post-sigmoid values; YOLOv5 exports before it, depending on the export
    /// script version. Getting this wrong does not crash — it produces boxes
    /// with scores near 0.5 for everything, which is why it is a field and not
    /// a guess.
    bool applySigmoid = false;

    /// Drop boxes whose class is not in this set when non-empty. Empty means
    /// keep everything. Used by the auto-aim layer to want only players, and
    /// applied here rather than after so the result list is already the right
    /// length for the consumer.
    std::vector<int> keepClasses;
};

/// Decodes one tensor: threshold, undo the letterbox, clamp to the frame,
/// suppress overlaps, sort by score.
std::vector<Box> decode(const OutputTensor&, const PostprocessConfig&,
                        const LetterboxMap&, const FrameView&);

/// Decodes the four-tensor layout: `boxes` as (N, 4) in target space, `scores`
/// as (N,) or (N, numClasses), `classes` and `count` optional.
std::vector<Box> decodeSeparate(const float* boxes, int numBoxes,
                                const float* scores, const float* classes,
                                int numScores, const PostprocessConfig&,
                                const LetterboxMap&, const FrameView&);

/// Non-maximum suppression, per class, greedy, highest score first.
///
/// Exposed separately because the auto-aim layer sometimes wants to suppress
/// across several ROIs after concatenating them, and doing that one ROI at a
/// time would keep two boxes on the same target.
std::vector<Box> nms(std::vector<Box> boxes, float iou);

/// Maps four frame-space corners into the model's input space. The inverse of
/// LetterboxMap::toFrame, needed to line the ROI up with the tensor.
void frameToTarget(const LetterboxMap&, float& x1, float& y1, float& x2,
                   float& y2);

/// The config a model of this size and class count wants.
PostprocessConfig postprocessFor(int inputSize, int numClasses);

/// Decides the head layout and the class count from a head's shape.
///
/// Every engine needs this, and every engine used to carry its own copy — which
/// is how the same off-by-one shipped four times. The rules, in order:
///
///  1. **A declared class count wins.** It is the only fact available from
///     outside the graph, and it is the fact a user can fix in the model list.
///     The 4 vs 5 offset then follows: v5 inserts an objectness score between
///     the box and the classes, v8 does not.
///
///  2. **Nothing declared: the anchor grid decides.** The grid a YOLO head runs
///     on is known — (H/8)² + (H/16)² + (H/32)² anchors for an anchor-free v8
///     export, and *three times* that for a v5 one, because v5 still carries
///     three anchor boxes per cell. So for a 256-pixel input the two candidates
///     are 1344 and 4032, and an observed anchor count matching one of them
///     exactly settles the layout. This is the part a channel-count rule cannot
///     do: [1,5,1344] and [1,4032,7] are both "a head", and only the anchors
///     say which family they came from.
///
///  3. **Otherwise v8**, the export the Add-Model dialog produces. The guess is
///     logged so a wrong box count can be traced back to it.
///
/// An exact grid match is required for (2) and a non-matching count falls
/// through to (3), so an unrecognised model behaves exactly as it did before.
///
/// `channelsFirst` selects only between the two v8 spellings; `V5Objectness` is
/// a statement about the numbers, and orientation travels separately (see
/// OutputTensor::channelsFirst). `inputSize` is the model's square input edge;
/// 0 or a non-dividing value disables rule (2). `numClasses` is left at 0 only
/// when the head carries no class scores at all, which the caller reports as a
/// bad head.
HeadLayout resolveHeadLayout(int anchors, int channels, bool channelsFirst,
                             int inputSize, int declaredClasses,
                             int& numClasses);

}  // namespace infer
}  // namespace aimbotng
