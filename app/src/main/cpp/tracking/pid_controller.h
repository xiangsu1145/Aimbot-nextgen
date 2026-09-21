// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::tracking::PPID — the aim's closed-loop controller: a plain PIDF.
//
//  Four gains, all of them the user's: kp, ki, kd and kf. There is NO
//  sensitivity compensation layer, no scaling of the gains, and no scaling of
//  the output ceiling — the ceilings are two constants and kf does the one job
//  that the loop cannot work out for itself.
//
//  P + I(leash, accumulated in SECONDS) + low-passed per-step D
//  + soft-start ramp + VELOCITY FEED-FORWARD F.
//
//  ── WHY F HAS TO EXIST ──────────────────────────────────────────────────────
//
//  A P (or PD) loop has NO MEMORY. At e = 0 it outputs 0, so holding a target
//  that moves at ΔT px/step requires a standing error, and the integral is what
//  erases that error. What the integral cannot do is erase it IMMEDIATELY: it
//  has to accumulate, and while it accumulates the crosshair trails. F supplies
//  the same steady output u_ss up front, with no accumulation and no unwind:
//
//      u_ss = ΔT / alpha        alpha = 视场 px / 手指 px（游戏灵敏度）
//
//  ── HOW kf IS COMPUTED: IT IS THE RECIPROCAL OF THE PLANT GAIN ──────────────
//
//  The crosshair is the screen centre, so the error IS the box's screen
//  coordinate, and with L steps of transport delay in the loop the identity is
//
//      Δe(k) = w(k) − alpha · u(k−L)          w = the target's own screen motion
//
//  so the target's own velocity is recoverable by subtracting OUR displacement —
//  provided we use the sample that actually produced it, L steps back, not the
//  one we sent last step:
//
//      ŵ = Δe + u(k−L)/kf                    F = kf · LPF(ŵ)
//      alpha_hat := 1/kf
//
//  **kf is the gain AND the reciprocal of the reconstruction constant — the same
//  number used twice**, and the residual self-term is (alpha_hat − alpha)·u(k−L)
//  × kf, i.e. exactly (1 − kf·alpha)·u(k−L). So:
//
//      kf = 1/alpha  ⇒  the self-term vanishes: a true feed-forward.
//
//  That is the answer to "kf 这个数值怎么在 pid 里算": it is not tuned against
//  the error, it is 1/(plant gain). Nothing else in the controller needs alpha.
//
//  ── THE TWO MISTAKES THIS CODE HAS ALREADY MADE (do not repeat them) ────────
//
//  ① USING u(k−1) INSTEAD OF u(k−L). Reconstructing with last step's output
//     leaves a spurious term proportional to our own CHANGE, which the loop
//     reads back as target velocity. Measured, 600 px/s, kout = 120, kf = 1/alpha
//     (scripts/aim_pidf_bench.py 表3):
//
//                  alpha   0.10    0.50    1.00
//        u(k−1)            338      10     779    ← pp, px
//        u(k−L)              5       8      16
//
//     u(k−1) is not merely worse, it is UNFIXABLE by kf: at alpha = 1 every kf
//     from 0.5 to 8 measured 656 … 871 px of sway. The delay-aligned form is
//     good across the whole range, and — 表9 — it does not care if L is wrong:
//     with the true L = 4, reconstruction delays of 2…7 all measured pp 5–10 px.
//     Hence kFfDelaySteps is a constant, not a user setting.
//
//  ② A SMALL NON-ZERO kf. kf = 0 disables F, and that is safe. But kf = 0.25
//     means alpha_hat = 4, so the reconstruction ADDS 4× our own output to the
//     velocity estimate, the vanishing self-term becomes (1 − 0.25·alpha), and F
//     degenerates into "repeat your own delayed command" — the bare-integrator
//     oscillator of the round-8 feed-forward. Measured sway, 600 px/s, kout 180:
//
//        alpha │ kf=0  kf=0.25 kf=0.5 kf=1  kf=3  kf=6  kf=12
//        0.10  │   2     794     746    685     4     4      6      (px)
//        0.30  │   3    1378    1117      7     6    13    322
//        1.00  │   5      16      13     14  1166  1199   1262
//
//     The usable band is kf·alpha ≈ 0.75 … 1.5, and it MOVES with alpha — which
//     is the whole reason the number is the user's to set, and why kf = 1/alpha
//     is the instruction rather than any fixed value.
//
//  ── THE CEILINGS ARE CONSTANTS (no alpha scaling) ───────────────────────────
//
//  kOutLimitPx = 180 finger px/step. Reachable view speed = alpha·180·120 px/s,
//  so the loop can hold 600 px/s down to alpha = 0.028 and 1500 px/s down to
//  alpha = 0.070 — and because the limiter is tanh, reaching u_ss = 125 needs a
//  raw ~190, so the constant has headroom rather than being the nominal figure.
//  180 rather than 240 because a larger ceiling makes the high-alpha end (which
//  fixed gains cannot serve anyway) swing further, and buys nothing below it.
//
//  kTrimLimitPx = 90, half the output ceiling: the integral is the residual
//  cleaner when F is running, and the DC carrier when F is off — 90 px/step of
//  finger travel is enough for 600 px/s at alpha ≥ 0.11 unaided.
//
//  ── WHAT THIS CONTROLLER DOES NOT COVER ─────────────────────────────────────
//
//  Fixed gains cannot serve every game. The delay-imposed bound is
//  kp·alpha < 2·sin(pi/(2(2L+1))) ≈ 0.45 at L = 4, and the per-step derivative
//  has its own bound of the same kind (2·kd·alpha); together they put the useful
//  ceiling near alpha ≈ 1.5. A high-sensitivity game is expected to LOWER kp and
//  kd — they are the user's sliders, and that is what PIDF means. Above
//  alpha ≈ 2 no gain set here will hold, and 表14 shows it wandering at
//  every kf.
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
//  kd is PER STEP, not per second: the coefficient on one step's change is `kd`
//  at any loop rate. The derivative is low-passed (kDerivTauSec) because the raw
//  one-step difference is the noisiest signal in the loop.
//
//  The output limiter is tanh, not clamp: unit slope through the origin, so it
//  is transparent during normal tracking and only compresses a re-lock lurch.
//
//  The soft start is CONTINUOUS (kRampRate). A binary "已达标/未达标" gain switch
//  with hysteresis was tried, was invented here rather than ported, and cost the
//  final approach: a 250 px step reached |e| < 2 px in 76 frames with the switch
//  against 20 without it. "到目标附近就变得特别慢" was that switch.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cmath>

