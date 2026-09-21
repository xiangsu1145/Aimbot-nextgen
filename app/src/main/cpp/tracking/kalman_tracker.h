// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::tracking — multi-target Kalman tracker (no Eigen / OpenCV)
//
//  Ports the desktop DsAi `kaerman.hpp` idea into this project: a per-frame
//  tracker that turns the *jittering* per-frame detection boxes into a small
//  set of stable tracks, each with a smoothed centre, a size, a persistent
//  track id, and a per-frame velocity (vx, vy). That buys three things the
//  aim loop currently lacks:
//
//    * jitter removal  — the box the aim chases no longer twitches every frame
//    * stable identity — track id is stable across frames, so the aim does not
//      hop between two adjacent enemies
//    * velocity        — vx,vy feed the "提前量" (lead) slider so a moving
//      target is aimed at where it WILL be, not where it was
//
//  The filter is a constant-velocity Kalman (state [cx, cy, w, h, vx, vy]).
//  When a track is briefly lost it HOLDS its last position (velocity zeroed)
//  for `terminateFrames` frames, instead of dropping — so a 1-frame dropout
//  or a momentary occlusion does not break the lock.
//
//  Matching is greedy IoU, per class (target counts are tiny, so the full
//  Hungarian solver from the original is unnecessary). Everything is plain
//  fixed-size matrix math in kalman_tracker.cpp.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/inference_types.h"

#include <cstdint>
#include <vector>

namespace aimbotng {
namespace tracking {

/// One tracked target, in screen pixels, ready for the aim loop to consume.
struct TrackedTarget {
    int   id        = -1;
    float cx = 0, cy = 0;          // smoothed centre
    float w  = 0, h  = 0;          // smoothed size
    float vx = 0, vy = 0;          // velocity (px per render-frame)
    float score    = 0.0f;
    int   cls      = -1;
    bool  confirmed = false;        // survived the confirm-frame count
    bool  lost      = false;        // no measurement for it this frame(s)

