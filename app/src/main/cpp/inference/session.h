// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — the thing the rest of the daemon actually talks to
//
//  No other inference header should be included outside this module. The
//  renderer, the capture thread, the shell server and the JNI surface all go
//  through a Session, which means the runtime layer can be rewritten, an engine
//  swapped, or a whole backend deleted, without any of them changing.
//
//  Two responsibilities are worth calling out because they are why this class
//  exists rather than the callers using an Engine directly:
//
//  * **Frames are dropped, not queued.** Inference on a crop takes milliseconds
//    and the capture side produces frames at screen rate. A pipeline that keeps
//    up with the producer ends up aiming at where the target was half a second
//    ago, which looks exactly like bad aim and is not. The queue holds one frame
//    — the newest — and counts what it threw away so the number is visible.
//
//  * **wantsFrames() is the capture side's switch.** This is the fix for a
//    problem the capture path has right now: its liveness is tied to the menu
//    being on screen, which is correct while the preview is the only consumer
//    and exactly backwards once inference is. While a target is being aimed at,
//    the menu is hidden and the frames are the thing that matters.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/engine.h"
#include "inference/inference_types.h"

#include <functional>
#include <memory>

namespace aimbotng {
namespace infer {

/// Owns one loaded model and one worker thread, and is the only inference type
/// the rest of the daemon needs to know about.
///
/// Not thread-safe by accident: every public method may be called from the
/// render thread, the JNI thread or the capture thread, and each does its own
/// locking. The result callback runs on the worker thread — it is the one place
/// a caller has to think about which thread it is on.
class Session {
public:
    /// Called on the worker thread once per completed frame, in order, never
    /// concurrently with itself. Keep it short: the worker is not doing anything
    /// else while it runs, so a long callback is a frame rate limit.
    ///
    /// `roi` is the crop the boxes are in frame coordinates for.
    using ResultFn = std::function<void(const std::vector<Box>&, const Roi&,
                                        const Timing&)>;

    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Loads `spec` on the pair. Replaces whatever was loaded, and does so
    /// without tearing the worker down — a model swap is a Settings action and
    /// should not stall the frame loop for the duration of a graph compile.
    ///
    /// Returns the same Status the engine would have returned. A failed load
    /// leaves no engine installed, so a broken model cannot be mistaken for a
    /// model that finds nothing.
    Status load(const ModelSpec&, Runtime, Ep);

    /// Unloads and stops the worker. Safe when nothing is loaded, safe twice.
    void unload();

    /// Whether a model is installed and the worker is running.
    bool running() const;

    // ── What to run on ──────────────────────────────────────────────────────

    /// Sets the crop. Applies from the next frame; already-queued frames keep
    /// the crop they were submitted with, so a box never comes back in the
    /// coordinates of a rect it was not computed for.
    void setRoi(const Roi&);

    /// Confidence threshold, applied in post-processing. Cheap enough to change
    /// per frame — it does not reload the model, which is what makes it usable
    /// as a live control in the menu.
    void setConfidence(float);

    // ── The frame path ──────────────────────────────────────────────────────

    /// Hands a frame over. **Never blocks**, and never copies: it parks the view
    /// and returns. If the worker has not picked up the previous frame, that one
    /// is discarded and the drop counter goes up.
    ///
    /// Legal to call with an invalid FrameView — it is counted and ignored,
    /// because the alternative is every capture-side caller remembering to check
    /// and one of them not doing it.
    ///
    /// Called from the capture thread. It must not stall it, which is the whole
    /// reason this is not `run()`.
    void submit(const FrameView&);

    /// Whether the pipeline wants frames at all right now: a model is loaded and
    /// a consumer is registered. **This is what the capture path should gate on**
    /// — not on whether a preview is on screen.
    bool wantsFrames() const;

    /// Registers the result callback. Set it before load(); passing an empty
    /// function means results are computed and discarded, which is a legitimate
    /// way to measure a backend's speed without wiring a consumer.
    void onResult(ResultFn);

    // ── What it cost ────────────────────────────────────────────────────────

    struct Stats {
        uint64_t submitted = 0;   // submit() calls that carried a valid frame
        uint64_t ran       = 0;   // completed inferences
        uint64_t dropped   = 0;   // superseded before they ran
        uint64_t failed    = 0;   // loaded, ran, and returned a bad status
        uint64_t invalid   = 0;   // submit() with an unusable FrameView

        /// Completed inferences per second, over a short trailing window.
        /// Zero before there is enough history to mean anything.
        double fps = 0.0;

        Timing last;                          // the most recent run
        Timing worst;                         // the worst single run seen
        std::string activeName;               // what actually ran
    };

    Stats stats() const;

    /// One line, log-shaped: the state, the rate, the drop count, the backend.
    /// The drop count is in it on purpose — a pipeline dropping most of its
    /// frames is not broken, but every consumer eventually needs to know.
    std::string statusLine() const;

    /// The loaded engine's own description: bound tensor shapes, class count,
    /// which execution provider actually answered. Empty when nothing is loaded.
    /// Static — it describes the model, not the run.
    std::string describe() const;

    /// The loaded model's input element type as a short label ("fp32" / "fp16" /
    /// …), or "" when nothing is loaded. Used to populate the model list.
    std::string inputDtypeName() const;

    /// Why the most recent run failed, or empty when it did not.
    ///
    /// Separate from the Stats counters on purpose: a counter can say that
    /// every frame failed, and only this can say why. A pipeline that fails
    /// silently is the one failure mode that looks exactly like working.
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace infer
}  // namespace aimbotng
