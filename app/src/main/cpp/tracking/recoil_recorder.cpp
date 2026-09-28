// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::recoil — recorder implementation. See recoil_recorder.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "tracking/recoil_recorder.h"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "capture/capture.h"

#define TAG "AimbotRecoil"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace recoil {

namespace {

using Clock = std::chrono::steady_clock;

// ── Analysis constants ──────────────────────────────────────────────────────
//
// All of them are deliberately loose, because the cost of being wrong is
// asymmetric: a missed shot only merges two knots of the same curve, whereas a
// spurious one writes a step into the profile that the player then "corrects"
// by hand at a place where nothing happened.

/// Output knot spacing, in ms. 10 ms is ~1 frame at 100 fps: finer would store
/// duplicate samples, coarser starts to round off the corner each kick makes.
constexpr float kKnotStepMs = 10.0f;
/// Upper bound on stored knots, so a stuck recording cannot write a 40 MB file.
constexpr int   kMaxKnots   = 1500;
/// Two kicks closer than this are one kick (a burst cannot fire faster than
/// ~1700 RPM, and the detector runs on 60 Hz samples).
constexpr float kMinShotGapMs = 34.0f;
/// A kick has to reach this fraction of the burst's STRONGEST frame to count —
/// the PC analyser's rule (rel_threshold off the max), not a multiple of the
/// median. The median on a phone is drift at ~0.001 deg/ms while a real kick
/// is only ~0.005-0.015 deg/ms at a phone FOV, so "N times the median" either
/// misses every shot or drowns in noise depending on the day's drift.
constexpr float kRelStepFactor = 0.20f;
/// …and this many degrees/ms in absolute terms, so pure noise on a still view
/// cannot turn into an infinite ratio. Kept just above the parked-view jitter
/// (~0.001) and far below a real kick — it is a noise floor, not a detector.
constexpr float kMinStepRateFloor = 0.004f;
/// The recording stops itself this long after arming. Firing happens with the
/// menu closed (the board covers the fire button), so "press stop when done"
/// is unreachable in the real flow — without this the burst sits in Recording
/// until the user reopens the menu, which reads as "结束录制没反应".
constexpr float kRecordWindowSec = 12.0f;
/// Samples whose marker match scored below this are shown as lost and excluded
/// from the quality figures. They are still used for the shape: a gap in the
/// track does not move the marker, it just means we stopped watching it.
constexpr float kGoodConf    = 0.55f;
/// Below this the whole measurement is refused outright.
constexpr float kFailConf    = 0.30f;
/// Between-shot drift / kick ratio above which the weapon clearly recovers
/// between shots (the "recovers_between_shots" class of the PC analyser).
constexpr float kRecoverRatio = 0.7f;
/// Give the producer this long to deliver a first frame before giving up.
constexpr double kArmTimeoutSec = 4.0;

/// One tracked marker position.
struct Sample {
    float t = 0.0f;      ///< seconds since the recording started
    float x = 0.0f;      ///< marker screen x, px
    float y = 0.0f;      ///< marker screen y, px
    float conf = 0.0f;   ///< NCC peak, 0..1
    bool  boundary = false;  ///< best match sat on the search window's edge
};

/// A sample reduced to view angles.
struct Theta {
    float tMs = 0.0f;
    float dx = 0.0f;   ///< yaw from the recording origin, degrees
    float dy = 0.0f;   ///< pitch from the recording origin, degrees (+ = down)
    float rate = 0.0f; ///< smoothed |dθ|/dt, degrees per ms
    bool  good = false;
};

// ── State ───────────────────────────────────────────────────────────────────

struct Store {
    RecState state = RecState::Idle;
    RecStats stats;

    std::string stem;
    float       fovDeg = 80.0f;
    int         screenW = 0, screenH = 0;
    float       focalPx = 0.0f;

    MarkerBox box;

    // Patch template (luma, mean-subtracted, unit-normalised denominator kept)
    std::vector<float> patch;
    int  patchW = 0, patchH = 0;
    float patchMean = 0.0f;
    float patchNorm = 0.0f;

    // Frame scratch, grown to whatever the crop turns out to be
    std::vector<uint8_t> rgba;
    int  cropW = 0, cropH = 0;
    int  cropX = 0, cropY = 0;
    uint64_t lastFrameId = 0;

    int64_t armedAtMs = 0;
    int     trimmedShots = 0;  ///< >0 when the recording held several bursts

    std::vector<Sample> samples;
    std::vector<Theta>  theta;

