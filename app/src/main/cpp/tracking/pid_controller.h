// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::tracking::PPID — the aim's closed-loop controller: a plain PIDF.
//
//  Four gains, all of them the user's: kp, ki, kd and kf. There is NO
//  sensitivity compensation layer, no scaling of the gains, and no scaling of
//  the output ceiling.
//
//  P + I(gated, on a leash) + low-passed per-step D + soft-start ramp
//  + VELOCITY FEED-FORWARD F (gated).
//
//  ── ROUND 11: WHAT WAS WRONG, AND WHY IT LOOKED LIKE IT WORKED ──────────────
//
//  Round 10 shipped a feed-forward that reconstructed the target's velocity with
//
//      ŵ = Δe + u(k−L)/kf            F = kf · LPF(ŵ)
//
//  and told the user to set kf = 1/alpha. That is correct *algebraically*, but
//  it made kf carry two jobs at once — the strength of F and the reciprocal of
//  the plant gain — and the two cannot be separated. Rearranged,
//
//      F = kf·LPF(Δe) + LPF(u(k−L))                                        (★)
//
//  the second term has gain EXACTLY 1, independent of kf: **the controller adds
//  its own delayed command back into its input**. That is a positive feedback
//  with a pole on the unit circle, i.e. an accelerator with no brake:
//
//    * it "tracks" — a DC pole integrates a target's steady motion to zero error
//      with no wind-up, which is why a small kf felt like it worked;
//    * it never settles — the same pole is marginally stable, so detector noise
//      keeps it moving. That is the left-right shake;
//    * it OVERSHOOTS when the target stops, **with ki = 0** — the pole has to
//      discharge and nothing damps it. Measured (scripts/aim_selftrap_bench.py
//      表2, alpha = 1, kf = 0.05, kd = 0.2): overshoot 3.3 px at ki = 0, and
//      1554 px once ki = 0.5 — the integral stacks a SECOND integrator on top;
//    * kd cannot brake it, because damping is not the currency a pole at z = 1
//      is short of.
//
//  The user's own reading — "ki causes the overshoot after the target stops" —
//  was right about the actor and wrong about the culprit: ki only amplifies it.
//
//  ── THE FIX: SPLIT THE TWO JOBS, AND BOUND THE RESIDUAL ─────────────────────
//
//      F = (kf/alpha_hat) · LPF( Δe + alpha_hat·u(k−L) )                   (★★)
//
//  alpha_hat is the reconstruction constant (the plant gain, estimated on-line);
//  kf is now ONLY a strength, 0…0.20. Note that one low-pass covers both terms —
//  (★) cancelled the self-term only if both are filtered identically, and an
//  earlier attempt at (★★) that filtered them separately left a high-passed copy
//  of our own command at unity gain and was worse than what it replaced.
//
//  Substituting Δe = w − alpha·u(k−L) into (★★) gives the residual self-term
//
//      kf · (1 − alpha/alpha_hat) · u(k−L)
//
//  so the error in alpha_hat is an ASYMMETRIC risk:
//      alpha_hat UNDER-estimated  → negative → extra damping        SAFE
//      alpha_hat exact            → zero    → a true feed-forward   IDEAL
//      alpha_hat OVER-estimated   → positive → the (★) trap returns BAD
//  and its size is scaled by kf, which is why a small kf is the conservative
//  choice — the opposite of round 10. Two things in the paragraph above were
//  CORRECTED by round 12 and must not be trusted: the bench that produced
//  "kf ≤ 0.5 is the robust band, so the slider stops at 0.20" never tested an
//  alpha below 0.5, and 0.20 is nowhere near the correct value — see the
//  round-12 section below. Also note that "a small kf is safe" is a statement
//  about STABILITY, not about tracking: a small kf is safe and useless.
//
//  ── ROUND 12: THE 0.20 CEILING ON kf WAS THE BUG ────────────────────────────
//
//  The user, at kp 0.06 / ki 0.1 / kd 0.15 / kf 0.08 with an in-game sensitivity
//  of 70: "就是不过冲了，但是目标停下会左右抖几下，并且跟枪跟不上了。"
//
//  Both symptoms are two NUMBERS, and neither is one of his gains.
//
//  1. THE FEED-FORWARD'S GAIN ON TARGET VELOCITY IS kf/alpha_hat, NOT kf.
//     Expand (★★) at DC — F = (kf/alpha_hat)·Δe + kf·u(k−L) — and substitute
//     Δe = w − alpha·u(k−L):
//
//         F = (kf/alpha_hat)·w + kf·(1 − alpha/alpha_hat)·u(k−L)
//
//     The DC command a loop must produce to hold a target moving w view px per
//     step is u_ss = w/alpha, so the fraction of that carrier the feed-forward
//     supplies is
//
//         (kf/alpha_hat)/(1/alpha) = kf·alpha/alpha_hat ≈ 0.8·kf
//
//     — INDEPENDENT of alpha, i.e. of the game. At the old ceiling of 0.20 that
//     is 16 %, and the user's 0.08 supplies 6 %. **kf could not reach the
//     carrier, which is literally what "kf 跟不上枪" says.** The integral then
//     had to carry the rest, and at low alpha the carrier is large: 50 finger
//     px/step for a 600 px/s target at alpha = 0.1.
//
//  2. THE GATE MADE CARRYING IT IMPOSSIBLE. Holding that carrier with P alone
//     needs an error of e = w/(alpha·kp) — 833 px at the user's gains — while
//     the gate's threshold is 1.5 × box ≈ 96…450 px. So the loop is ALWAYS "not
//     settled", and on that branch the integral is ZEROED, every frame, forever.
//     It can never build the carrier; the error stays pinned at 833 px; and the
//     only condition that would end it (|e| < 0.06·box) never arrives. The gate
//     is a LATCH and |Δe| < 2 px cannot hold on a strafing target (Δe ≈ 5 px at
//     600 px/s), so nothing opens it. Measured, alpha = 0.1, box 150 px
//     (scripts/aim_gate_lockout_bench.py 表1/表2): gate shut 80…93 % of frames,
//     error parked at 475…663 px, against 12 px with the gate off. THAT is
//     "跟不上", and no value of kf inside 0…0.20 can tune out of it.
//
//  3. SO kf NOW REACHES 2.0, AND ITS CORRECT VALUE IS A CONSTANT. From (1),
//     a full carrier needs kf = alpha_hat/alpha, i.e. the reciprocal of the
//     estimator's own bias (1/0.8 = 1.25); with the feed-forward's filter lag
//     the measured optimum is ~1.0. That number does NOT depend on the game, the
//     sensitivity or the device, because alpha_hat is estimated on-line and the
//     ratio is scale-free. The slider is a trim, not a calibration: 1.0 is the
//     value, 0.8…1.5 the usable band, and 0 exactly switches F off.
//     Measured at the user's gains, alpha = 0.1: kf 0.08 -> lag 663 px; 0.20 ->
//     645; 0.40 -> 126; 0.60 -> 82; 0.80 -> 36; 1.00 -> −12; 1.20 -> −62.
//     Across alpha = 0.05…2.5 at kf = 1.0: lag −1…−19 px. With alpha_hat wrong
//     by 0.4×…3.0×: lag −33…+12 px. The old ceiling put the ENTIRE slider inside
//     the first two cells.
//
//  4. THE FEED-FORWARD IS NO LONGER GATED. F is a MEASUREMENT (ŵ is
//     reconstructed from the Δe just observed), not the "bet" the gate's
//     rationale describes, so gating it protected nothing and latched the loop
//     out of the regime it exists for. The gate now governs the INTEGRAL only —
//     which is the one thing the reference PID's rule really protects.
//
//  The price, stated honestly: the feed-forward's NOISE gain is also
//  kf/alpha_hat, so at kf = 1.0 and alpha = 0.1 a 1.5 px detection wobble
//  becomes ~5 px of crosshair shimmer (表A) — that is what the user's small kf
//  was accidentally buying, and it is much cheaper than a 650 px lag. The error
//  is built from the tracker's SMOOTHED position, not the raw detector, so the
//  figure in practice is nearer 1…3 px. If the shimmer is still objectionable,
//  raise kFfTauSec (0.033 -> 0.05…0.08 trades a few px of lead for it) rather
//  than lowering kf, which trades the tracking back.
//
//  ── THE GATE (ported from the reference PID the user pointed at) ────────────
//
//  The other half of the overshoot fix is not a gain at all. When the aim is far
//  off or the error is changing fast, the loop is in a TRANSIENT: that is the
//  one place a velocity feed-forward is worthless (a lead is a bet that the
//  target keeps going) and the one place an integrator can only do harm. So both
//  are switched off there:
//
//    * |e| < tiny                → settled; hold what has been learned
//    * |e| >= gate               → NOT settled, and the integral is ZEROED
//    * otherwise, |Δe| below the
//      tolerance for N frames    → settled
//
//  with gate == kGateFrac × the target's own box width. Expressing the gate in
//  TARGET WIDTHS rather than pixels is the reference's real trick: the same
//  pixels mean "far off" on a distant 20 px box and "on target" on a 300 px one,
//  so a fixed pixel gate is either twitchy at range or blind up close, while
//  this one scales with distance for free. The integral is ZEROED, not frozen,
//  on the far branch: freezing it leaves the charge to be spent the moment the
//  target slows, which is precisely the "overshoot after it stops".
//
//  ── THE CEILINGS ARE CONSTANTS (no alpha scaling) ───────────────────────────
//
//  kOutLimitPx = 180 finger px/step. Reachable view speed = alpha·180·120 px/s,
//  so the loop can hold 600 px/s down to alpha = 0.028 and 1500 px/s down to
//  alpha = 0.070, and because the limiter is tanh, reaching u_ss = 125 needs a
//  raw ~190, so the constant has headroom rather than being the nominal figure.
//
//  kTrimLimitPx = 90, half the output ceiling. The integral is now the DC
//  carrier again (F is deliberately partial), so it needs real range — but the
//  gate is what keeps a big carrier from becoming a big overshoot.
//
//  ── WHAT THIS CONTROLLER DOES NOT COVER ─────────────────────────────────────
//
//  Fixed gains cannot serve every game. The delay-imposed bound is
//  kp·alpha < 2·sin(pi/(2(2L+1))) ≈ 0.285 at L = 5, and the per-step derivative
//  has a bound of the same kind (2·kd·alpha). Measured with everything else
//  correct (表6): at alpha = 2.5, kp·alpha = 0.20 gives 15 px of sway and
//  kp·alpha = 0.25 gives 42 px, then 2527 px at 0.30. A high-sensitivity game
//  is expected to LOWER kp and kd — that is what PIDF means. If the aim shakes
//  at rest and no gain helps, compute kp × your game's alpha BEFORE touching kf.
//
//  ── Kept from earlier revisions, because the repairs are real ────────────────
//
//  A per-frame `reset()` is NOT here. setGains() below is pure assignment. An
//  earlier version called init(), which ended in reset(), once per render frame
//  — 120 times a second — which destroyed the integral and the derivative
//  history and turned the loop into a first-order proportional law with an
//  irreducible steady-state lag. If the aim is ever "slow AND twitchy" again,
//  grep for what writes state every frame BEFORE touching a gain.
//
//  kd is PER STEP, not per second, and it is low-passed (kDerivTauSec) because
//  the raw one-step difference is the noisiest signal in the loop.
//
//  The output limiter is tanh, not clamp: unit slope through the origin, so it
//  is transparent during normal tracking and only compresses a re-lock lurch.
//
//  The soft start is CONTINUOUS (kRampRate). A binary "已达标/未达标" gain switch
//  with hysteresis was tried, was invented here rather than ported, and cost the
//  final approach: a 250 px step reached |e| < 2 px in 76 frames with the switch
//  against 20 without it. The GATE above is a different thing — it switches the
//  the INTEGRAL, not the proportional gain, so the approach is still driven at
//  full authority. (Round 12 took the feed-forward out of its jurisdiction — see
//  the round-12 section above.)
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cmath>

