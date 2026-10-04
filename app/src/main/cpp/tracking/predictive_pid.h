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
//  margin.** The default is 0.60 — safe for a plant gain anywhere in
//  (0, 2.67) — and the slider stops at 0.80.
//
//  WHERE THE CARRIER LIVES, which is the structural change. The old loop asked
//  the INTEGRAL to hold the entire standing command u_ss = w/alpha and treated
//  the feed-forward as a trim, which is why the integral could never be cleared.
//  Here the standing command is SPLIT:
//
//      ff = lookahead · u_ss        (the prediction term's share)
//      I  = (1 − lookahead) · u_ss  (the integral's share)
//
//  so the integral is no longer *the* carrier and can be made disposable when a
//  target stops — but it is not a formality either, and the first version of this
//  file got that wrong in a way that showed up on the device immediately. See the
//  two starred notes in updateIntegral(); the short version is that the integral
//  must be HELD while the aim is on target, because the prediction term is a copy
//  of our own output and cannot bootstrap its share without it:
//
//      u = lookahead·u(k−L) + I   ⇒   u → I/(1 − lookahead)
//
//  Three states, and only the first is "free":
//    * off target — leak kIntegralLeakPerFrame per frame and accumulate e·ki;
//    * ON TARGET (error snapped to 0 within kErrorEpsPx, or inside the caller's
//      deadzone) — HOLD. Neither accumulate nor leak: the charge equals the
//      carrier and there is no error behind it to wind anything up with;
//    * ERROR REVERSED — `integral = 0` outright. That is the release that makes
//      a stop quiet: at rest the target stops, the aim over-runs, the error
//      reverses, and the charge is gone in one step instead of overshooting
//      twice. ⚠ A ZERO error is NOT a reversal, however — sameSign(0, x) is
//      false, so a zero error reads as an opposing one and the carrier gets
//      wiped whenever the aim settles (and on every crossing);
//    * its adaptive gain iGain rises from 0, so a fresh engagement starts with
//      little integral authority.
//
//  UNITS. The caller's error is in SCREEN px and the returned output is in
//  FINGER px, exactly as the page has always wired it, so kp/ki/kd keep the
//  meaning the user already knows and his stored tuning stays readable.
//  kSensFrozen is the bridge, and it now equals 1.0 — the reference's own value
//  (`rawVelocity = Δe + lastOutput_`, unscaled). It appears in the reconstruction
//  and in the jump thresholds. At a steady lock it CANCELS out of the carrier
//  share, so tracking speed does not depend on it; what it does set is the noise
//  gain (lookahead/kSensFrozen) and the scale of v̂, which is why the trust gate's
//  thresholds are quoted against kSensFrozen and must be re-derived if it moves.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aimbotng {
namespace tracking {

// ── The bridge to the plant, and the loop's own limits ──────────────────────

/// View px per finger px. FROZEN — never estimated, never adapted. It is the
/// constant in the reconstruction `r = Δe + kSensFrozen·u(k−L)`: it says "how
/// much of the error's frame-to-frame change is OUR OWN previous command".
///
/// ★★ THE SAFE DIRECTION IS AN OVER-ESTIMATE, NOT AN UNDER-ESTIMATE. This file
/// previously claimed the opposite ("Low is safe") and that was backwards.
/// Writing α for the game's true value, the self-copy coefficient is
///
///     s = lookahead · (1 − α / kSensFrozen)
///
/// so kSensFrozen → 0 sends |s| → ∞ — the loop amplifies its own delayed output
/// without bound — while kSensFrozen → ∞ sends s → lookahead, which is bounded by
/// 1 for every setting of the slider. UNDER-estimating is the dangerous side.
///
/// ★★ AND IT SETS THE NOISE GAIN, WHICH IS WHY IT IS 1.0 AND NOT SMALLER. The
/// prediction term is ff = (lookahead/kSensFrozen)·v̂, so the detector's
/// frame-to-frame jitter reaches the COMMAND with gain lookahead/kSensFrozen. At
/// 0.10 that is 10× what the reference controller has. The reference adds its own
/// previous output to Δe UNSCALED — `rawVelocity = normalizedErrorDiff +
/// lastOutput_;`, pid_chirs.cpp:447 — and that one number, not any gain, is why
/// the first build of this controller read as "默认参数直接飞" on the device while
/// the vendor's controller is smooth on the same phone with the same sliders.
/// 1.0 is also the value with the smallest |s| for a game near α = 1, which is the
/// only kind of game the reference can serve at all.
///
/// Do not tune this, and do not read it as a performance knob: with 1.0 the DC
/// carrier share is still exactly `lookahead` (α cancels there), so nothing about
// 废除全局硬编码死值除数，对齐克里斯控制器统一误差尺度（定义 1.0 仅作符号兼容）
constexpr float kSensFrozen = 1.0f;

/// The output ceiling, finger px/step, applied as a tanh knee so the command
/// stays linear where it works and saturates where a re-lock would otherwise
/// fling the finger across the panel. 180 finger px/step at 120 Hz is
/// 21600 finger px/s, i.e. α·180·120 ≈ 21600 view px/s at α = 1 — two orders of
/// magnitude above anything a target can do.
///
/// ⚠ 180 → 260 → BACK TO 180. The raise was justified with "0.10·180·120 = 2160
/// view px/s cannot serve a 1200 px/s target", and that arithmetic was an artifact
/// of kSensFrozen being wrong by 10×: it silently assumed the loop reaches only
/// kSensFrozen of each finger px, which is not what that constant means. With 1.0
/// the same ceiling serves 21600 px/s. Same class of mistake as kTrustVelScale —
/// see the note there.
constexpr float kAimOutLimitPx = 180.0f;

/// The integral's leash, finger px. NOT a safety rail in the old sense: the
/// integral here is a residual cleaner whose steady state is leak-driven
/// (≈ 66 × e × ki while it is free to accumulate), and this only bounds it.
///
/// 400 → 150, back to this project's own proven value and roughly to the
/// reference's own integral ceiling, which is atan2-compressed at
/// (π/2)·(baseLimit − kiLimit) = (π/2)·90 ≈ 141 finger px for baseLimit 180.
/// 400 was chosen while kSensFrozen was 10× low, i.e. while a "finger px" in this
/// file was implicitly worth a tenth of one on the device; in real units 400 is a
/// quarter of the whole panel per step, and a charge that big is exactly the
/// energy the old controller's creep-snap limit cycle ran on.
constexpr float kAimTrimLimitPx = 150.0f;

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

/// 0.80 → 0.60, and the reason is which failure it is allowed to have. The
/// stability condition is |s| < 1 with s = lookahead·(1 − α); 0.80 is safe for
/// α ∈ (0, 2.25) and 0.60 is safe for α ∈ (0, 2.67), so the lower default simply
/// tolerates a wider range of games while still handing the prediction term more
/// than half the carrier. It costs the integral a bigger share — with the in-band
/// hold (see updateIntegral) that is no longer a way to lose the track, which is
/// what makes the conservative default affordable now and did not before.
/// The slider still reaches 0.80 for anyone who measures a quiet 0.80.
constexpr float kLookaheadDefault = 0.60f;

// ── Timing ─────────────────────────────────────────────────────────────────

constexpr float kNominalDt     = 1.0f / 120.0f;
constexpr float kMinDt         = 0.001f;
constexpr float kMaxDt         = 0.050f;
constexpr float kFrameScaleMin = 0.35f;
constexpr float kFrameScaleMax = 2.50f;

// ── Prediction Latency Compensation (克里斯控制器动力学延迟补偿) ───────────────
constexpr float kMaxPredictionLatencySeconds = 0.120f;  // 最大前瞻延迟 120ms
constexpr float kLatencyRiseSmoothing        = 0.65f;   // 延迟增大时快速跟随
constexpr float kLatencyFallSmoothing        = 0.12f;   // 延迟减小时慢速收敛，防剧烈回落
constexpr float kMaxPredictionLookaheadFrames = 4.50f;  // 最大前瞻 4.5 帧

// ── Error conditioning (all in SCREEN px) ──────────────────────────────────

/// Below this the error is snapped to zero. This is not a deadzone — it is the
/// trigger for "we are on target", and it is what drops the integral (see
/// kSameSign note in updateIntegral). 0.3 view px is a fifth of a pixel on a
/// 1080-wide screen, i.e. it fires when the aim has genuinely arrived and not
/// before.
constexpr float kErrorEpsPx = 0.30f;

/// |iTotal| below this is not added at all, and |predictionTerm| below half of
/// it is dropped. Output units (finger px) — sub-pixel chatter only.
///
/// ⚠ 0.30 → 0.05. The comment this replaces said the deadband exists "on the two
/// terms whose steady state is zero", and that premise is FALSE in this
/// architecture: the prediction term's steady state is `lookahead · u_ss` and the
/// integral's is `(1 − lookahead) · u_ss`, i.e. together they ARE the carrier.
/// A deadband of 0.30 finger px is not small next to a residual cleaner whose
/// whole output can be a couple of px, so on a close approach the two terms were
/// being nulled one after the other while P and D were simultaneously gated by
/// the aim deadzone — leaving the axes at EXACTLY zero on target. That is
/// discrete, not gradual, and it reads on screen as "准心到头上了就立马松手":
/// the command had to grow back past 0.30 before anything moved again, and the
/// grow-back is what makes the error reappear.
/// 0.05 is small enough that the only thing it can suppress is a term that is
/// already doing nothing.
constexpr float kOutDeadbandPx = 0.05f;

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

/// The FIR the velocity is finally passed through: weights rising toward the
/// newest. This is not just smoothing — it is what makes the estimate LAG
/// deliberately, which is the right thing to do with a signal that has our own
/// output in it.
///
/// ★ ROUND 28 — SHORTENED, 6 SAMPLES TO 4, AND THE CONFIRMATION TO ONE.
///
/// The estimator's total delay is what sets the ALLOWED gain on the
/// feed-forward self-loop (`u += kf · u(n−L)`): a longer L means a smaller kf
/// that still stays stable. Measured on the device, that ceiling had been
/// carried down to kf ≈ 0.21, and the whole cost is paid in the carrier — at
/// kf = 0.21 the integral has to supply 1 − kf = 79 % of it, and the integral is
/// a slow accumulator that HOLDS inside the band, which is exactly the "起步慢
/// + 停下过冲" pair. The old chain was kPredSamples + kPredStableReady +
/// TWO Kalman stages ≈ 16 frames of lag; the reference implementation of the
/// same idea carries one filter stage and ~2.4 frames.
///
/// So this is a DELAY REDUCTION and nothing else, which is why it is the safe
/// direction: delay is a pure destabiliser here, so removing it can only add
/// phase margin. It introduces no new source of oscillation. What it does NOT
/// do is raise the gain for you — kf still has to be re-tuned upward afterwards,
/// and that is the point.
///
/// Weights keep the old shape (a rising ramp with extra weight on the newest
/// sample) and still sum to 1.00, so the estimator's DC gain is unchanged: only
/// its phase lag moves.
constexpr std::size_t kPredSamples     = 4;
constexpr std::size_t kPredHistoryCap  = 16;
constexpr std::size_t kPredHistoryMask = kPredHistoryCap - 1;
constexpr int         kPredStableReady = 2;
constexpr float kPredWeights[kPredSamples] = {0.12f, 0.20f, 0.29f, 0.39f};

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
/// rather than like a ring: it falls with |Δv̂|, with a reversal and with a drop,
/// and rises only while the estimate is stable. It gates the
/// feed-forward, so the loop's response to its own instability is to remove its
/// own excitation — the one mechanism the old controller never had, because
/// there the same fault was AMPLIFIED (carry → the integral) instead.
/// ⚠ 8.0 → 0.8 → BACK TO 8.0. These three constants were "corrected" during the
/// simulation round on the strength of a measurement — "a 600 px/s strafe only
/// earns speedFactor 0.47 at 8.0, so half the carrier is thrown away" — that was
/// true and still is. What the measurement could not see is that |v̂| is
/// proportional to kSensFrozen: the reconstruction returns kSensFrozen·u at a
/// steady lock, so while kSensFrozen was 0.10 every velocity in this file was
/// 10× smaller than it is now, and 0.8 was the right knee for a world in which
/// this file's "px/frame" meant a tenth of a real one. Restoring kSensFrozen to
/// 1.0 restores the scale, and with the scale back, the reference's own 8.0 is
/// the matched value again: a 600 px/s strafe at α = 1 is 5 px/frame → 0.47, and
/// at α = 0.3 it is 16.7 px/frame → 0.88. ★ kTrustVelScale and kSensFrozen are a
/// MATCHED PAIR; changing either alone silently re-tunes this gate by the same
/// factor. Same for kVelZeroPx and kDirChangeVelPx, which are thresholds on the
/// same quantity.
constexpr float kTrustVelScale      = 8.0f;    // speedFactor = 1 − exp(−|v|/this)
constexpr float kTrustAccelScale    = 6.0f;    // stableVelocity = exp(−|Δv|/this)
/// The distance factor is the one member of this set that is NOT coupled to
/// kSensFrozen, because it reads the ERROR (view px) rather than the velocity —
/// so the simulation's argument for 0.70 / 10.0 stands on its own merits. It is
/// still reverted to the reference's 0.42 / 22.0 for now, on a different and
/// simpler principle: every other number in the gate is the vendor's, this is not
/// the failure the user is reporting, and changing a tuned set in one place
/// without being able to test the result is how the last round went wrong. What
/// it costs is a ~0.6 trust ceiling on a dead-on lock, i.e. the floor taxes the
/// carrier a little; what it buys is that the gate behaves as its author verified
/// it. Revisit with device data, not with a simulation.
constexpr float kTrustDistFloor     = 0.42f;   // trust floor at zero error
constexpr float kTrustDistErrScale  = 22.0f;   // …and how fast it rises with |e|
constexpr float kTrustRiseRate      = 0.32f;
constexpr float kTrustFallRate      = 0.72f;   // falls faster than it rises

/// ★ NO TRUST GATE ON THE PREDICTION TERM — `kFfTrustFloor` is gone.
///
/// An earlier version of this file gated the prediction term with
/// `floor + (1 − floor)·trust`. It was described as a deviation from the
/// reference "so the carrier cannot be switched off", and that description was
/// half right: a floor keeps the carrier alive, but the gate still moves it by
/// up to 4× on a signal that swings fastest exactly when the target arrives or
/// stops. `stableVelocity = exp(−|Δv̂|/6)` collapses whenever v̂ changes — which
/// is the definition of arriving — so on arrival this gate dropped the command
/// to 25 % and let it back up afterwards. That is one of the two mechanisms
/// behind "准心到头上了就立马松手，然后又去跟枪".
///
/// Checked against pid_chirs.cpp: the reference does NOT gate its prediction term
/// at all. Its trust feeds only (a) kpScale/kdScale boosts, (b) predictionScale,
/// which is a BOOST, and (c) the integral, where trust ATTENUATES — the exact
/// opposite allocation to the one this file had. Trust is evidence about the
/// ESTIMATE, so what it is allowed to do is stop the residual cleaner from
/// fighting a good estimate; it is not allowed to modulate the carrier.
/// (The reference does have one trust-gated feed-forward, but it is a second,
/// separate term — `feedForwardTerm = v̂ · feedForwardGain · trust` — and its
/// default gain is 0.0, i.e. off.)

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

/// ★ 克里斯远场累加封顶（等效于其 iGain 远场抑制，pid.cpp:1185-1190）：
/// 积分累加把 e 折算为 min(|e|, 50)，远场累加速率被钉在 50·ki/帧且与 e 无关，
/// 大误差想充也充不大——配合下面的输出近场门控，根治远场拉枪的积分充能飞出。
constexpr float kIntegralAccCapPx = 50.0f;
/// ★ 克里斯输出近场门控（pid.cpp:219-224）：
/// integralScale = 0.25 + 0.75·e^{−|e|/12}·√(e^{−|e|/12})
/// 积分状态可以蓄，但到达输出的份额在远处只有 25%，贴近目标全额放行。
/// ki 因此可以保持高值（近场刚度高、不滞后），又不会在远场推飞准星。
constexpr float kIntegralNearGateErrScale = 12.0f;

// ── The integral ───────────────────────────────────────────────────────────

/// Per-frame leak. At 120 Hz this is a **0.16 s** time constant (19 frames), so
/// a charge that is no longer justified decays on its own — no detector
/// required. It is ALSO the integrator's DC gain — a charge of `ki·e/(1−leak)`
/// is what holds the command — so this one number decides both how fast a
/// charge releases and how much steady-state error is needed to hold a given
/// carrier. That is why it cannot be tuned as "just a release rate".
///
/// ⚠⚠ IT WAS 0.985 (0.55 s, 66 frames), AND THAT WAS THE "一会能跟上，一会突然
/// 慢一点" FAULT. A leaky integrator is a POLE, and its time constant has to
/// stay short compared with the loop's dead time; at 66 frames against a ~8
/// frame dead time (2 detector frames + the 6-tap velocity FIR) it had stopped
/// being a trim on the loop and become a second plant, able to resonate with
/// the first. Measured on the model at the user's own gains (kp 0.06, kd 0.24,
/// 前馈 0.74, 600 px/s strafe): the error limit-cycled between −6.6 and +8.3 px
/// with a 3.2 s period, and the telemetry's `trim` column showed the mechanism
/// outright — the charge was put to 0.00 at every zero crossing of the error
/// and needed ~250 frames to rebuild, so for two or three frames at a time the
/// crosshair genuinely was not being driven. Not "跟不上": intermittent.
///
/// The knee is SHARP and it MOVES with the plant gain, so the shipped value is
/// deliberately several steps below it rather than on it. Peak-to-peak error of
/// the settled strafe response, same gains, sweeping only the leak:
///
///     leak    tau      pp @α=1.0    pp @α=2.0
///     0.985   551 ms     14.90        10.9      ← was shipped
///     0.980   412 ms     14.41        11.7
///     0.975   329 ms      0.4         13.0      ← knee at the far end
///     0.970   274 ms      0.3          1.9      ← clean, but no ki left
///     0.960   204 ms      0.2          2.1
///     0.950   162 ms      0.19         2.3      ← shipped
///     0.940   135 ms      0.2          2.5
///
/// Two entries in that table decided the value. First, the knee sits at a LOWER
/// leak the larger the plant gain — at α_true = 2.0 the last quiet value is
/// 0.970, at α_true = 1.0 it is 0.975 — so shipping 0.970, which tracks a
/// little better, would park the loop on the edge for a game twice as sensitive
/// as assumed, with the user's own next tweak pushing it over. α is unknown on
/// the device (no game-side measurement is available), so the margin has to be
/// taken against its worst measured case; 0.950 is four steps clear of it.
/// Second, and less obvious: what pays for that margin is NOT the tracking.
/// The integral's lag is bought back with `ki`, which is a slider the user can
/// see and move, instead of with a constant nobody can observe. 0.970 has
/// almost no range at all (at ki = 0.02 it is already oscillating, pp 14.2,
/// while 0.950 holds pp 1.3 at the same ki), whereas 0.950 keeps ki ≈ 0.01…0.03
/// usable — which is the "can be conservative AND aggressive" property this
/// page is supposed to have.
///
/// Reference implementations of this loop ship 0.985 and are not wrong THERE:
/// theirs is a library whose caller passes gains an order of magnitude larger
/// (kp into the hundreds), so their integral is a small correction on top of a
/// large proportional path. Here the proportional path is 0.05–0.10 and the
/// integral holds a real share of the carrier, so its charge is genuinely able
/// to move the loop. Same number, different regime.
constexpr float kIntegralLeakPerFrame = 0.95f;

/// The adaptive integral gain. It starts at ZERO and rises toward
/// 1 − |e|/threshold while the error is small, i.e. it only gains authority
/// once the aim is close. Together with the leak this is why a fresh engagement
/// cannot wind up.
///
/// ★ NOTE on `rate` (round 26): adjustGain() only USES it while |e| <
/// threshold — above the threshold the suppression branch hard-wires 0.1. So
/// this constant governs the recovery that happens AS THE AIM CLOSES, which is
/// the last stretch of the approach and not the initial charge. At 0.025 that
/// recovery was a 40-frame (0.33 s) affair: the same "second ramp" the
/// prediction gain had, and the same reason the final pixels took so long to
/// close. 0.15 is ~7 frames — still rate-limited, still monotone toward the
/// same target, so the steady state is unchanged and only the approach is
/// quicker.
constexpr float kIntegralGainThreshold = 50.0f;
constexpr float kIntegralGainRate      = 0.15f;

/// The prediction gain.
///
/// ★ ROUND 26 — THE 0.4 s RAMP-IN IS GONE, and the 乱甩 it was written against
/// does not come back with it. That guard was aimed at a STEP in the carrier,
/// but this carrier cannot step: the only thing this gain multiplies is `vel`,
/// and `vel` is the output of a 6-tap FIR fed by two cascaded Kalman filters,
/// with `pushVelocity` publishing NOTHING until six samples have arrived and
/// two of them agree on a direction. The term is therefore already ~15-20
/// frames old before this gain has any say at all. A slow rate bought nothing
/// except a SECOND ramp stacked on the first: 40 frames of time constant at
/// 0.025, i.e. ~0.77 s before the carrier reaches 90 %, with the entire
/// tracking command simply absent for that whole span. That is the reported
/// "跟枪要等一段时间才开始跟". 0.40 settles in ~10 frames, inside the velocity
/// filter's own settling time, so what limits the ramp is now the filters
/// rather than a constant nobody can observe.
///
/// It is still a ramp and still monotone, so no phase is changed and the
/// stability margin (lookahead < 1) is untouched.
constexpr float kPredGainRate = 0.40f;

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

// ── The approach-braking set (ROUND 29, ported from the Chris controller) ──
//
// The reference (D:\克里斯开源某控制器\pid.cpp, chimera_reconstructed::
// updateAxis) has no D term at all. What it has instead is three ASYMMETRIC
// braking mechanisms that only ever REMOVE energy — they fire while the error
// is converging or has been crossed, and do nothing while it grows. This
// loop's D is the opposite shape: symmetric in Δe, linear in kd, and blind to
// how large a fraction of the remaining error one frame actually closed. Two
// of his three mechanisms are covered here already (the errorRate offset by
// the D term itself, the sign-flip decay by the integral's reversal wipe), so
// the port below brings over the one this loop never had — convergence
// braking, his "越来越慢" — plus the saturation pair that makes the P knob
// behave like his outputGain instead of like the loop gain.
//
// His braking (pid.cpp:213):
//
//     prevMag > 1 && prevMag > |e| && same sign:
//         command *= 1 − (1 − |e|/prevMag) · (approachDamping · 0.2)
//
// i.e. the output is cut in proportion to the FRACTION of the remaining error
// the frame just closed. On an exponentially settling approach (e ← ρ·e) that
// fraction is the constant (1−ρ), so the brake raises ρ — the approach
// literally gets slower the closer it is — and releases to a no-op the moment
// the error stops shrinking: a steady lock, a strafe lag and a completed stop
// all have shrink ≈ 0 and pass through untouched. That self-release is what
// makes it safe to apply to the WHOLE output here, feed-forward included. It
// is the opposite shape from the round-26 fault, where the carrier was
// suppressed by the SIZE of the error (a positive feedback: big error → less
// drive → bigger error); this one is driven by the CLOSURE of the error,
// which only exists while the loop is already winning, so it can slow an
// approach but never stall one.
constexpr float kConvBrakeStrength = 1.0f;  // his approachDamping 5.0 × 0.2

// …but his multiplier is allowed to reach exactly 0 (an instant collapse of
// the error reads as "full brake" for one frame). This file's history — the
// band in round 15, the deadband in round 26 — made "output silently zeroed
// near the target" the most expensive fault class on the device, so the
// multiplier is floored: the brake may halve nothing more than 60 % of the
// command in a single step, and never zero it.
constexpr float kConvBrakeMinMult  = 0.40f;

/// His gate is `prevMag > 1.0` (view px here). Below about a pixel of error
/// the frame-to-frame ratio is OneEuro filter noise, not closure, and braking
/// on it would be a permanent tax on exactly the terminal phase the brake is
/// supposed to smooth.
constexpr float kConvBrakeErrFloor = 3.0f;

// ── Overshoot Feedforward & Integral suppression ───────────────────────────
// Beyond 1.0 px of error, any feedforward or integral component that pushes in
// the direction of the overshoot (away from target) is suppressed to zero and
// prediction memory is scaled down, preventing forward momentum from fighting
// the restoring P and braking D terms.
constexpr float kOvershootFloorPx  = 1.0f;

// ── P saturation, the port of his normalizedError clamp ────────────────────
//
// His P term is `clamp(error · outputGain / 5, ±3)` — at his default gain the
// knee sits at error = 60 px, and BEYOND THE KNEE RAISING THE GAIN CHANGES
// NOTHING. That is the structural reason his kp cannot shake the aim: the
// knob only has authority in the last 60 px. This loop's P was linear all the
// way out, so kp was the loop gain itself and every raise went straight into
// the phase margin — the reported "kp 拉大会乱晃".
//
// The port is a soft knee (tanh) with the same break point: linear below
// ~30 px, asymptoting at kPSatPx, so on-target behaviour and the terminal
// approach are bit-identical to the unsaturated loop. THE INTENDED USAGE
// CHANGES WITH IT: from this round on, "kp 拉大" is how terminal aggression
// is bought — the knee keeps the raise local, past ~60 px it cannot reach the
// loop's stability any more — and far-field approach speed stays the
// feed-forward's job, as designed. If the initial pull of a re-lock now feels
// soft, that is the knee working; raise kp to pay for it, which is free.
constexpr float kPSatPx = 150.0f;

// ── The derivative's rate clamp, the port of his errorRate clamp ───────────
//
// His errorRate is clamped to ±10 before anything downstream sees it; this
// loop's dNorm was unbounded, its only guard the jump detector zeroing de
// outright. 20 px/frame is 2400 px/s — an order of magnitude above any real
// strafe — so the clamp is invisible to real motion and only caps what a
// detector spike that slipped past the jump gate can cost through kd.
constexpr float kDerivClampPx = 20.0f;

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

