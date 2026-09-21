// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::tracking — implementation. See kalman_tracker.h.
//
//  Dependency-free 6-state constant-velocity Kalman tracker with greedy
//  per-class IoU association. The only external type used is infer::Box.
// ─────────────────────────────────────────────────────────────────────────────
#include "tracking/kalman_tracker.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

// Same tag the rest of the daemon uses, so `logcat -s AimbotNg` sees the
// tracker alongside everything else. Only a low-rate summary is printed — see
// the tick in update().
#define LOGTAG "AimbotNg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGTAG, __VA_ARGS__)

namespace aimbotng {
namespace tracking {

// (The cruise-velocity filter that used to live here was removed. Its whole
// purpose was to hand the open-loop feed-forward / lead terms a quieter
// velocity than the Kalman state's, and it could not do so without paying more
// in phase lag than it returned in jitter — see the note on TrackedTarget's
// velocity in kalman_tracker.h, and scripts/cruise_tau_objective.py, which
// reproduces the measurement if anyone wants to argue with it.)

// ── Association scoring ─────────────────────────────────────────────────────
//
// kClassBonus — added to a candidate pair's score when the track and the
//   detection agree on class. Association is class-AGNOSTIC (see
//   matchAndUpdate); this bonus is what keeps the preference, without the hard
//   partition that let one enemy own two identities.
//
// kInheritGateRadius — how much further than the normal gate a detection may be
//   from a JUST-LOST track and still be accepted as that same track returning
//   (rather than a new enemy spawning). Expressed in box-sizes, like
//   TrackerConfig::gateRadius.
constexpr float kClassBonus        = 0.06f;
constexpr float kInheritGateRadius = 2.5f;
constexpr int   kInheritMaxLostFrames = 2;

// ── Tiny fixed-size matrix (6x6 / 4x4 is all we need) ────────────────────────
namespace {

struct Mat {
    int rows = 0, cols = 0;
    std::vector<float> d;

    Mat() = default;
    Mat(int r, int c) : rows(r), cols(c), d(static_cast<size_t>(r * c), 0.0f) {}

    static Mat zeros(int r, int c) { return Mat(r, c); }
    static Mat identity(int n) {
        Mat m(n, n);
        for (int i = 0; i < n; ++i) m.at(i, i) = 1.0f;
        return m;
    }

    float& at(int r, int c) { return d[static_cast<size_t>(r * cols + c)]; }
    float  at(int r, int c) const { return d[static_cast<size_t>(r * cols + c)]; }

