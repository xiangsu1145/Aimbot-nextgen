// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — frame to tensor
//
//  Written once, used by every runtime. An RGBA frame from the capture path
//  becomes an interleaved RGB buffer of the model's expected size, with the
//  aspect ratio preserved and the padding recorded so postprocess can undo it.
//
//  Why this is not inside each engine: it is the single largest *identical*
//  piece of work in the whole pipeline, and the old project proved the cost of
//  duplicating it — four copies, four places to get the stride arithmetic
//  wrong, and one of them bound to be missed.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/inference_types.h"

#include <cstddef>
#include <cstdint>

namespace aimbotng {
namespace infer {

/// How a source frame becomes a tensor.
///
/// The defaults are the YOLO export defaults, because that is the only family
/// of model this pipeline exists to run: RGB, /255, letterboxed to a square
/// with 114/255 grey bars. Every field is here because some export out there
/// disagrees with the default, not because a caller is expected to tune them.
struct PreprocessConfig {
    int targetW = 640;
    int targetH = 640;

    /// Preserve the aspect ratio and pad. Turning this off stretches instead,
    /// which is faster and wrong for anything the model was not trained on.
    bool keepAspect = true;

    /// Colour of the letterbox bars. 114/255 is the grey the YOLO training
    /// pipeline uses; pure black is close enough to be tempting and measurably
    /// worse on boxes that touch the frame edge.
    float padValue = 114.0f / 255.0f;

    /// Interleave order of the *output* buffer. ONNX exports are RGB; some
    /// converted TFLite models are BGR, and a model fed the wrong one still
    /// produces confident boxes — just at the wrong coordinates.
    bool  bgr = false;

    /// 1/255 for float models. One, not 1/255, for models that want raw 0..255.
    float scale = 1.0f / 255.0f;

    /// Normalisation, applied after `scale`. The defaults are the identity.
    bool  normalize = false;
    float mean[3] = {0.0f, 0.0f, 0.0f};
    float stdv[3] = {1.0f, 1.0f, 1.0f};

    /// Whether the model wants its input transposed to (1, 3, H, W) — NCHW —
    /// or left as (1, H, W, 3) — NHWC. Written by *this* side rather than
    /// fixed up afterwards, because transposing a 640x640x3 buffer after the
    /// fact is a second full pass over 1.2 MB.
    bool nchw = false;
};

/// Everything needed to take a detection back out of target space and into the
/// frame it came from. Produced by the letterbox functions, consumed by the
/// decoders, and — critically — carried per-frame rather than stored, because
/// an aspect ratio that changes mid-session (a rotation, a resize) would
/// otherwise silently skew every box until the next reload.
struct LetterboxMap {
    float scale = 1.0f;   // source pixels -> target pixels
    float padX  = 0.0f;   // left bar, target pixels
    float padY  = 0.0f;   // top bar, target pixels

    int roiX = 0;         // the source rect that was cropped, frame pixels
    int roiY = 0;

    /// Target-space box -> frame-space box. In place, for four floats.
    void toFrame(float& x1, float& y1, float& x2, float& y2) const;

    /// Convenience for the common "there was no letterbox" case, e.g. a model
    /// whose input matches the crop exactly.
    static LetterboxMap identity() { return LetterboxMap{}; }
};

/// RGBA8888 (the capture format) -> interleaved RGB, float, normalised.
///
/// `out` must hold `targetW * targetH * 3` floats, laid out NHWC or NCHW
/// according to the config. The ROI is cropped out of the frame first; the
/// result is then scaled and padded into the target square.
///
/// Returns the map to hand to the decoder. Returns an identity map, and writes
/// nothing, when the frame or the config does not make sense — the caller
/// checks FrameView::valid() and the ROI itself, and a silent identity is
/// easier to notice than garbage coordinates.
LetterboxMap letterboxRgba(const FrameView&, const Roi&,
                           const PreprocessConfig&, float* out);

/// The same, to interleaved RGB **uint8**.
///
/// This is not a convenience: quantised models take uint8 input directly, and
/// going through float to get there costs a 1.2 MB conversion per frame for
/// nothing. The scaling and normalisation fields are ignored for this variant,
/// since uint8 IS the 0..255 representation they would produce.
LetterboxMap letterboxRgbaU8(const FrameView&, const Roi&,
                             const PreprocessConfig&, uint8_t* out);

/// Bytes a preprocessed frame occupies, for a caller sizing its own buffer.
size_t preprocessBufferBytes(const PreprocessConfig&, int bytesPerChannel);

/// The config a model of this size wants, with everything else at default.
/// Exists so the Session and the menu cannot disagree about what "the config
/// for a 640 model" means.
PreprocessConfig preprocessFor(int inputSize);

}  // namespace infer
}  // namespace aimbotng