    /// 设置系统物理全链路延迟（秒，克里斯控制器延迟接口，带非对称平滑滤波）
    void setLatencySeconds(float latencySeconds) {
        if (!std::isfinite(latencySeconds)) return;
        const float clampedLatency = clampRange(
            latencySeconds,
            kNominalDt,
            kMaxPredictionLatencySeconds
        );
        if (!hasLatencyHint_) {
            latencySeconds_ = clampedLatency;
            hasLatencyHint_ = true;
            return;
        }
        const float latencyDelta = clampedLatency - latencySeconds_;
        const float smoothing = latencyDelta > 0.0f
            ? kLatencyRiseSmoothing
            : kLatencyFallSmoothing;
        latencySeconds_ += latencyDelta * smoothing;
    }

    /// 设置系统延迟帧数（按 120Hz 标称帧时计算）
    void setLatencyFrames(float frames) {
        if (!std::isfinite(frames)) return;
        setLatencySeconds(frames * kNominalDt);
    }

    float latencySeconds() const { return latencySeconds_; }
    float latencyFrames() const { return latencySeconds_ / kNominalDt; }

    /// The engaged target's size in SCREEN px (the box's longer side). Used only
    /// to express the jump thresholds as a fraction of a target rather than as a
    /// fixed number of pixels, so a distant 20 px box and a contact-range 300 px
    /// box are judged the same way. 0 disables the box-relative part.
    void setTargetBoxPx(float px) { boxPx_ = (px > 0.0f) ? px : 0.0f; }