    Mat operator+(const Mat& o) const {
        Mat m(rows, cols);
        for (size_t i = 0; i < d.size(); ++i) m.d[i] = d[i] + o.d[i];
        return m;
    }
    Mat operator-(const Mat& o) const {
        Mat m(rows, cols);
        for (size_t i = 0; i < d.size(); ++i) m.d[i] = d[i] - o.d[i];
        return m;
    }
    Mat operator*(const Mat& o) const {
        Mat m(rows, o.cols);
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < o.cols; ++j) {
                float s = 0.0f;
                for (int k = 0; k < cols; ++k) s += at(i, k) * o.at(k, j);
                m.at(i, j) = s;
            }
        return m;
    }
    Mat transpose() const {
        Mat m(cols, rows);
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) m.at(j, i) = at(i, j);
        return m;
    }
    // Gauss-Jordan inverse (square matrices only; used on 4x4 / 6x6).
    Mat inverse() const {
        Mat a = *this;
        const int n = rows;
        Mat inv = identity(n);
        for (int col = 0; col < n; ++col) {
            int piv = col;
            for (int r = col + 1; r < n; ++r)
                if (std::fabs(a.at(r, col)) > std::fabs(a.at(piv, col))) piv = r;
            if (piv != col) {
                for (int c = 0; c < n; ++c) {
                    float t = a.at(col, c); a.at(col, c) = a.at(piv, c); a.at(piv, c) = t;
                    t = inv.at(col, c); inv.at(col, c) = inv.at(piv, c); inv.at(piv, c) = t;
                }
            }
            float d0 = a.at(col, col);
            if (d0 == 0.0f) d0 = 1e-12f;
            for (int c = 0; c < n; ++c) { a.at(col, c) /= d0; inv.at(col, c) /= d0; }
            for (int r = 0; r < n; ++r) {
                if (r == col) continue;
                const float f = a.at(r, col);
                if (f == 0.0f) continue;
                for (int c = 0; c < n; ++c) {
                    a.at(r, c)   -= f * a.at(col, c);
                    inv.at(r, c)  -= f * inv.at(col, c);
                }
            }
        }
        return inv;
    }
};

// IoU between two centre+size boxes.
float iouBox(float cx1, float cy1, float w1, float h1,
             float cx2, float cy2, float w2, float h2) {
    const float x1 = cx1 - w1 * 0.5f, y1 = cy1 - h1 * 0.5f;
    const float x2 = cx2 - w2 * 0.5f, y2 = cy2 - h2 * 0.5f;
    const float xa = std::max(x1, x2), ya = std::max(y1, y2);
    const float xb = std::min(x1 + w1, x2 + w2), yb = std::min(y1 + h1, y2 + h2);
    const float iw = std::max(0.0f, xb - xa), ih = std::max(0.0f, yb - ya);
    const float inter = iw * ih;
    if (inter <= 0.0f) return 0.0f;
    const float a1 = w1 * h1, a2 = w2 * h2;
    return inter / (a1 + a2 - inter);
}

Mat initCovariance() {
    Mat P = Mat::identity(6);
    for (int i = 0; i < 6; ++i) P.at(i, i) = 10.0f;
    return P;
}

// Constant-velocity transition: cx += vx, cy += vy.
Mat transitionA() {
    Mat A = Mat::identity(6);
    A.at(0, 4) = 1.0f;
    A.at(1, 5) = 1.0f;
    return A;
}

// Measurement matrix: observe [cx, cy, w, h].
Mat measureH() {
    Mat H = Mat::zeros(4, 6);
    H.at(0, 0) = 1.0f; H.at(1, 1) = 1.0f;
    H.at(2, 2) = 1.0f; H.at(3, 3) = 1.0f;
    return H;
}

}  // namespace

// One internal Kalman track (member of MultiTracker, visible to the .cpp only).
struct MultiTracker::KTrack {
    int   id = -1;
    Mat   x;          // 6x1 state: [cx, cy, w, h, vx, vy]
    Mat   P;          // 6x6 covariance
    int   init_frames = 0;
    bool  confirmed  = false;
    int   terminate_count = 0;
    Mat   last_valid; // 6x1 last good state (for lost-hold)
    bool  lost = false;
    int   lost_frames = 0;   // counts consecutive lost frames; reset on re-match
    int   cls = -1;
    float score = 0.0f;

    /// ── Association anchor: the last MEASURED box ──────────────────────────
    ///
    /// Identity is decided against where the target was last actually seen,
    /// never against the velocity-extrapolated prediction. See the note on
    /// TrackerConfig::gateRadius — using the prediction here let a bad velocity
    /// estimate destroy the very track it belonged to.
    float asCx = 0.0f, asCy = 0.0f, asW = 0.0f, asH = 0.0f;
};

// ── Global accessors ─────────────────────────────────────────────────────────
MultiTracker& tracker() {
    static MultiTracker t;
    return t;
}

TrackerConfig& trackerConfig() {
    static TrackerConfig c;
    return c;
}

// A track has to outlive its own prediction window, otherwise the
// predictHoldFrames slider is capped in secret by terminateFrames: the track is
// erased while it is still supposed to be extrapolating, and the slider looks
// broken for every value above terminateFrames. That cap was hit in practice —
// the default terminateFrames=5 silently made anything past "predict 4 frames"
// a no-op. The +1 is the frame on which the count reaches zero.
int MultiTracker::liveFrames() const {
    return std::max(cfg_.terminateFrames, cfg_.predictHoldFrames + 1);
}

// Monotonic, never recycled — see the note on `nextId_` in the header. The
// recycle pool that used to live here was not a memory optimisation worth
// having (one int per track, an int lasts two million tracks) and it actively
// obscured identity churn by handing the same few integers back and forth.
int MultiTracker::allocId() {
    return nextId_++;
}