namespace aimbotng {
namespace tracking {

// ── The loop's hard limits (finger px per control step) ─────────────────────
//
// Constants, NOT tuning parameters and NOT scaled by anything. See the header
// for the coverage each one buys.
constexpr float kOutLimitPx  = 180.0f;
constexpr float kTrimLimitPx =  90.0f;
constexpr float kDerivTauSec = 0.025f;

// ── Feed-forward ────────────────────────────────────────────────────────────
//
// kFfTauSec: ŵ = Δe + alpha_hat·u(k−L) is a ONE-STEP difference, so it is as
// noisy as the detector (sigma ~1.5 px ⇒ several px/step of phantom velocity).
// A first-order low-pass at 33 ms (4 steps at 120 Hz) keeps that noise out of
// the crosshair without eating the lag F was added to remove.
//
// kFfDelaySteps: how many control steps of transport delay the reconstruction
// compensates for. L is a property of the whole chain (detection staleness +
// touch injection + the game's own frame), roughly 40 ms at 120 Hz, so 5. It is
// NOT critical — measured, a wrong value anywhere from 2 to 7 changes the sway
// by a few px — so it is a constant and never a slider.
//
// kFfStrengthMax: the top of the kf slider. 2.0 — NOT 0.20. Round 11 had 0.20
// and that ceiling was the bug the user was feeling: from the round-12 section,
// the feed-forward's carrier fraction is ≈ 0.8·kf, so 0.20 caps it at 16 % of
// the DC command the loop needs, no matter what the game's sensitivity is. The
// CORRECT value is ~1.0 for every game (kf = alpha_hat/alpha, and alpha_hat is
// estimated on-line), so the range has to straddle 1.0 comfortably on both
// sides. 2.0 is headroom, not a target.
constexpr float kFfTauSec      = 0.033f;
constexpr int   kFfDelaySteps  = 5;
constexpr float kFfStrengthMax = 2.0f;

// ── Soft start ──────────────────────────────────────────────────────────────
//
// A re-acquisition is a step of 100-300 px of error in ONE frame. The P term
// would convert that straight into a 10-30 px finger lurch, so the feedback is
// faded in from zero over ~4 steps. Continuous, no state bit, no threshold to
// chatter on.
constexpr float kRampRate = 0.30f;

// ── The gate (see the header) ───────────────────────────────────────────────
//
// All three are fractions of, or offsets in, the TARGET'S BOX WIDTH, so they
// hold their meaning at any range. Order of magnitude: a 64 px box gives a
// 3.8 px "already on target" radius and a 96 px "give up and restart" radius,
// which is 1.5 target widths — the reference PID's own figure.
constexpr float kGateFrac     = 1.5f;   // |e| >= this × box  ⇒ not settled
constexpr float kGateTinyFrac = 0.06f;  // |e| <  this × box  ⇒ settled
constexpr float kStabTolPx    = 2.0f;   // |Δe| below this counts as "not moving"
constexpr int   kStabNeed     = 2;      // ... for this many consecutive frames

// ── On-line plant-gain (alpha) estimation ───────────────────────────────────
//
// kAlphaBias: the estimate is deliberately pulled UNDER the true value, because
// that is the safe side of the asymmetry in the header (under-estimate ⇒ the
// residual self-term is damping). The window fit over-estimates a little at low
// alpha and under-estimates at high alpha (表5, +30% at 0.5 … −31% at 5.0), so
// 0.8 lands the used value at 0.55…1.04× the truth across that whole span.
constexpr float kAlphaBias = 0.8f;
constexpr float kAlphaMin  = 0.05f;
constexpr float kAlphaMax  = 4.0f;

namespace {
// ── Soft limiter ─────────────────────────────────────────────────────────────
//
// |v| << limit  →  v unchanged (unit slope — the limiter must NOT act as a gain)
// |v| >> limit  →  asymptotes to ±limit, smoothly and differentiably.
static inline float softLimit(float v, float limit) {
    if (limit <= 0.0f) return v;
    return limit * std::tanh(v / limit);
}
}  // namespace

// ── On-line alpha estimator ─────────────────────────────────────────────────
//
// The identity that makes the plant gain observable at all is
//
//      e(k) = T(k) − alpha·U(k)          U = the cumulative finger displacement
//
// where T is the target's own screen position. Round 10 declared alpha
// unidentifiable — correctly, for the signal it looked at: the tracker reports
// the box's SCREEN velocity, which is ΔT_world − alpha·u, so the aim's own
// output is inside the measurement and one cannot difference it out. But the
// regressor that DOES work is not the error, it is U: our own command history is
// known exactly, it is rich in content that a target's smooth motion does not
// have, and over a short window T is well approximated by a low-order polynomial.
//
// Least squares on  e(k) = c0 + c1·k + c2·k² − alpha·U(k)  therefore identifies
// alpha from the CONTRAST between our own jagged output and the target's smooth
// motion. It is only solvable while the command actually varies (an approach, a
// swing, a target switch) — in a perfectly steady hold U is collinear with k and
// the fit is singular, which is also when alpha does not matter.
//
// Nothing here is on the critical path: the controller falls back to 1.0 and the
// whole kf range stays stable with alpha_hat off by 2× in either direction.
struct AlphaEstimator {
    static constexpr int kWin = 90;      // 0.75 s at 120 Hz