    /// ── One velocity, and why there is only one ────────────────────────────
    ///
    /// `vx,vy` above are the Kalman state's velocity, tuned for POSITION
    /// accuracy — the filter is deliberately quick to correct (K_pos = 0.82 /
    /// K_vel = 0.42, so vx rings by ±20-40 %).
    ///
    /// The open-loop terms — the 速度前馈 feed-forward and the 提前量 lead —
    /// multiply that velocity straight into the finger, so its ripple becomes
    /// finger motion. The obvious defence is a SECOND, slower estimate of the
    /// same quantity (a ~100 ms low-pass) handed to those two terms only,
    /// leaving the closed loop on the quick one.
    ///
    /// That was built, measured, and removed. It is a Bode trade with no free
    /// point: a smoother IS a lag, and a lag in an OPEN-LOOP term is trailing
    /// error that nothing downstream can undo. Over a realistic motion suite
    /// (walk / strafe / sprint, twitch-to-full reversals, smooth weaves) a
    /// 100 ms second pole cost +41…+81 px of extra error on a smooth weave —
    /// half a body width — to remove ~1 px/step of finger ripple, which is
    /// sub-pixel in view space once the game's sensitivity is applied. Its
    /// free point, where the cost drops under a tenth of a box, is tau <= 8 ms,
    /// i.e. no filter at all. And `vx,vy` is ALREADY a low-pass: it is the
    /// Kalman state with K_vel = 0.42, tau ~= 20 ms at 120 Hz, so a second pole
    /// on top of it is double filtering.
    ///
    /// If the feed-forward ever does feel buzzy on the device, MOVE this pole
    /// (TrackerConfig / k_vel) rather than adding another, and re-measure with
    /// scripts/cruise_tau_objective.py before believing the trade.
    ///
};

/// Tuning knobs. The aim Settings page writes these via trackerConfig().
struct TrackerConfig {
    float iouThreshold    = 0.15f; // min association score to pair a det with a track
    int   confirmFrames   = 1;     // frames before a new track is output
    int   terminateFrames = 5;     // missed frames before a track is dropped
    /// Missed frames to keep Kalman-forward-prediction; after this the track
    /// freezes at its last valid position. NOTE: this also *extends* the
    /// track's lifetime — a track is never dropped while it is still inside
    /// its prediction window, because a slider that says "predict 20 frames"
    /// and then silently does nothing past terminateFrames is a lie. The
    /// effective lifetime is max(terminateFrames, predictHoldFrames + 1).
    int   predictHoldFrames = 3;
    float processNoise    = 0.1f;  // position process noise
    float measNoise       = 0.1f;  // position measurement noise
    float innovThresh     = 20.0f; // px: innovation above this => snap to measurement
    /// Association gate, in units of the box's own size: a detection whose
    /// centre is further than `gateRadius * 0.5*(w+h)` from the track's
    /// ANCHOR centre can never be matched to it. This is what keeps the
    /// fallback score honest — without a gate, "nearest remaining box" happily
    /// matches across the screen and silently swaps two enemies.
    ///
    /// The anchor is the LAST MEASURED box, not the velocity-extrapolated one,
    /// and that distinction is the fix for a nasty self-inflicted failure. The
    /// gate and the IoU used to be computed against `x + v` — the prediction —
    /// so a track whose velocity estimate was momentarily wrong (which is
    /// exactly what a fast target, a fresh track, or a camera pan produces)
    /// moved its own goalposts out from under the detection, failed to match,
    /// and was deleted and re-created under a NEW id. The prediction therefore
    /// caused the very identity churn that the prediction was added to prevent.
    /// The reference tracker never had the bug because its state transition
    /// deliberately does not move the box (kaerman.hpp sets A(0,4)=0 and
    /// A(4,4)=0, so X_prior IS the last posterior and association is always
    /// against where the target was last actually SEEN). We keep the prediction
    /// for the published centre — the aim needs it — and use the measurement for
    /// identity.
    float gateRadius      = 1.5f;
};

class MultiTracker {
public:
    /// Advance the tracker by one render frame. `dets` are screen-pixel boxes
    /// from the detector snapshot.
    ///
    /// `freshMeasurement` says whether `dets` is a snapshot this tracker has
    /// not seen yet. It MUST be false when the caller is re-passing the same
    /// result it passed last frame, which is the normal case here: the HUD
    /// render loop runs at 120 Hz while the detector completes at ~60 Hz, so
    /// every second call carries an identical snapshot.
    ///
    /// Feeding a measurement twice is not harmless, and the damage is
    /// measurable. The filter predicts the next centre from (vx, vy) and then
    /// corrects toward the measurement; a repeat corrects it back toward a
    /// position that is already one frame old. Simulated with this tracker's
    /// own steady-state gains (K_pos = 0.82, K_vel = 0.42) against a target
    /// moving 100 px per detector frame on a 120 Hz render loop:
    ///
    ///     re-associating repeats        only fresh results
    ///     ------------------------      ------------------
    ///     lag 9…41 px, alternating      lag ≈ 0 px
    ///     vx 39…61 px/frame             vx 49.8 px/frame
    ///     (true value: 50)              (true value: 50)
    ///
    /// Two failures fall out of that. The published centre is dragged
    /// backwards — "the detection box is half a body behind", a lag baked into
    /// the position the aim chases, which no gain slider can remove. And the
    /// velocity estimate rings by ±22 % instead of converging — which is what
    /// "turn the feed-forward up and it just shakes" looks like from inside,
    /// because lead and feed-forward both multiply (vx, vy), so their ripple
    /// becomes finger motion.
    ///
    /// Prediction still advances every render frame (that is what keeps the box
    /// smooth and in step with the display between detector frames); only
    /// association, the missed-frame counters and track creation are gated.
    ///
    /// There is deliberately no `dtSec` parameter. The motion model advances
    /// exactly one render frame per call and (vx, vy) are px per RENDER frame,
    /// so a 120 Hz frame and a 60 Hz frame are each "one step" and the
    /// transition wants no timestep. The one thing that did need one — the
    /// cruise velocity filter — is gone (see the note on TrackedTarget above),
    /// and a parameter kept alive only for a removed feature is a trap: the next
    /// reader assumes it is load-bearing and threads it through new code.
    void update(const std::vector<infer::Box>& dets,
                bool freshMeasurement = true);

    /// Stable, smoothed tracks from the most recent update (confirmed only).
    const std::vector<TrackedTarget>& tracks() const { return out_; }

    /// Drop every track (e.g. when the model stops producing detections).
    void reset();

private:
    TrackerConfig cfg_;
    std::vector<TrackedTarget> out_;

    struct KTrack;
    std::vector<KTrack> tracks_;
    /// Monotonic. Ids are deliberately NOT recycled.
    ///
    /// The original code kept a LIFO `freeIds_` pool (as does the reference
    /// tracker), and that pool hides exactly the fault the user reported. With
    /// one enemy that briefly splits into two tracks — a class flip, a missed
    /// association, a head box that blinks out — the dying track's id goes back
    /// on the pool and the next new track pops it straight off again, so the id
    /// returns to the same number and the churn becomes invisible in the one
    /// place it should be obvious. The observed pattern of a single target's id
    /// alternating 1,2,1,2 is that pool handing the same two integers back and
    /// forth. Never reusing an id costs nothing (an int lasts ~2 million
    /// tracks) and makes "the id changed" mean what it says.
    int   nextId_   = 1;

    int  allocId();
    void predictAll(bool countMiss);
    void matchAndUpdate(const std::vector<infer::Box>& dets);
    void emitTracks();

    /// Frames an unmatched track survives before it is dropped. Always covers
    /// the whole prediction window — see the note on predictHoldFrames.
    int  liveFrames() const;
};

/// Global tracker instance (stateful across frames — that is the whole point).
MultiTracker& tracker();

/// Global, mutable tracker tuning. The Settings page writes into this; the
/// tracker reads it at the top of each update().
TrackerConfig& trackerConfig();

}  // namespace tracking
}  // namespace aimbotng
