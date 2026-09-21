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
//  ── ROUND 13: WHY kf = 1.0 FLEW THE CROSSHAIR OFF THE SCREEN ────────────────
//
//  User, on the round-12 build: "kf 现在是 1 了，自瞄直接飞出去啊".
//
//  alpha_hat reaches the controller only while the estimator reports a valid fit
//  (AimController::step in aim_section.h), so before the first fit — and
//  forever, for a user who never excites one — the controller kept its DEFAULT
//  of 1.0. The true alpha in the touch regime is near 0.1, and the residual
//  self-term derived above is
//
//      kf · (1 − alpha/alpha_hat) = 1.0 · (1 − 0.1/1.0) = +0.9
//
//  — a POSITIVE feedback of 0.9 on our own delayed command. An accelerator with
//  the brake removed: the command grows geometrically, the tanh limiter pins it
//  at 180 finger px/step, and the crosshair leaves the screen.
//
//  The same arithmetic at the kf the user had before (0.08) is +0.072, small
//  enough to look like a bad gain: "不过冲了，但目标停下会左右抖几下，并且跟枪跟不
//  上了". ONE root cause, THREE symptoms:
//
//      +0.9·kf → runs away            (飞出去)
//      +0.07·kf → shakes after a stop (停下抖几下: the residual keeps pushing
//                                       after the target has already stopped)
//      gain kf/alpha_hat = 0.08 in place of 0.8 → no DC carrier (跟不上)
//
//  ── THE REPAIRS, ALL OF THEM PORTED FROM THE TWO REFERENCE FILES ────────────
//
//  1. alpha_hat is NEVER 1.0 by default again. The seed, and the fallback while
//     no fit exists, is kAlphaHatSeed = 0.10 — chosen because it is on the SAFE
//     side of the asymmetry: an UNDER-estimate makes the residual negative, i.e.
//     damping. The value is also pushed UNCONDITIONALLY now, not only once a fit
//     has succeeded, and the estimator's accepted fits are rate-limited so one
//     bad window cannot slam the constant.
//
//  2. A JUMP DETECTOR, pid (1).cpp:40 — `if (|Δe| > 30) reset()`. A re-lock, a
//     flick or a target switch is 100–300 px of Δe in ONE frame, and the
//     reconstruction cannot tell that apart from the target really moving that
//     far; it would read the whole flick as target velocity and fire it through
//     F. The reference throws the state away. What makes it work there is the
//     second half of the trick: its reset also zeroes kp_gain, and the ENTIRE
//     output is multiplied by kp_gain — so the jump frame emits ZERO and the
//     gains fade back in over the next few frames. That is exactly what ramp
//     already does here, so the jump branch sets ramp = 0.
//
//  3. BOUNDS, so that a wrong alpha_hat can never be violent: the reconstructed
//     velocity is clamped to ±kFfVelMaxPx, the feed-forward to ±kFfLimitPx, and
//     the gain kf/alpha_hat to kFfGainMax. The limiter is the LAST line of
//     defence, not the mechanism — alpha_hat being right is the mechanism.
//
//  4. FAST RELEASE on the feed-forward low-pass. When a target stops, a
//     symmetric 33 ms filter keeps the old velocity alive for ~4 frames — 4 × 50
//     finger px at alpha = 0.1 — which is the crosshair sailing past and then
//     being dragged back: the "抖几下". A shorter constant on the way DOWN
//     (kFfTauFastSec) lets a decay happen at once while a rise stays filtered.
//
//  5. A DEADBAND on the feed-forward, pid (1).cpp:55 (`|ki_raw| > 0.5` else 0),
//     so detector noise cannot keep a small command alive at rest.
//
//  6. A RATE-LIMITED PROXIMITY WEIGHT on P and I, ported from pid (1).cpp's
//     kp_integral()/adjust_integral(): both fade toward half strength while the
//     aim is far off and are allowed back to full only as it closes
//     (`比例 = 比例系数*0.5*e`, `积分累计 += e*0.5*dt` on the 未达标 branch).
//     That is the reference's real anti-overshoot device — not a gain VALUE, a
//     gain SCHEDULE, and the schedule is what a single slider cannot express.
//     It REPLACES the hard 已达标 latch, which round 12 measured self-locking:
//     a latch cannot reopen on a strafing target, and the reference's own rule
//     parked the error at 475…663 px here. A weight with a floor of 0.5 and a
//     rate limit cannot latch, and the JUMP branch is what does the hard reset.
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
//
//  ── ROUND 14: WHERE THE TRACKING WENT, AND WHY IT WAS NOT THE GAINS ─────────
//
//  The user, on the round-13 build: "kf 根本做不到跟枪 … ki 会导致过冲并且很慢的
//  描回来 … 我之前让你修过冲乱晃的时候本地 git 提交的那个 kf 是左右晃，但是他可以有
//  跟枪效果". The third clause is the one that matters: an EARLIER build tracked, so
//  this was a regression rather than a tuning limit — and it had three causes,
//  none of them a gain value.
//
//  1. THE INTEGRAL'S LEASH WAS 90, AND 90 IS A CEILING ON SPEED, NOT A RAIL.
//     Holding a target that moves w view px per step needs a standing command of
//     u_ss = w/alpha. At alpha = 0.1 that is TEN TIMES w — 50 finger px/step at
//     600 px/s, 125 at 1500 — so a leash of 90 caps the integral at
//     90 × 0.1 = 9 view px/step = 1080 px/s, and everything faster has to be paid
//     for out of the P term's remaining error: hundreds of px of it. Raising ki
//     could not have helped, for the same reason a bigger engine does not help a
//     car whose speedometer is pegged. The leash is 150 now.
//
//  2. THE INTEGRAL CAME BACK AT THE RATE IT WENT OUT. A 50 px trim built against
//     a strafe needs 50/(ki·|e|·dt) frames to return — at ki = 0.1, e = −20 px,
//     120 Hz that is 3000 frames, twenty-five seconds. "很慢的描回来" is that
//     arithmetic verbatim, and it is why "ki 会导致过冲" was unfixable by tuning:
//     the wind-up was not the bug, the discharge rate was. The trim now unwinds
//     kIntReleaseGain (6×) faster once the error has reversed.
//
//  3. THE PROXIMITY SCHEDULE WAS THROTTLING THE CARRIER. Ported in round 13 from
//     pid (1).cpp, it scaled BOTH P and I down to a floor of 0.5 while the aim
//     was far off — halving kp, and halving the integral, at the exact moment the
//     integral had to build the DC carrier. Whatever it bought against a re-lock
//     lurch, it cost the steady-state hold. Floor 0.75, and P alone.
//
//  And the SHAKE, which the user has reported since round 10 and which rounds
//  11-13 kept attacking from the wrong side:
//
//      the residual self-term is a self-COPY of our own delayed command, with
//      coefficient s = kf·(1 − alpha/alpha_hat), and the characteristic equation
//      of that copy is z^L = s — so |z| = |s|^(1/L), and it is stable only while
//      |s| < 1.
//
//  Round 10 had s ≈ 1, because in that formulation the copy's gain did not depend
//  on kf at all — marginally stable, which is exactly "跟得上，但一直晃". Round 12
//  then raised kf until |s| could reach 1 again and the crosshair left the screen.
//  The answer is not a value of kf, it is a RANGE for it: with the estimator's
//  bias and its ±30 %, r = alpha/alpha_hat lands near 1.0…2.0, and there kf = 0.80
//  puts s in −0.80…0.00; on the other side of r it cannot exceed +0.80 either, so
//  the copy ALWAYS decays. kf = 1.00 has no such property — an over-estimated
//  alpha drives s to +1.00 and the pole onto the circle, which is the round-10
//  marginal case wearing a different formula. Meanwhile the carrier share
//  kf·r/(1 − kf + kf·r) is 0.79…0.88 at either setting. So the slider is
//  0.00–1.20 with a working value of 0.80: 1.00 is reachable and is the EDGE, not
//  the target. The ~15 % of carrier kf leaves is the integral's job now — a job
//  the integral can finally do, because of (1) and (3).
//
//  Two more repairs, both about refusing to believe a fit that is not one:
//
//    * The alpha estimator's excitation guard tested the VARIANCE of U, which
//      misses the case that matters: a steady track makes U a straight line in k,
//      so the variance is large and U is COLLINEAR with the k regressor, the
//      normal equations go near-singular, and the solve still returns a number.
//      It now tests the correlation between U and k directly (reject above 0.985)
//      and requires the fit to explain 75 % of the window's variance. A steady
//      hold is rejected — correctly, because alpha is neither observable nor
//      important then.
//    * kJumpResetPx went 30 → 60, and the threshold now takes the larger of that
//      and the target's own BOX WIDTH. Δe contains our own command; firing this
//      branch zeroes the soft-start ramp and the integral, so a threshold close
//      to a working strafe is itself a throttle.
//
//  None of the three causes above is a gain, and all three of the fixes are
//  arithmetic (u_ss = w/alpha; 50/(ki·|e|·dt) frames; |s| < 1) — they need no
//  simulator to check, only the log.
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