    void reset() {
        head_ = count_ = tick_ = 0;
        cumU_ = 0.0f;
        alpha_ = 1.0f;
        valid_ = false;
    }

    /// One control step. `e` is the raw error in screen px; `uDelayed` is the
    /// command that produced it, i.e. the output from kFfDelaySteps steps ago.
    void push(float e, float uDelayed) {
        cumU_ += uDelayed;
        if (!(std::isfinite(e) && std::isfinite(cumU_))) return;
        e_[head_] = e;
        U_[head_] = cumU_;
        k_[head_] = static_cast<float>(tick_);
        head_ = (head_ + 1) % kWin;
        if (count_ < kWin) ++count_;
        ++tick_;
        if (count_ == kWin && (tick_ % 30) == 0) solve();
    }

    /// The value to USE (already biased low and clamped), never unset.
    float value() const { return alpha_; }
    /// True once a fit has ever succeeded — for the log only.
    bool  valid() const { return valid_; }
    /// The last raw fit, before the bias. For the log only.
    float raw()   const { return raw_; }

private:
    void solve() {
        // Normal equations for parameters [c0, c1, c2, alpha] with regressors
        // [1, k, k², −U]. 4×4 with a partial-pivot Gauss-Jordan; 90 samples once
        // every 30 steps is nothing.
        double M[4][4] = {{0}}, b[4] = {0};
        for (int i = 0; i < kWin; ++i) {
            const double x[4] = {1.0, k_[i], double(k_[i]) * k_[i], -double(U_[i])};
            const double y = e_[i];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) M[r][c] += x[r] * x[c];
                b[r] += x[r] * y;
            }
        }
        // Excitation guard: U must actually vary, or the fit is meaningless even
        // when the matrix happens to invert.
        double uMean = 0.0, uVar = 0.0;
        for (int i = 0; i < kWin; ++i) uMean += U_[i];
        uMean /= kWin;
        for (int i = 0; i < kWin; ++i) uVar += (U_[i] - uMean) * (U_[i] - uMean);
        uVar /= kWin;
        if (uVar < 25.0) return;                       // < 5 px rms of travel: skip

