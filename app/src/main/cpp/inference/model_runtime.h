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
#include "inference/model_store.h"   // model::Engine — the menu's vocabulary

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

/// Loads the model and initialises the backend (delegate compile for HTP/NPU,
/// graph optimisation for ORT, interpreter create for LiteRT, etc.) but does
/// NOT start the worker consuming frames. Lets the caller pay the 1–3 second
/// cold-start at a moment the user does not see — typically right after they
/// tap Load on the Model page — so a later arm() is essentially instant.
///
/// Safe to call from a background thread; the worker thread it unblocks is
/// owned by the session. Returns ok if the same model is already prepared.
Status prepare();

/// Lets the worker start consuming frames. Cheap because the engine is
/// already prepared: this only flips the `wants` flag the capture side gates
/// on. If prepare() has not finished yet, arm() records the intent and
/// prepare() honours it on completion.
///
/// No-op if already armed.
void arm();

/// Stops the worker consuming frames. The engine stays loaded, so a later
/// arm() is again instant. No-op if already disarmed.
void disarm();

/// Whether the engine is loaded and ready to run frames.
///
/// Deliberately separate from `model::loadedId()`, which only says which entry
/// carries the loaded flag in the store — a *persistent* choice, restorable
/// from config.json, that says nothing about whether anything has been built in
/// memory yet. The held-finger path has to ask THIS question before deciding it
/// can run, because that is the one that knows whether a compile is still owed.
bool engineReady();

/// Starts `prepare()` on a detached background thread if it is not already
/// running, and returns immediately.
///
/// This is the bridge between the two paths into inference. The Model page pays
/// the compile when the user flips its switch — they asked for it, they are
/// watching, and the Compiling card explains the wait. The held-finger path has
/// no switch and no card: the user just puts a finger down and expects
/// detection. Without this, that path calls arm() against an engine that was
/// never built, arm() records `pendingArm` and returns, and the finger does
/// nothing at all — for as many presses as it takes for something else to
/// trigger a prepare().
///
/// Idempotent: a second call while a compile is in flight is a no-op, so a
/// per-frame caller stays cheap. The result surfaces through the normal
/// snapshot/status path, not through the return value, because there is nothing
/// useful a caller can do with a failure it cannot wait for.
void prepareAsync();

/// Marks the built engine as no longer matching the store.
///
/// The Settings dialog lets the user change the *engine* (backend), the CPU
/// thread count and the HTP performance vote of the model that is currently
/// loaded — but those are compile-time inputs, not runtime knobs. The delegate
/// is built from them, and the graph is optimised and quantised for them; there
/// is no way to change one on a live interpreter.
///
/// `prepare()`'s fast path is `g_engineReady && g_loadedId == id`, which cannot
/// see any of that: the id did not change, so the fast path keeps answering
/// "already prepared" and the next arm() runs on the old backend. The user
/// changes HTP to CPU, taps Save, holds the inference area, and gets the same
/// timings as before with no hint as to why. So this has to be called by
/// whoever mutates those fields, and it is the ONLY thing that makes such a
/// change take effect.
///
/// Disarms as well: the engine the worker is feeding frames to is about to be
/// replaced, and letting it keep consuming frames during the swap would submit
/// a frame to a half-torn-down session. Cheap and idempotent — the next
/// prepare() (from the Model page's switch, or from a held finger via
/// prepareAsync()) rebuilds.
void invalidateEngine();

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

/// Whether the backend an engine needs is present and loadable on this device.
///
/// Used by the menu to grey a row out instead of letting a tap fail seconds
/// later. Asking costs a probe the first time (for NeuroPilot, a dlopen of the
/// vendor library) and nothing after that — epAvailable() caches per pair.
bool engineAvailable(model::Engine engine);

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