// ── The integral's leash (round 14) ─────────────────────────────────────────
//
// This was 90, and 90 was not a safety rail — it was a hard ceiling on how fast
// a target the integral could hold at all. Holding a target that moves w view px
// per step needs a standing command of u_ss = w/alpha; at alpha = 0.1 that is TEN
// TIMES w, so 90 finger px/step caps the view at 90 x 0.1 = 9 px/step, i.e.
// 1080 px/s, and everything past that has to come out of the P term's remaining
// error — hundreds of px of it. That is the arithmetic behind "kf 做不到跟枪"
// once the feed-forward is small, and raising ki could not fix it because the
// ceiling was above ki, not inside it.
//
// 150 is still strictly below the output ceiling, so softLimit() remains the
// last word on what the world receives.
constexpr float kTrimLimitPx = 150.0f;

// How much faster the integral UNWINDS than it winds when the error has already
// reversed and is pulling it back toward zero (round 14). Without this a trim of
// 50 px built against a strafe comes back at ki·|e|·dt per frame — at ki = 0.1,
// e = -20 px, 120 Hz that is 3000 frames, twenty-five seconds of "很慢的描回来".
constexpr float kIntReleaseGain = 6.0f;

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
// ── ROUND 14: the kf ceiling is 1.20 and 0.80 is the working value ──────────
//
// kFfStrengthMax is the top of the kf slider. Round 11 had 0.20, and that
// ceiling was a real bug — from the round-12 section, the carrier share is
// ≈ 0.8·kf, so 0.20 caps the feed-forward at 16 % of the DC command a moving
// target needs, whatever the game's sensitivity. Round 12 then went to 2.0,
// which over-corrected: the value that MATTERS is set by stability, not by the
// carrier account, because the residual self-term is a self-COPY of our own
// delayed command with coefficient
//
//     s = kf · (1 − alpha/alpha_hat)
//
// and the loop's characteristic equation for that copy is z^L = s, i.e.
// |z| = |s|^(1/L). Stable only while |s| < 1. With the estimator's own bias and
// its ±30 %, r = alpha/alpha_hat lands near 1.0…2.0, and then
//
//     kf = 1.00  →  s = −1.00…0.00   a pole ON the unit circle   fragile
//     kf = 0.80  →  s = −0.80…0.00   strictly inside it          safe
//
// while the carrier share kf·r/(1 − kf + kf·r) is 0.79…0.88 either way. 0.80
// therefore buys the entire stability margin for the ~15 % of the carrier the
// integral is already cleaning up — which is the trade "跟枪稳定不抖" asks for.
// 1.20 stays reachable for a game whose alpha the fit cannot reach; above that
// the self-copy is a limit cycle, not a trim.
//
// kFfTauSec was 33 ms. At kf = 0.80 and alpha_hat = 0.1 the feed-forward's noise
// gain is kf/alpha_hat = 8, so this filter is what decides how much of the
// detector's wobble reaches the finger; 45 ms costs a few px of lead and takes a
// stop's worth of shimmer out. It is a DELIBERATE trade, not a free lunch — if
// tracking ever feels soft, this is the first number to put back.
//
// kFfTauFastSec: the release time constant (round-13 point 4). Asymmetric on
// purpose — a rising ŵ stays filtered against detector noise, a falling one is
// followed immediately, because by then the old velocity is not a lead any more,
// it is pure error.
//
// kFfGainMax / kFfVelMaxPx / kFfLimitPx / kFfDeadbandPx: the bounds from
// round-13 point 3 and the deadband from point 5. All four exist so that a
// WRONG alpha_hat is survivable rather than violent; none of them is the
// mechanism.
constexpr float kFfTauSec      = 0.045f;
constexpr float kFfTauFastSec  = 0.015f;
constexpr int   kFfDelaySteps  = 5;
constexpr float kFfStrengthMax = 1.20f;
constexpr float kFfGainMax     = 20.0f;
constexpr float kFfVelMaxPx    = 60.0f;
constexpr float kFfLimitPx     = 120.0f;
constexpr float kFfDeadbandPx  = 0.5f;