        for (int i = 0; i < 4; ++i) {
            int pv = i;
            for (int r = i + 1; r < 4; ++r) if (std::fabs(M[r][i]) > std::fabs(M[pv][i])) pv = r;
            if (std::fabs(M[pv][i]) < 1e-9) return;    // singular: keep old value
            for (int c = 0; c < 4; ++c) std::swap(M[i][c], M[pv][c]);
            std::swap(b[i], b[pv]);
            for (int r = i + 1; r < 4; ++r) {
                const double f = M[r][i] / M[i][i];
                for (int c = i; c < 4; ++c) M[r][c] -= f * M[i][c];
                b[r] -= f * b[i];
            }
        }
        double x[4] = {0};
        for (int i = 3; i >= 0; --i) {
            double s = b[i];
            for (int j = i + 1; j < 4; ++j) s -= M[i][j] * x[j];
            x[i] = s / M[i][i];
        }
        const double a = x[3];
        if (!(a > kAlphaMin && a < 8.0)) return;       // reject absurd fits
        raw_ = static_cast<float>(a);
        alpha_ = std::clamp(kAlphaBias * raw_, kAlphaMin, kAlphaMax);
        valid_ = true;
    }

    float e_[kWin] = {0.0f};
    float U_[kWin] = {0.0f};
    float k_[kWin] = {0.0f};
    int   head_ = 0, count_ = 0, tick_ = 0;
    float cumU_ = 0.0f;   // running sum of the delayed commands == U(k)
    float alpha_ = 1.0f;
    float raw_ = 0.0f;
    bool  valid_ = false;
};

