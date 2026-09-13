// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::tracking — implementation. See kalman_tracker.h.
//
//  Dependency-free 6-state constant-velocity Kalman tracker with greedy
//  per-class IoU association. The only external type used is infer::Box.
// ─────────────────────────────────────────────────────────────────────────────
#include "tracking/kalman_tracker.h"

#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace aimbotng {
namespace tracking {

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
    int   cls = -1;
    float score = 0.0f;
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

int MultiTracker::allocId() {
    if (!freeIds_.empty()) {
        const int id = freeIds_.back();
        freeIds_.pop_back();
        return id;
    }
    return nextId_++;
}

void MultiTracker::reset() {
    tracks_.clear();
    out_.clear();
    nextId_ = 1;
    freeIds_.clear();
}

void MultiTracker::predictAll() {
    const Mat A = transitionA();
    const Mat At = A.transpose();
    Mat Q = Mat::identity(6);
    for (int i = 0; i < 6; ++i) Q.at(i, i) = cfg_.processNoise;

    for (auto& t : tracks_) {
        if (!t.lost) {
            t.x = A * t.x;
            t.P = A * t.P * At + Q;
        } else {
            // Hold last position, velocity zeroed.
            t.x = t.last_valid;
            t.x.at(4, 0) = 0.0f;
            t.x.at(5, 0) = 0.0f;
            t.P = t.P + Q;
        }
    }
}

void MultiTracker::matchAndUpdate(const std::vector<infer::Box>& dets) {
    const Mat H  = measureH();
    const Mat Ht = H.transpose();
    Mat R = Mat::identity(4);
    for (int i = 0; i < 4; ++i) R.at(i, i) = cfg_.measNoise;
    Mat I6 = Mat::identity(6);

    // Group track indices and detection indices by class.
    std::map<int, std::vector<int>> trackByCls, detByCls;
    for (size_t i = 0; i < tracks_.size(); ++i) trackByCls[tracks_[i].cls].push_back((int)i);
    for (size_t j = 0; j < dets.size(); ++j)   detByCls[dets[j].cls].push_back((int)j);

    std::set<int> matchedTracks, matchedDets;
    std::set<int> allClasses;
    for (auto& p : trackByCls) allClasses.insert(p.first);
    for (auto& p : detByCls)   allClasses.insert(p.first);

    for (int cls : allClasses) {
        auto ti = trackByCls.find(cls);
        auto dj = detByCls.find(cls);
        const std::vector<int>& tIdx = (ti != trackByCls.end()) ? ti->second : std::vector<int>{};
        const std::vector<int>& dIdx = (dj != detByCls.end()) ? dj->second : std::vector<int>{};

        // Greedy best-IoU association.
        while (true) {
            int bestT = -1, bestD = -1;
            float bestIoU = cfg_.iouThreshold;  // strictly greater
            for (int ti_i : tIdx) {
                if (matchedTracks.count(ti_i)) continue;
                const auto& tr = tracks_[ti_i];
                const float tcx = tr.x.at(0, 0), tcy = tr.x.at(1, 0);
                const float tw  = tr.x.at(2, 0), th = tr.x.at(3, 0);
                for (int dj_j : dIdx) {
                    if (matchedDets.count(dj_j)) continue;
                    const auto& d = dets[dj_j];
                    const float iou = iouBox(tcx, tcy, tw, th,
                                            d.cx(), d.cy(), d.w(), d.h());
                    if (iou > bestIoU) { bestIoU = iou; bestT = ti_i; bestD = dj_j; }
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
            t.cls    = cls;
            t.score  = d.score;
            t.lost   = false;
            t.terminate_count = cfg_.terminateFrames;
            if (!t.confirmed) {
                if (++t.init_frames >= cfg_.confirmFrames) t.confirmed = true;
            }
            matchedTracks.insert(bestT);
            matchedDets.insert(bestD);
        }

        // Unmatched detections of this class spawn new tracks.
        for (int dj_j : dIdx) {
            if (matchedDets.count(dj_j)) continue;
            const auto& d = dets[dj_j];
            KTrack t;
            t.id = allocId();
            t.x = Mat(6, 1);
            t.x.at(0, 0) = d.cx(); t.x.at(1, 0) = d.cy();
            t.x.at(2, 0) = d.w();  t.x.at(3, 0) = d.h();
            t.x.at(4, 0) = 0.0f;  t.x.at(5, 0) = 0.0f;
            t.P = initCovariance();
            t.last_valid = t.x;
            t.cls = cls;
            t.score = d.score;
            t.init_frames = 1;
            t.confirmed = (cfg_.confirmFrames <= 1);
            t.terminate_count = cfg_.terminateFrames;
            t.lost = false;
            tracks_.push_back(std::move(t));
        }
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
            freeIds_.push_back(tracks_[i].id);
            tracks_.erase(tracks_.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // Emit confirmed tracks (including lost-but-held ones) for the aim loop.
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

void MultiTracker::update(const std::vector<infer::Box>& dets) {
    cfg_ = trackerConfig();
    if (dets.empty()) {
        // No detections this frame: every track goes lost and counts down.
        for (auto& t : tracks_) t.lost = true;
        // Re-run association with empty dets to drive termination + output.
        matchAndUpdate(dets);
        return;
    }
    predictAll();
    matchAndUpdate(dets);
}

}  // namespace tracking
}  // namespace aimbotng