namespace aimbotng {
namespace tracking {

// ── The loop's hard limits (finger px per control step) ─────────────────────
//
// Constants, NOT tuning parameters and NOT scaled by anything. They exist to
// stop a re-lock from flinging the finger across the panel, and to keep a bad
// velocity estimate out of the crosshair. See the header for the coverage each
// one buys and for why 180 rather than 240.
constexpr float kOutLimitPx  = 180.0f;
constexpr float kTrimLimitPx =  90.0f;
constexpr float kDerivTauSec = 0.025f;

// ── Feed-forward ────────────────────────────────────────────────────────────
//
// kFfTauSec: ŵ = Δe + u(k−L)/kf is a ONE-STEP difference, so it is as noisy as
// the detector (sigma ~1.5 px ⇒ several px/step of phantom velocity). A
// first-order low-pass at 33 ms (4 steps at 120 Hz) keeps that noise out of the
// crosshair without eating the lag F was added to remove.
//
// kFfDelaySteps: how many control steps of transport delay the reconstruction
// compensates for. L is a property of the whole chain (detection staleness +
// touch injection + the game's own frame), roughly 40 ms at 120 Hz, so 5. It is
// NOT critical — measured, a wrong value anywhere from 2 to 7 changes the sway
// by a few px — so it is a constant and never a slider.
constexpr float kFfTauSec     = 0.033f;
constexpr int   kFfDelaySteps = 5;

// ── Soft start ──────────────────────────────────────────────────────────────
//
// A re-acquisition is a step of 100-300 px of error in ONE frame. The P term
// would convert that straight into a 10-30 px finger lurch, so the feedback is
// faded in from zero over ~4 steps. Continuous, no state bit, no threshold to
// chatter on.
constexpr float kRampRate = 0.30f;

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
    //             Its USEFUL ceiling is set by the loop's delay: a discrete
    //             proportional loop with L steps of transport delay is stable
    //             only while kp·alpha < 2·sin(pi/(2(2L+1))) — 0.45 at L = 4.
    // ki        : integral gain, (px of output) per (px of error x second). The
    //             DC carrier when F is off, a residual cleaner when it is on.
    // kd        : derivative gain, PER STEP, dimensionless. Damping, and the only
    //             currency a delay-limited loop has for buying phase margin. Its
    //             own bound is of the same form (2·kd·alpha).
    // outSmooth : EMA on the output, 0..1. 1.0 = OFF (the default) and there is a
    //             reason it ships off: an EMA is pure phase lag, and phase lag is
    //             the one thing a delay-limited loop cannot afford.
    // kf        : feed-forward gain == 1/alpha_hat. Set it to 1/alpha and F is
    //             exact; 0 disables F entirely. See the header — this is the one
    //             number the loop cannot work out for itself, because the error
    //             IS the box's screen coordinate and the aim's own output is in
    //             it, which makes alpha unidentifiable from inside.
    //
    // NOTE: this does NOT reset the controller. See the class comment.
    void setGains(float kp_, float ki_, float kd_, float outSmooth_, float kf_) {
        kp        = kp_;
        ki        = ki_;
        kd        = kd_;
        outSmooth = std::clamp(outSmooth_, 0.0f, 1.0f);
        kf        = std::max(0.0f, kf_);
        outLimit  = kOutLimitPx;
        trimLimit = kTrimLimitPx;
    }

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
    //                 a switch, and u(k−L)/kf alone would be read as a real
    //                 target velocity. The OUTPUT HISTORY is deliberately NOT
    //                 cleared — those commands really did move the view, so they
    //                 are still the right thing to subtract.
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
        }