    /// 根据端到端全链路延迟帧数，从历史输出缓冲区插值获取对应时刻的输出控制量，
    /// 确保速度重构 rawMeas 中的误差微分与自身控制量在时间相位上精准对齐！
    float getDelayedOutput(float latencyFrames) const {
        const float clampedFrames = clampRange(latencyFrames, 1.0f, 8.0f);
        const int i0 = static_cast<int>(std::floor(clampedFrames));
        const int i1 = i0 + 1;
        const float frac = clampedFrames - static_cast<float>(i0);
        const float u0 = outHist_[(outHead_ - i0) & kOutHistMask];
        const float u1 = outHist_[(outHead_ - i1) & kOutHistMask];
        return u0 + (u1 - u0) * frac;
    }

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
                 bool targetChanged, bool measurementCorrected,
                 float targetVel = 0.0f) {
        if (!std::isfinite(error)) { reset(); return 0.0f; }

        dt = clampRange(dt, kMinDt, kMaxDt);
        const float dtScale = dt / kNominalDt;

        // 误差微抖门限
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
            if (step >= hard) {
                ++jumpHard_;
                reset();
                hardReset = true;
            } else if (step >= soft) {
                ++jumpSoft_;
                softJump = true;
            }
        }

        const float filtered = errFilter_.update(raw, dt);
        const float blend    = rawBlend(raw, hasPrevRaw_ ? prevRaw_ : lastError_);
        const float e        = round2(filtered + (raw - filtered) * blend);

        const float rawDe = (hardReset || softJump) ? 0.0f : (e - lastError_);
        const float absE  = std::fabs(e);
        const float absDe = std::fabs(rawDe);


        // 速度微分
        constexpr float kVelNoiseFloor = 0.20f;
        const float deVel = (absDe < kVelNoiseFloor) ? 0.0f : rawDe;
        const float dNorm = deVel * (kNominalDt / dt);

        // D 项消噪死区
        constexpr float kDerivNoiseFloor = 0.35f;
        float deD = 0.0f;
        if (absDe >= kDerivNoiseFloor) {
            deD = std::copysign(absDe - kDerivNoiseFloor, rawDe);
        }
        const float dNormForD = deD * (kNominalDt / dt);

        // ★ 克里斯控制器速度重构（D:\克里斯控制器\pid.cpp:751-766）：
        //     rawVelocity = errorDiff + lastOutput_
        // 误差导数里混着自身引起的运动，加回上一帧实际派发的位移后剩下的才是
        // 目标真实速度。拉静止目标: Δe=−u, +u → v̂≈0，前馈不捣乱（治"甩飞"）；
        // 咬住移动目标: Δe≈0, +u → v̂=u_ss，前馈瞬时扛起载波（治"降ki滞后"）。
        // lastOutput_ 是 clamp/刹车/平滑之后的最终输出，最接近真实注入量。
        // α=1 假设与克里斯一致（kSensFrozen = 1.0）。
        float vel = 0.0f;
        // hold 只留给真正的状态跳变帧（reset 已清滤波器，softJump 的 Δe 是跟踪器的）。
        // ★ measurementCorrected 不能进 hold：控制帧率=检测帧率时每帧都是修正帧，
        // hold 会变成永久冻结——设备日志实测 v̂ 恒定 1.87 连续 8 帧（滤波器不再
        // 更新），前馈整体失效。修正踢腿由 velFilter_ 卡尔曼平均吸收（克里斯
        // 观测器思路：不存在二值的"这帧不是速度"，只有连续的置信度）。
        const bool deIsNotVelocity = hardReset || softJump;
        if (std::fabs(targetVel) > 0.02f) {
            vel = velFilter_.update(targetVel, dt);
        } else if (deIsNotVelocity) {
            vel = velFilter_.x;
        } else {
            // ★ 速度重构必须吃原始误差差分（raw − prevRaw_），不能吃 rawDe。
            // rawDe 来自 OneEuro 滤波后的误差（kErrMinCutoff 1.05Hz ≈ 150ms 时间
            // 常数）：急停后滤波误差还在走 → v̂ 不归零 → ff 推着过冲（"停下晃"）；
            // 起步时滤波误差没反应 → v̂ 迟迟为 0 → ff 缺席（"起步肉"）。设备日志
            // 实测：v̂ 连续 5 帧读 0 而目标每帧动 2~3px。去抖是 P/D 的事，它们的
            // 输入继续走滤波路径；速度这条路只过 velFilter_ 卡尔曼。
            const float rawDiff = hasPrevRaw_ ? (raw - prevRaw_) : 0.0f;
            float rawVelocity = rawDiff + lastOutput_;
            // 近场混合（pid.cpp:762-766）：贴住目标时向 (Δe + 0.5u) 收，
            // 抑制自回声在速度估计里的残余噪声。
            if (absE < 1.0f && absDe < 0.1f) {
                const float nearZeroVelocity = rawDiff + 0.5f * lastOutput_;
                rawVelocity = rawVelocity * 0.35f + nearZeroVelocity * 0.65f;
            }
            vel = velFilter_.update(rawVelocity, dt);
        }
        if (std::fabs(vel) < 0.15f) vel = 0.0f;
        velNow_  = vel;
        velLast_ = vel;
        velFresh_ = true;

        // ── 1. 比例项 P（平滑果断，无60px硬饱和，具备真正的快速拉枪与跟枪响应力）──
        const float gate = frozen ? 0.0f : 1.0f;
        const float nearBlend = std::fmin(1.0f, std::fmax(0.0f, (absE - 0.5f) / 12.0f));
        const float nearScale = 0.70f + 0.30f * nearBlend;
        const float farBlend  = std::fmin(1.0f, std::fmax(0.0f, (absE - 20.0f) / 60.0f));
        const float farBoost  = 1.0f + 0.35f * farBlend;
        float eShaped = std::copysign(absE * nearScale * farBoost, e);
        if (absE < 0.5f && std::fabs(vel) < 0.2f && std::fabs(dNorm) < 0.2f) {
            eShaped = 0.0f;
        }
        // ★ P 软饱和（自 4c94f4a 存档点活代码原样移植）：eForP = kPSatPx·tanh。
        // 线性区（<~60px）与原式一致；大误差渐近 kPSatPx——快速滑动后跟踪器
        // 会按自身速度模型把发布中心朝滑动方向外推若干帧（"鬼影"），无饱和的
        // 线性 P 会以几十 px/帧追鬼影（读作"往滑动方向甩飞"），饱和后 ~9px/帧，
        // 检测修正帧一到就被拉回。
        const float pT = kPSatPx * std::tanh(eShaped / kPSatPx) * kp_ * gate;

        // ── 2. 微分项 D（强力减震阻尼器）──────────────────────────────────────
        // 始终保持有效阻尼，直接吸收准星冲撞靶心时的晃荡动能，杜绝弹簧反弹
        float dT = clampRange(dNormForD, -kDerivClampPx, kDerivClampPx) * kd_ * gate;
        if (frozen || (absE < 0.5f && absDe < 0.2f)) {
            dT = 0.0f;
        }

        // ── 3. 积分项 I（纯残料清扫，克里斯三重护栏）─────────────────────────
        // 前馈已扛起载波（速度重构含自运动补偿），积分从此只清稳态残差。
        // ★ 护栏一（pid.cpp:1391-1394 prepareIntegralForError）：变号硬清零。
        //   过零电荷是纯外推能量，直接排空——旧版"运动中保留 70~95%"的软泄放
        //   是载波缺失时代防断崖失速的补丁，载波修复后不再需要，它是极限环续命源。
        // ★ 护栏二（pid.cpp:1185 iGain 远场抑制的等效写法）：远场累加封顶，
        //   累加速率 = min(|e|,50)·ki，与大误差无关。
        // ★ 护栏三（pid.cpp:219-224 integralScale）：输出近场门控，远处只放行
        //   25%，贴近目标全额——ki 可以保持高值而不飞。
        float iOut = 0.0f;
        if (ki_ > 1e-4f && !frozen) {
            if (integral_ != 0.0f && e != 0.0f && !sameSign(e, integral_)) {
                integral_ = 0.0f;
            }
            if (absE < 3.0f && absDe < 2.0f) {
                // 近距微抖区：冻结累加并温和泄电（保留原有机制）
                integral_ *= 0.88f;
            } else {
                if (integral_ != 0.0f) {
                    integral_ *= 0.94f;
                }
                const float eAcc = std::copysign(
                    std::fmin(absE, kIntegralAccCapPx), e);
                integral_ += eAcc * ki_ * dtScale;
            }
            const float iCap = std::fmin(30.0f, std::fmax(2.0f, absE * 0.5f));
            integral_ = clampRange(integral_, -iCap, iCap);
            const float nearGate = std::exp(-absE / kIntegralNearGateErrScale);
            const float integralScale =
                0.25f + 0.75f * nearGate * std::sqrt(nearGate);
            iOut = integral_ * integralScale;
        } else {
            integral_ *= 0.80f;
        }

        // ── 4. 前馈项 FF（真正物理跟枪载波，克里斯 lookahead 结构）─────────────
        // 速度重构已含自运动补偿，v̂ 就是目标真实速度（screen px/帧 ≈ finger px/帧）。
        // 稳态咬住时 v̂ = u_ss，ff = lookahead·u_ss，恰好扛起 lookahead 份额的
        // 载波，剩余 (1−lookahead) 由 P+I 补——积分从此只是残料清扫。
        // 旧版 ×2.5 系数是为 dNorm 低估打的补丁，真速度下不再需要，已移除。
        float ff = lookahead_ * vel;
        if (frozen || (absE < 0.8f && std::fabs(vel) < 0.25f)) {
            ff *= std::clamp(absE / 0.8f, 0.0f, 1.0f);
        }
        ff = clampRange(ff, -kFfLimitPx, kFfLimitPx);

        // ── 过冲方向抑制（★ 自 4c94f4a 存档点活代码原样移植）────────────────
        // 误差越过目标 ≥1px 后，任何仍朝"远离目标"方向推的前馈分量清零、
        // 速度记忆减半，积分分量清零且电荷排空——过冲发生后它们只会把准星
        // 往外甩（"有时候甩出去"的来源就是本文件此前缺了这段）。
        // 误差未越过的正常逼近侧不受影响，跟枪载波保持原样。
        if (e <= -kOvershootFloorPx) {
            if (ff > 0.0f) {
                ff = 0.0f;
                velFilter_.x *= 0.5f;
            }
            if (iOut > 0.0f) {
                iOut = 0.0f;
                integral_ = 0.0f;
            }
        } else if (e >= kOvershootFloorPx) {
            if (ff < 0.0f) {
                ff = 0.0f;
                velFilter_.x *= 0.5f;
            }
            if (iOut < 0.0f) {
                iOut = 0.0f;
                integral_ = 0.0f;
            }
        }

        // 合成总输出
        float out = pT + dT + iOut + ff;
        out = clampRange(out, -kAimOutLimitPx, kAimOutLimitPx);

        // ── 收敛刹车（★ 自 4c94f4a 存档点活代码原样移植）────────────────────
        // 误差同号且确实在收缩时，按"本帧收掉的剩余误差比例"整体裁剪输出：
        // 逼近越接近目标越慢，从根上拦住 P 打在滞后误差上造成的过冲振荡
        // （"有时候甩飞出去"的来源——ki=0/前馈=0 也甩，就是这条路径）。
        // 稳态咬住、匀速滞后、已完成的停止的 shrink≈0，原样通过不衰减。
        // 跳帧豁免：那一帧的步长是跟踪器的，不是逼近的。
        const float prevMag = std::fabs(lastError_);
        if (!hardReset && !softJump &&
            prevMag > kConvBrakeErrFloor && prevMag > absE &&
            sameSign(e, lastError_)) {
            const float shrink = 1.0f - absE / prevMag;
            out *= clampRange(1.0f - shrink * kConvBrakeStrength,
                              kConvBrakeMinMult, 1.0f);
        }


        if (outSmooth_ < 0.999f) {
            outSmoothState_ += (out - outSmoothState_) * outSmooth_;
            out = outSmoothState_;
        }

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
    void reset() {
        integral_  = 0.0f;
        iGain_     = 0.0f;
        predGain_  = 1.0f;
        outGain_   = 1.0f;
        lastError_ = 0.0f;
        prevRaw_   = 0.0f;
        hasPrevRaw_ = false;
        lastOutput_ = 0.0f;
        for (std::size_t i = 0; i < kOutHistCap; ++i) outHist_[i] = 0.0f;
        outHead_ = 0;
        outSmoothState_ = 0.0f;
        prevTrust_ = 1.0f;
        latencySeconds_ = kNominalDt;
        hasLatencyHint_ = false;
        velNow_ = 0.0f;
        velLast_ = 0.0f;
        transferBleed_ = 0.0f;
        velFresh_ = false;
        velocityCoherence_ = 1.0f;
        lastCoherenceVelocity_ = 0.0f;
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
    /// ★ ROUND 26 — jump-detector firing counts, cumulative since construction.
    /// Diagnostics only; nothing in the loop reads them.
    int jumpSoftCount() const { return jumpSoft_; }
    int jumpHardCount() const { return jumpHard_; }

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

        // ★ 克里斯控制器机制：直接使用变向 (reversal) 与减速急停 (drop) 进行连续自适应门控与记忆排空，
        // 彻底解决因人为死区导致目标减速或折返时无法及时泄放前馈与积分记忆、进而猛烈冲出靶心的问题！
        const float distTrust = kTrustDistFloor +
                                (1.0f - kTrustDistFloor) *
                                clamp01(absE / kTrustDistErrScale);

        float target = 0.0f;
        if (av > kEps) {
            const float speedFactor = 1.0f - std::exp(-av / kTrustVelScale);
            const float stableVel   = std::exp(-acc / kTrustAccelScale);
            const float revGate     = (reversal > 0.0f)
                                          ? std::exp(-kReversalGateStrength * reversal)
                                          : 1.0f;
            const float stopGate    = (drop > 0.0f)
                                          ? std::exp(-kStopGateStrength * drop)
                                          : 1.0f;
            target = clamp01(speedFactor * stableVel * revGate * stopGate * distTrust);
        }
        const float rate = (target < trustPrev) ? kTrustFallRate : kTrustRiseRate;
        a.trust = clamp01(trustPrev + (target - trustPrev) * rate);

        a.predScale = 1.0f + kKpTrustBoost * a.trust;

        const float nearInt = std::exp(-absE / kIntegralErrScale);
        const float stableE = std::exp(-absDe / kIntegralDiffScale);
        a.intScale = clampRange(kIntegralScaleFloor +
                                 (1.0f - kIntegralScaleFloor) * nearInt * stableE,
                                0.0f, 1.0f);
        a.intScale *= (1.0f - kIntegralTrustCut * a.trust);

        // 目标减速或急停变向时，立即按比例清退速度记忆与积分残余蓄水池，从物理源头扼杀过冲！
        const float rd = reversal + drop;
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
        if (predMoves_ == 0) return 0.0f;

        float weighted = 0.0f;
        float weightSum = 0.0f;
        const std::size_t count = predMoves_;
        std::size_t idx = (predHead_ - count) & kPredHistoryMask;
        for (std::size_t i = 0; i < count; ++i) {
            const float w = kPredWeights[kPredSamples - count + i];
            weighted += predHist_[idx] * w;
            weightSum += w;
            idx = (idx + 1) & kPredHistoryMask;
        }
        // ★ 多帧方向一致性确认：
        // 目标静止时，检测框量化抖动表现为正负频繁跳变（一帧+一帧-）。
        // 真实移动必定是连续同向位移。如果方向突变反转，判定为噪声，立即清零计数并丢弃，
        // 只有连续同向才输出速度，彻底消除静止左右抽搐！
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

    /// Three states, and the middle one is the one that was wrong.
    ///
    ///   off target (error != 0, outside the band) — normal: leak, accumulate
    ///   ON TARGET  (error snapped to 0, or inside the band) — HOLD: neither
    ///   reversal (error of the opposite sign) — DROP the charge outright
    ///
    /// `frozen` is the caller's stop band. It is only one of the two ways to be
    /// "on target"; the other is the error snap itself, and relying on the
    /// caller's deadzone to define this state meant a user who set 死区 = 0 got
    /// no hold at all.
    float updateIntegral(float e, float absE, float absDe, bool frozen, float dtScale) {
        if (ki_ <= kEps) { integral_ = 0.0f; return 0.0f; }

        // ── THE RELEASE ───────────────────────────────────────────────────
        // A genuine sign REVERSAL means we have crossed the target: the charge
        // is now pushing the aim away from it, so it is dropped in one step
        // rather than bled. This is what makes a stop quiet — the target stops,
        // the aim over-runs, the error reverses, and the charge that caused the
        // over-run is gone before it can overshoot twice.
        // 变号平滑泄电：运动跟枪时维持必要载波，避免断崖失速导致皮筋极限环抽搐；
        // 静止过冲时按比例衰减多余电荷。
        if (integral_ != 0.0f && e != 0.0f && !sameSign(e, integral_)) {
            const float motion = clamp01(std::fabs(velNow_) / 0.35f);
            if (motion > 0.3f && absE < 3.0f) {
                integral_ *= (0.70f + 0.25f * motion);
            } else {
                integral_ *= 0.25f;
                if (std::fabs(integral_) < 0.05f) integral_ = 0.0f;
                iGain_ *= 0.5f;
            }
        }

        // ── THE HOLD & LEAK ───────────────────────────────────────────────
        // 目标静止/微抖防晃：在近距离极小步长区间 (|e| < 3.0px 且 |Δe| < 2.0px)，
        // 判定为 YOLO 框边缘检测微抖动，冻结积分累加并主动温和泄电，杜绝静止时积分微振荡
        const bool isJitterZone = (absE < 3.0f && absDe < 2.0f);
        if (frozen) {
            // 在用户死区内：停止累加并快速泄电，防止累积电荷把准星轰出死区
            if (integral_ != 0.0f) {
                integral_ *= 0.80f;
            }
        } else if (e == 0.0f || isJitterZone) {
            // 在目标微抖区内：温和衰减残余电荷
            if (integral_ != 0.0f) {
                integral_ *= 0.88f;
            }
        } else {
            if (integral_ != 0.0f) {
                integral_ *= std::pow(kIntegralLeakPerFrame,
                                      clampRange(dtScale, kFrameScaleMin, kFrameScaleMax));
            }
            integral_ += e * ki_ * iGain_ * dtScale;
        }

        // 静止时收紧限幅杜绝微抖；运动跟枪时放宽容量，保证积分能辅助消除跟踪稳态滞后
        const float motion = clamp01(std::fabs(velNow_) / 0.35f);
        const float staticCap = std::fmax(1.0f, absE * kp_);
        const float maxIntegralCapacity = staticCap + (kAimTrimLimitPx - staticCap) * motion;
        integral_ = clampRange(integral_, -maxIntegralCapacity, maxIntegralCapacity);
        return integral_;
    }

    // ── Parameters ────────────────────────────────────────────────────────
    // Placeholders for the frame between construction and the first setParams();
    // they mirror the page's shipped defaults so the two cannot disagree.
    float kp_        = 0.05f;
    float ki_        = 0.01f;
    float kd_        = 0.15f;
    float outSmooth_ = 1.0f;
    float lookahead_ = kLookaheadDefault;
    float boxPx_     = 0.0f;

    // ── State ─────────────────────────────────────────────────────────────
    float latencySeconds_ = kNominalDt;
    bool  hasLatencyHint_ = false;
    float integral_  = 0.0f;
    float iGain_     = 0.0f;
    float predGain_  = 0.0f;
    float outGain_   = kOutGainFloor;
    float lastError_ = 0.0f;
    float prevRaw_   = 0.0f;
    bool  hasPrevRaw_ = false;
    float lastOutput_ = 0.0f;
    static constexpr std::size_t kOutHistCap = 16;
    static constexpr std::size_t kOutHistMask = kOutHistCap - 1;
    float outHist_[kOutHistCap] = {};
    std::size_t outHead_ = 0;
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

    // ★ 克里斯控制器核心机制：速度相干性追踪 (消除检测框微晃与静止抽搐)
    float velocityCoherence_ = 1.0f;
    float lastCoherenceVelocity_ = 0.0f;

    OneEuro      errFilter_;
    ScalarKalman velFilter_{kVelKalmanQ, kVelKalmanR, 0.0f, 1.0f};
    ScalarKalman velSmooth_{kVelKalmanQ, kVelKalmanR, 0.0f, 1.0f};

    // Last step's contributions, for the log line only.
    float pDisp_ = 0.0f, dDisp_ = 0.0f, iDisp_ = 0.0f, fDisp_ = 0.0f;

    /// ★ ROUND 26 — how many times the jump detector fired, cumulative.
    ///
    /// A soft/hard jump is the loop saying "I mistook this step for a track
    /// switch". Counted from construction on purpose and NOT cleared by
    /// reset(): the interesting number is the RATE at which they accrue during
    /// an ordinary approach, and a hard jump would otherwise erase its own
    /// evidence. On a clean approach this should stay flat; if it climbs while
    /// a target is merely being tracked, the `selfStep` correction above is
    /// either missing a case or kSensFrozen is far enough off to hide one.
    int jumpSoft_ = 0;
    int jumpHard_ = 0;

    /// Used by the jump detector: a soft jump cuts the transient state without
    /// wiping the loop (see kJumpIntegralKeep).
    void softReset() {
        transferBleed_ = kJumpIntegralKeep;
        prevTrust_    *= kJumpTrustKeep;
        for (std::size_t i = 0; i < kOutHistCap; ++i) outHist_[i] = 0.0f;
        outHead_ = 0;
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