// ── Jump detector (pid (1).cpp:40) ──────────────────────────────────────────
//
// |Δe| above this, in screen px for ONE control step, is a re-lock, a flick or
// a target switch rather than target motion. 60 px/step is 7200 px/s of screen
// velocity, well above anything a tracked target produces between detections.
//
// ROUND 14: raised from 30, and the threshold in update() now takes the LARGER
// of this and the target's own BOX WIDTH. Δe contains our own delayed command —
// Δe = w − alpha·u(k−L) — so a bare 30 was close enough to a working strafe to
// fire on real motion, and firing this branch zeroes the soft-start ramp and the
// integral, which is itself a throttle that reads as "跟不上".
constexpr float kJumpResetPx = 60.0f;

// ── Soft start ──────────────────────────────────────────────────────────────
//
// A re-acquisition is a step of 100-300 px of error in ONE frame. The P term
// would convert that straight into a 10-30 px finger lurch, so the feedback is
// faded in from zero over ~4 steps. Continuous, no state bit, no threshold to
// chatter on.
constexpr float kRampRate = 0.30f;

// ── The proximity weight (see round-13 point 6) ─────────────────────────────
//
// kGateFrac is the distance, in TARGET WIDTHS, at which the gain schedule has
// decayed to its floor — the reference's 动态判断阈值 = 动态系数 × 最近目标宽度.
// Expressing it in target widths rather than pixels is the reference's real
// trick: the same pixels mean "far off" on a distant 20 px box and "on target"
// on a 300 px one, so a fixed pixel threshold is either twitchy at range or
// blind up close, while this one scales with distance for free.
//
// kNearRate / kNearDropRate are the reference's own 0.03 and 0.1: opening is
// slow (a gain that comes back too fast re-excites the transient), closing is
// three times faster (a big error must not be driven at full authority).
// kNearFloor is the 0.5 of `比例系数*0.5` — the weight never reaches zero, which
// is what makes this a schedule and not the latch round 12 had to remove.
//
// ROUND 14: the floor is 0.75 and the schedule scales P ALONE. At 0.5 it was
// halving kp on every approach — lengthening exactly the pull-in that "跟不上"
// is about — and it was scaling ki as well, i.e. throttling the DC carrier by 2×
// at the very moment the carrier has to be built. A 0.75 floor still softens a
// re-lock's first frames, which is all this device was ever for, and leaves both
// carrier channels (the integral and the feed-forward) untouched.
constexpr float kGateFrac     = 1.5f;   // |e| >= this × box  ⇒ weight at floor
constexpr float kNearRate     = 0.03f;  // per step, opening
constexpr float kNearDropRate = 0.10f;  // per step, closing
constexpr float kNearFloor    = 0.75f;  // minimum gain multiplier, P only

