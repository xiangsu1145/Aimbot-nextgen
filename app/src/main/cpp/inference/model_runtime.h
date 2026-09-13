// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer::runtime — the inference platform's front door
//
//  Everything above this header (the menu, the JNI surface, the overlay) talks
//  to inference through these seven functions and nothing else. The engines, the
//  session, the pre/post chain and the model store are all behind it.
//
//  Two jobs, and they are both about being the only place that knows how the
//  pieces fit:
//
//   * **It decides what to run.** The menu's switch sets a model as loaded; this
//     turns that entry into the ModelSpec an engine wants and picks the pair.
//     Keeping the mapping here means the menu never has to know what a Runtime
//     is.
//
//   * **It is the frame pump.** Frames belong to the capture module, which hands
//     out copies. Something has to call that once per render frame and feed the
//     session, and doing it here rather than in the render loop keeps the render
//     loop's own logic about pacing and input untouched.
//
//  The one thing it does not own is whether the capture side is producing at
//  all: that stays with the capture module, and this only *asks* (wantsFrames()).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/inference_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aimbotng {
namespace infer {
namespace runtime {

/// What the last completed inference produced, and how it went.
///
/// Detections are in **screen** coordinates. The engine works in the crop the
/// capture module handed it, and this is where that gets undone — a caller
/// drawing a box or aiming at one should never have to know a crop exists.
struct Snapshot {
    bool        active = false;   // a model is loaded and the worker is running
    std::string backend;          // "ONNX Runtime / XNNPACK"
    std::string status;           // human text; the failure reason when !active

    Timing   timing;              // the most recent frame
    double   fps = 0.0;           // completed inferences per second
    uint64_t ran = 0, dropped = 0, failed = 0;

    /// The crop the boxes are valid for, in screen pixels. A box is only
    /// meaningful against the frame it was found in.
    Roi boxesIn;

    /// Class names by index, as the model entry declares them. The detector
    /// returns an index; a caller drawing a label needs the name, and asking the
    /// model store for it per box would be a lock per rectangle per frame.
    std::vector<std::string> classes;

    std::vector<Box> detections;
};

/// Loads whichever model the store has marked as loaded and starts running it.
///
/// A no-op returning ok when it is already running, so it is safe to call from a
/// switch that is polled every frame.
Status start();

/// Stops the worker and unloads. Safe when nothing is running, safe twice.
void stop();

bool running();

/// Whether the pipeline wants frames right now. **The capture path gates on
/// this** — not on whether a preview is on screen. While a target is being aimed
/// at the menu is hidden, and that is exactly when the frames matter.
bool wantsFrames();

/// Hands the newest capture frame to the worker. Cheap and non-blocking; safe to
/// call every frame whether or not inference is running.
void pump();

/// The latest result. Cheap — a copy of a small struct and a short vector.
Snapshot snapshot();

/// One short line for the Model page header: "12.4 ms | 31 fps", or the reason
/// there is nothing to show.
///
/// ASCII only, on purpose: this string is truncated to fit a header line, and a
/// byte-wise cut through a multi-byte separator produces a replacement glyph
/// that looks like a rendering bug rather than a layout decision.
std::string timingText();

/// The same reading with the stage split appended — "12.4 ms | 31 fps | pre 1.2
/// / infer 9.8 / post 1.4". Callers that have the width should prefer this; the
/// ones that do not fall back to timingText(), and having both as separate
/// strings means the fallback costs one measurement rather than a search.
std::string timingTextDetailed();

/// The long form for the log: the bound tensor shapes, the class count, which
/// provider answered. Describes the *model*, so it is fixed once a load
/// succeeds — not a per-frame reading.
std::string describe();

/// One line of live state, for a script or a bug report: whether it is running,
/// the backend, the counters, how many boxes the last frame produced, and the
/// stage split. Built on demand, so it costs a few string copies — call it from
/// a debug path, never from the frame loop.
std::string statusLine();

}  // namespace runtime
}  // namespace infer
}  // namespace aimbotng