    // ── Analysis cursors ────────────────────────────────────────────────────
    int      stage = 0;
    int      cursor = 0;
    std::vector<ShotMark> marks;
    Profile  out;
    float    t0Ms = 0.0f;     ///< the first detected shot, in recording ms
    float    tEndMs = 0.0f;
    /// Median between-shot motion / median kick motion, averaged over the shot
    /// intervals. > kRecoverRatio means the weapon recovers on its own.
    float    driftRatio = 0.0f;
};

Store g;

int64_t nowMs() {
    static const auto base = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - base)
        .count();
}

/// Rec.709 luma, integer-ish. The marker is a high-contrast patch, so any
/// monotone brightness map works; this one just has to be the same for the
/// template and for every frame it is matched against.
inline float luma(const uint8_t* p) {
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

/**
 * Zero-mean normalised cross correlation of the template against one offset.
 *
 * Zero-mean (rather than plain NCC) is what makes it indifferent to a global
 * exposure change — auto-exposure stepping as the view swings is a real effect
 * on a phone, and it would otherwise show up as a slowly growing offset in the
 * track, i.e. as drift that does not exist.
 */
float nccAt(const uint8_t* px, int w, int h, int stride,
            int ox, int oy, float& outMean, float& outNorm) {
    outMean = 0.0f;
    outNorm = 0.0f;
    if (ox < 0 || oy < 0 || ox + g.patchW > w || oy + g.patchH > h) return -2.0f;

    float sum = 0.0f;
    const int n = g.patchW * g.patchH;
    for (int y = 0; y < g.patchH; ++y) {
        const uint8_t* row = px + (oy + y) * stride + ox * 4;
        for (int x = 0; x < g.patchW; ++x) {
            sum += luma(row + x * 4);
        }
    }
    const float mean = sum / static_cast<float>(n);

    float dot = 0.0f, norm = 0.0f;
    for (int y = 0; y < g.patchH; ++y) {
        const uint8_t* row = px + (oy + y) * stride + ox * 4;
        for (int x = 0; x < g.patchW; ++x) {
            const float v = luma(row + x * 4) - mean;
            dot += v * g.patch[static_cast<size_t>(y) * g.patchW + x];
            norm += v * v;
        }
    }
    outMean = mean;
    outNorm = std::sqrt(norm);
    if (g.patchNorm <= 1e-6f || outNorm <= 1e-6f) return 0.0f;
    return dot / (g.patchNorm * outNorm);
}

/// Grabs one frame, locks the template if needed, and appends a sample.
/// Returns false when there was no new frame this call.
bool sampleOneFrame() {
    int w = 0, h = 0;
    if (!capture::peekSize(w, h) || w <= 0 || h <= 0) return false;

    if (w != g.cropW || h != g.cropH) {
        g.cropW = w;
        g.cropH = h;
        g.rgba.assign(static_cast<size_t>(w) * h * 4u, 0u);
        capture::cropOrigin(g.cropX, g.cropY);
        // The template is only built once, at the start; a resize mid-recording
        // means the producer re-negotiated the mirror, and the patch would be
        // describing a different crop from then on. Refuse rather than mix.
        if (!g.patch.empty()) {
            g.state = RecState::Failed;
            g.stats.state = RecState::Failed;
            g.stats.message = "画面尺寸在录制中改变，本次作废";
            LOGW("capture crop changed mid-recording (%dx%d)", w, h);
            return false;
        }
    }

    if (!capture::copyLatestInto(g.rgba.data(), g.rgba.size(), w, h, g.lastFrameId))
        return false;

    const uint8_t* px = g.rgba.data();
    const int stride = w * 4;

    // ── Template ────────────────────────────────────────────────────────────
    if (g.patch.empty()) {
        const float inset = kPatchInset;
        const int pw = static_cast<int>(g.box.size - inset * 2.0f);
        if (pw < kPatchMinPx) return false;
        const int ox = static_cast<int>(g.box.x - g.cropX + inset + 0.5f);
        const int oy = static_cast<int>(g.box.y - g.cropY + inset + 0.5f);
        if (ox < 0 || oy < 0 || ox + pw > w || oy + pw > h) {
            g.state = RecState::Failed;
            g.stats.state = RecState::Failed;
            g.stats.message = "标记不在采集方形内，无法取样";
            return false;
        }
        g.patchW = g.patchH = pw;
        g.patch.assign(static_cast<size_t>(pw) * pw, 0.0f);

        float sum = 0.0f;
        for (int y = 0; y < pw; ++y) {
            const uint8_t* row = px + (oy + y) * stride + ox * 4;
            for (int x = 0; x < pw; ++x) sum += luma(row + x * 4);
        }
        const float mean = sum / static_cast<float>(pw * pw);
        float norm = 0.0f;
        for (int y = 0; y < pw; ++y) {
            const uint8_t* row = px + (oy + y) * stride + ox * 4;
            for (int x = 0; x < pw; ++x) {
                const float v = luma(row + x * 4) - mean;
                g.patch[static_cast<size_t>(y) * pw + x] = v;
                norm += v * v;
            }
        }
        g.patchNorm = std::sqrt(norm);
        g.patchMean = mean;
        // A flat patch cannot be tracked. Catching it here is much kinder than
        // letting the correlation wander for four seconds and reporting "lost".
        //
        // The comparison is against the RMS deviation (norm / sqrt(n), and the
        // patch is square so sqrt(n) == pw) — i.e. "the average pixel differs
        // from the mean by this many grey levels". Dividing by n instead would
        // make the threshold resolution-dependent: on a 36x36 patch even a
        // half-yellow/half-black hazard stripe scores ~3.3 and anything softer
        // reads as "no texture".
        {
            const float rms = g.patchNorm / static_cast<float>(pw);
            if (rms < 3.0f) {
                g.state = RecState::Failed;
                g.stats.state = RecState::Failed;
                char msg[96];
                std::snprintf(msg, sizeof(msg),
                              "标记区域太平均 (%.1f)，换个高对比的位置", rms);
                g.stats.message = msg;
                return false;
            }
        }
        LOGI("recoil: template %dx%d locked at crop (%d,%d)", pw, pw, ox, oy);
    }

    // ── Search ──────────────────────────────────────────────────────────────
    Sample s;
    s.t = static_cast<float>(nowMs() - g.armedAtMs) / 1000.0f;

    const int cx = (g.samples.empty() ? static_cast<int>(g.box.x - g.cropX + 0.5f)
                                      : static_cast<int>(g.samples.back().x - g.cropX + 0.5f));
    const int cy = (g.samples.empty() ? static_cast<int>(g.box.y - g.cropY + 0.5f)
                                      : static_cast<int>(g.samples.back().y - g.cropY + 0.5f));

    float mean = 0.0f, norm = 0.0f;
    float best = -2.0f;
    int bx = cx, by = cy;

    // Coarse pass on a 2 px lattice, then a 1 px refine around the winner: the
    // full lattice is 41x41 windows of ~1.2k texels each, which at 60 Hz is
    // real work for a render thread that also has a game to draw over.
    for (int oy = cy - kSearchRadius; oy <= cy + kSearchRadius; oy += 2) {
        for (int ox = cx - kSearchRadius; ox <= cx + kSearchRadius; ox += 2) {
            const float v = nccAt(px, w, h, stride, ox, oy, mean, norm);
            if (v > best) { best = v; bx = ox; by = oy; }
        }
    }
    for (int oy = by - 2; oy <= by + 2; ++oy) {
        for (int ox = bx - 2; ox <= bx + 2; ++ox) {
            const float v = nccAt(px, w, h, stride, ox, oy, mean, norm);
            if (v > best) { best = v; bx = ox; by = oy; }
        }
    }

    // Sub-pixel: the parabola through the three samples around the integer peak.
    // Without it a slow horizontal drift quantises to 1 px steps and the shot
    // detector sees a staircase where the truth was smooth — the very thing the
    // PC analyser does the same fit for.
    float subx = 0.0f, suby = 0.0f;
    {
        const float s0 = best;
        float sm = 0.0f, sp = 0.0f, dum = 0.0f;
        sm = nccAt(px, w, h, stride, bx - 1, by, dum, dum);
        sp = nccAt(px, w, h, stride, bx + 1, by, dum, dum);
        const float den = sm - 2.0f * s0 + sp;
        if (std::fabs(den) > 1e-5f) subx = 0.5f * (sm - sp) / den;
        sm = nccAt(px, w, h, stride, bx, by - 1, dum, dum);
        sp = nccAt(px, w, h, stride, bx, by + 1, dum, dum);
        const float den2 = sm - 2.0f * s0 + sp;
        if (std::fabs(den2) > 1e-5f) suby = 0.5f * (sm - sp) / den2;
        if (subx < -0.5f || subx > 0.5f) subx = 0.0f;
        if (suby < -0.5f || suby > 0.5f) suby = 0.0f;
    }

    s.conf = (best < 0.0f) ? 0.0f : (best > 1.0f ? 1.0f : best);
    s.x = static_cast<float>(g.cropX + bx) + subx;
    s.y = static_cast<float>(g.cropY + by) + suby;
    s.boundary = (std::abs(bx - cx) >= kSearchRadius - 1 ||
                  std::abs(by - cy) >= kSearchRadius - 1);

    g.samples.push_back(s);
    g.stats.frames = static_cast<int>(g.samples.size());
    if (s.conf >= kGoodConf) ++g.stats.good;
    g.stats.seconds = s.t;
    return true;
}

/// Degrees per screen pixel at the centre of the frame.
float degPerPx() {
    if (g.focalPx <= 1.0f) return 0.0f;
    return (180.0f / 3.14159265358979f) / g.focalPx;
}

// ── Analysis passes ─────────────────────────────────────────────────────────

/// Pass 0: samples -> θ, and a smoothed rate for the shot detector.
void passTheta(int from, int to) {
    const float k = degPerPx();
    if (g.theta.size() != g.samples.size()) g.theta.resize(g.samples.size());
    if (g.samples.empty()) return;
    const float x0 = g.samples[0].x, y0 = g.samples[0].y;
    const float t0 = g.samples[0].t;

    for (int i = from; i < to && i < static_cast<int>(g.samples.size()); ++i) {
        const Sample& s = g.samples[i];
        Theta& th = g.theta[i];
        th.tMs = (s.t - t0) * 1000.0f;
        // View angle = MINUS the world's screen displacement. The world moves
        // the opposite way to the camera, and that is the whole measurement.
        th.dx = -(s.x - x0) * k;
        th.dy = -(s.y - y0) * k;
        th.good = (s.conf >= kGoodConf) && !s.boundary;

        if (i > 0) {
            const Sample& p = g.samples[i - 1];
            const float dt = (s.t - p.t) * 1000.0f;
            const Theta& pt = g.theta[i - 1];
            const float ddx = th.dx - pt.dx, ddy = th.dy - pt.dy;
            const float d = (dt > 0.1f) ? std::sqrt(ddx * ddx + ddy * ddy) / dt : 0.0f;
            // Time-constant EMA (tau ≈ 15 ms), not a fixed alpha: a kick is
            // 1-2 frames long, and a fixed 0.35 alpha is a ~3-frame boxcar
            // WHOSE WIDTH IN MS DEPENDS ON THE PRODUCER'S FRAME RATE — at
            // 66 fps that is 36 ms, which smears a 15 ms kick down to half
            // its peak and drops it under the detector's floor. A fixed tau
            // smears the same AMOUNT of jitter regardless of frame rate.
            const float a = dt / (dt + 15.0f);
            th.rate = (i == 1) ? d : (pt.rate + (d - pt.rate) * a);
        }
    }
}

/// Pass 1: pick the shots out of the rate series, in one go over the full
/// series (n is a few hundred — one frame's work, no slicing needed).
///
/// Two rounds, like the PC analyser's find_shot_frames: candidates are local
/// maxima above a floor taken off the burst's own strongest frame, accepted
/// strongest-first with a minimum gap; then, with >= 4 marks, the gap is
/// re-estimated from the median inter-shot interval (~0.55x) and detection
/// re-runs. The second round kills double-detections (one kick split into two
/// close peaks by smoke or a redraw) and frees slots for genuine weaker kicks.
void detectShots() {
    g.marks.clear();
    const int n = static_cast<int>(g.theta.size());
    if (n < 5) return;

    float maxR = 0.0f;
    for (int i = 1; i < n; ++i) {
        if (g.theta[i].good) maxR = std::max(maxR, g.theta[i].rate);
    }
    const float floor = std::max(kMinStepRateFloor, maxR * kRelStepFactor);
    if (maxR <= 0.0f) return;

    struct Cand { float r; int i; };
    std::vector<Cand> cands;
    cands.reserve(n / 4);
    for (int i = 1; i < n; ++i) {
        if (!g.theta[i].good) continue;
        const float r = g.theta[i].rate;
        if (r < floor) continue;
        const float prev = (i > 0) ? g.theta[i - 1].rate : -1.0f;
        const float next = (i + 1 < n) ? g.theta[i + 1].rate : -1.0f;
        if (r < prev || r < next) continue;  // local max
        cands.push_back(Cand{r, i});
    }

    auto accept = [&](float minGapMs) {
        std::vector<Cand> byStrength = cands;
        std::sort(byStrength.begin(), byStrength.end(),
                  [](const Cand& a, const Cand& b) { return a.r > b.r; });
        std::vector<int> taken;
        taken.reserve(byStrength.size());
        for (const Cand& c : byStrength) {
            bool clash = false;
            for (int t : taken) {
                if (std::fabs(g.theta[c.i].tMs - g.theta[t].tMs) < minGapMs) {
                    clash = true;
                    break;
                }
            }
            if (!clash) taken.push_back(c.i);
        }
        std::sort(taken.begin(), taken.end());
        g.marks.clear();
        for (int idx : taken) {
            ShotMark m;
            m.n = static_cast<int>(g.marks.size()) + 1;
            m.tMs = g.theta[idx].tMs;
            g.marks.push_back(m);
        }
    };

    accept(kMinShotGapMs);
    if (g.marks.size() >= 4) {
        std::vector<float> gaps;
        gaps.reserve(g.marks.size() - 1);
        for (size_t k = 1; k < g.marks.size(); ++k)
            gaps.push_back(g.marks[k].tMs - g.marks[k - 1].tMs);
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        const float median = gaps[gaps.size() / 2];
        const float adaptive = std::max(kMinShotGapMs, median * 0.55f);
        if (adaptive > kMinShotGapMs) accept(adaptive);
    }
}

/// Pass 2: crop to the first shot and resample onto the fixed knot grid.
void passResample(int from, int to) {
    const int n = static_cast<int>(g.theta.size());
    if (n < 2) return;
    const float span = g.tEndMs;
    if (span <= 0.0f) return;

    const int total = std::min(kMaxKnots, static_cast<int>(span / kKnotStepMs) + 2);
    if (g.out.knots.empty()) g.out.knots.reserve(total);

    auto interp = [&](float tMs, bool y) {
        const size_t i = static_cast<size_t>(
            std::upper_bound(g.theta.begin(), g.theta.end(), tMs,
                             [](float v, const Theta& th) { return v < th.tMs; }) -
            g.theta.begin());
        if (i == 0) return y ? g.theta.front().dy : g.theta.front().dx;
        if (i >= g.theta.size()) return y ? g.theta.back().dy : g.theta.back().dx;
        const Theta& a = g.theta[i - 1];
        const Theta& b = g.theta[i];
        const float dt = b.tMs - a.tMs;
        const float u = (dt > 1e-4f) ? (tMs - a.tMs) / dt : 0.0f;
        const float av = y ? a.dy : a.dx;
        const float bv = y ? b.dy : b.dx;
        return av + (bv - av) * u;
    };

    for (int k = from; k < to && k < total; ++k) {
        Knot kn;
        kn.tMs = k * kKnotStepMs;
        kn.dx = interp(g.t0Ms + kn.tMs, false);
        kn.dy = interp(g.t0Ms + kn.tMs, true);
        g.out.knots.push_back(kn);
    }
}

/// Re-bases θ on the first shot, then records what the burst actually spans.
///
/// R36c: the recording window is 12 s and nothing stops the player from firing
/// SEVERAL bursts (or re-aiming between them) inside one. Every gap between
/// bursts is real movement the marker faithfully records, and — the killer —
/// the shot detector's threshold is a fraction of the STRONGEST rate frame in
/// the whole series, so one burst of fast re-aiming lifts the floor above most
/// genuine kicks and 40 shots read as 5. So: split the marks into bursts
/// (gap > kBurstGapMs is a pause, no weapon fires that slowly), keep the burst
/// with the MOST marks, trim everything else, and re-base the ANGLES — not
/// just the time — on that burst's first shot, so the profile starts at zero
/// instead of carrying wherever the aim was pointing before the trigger.
void cropToFirstShot() {
    if (g.marks.empty()) {
        g.t0Ms = 0.0f;
        g.tEndMs = g.theta.empty() ? 0.0f : g.theta.back().tMs;
        return;
    }

    constexpr float kBurstGapMs = 350.0f;  // ~4x the slowest shot interval
    constexpr float kTailMs     = 60.0f;   // record the last kick's settle

    size_t bestA = 0, bestB = 0;           // [bestA, bestB) in g.marks
    size_t a = 0;
    for (size_t k = 1; k <= g.marks.size(); ++k) {
        const bool split = (k == g.marks.size()) ||
                           (g.marks[k].tMs - g.marks[k - 1].tMs > kBurstGapMs);
        if (!split) continue;
        if (k - a > bestB - bestA) { bestA = a; bestB = k; }
        a = k;
    }
    const bool trimmed = (bestB - bestA) < g.marks.size();
    const int keptShots = static_cast<int>(bestB - bestA);

    const float t0 = g.marks[bestA].tMs;
    const float lastMark = g.marks[bestB - 1].tMs;

    // Keep only the chosen burst's marks, renumbered from 1.
    std::vector<ShotMark> kept(g.marks.begin() + bestA, g.marks.begin() + bestB);
    for (size_t k = 0; k < kept.size(); ++k) {
        kept[k].tMs -= t0;
        kept[k].n = static_cast<int>(k) + 1;
    }
    g.marks.swap(kept);

    // End at the last kick plus a short settle tail — not at recording end,
    // which would append the pause and the re-aiming that followed the burst.
    const float endAbs = std::min(g.theta.back().tMs, lastMark + kTailMs);
    g.t0Ms   = t0;
    g.tEndMs = std::max(1.0f, endAbs - t0);

    // Re-base the ANGLES on the first shot: whatever the marker was doing
    // before the trigger (or where the aim happened to point) must not ride
    // into the profile as a constant offset. Linear interp at t0 on the raw
    // series, then subtract from every sample.
    auto interpAt = [&](float tMs, const float Theta::*v) {
        const size_t i = static_cast<size_t>(
            std::upper_bound(g.theta.begin(), g.theta.end(), tMs,
                             [](float val, const Theta& th) { return val < th.tMs; }) -
            g.theta.begin());
        if (i == 0) return g.theta.front().*v;
        if (i >= g.theta.size()) return g.theta.back().*v;
        const Theta& p = g.theta[i - 1];
        const Theta& q = g.theta[i];
        const float dt = q.tMs - p.tMs;
        const float u = (dt > 1e-4f) ? (tMs - p.tMs) / dt : 0.0f;
        return p.*v + (q.*v - p.*v) * u;
    };
    const float baseDx = interpAt(t0, &Theta::dx);
    const float baseDy = interpAt(t0, &Theta::dy);
    for (Theta& th : g.theta) { th.dx -= baseDx; th.dy -= baseDy; }

    if (trimmed) {
        g.trimmedShots = keptShots;
        LOGI("recoil: recording held several bursts — kept the longest (%d shots, "
             "%.0f..%.0f ms)", keptShots, t0, endAbs);
    }
}

/// Pass 3: quality figures, class, and the file.
void passFinish() {
    float mn = 1.0f, sum = 0.0f;
    int good = 0, total = 0;
    for (const Sample& s : g.samples) {
        ++total;
        if (s.conf < kGoodConf) continue;
        ++good;
        mn = std::min(mn, s.conf);
        sum += s.conf;
    }
    if (good == 0) { mn = 0.0f; }

    // Drift between shots vs. the kick itself. A weapon that returns to zero
    // between shots reads as *mostly* drift; the profile is then useless for
    // compensation and the class says so, which keeps it off the player.
    //
    // Per shot interval: `kick` is the fastest frame in it (the bullet leaving)
    // and `drift` is the MEDIAN frame (what the view does while nothing fires).
    // Comparing those two, rather than two maxima, is what makes the test mean
    // "the view comes back on its own": a maximum-vs-maximum comparison would
    // call any interval with one noisy frame a kick.
    if (g.marks.size() >= 2) {
        double kickSum = 0.0, driftSum = 0.0;
        int intervals = 0;
        for (size_t i = 1; i < g.marks.size(); ++i) {
            const float a = g.marks[i - 1].tMs, b = g.marks[i].tMs;
            if (b - a < 20.0f) continue;

            const size_t ia = static_cast<size_t>(
                std::lower_bound(g.theta.begin(), g.theta.end(), a,
                                 [](const Theta& th, float v) { return th.tMs < v; }) -
                g.theta.begin());
            const size_t ib = static_cast<size_t>(
                std::lower_bound(g.theta.begin(), g.theta.end(), b,
                                 [](const Theta& th, float v) { return th.tMs < v; }) -
                g.theta.begin());
            if (ia >= g.theta.size() || ib >= g.theta.size() || ib <= ia + 2) continue;

            std::vector<float> rates;
            rates.reserve(ib - ia);
            for (size_t k = ia + 1; k <= ib; ++k) {
                const float dt = g.theta[k].tMs - g.theta[k - 1].tMs;
                if (dt < 1e-3f) continue;
                const float ddx = g.theta[k].dx - g.theta[k - 1].dx;
                const float ddy = g.theta[k].dy - g.theta[k - 1].dy;
                rates.push_back(std::sqrt(ddx * ddx + ddy * ddy) / dt);
            }
            if (rates.size() < 3) continue;

            std::vector<float> sorted = rates;
            std::sort(sorted.begin(), sorted.end());
            const float kick  = sorted.back();
            const float drift = sorted[sorted.size() / 2];
            kickSum += kick;
            driftSum += drift;
            ++intervals;
        }
        if (intervals > 0) {
            g.driftRatio = static_cast<float>(driftSum / std::max(1e-6, kickSum));
        }
    }

    float mnx = 0.0f, mxx = 0.0f, mny = 0.0f, mxy = 0.0f;
    for (const Knot& k : g.out.knots) {
        mnx = std::min(mnx, k.dx); mxx = std::max(mxx, k.dx);
        mny = std::min(mny, k.dy); mxy = std::max(mxy, k.dy);
    }
    g.out.dxPeak = mxx - mnx;
    g.out.dyPeak = mxy - mny;

    std::string klass = "standard";
    if (g.out.knots.size() < 5 || g.marks.size() < 2) {
        klass = "unclear";
    } else if (g.driftRatio > kRecoverRatio) {
        klass = "recovers_between_shots";
    }
    if (mn < kFailConf || good * 3 < total) klass = "unclear";

    const float secs = std::max(0.001f, g.stats.seconds);
    g.out.schema  = 1;
    g.out.name    = g.stem;
    g.out.source  = "device";
    g.out.fps     = static_cast<float>(g.samples.size()) / secs;
    g.out.srcW    = g.cropW;
    g.out.srcH    = g.cropH;
    g.out.fovDeg  = g.fovDeg;
    g.out.focalPx = g.focalPx;
    g.out.recoilClass = klass;
    g.out.minConf = mn;
    g.out.meanConf = (good > 0) ? (sum / good) : 0.0f;
    g.out.shots   = static_cast<int>(g.marks.size());
    // There is no ammunition readout on the device (no OCR — see the header),
    // so this gate cannot be checked here. Written false rather than true so
    // the page says "未核对" instead of implying a check that never ran.
    g.out.shotsMatchMagazine = false;
    g.out.marks   = g.marks;
    g.out.note    = "device marker track";
    g.out.loaded  = true;

    g.out.save(profilePath(g.stem));

    g.stats.shots    = g.out.shots;
    g.stats.dxPeak   = g.out.dxPeak;
    g.stats.dyPeak   = g.out.dyPeak;
    g.stats.minConf  = mn;
    g.stats.meanConf = g.out.meanConf;
    g.stats.fps      = g.out.fps;
    g.stats.klass    = klass;
    g.stats.outStem  = g.stem;
    g.stats.progress = 1.0f;
    g.stats.stage.clear();

    if (klass == "standard") {
        g.stats.message = (g.trimmedShots > 0)
            ? "分析完成（检测到多段开火，已取最长一段 " +
                  std::to_string(g.trimmedShots) + " 发）；下次一次打完整梭"
            : "分析完成";
        g.state = RecState::Done;
    } else {
        g.stats.message = (klass == "recovers_between_shots")
            ? "分析完成，但这把枪两发之间会回正，不能用于压枪"
            : "分析完成，但样本质量不足（标记丢失太多），建议重录";
        g.state = RecState::Done;
    }
    g.stats.state = g.state;
    LOGI("recoil: analyzed %d samples -> %zu knots, %d shots, class=%s",
         total, g.out.knots.size(), g.out.shots, klass.c_str());
}

/// Chunked driver for the analysis. Returns true when everything is done.
void runAnalysisSlice() {
    constexpr int kSlice = 24;   // samples per pass per frame
    const int n = static_cast<int>(g.samples.size());

    switch (g.stage) {
        case 0:
            g.stats.stage = "换算角度";
            passTheta(g.cursor, g.cursor + kSlice);
            g.cursor += kSlice;
            g.stats.progress = 0.30f * std::min(1.0f, static_cast<float>(g.cursor) /
                                                          std::max(1, n));
            if (g.cursor >= n) { g.stage = 1; g.cursor = 0; }
            break;
        case 1:
            // Detection runs once over the whole series (see detectShots) —
            // slicing it would hide the global maximum the threshold is taken
            // from. n is a few hundred, so this is one frame's work.
            g.stats.stage = "识别逐发时刻";
            g.stats.progress = 0.45f;
            detectShots();
            cropToFirstShot();
            g.stats.progress = 0.60f;
            g.stage = 2;
            g.cursor = 0;
            break;
        case 2: {
            g.stats.stage = "重采样弹道";
            const int total = std::min(kMaxKnots,
                                       static_cast<int>(g.tEndMs / kKnotStepMs) + 2);
            passResample(g.cursor, g.cursor + kSlice);
            g.cursor += kSlice;
            g.stats.progress = 0.60f + 0.35f * std::min(1.0f, static_cast<float>(g.cursor) /
                                                                  std::max(1, total));
            if (g.cursor >= total) { g.stage = 3; }
            break;
        }
        default:
            g.stats.stage = "写出档案";
            passFinish();
            break;
    }
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────

bool start(const std::string& stem, float fovDeg, const MarkerBox& box,
           int screenW, int screenH) {
    g = Store{};
    g.stats.state = RecState::Armed;
    g.stats.message = "等待采集画面…";
    g.state = RecState::Armed;

    if (!validStem(stem)) {
        g.state = RecState::Failed;
        g.stats.state = RecState::Failed;
        g.stats.message = "配置名不合法";
        return false;
    }
    if (!box.placed || box.size < kPatchMinPx + kPatchInset * 2.0f + 4.0f) {
        g.state = RecState::Failed;
        g.stats.state = RecState::Failed;
        g.stats.message = "先放置标记框";
        return false;
    }
    if (fovDeg < 20.0f || fovDeg > 140.0f) {
        g.state = RecState::Failed;
        g.stats.state = RecState::Failed;
        g.stats.message = "FOV 超出范围";
        return false;
    }

    g.stem = stem;
    g.fovDeg = fovDeg;
    g.stats.windowSec = kRecordWindowSec;
    g.box = box;
    g.screenW = screenW;
    g.screenH = screenH;
    g.focalPx = (screenH * 0.5f) / std::tan(fovDeg * 0.5f * 3.14159265358979f / 180.0f);
    g.armedAtMs = nowMs();

    LOGI("recoil: record start stem=%s fov=%.1f focal=%.1fpx marker=(%.0f,%.0f)+%.0f",
         stem.c_str(), fovDeg, g.focalPx, box.x, box.y, box.size);
    return true;
}

void stopAndAnalyze() {
    if (g.state != RecState::Recording && g.state != RecState::Armed) return;
    if (g.samples.size() < 8) {
        g.state = RecState::Failed;
        g.stats.state = RecState::Failed;
        g.stats.message = "样本太少，检查标记是否在采集方形内";
        return;
    }
    g.state = RecState::Analyzing;
    g.stats.state = RecState::Analyzing;
    g.stats.message = "正在分析…";
    g.stage = 0;
    g.cursor = 0;
    g.marks.clear();
    g.out = Profile{};
    g.stats.progress = 0.0f;
    LOGI("recoil: recording stopped after %zu samples", g.samples.size());
}

void abort() {
    const RecStats keep{};
    g = Store{};
    g.stats = keep;
    g.state = RecState::Idle;
    g.stats.state = RecState::Idle;
}

void dismiss() {
    if (g.state == RecState::Done || g.state == RecState::Failed) {
        RecStats keep;
        g = Store{};
        g.stats = keep;
        g.state = RecState::Idle;
        g.stats.state = RecState::Idle;
    }
}

void pump() {
    switch (g.state) {
        case RecState::Armed:
            if (nowMs() - g.armedAtMs > static_cast<int64_t>(kArmTimeoutSec * 1000.0)) {
                g.state = RecState::Failed;
                g.stats.state = RecState::Failed;
                g.stats.message = "采集没有画面，先去「画面」页打开开关";
                break;
            }
            if (sampleOneFrame()) {
                g.state = RecState::Recording;
                g.stats.state = RecState::Recording;
                g.stats.message = "录制中…打完整梭再点结束";
            }
            break;
        case RecState::Recording:
            sampleOneFrame();   // a failed call just means no new frame this tick
            // Self-terminating: firing happens with the menu closed, where the
            // stop button is unreachable. The window is sized for close-menu +
            // empty-a-mag + margin; tapping 结束录制 earlier still wins.
            if (g.stats.seconds >= kRecordWindowSec) stopAndAnalyze();
            break;
        case RecState::Analyzing:
            runAnalysisSlice();
            break;
        default:
            break;
    }
}

RecState state() { return g.state; }
const RecStats& stats() { return g.stats; }

bool wantsFrames() {
    return g.state == RecState::Armed || g.state == RecState::Recording;
}

int sampleCount() { return static_cast<int>(g.samples.size()); }

bool sampleAt(int i, float& outTMs, float& outDxDeg, float& outDyDeg) {
    if (i < 0 || i >= static_cast<int>(g.samples.size())) return false;
    const float k = degPerPx();
    const float x0 = g.samples[0].x, y0 = g.samples[0].y;
    const float t0 = g.samples[0].t;
    outTMs  = (g.samples[i].t - t0) * 1000.0f;
    outDxDeg = -(g.samples[i].x - x0) * k;
    outDyDeg = -(g.samples[i].y - y0) * k;
    return true;
}

float sampleConf(int i) {
    if (i < 0 || i >= static_cast<int>(g.samples.size())) return 0.0f;
    return g.samples[i].conf;
}

}  // namespace recoil
}  // namespace aimbotng