// ── On-line plant-gain (alpha) estimation ───────────────────────────────────
//
// kAlphaBias: the estimate is deliberately pulled UNDER the true value, because
// that is the safe side of the asymmetry in the header (under-estimate ⇒ the
// residual self-term is damping). The window fit over-estimates a little at low
// alpha and under-estimates at high alpha (表5, +30% at 0.5 … −31% at 5.0), so
// 0.8 lands the used value at 0.55…1.04× the truth across that whole span.
//
// kAlphaHatSeed: what the controller uses BEFORE any fit has succeeded — and
// what it keeps using if no fit ever does. It used to be 1.0, and that was the
// round-13 runaway: in the touch regime the truth is near 0.1, so 1.0 is a huge
// OVER-estimate and the residual self-term turns into +0.9·kf of positive
// feedback. 0.10 is picked to be on the SAFE side — an under-estimate makes the
// residual negative, i.e. damping — and it is simultaneously the right order of
// magnitude for the phone, so an unconverged controller still carries a target
// roughly correctly instead of merely not exploding.
//
// kAlphaStep: the accepted fit is blended in rather than adopted. The window fit
// is refitted every kAlphaSolveEvery steps and its error is ±30 %, so adopting
// each one wholesale would step the loop gain around for no reason.
constexpr float kAlphaBias       = 0.8f;
// ROUND 14: narrowed from 0.05…4.0. The self-copy coefficient is
// s = kf·(1 − alpha/alpha_hat), so a WILD fit is not a cosmetic problem: at
// alpha_hat = 0.05 against a true alpha of 0.5 it is −7.2, |s| > 1, and the copy
// stops being a damping term and becomes a limit cycle. A touch game's plant gain
// does not plausibly sit outside 0.08…2.0, so the clamp can refuse to believe it
// does, and even a clamped value leaves r inside the kf = 0.8 window
// (r ∈ 0.25…2.25 for |s| < 1).
constexpr float kAlphaMin        = 0.08f;
constexpr float kAlphaMax        = 2.0f;
constexpr float kAlphaHatSeed    = 0.10f;
constexpr float kAlphaStep       = 0.35f;
constexpr int   kAlphaSolveEvery = 15;

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
// Nothing here is on the critical path: the controller starts from kAlphaHatSeed
// (0.10 — the SAFE side, never 1.0 again; see the round-13 header section) and
// the whole kf range stays stable with alpha_hat off by 2× either way.
struct AlphaEstimator {
    static constexpr int kWin = 60;      // 0.5 s at 120 Hz