        // ── Soft start ────────────────────────────────────────────────────────
        ramp = std::min(1.0f, ramp + kRampRate);

        // ── Derivative: filtered, PER STEP ───────────────────────────────────
        const float a = dt / (dt + kDerivTauSec);
        deriv += a * ((error - lastError) - deriv);
        float d = std::clamp(kd * ramp * deriv, -outLimit, outLimit);

        // Proportional — per step, NOT scaled by dt (its stability bound is a
        // per-sample one; see the class note).
        float p = kp * ramp * error;

        // Deadzone: suppress the proportional and derivative terms only.
        if (frozen) { p = 0.0f; d = 0.0f; }

        // ── Feed-forward: reconstruct the target's OWN screen velocity ───────
        // ŵ = Δe + u(k−L)/kf, where u(k−L) is the command that produced the Δe
        // we are looking at — the delay-aligned sample out of the ring, NOT the
        // previous step's output. See mistake ① in the header: u(k−1) leaves a
        // term proportional to our own rate of change, which the loop reads back
        // as target velocity, and no value of kf repairs it.
        // Added even when `frozen`: a target crossing the stop band at speed is
        // exactly what the feed-forward exists to hold.
        float ff = 0.0f;
        if (kf > 0.0f && !ffInhibit) {
            const float af = dt / (dt + kFfTauSec);
            const float dmeas = (error - lastError) + uHist[uHead] / kf;
            ffVel += af * (dmeas - ffVel);
            ff = std::clamp(kf * ffVel, -outLimit, outLimit);
        }
        ffInhibit = false;

        // ── Integral: conditional on saturation, on its own leash ───────────
        // Stop accumulating while the output is already saturated the way the
        // error wants to push it. Without this the integral builds a reserve
        // behind a clipped output and spends it later — the "large, slow sway".
        // The feed-forward is a legitimate part of `raw` here: it is a command,
        // not a state that can wind up.
        const float raw = p + integral + d + ff;
        const bool pushHi = raw >=  outLimit && error > 0.0f;
        const bool pushLo = raw <= -outLimit && error < 0.0f;
        if (!frozen && !pushHi && !pushLo) {
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
        for (int i = 0; i < kFfDelaySteps; ++i) uHist[i] = 0.0f;
    }

    /// Read-only views for the diagnostic log.
    ///
    /// `ffValue()` is THE number to watch on a moving target: it should settle at
    /// about ΔT/alpha (== kf·ŵ) and HOLD there. Near zero while the error trails
    /// the target means F is not running (kf = 0) or is being reset every frame.
    ///
    /// `integralValue()` is the residual cleaner while F runs: it should stay
    /// SMALL. A large trim with a small `ff` on a moving target means kf is
    /// wrong — the ratio between the two is roughly how wrong.
    float ffValue()        const { return kf * ffVel; }
    float integralValue()  const { return integral; }
    float lastErrorValue() const { return lastError; }
    /// The derivative's CONTRIBUTION to this step's output, in px.
    float derivPx()        const { return kd * ramp * deriv; }
    /// The output ceiling in force, finger px/step (a constant).
    float outLimitPx()     const { return outLimit; }
    /// The feed-forward gain in force.
    float ffGainValue()    const { return kf; }

private:
    float kp        = 0.10f;
    float ki        = 0.5f;
    float kd        = 0.20f;
    float outSmooth = 1.0f;
    float kf        = 3.0f;

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

    float uHist[kFfDelaySteps] = {0.0f};  // our own commands, for u(k−L)
    int   uHead    = 0;                   // index of the OLDEST entry
};

}  // namespace tracking
}  // namespace aimbotng