// ── Per-step position controller ─────────────────────────────────────────────
//
// update() takes the position error in px and returns the finger displacement
// in px for this one control step. One instance per axis.
class PPID {
public:
    PPID() = default;

    // ── Parameters ────────────────────────────────────────────────────────────
    //
    // kp        : proportional gain, "fraction of the error covered per step".
    //             Its USEFUL ceiling is set by the loop's delay: kp·alpha must
    //             stay below 2·sin(pi/(2(2L+1))) — 0.285 at L = 5. THIS is the
    //             first thing to check when the aim shakes at rest.
    // ki        : integral gain, (px of output) per (px of error x second). The
    //             DC carrier — F supplies only part of it on purpose — and it is
    //             safe to use precisely because the gate zeroes it in transients.
    // kd        : derivative gain, PER STEP, dimensionless. Damping, and the only
    //             currency a delay-limited loop has for buying phase margin. Its
    //             own bound is of the same form (2·kd·alpha).
    // outSmooth : EMA on the output, 0..1. 1.0 = OFF (the default) and there is a
    //             reason it ships off: an EMA is pure phase lag, and phase lag is
    //             the one thing a delay-limited loop cannot afford.
    // kf        : feed-forward STRENGTH, 0…kFfStrengthMax. Not the plant gain,
    //             and no longer a per-game calibration: the correct value is the
    //             CONSTANT ≈1.0 (kf = alpha_hat/alpha, and alpha_hat is estimated
    //             on-line), 0 turns F off, and 2.0 is headroom. See the header.
    //
    // NOTE: this does NOT reset the controller. See the class comment.
    void setGains(float kp_, float ki_, float kd_, float outSmooth_, float kf_) {
        kp        = kp_;
        ki        = ki_;
        kd        = kd_;
        outSmooth = std::clamp(outSmooth_, 0.0f, 1.0f);
        kf        = std::clamp(kf_, 0.0f, kFfStrengthMax);
        outLimit  = kOutLimitPx;
        trimLimit = kTrimLimitPx;
    }

