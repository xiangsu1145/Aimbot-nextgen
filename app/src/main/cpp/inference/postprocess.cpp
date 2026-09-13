// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of postprocess.h
//
//  Four layouts, one decoder. The only thing the layouts disagree about is
//  where a number lives; what the numbers mean is the same, so the accessor is
//  the branch and everything after it is shared.
//
//  Order of operations matters and is not arbitrary:
//
//    1. threshold on the raw score  — cheapest test, and most anchors fail it
//    2. map back through the letterbox
//    3. clamp to the frame, drop anything whose centre fell outside
//    4. suppress overlaps
//    5. truncate to maxDetections
//
//  Doing the map before the suppression is deliberate: NMS compares areas, and
//  suppressing in target space would judge two boxes by the aspect the letterbox
//  gave them rather than the aspect they have on screen.
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/postprocess.h"

#include <algorithm>
#include <cmath>

namespace aimbotng {
namespace infer {
namespace {

inline float sigmoidf(float x) { return 1.0f / (1.0f + std::exp(-x)); }

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float iouOf(const Box& a, const Box& b) {
    const float ix = std::max(0.0f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
    const float iy = std::max(0.0f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    const float inter = ix * iy;
    if (inter <= 0.0f) return 0.0f;
    const float uni = a.area() + b.area() - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

bool classWanted(const PostprocessConfig& cfg, int cls) {
    if (cfg.keepClasses.empty()) return true;
    return std::find(cfg.keepClasses.begin(), cfg.keepClasses.end(), cls) !=
           cfg.keepClasses.end();
}

/// Turns a target-space cx/cy/w/h into frame-space corners, clamped to the
/// frame. Returns false when the box's centre landed outside the frame, which
/// is the one case that cannot be salvaged by clamping: a box centred off-frame
/// is an artefact of the letterbox, not a target.
bool toFrameBox(float cx, float cy, float bw, float bh, const LetterboxMap& map,
                const FrameView& frame, Box& out) {
    float x1 = cx - bw * 0.5f;
    float y1 = cy - bh * 0.5f;
    float x2 = cx + bw * 0.5f;
    float y2 = cy + bh * 0.5f;
    map.toFrame(x1, y1, x2, y2);

    const float fw = static_cast<float>(frame.width);
    const float fh = static_cast<float>(frame.height);
    const float ccx = (x1 + x2) * 0.5f;
    const float ccy = (y1 + y2) * 0.5f;
    if (ccx < 0.0f || ccy < 0.0f || ccx >= fw || ccy >= fh) return false;

    out.x1 = clampf(x1, 0.0f, fw);
    out.y1 = clampf(y1, 0.0f, fh);
    out.x2 = clampf(x2, 0.0f, fw);
    out.y2 = clampf(y2, 0.0f, fh);
    return out.x2 > out.x1 && out.y2 > out.y1;
}

/// Reads one value out of a tensor whose memory order the layout describes.
struct Reader {
    const float* data = nullptr;
    int rows = 0;          // anchors
    int cols = 0;          // values per anchor
    bool channelsFirst = false;

    float at(int anchor, int channel) const {
        return channelsFirst ? data[static_cast<size_t>(channel) * static_cast<size_t>(rows) +
                                     static_cast<size_t>(anchor)]
                             : data[static_cast<size_t>(anchor) * static_cast<size_t>(cols) +
                                     static_cast<size_t>(channel)];
    }
};

}  // namespace

// ── NMS ─────────────────────────────────────────────────────────────────────

std::vector<Box> nms(std::vector<Box> boxes, float iou) {
    if (boxes.size() < 2) return boxes;

    std::sort(boxes.begin(), boxes.end(),
              [](const Box& a, const Box& b) { return a.score > b.score; });

    std::vector<Box>    kept;
    std::vector<char>   dead(boxes.size(), 0);
    kept.reserve(boxes.size());

    for (size_t i = 0; i < boxes.size(); ++i) {
        if (dead[i]) continue;
        kept.push_back(boxes[i]);
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (dead[j]) continue;
            // Per class: a person overlapping a car is two findings, not one.
            // Suppressing across classes is the classic way to lose every
            // detection but the highest-scoring one on a crowded screen.
            if (boxes[i].cls != boxes[j].cls) continue;
            if (iouOf(boxes[i], boxes[j]) > iou) dead[j] = 1;
        }
    }
    return kept;
}

// ── Decode ──────────────────────────────────────────────────────────────────

std::vector<Box> decode(const OutputTensor& t, const PostprocessConfig& cfg,
                        const LetterboxMap& map, const FrameView& frame) {
    std::vector<Box> out;
    if (!t.valid() || !frame.valid()) return out;

    const int anchors = t.rows;
    out.reserve(static_cast<size_t>(std::min(anchors, 256)));

    if (cfg.layout == HeadLayout::Separate) {
        // A four-tensor model handed to the single-tensor decoder is a caller
        // bug, and returning nothing is the honest answer: there is no way to
        // guess which other buffers the caller meant.
        return out;
    }

    const Reader r{t.data, t.rows, t.cols, t.channelsFirst};
    const bool v5 = (cfg.layout == HeadLayout::V5Objectness);

    // Where the class scores start, and how many there are. Taken from the
    // tensor's own width where it can be, because a stored class count that
    // disagrees with the model produces confident nonsense rather than an error.
    const int firstClass = v5 ? 5 : 4;
    const int available = t.cols - firstClass;
    if (available < 1) return out;
    const int classes = cfg.numClasses > 0 ? std::min(cfg.numClasses, available) : available;

    for (int i = 0; i < anchors; ++i) {
        int   bestCls   = 0;
        float bestScore = -1.0f;
        for (int c = 0; c < classes; ++c) {
            float s = r.at(i, firstClass + c);
            if (cfg.applySigmoid) s = sigmoidf(s);
            if (s > bestScore) { bestScore = s; bestCls = c; }
        }
        if (v5) {
            float obj = r.at(i, 4);
            if (cfg.applySigmoid) obj = sigmoidf(obj);
            bestScore *= obj;
        }
        if (bestScore < cfg.confidence) continue;
        if (!classWanted(cfg, bestCls)) continue;

        float cx = r.at(i, 0);
        float cy = r.at(i, 1);
        float bw = r.at(i, 2);
        float bh = r.at(i, 3);

        // Match the old project's normalizeIfNeeded(): TFLite/YOLOv8 exports
        // frequently emit normalized (0..1) coordinates. Treat a small cx,cy
        // (<= 1.5) as normalized and scale back to input pixels, otherwise
        // assume the values are already pixels. Without this the boxes land in
        // the top-left at ~1px because 0.5 is taken for half a pixel.
        if (cfg.inputW > 0 && cfg.inputH > 0 && cx <= 1.5f && cy <= 1.5f) {
            cx *= static_cast<float>(cfg.inputW);
            cy *= static_cast<float>(cfg.inputH);
            bw *= static_cast<float>(cfg.inputW);
            bh *= static_cast<float>(cfg.inputH);
        }

        Box box;
        box.score = bestScore;
        box.cls   = bestCls;
        if (toFrameBox(cx, cy, bw, bh, map, frame, box)) out.push_back(box);
    }

    out = nms(std::move(out), cfg.iou);
    if (cfg.maxDetections > 0 && static_cast<int>(out.size()) > cfg.maxDetections) {
        out.resize(static_cast<size_t>(cfg.maxDetections));
    }
    out.shrink_to_fit();
    return out;
}

std::vector<Box> decodeSeparate(const float* boxes, int numBoxes,
                                const float* scores, const float* classes,
                                int numScores, const PostprocessConfig& cfg,
                                const LetterboxMap& map, const FrameView& frame) {
    std::vector<Box> out;
    if (boxes == nullptr || scores == nullptr || !frame.valid()) return out;

    const int n = numBoxes > 0 ? numBoxes : numScores;
    if (n <= 0) return out;

    // The scores buffer is either one value per box or a full class row per
    // box; the stride tells them apart without a flag.
    const int classStride = (numScores >= n && cfg.numClasses > 0 &&
                             numScores % n == 0)
                                ? numScores / n
                                : 1;

    for (int i = 0; i < n; ++i) {
        int   bestCls   = 0;
        float bestScore = scores[i * classStride];
        for (int c = 1; c < classStride; ++c) {
            const float s = scores[i * classStride + c];
            if (s > bestScore) { bestScore = s; bestCls = c; }
        }
        if (classes != nullptr) bestCls = static_cast<int>(classes[i]);
        if (bestScore < cfg.confidence) continue;
        if (!classWanted(cfg, bestCls)) continue;

        // A detector-attached post-processing block emits y1,x1,y2,x2 in input
        // pixels — the order is a TF object-detection convention, not a typo.
        // Normalised (0..1) variants exist but are not handled: they would need
        // the input size, and guessing whether a tensor is normalised from its
        // values is exactly the kind of inference that works on the model it was
        // tested with and silently misplaces boxes on the next one.
        const float x1 = std::min(boxes[i * 4 + 1], boxes[i * 4 + 3]);
        const float y1 = std::min(boxes[i * 4 + 0], boxes[i * 4 + 2]);
        const float x2 = std::max(boxes[i * 4 + 1], boxes[i * 4 + 3]);
        const float y2 = std::max(boxes[i * 4 + 0], boxes[i * 4 + 2]);

        Box box;
        box.score = bestScore;
        box.cls   = bestCls;
        if (toFrameBox((x1 + x2) * 0.5f, (y1 + y2) * 0.5f, x2 - x1, y2 - y1,
                       map, frame, box)) {
            out.push_back(box);
        }
    }

    out = nms(std::move(out), cfg.iou);
    if (cfg.maxDetections > 0 && static_cast<int>(out.size()) > cfg.maxDetections) {
        out.resize(static_cast<size_t>(cfg.maxDetections));
    }
    out.shrink_to_fit();
    return out;
}

// ── Coordinate helpers ──────────────────────────────────────────────────────

void frameToTarget(const LetterboxMap& map, float& x1, float& y1, float& x2,
                   float& y2) {
    x1 = (x1 - static_cast<float>(map.roiX)) * map.scale + map.padX;
    y1 = (y1 - static_cast<float>(map.roiY)) * map.scale + map.padY;
    x2 = (x2 - static_cast<float>(map.roiX)) * map.scale + map.padX;
    y2 = (y2 - static_cast<float>(map.roiY)) * map.scale + map.padY;
}

PostprocessConfig postprocessFor(int inputSize, int numClasses) {
    (void)inputSize;   // the decoder reads geometry from the tensor, not a hint
    PostprocessConfig cfg;
    cfg.numClasses = numClasses > 0 ? numClasses : 1;
    return cfg;
}

}  // namespace infer
}  // namespace aimbotng
