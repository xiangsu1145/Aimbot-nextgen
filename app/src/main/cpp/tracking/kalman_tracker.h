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
};

/// Tuning knobs. The aim Settings page writes these via trackerConfig().
struct TrackerConfig {
    float iouThreshold    = 0.5f;  // min IoU to associate a det with a track
    int   confirmFrames   = 1;     // frames before a new track is output
    int   terminateFrames = 5;     // missed frames before a track is dropped
    float processNoise    = 0.1f;  // position process noise
    float measNoise       = 0.1f;  // position measurement noise
    float innovThresh     = 20.0f; // px: innovation above this => snap to measurement
};

class MultiTracker {
public:
    /// Advance the tracker by one frame. `dets` are screen-pixel boxes from the
    /// detector snapshot. Call once per render frame.
    void update(const std::vector<infer::Box>& dets);

    /// Stable, smoothed tracks from the most recent update (confirmed only).
    const std::vector<TrackedTarget>& tracks() const { return out_; }

    /// Drop every track (e.g. when the model stops producing detections).
    void reset();

private:
    TrackerConfig cfg_;
    std::vector<TrackedTarget> out_;

    struct KTrack;
    std::vector<KTrack> tracks_;
    int   nextId_   = 1;
    std::vector<int> freeIds_;

    int  allocId();
    void predictAll();
    void matchAndUpdate(const std::vector<infer::Box>& dets);
};

/// Global tracker instance (stateful across frames — that is the whole point).
MultiTracker& tracker();

/// Global, mutable tracker tuning. The Settings page writes into this; the
/// tracker reads it at the top of each update().
TrackerConfig& trackerConfig();

}  // namespace tracking
}  // namespace aimbotng