void MultiTracker::reset() {
    tracks_.clear();
    out_.clear();
    nextId_ = 1;
}

// updateCruise() was here. The feed-forward and the lead now consume the Kalman
// velocity directly, and the reasoning is in kalman_tracker.h — a slower
// estimate cannot be bought without paying phase lag, and phase lag in an
// open-loop term is trailing error.

void MultiTracker::predictAll(bool countMiss) {
    const Mat A = transitionA();
    const Mat At = A.transpose();
    Mat Q = Mat::identity(6);
    for (int i = 0; i < 6; ++i) Q.at(i, i) = cfg_.processNoise;

    // `countMiss` is false on a render frame that re-consumed the previous
    // detector snapshot. The motion model still advances (the box must travel
    // with the display), but "how many detector results have we missed" must
    // only be counted once per real detector frame — otherwise the
    // predictHoldFrames window is spent twice as fast on a 120 Hz display and
    // the slider's number stops meaning what it says.
    for (auto& t : tracks_) {
        if (!t.lost) {
            t.x = A * t.x;
            t.P = A * t.P * At + Q;
        } else {
            if (countMiss) ++t.lost_frames;
            // Forward-predict using vx,vy for the first predictHoldFrames missed
            // frames (covers brief occlusions: wall, smoke, foliage), then freeze
            // at last_valid so a stale velocity estimate does not drift the aim
            // across the screen on a longer miss. predictHoldFrames=0 disables
            // forward-prediction entirely and falls back to instant freeze.
            if (cfg_.predictHoldFrames > 0 && t.lost_frames <= cfg_.predictHoldFrames) {
                t.x = A * t.x;
                t.P = A * t.P * At + Q;
            } else {
                t.x = t.last_valid;
                t.x.at(4, 0) = 0.0f;
                t.x.at(5, 0) = 0.0f;
                t.P = t.P + Q;
            }
        }
    }
}