    void reset() {
        head_ = count_ = tick_ = 0;
        cumU_ = 0.0f;
        alpha_ = kAlphaHatSeed;
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
        if (count_ == kWin && (tick_ % kAlphaSolveEvery) == 0) solve();
    }

    /// The value to USE (already biased low and clamped), never unset. Before
    /// the first fit this is kAlphaHatSeed, NOT 1.0.
    float value() const { return alpha_; }
    /// True once a fit has ever succeeded — for the log only.
    bool  valid() const { return valid_; }
    /// The last raw fit, before the bias. For the log only.
    float raw()   const { return raw_; }

private:
    void solve() {
        // Normal equations for parameters [c0, c1, c2, alpha] with regressors
        // [1, k, k², −U]. 4×4 with a partial-pivot Gauss-Jordan; 60 samples once
        // every 15 steps is nothing.
        double M[4][4] = {{0}}, b[4] = {0};
        for (int i = 0; i < kWin; ++i) {
            const double x[4] = {1.0, k_[i], double(k_[i]) * k_[i], -double(U_[i])};
            const double y = e_[i];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) M[r][c] += x[r] * x[c];
                b[r] += x[r] * y;
            }
        }
        // ── Excitation guard, and the round-14 correction to it ──────────────
        //
        // U must vary, AND it must vary in a way the polynomial terms cannot
        // mimic. The original version tested variance alone, which does not catch
        // the case that actually matters: while a target is tracked at a steady
        // speed the command is nearly constant, so U is a STRAIGHT LINE in k —
        // the variance is huge and U is collinear with the k regressor. The
        // normal equations are then near-singular, the solve still returns a
        // number, and that number can be anything inside the clamp.
        //
        // A wild alpha_hat is not cosmetic here: s = kf·(1 − alpha/alpha_hat) is
        // the coefficient on a self-COPY of our own delayed command, so a wrong
        // HIGH alpha_hat is positive feedback — the shake this round exists to
        // remove. The correlation between U and k is the direct test.
        //
        // A high correlation does not mean the guard fires too often: steady
        // tracking is precisely when alpha is unobservable AND when it does not
        // matter, because the reconstruction is then driven by the observed
        // error rather than by the assumed plant gain.
        double uMean = 0.0, uVar = 0.0;
        for (int i = 0; i < kWin; ++i) uMean += U_[i];
        uMean /= kWin;
        for (int i = 0; i < kWin; ++i) uVar += (U_[i] - uMean) * (U_[i] - uMean);
        uVar /= kWin;
        if (uVar < 9.0) return;                        // < 3 px rms of travel: skip

        double kMean = 0.0, kVar = 0.0, ukCov = 0.0;
        for (int i = 0; i < kWin; ++i) kMean += k_[i];
        kMean /= kWin;
        for (int i = 0; i < kWin; ++i) {
            const double dk = k_[i] - kMean;
            kVar  += dk * dk;
            ukCov += (U_[i] - uMean) * dk;
        }
        kVar  /= kWin;
        ukCov /= kWin;
        if (kVar < 100.0) return;
        if (std::fabs(ukCov / std::sqrt(uVar * kVar)) > 0.985) return;

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
        if (!(a > kAlphaMin && a < kAlphaMax)) return;  // reject absurd fits

        // ── Second guard: the fit must actually EXPLAIN the window ────────────
        // A spurious alpha can satisfy the normal equations without describing
        // anything. Requiring the residual to be small against the error's own
        // spread is what catches that. In a steady hold the spread is pure noise
        // and this rejects — correctly, because alpha is not observable then.
        double eMean = 0.0;
        for (int i = 0; i < kWin; ++i) eMean += e_[i];
        eMean /= kWin;
        double rss = 0.0, eVar = 0.0;
        for (int i = 0; i < kWin; ++i) {
            const double fit = x[0] + x[1] * k_[i] + x[2] * k_[i] * k_[i] - a * U_[i];
            const double r   = e_[i] - fit;
            const double d   = e_[i] - eMean;
            rss += r * r;
            eVar += d * d;
        }
        if (eVar < 1e-6) return;
        if (rss > 0.25 * eVar) return;                 // explains < 75 %: not a fit

        raw_ = static_cast<float>(a);
        // BLEND the fit in rather than adopting it (round-13 point 1). The fit is
        // refitted every kAlphaSolveEvery steps with a ±30 % error, so adopting
        // each one wholesale steps the loop gain around for no reason — and the
        // step happens exactly when the loop has just been excited, which is the
        // worst moment for it.
        const float want = std::clamp(kAlphaBias * raw_, kAlphaMin, kAlphaMax);
        alpha_ += kAlphaStep * (want - alpha_);
        valid_ = true;
    }

    float e_[kWin] = {0.0f};
    float U_[kWin] = {0.0f};
    float k_[kWin] = {0.0f};
    int   head_ = 0, count_ = 0, tick_ = 0;
    float cumU_ = 0.0f;   // running sum of the delayed commands == U(k)
    float alpha_ = kAlphaHatSeed;
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

    /// The engaged target's box width in screen px — the UNITS of the proximity
    /// weight's threshold (kGateFrac × this). Passing <= 0 disables the schedule
    /// and leaves P and I at full gain.
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
            nearW     = 0.0f;
        }

        // ── Soft start ────────────────────────────────────────────────────────
        ramp = std::min(1.0f, ramp + kRampRate);

        const float de = error - lastError;

        // ── Jump detector (pid (1).cpp:40) ───────────────────────────────────
        // `if (|Δe| > 30) reset()` in the reference. A re-lock, a flick or a
        // target switch is 100–300 px of Δe in ONE frame, and the reconstruction
        // cannot tell that apart from the target really moving that far: it would
        // read the whole flick as target velocity and fire it through F, which is
        // the lurch the output limiter then treats as a fact of life.
        //
        // What makes it work in the reference is the SECOND half of the trick:
        // its reset also sets kp_gain = 0, and the ENTIRE output is multiplied by
        // kp_gain — so the jump frame emits ZERO and the gains fade back in over
        // the next few frames. ramp is that mechanism here, so the jump branch
        // zeroes ramp. `lastError` is NOT touched: it is tracking the caller's
        // error, and zeroing it would make the derivative read kd·error/dt.
        //
        // THE THRESHOLD IS NOT A BARE 30 px. Δe is not the target's motion — it
        // is  Δe = w − alpha·u(k−L), and our own command is inside it. At the
        // touch regime's alpha ≈ 0.1 that term is at most 18 px, but at alpha = 1
        // a full-output step legitimately moves the error 180 px, so the
        // reference's literal `|Δe| > 30` would reset the loop EVERY frame. The
        // bound on legitimate motion is |ΔT| + alpha·kOutLimitPx, so the threshold
        // is kJumpResetPx plus that (48 px at alpha 0.1, 210 px at alpha 1) — and
        // a flick or a re-lock at either sensitivity still clears it by a wide
        // margin.
        // ROUND 14: the threshold takes the LARGER of a fixed floor and the
        // target's own BOX WIDTH. In one control step a target cannot travel more
        // than its own width unless what changed was not target motion at all —
        // and the box width is the only range-invariant yardstick available here.
        // Our own contribution, alpha_hat·kOutLimitPx, is added on top because
        // Δe contains it.
        const float jumpPx = std::max(kJumpResetPx, boxPx) + alphaHat * kOutLimitPx;
        if (std::fabs(de) > jumpPx) {
            ffVel     = 0.0f;
            ffInhibit = true;
            integral  = 0.0f;
            deriv     = 0.0f;
            nearW     = 0.0f;
            ramp      = 0.0f;
        }

        // ── Proximity weight: the reference's gain SCHEDULE ──────────────────
        // pid (1).cpp's kp_integral()/adjust_integral(), both of them a weight in
        // [0, 1] that chases (1 − |e|/threshold) with a rate limit, plus a
        // faster decay once |e| is past the threshold:
        //
        //     if (|e| <  阈值) w += ((1 − |e|/阈值) − w) · 0.03
        //     else             w += ((阈值/|e|)·w − w) · 0.1
        //
        // The threshold is in TARGET WIDTHS (kGateFrac × box), which is the other
        // reference file's device. The weight multiplies P ONLY (round 14), at a
        // floor of 0.75: at the old 0.5 it was halving kp AND ki on every
        // approach — lengthening the pull-in and throttling the DC carrier at the
        // exact moment the carrier had to be built. What it still does is soften
        // a re-lock's first frames, which is all this device was ever for.
        // The floor keeps it a schedule rather than a latch: round 12 removed the
        // hard 已达标 latch because a weight that reaches zero can never reopen.
        float want = 1.0f;
        float rate = kNearRate;
        if (boxPx > 0.0f) {
            const float gate = kGateFrac * boxPx;
            const float ae   = std::fabs(error);
            if (ae < gate) { want = 1.0f - ae / gate; rate = kNearRate;     }
            else           { want = gate / ae;        rate = kNearDropRate; }
        }
        nearW = std::clamp(nearW + (want - nearW) * rate, 0.0f, 1.0f);
        const float gainScale = kNearFloor + (1.0f - kNearFloor) * nearW;

        // ── Derivative: filtered, PER STEP ───────────────────────────────────
        const float a = dt / (dt + kDerivTauSec);
        deriv += a * (de - deriv);
        float d = std::clamp(kd * ramp * deriv, -outLimit, outLimit);

        // Proportional — per step, NOT scaled by dt (its stability bound is a
        // per-sample one; see the class note).
        float p = kp * ramp * gainScale * error;

        // Deadzone: suppress the proportional and derivative terms only.
        if (frozen) { p = 0.0f; d = 0.0f; }

        // ── Feed-forward: reconstruct the target's OWN screen velocity ───────
        // ŵ = Δe + alpha_hat·u(k−L), then scaled to finger px by 1/alpha_hat and
        // by the strength kf. ONE low-pass covers both terms — filtering them
        // separately leaves a high-passed copy of our own command at unity gain,
        // which is the trap in the header. u(k−L) is the delay-aligned sample out
        // of the ring: the command that actually produced the Δe we are looking
        // at, NOT the previous step's output.
        //
        // NOT gated. Round 12 established why: F is a MEASUREMENT (ŵ is
        // reconstructed from the Δe just observed), not the "bet that the target
        // keeps going" the old gate's rationale describes, so suppressing it
        // protected nothing and latched the loop out of the regime it exists for.
        // What guards it instead is the JUMP branch above — which is the
        // reference's own answer to the same problem (pid (1).cpp:40), and unlike
        // a latch it cannot get stuck.
        //
        // Bounded four ways (round-13 point 3): the reconstruction velocity, the
        // gain, the resulting command, and a deadband. The bounds are there so
        // that a wrong alpha_hat is survivable; alpha_hat being right is what
        // makes F a feed-forward rather than a slow oscillator.
        //
        // ASYMMETRIC low-pass (round-13 point 4): a RISING ŵ stays filtered at
        // kFfTauSec against detector noise, but a FALLING one is followed at
        // kFfTauFastSec. When a target stops, the old velocity is no longer a
        // lead — it is pure error — and holding it for the 33 ms a symmetric
        // filter would is what makes the crosshair sail past and get dragged
        // back: the user's "目标停下会左右抖几下".
        float ff = 0.0f;
        if (kf > 0.0f && alphaHat > 0.0f && !ffInhibit) {
            const float gain = std::min(kf / alphaHat, kFfGainMax);
            const float meas = std::clamp(de + alphaHat * uHist[uHead],
                                          -kFfVelMaxPx, kFfVelMaxPx);
            const float tau  = (std::fabs(meas) < std::fabs(ffVel))
                                   ? kFfTauFastSec : kFfTauSec;
            const float af   = dt / (dt + tau);
            ffVel += af * (meas - ffVel);
            ff = std::clamp(gain * ffVel, -kFfLimitPx, kFfLimitPx);
            // Deadband, pid (1).cpp:55 (`|ki_raw| > 0.5` else 0): without it a
            // small reconstruction wanders around zero at rest and is integrated
            // by nothing but the detector.
            if (std::fabs(ff) < kFfDeadbandPx) ff = 0.0f;
        }
        ffInhibit = false;

        // ── Integral: the DC carrier, unconditioned and quick to let go ──────
        //
        // ROUND 14 — three changes, all of them about one complaint
        // ("ki 会导致过冲并且很慢的描回来"):
        //
        //   1. gainScale is GONE from this line. The proximity schedule was
        //      throttling the carrier by up to 2x at exactly the moment the
        //      carrier had to be built.
        //   2. UNWIND FASTER THAN YOU WIND. The integral used to come back at
        //      exactly the rate it went out — ki·e·dt — so a 50 px trim built
        //      against a strafe took 50/(ki·|e|·dt) frames to give back. At
        //      ki = 0.1, e = −20 px, 120 Hz that is 3000 frames: twenty-five
        //      seconds. Once the error has reversed the trim is no longer wanted,
        //      so it is now released kIntReleaseGain times faster.
        //   3. BACK-CALCULATION, so the integral cannot park a reserve behind a
        //      clipped output and spend it later.
        const float uWant = p + integral + d + ff;
        const bool pushHi = uWant >=  outLimit && error > 0.0f;
        const bool pushLo = uWant <= -outLimit && error < 0.0f;
        if (!frozen && !pushHi && !pushLo) {
            const bool opposing = (integral * error) < 0.0f;
            integral += (opposing ? kIntReleaseGain : 1.0f) * ki * error * dt;
            integral  = std::clamp(integral, -trimLimit, trimLimit);
        }

        float u = softLimit(p + integral + d + ff, outLimit);
        // Give back exactly what the limiter refused to deliver, so the trim
        // tracks the command that was SENT rather than the one that was asked for.
        if (!frozen && std::fabs(p + integral + d + ff) > outLimit) {
            integral = std::clamp(integral + (u - (p + integral + d + ff)),
                                  -trimLimit, trimLimit);
            u = softLimit(p + integral + d + ff, outLimit);
        }

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
        nearW     = 0.0f;
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
    /// `integralValue()` is the DC carrier's reserve. It should be modest on a
    /// moving target and near zero on a still one; pinned at ±trimLimit means the
    /// carrier is bigger than the leash allows and kf should be carrying more of
    /// it.
    ///
    /// `nearWValue()` is the proximity weight: near 1 while the aim is on target,
    /// near 0 while it is far off. If it sits at 0 while the crosshair is clearly
    /// on target, the BOX WIDTH the caller passes in is wrong — the threshold is
    /// measured in target widths, not pixels.
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
    /// The proximity weight in force, 0…1 (was the gate's boolean).
    float nearWValue()     const { return nearW; }
    /// Kept for the log: "on target" now means the schedule is mostly open.
    bool  settledNow()     const { return nearW > kNearFloor; }

private:
    float kp        = 0.10f;
    float ki        = 0.5f;
    float kd        = 0.20f;
    float outSmooth = 1.0f;
    float kf        = 1.0f;
    float alphaHat  = kAlphaHatSeed;   // NEVER 1.0 by default — see round 13

    // ── State ─────────────────────────────────────────────────────────────────
    float integral  = 0.0f;
    float lastError = 0.0f;
    float outPrev   = 0.0f;
    float deriv     = 0.0f;   // filtered one-step change of the error
    float ramp      = 0.0f;   // soft-start ramp, 0..1
    float ffVel     = 0.0f;   // filtered ŵ, the target's own screen velocity
    bool  ffInhibit = false;  // one step of silence after a jump or a switch
    float outLimit  = kOutLimitPx;
    float trimLimit = kTrimLimitPx;

    // Proximity weight (the reference's gain schedule)
    float boxPx     = 0.0f;   // engaged target's box width; 0 = schedule off
    float nearW     = 0.0f;   // 0…1, rate-limited; P and I follow it

    float uHist[kFfDelaySteps] = {0.0f};  // our own commands, for u(k−L)
    int   uHead    = 0;                   // index of the OLDEST entry
};

}  // namespace tracking
}  // namespace aimbotng