    /// The reconstruction constant, from the estimator. Only ever changes the
    /// residual self-term, never the loop's gain, so a bad value degrades the
    /// damping rather than the stability near the working point.
    void setAlphaHat(float a) { alphaHat = std::clamp(a, kAlphaMin, kAlphaMax); }

    /// The engaged target's box width in screen px, for the gate. <= 0 disables
    /// the gate (both the feed-forward and the integrator then run open).
    void setTargetBoxPx(float px) { boxPx = (px > 0.0f && std::isfinite(px)) ? px : 0.0f; }

    // ── Core update ───────────────────────────────────────────────────────────
    //
    // error         : current position error (target - crosshair), px
    // dt            : control step, seconds
    // frozen        : true when this axis is inside its deadzone. The caller is
    //                 discarding the proportional and derivative terms for this
    //                 axis; the integrator is held, because accumulating behind
    //                 a discarded output is wind-up and its symptom is a lurch
    //                 the moment the target leaves the stop band. The trim AND
    //                 the feed-forward are still added, so a target drifting
    //                 through the band is still followed by what the loop has
    //                 already learned rather than snapping back.
    // targetChanged : true when the caller switched to a DIFFERENT track this
    //                 step (it knows, from the track id). The integral, the
    //                 feed-forward velocity and the soft-start ramp are re-seeded
    //                 — a switch is not the target moving that far, and the old
    //                 target's velocity is not the new one's. The feed-forward is
    //                 also INHIBITED for this one step: Δe is meaningless across
    //                 a switch, and u(k−L) alone would be read as a real target
    //                 velocity. The OUTPUT HISTORY is deliberately NOT cleared —
    //                 those commands really did move the view, so they are still
    //                 the right thing to subtract.
    //
    // Returns: finger displacement for this step, px.
    float update(float error, float dt, bool frozen = false,
                 bool targetChanged = false) {
        if (!std::isfinite(error) || dt <= 0.0f) return 0.0f;

        if (targetChanged) {
            integral  = 0.0f;
            deriv     = 0.0f;
            lastError = error;   // not 0: zeroing it would make the derivative
                                 // read kd * error / dt for one step, which is
                                 // the lurch this branch exists to avoid.
            ramp      = 0.0f;
            ffVel     = 0.0f;
            ffInhibit = true;
            stabCount = 0;
            settled   = true;
        }

        // ── Soft start ────────────────────────────────────────────────────────
        ramp = std::min(1.0f, ramp + kRampRate);

        const float de = error - lastError;

        // ── Derivative: filtered, PER STEP ───────────────────────────────────
        const float a = dt / (dt + kDerivTauSec);
        deriv += a * (de - deriv);
        float d = std::clamp(kd * ramp * deriv, -outLimit, outLimit);

        // Proportional — per step, NOT scaled by dt (its stability bound is a
        // per-sample one; see the class note).
        float p = kp * ramp * error;

        // Deadzone: suppress the proportional and derivative terms only.
        if (frozen) { p = 0.0f; d = 0.0f; }

        // ── The gate: the INTEGRAL's transient guard ─────────────────────────
        // See the header: an integrator can only do harm in a transient, and that
        // is all this now governs — round 12 removed the feed-forward from its
        // jurisdiction (see the FF branch below), because gating a MEASUREMENT
        // protected nothing and latched the loop out of low-alpha tracking. The
        // threshold is in TARGET WIDTHS, so it means the same thing at any range,
        // and the integral is ZEROED (not frozen) on the far branch.
        bool allow = true;
        if (boxPx > 0.0f) {
            const float gate = kGateFrac * boxPx;
            const float tiny = kGateTinyFrac * boxPx;
            const float ae   = std::fabs(error);
            if (ae < tiny) {
                settled = true;
                stabCount = 0;
            } else if (ae >= gate) {
                settled = false;
                stabCount = 0;
                integral = 0.0f;      // ZEROED, not frozen — see the header
            } else {
                stabCount = (std::fabs(de) < kStabTolPx) ? stabCount + 1 : 0;
                if (stabCount >= kStabNeed) { settled = true; stabCount = 0; }
            }
            allow = settled;
        }

        // ── Feed-forward: reconstruct the target's OWN screen velocity ───────
        // ŵ = Δe + alpha_hat·u(k−L), then scaled to finger px by 1/alpha_hat and
        // by the strength kf. ONE low-pass covers both terms — filtering them
        // separately leaves a high-passed copy of our own command at unity gain,
        // which is the trap in the header. u(k−L) is the delay-aligned sample out
        // of the ring: the command that actually produced the Δe we are looking
        // at, NOT the previous step's output.
        //
        // NOT gated by `allow` — deliberately. The gate's rationale is "a lead is
        // a bet that the target keeps going", but this F is a MEASUREMENT: ŵ is
        // reconstructed from the Δe just observed, so there is nothing to protect
        // by suppressing it. Gating it does positive harm, because the gate is a
        // LATCH: |Δe| < 2 px cannot hold on a strafing target, and the only other
        // way back in is |e| < 0.06·box — unreachable at low alpha, where holding
        // a moving target with P alone needs |e| = w/(alpha·kp) (833 px at
        // alpha = 0.1, kp = 0.06) while the threshold is only 1.5·box. A gated
        // feed-forward therefore locks ITSELF out exactly in the regime it exists
        // for — that is the user's "跟不上" (表1/表2: gate shut 80…93 % of frames,
        // error parked at 475…663 px).
        float ff = 0.0f;
        if (kf > 0.0f && alphaHat > 0.0f && !ffInhibit) {
            const float af = dt / (dt + kFfTauSec);
            ffVel += af * ((de + alphaHat * uHist[uHead]) - ffVel);
            ff = std::clamp(kf * ffVel / alphaHat, -outLimit, outLimit);
        }
        ffInhibit = false;

        // ── Integral: gated, and conditional on saturation ───────────────────
        // Stop accumulating while the output is already saturated the way the
        // error wants to push it. Without this the integral builds a reserve
        // behind a clipped output and spends it later.
        const float raw = p + integral + d + ff;
        const bool pushHi = raw >=  outLimit && error > 0.0f;
        const bool pushLo = raw <= -outLimit && error < 0.0f;
        if (allow && !frozen && !pushHi && !pushLo) {
            integral += ki * error * dt;
            integral  = std::clamp(integral, -trimLimit, trimLimit);
        }

        const float u = softLimit(p + integral + d + ff, outLimit);

        outPrev = outSmooth * u + (1.0f - outSmooth) * outPrev;

        // Record the command the world actually received (post-EMA) and advance
        // the ring. uHead always points at the oldest entry, so uHist[uHead] IS
        // the output from kFfDelaySteps steps ago.
        uHist[uHead] = outPrev;
        uHead = (uHead + 1) % kFfDelaySteps;

        lastError = error;
        return outPrev;
    }

