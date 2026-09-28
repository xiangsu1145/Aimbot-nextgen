// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::recoil — on-device pattern recorder and analyser
//
//  Why this exists at all, given the PC analyser (`D:\recoil-analyser`):
//
//  The PC tool is the accurate one — it tracks a wall marker across a recorded
//  video WITH sub-pixel NCC, reads the ammunition counter with PaddleOCR to
//  place every shot, and cross-checks the two. None of that can run on the
//  phone: cv2 + OCR is far too heavy for a 116 Hz render thread, and the daemon
//  has no video decoder to feed it from either (there is no encoder in this
//  project — the capture path hands out single frames, not a stream).
//
//  What the phone CAN do is the part that actually matters here: hold a patch
//  of the wall still under a correlation tracker while the burst plays out, and
//  report where it went. That is the measurement. Everything the PC tool adds on
//  top of it — OCR shot detection, cross-validation — exists to make the *shot
//  times* precise, and shot times are recoverable from the trajectory itself,
//  because a recoil kick is the largest thing that happens to the view between
//  two frames. So:
//
//    frames -> marker track (this file)  ->  θ(t)  ->  per-shot steps
//         ->  resampled knots  ->  <stem>.json
//
//  The result is self-contained: no video file, no PC round trip, and the
//  progress bar has something real to report (samples tracked, then samples
//  analysed) instead of being theatre.
//
//  The frame source is the existing capture pipeline, which hands out a SQUARE
//  CENTRE CROP of the screen at `capture::size()`. That constrains where the
//  marker may be placed — inside that square — so the page draws the square and
//  refuses to start when the marker is not in it. For the horizontal axis this
//  costs nothing (the horizontal excursion is tens of pixels); for the vertical
//  one it is a real limit, and the reason the note is on the page rather than
//  buried here.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

#include "tracking/recoil_profile.h"

namespace aimbotng {
namespace recoil {

enum class RecState {
    Idle,        ///< nothing going on
    Armed,       ///< waiting for frames / producer to come up
    Recording,   ///< sampling the marker every frame that arrives
    Analyzing,   ///< stop was pressed; the sample set is being reduced
    Done,        ///< analysis finished; the summary is on screen
    Failed,      ///< it did not work, and `stats.message` says why
};

/// What the progress popup paints. Updated in place every frame while the
/// recorder runs, so the UI only has to read it.
struct RecStats {
    RecState state = RecState::Idle;
    float    progress = 0.0f;   ///< 0..1, meaningful in Analyzing / Done
    int      frames = 0;        ///< frames sampled so far
    int      good = 0;          ///< frames whose marker match was accepted
    float    seconds = 0.0f;   ///< wall time the recording has been running
    float    windowSec = 0.0f; ///< auto-stop horizon (0 when not recording)
    std::string message;        ///< one line, the reason for the current state
    std::string stage;          ///< which analysis pass is running

    // ── Result (valid in Done) ──────────────────────────────────────────────
    int   shots = 0;
    float dxPeak = 0.0f, dyPeak = 0.0f;
    float minConf = 0.0f, meanConf = 0.0f;
    float fps = 0.0f;
    std::string klass = "unknown";
    std::string outStem;        ///< what got written, for the popup's caption
};

/// Screen-space rectangle of the marker the tracker locks onto. The patch that
/// is actually correlated is this rect DEFLATED by [kPatchInset], so the dashed
/// outline the page draws around it never lands inside the correlation window
/// (a stationary outline over moving content is a systematic bias, not noise).
struct MarkerBox {
    float x = -1.0f, y = -1.0f, size = 48.0f;
    bool  placed = false;
};

inline constexpr float kPatchInset   = 6.0f;   ///< px trimmed off each side
inline constexpr int   kPatchMinPx   = 10;     ///< smallest usable patch side
inline constexpr int   kSearchRadius = 20;     ///< px the marker may jump per frame

/// Begins a recording. `stem` names the output profile. `fovDeg` is recorded as
/// provenance and used for px→deg; because the compensation scale is S/Θpp it
/// cancels out of the result, so a wrong FOV shifts α but not the compensation.
/// `screenW/H` are the *display* size — the focal length is derived from the
/// screen height, not from the capture crop, or the degrees would change with
/// the crop size.
/// Returns false (with `stats().message` set) when the marker is unusable.
bool start(const std::string& stem, float fovDeg, const MarkerBox& box,
           int screenW, int screenH);

/// Stops sampling and runs the analysis. The stem / box handed to start() are
/// kept, so this needs no arguments. Cheap enough to run on the render thread
/// in slices — see pump().
void stopAndAnalyze();

/// Throws the samples away without writing anything.
void abort();

/// One render-thread slice: samples one frame while Recording, or advances the
/// analysis while Analyzing. Called from the page's sync, every frame.
void pump();

/// Clears the summary and returns to Idle (the popup's dismiss button).
void dismiss();

RecState state();
const RecStats& stats();

/// True while frames are being consumed — the page sets capture's
/// `setPreviewWanted` from this, so the mirror stays up for the recorder the
/// same way it does for inference.
bool wantsFrames();

/// The samples collected so far, for the page's little live scope.
int sampleCount();
bool sampleAt(int i, float& outTMs, float& outDxDeg, float& outDyDeg);
/// Marker match confidence of sample `i`, 0..1.
float sampleConf(int i);

}  // namespace recoil
}  // namespace aimbotng