void MultiTracker::matchAndUpdate(const std::vector<infer::Box>& dets) {
    const Mat H  = measureH();
    const Mat Ht = H.transpose();
    Mat R = Mat::identity(4);
    for (int i = 0; i < 4; ++i) R.at(i, i) = cfg_.measNoise;
    Mat I6 = Mat::identity(6);

    // ── Association: ONE pool, every track against every detection ───────────
    //
    // This used to be partitioned by class — tracks and detections grouped into
    // independent per-class sub-problems. That partition is where "只有一个目标，
    // id有时候都变" comes from, and it is worth spelling out because the fault is
    // upstream of the tracker.
    //
    // NMS in the decoder is per class deliberately (postprocess.cpp: "a person
    // overlapping a car is two findings, not one"), so for a head/body model ONE
    // ENEMY LEGITIMATELY PRODUCES TWO BOXES in the same frame — a head and a
    // body — and both survive. A hard per-class partition then guarantees they
    // become two independent tracks with two different ids. The aim loop picks
    // whichever is nearer the crosshair, and which of the two is nearer
    // flip-flops as the target moves: one enemy, two ids, alternating. Every flip
    // is also a `targetChanged` to the controller, so the integrator is wiped
    // several times a second — which is a second, independent reason the aim
    // cannot hold the steady-state trim that removes the trailing error.
    //
    // One pool fixes the identity half: the two boxes now compete for tracks
    // instead of being assigned to separate ones, and the class-match bonus
    // below keeps a head box attached to a head track when geometry is tied.
    // The other half — that the aim must not read a head and a body as two
    // different enemies — belongs in the selection, and is handled there (see
    // the sticky pick in syncAimPage).
    //
    // Greedy association on a combined score, ranked by IoU with a
    // normalised-centre-distance fallback so the loop still has a gradient when
    // the boxes do not overlap at all.
    //
    // Why the fallback exists — and why it is the fix for "it cannot follow a
    // fast target": IoU alone collapses exactly when it is needed most. A target
    // crossing the panel at ~60 px/frame against a 100 px box yields IoU ≈ 0.2
    // between consecutive detector frames, which is under the old 0.5 threshold,
    // so EVERY frame failed to match. The track was therefore deleted and
    // re-created each frame, and a new track starts with velocity ZERO and
    // position = the raw detection. Track id churn, jittery box, and — the
    // expensive part — the velocity that every lead and feed-forward term reads
    // was permanently 0. No amount of slider tuning can fix a signal that is
    // identically zero. The centre-distance term keeps a fast target attached to
    // its own track, so the velocity estimate has time to converge.
    //
    // The gate is what keeps the fallback safe: a detection further than
    // gateRadius box-sizes from the track's ANCHOR is rejected outright, so
    // "nearest remaining box" cannot match across the screen and swap two
    // different enemies. The anchor is the last MEASURED box, not the
    // velocity-extrapolated one — see the note on TrackerConfig::gateRadius for
    // why that specific change matters.
    std::set<int> matchedTracks, matchedDets;

    while (true) {
        int bestT = -1, bestD = -1;
        float bestScore = cfg_.iouThreshold;  // strictly greater
        for (size_t ti_i = 0; ti_i < tracks_.size(); ++ti_i) {
            if (matchedTracks.count(static_cast<int>(ti_i))) continue;
            const auto& tr = tracks_[ti_i];
            const float tcx = tr.asCx, tcy = tr.asCy;   // anchor, NOT prediction
            const float tw  = tr.asW, th  = tr.asH;
            const float scale = 0.5f * (tw + th);       // box-size yardstick
            for (size_t dj_j = 0; dj_j < dets.size(); ++dj_j) {
                if (matchedDets.count(static_cast<int>(dj_j))) continue;
                const auto& d = dets[dj_j];
                const float dcx = d.cx() - tcx, dcy = d.cy() - tcy;
                const float dist = std::sqrt(dcx * dcx + dcy * dcy);
                if (scale > 1.0f && dist > cfg_.gateRadius * scale) continue;
                const float iou = iouBox(tcx, tcy, tw, th,
                                        d.cx(), d.cy(), d.w(), d.h());
                // 0 = one centre on the other, 1 = exactly one box-size apart.
                const float nd = (scale > 1.0f) ? (dist / scale) : 0.0f;
                float score = iou + 0.3f * std::max(0.0f, 1.0f - nd);
                // Class is a PREFERENCE, not a partition: it breaks ties toward
                // the track's own class without ever forbidding a cross-class
                // match, so a detection whose label flickers stays attached.
                if (d.cls == tr.cls) score += kClassBonus;
                if (score > bestScore) {
                    bestScore = score; bestT = static_cast<int>(ti_i);
                    bestD = static_cast<int>(dj_j);
                }
            }
        }
        if (bestT < 0) break;

        // Measurement for the matched detection.
        const auto& d = dets[bestD];
        Mat z(4, 1);
        z.at(0, 0) = d.cx();
        z.at(1, 0) = d.cy();
        z.at(2, 0) = d.w();
        z.at(3, 0) = d.h();

        auto& t = tracks_[bestT];
        Mat y = z - H * t.x;  // 4x1 innovation
        const float innov = std::sqrt(y.at(0, 0) * y.at(0, 0) + y.at(1, 0) * y.at(1, 0));
        if (innov > cfg_.innovThresh) {
            // Outlier (camera moved / box jump): snap to measurement.
            t.x.at(0, 0) = z.at(0, 0); t.x.at(1, 0) = z.at(1, 0);
            t.x.at(2, 0) = z.at(2, 0); t.x.at(3, 0) = z.at(3, 0);
            t.x.at(4, 0) = 0.0f;       t.x.at(5, 0) = 0.0f;
        } else {
            Mat S = H * t.P * Ht + R;          // 4x4
            Mat K = t.P * Ht * S.inverse();    // 6x4
            t.x = t.x + K * y;
            t.P = (I6 - K * H) * t.P;
        }
        t.last_valid = t.x;
        // Anchor the identity on the measurement just consumed.
        t.asCx = d.cx(); t.asCy = d.cy(); t.asW = d.w(); t.asH = d.h();
        t.cls    = d.cls;
        t.score  = d.score;
        t.lost   = false;
        t.lost_frames = 0;          // reset on re-match
        t.terminate_count = liveFrames();
        if (!t.confirmed) {
            if (++t.init_frames >= cfg_.confirmFrames) t.confirmed = true;
        }
        matchedTracks.insert(bestT);
        matchedDets.insert(bestD);
    }

    // Unmatched detections either RETURN to a track we already own, or spawn.
    //
    // The inheritance step is what stops the churn at its source. A detection
    // that failed the greedy pass because its box JUMPED — a camera pan, a fast
    // flick, one frame of bad localisation — is the same enemy, and minting a
    // fresh id for it leaves the old track alive as a `lost` duplicate for the
    // next `terminateFrames` frames. Two tracks at one enemy is exactly the
    // state in which the nearest-target pick alternates between ids. So: a
    // just-lost track whose anchor is inside the relaxed inherit gate adopts
    // this detection and keeps its id. The relaxed gate is the only place a
    // detection may match from further away than gateRadius, and it is bounded
    // to tracks matched within the last couple of detector results, so a
    // long-dead track can never steal a genuinely new enemy.
    for (size_t dj_j = 0; dj_j < dets.size(); ++dj_j) {
        if (matchedDets.count(static_cast<int>(dj_j))) continue;
        const auto& d = dets[dj_j];

        int inherited = -1;
        for (size_t k = 0; k < tracks_.size(); ++k) {
            if (matchedTracks.count(static_cast<int>(k))) continue;
            const auto& tr = tracks_[k];
            if (tr.lost_frames > kInheritMaxLostFrames) continue;
            const float scale = 0.5f * (tr.asW + tr.asH);
            if (scale <= 1.0f) continue;
            const float dx = d.cx() - tr.asCx, dy = d.cy() - tr.asCy;
            if (std::sqrt(dx * dx + dy * dy) > kInheritGateRadius * scale) continue;
            inherited = static_cast<int>(k);
            break;
        }

        if (inherited >= 0) {
            // Re-acquisition: same identity, state re-seeded from the
            // measurement with velocity 0 (nothing is known about the motion
            // across the gap, and a stale velocity is worse than none).
            auto& t = tracks_[inherited];
            t.x = Mat(6, 1);
            t.x.at(0, 0) = d.cx(); t.x.at(1, 0) = d.cy();
            t.x.at(2, 0) = d.w();  t.x.at(3, 0) = d.h();
            t.x.at(4, 0) = 0.0f;   t.x.at(5, 0) = 0.0f;
            t.P = initCovariance();
            t.last_valid = t.x;
            t.asCx = d.cx(); t.asCy = d.cy(); t.asW = d.w(); t.asH = d.h();
            t.cls = d.cls;
            t.score = d.score;
            t.lost = false;
            t.lost_frames = 0;
            t.terminate_count = liveFrames();
            matchedTracks.insert(inherited);
            matchedDets.insert(static_cast<int>(dj_j));
            continue;
        }

        KTrack t;
        t.id = allocId();
        t.x = Mat(6, 1);
        t.x.at(0, 0) = d.cx(); t.x.at(1, 0) = d.cy();
        t.x.at(2, 0) = d.w();  t.x.at(3, 0) = d.h();
        t.x.at(4, 0) = 0.0f;  t.x.at(5, 0) = 0.0f;
        t.P = initCovariance();
        t.last_valid = t.x;
        t.asCx = d.cx(); t.asCy = d.cy(); t.asW = d.w(); t.asH = d.h();
        t.cls = d.cls;
        t.score = d.score;
        t.init_frames = 1;
        t.confirmed = (cfg_.confirmFrames <= 1);
        t.terminate_count = liveFrames();
        t.lost = false;
        tracks_.push_back(std::move(t));
    }

    // Unmatched tracks: mark lost, count down, drop when expired.
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (matchedTracks.count(static_cast<int>(i)) == 0) {
            tracks_[i].lost = true;
            tracks_[i].terminate_count -= 1;
        }
    }
    for (size_t i = 0; i < tracks_.size();) {
        if (tracks_[i].terminate_count <= 0) {
            tracks_.erase(tracks_.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // NOTE: emitTracks() is NOT called here any more. The cruise velocity has to
    // be advanced from the freshly corrected Kalman state before the published
    // tracks are filled in, and that step is common to both the fresh and the
    // repeat path — so update() sequences it. See there.
}

// Publish the confirmed tracks (including lost-but-held ones) for the aim loop
// and the HUD. Split out of matchAndUpdate() so a render frame that carries no
// fresh detector result can still publish the freshly predicted positions
// without touching any track state.
void MultiTracker::emitTracks() {
    out_.clear();
    for (const auto& t : tracks_) {
        if (!t.confirmed) continue;
        TrackedTarget o;
        o.id = t.id;
        o.cx = t.x.at(0, 0); o.cy = t.x.at(1, 0);
        o.w  = t.x.at(2, 0); o.h  = t.x.at(3, 0);
        o.vx = t.x.at(4, 0); o.vy = t.x.at(5, 0);
        o.score = t.score;
        o.cls = t.cls;
        o.confirmed = t.confirmed;
        o.lost = t.lost;
        out_.push_back(o);
    }
}

void MultiTracker::update(const std::vector<infer::Box>& dets,
                          bool freshMeasurement) {
    cfg_ = trackerConfig();
    // One motion-model step per RENDER frame, always. This is what keeps the
    // published centre in step with the display: at 120 Hz the box has to move
    // twice between two 60 Hz detector results, and it does — each call runs
    // the constant-velocity transition, so (vx, vy) are px per RENDER frame.
    //
    // Association, however, runs only on a fresh result. Re-associating the
    // same snapshot pulls the position back to a measurement that is already
    // one frame old — fighting the prediction, and leaving the velocity
    // estimate ringing instead of converging. The measured cost (lag 9…41 px
    // and vx ±22 %) is in the header note on freshMeasurement.
    predictAll(freshMeasurement);
    if (freshMeasurement) {
        matchAndUpdate(dets);
    }

    // Publish. This has to run on EVERY render frame, fresh measurement or not —
    // the motion model advanced above, so the box the aim reads must move with
    // it even when the detector produced nothing new.
    emitTracks();

    // Low-rate observability — one line per 240 fresh results, about 4 s at
    // 60 Hz. The tracker has no other output a user can see, so a velocity
    // pinned near zero (association never matching, so every frame spawns a
    // fresh track that starts from rest) is indistinguishable from "the aim is
    // just bad today". That ambiguity is the expensive part. With this line,
    // `vx`/`vy` are readable straight off the device via
    // `logcat -s AimbotNg | grep tracker`.
    //
    // The id LIST is printed rather than just the first track's id, because the
    // single most useful question about this tracker is "how many identities am
    // I carrying for how many enemies". One enemy and three ids on this line is
    // the churn; one enemy and one id is correct. `vx`/`vy` are the Kalman
    // velocity — the one and only velocity, since the feed-forward, the lead and
    // the closed loop all read it — so if both sit near zero while the target is
    // plainly moving, the association is never matching and no gain will help.
    static int s_logTick = 0;
    if (freshMeasurement && ++s_logTick >= 240) {
        s_logTick = 0;
        if (out_.empty()) {
            LOGI("tracker: no confirmed tracks");
        } else {
            const TrackedTarget& t0 = out_[0];
            char ids[96];
            int  off = 0;
            for (size_t i = 0; i < out_.size() && off < 80; ++i) {
                off += snprintf(ids + off, sizeof(ids) - static_cast<size_t>(off),
                                "%s%d%s", i ? "," : "", out_[i].id,
                                out_[i].lost ? "L" : "");
            }
            ids[off < (int)sizeof(ids) ? off : (int)sizeof(ids) - 1] = '\0';
            LOGI("tracker: %u track(s) [%s] #%d cls=%d cx=%.1f cy=%.1f "
                 "vx=%.2f vy=%.2f lost=%d",
                 static_cast<unsigned>(out_.size()), ids, t0.id, t0.cls,
                 t0.cx, t0.cy, t0.vx, t0.vy, t0.lost ? 1 : 0);
        }
    }
}

}  // namespace tracking
}  // namespace aimbotng