    /// The command that produced the error we most recently acted on — the
    /// delay-aligned sample, which the alpha estimator needs as its regressor's
    /// increment. Read BEFORE the next update() shifts the ring.
    float delayedCommand() const { return uHist[uHead]; }

    // ── Reset all internal state ───────────────────────────────────────────────
    // Called on a fresh engagement — NOT on a slider change and NOT on the
    // synthetic finger's re-anchor. The trim is a property of the target's
    // velocity and of the game's sensitivity; neither of those changes when the
    // finger is lifted and re-pressed, so wiping it there only means the aim
    // has to re-learn the same target's speed every few hundred pixels.
    void reset() {
        integral  = 0.0f;
        lastError = 0.0f;
        outPrev   = 0.0f;
        deriv     = 0.0f;
        ramp      = 0.0f;
        ffVel     = 0.0f;
        ffInhibit = false;
        uHead     = 0;
        stabCount = 0;
        settled   = true;
        for (int i = 0; i < kFfDelaySteps; ++i) uHist[i] = 0.0f;
    }

    /// Read-only views for the diagnostic log.
    ///
    /// `ffValue()` is THE number to watch on a moving target: at kf ≈ 1.0 it
    /// should settle at about ΔT/alpha — the WHOLE DC carrier — and hold there.
    /// Near zero while the error trails the target means F is not running (kf = 0
    /// or every frame reports targetChanged). Before round 12 it also settled low
    /// whenever the gate was shut, which at low alpha was most of the time.
    ///
    /// `integralValue()` is the DC carrier: with kf = 0.20 it supplies most of
    /// u_ss, so it should be a SIZABLE number on a moving target and near zero on
    /// a still one. It going to zero and STAYING there while the target strafes
    /// means the gate never opens — check the box width the caller passes in.
    float ffValue()        const { return alphaHat > 0.0f ? kf * ffVel / alphaHat : 0.0f; }
    float integralValue()  const { return integral; }
    float lastErrorValue() const { return lastError; }
    /// The derivative's CONTRIBUTION to this step's output, in px.
    float derivPx()        const { return kd * ramp * deriv; }
    /// The output ceiling in force, finger px/step (a constant).
    float outLimitPx()     const { return outLimit; }
    /// The feed-forward STRENGTH in force.
    float ffGainValue()    const { return kf; }
    /// The reconstruction constant in force (the estimated plant gain).
    float alphaHatValue()  const { return alphaHat; }
    /// Whether the current step was judged "settled" (gate open).
    bool  settledNow()     const { return settled; }

private:
    float kp        = 0.10f;
    float ki        = 0.5f;
    float kd        = 0.20f;
    float outSmooth = 1.0f;
    float kf        = 1.0f;
    float alphaHat  = 1.0f;

    // ── State ─────────────────────────────────────────────────────────────────
    float integral  = 0.0f;
    float lastError = 0.0f;
    float outPrev   = 0.0f;
    float deriv     = 0.0f;   // filtered one-step change of the error
    float ramp      = 0.0f;   // soft-start ramp, 0..1
    float ffVel     = 0.0f;   // filtered ŵ, the target's own screen velocity
    bool  ffInhibit = false;  // one step of silence after a target switch
    float outLimit  = kOutLimitPx;
    float trimLimit = kTrimLimitPx;

    // Gate
    float boxPx     = 0.0f;   // engaged target's box width; 0 = gate disabled
    int   stabCount = 0;
    bool  settled   = true;

    float uHist[kFfDelaySteps] = {0.0f};  // our own commands, for u(k−L)
    int   uHead    = 0;                   // index of the OLDEST entry
};

}  // namespace tracking
}  // namespace aimbotng
