// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::tracking::PredictivePid — the aim's closed-loop controller.
//
//  ROUND 22. This replaces the PIDF of rounds 1–21 wholesale, not by retuning
//  it. The old controller is still on disk at tracking/pid_controller.h and is
//  no longer included by anything, so the whole change reverts with one line.
//
//  WHY THE REPLACEMENT, in one paragraph. The old loop needed ONE number it
//  could not have: alpha, the game's sensitivity in view px per finger px. It
//  estimated that number on-line, and every fault the user reported for twelve
//  rounds came out of that estimate being (a) wrong and (b) MOVING:
//
//    * the feed-forward gain was kf/alpha_hat, so the estimate's error was
//      multiplied into the loop's own gain — a time-varying gain, which is a
//      jitter source by definition and cannot be tuned away;
//    * the integral was dragged toward carry = ffVel/alpha_hat, so the speed
//      the trim was asked for was 1/alpha_hat times a noisy reconstruction —
//      with the seed 0.10 against a true 0.30 that target is the WRONG SIGN,
//      which turns a discharge into an oscillator driver;
//    * the stop detector's threshold was also divided by alpha_hat, so a small
//      estimate made the detector unable to ever fire.
//
//  This controller has NO alpha in it at all, in any denominator. It gets the
//  target's velocity the way the reference implementation does:
//
//      r = Δe + alpha_f · u(k−L)          (screen px per frame)
//
//  which is "how much the error moved" plus "how much of that we caused
//  ourselves, put back". alpha_f is a FROZEN constant, not an estimate, and —
//  this is the point — **nothing that decides the loop's behaviour depends on
//  its value**:
//
//      at a steady lock  Δe = 0  ⇒  r = alpha_f·u  ⇒  ff = lookahead·u
//
//  so the feed-forward supplies exactly `lookahead` of the carrier whatever
//  alpha_f is. The one place alpha_f does enter is the transient self-copy,
//  whose coefficient is
//
//      s = lookahead · (1 − alpha_true / alpha_f)
//
//  and the whole design is built so that |s| cannot be the thing that decides
//  whether the loop is stable: lookahead is capped at kLookaheadMax = 0.80, and
//  a wrong-alpha_f contributes a factor, not an exponent. Getting alpha_f wrong
//  LOW is safe (|1 − ρ| < 1); getting it wrong HIGH is the risk, which is why
//  the constant is 0.10 — the low end of what a touch game can be — and why
//  the feed-forward is hard-clamped at kFfLimitPx.
//
//  The stability rule this buys, stated plainly and worth remembering because
//  it needs no model of the game at all: **the feed-forward is a delayed copy
//  of our own output with coefficient `lookahead`, so `lookahead < 1` is the
//  margin.** 0.80 is 20 % inside it, with the two velocity filters and the
//  trust gate below adding margin on top.
//
//  WHERE THE CARRIER LIVES, which is the structural change. The old loop asked
//  the INTEGRAL to hold the standing command u_ss = w/alpha and treated the
//  feed-forward as a trim — which is why the integral could never be cleared
//  (clearing it removed the carrier) and why the charge left over when a target
//  stopped had nowhere to go but out through the aim. Here the PREDICTION TERM
//  is the carrier (`ff = lookahead·v̂/sens`) and the integral is only a
//  residual cleaner, small by construction and free to be thrown away:
//
//    * the error is snapped to 0 within kErrorEpsPx, and then
//    * `!sameSign(error, integral) ⇒ integral = 0` — with the error at zero,
//      the charge is dropped outright. That single line is what removes the
//      "target stopped ⇒ the aim sways" fault: at rest there is no charge left
//      to overshoot with, and no oscillator to keep feeding;
//    * the integral leaks kIntegralLeakPerFrame per frame regardless;
//    * its adaptive gain iGain rises from 0, so a fresh engagement starts with
//      no integral authority at all.
//
//  UNITS. The caller's error is in SCREEN px and the returned output is in
//  FINGER px, exactly as the page has always wired it, so kp/ki/kd keep the
//  meaning the user already knows and his stored tuning stays readable. alpha_f
//  is the only bridge, and it appears in exactly two places: the reconstruction
//  above and the jump thresholds below.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aimbotng {
namespace tracking {

// ── The bridge to the plant, and the loop's own limits ──────────────────────

/// View px per finger px. FROZEN — never estimated, never adapted. See the
/// header note: nothing the loop's behaviour depends on is a function of this
/// value; it only scales the reconstruction's transient term. Low is safe
/// (the self-copy coefficient shrinks), high is the risk (it grows), so this
/// sits at the low end of a touch game's plausible range.
///
/// A symptom that this is too HIGH: the aim kicks sideways the instant the
/// target reverses, or sways on a standing target with the feed-forward up.
/// Lower it. Never raise it to "make the aim faster" — raise 前馈 for that.
constexpr float kSensFrozen = 0.10f;

/// The output ceiling, finger px/step, applied as a tanh knee so the command
/// stays linear where it works and saturates where a re-lock would otherwise
/// fling the finger across the panel. 260 finger px/step at 120 Hz is
/// 31200 finger px/s, i.e. kSensFrozen·260·120 = 3120 view px/s reachable.
///
/// Raised 180 → 260 after the simulation showed the ceiling clipping a 1200
/// px/s target's carrier (the loop was told to run out of command before the
/// target ran out of speed, which reads on screen as "跟不上"). The tanh knee
/// is unchanged — κ ≈ 0.55 across the whole working range either way, so the
/// only thing this number decides is where saturation starts.
constexpr float kAimOutLimitPx = 260.0f;

/// The integral's leash, finger px. NOT a safety rail in the old sense: the
/// integral here is a residual cleaner whose steady state is
/// kIntegralLeak-driven (≈ 66 × e × ki), and this only bounds it. It is set
/// well above anything a cleaner needs so it can never be the thing that
/// limits tracking — the feed-forward is what carries a moving target.
constexpr float kAimTrimLimitPx = 400.0f;

/// The feed-forward's own hard clamp, finger px/step. A rail against a wrong
/// kSensFrozen and nothing else: at the shipped lookahead the term settles
/// around lookahead·u_ss, which is under this for every target the output
/// ceiling can serve.
constexpr float kFfLimitPx = 120.0f;

/// 前馈 — the lookahead, in frames. THE carrier knob, and the one number in
/// this file that the user tunes for tracking:
///
///     ff = lookahead · v̂ / kSensFrozen        (finger px)
///
/// At a steady lock v̂ = kSensFrozen·u, so the feed-forward supplies exactly
/// `lookahead` of the standing command and the error closes to whatever P and
/// I need to make up the rest:
///
///     lookahead 0.00  the integral carries everything — the old failure mode
///     lookahead 0.80  the shipped value: 80 % of the carrier, 20 % left to
///                     P/I, i.e. a small standing error and a very large
///                     stability margin
///     lookahead 1.00  exactly full carrier; marginal (|s| = 1 at alpha_f
///                     exact) and NOT reachable from the slider
///
/// So this is the same "share of the carrier" idea the old kf row had, and it
/// is deliberately named the same way on the page. What changed is that the
/// share no longer runs through a 1/alpha_hat.
constexpr float kLookaheadMax     = 0.80f;
constexpr float kLookaheadDefault = 0.80f;

// ── Timing ─────────────────────────────────────────────────────────────────

constexpr float kNominalDt     = 1.0f / 120.0f;
constexpr float kMinDt         = 0.001f;
constexpr float kMaxDt         = 0.050f;
constexpr float kFrameScaleMin = 0.35f;
constexpr float kFrameScaleMax = 2.50f;

// ── Error conditioning (all in SCREEN px) ──────────────────────────────────

/// Below this the error is snapped to zero. This is not a deadzone — it is the
/// trigger for "we are on target", and it is what drops the integral (see
/// kSameSign note in updateIntegral). 0.3 view px is a fifth of a pixel on a
/// 1080-wide screen, i.e. it fires when the aim has genuinely arrived and not
/// before.
constexpr float kErrorEpsPx = 0.30f;

/// |iTotal| below this is not added at all, and |predictionTerm| below half of
/// it is dropped: output units (finger px), and the last line of defence
/// against sub-pixel chatter.
constexpr float kOutDeadbandPx = 0.30f;

/// OneEuroFilter tuning for the error. Low cutoff (this is a POSITION being
/// denoised, not a control signal), beta scales the cutoff with the filtered
/// derivative so a real swing is not lagged.
constexpr float kErrMinCutoff     = 1.05f;
constexpr float kErrBeta          = 0.17f;
constexpr float kErrDerivCutoff   = 1.15f;

/// The raw/filtered error blend. A large one-step step (a re-association) or a
/// large distance both push the controller toward the RAW value, so a fast
/// approach is not smoothed into mush by the filter above.
constexpr float kFastStepBlendStart = 6.0f;
constexpr float kFastStepBlendFull  = 30.0f;
constexpr float kFarErrorBlendStart = 10.0f;
constexpr float kFarErrorBlendFull  = 60.0f;
constexpr float kFarErrorBlendMax   = 0.65f;

/// Jump detection, SCREEN px. A step this big is not motion — it is the tracker
/// re-associating or the selection switching enemies. Split by size: above the
/// first, drop the transient state; above the second, throw everything away.
/// The caller also tells us directly via `measurementCorrected`/`targetChanged`,
/// which is strictly better information than a magnitude, so these two are the
/// fallback for callers that do not.
constexpr float kMeasurementJumpStep = 30.0f;
constexpr float kHardTargetSwitchStep = 120.0f;
/// What survives a soft jump: the charge is cut to a quarter, the velocity gain
/// halves, the memory is bled. Not zero — a head box and a body box of the same
/// enemy are two tracks, and wiping on every one of those would keep the
/// integral permanently empty.
constexpr float kJumpIntegralKeep = 0.25f;
constexpr float kJumpTrustKeep    = 0.25f;

// ── Velocity reconstruction ────────────────────────────────────────────────

/// The velocity filters. A Kalman pair (q = 0.1) rather than a single EMA: the
/// reconstruction is a difference of two large numbers and the filter is what
/// keeps its noise from reaching the output, while the q/r ratio leaves enough
/// bandwidth for a real direction change to come through inside ~6 frames.
constexpr float kVelKalmanQ = 0.1f;
constexpr float kVelKalmanR = 1.0f;

/// Below this the reconstructed velocity is taken to be zero (SCREEN px/frame,
/// i.e. 60 px/s). A target that slow is a target standing still, and zeroing it
/// is what lets the feed-forward release cleanly instead of chasing noise.
constexpr float kVelZeroPx = 0.5f;

/// The FIR the velocity is finally passed through: 6 samples, weights rising
/// toward the newest. This is not just smoothing — it is what makes the
/// estimate LAG deliberately, which is the right thing to do with a signal that
/// has our own output in it.
constexpr std::size_t kPredSamples     = 6;
constexpr std::size_t kPredHistoryCap  = 16;
constexpr std::size_t kPredHistoryMask = kPredHistoryCap - 1;
constexpr int         kPredStableReady = 2;
constexpr float kPredWeights[kPredSamples] = {0.05f, 0.09f, 0.13f, 0.17f, 0.24f, 0.32f};

// ── The stop / reversal machinery (this is what replaced carry) ─────────────

/// Speeds below this (SCREEN px/frame) cannot produce a meaningful ratio, so
/// the reversal and drop tests are skipped. 2.5 px/frame = 300 px/s.
constexpr float kDirChangeVelPx = 2.5f;

/// `drop` is the DIMENSIONLESS deceleration — (|v_prev| − |v|)/|v_prev| — and
/// `reversal` the normalised sign flip. Both are ratios, so neither carries a
/// unit and neither can be disabled by a wrong kSensFrozen. That property is
/// the whole reason the stop detection moved here: the old detector's test was
/// `|carry| < 1 px` with carry = ffVel/alpha_hat, so a small alpha_hat made the
/// gate impossible to satisfy and the charge could never be released.
constexpr float kStopGateStrength    = 3.6f;   // exp(−kStopGateStrength·drop)
constexpr float kReversalGateStrength = 5.2f;  // exp(−kReversalGateStrength·reversal)

/// …but a ratio fed from a noisy per-frame difference is never zero, and both
/// of these need a DEADBAND before they are believed. The velocity estimate
/// wobbles by a few percent from frame to frame even on a rock-steady strafe,
/// and `drop = 0.05` already costs 16% of the trust — exp(−3.6·0.05) = 0.84 —
/// so with no deadband the gate permanently reads "the target is slowing down"
/// on a target doing nothing of the sort, and the carrier is taxed for it.
/// Subtracting a band first means the gate reacts only to decelerations and
/// sign flips big enough to be real. The cost is that a genuinely gentle stop
/// is ignored; that is the right trade, because a gentle stop barely needs its
/// memory released while a hard one is exactly what overshoots.
constexpr float kStopDropDeadband = 0.25f;     // drop below this is estimation noise
constexpr float kReversalDeadband = 0.20f;     // reversal below this is estimation noise

/// How much memory a reversal or a stop is allowed to erase, as a fraction of
/// the reversal/drop amount. Velocity memory and integral memory bleed
/// separately because they should: the velocity estimate is rebuilt in ~8
/// frames and can be cut hard, whereas cutting the integral hard is what makes
/// a stop look like a step.
constexpr float kVelMemBleedStrength      = 0.90f;
constexpr float kVelMemBleedMax           = 0.90f;
constexpr float kIntegralMemBleedStrength = 0.95f;
constexpr float kIntegralMemBleedMax      = 0.95f;

// ── The trust gate: the negative feedback on this controller's own fault ────

/// Trust measures how much the velocity estimate looks like a real velocity
/// rather than like a ring: it falls with |Δv̂|, with a reversal and with a
/// drop, and rises only while the estimate is stable. It gates the
/// feed-forward, so the loop's response to its own instability is to remove its
/// own excitation — the one mechanism the old controller never had, because
/// there the same fault was AMPLIFIED (carry → the integral) instead.
/// `kTrustVelScale` must be the scale of a TYPICAL per-frame speed, not of a
/// fast one. It is the knee of speedFactor = 1 − exp(−|v|/this), i.e. the
/// speed at which the gate opens to 63%. At 8.0 a 600 px/s target (5 px/frame)
/// only buys speedFactor = 0.47 — the gate dutifully threw away half the
/// carrier on a perfectly ordinary strafe, which the simulation measured as a
/// 8.8 px lag. At 0.8 the same strafe gets 0.998 and the lag is 2.5 px.
/// "Gate the uncertainty, not the motion" — a target moving at all is
/// evidence FOR the estimate, so the gate should be open by then.
constexpr float kTrustVelScale      = 0.8f;    // speedFactor = 1 − exp(−|v|/this)
constexpr float kTrustAccelScale    = 6.0f;    // stableVelocity = exp(−|Δv|/this)
/// The distance factor is the OTHER half of the same mistake. 0.42 meant a
/// dead-on lock (|e| = 0) still had 42% of its carrier withheld, and 22.0 px
/// meant it took a nearly-framed target to earn it back — so a tight lock sat
/// at ~0.6 trust and paid a permanent 40% carrier tax. 0.70 / 10.0 puts an
/// ordinary 2 px lock at 0.94. The floor stays deliberately below 1 because a
/// perfectly centred estimate really is the one case with no independent
/// evidence behind it.
constexpr float kTrustDistFloor     = 0.70f;   // trust floor at zero error
constexpr float kTrustDistErrScale  = 10.0f;   // …and how fast it rises with |e|
constexpr float kTrustRiseRate      = 0.32f;
constexpr float kTrustFallRate      = 0.72f;   // falls faster than it rises

/// OUR DELIBERATE DEVIATION from the reference. The reference lets the trust
/// gate take the feed-forward to zero, and with the carrier living in the
/// feed-forward a zero gate means "no carrier at all" — the aim would stop
/// following whenever the target moved erratically, which is the user's own
/// complaint restated. So trust can cut the carrier to this floor and no
/// further. 0.25 keeps 25 % of the carrier through the worst transient, which
/// is enough for P/I to work against.
constexpr float kFfTrustFloor = 0.25f;

/// Adaptive gain on P and D while the estimate is trusted. A small boost only;
/// the point is that near-target damping rises when the error is small and the
/// error is changing, which is exactly the terminal phase of an approach.
constexpr float kKpTrustBoost = 0.20f;
constexpr float kKdTrustBoost = 0.40f;
constexpr float kKdNearBoost  = 0.20f;
constexpr float kNearDampErrScale    = 12.0f;
constexpr float kIntegralErrScale    = 8.0f;
constexpr float kIntegralDiffScale   = 3.0f;
/// The integral's authority as a function of the error: it is deliberately
/// reduced when far off (anti-wind-up without a clamp) and when the estimate is
/// trusted (because then the feed-forward is doing the work).
constexpr float kIntegralScaleFloor = 0.25f;
constexpr float kIntegralTrustCut   = 0.65f;

// ── The integral ───────────────────────────────────────────────────────────

/// Per-frame leak. At 120 Hz this is a 0.66 s time constant, so a charge that
/// is no longer justified decays on its own — no detector required.
constexpr float kIntegralLeakPerFrame = 0.985f;

/// The adaptive integral gain. It starts at ZERO and rises toward
/// 1 − |e|/threshold while the error is small, i.e. it only gains authority
/// once the aim is close. Together with the leak this is why a fresh engagement
/// cannot wind up.
constexpr float kIntegralGainThreshold = 50.0f;
constexpr float kIntegralGainRate      = 0.025f;

/// The prediction gain, same shape and same reason: the carrier RAMPS IN over
/// roughly 0.4 s instead of arriving in one step. A step in the carrier is a
/// step in output velocity, which is the "乱甩" fault of round 15 — so it
/// ramps, exactly as the kf guard did, but with no estimator and no latch.
constexpr float kPredGainRate = 0.025f;

// ── The output gain ────────────────────────────────────────────────────────

/// A single global gain on the whole output, 0.35…1.0, adapting with the error.
/// This is the "others' pull is not fast" knob, in the reference's own terms: a
/// re-lock starts at 0.45–0.70 (soft) and tightens toward 1.0 as the error
/// closes. It costs nothing at a steady lock (it is 1.0 there) and it is what
/// keeps a switch from flinging.
constexpr float kOutGainFloor      = 0.35f;
constexpr float kInitOutGainNear   = 0.45f;
constexpr float kInitOutGainFar    = 0.70f;
constexpr float kInitOutGainErrLo  = 8.0f;
constexpr float kInitOutGainErrHi  = 80.0f;
constexpr float kOutGainThreshold  = 640.0f;
constexpr float kOutGainRate       = 0.03f;

constexpr float kMaxPidGain = 200.0f;
constexpr float kMaxKiGain  = 20.0f;
constexpr float kEps        = 1e-6f;

// ── Small helpers ──────────────────────────────────────────────────────────

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float clampRange(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool  sameSign(float a, float b) { return (a * b) > 0.0f; }
inline float round2(float v) { return std::round(v * 100.0f) / 100.0f; }

/// A one-pole low-pass that reports whether it has ever been seeded.
struct LowPass {
    bool  init = false;
    float value = 0.0f;
    float update(float in, float a) {
        if (!init) { init = true; value = in; return value; }
        value += a * (in - value);
        return value;
    }
    void reset() { init = false; value = 0.0f; }
};

/// The OneEuro filter, for the error. Slow where the signal is slow (so jitter
/// dies), fast where it is fast (so the terminal approach keeps its shape).
struct OneEuro {
    bool  init = false;
    LowPass vf;
    LowPass df;

    static float alphaFor(float cutoff, float dt) {
        if (cutoff <= 0.0f || dt <= 0.0f) return 1.0f;
        const float tau = 1.0f / (6.28318531f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }

    float update(float in, float dt) {
        if (!init) { init = true; vf.update(in, 1.0f); df.update(0.0f, 1.0f); return in; }
        const float d  = (in - vf.value) / dt;
        const float fd = df.update(d, alphaFor(kErrDerivCutoff, dt));
        const float cutoff = kErrMinCutoff + kErrBeta * std::fabs(fd);
        return vf.update(in, alphaFor(cutoff, dt));
    }

    void reset() { init = false; vf.reset(); df.reset(); }
};

/// A scalar Kalman filter, q on the process and r on the measurement.
struct ScalarKalman {
    float q = 1.0f, r = 1.0f, x = 0.0f, p = 1.0f;

    float update(float meas, float dt) {
        const float n = clampRange(dt / kNominalDt, kFrameScaleMin, kFrameScaleMax);
        const float pp = p + q * n;
        const float k  = pp / (pp + r);
        x += (meas - x) * k;
        p = (1.0f - k) * pp;
        return x;
    }
    void reset() { x = 0.0f; p = 0.0f; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  PredictivePid — one axis.
// ─────────────────────────────────────────────────────────────────────────────

class PredictivePid {
public:
    PredictivePid() = default;

    // ── Parameters: PURE ASSIGNMENT, NEVER A RESET ─────────────────────────
    //
    // Called once per render frame from the page. A version that cleared state
    // here (the page's own history: it once ended in reset()) would wipe the
    // integrator and the velocity memory 120 times a second. A genuinely new
    // engagement calls reset() instead.
    //
    // kp / ki / kd keep the page's existing units — px of output per px of
    // error, per second, and per step respectively — so a value the user
    // arrived at by tuning is still that value here.
    void setParams(float kp, float ki, float kd, float outSmooth, float lookahead) {
        kp_ = clampRange(kp, 0.0f, kMaxPidGain);
        ki_ = clampRange(ki, 0.0f, kMaxKiGain);
        kd_ = clampRange(kd, 0.0f, kMaxPidGain);
        outSmooth_ = clamp01(outSmooth);
        lookahead_ = clampRange(lookahead, 0.0f, kLookaheadMax);
    }

    /// The engaged target's size in SCREEN px (the box's longer side). Used only
    /// to express the jump thresholds as a fraction of a target rather than as a
    /// fixed number of pixels, so a distant 20 px box and a contact-range 300 px
    /// box are judged the same way. 0 disables the box-relative part.
    void setTargetBoxPx(float px) { boxPx_ = (px > 0.0f) ? px : 0.0f; }

    // ── The control step ───────────────────────────────────────────────────

    /// `error`      — target − crosshair, SCREEN px (positive = target to the right/below)
    /// `dt`         — real step time, seconds
    /// `frozen`     — the caller's per-axis "inside the stop band" decision
    /// `targetChanged`       — the caller picked a DIFFERENT track this step
    /// `measurementCorrected` — the tracker re-associated this step, so Δe
    ///                          contains a correction kick and is NOT a velocity
    ///
    /// Returns the FINGER displacement for this step, px.
    float update(float error, float dt, bool frozen,
                 bool targetChanged, bool measurementCorrected) {
        if (!std::isfinite(error)) { reset(); return 0.0f; }

        dt = clampRange(dt, kMinDt, kMaxDt);
        const float dtScale = dt / kNominalDt;   // 1.0 at the loop's nominal rate

        // ── Error conditioning ────────────────────────────────────────────
        // Snap-to-zero first: it is the "we have arrived" signal, and the
        // integral's sign test below reads it.
        float raw = error;
        if (std::fabs(raw) < kErrorEpsPx) raw = 0.0f;

        bool hardReset = false;
        bool softJump  = false;
        if (targetChanged) {
            reset();
            hardReset = true;
        } else if (hasPrevRaw_) {
            const float step = std::fabs(raw - prevRaw_);
            const float soft = std::fmax(kMeasurementJumpStep, 0.5f * boxPx_);
            const float hard = std::fmax(kHardTargetSwitchStep, 1.5f * boxPx_);
            if (step >= hard)      { reset(); hardReset = true; }
            else if (step >= soft) { softReset(); softJump = true; }
        }

        const float filtered = errFilter_.update(raw, dt);
        const float blend    = rawBlend(raw, hasPrevRaw_ ? prevRaw_ : lastError_);
        const float e        = round2(filtered + (raw - filtered) * blend);

        // ── Derivative ────────────────────────────────────────────────────
        //
        // Zeroed on a jump. A re-association is not motion, and letting it into
        // the D term is a one-frame spike of exactly the size of the jump —
        // kd·(that) is hundreds of pixel of finger travel for a single step.
        const float de    = (hardReset || softJump) ? 0.0f : (e - lastError_);
        const float absE  = std::fabs(e);
        const float absDe = std::fabs(de);
        // Per-step difference normalised to the nominal step, so the D term and
        // the reconstruction keep their meaning when the loop changes rate.
        const float dNorm = de * (kNominalDt / dt);

        // ── Adaptive gains (all driven by the error, all rate-limited) ─────
        iGain_    = adjustGain(iGain_,    absE, kIntegralGainThreshold, kIntegralGainRate);
        // NOT adjustGain — deliberately. The feed-forward is the CARRIER, so
        // throttling it with the error is a positive feedback loop: a large
        // error suppresses the very term that exists to remove it, so the error
        // grows and the term shrinks further. The simulation measured exactly
        // that: a hard 33 px standing lag on a 600 px/s strafe with predGain
        // pinned at 0.6 and trust down at 0.41. A term that carries the target
        // may only ramp UP, at a bounded rate; the one thing allowed to take it
        // back down is the trust gate upstream, and only on evidence about the
        // ESTIMATE (|Δv̂|, reversal, deceleration) — never on the size of the
        // error it is there to close.
        predGain_ = clamp01(predGain_ + (1.0f - predGain_) * kPredGainRate);
        if (!hasPrevRaw_) outGain_ = std::fmax(outGain_, initOutGainFor(absE));
        outGain_  = adjustGain(outGain_,  absE, kOutGainThreshold, kOutGainRate);

        // ── The velocity reconstruction ───────────────────────────────────
        //
        //   r = Δe + kSensFrozen·u_prev
        //
        // "How far the error moved, plus how much of that was us." At a steady
        // lock Δe = 0 and r = kSensFrozen·u, so the feed-forward below supplies
        // exactly `lookahead` of the carrier — the property the whole design
        // rests on, and the one thing the old loop could not have because its
        // u_prev was scaled by an estimate that moved.
        //
        // SKIPPED ENTIRELY on a corrected step. When the tracker pulls the
        // published centre back onto a detector result one render frame old,
        // Δe is the model's own velocity PLUS that pull, and the pull is
        // proportional to how far WE rotated. Feeding it in would be the loop
        // reading its own handwriting as target motion — at gain `lookahead`
        // here and (in the old design) at gain 1 through carry into the
        // integral, which is what made "ki = 0.01 and it still shakes"
        // possible. Holding last step's estimate for one frame costs nothing:
        // the filters below are already ~6-frame devices.
        float vel = velLast_;
        if (!measurementCorrected && lookahead_ > 0.0f) {
            const float meas = dNorm + kSensFrozen * lastOutput_;
            float fv = velFilter_.update(meas, dt);

            // Near a lock, weight the estimate toward "what my own command and
            // the residual error imply" — the endpoint of a settle is where the
            // reconstruction is least trustworthy, because both of its terms are
            // small and it is their DIFFERENCE that was supposed to be the
            // signal.
            if (absE < 1.0f && absDe < 0.1f) {
                fv = fv * 0.35f + (lastOutput_ * 0.5f + dNorm) * 0.65f;
            }

            float sv = velSmooth_.update(fv, dt);
            if (std::fabs(sv) <= kVelZeroPx) sv = 0.0f;

            vel = pushVelocity(sv);
            velFresh_ = true;
        }

        // ── Stop / reversal machinery ─────────────────────────────────────
        Adaptive ad = computeAdaptive(absE, absDe, vel, velLast_, prevTrust_);
        prevTrust_ = ad.trust;
        velNow_    = vel;
        velLast_   = vel;

        if (transferBleed_ > 0.0f) {
            integral_ *= transferBleed_;
            iGain_    *= transferBleed_;
            predGain_ *= transferBleed_;
            transferBleed_ = 0.0f;
        }
        if (ad.velMemScale < 0.999f) scalePredictionMemory(ad.velMemScale);
        if (ad.intMemScale < 0.999f) {
            // Denominator form: the integral is a residual cleaner with no
            // external target, so cutting it is a pure gain reduction with no
            // side effect on the carrier — the property the old `carry` bleed
            // was missing, and the reason the old one could drive the charge
            // past zero and into the opposite sign.
            integral_ *= ad.intMemScale;
            iGain_    *= ad.intMemScale;
        }

        // ── Integral ──────────────────────────────────────────────────────
        const float iTerm = updateIntegral(e, frozen, dtScale);

        // ── The output sum ───────────────────────────────────────────────
        //
        // Frozen (inside the stop band) suppresses P and D only. It must NOT
        // touch the feed-forward: the band's job is to stop the loop chasing
        // the detector's box jitter, and the feed-forward is not chasing
        // anything — it is the command a moving target requires. Suppressing it
        // is round 15's fault, where the band zeroed the axis outright and read
        // on the device as 乱甩乱晃.
        const float gate = frozen ? 0.0f : 1.0f;
        const float pT   = e * kp_ * (1.0f + kKpTrustBoost * ad.trust) * gate;
        const float dT   = dNorm * kd_ *
                           (1.0f + kKdTrustBoost * ad.trust) * gate;
        float iOut = iTerm * ad.intScale;

        // The prediction term, in FINGER px:
        //   (lookahead · v̂ [screen px/frame]) / kSensFrozen = finger px.
        // predictionScale is the trusted-lookahead boost and the trust gate;
        // the floor in kFfTrustFloor is what keeps the carrier alive through a
        // transient. See kFfTrustFloor.
        const float ffGate = kFfTrustFloor + (1.0f - kFfTrustFloor) * ad.trust;
        float ff = (lookahead_ * predGain_ * ad.predScale / kSensFrozen) * vel * ffGate;
        ff = clampRange(ff, -kFfLimitPx, kFfLimitPx);

        // Deadbands on the two terms whose steady state is zero. P and D are
        // deliberately exempt — they are the loop's restoring force, and gating
        // them is the bang-bang the page's own history already paid for once.
        if (std::fabs(iOut) <= kOutDeadbandPx)          iOut = 0.0f;
        if (std::fabs(ff)   <= kOutDeadbandPx * 0.5f)   ff   = 0.0f;

        float out = pT + iOut + dT + ff;

        // Soft ceiling, then the global gain, then the optional output EMA.
        out = kAimOutLimitPx * std::tanh(out / kAimOutLimitPx);
        out = round2(out * outGain_);
        if (outSmooth_ < 0.999f) {
            outSmoothState_ += (out - outSmoothState_) * outSmooth_;
            out = outSmoothState_;
        }

        // `lastOutput_` is what the PLANT saw — the post-gain, post-EMA value.
        // The reconstruction's next step reads it, so it must be the physical
        // command and not the pre-gain sum.
        lastOutput_  = out;
        lastError_   = e;
        prevRaw_     = raw;
        hasPrevRaw_  = true;

        pDisp_ = pT;
        dDisp_ = dT;
        iDisp_ = iOut;
        fDisp_ = ff;
        return out;
    }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// A new engagement, a target switch, or a jump. Wipes EVERYTHING, including
    /// the output gain and the velocity memory — the state that belongs to the
    /// target we were engaging and to nothing else.
    void reset() {
        integral_  = 0.0f;
        iGain_     = 0.0f;
        predGain_  = 0.0f;
        outGain_   = kOutGainFloor;
        lastError_ = 0.0f;
        prevRaw_   = 0.0f;
        hasPrevRaw_ = false;
        lastOutput_ = 0.0f;
        outSmoothState_ = 0.0f;
        prevTrust_ = 0.0f;
        velNow_ = 0.0f;
        velLast_ = 0.0f;
        transferBleed_ = 0.0f;
        velFresh_ = false;
        resetPredictionMemory();
        errFilter_.reset();
        velFilter_.reset();
        velSmooth_.reset();
        pDisp_ = dDisp_ = iDisp_ = fDisp_ = 0.0f;
    }

    // ── Telemetry (for the aim log line; no effect on the loop) ─────────────

    /// The integral's actual contribution to the output, finger px. Small is
    /// healthy: the feed-forward carries a moving target, so this should stay
    /// near zero and return to zero at rest.
    float integralValue()  const { return iDisp_; }
    /// The prediction term's contribution, finger px. THE number to watch on a
    /// moving target: it should settle at lookahead·u_ss and hold.
    float ffValue()        const { return fDisp_; }
    /// The P and D contributions, finger px.
    float pValue()         const { return pDisp_; }
    float dValue()         const { return dDisp_; }
    /// The reconstructed target velocity, SCREEN px per frame.
    float velValue()       const { return velNow_; }
    /// The output gain in force, 0.35…1.0.
    float outGain()        const { return outGain_; }
    /// The trust in force, 0…1. Collapsing toward 0 means the velocity estimate
    /// is ringing — read it together with the aim's sway.
    float trustValue()     const { return prevTrust_; }
    /// The prediction gain in force, 0…1. While it is still climbing the
    /// carrier is still ramping in; that is the first fraction of a second of a
    /// session and not a fault.
    float predGainValue()  const { return predGain_; }
    /// True once enough consecutive velocity samples have agreed on a direction
    /// for the reconstruction to be published at all.
    bool  velocityReady()  const { return velFresh_; }
    /// The output ceiling in force, finger px/step. A constant.
    float outLimitPx()     const { return kAimOutLimitPx; }
    /// The lookahead in force, frames — for the log.
    float lookaheadValue() const { return lookahead_; }

private:
    // ── Adaptive state ────────────────────────────────────────────────────

    struct Adaptive {
        float trust      = 0.0f;
        float predScale  = 1.0f;
        float intScale   = 1.0f;
        float velMemScale = 1.0f;
        float intMemScale = 1.0f;
    };

    /// The ratio-driven state machine that replaced `carry`. `drop` and
    /// `reversal` are dimensionless, so unlike the old stop detector this one
    /// cannot be defeated by a wrong plant model.
    Adaptive computeAdaptive(float absE, float absDe, float vel,
                             float velPrev, float trustPrev) const {
        Adaptive a;
        const float av  = std::fabs(vel);
        const float ap  = std::fabs(velPrev);
        const float acc = std::fabs(vel - velPrev);

        const bool pair = (av > kDirChangeVelPx) && (ap > kDirChangeVelPx);
        float reversal = 0.0f;
        if (pair && (vel * velPrev) < 0.0f) {
            reversal = -(vel * velPrev) / (av * ap + kEps);
        }

        float drop = 0.0f;
        if (ap > kDirChangeVelPx && ap > av) {
            drop = (ap - av) / (ap + kEps);
        }

        // Believe neither ratio until it clears its deadband (see the constants
        // above). Everything downstream — both gates AND the memory bleed — uses
        // the excess, so a few percent of estimation wobble is simply invisible
        // instead of being a permanent tax on the carrier.
        const float reversalE = std::fmax(0.0f, reversal - kReversalDeadband);
        const float dropE     = std::fmax(0.0f, drop - kStopDropDeadband);

        const float distTrust = kTrustDistFloor +
                                (1.0f - kTrustDistFloor) *
                                clamp01(absE / kTrustDistErrScale);

        float target = 0.0f;
        if (av > kEps) {
            const float speedFactor = 1.0f - std::exp(-av / kTrustVelScale);
            const float stableVel   = std::exp(-acc / kTrustAccelScale);
            const float revGate     = (reversalE > 0.0f)
                                          ? std::exp(-kReversalGateStrength * reversalE)
                                          : 1.0f;
            const float stopGate    = (dropE > 0.0f)
                                          ? std::exp(-kStopGateStrength * dropE)
                                          : 1.0f;
            target = clamp01(speedFactor * stableVel * revGate * stopGate * distTrust);
        }
        const float rate = (target < trustPrev) ? kTrustFallRate : kTrustRiseRate;
        a.trust = clamp01(trustPrev + (target - trustPrev) * rate);

        a.predScale = 1.0f + kKpTrustBoost * a.trust;   // a small, bounded boost

        const float nearInt = std::exp(-absE / kIntegralErrScale);
        const float stableE = std::exp(-absDe / kIntegralDiffScale);
        a.intScale = clampRange(kIntegralScaleFloor +
                                 (1.0f - kIntegralScaleFloor) * nearInt * stableE,
                                0.0f, 1.0f);
        a.intScale *= (1.0f - kIntegralTrustCut * a.trust);

        // The bleed. A reversal or a stop is the one time the memory of "the
        // target was moving at v" is actively misleading, so it is cut by an
        // amount proportional to the reversal/drop and no more — again the
        // EXCESS over the deadband, so the estimate's own noise can never
        // spontaneously erase the memory of a real, steady target.
        const float rd = reversalE + dropE;
        a.velMemScale = 1.0f - clampRange(rd * kVelMemBleedStrength, 0.0f, kVelMemBleedMax);
        a.intMemScale = 1.0f - clampRange(rd * kIntegralMemBleedStrength, 0.0f, kIntegralMemBleedMax);
        return a;
    }

    static float adjustGain(float gain, float absE, float threshold, float rate) {
        if (threshold <= 0.0f) return clamp01(gain);
        float target, r;
        if (threshold <= absE) {
            target = (absE == 0.0f) ? 1.0f : threshold / absE;   // suppress when far off
            r = 0.1f;
        } else {
            target = 1.0f - absE / threshold;
            r = rate;
        }
        return clamp01(gain + (target - gain) * r);
    }

    static float initOutGainFor(float absE) {
        const float blend = clamp01((absE - kInitOutGainErrLo) /
                                    (kInitOutGainErrHi - kInitOutGainErrLo));
        return kInitOutGainNear + (kInitOutGainFar - kInitOutGainNear) * blend;
    }

    static float rawBlend(float raw, float prev) {
        const float step = std::fabs(raw - prev);
        const float absE = std::fabs(raw);
        const float stepBlend =
            clamp01((step - kFastStepBlendStart) / (kFastStepBlendFull - kFastStepBlendStart));
        const float distBlend =
            clamp01((absE - kFarErrorBlendStart) / (kFarErrorBlendFull - kFarErrorBlendStart)) *
            kFarErrorBlendMax;
        return std::fmax(stepBlend, distBlend);
    }

    // ── The velocity FIR ──────────────────────────────────────────────────

    /// Push a new sample and return the published velocity. Below kPredSamples
    /// samples it publishes nothing (there is no direction yet), and it requires
    /// kPredStableReady consecutive samples agreeing on a SIGN before it
    /// publishes anything at all — a direction change resets the count and
    /// refills the window with the new value, which is what stops the FIR from
    /// reporting the average of "left" and "right" and calling it zero.
    float pushVelocity(float v) {
        predHist_[predHead_] = v;
        predHead_ = (predHead_ + 1) & kPredHistoryMask;
        if (predMoves_ < kPredSamples) ++predMoves_;
        if (predMoves_ < kPredSamples) return 0.0f;

        float weighted = 0.0f;
        std::size_t idx = (predHead_ - kPredSamples) & kPredHistoryMask;
        for (std::size_t i = 0; i < kPredSamples; ++i) {
            weighted += predHist_[idx] * kPredWeights[i];
            idx = (idx + 1) & kPredHistoryMask;
        }

        if (sameSign(lastPredVel_, weighted) || lastPredVel_ == 0.0f || weighted == 0.0f) {
            if (predStable_ < kPredStableReady) ++predStable_;
        } else {
            predStable_ = 0;
            for (std::size_t i = 0; i < kPredHistoryCap; ++i) predHist_[i] = weighted;
        }
        lastPredVel_ = weighted;
        return (predStable_ < kPredStableReady) ? 0.0f : weighted;
    }

    void scalePredictionMemory(float scale) {
        scale = clampRange(scale, 0.0f, 1.0f);
        if (predMoves_ == 0 && lastPredVel_ == 0.0f) return;
        for (std::size_t i = 0; i < kPredHistoryCap; ++i) predHist_[i] *= scale;
        lastPredVel_ *= scale;
        if (scale < 0.35f) predStable_ = 0;
    }

    void resetPredictionMemory() {
        for (std::size_t i = 0; i < kPredHistoryCap; ++i) predHist_[i] = 0.0f;
        predHead_ = 0;
        predMoves_ = 0;
        predStable_ = 0;
        lastPredVel_ = 0.0f;
    }

    // ── The integral ──────────────────────────────────────────────────────

    /// `frozen` (the caller's stop band) pauses ACCUMULATION only. The leak, the
    /// sign test and the clamp all still run: the band means "stop adding to a
    /// charge", never "keep the charge forever".
    float updateIntegral(float e, bool frozen, float dtScale) {
        if (ki_ <= kEps) { integral_ = 0.0f; return 0.0f; }

        // THE LINE THAT REMOVES THE SWAY. The error is zero at rest (see
        // kErrorEpsPx), and a charge with no error behind it is dropped
        // outright — not bled toward some measured target, DROPPED. The old
        // controller could not do this because its integral was the carrier and
        // clearing it removed the loop's ability to follow; here the carrier is
        // the prediction term, so the integral is free to be thrown away, and
        // throwing it away is exactly what makes a stop quiet.
        if (integral_ != 0.0f && !sameSign(e, integral_)) {
            integral_ = 0.0f;
            iGain_    = 0.0f;
        }

        if (integral_ != 0.0f) {
            integral_ *= std::pow(kIntegralLeakPerFrame, clampRange(dtScale, kFrameScaleMin,
                                                                   kFrameScaleMax));
        }
        if (!frozen) integral_ += e * ki_ * iGain_ * dtScale;

        integral_ = clampRange(integral_, -kAimTrimLimitPx, kAimTrimLimitPx);
        return integral_;
    }

    // ── Parameters ────────────────────────────────────────────────────────
    float kp_        = 0.30f;
    float ki_        = 0.20f;
    float kd_        = 0.25f;
    float outSmooth_ = 1.0f;
    float lookahead_ = kLookaheadDefault;
    float boxPx_     = 0.0f;

    // ── State ─────────────────────────────────────────────────────────────
    float integral_  = 0.0f;
    float iGain_     = 0.0f;
    float predGain_  = 0.0f;
    float outGain_   = kOutGainFloor;
    float lastError_ = 0.0f;
    float prevRaw_   = 0.0f;
    bool  hasPrevRaw_ = false;
    float lastOutput_ = 0.0f;
    float outSmoothState_ = 0.0f;
    float velNow_    = 0.0f;    ///< published velocity this step (SCREEN px/frame)
    float velLast_   = 0.0f;    ///< the same, one step ago — the drop/reversal reference
    float prevTrust_ = 0.0f;
    bool  velFresh_  = false;
    float transferBleed_ = 0.0f;

    float predHist_[kPredHistoryCap] = {};
    std::size_t predHead_ = 0;
    std::size_t predMoves_ = 0;
    int   predStable_ = 0;
    float lastPredVel_ = 0.0f;

    OneEuro      errFilter_;
    ScalarKalman velFilter_{kVelKalmanQ, kVelKalmanR, 0.0f, 1.0f};
    ScalarKalman velSmooth_{kVelKalmanQ, kVelKalmanR, 0.0f, 1.0f};

    // Last step's contributions, for the log line only.
    float pDisp_ = 0.0f, dDisp_ = 0.0f, iDisp_ = 0.0f, fDisp_ = 0.0f;

    /// Used by the jump detector: a soft jump cuts the transient state without
    /// wiping the loop (see kJumpIntegralKeep).
    void softReset() {
        transferBleed_ = kJumpIntegralKeep;
        prevTrust_    *= kJumpTrustKeep;
        resetPredictionMemory();
        velFilter_.reset();
        velSmooth_.reset();
        velNow_ = 0.0f;
        velLast_ = 0.0f;
        velFresh_ = false;
    }
};

}  // namespace tracking
}  // namespace aimbotng
