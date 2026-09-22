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
//
//  ── ROUND 15: THE DEADBAND WAS NOT CONNECTED TO THE INTEGRAL ────────────────
//
//  The user tuned it himself and got it tracking: kp 0.05 / ki 0.20 / kd 0.26 /
//  kf 0.00 held a moving target steadily. Two symptoms were left, and he named
//  the cause of both:
//
//      "静止不动的时候视角也一直晃 … 原因是ki。死区调1也没用。"
//      "跳起来并且运动，这个时候y也会动，你的算法会变成一个一个阶梯状很抖。"
//
//  He is right, and the two symptoms are ONE bug. Inside the stop band the axis
//  used to emit
//
//      u = 0·P + integral + 0·D + ff
//
//  — the proportional and derivative terms were suppressed, and the INTEGRAL
//  was merely "held" (not accumulated) and still added to the output. So the
//  one term the band was supposed to silence was the only term still driving:
//
//    * AT REST the trim is a standing charge with no restoring force. P — the
//      only term that can unwind it — is switched off by the band itself, and
//      the discharge path (kIntReleaseGain) needs |e| to be large, which by
//      definition cannot happen while the aim is ON target. So the charge
//      survives, keeps pushing the finger, walks the view off the target, the
//      axis exits the band, P snaps it back — a relaxation cycle whose amplitude
//      is the band width. That is the "一直晃", and it is why a bigger deadband
//      did not help: 死区只管住了 P，没管住 I.
//
//    * ON A MOVING TARGET the same cycle repeats in whichever axis is inside
//      its band (a jump moves Y fast, so Y spends time crossing in and out), and
//      because the push and the snap alternate with the aim's integer finger
//      steps, what the user sees is a STAIRCASE: creep, snap, creep, snap. The
//      "阶梯状" is a relay limit cycle, not a quantisation artefact.
//
//  THE FIX, and the trap inside the fix (round 16):
//
//    1. Inside the band the integral STOPS ACCUMULATING. A bias has no
//       justification while the aim is on target.
//    2. Its contribution is NEVER suppressed — only its rate is gated. Round 15
//       suppressed the contribution (iTerm = 0) as well, and that is the whole
//       of "乱甩乱晃": u is a per-step displacement, so dropping a trim of N
//       px/step the moment the band is entered and restoring it on the way out
//       is a velocity step of N px/step at every band crossing. With kf = 0 the
//       band therefore zeroed the axis completely (P, D and I all off) and the
//       axis banged between "nothing" and "everything".
//    3. Instead it is BLEED TOWARD THE MEASURED CARRIER (kDzTrimBleedTauSec),
//       not toward zero. carry = ffVel/alpha_hat is what the reconstruction says
//       the target needs; at rest that is 0 (so the standing charge still dies —
//       the round-15 complaint is genuinely fixed) and on a strafe it is the
//       carrier itself (so crossing the band does not cost the target).
//    4. The feed-forward is still added, unchanged. It is a MEASUREMENT, not an
//       accumulated charge, and at kf = 0 it contributes nothing while the
//       measurement it is built on — ffVel — is what the bleed is aimed at, so
//       it must be computed whether kf is zero or not. That is why the ŵ
//       low-pass no longer sits behind `if (kf > 0)`.
//    5. P and D are switched off inside the band, and that switch is NOT the
//       same problem: inside their band P <= kp x band and D is bounded by the
//       error's rate, both well under 1 px/step at any sane gain, so the step
//       they make at the boundary is invisible. The integral's is not.
//
//  And kf's range is now the user's: 0.00–0.50, default 0.05. Round 14 argued
//  for 0.80 from the self-copy stability window; the user's own tuning found
//  that the INTEGRAL carries the DC command perfectly well once its leash and
//  its discharge rate are right, and a small kf is then a noise-free trim on top
//  rather than the carrier. kf is his knob — 0.50 is the ceiling because the
//  self-copy coefficient s = kf·(1 − alpha/alpha_hat) cannot leave the unit
//  circle there even with the estimator off by 2×, so the whole slider is
//  usable instead of ending in a runaway.
//
//  ── ROUND 17: THE BLEED'S TRIGGER WAS INSIDE ITS OWN SYMPTOM ────────────────
//
//  The user, on the round-16 build, with the loop finally tracking well
//  (kp 0.05 / ki 0.22 / kd 0.26 / kf 0): "低kp高ki这样能稳定跟枪，但是高ki，目标
//  停下就会晃". He also reported the other half of the same report in the same
//  breath and it is the key: "死区调1也没用".
//
//  Both sentences are one sentence. Round 15/16 built a bleed for exactly this
//  shake and gated it on the stop band, |e| <= dz — and the shake is a LIMIT
//  CYCLE WHOSE AMPLITUDE IS THE BAND. So:
//
//      the frames where the bleed is needed are the frames where |e| > dz,
//      i.e. the frames where the bleed is switched off.
//
//  A bigger deadzone cannot help, and that is not a tuning observation, it is
//  arithmetic: enlarging the band enlarges the cycle that escapes it. The two
//  mechanisms were fighting over one trigger and the symptom won.
//
//  THE FIX IS TO TRIGGER ON A MEASUREMENT INSTEAD OF ON THE ERROR, and the
//  measurement was already being computed. carry = ffVel/alpha_hat is the
//  per-step velocity the reconstruction says the target needs, and it is built
//  from Δe + alpha_hat·u(k−L) — our OWN delayed command is subtracted out of it.
//  That single property is what makes it immune to the symptom: while the loop
//  oscillates against a target that is standing still, carry still reads ~0,
//  where the error reads "everything". On a strafe carry is the standing carrier
//  itself, so the same rule leaves a working trim alone.
//
//      |trimWant| ~ 0 for kStopHoldSteps consecutive steps  ⇒  the target is at
//      rest  ⇒  bleed the trim toward trimWant (== 0), at kStopBleedTauSec.
//      (trimWant = carry − ff, the trim's own share of the carrier — round 19b.)
//
//  Three things this deliberately does NOT do:
//
//    * it does not suppress the trim's contribution (round 16's rule — the bleed
//      moves a RATE);
//    * it does not use a zero-crossing count as the trigger. A counter fires on
//      every real turnaround and buys nothing here: the reconstruction above
//      already answers "is the target moving", and the existing kIntReleaseGain
//      path already handles a sign reversal;
//    * it does not gate the accumulation while bleeding. At ki = 0.22 and 2 px
//      of error the accumulation settles the trim at ki·e·tau ≈ 0.009 px/step
//      against the discharge, so a second gate would only be a second place to
//      get the rate-vs-contribution rule wrong.
//
//  Cost, stated: a target that holds still for 100 ms in the middle of a weave
//  loses its trim there and has to rebuild it. That is the trade the report asks
//  for — a stationary target no longer shakes — and kf > 0 covers the gap with a
//  measurement rather than a charge. `stop=` on the aim telemetry line is this
//  detector's state, per axis; read it with `trim` and `cr`.
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
//
// ROUND 19c raised it 6 -> 20. This is the ONE constant in the stop path that
// can be raised without touching the tracking case, and the reason is that it is
// self-scaling: what it moves is 20·ki·|e|·dt, so a jitter-flipped error (|e| a
// fraction of a pixel) moves almost nothing, while a genuine overshoot (|e| tens
// of pixels) is emptied fast. That is also why it is safe on a target that
// reverses — a strafe CHANGES SIGN with |e| small at the crossing, and what is
// left there is the accumulator being carried across, which a faster unwind
// helps rather than hurts (verified: the constant is bit-identical on a
// constant-velocity strafe and marginally better on a 0.6 s zig-zag).
//
// What it fixes is the second half of the user's "先过冲然后立马拉回来，之后震荡"
// (round 19c). The ff half of the carrier goes away in 15 ms on its own, but the
// trim half can only be redeemed by reversed error — and the reversed error IS
// the overshoot. At 6 the redemption lagged the measurement badly enough that
// the trim was still pushing 8 steps after the target had stopped, which is what
// the simulation shows as a 58 px peak against a 25 px floor set by
// kFfDelaySteps alone.
constexpr float kIntReleaseGain = 20.0f;

// ── The stop band's trim bleed (round 15, corrected in round 16) ────────────
//
// Inside the deadzone the integral stops ACCUMULATING and is pulled toward the
// carrier the target actually needs, with this time constant. Its CONTRIBUTION
// is never removed. That correction is the whole of round 16, and it is what the
// user's "乱甩乱晃" was:
//
//   A VELOCITY TERM MAY BE GATED IN RATE, NEVER IN CONTRIBUTION. u is finger px
//   PER STEP, so a trim of 50 px/step is a 6000 px/s slide. Switching that term
//   off on the frame the band is entered and back on when it is left is a step
//   in velocity of |trim| px/step, once per band crossing — and a loop handed a
//   step that large can do nothing but bang between the two states. Round 15
//   removed the contribution AND bled the charge, so with kf = 0 the band zeroed
//   the axis outright (P, D and I all off) and the axis alternated between
//   "nothing" and "the whole command": flinging, at the band's own rate.
//
// So the band does two things and only two: P and D stop driving — they are the
// terms that amplify the detector's jitter, which is the band's actual job — and
// the integral is pulled toward what the loop MEASURES the target to need,
//
//     carry = ffVel / alpha_hat        (finger px/step, from the reconstruction)
//     trimWant = carry − ff            (round 19b: the trim's own SHARE of it)
//
// instead of toward zero. That distinction is load-bearing at exactly two
// moments:
//
//   * AT REST carry is 0, so a standing charge is bled off and stops walking the
//     view off the target — round 15's complaint, still fixed.
//   * ON A STRAFE carry IS the WHOLE carrier, and the trim is bled toward the
//     part of it the feed-forward is NOT supplying — (1−kf) of it — so crossing
//     the band no longer destroys what the loop learned and a moving target is
//     still followed through it. Round 15 broke this by bleeding to zero, and
//     with kf = 0 there was then nothing left to carry motion at all (burst,
//     stall, burst).
//
// Before round 19 the second line read `trimWant = carry`, and that was correct:
// with kf at 0.05, F supplied 5 % of the carrier and the trim's share was 95 % of
// the whole thing. Raising the kf ceiling to 0.80 silently invalidated it — at
// kf = 0.50 the target became twice the trim's share, the sum came out at
// 1.5·u_ss, and the band turned into a limit cycle that reads on the device as
// occasional left–right sway on the axis carrying the carrier. The fix is stated
// as a conservation (whole minus what F gave) rather than as (1−kf)·carry so that
// it stays right while ff is clamped, deadbanded or transient.
//
// 0.10 s, not 0.30: the bleed is only ever visible as the difference between the
// charge and the measured carrier, and at rest that difference is the whole
// charge — so it may not take a third of a second to leave. The 0.30 s round 15
// used is part of why "视角一直晃" survived that build.
constexpr float kDzTrimBleedTauSec = 0.10f;

// ── The stop detector: the bleed's REAL trigger (round 17) ───────────────────
//
// Rounds 15/16 gave the trim a bleed but wired its trigger to the STOP BAND
// (|e| <= dz). Held on a stationary target inside the band that is correct, and
// it is also useless for the symptom the user actually reported, because of a
// small trap worth stating plainly:
//
//     AN OSCILLATION KEEPS |e| OUTSIDE THE BAND BY CONSTRUCTION.
//
// The limit cycle's amplitude IS the band width (round 15's relaxation cycle:
// creep, snap, creep), so the very frames this bleed exists to fix are the
// frames where |e| > dz and the bleed is switched off. The mechanism was
// competing with its own symptom for its trigger — and losing. That is the
// precise reason "死区调 1 也没用" kept being true, whatever the band was set to.
//
// So the trigger moves onto a MEASUREMENT: carry = ffVel/alpha_hat, the per-step
// velocity the reconstruction says the target needs. Two properties make it the
// right variable, and the error has neither of them:
//
//   * carry is built from Δe + alpha_hat·u(k−L) — OUR OWN COMMAND IS SUBTRACTED
//     OUT OF IT — so while the loop oscillates against a STATIONARY target,
//     carry still reads ~0. It is immune to the symptom. The error is the
//     symptom.
//   * on a strafe carry IS the standing carrier, so the bleed leaves the trim
//     exactly where the trim is doing real work — its own share of it, after F's
//     part is subtracted (round 19b). Nothing has to be told which case it is in;
//     the measurement answers it.
//
// kStopBleedTauSec is FAST (20 ms) because it has to outrun the limit cycle it
// fights: 3 Hz is ~40 steps per period, and a charge that survives most of one
// period has already walked the view off the target.
//
// kStopHoldSteps is the price of trusting a noisy measurement: the condition must
// hold for this many CONSECUTIVE steps before the bleed engages (the counter
// resets the instant it fails, so a resumed strafe is followed at once). It is
// the only thing standing between "the target stopped" and "the target reversed
// for a step". A weave crosses zero for a step or two; a stop stays there.
//
// ROUND 19c halved it, 12 -> 6 (100 ms -> 50 ms). It was 12 to keep a MISFIRE
// from throwing away a charge the loop still needed, and round 19b removed that
// price: the bleed's target is now trimWant, the trim's own SHARE of the carrier,
// so a misfire no longer empties anything — it pulls the trim onto the value the
// measurement is asking for, which on a strafe is the value it already holds.
// What was left was pure delay, and delay here is paid in overshoot: the counter
// only starts once ffVel has already decayed past the eps (6 steps), so 12 put
// the discharge 18 steps after the stop, and the trim kept pushing for all of
// them. Verified by simulation: 58 px peak -> 48 px at 12 -> 6 with a
// constant-velocity strafe bit-identical, and the residual floor is now
// kFfDelaySteps·w (the commands already in flight), not the counter.
//
// kStopHoldSteps needs a condition, and ROUND 22 replaced the one it had. The old
// form —
//
//     |trimWant| < kStopCarryEpsPx   OR   |trimWant| < kStopCarryFrac · |trim|
//
// — reasoned carefully about trimWant = carry − ff being the carrier's TRIM SHARE
// and about the ratio carry/trim = 1/(1−kf) getting harder as kf rises, and every
// step of that reasoning is sound. It failed for a reason none of it addressed: at
// the instant of the stop the measurement IS NOT SMALL. meas goes from ~0 (steady
// strafe, de ≈ 0) to (alpha_hat − alpha)·u ≈ −2·u_ss, so |trimWant| does not drift
// below 1 px — it jumps UP to roughly twice the trim. Neither disjunct can then
// hold (the second would need |trim| > 2·|trimWant|), the counter never reached
// kStopHoldSteps, and the charge standing at the moment of the stop was never
// released at all. See kStopDropThreshold.
//
// The replacement asks a different question — not "is the charge justified?"
// (which presumes the measurement is a valid carrier at that instant) but "did the
// target stop?" — and answers it from the drop ratio, which is what makes it
// independent of alpha_hat's bias. Once the answer is yes, trimWant has already
// been forced to zero at the assignment above, so the bleed is a straight
// discharge rather than a discharge onto a target that is itself a fault.
//
// (`cr` on the aim telemetry line still prints the WHOLE carrier, so at kf = 0.5
// on a strafe the trim should read about half of it — that is the split working.)
//
// A zero-crossing COUNT is deliberately not a trigger here. It fires on every
// real turnaround too, and "the target really did reverse" is handled by the
// existing kIntReleaseGain path; all a counter would add is a reason to kill the
// trim on legitimate motion. The measurement above already answers the question
// the counter was being asked.
//
// Same rule as every other gate in this file (round 16): this moves the
// integrator's RATE, never its CONTRIBUTION. trimWant is the target, and the
// value is still added to the output on the step it is bled.
// ROUND 22 raised this from 0.02 s. The release is now driven by a trigger that
// actually fires (see kStopDropThreshold), so its RATE is what decides whether
// the stop is smooth or a step: at 0.02 s a 15 px charge vanished in ~2 steps =
// 6 px/step of velocity step, which is precisely the "乱甩" fault round 16 was
// written to avoid. 0.15 s spreads the same release over ~18 steps.
constexpr float kStopBleedTauSec = 0.15f;
constexpr int   kStopHoldSteps   = 6;

// ── ROUND 22: the stop detector reads a DIMENSIONLESS ratio ──────────────────
//
// Round 17's trigger was `|trimWant| < 1px || |trimWant| < 0.5·|integral|`, and
// it cannot fire. trimWant is built from carry = ffVel/alpha_hat, and the instant
// the target stops, de flips from ~0 to −alpha·u while meas becomes
// (alpha_hat − alpha)·u — a CHANGED number, not a vanishing one. At
// rho = alpha_hat/alpha ≈ 1/3 that is −2·u_ss, so |trimWant| jumps UP to ~2–3x
// the trim: `wantMag < 1px` is false by an order of magnitude, and
// `wantMag < 0.5·|integral|` needs |integral| > 2·wantMag ≈ 70 px when the trim
// really only holds ~15 px. The charge standing in the integrator at the moment
// of the stop is therefore never released — it keeps driving the crosshair past
// the target, the error reverses, P and D drag it back, the residual is charged
// again, and the axis rings. That is the reported "目标停下就一直左右晃".
//
// The fix is to stop asking alpha_hat anything. "The target stopped" physically
// means ITS SPEED collapsed, and ffVel is our only estimate of that speed — so
// the trigger is the FRACTIONAL DROP of |ffVel|, which carries no units and no
// alpha_hat. pid_chirs.cpp:632 uses the same device (`drop = (|v_prev| −
// |v|)/|v_prev|`), and it is why that controller needs no calibrated alpha to
// survive a stop. This is the "两个状态，两套行为" the user asked for: while the
// target moves (drop ≈ 0) nothing here runs at all and the follow is untouched.
//
// kStopDropVelMinPx guards the denominator: at rest |ffVel| is a few tenths of a
// px/step of reconstruction noise, and a ratio taken there is meaningless (and
// would fire on every frame). It must NOT be set from intuition about "walking
// speed", because ffVel is a SCREEN px/step and on a steady strafe it settles at
// ρ·w — at the seed rho = 1/3 and w = 5 px/step that is only ~1.7, so a floor of
// 2.0 would have switched the whole detector off for exactly the user it is for.
// 0.5 is ~2x the at-rest noise floor. kStopDropReleaseSec lets the flag latch
// briefly, so one noisy step cannot end the "stopped" state mid-release.
constexpr float kStopDropThreshold  = 0.45f;
constexpr float kStopDropVelMinPx   = 0.5f;
constexpr float kStopDropReleaseSec = 0.25f;

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
// ── ROUND 19: the kf ceiling is 0.80, and 0.05 is still the default ─────────
//
// kFfStrengthMax is the top of the kf slider. Round 15 set it to the user's own
// range (0.50); round 19 widens it to 0.80, the last value the self-copy window
// below calls SAFE. The range and the stability argument had been talking past
// each other for two rounds: 0.50 was derived as the top of a USABLE range and
// then quoted as a ceiling, while the controller's own log line (ffValue() is
// THE number to watch … at kf ≈ 1.0 it should settle at about ΔT/alpha — the
// WHOLE DC carrier) describes a working point the slider could not reach.
//
// What decides the ceiling is not the SIZE of kf. The DC carrier splits
// kf / (1 − kf) with alpha_hat cancelling exactly (round-18 section), so kf is a
// choice about how much of the DC command and how much of the detector's noise
// reach the finger — NOT about whether the loop can track. What kf does buy,
// dangerously, is gain on the ECHO: s = kf·(1 − alpha/alpha_hat) is the
// coefficient on a copy of our own delayed command, so a HIGH kf on an
// alpha_hat that is merely PLAUSIBLE is the shake this round exists to remove.
// That is a question about alpha_hat being CORRECT, not about kf being large,
// and round 19 answers it with the guard below instead of by clipping the knob.
//
// With the estimator's bias and its ±30 %, r = alpha/alpha_hat lands near
// 1.0…2.0, so
//
//     kf = 1.00  →  s = −1.00…0.00   a pole ON the unit circle   fragile
//     kf = 0.80  →  s = −0.80…0.00   strictly inside it          safe
//     kf = 0.50  →  s = −0.50…0.00   half the margin, still safe  usable
//
// and the whole 0.00–0.80 slider is therefore reachable without a runaway, and
// the top of it is now the value this derivation actually endorses.
//
// kFfTauSec was 33 ms and is 45 ms. At kf = 0.80 and alpha_hat = 0.1 the
// feed-forward's noise gain is kf/alpha_hat = 8; at the new default of 0.05 it
// is 0.5, i.e. the feed-forward is no longer the loudest path for detector noise
// either, so this filter is a belt rather than the mechanism now. It is kept
// because a user who raises kf to 0.80 still needs it.
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

// kReconHoldMaxSteps: round 20. The reconstruction samples Δe only on steps the
// tracker did NOT correct (see AimController::update's `measurementCorrected`),
// because on a corrected step Δe carries the Kalman innovation rather than a
// velocity. In the normal regime that is a clean sample every other step — the
// detector is ~60 Hz against a 120 Hz loop — and two consecutive corrected steps
// never happen. They DO happen the moment the render loop stalls to the
// detector's rate: then every frame consumes a new result, `reconHold` runs up
// unchecked, and holding "until the next clean sample" would freeze the velocity
// for ever, which reads as 完全跟不上 — a far worse failure than the ring this
// exists to remove. So the hold is bounded: after this many consecutive corrected
// steps the sample is taken anyway. 2 is the smallest value that leaves the
// 60 Hz-detector regime untouched (a clean step always intervenes) while keeping
// the degenerate regime at a 50 % sample rate instead of zero.
constexpr int   kReconHoldMaxSteps = 2;
constexpr float kFfStrengthMax = 0.80f;
constexpr float kFfGainMax     = 20.0f;
constexpr float kFfVelMaxPx    = 60.0f;
constexpr float kFfLimitPx     = 120.0f;
constexpr float kFfDeadbandPx  = 0.5f;

// ── ROUND 19: the kf GUARD — a cap on the effective strength, not on the knob ─
//
// The complaint that started this round is precise — "ki 可以让跟枪很稳，但是目标
// 停止之后就会震荡" — and the tuning the user found for it is "kf 小了不抖但跟不上,
// kf 大了跟得上但抖". Both halves are real, and they are two different mechanisms:
//
//   * kf SMALL is quiet because the echo gain s = kf·(1 − alpha/alpha_hat) is
//     small, and it does not keep up because the integral then has to carry
//     (1 − kf) of the DC command — which is exactly what "ki 低了跟不上" is.
//   * kf LARGE keeps up for the same sentence read backwards, and shakes because
//     |s| grows with kf, and s is a gain on a copy of our OWN delayed command.
//     Round 18 measured the crossing point on the device: 0.05 quiet, 0.15 quiet,
//     0.50 an even-amplitude ring, 1.00 divergent.
//
// So |s| < 1 is the whole game, and s depends on alpha_hat being RIGHT — not on
// kf being small. The seed is a guess (kAlphaHatSeed = 0.10) and the estimator
// can take a second to corroborate it, which is the second the user sees as
// "第一次启动自瞄会一直晃，多晃几下就平稳了": the shake was the excitation the
// estimator needed before it would fit anything.
//
// Round 19 therefore caps the EFFECTIVE strength while the estimate is
// uncorroborated and lifts the cap once it is. Two consequences:
//
//   1. The slider still means what it says. It is clamped only for as long as
//      nobody has confirmed alpha_hat, never permanently — a user who wants to
//      work at 0.80 gets 0.80, with an estimate that two independent fits agree
//      on. The working point stays his knob; just the guard is ours.
//   2. The startup shake goes away by CONSTRUCTION rather than by tolerance: the
//      loop starts under the cap, i.e. guaranteed quiet, and the excitation the
//      estimator needs comes from the aim's own approach instead of from the
//      oscillation. Round 18's "shake for a second, then settle" cannot arise any
//      more, because nothing about the loop needs the shake.
//
// 0.15 is not a tuning value — it is the largest strength round 18 measured as
// quiet at the WORST alpha_hat the clamp can hold. It is a floor of safety, so it
// is deliberately NOT a slider.
constexpr float kFfGuardStrengthMax = 0.15f;
// How fast the effective strength may travel toward the slider's value once the
// cap is lifted. Per RENDER frame (setGains runs there), so it is a rate and not
// a time constant — ~0.5 s at 120 Hz. A step in kf is a step in the DC carrier's
// split, i.e. a velocity step, which is the same class of fault as round 15's
// "乱甩乱晃"; so it moves like every other gate in this file: a RATE, never a jump.
constexpr float kFfGuardRate        = 0.05f;

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
// ROUND 19b: the rate at which the CONTROLLER adopts a new alpha_hat, per control
// step. The estimator moves in visible jumps — it refits every kAlphaSolveEvery
// steps and blends by kAlphaStep, and the value the controller is handed also
// steps when the second fit joins the average — and alpha_hat is now the
// DENOMINATOR of both the feed-forward gain (kf/alpha_hat) and the measured
// carrier (ffVel/alpha_hat). The DC cancellation ff = kf·u_ss only holds once
// ffVel has re-converged, which takes kFfTauSec, so every jump in alpha_hat is a
// kFfTauSec-long ripple on the term that now carries most of the command.
// Adopting it as a rate is that ripple divided by ~30; it costs nothing, because
// alpha belongs to the GAME and not to the engagement — a plant gain that takes a
// second to be adopted is not a plant gain that was ever needed sooner.
constexpr float kAlphaUseRate    = 0.01f;

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
    // ROUND 19: 60 → 24. At 60 the first fit could not happen until 0.5 s of
    // tracking had elapsed, and the guards below then had to be satisfied by a
    // window that is mostly a steady hold — which is precisely the window in
    // which u does not vary and the fit is refused. The estimator was therefore
    // slow for a reason that also made it unlikely, and the loop paid for it
    // with the startup shake (see kFfGuardStrengthMax). A 0.2 s window at
    // 120 Hz is 24 samples, which is still far more than the 4 parameters need.
    static constexpr int kWin = 24;      // 0.2 s at 120 Hz

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
        // ROUND 19: 9.0 (3 px rms) → 1.0 (1 px rms). The old number asked the
        // command to swing 3 px rms before alpha was even looked for, and a
        // tracking loop that is doing its job does not swing that much: the only
        // windows that ever qualified were the approach and the flick, never the
        // hold. The collinearity test below is the guard that actually protects
        // the fit from a steady hold, so this one need not be strict as well.
        if (uVar < 1.0) return;                        // < 1 px rms: no spread, alpha unobservable

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
        // A window of consecutive ticks has variance (kWin² − 1)/12, i.e. 47.9 at
        // kWin = 24 and 300 at the old 60. So this is a singularity valve, not a
        // selectivity knob — it can only fire when the window is NOT filled with
        // consecutive steps. ROUND 19 lowers the threshold with the window: left
        // at 100 it would have rejected every single fit, silently, because 47.9
        // < 100.
        if (kVar < 20.0) return;
        // ROUND 19: 0.985 → 0.97. This correlation is the direct test for "u is
        // just a straight line in k", so it is the guard doing the real work —
        // and 0.985 is close enough to 1 that a slightly wandering command
        // passes while U and k are still nearly collinear, which is how a
        // spurious alpha gets through. The window is 4x shorter now, so the same
        // protection costs less selectivity and it can afford to be tightened.
        if (std::fabs(ukCov / std::sqrt(uVar * kVar)) > 0.97) return;

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

// ── Slope alpha estimator (round 19) ────────────────────────────────────────
//
// The identity the feed-forward already reconstructs from (see update()) is
//
//     Δe_k = w_k − alpha · u(k−L)
//
// Δe is the error's one-step difference and u(k−L) the delay-aligned command —
// the same pair of numbers the estimator above consumes, read at the same point
// in the loop, so this costs no extra plumbing. Fitting that line answers "what
// is alpha" with ONE regressor instead of four:
//
//     slope      = −alpha
//     intercept  =  w   (the target's own screen velocity, free of alpha_hat)
//
// WHY A SECOND ESTIMATOR AND NOT A REPLACEMENT. The 4-parameter fit above is
// computed on U, the CUMULATIVE command. That is what forces it to need a long
// window and a large uVar before it will even try, and what forces it to carry a
// c0 + c1·k + c2·k² polynomial plus a guard against that polynomial being
// collinear with U. Differencing removes the polynomial by construction: a
// smoothly moving target contributes to Δe only its own ΔT, which over a short
// window is well described by a STRAIGHT line — and that is exactly what c1·k
// absorbs here.
//
// What differencing does NOT remove is the blind spot. With a constant command
// the regressor has no spread and nothing is identified, so returning without a
// new answer is correct behaviour rather than a failure: that is the moment alpha
// is unobservable AND does not matter, because the reconstruction is then driven
// by the observed error and not by the assumed plant gain.
//
// The two fits are genuinely independent — cumulative positions with a quadratic
// detrend versus differences with a linear one, over different window lengths.
// Two independent methods landing on the same alpha is real evidence, and that
// agreement is what `AimController::step()` gates the kf guard on (see
// kFfGuardStrengthMax). Neither estimator is trusted on its own.
//
// ROUND 19 deliberately gives this one NO residual guard. The polynomial fit
// needs one because a 4-parameter solve over a short window can satisfy the
// normal equations while describing nothing; a 2-parameter line cannot — and the
// question a residual guard was really answering ("is this fit believable?") is
// answered far better by two independent fits agreeing than by a threshold on
// either one of them.
struct SlopeAlphaEstimator {
    static constexpr int kWin = 24;      // 0.2 s at 120 Hz

    void reset() {
        head_ = count_ = tick_ = 0;
        havePrev_ = false;
        prevE_ = 0.0f;
        alpha_ = kAlphaHatSeed;
        valid_ = false;
    }

    /// One control step. `e` is the raw error in screen px and `uDelayed` the
    /// command from kFfDelaySteps steps ago — exactly the arguments the
    /// polynomial estimator takes.
    void push(float e, float uDelayed) {
        if (!(std::isfinite(e) && std::isfinite(uDelayed))) return;
        // The first sample can only seed the difference; every step after it
        // contributes one pair. The pairing is (Δe_k, u(k−L)) — the one the
        // physical identity describes — NOT (Δe_k, u_k).
        if (!havePrev_) { prevE_ = e; havePrev_ = true; return; }
        const float de = e - prevE_;
        prevE_ = e;

        dE_[head_] = de;
        U_[head_]  = uDelayed;
        k_[head_]  = static_cast<float>(tick_);
        head_ = (head_ + 1) % kWin;
        if (count_ < kWin) ++count_;
        ++tick_;
        if (count_ == kWin && (tick_ % kAlphaSolveEvery) == 0) solve();
    }

    /// The value to USE, read alongside the polynomial estimator's. Never unset.
    float value() const { return alpha_; }
    /// True once THIS fit has succeeded at least once.
    bool  valid() const { return valid_; }

private:
    void solve() {
        // Normal equations for [c0, c1, alpha] with regressors [1, k, −u].
        double M[3][3] = {{0}}, b[3] = {0};
        for (int i = 0; i < kWin; ++i) {
            const double x[3] = {1.0, k_[i], -double(U_[i])};
            const double y = dE_[i];
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) M[r][c] += x[r] * x[c];
                b[r] += x[r] * y;
            }
        }

        // ── Excitation guard: the same test, for the same reason ─────────────
        // u must SPREAD, and it must not spread along k. A steady hold gives a
        // nearly constant u (no spread); a slow ramp of u gives a spread that is
        // collinear with k, which c1·k then absorbs — leaving the alpha column
        // explaining nothing while the solve still returns some number inside the
        // clamp. Both cases are refused, and refusing costs nothing.
        double uMean = 0.0, uVar = 0.0;
        for (int i = 0; i < kWin; ++i) uMean += U_[i];
        uMean /= kWin;
        for (int i = 0; i < kWin; ++i) uVar += (U_[i] - uMean) * (U_[i] - uMean);
        uVar /= kWin;
        if (uVar < 1.0) return;

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
        if (kVar < 20.0) return;
        if (std::fabs(ukCov / std::sqrt(uVar * kVar)) > 0.97) return;

        for (int i = 0; i < 3; ++i) {
            int pv = i;
            for (int r = i + 1; r < 3; ++r) if (std::fabs(M[r][i]) > std::fabs(M[pv][i])) pv = r;
            if (std::fabs(M[pv][i]) < 1e-9) return;    // singular: keep the old value
            for (int c = 0; c < 3; ++c) std::swap(M[i][c], M[pv][c]);
            std::swap(b[i], b[pv]);
            for (int r = i + 1; r < 3; ++r) {
                const double f = M[r][i] / M[i][i];
                for (int c = i; c < 3; ++c) M[r][c] -= f * M[i][c];
                b[r] -= f * b[i];
            }
        }
        double x[3] = {0};
        for (int i = 2; i >= 0; --i) {
            double s = b[i];
            for (int j = i + 1; j < 3; ++j) s -= M[i][j] * x[j];
            x[i] = s / M[i][i];
        }

        // The coefficient on the regressor −u IS alpha, because Δe = w − alpha·u.
        // The clamp refuses absurd fits for the same reason the other estimator
        // does: at alpha_hat = 0.05 against a true 0.5 the echo coefficient is
        // s = −7.2 and the self-copy stops being damping.
        const double a = x[2];
        if (!(a > kAlphaMin && a < kAlphaMax)) return;

        // Same bias, same blend, same reason: the fit is refitted every
        // kAlphaSolveEvery steps, so adopting each one wholesale would walk the
        // loop gain around for nothing.
        const float want = std::clamp(kAlphaBias * static_cast<float>(a),
                                      kAlphaMin, kAlphaMax);
        alpha_ += kAlphaStep * (want - alpha_);
        valid_ = true;
    }

    float dE_[kWin] = {0.0f};
    float U_[kWin]  = {0.0f};
    float k_[kWin]  = {0.0f};
    int   head_ = 0, count_ = 0, tick_ = 0;
    bool  havePrev_ = false;
    float prevE_ = 0.0f;
    float alpha_ = kAlphaHatSeed;
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
    // kf        : feed-forward STRENGTH, 0…kFfStrengthMax (0.50 since round 15).
    //             Not the plant gain, and not a per-game calibration: the
    //             reconstruction constant is derived on-line (see `alphaEst`).
    //             0 turns F off exactly — which is a perfectly good setting now
    //             that the integral carries the DC command — and 0.05 … 0.20 is
    //             a noise-free trim on top of it. See the round-15 section.
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

    /// The reconstruction constant, from the estimator.
    ///
    /// ROUND 19b: adopted AS A RATE, not assigned. The old comment claimed this
    /// "only ever changes the residual self-term, never the loop's gain" — that
    /// was written when kf shipped at 0.05 and F carried 5 % of the command, and
    /// it stopped being true the moment round 19 let kf reach 0.80. alpha_hat is
    /// the denominator of the feed-forward gain (kf/alpha_hat) AND of the
    /// measured carrier (ffVel/alpha_hat), so a step in it is a step in the
    /// crosshair's velocity, and the estimator produced one every
    /// kAlphaSolveEvery steps. See kAlphaUseRate.
    ///
    /// The first value is adopted whole: at start-up the seed is a guess and
    /// there is nothing to step away from, and the kf guard is holding the
    /// feed-forward down anyway.
    void setAlphaHat(float a) {
        const float want = std::clamp(a, kAlphaMin, kAlphaMax);
        if (!alphaHatInit_) { alphaHat = want; alphaHatInit_ = true; return; }
        alphaHat += kAlphaUseRate * (want - alphaHat);
    }

    /// The engaged target's box width in screen px — the UNITS of the proximity
    /// weight's threshold (kGateFrac × this). Passing <= 0 disables the schedule
    /// and leaves P and I at full gain.
    void setTargetBoxPx(float px) { boxPx = (px > 0.0f && std::isfinite(px)) ? px : 0.0f; }

    // ── Core update ───────────────────────────────────────────────────────────
    //
    // error         : current position error (target - crosshair), px
    // dt            : control step, seconds
    // frozen        : true when this axis is inside its deadzone. The band gates
    //                 the integral's RATE, never its CONTRIBUTION: the
    //                 proportional and derivative terms stop driving (they are
    //                 what amplify the detector's jitter — the band's real job),
    //                 the integrator stops accumulating, and the charge it holds
    //                 is bled toward what the reconstruction measures the target
    //                 to need — MINUS the part the feed-forward is already
    //                 supplying this step (round 19b: trimWant = carry − ff,
    //                 carry = ffVel/alpha_hat). At rest both terms are 0; on a
    //                 strafe the trim is left holding its own (1−kf) share of the
    //                 standing carrier and not the whole of it, which is what
    //                 stops the band from over-charging it once kf is large
    //                 enough to matter (kDzTrimBleedTauSec). It is STILL ADDED to the output at
    //                 all times, and round 15 is the reason that sentence has to
    //                 be here: it suppressed the contribution, which turned the
    //                 band into a bang-bang in a per-step displacement term and
    //                 read on the device as "乱甩乱晃". The feed-forward is
    //                 added as always.
    //
    //                 NOTE (round 17): `frozen` is NOT the only route into that
    //                 bleed any more, and it never could be — see kStopHoldSteps.
    //                 A charge that oscillates holds |e| outside the band by
    //                 construction, so the band's own trigger is absent exactly
    //                 when the bleed is needed. The stop detector is the second
    //                 trigger, on the measurement rather than on the error.
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
    //
    // measurementCorrected : ROUND 20. True when the TRACKER re-associated this
    //                 step — it pulled the published centre back onto a detector
    //                 result that is already a render frame old
    //                 (kalman_tracker.cpp: predictAll runs every render frame,
    //                 matchAndUpdate only on a fresh result). On those steps de is
    //                 NOT a velocity: it is the constant-velocity extrapolation
    //                 PLUS the correction kick, and the kick is the distance the
    //                 prediction had already run — proportional to how far WE
    //                 rotated the view since the last detector result. The
    //                 tracker's own note measures the cost at 9…41 px of
    //                 published lag and vx ±22 %. Feeding that into the
    //                 reconstruction puts our own view rotation back into the
    //                 loop through BOTH agents that multiply a velocity — the
    //                 feed-forward (× kf) and the integrator (carry → trimWant,
    //                 × 1) — which is why "the faster the view moves, the worse
    //                 it shakes" and why a tiny ki behaves the same way.
    //                 On a step the tracker did NOT correct, the model advanced
    //                 the published centre by exactly its own (vx, vy), so de
    //                 there IS the Kalman velocity, clean. The reconstruction
    //                 therefore samples de only on those steps and RE-USES the
    //                 last clean sample in between, so its sample rate becomes the
    //                 detector's — the rate the quantity is actually measured at.
    //                 Default false = never gate, i.e. the pre-round-20 behaviour,
    //                 so a caller that does not know is not silently changed.
    float update(float error, float dt, bool frozen = false,
                 bool targetChanged = false, bool measurementCorrected = false) {
        if (!std::isfinite(error) || dt <= 0.0f) return 0.0f;

        if (targetChanged) {
            integral  = 0.0f;
            deriv     = 0.0f;
            lastError = error;   // not 0: zeroing it would make the derivative
                                 // read kd * error / dt for one step, which is
                                 // the lurch this branch exists to avoid.
            ramp      = 0.0f;
            ffVel     = 0.0f;
            ffVelPrev = 0.0f;   // round 22: a new enemy's speed is not this one's,
            stopDrop  = 0.0f;   //   so losing the old track is not a "stop"
            ffInhibit = true;
            nearW     = 0.0f;
            stopSteps = 0;   // a new enemy's "we have been at rest" count is not ours
            deCleanValid = false;   // the old track's clean sample is not the new one's
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
            ffVelPrev = 0.0f;   // round 22: a jump is not a deceleration
            stopDrop  = 0.0f;
            ffInhibit = true;
            integral  = 0.0f;
            deriv     = 0.0f;
            nearW     = 0.0f;
            ramp      = 0.0f;
            stopSteps = 0;   // a jump is not rest — do not let the count survive it
            deCleanValid = false;   // Δe across a jump is not a velocity either
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

        // ── Deadzone, part 1: P and D stop driving (round 15, kept) ───────────
        //
        // These two are what amplify the detector's jitter, which is the entire
        // reason a stop band exists: inside the band P <= kp x band and D is
        // bounded by the error's own rate, so at any sane gain the step they make
        // at the boundary is well under a pixel per step. Switching them off is
        // therefore free, and it is what makes the centre quiet.
        //
        // The integral is a different animal — it is a per-step DISPLACEMENT, it
        // can legitimately hold 50 px/step, and a 50 px/step step in velocity is
        // what "乱甩乱晃" was. So it is NOT switched off here. It is handled after
        // the reconstruction below, by being bled toward the measured carrier.
        if (frozen) {
            p = 0.0f;
            d = 0.0f;
        }

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
        // The MEASUREMENT runs whether kf is zero or not (round 16); only the
        // CONTRIBUTION is scaled by kf. At kf = 0 the feed-forward adds nothing,
        // but ffVel is still the loop's only knowledge of how fast the target is
        // actually moving, and both the diagnostic readout and the deadzone's
        // bleed target (carry = ffVel/alpha_hat) are built from it. Before this
        // the whole block sat behind `if (kf > 0)`, so at the user's kf = 0 every
        // number derived from ŵ was identically zero — including the one the band
        // needed, which is why round 15's band could only bleed to zero.
        const bool velValid = (alphaHat > 0.0f) && !ffInhibit;
        float ff = 0.0f;
        // ── ROUND 20: sample de only where de IS a velocity ──────────────────
        //
        // de is the step-to-step change of the tracker's PUBLISHED centre, and
        // that centre follows two different laws depending on the step
        // (kalman_tracker.cpp:503-521): on a step the detector produced nothing
        // new, predictAll advanced it by exactly (vx, vy), so de == vx and is
        // clean; on a step the tracker re-associated, matchAndUpdate pulled it
        // back onto a measurement that is already a render frame old, so
        // de == vx + kick — and the kick is the distance the prediction had
        // already run, i.e. proportional to OUR OWN view rotation over the
        // detector period. That is the "the faster the view moves, the worse it
        // shakes" mechanism, and it is why the corruption scales with |u|: it
        // re-enters the loop through ff (× kf) AND through carry → trimWant →
        // the integrator (× 1), so a tiny ki rings just as well as a large one.
        //
        // So the sample is taken only from an UNCORRECTED step, and it is HELD
        // across corrected ones rather than dropped, so the filter's bandwidth
        // stays the detector's own and never collapses if the detector happens to
        // run at the render rate.
        if (velValid) {
            if (!measurementCorrected) {
                deClean      = de;
                deCleanValid = true;
                reconHold    = 0;
            } else if (++reconHold >= kReconHoldMaxSteps) {
                // Watchdog: nothing but corrected steps in a row means the render
                // loop has fallen to the detector's rate. Reusing the held sample
                // there would freeze the velocity for ever — take this one and
                // accept its innovation, which at that rate is only a single
                // render frame of drift. See kReconHoldMaxSteps.
                deClean      = de;
                deCleanValid = true;
                reconHold    = 0;
            }
        }
        // Round 22: the fractional drop of the reconstructed speed is the stop
        // detector's input. It is MEASURED inside the block below (without a fresh
        // sample it has no meaning) and DECAYED outside it (so a stretch with no
        // valid sample cannot leave the flag stuck on).
        float dropNow = 0.0f;
        if (velValid && deCleanValid) {
            const float meas = std::clamp(deClean + alphaHat * uHist[uHead],
                                          -kFfVelMaxPx, kFfVelMaxPx);
            const float tau  = (std::fabs(meas) < std::fabs(ffVel))
                                   ? kFfTauFastSec : kFfTauSec;
            const float af   = dt / (dt + tau);
            ffVelPrev = ffVel;      // the DENOMINATOR, captured before the filter moves
            ffVel += af * (meas - ffVel);
            {
                const float aPrev = std::fabs(ffVelPrev);
                const float aNow  = std::fabs(ffVel);
                if (aPrev > kStopDropVelMinPx && aPrev > aNow)
                    dropNow = (aPrev - aNow) / aPrev;
            }
            if (kf > 0.0f) {
                const float gain = std::min(kf / alphaHat, kFfGainMax);
                ff = std::clamp(gain * ffVel, -kFfLimitPx, kFfLimitPx);
                // Deadband, pid (1).cpp:55 (`|ki_raw| > 0.5` else 0): without it a
                // small reconstruction wanders around zero at rest and is
                // integrated by nothing but the detector.
                if (std::fabs(ff) < kFfDeadbandPx) ff = 0.0f;
            }
        }
        // Round 22: instant attack, slow release. The flag must catch the first
        // frame of the collapse, but one soft step must not clear it and restart
        // the release from scratch. While the target is moving dropNow == 0 and
        // this decays to zero on its own, which is what leaves the follow untouched.
        stopDrop = std::max(dropNow, stopDrop - dt / kStopDropReleaseSec);
        if (stopDrop < 0.0f) stopDrop = 0.0f;
        ffInhibit = false;

        // ── The measured carrier, computed whether the band is open or shut ───
        //
        // ffVel/alpha_hat is in the trim's own units — finger px per step — which
        // is what makes it usable as a target at all; the 0.5 px deadband keeps a
        // jitter-driven ŵ from leaving a permanent sub-pixel charge at rest.
        //
        // It is hoisted out of the `frozen` branch (round 17) because TWO
        // mechanisms now read it: the stop band's bleed, and the stop detector
        // that triggers when the target comes to rest. It is a MEASUREMENT, so
        // there was never a reason to read it only inside the band.
        const bool  haveCarry = (alphaHat > 0.0f) && velValid;
        float carry = 0.0f;
        if (haveCarry) {
            carry = std::clamp(ffVel / alphaHat, -trimLimit, trimLimit);
            if (std::fabs(carry) < kFfDeadbandPx) carry = 0.0f;
        }

        // ── ROUND 19b: the trim's TARGET is the carrier's TRIM SHARE ──────────
        //
        // carry = ffVel/alpha_hat is the WHOLE carrier — the total command the
        // target's motion demands — not the part the integral is supposed to
        // supply. The two are only the same number while kf is ~0.05 and F is
        // carrying 5 % of it. At kf = 0.50 the split is half and half, so a trim
        // bled toward `carry` is bled to TWICE its share: the sum becomes
        // 1.5·u_ss, the crosshair is pushed past the target, the error leaves the
        // band, P and D switch back on and drag it in, the band re-closes and the
        // charge is pumped again — a limit cycle, on the axis carrying the
        // carrier, and only while the crosshair is close enough to be frozen.
        // That is the "有时候左右晃" round 19 traded the violent shake for.
        //
        // Written as a CONSERVATION and not as a formula: the trim wants what is
        // left after the feed-forward has supplied its own part this step. It is
        // therefore right at every kf, right while ff is clamped or deadbanded,
        // and at kf = 0 it is `carry` verbatim — which is the behaviour the whole
        // deadzone was tuned against, so this cannot regress the old settings.
        float trimWant = integral;                 // no measurement: hold
        // Clamped to the leash: carry and ff each carry their own limit, so their
        // difference can exceed it, and the old target could not — a charge that
        // is allowed to park above trimLimit is a charge the integrator will
        // spend later, which is the exact fault back-calculation exists to stop.
        if (haveCarry) {
            if (stopDrop > kStopDropThreshold) {
                // ROUND 22: the target has stopped. The charge standing in the
                // integrator is no longer what its motion demands — it is now pure
                // poison — so the target becomes ZERO and the two branches below
                // discharge it. Deliberately NOT `carry`: at the instant of the
                // stop, carry is built from meas = (alpha_hat − alpha)·u, which at
                // rho < 1 is the old carrier NEGATED and amplified (≈ −2·u_ss).
                // Bleeding toward that is charging the integrator in REVERSE, not
                // releasing it, and it is what turned round 19's overshoot into a
                // standing oscillation. See kStopDropThreshold.
                trimWant = 0.0f;
            } else {
                const float want = std::clamp(carry - ff, -trimLimit, trimLimit);
                // ROUND 22: a measurement is believed only while it agrees with the
                // charge it is asking about. Same sign is the steady strafe, where
                // carry == u_ss and this is round 16's own schedule kept intact.
                // Opposite sign is the reconstruction quoting our own output back
                // at us — following it there is exactly how the loop flips sign.
                if (want * integral >= 0.0f) trimWant = want;
            }
        }

        // ── Deadzone, part 2: the trim bleeds toward the MEASURED CARRIER ─────
        //
        // The band stops the accumulation (the block below is skipped while
        // frozen) and pulls the charge toward what the target is measured to need
        // instead of toward zero. carry = ffVel/alpha_hat is in the trim's own
        // units — finger px per step — which is what makes it usable as a target
        // at all; the 0.5 px deadband keeps a jitter-driven ŵ from leaving a
        // permanent sub-pixel charge at rest.
        //
        // Placement matters twice. It must run AFTER the reconstruction, because
        // ffVel is the filter of the step just observed. And it must not be a
        // separate term in the sum below — iTerm simply FOLLOWS the state, so the
        // band changes the integrator's RATE and never its CONTRIBUTION. That
        // sentence is the whole of round 16: round 15 removed the contribution,
        // and at kf = 0 that made the band zero the axis outright.
        float iTerm = integral;
        if (frozen) {
            const float want  = trimWant;   // the trim's own share of the carrier
            const float bleed = std::min(1.0f, dt / kDzTrimBleedTauSec);
            integral += (want - integral) * bleed;
            if (std::fabs(integral) < 0.01f) integral = 0.0f;
            iTerm = integral;
        } else if (haveCarry) {
            // ── Stop detector (round 22): a DIMENSIONLESS trigger ─────────────
            //
            // Round 17 asked the MEASUREMENT whether the charge was still
            // justified, via |trimWant| — and that test could never fire (see
            // kStopDropThreshold for the arithmetic). What replaces it is the
            // fractional collapse of |ffVel|: the physical definition of "the
            // target stopped", and a ratio, so there is no alpha_hat in it to be
            // wrong. pid_chirs.cpp:632 reaches for the same quantity.
            //
            // This is the state in which a standing charge is pure poison: a
            // per-step displacement with no restoring force, which the stop band
            // cannot reach because the oscillation that charge produces is what
            // holds |e| OUTSIDE the band. The band and this branch cover the two
            // halves of that fault — the band handles the charge that has wandered
            // inside it, this one handles the charge that cannot.
            //
            // The bleed is toward trimWant, which this trigger has just set to
            // ZERO, so it is a discharge and not a reversal. The value still enters
            // the sum on every step — it just decays at kStopBleedTauSec — and the
            // accumulation below still runs against it, so a target that is only
            // slowing rather than stopped re-charges almost immediately.
            const bool restNow = (stopDrop > kStopDropThreshold);
            if (restNow) ++stopSteps;
            else         stopSteps = 0;
            if (stopSteps >= kStopHoldSteps && std::fabs(integral) > 0.01f) {
                const float bleed = std::min(1.0f, dt / kStopBleedTauSec);
                integral += (trimWant - integral) * bleed;
                if (std::fabs(integral) < 0.01f) integral = 0.0f;
                iTerm = integral;
                // ROUND 22: and the reconstruction goes with it. ffVel is what fed
                // the charge in the first place, and until now nothing ever cleared
                // it — the stop path released the integrator and left the velocity
                // estimate standing, which treated half the fault. Same rate for
                // both, so neither is left behind to keep the axis awake.
                ffVel -= ffVel * bleed;
            }
        }

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
        const float uWant = p + iTerm + d + ff;
        const bool pushHi = uWant >=  outLimit && error > 0.0f;
        const bool pushLo = uWant <= -outLimit && error < 0.0f;
        if (!frozen && !pushHi && !pushLo) {
            const bool opposing = (integral * error) < 0.0f;
            if (opposing) {
                // ROUND 22: the fast release is CLAMPED AT ZERO. Written as
                // `integral += kIntReleaseGain·ki·e·dt` — the old form — a reversed
                // error pushed the charge THROUGH zero and built it up in the
                // opposite direction. That is not releasing an overshoot, it is
                // re-charging the axis backwards, and it is the engine of the ring:
                // the charge swings + → − → + and the crosshair with it, because
                // releasing only makes the error reverse again. Here |integral| can
                // only be REDUCED; once it reaches zero the ordinary accumulation
                // below takes over and the axis may legitimately reverse. Same speed
                // as before, minus the reversal.
                const float release = kIntReleaseGain * ki * std::fabs(error) * dt;
                if (release > 0.0f) {
                    const float mag = std::fabs(integral) - release;
                    integral = (mag > 0.0f) ? std::copysign(mag, integral) : 0.0f;
                }
            } else {
                integral += ki * error * dt;
            }
            integral  = std::clamp(integral, -trimLimit, trimLimit);
            iTerm     = integral;   // keep the sum below in step with the state
        }

        float u = softLimit(p + iTerm + d + ff, outLimit);
        // Give back exactly what the limiter refused to deliver, so the trim
        // tracks the command that was SENT rather than the one that was asked for.
        if (!frozen && std::fabs(p + iTerm + d + ff) > outLimit) {
            integral = std::clamp(integral + (u - (p + iTerm + d + ff)),
                                  -trimLimit, trimLimit);
            iTerm    = integral;
            u = softLimit(p + iTerm + d + ff, outLimit);
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
        ffVelPrev = 0.0f;
        stopDrop  = 0.0f;
        ffInhibit = false;
        deClean      = 0.0f;
        deCleanValid = false;
        reconHold    = 0;
        uHead     = 0;
        nearW     = 0.0f;
        stopSteps = 0;
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
    /// The carrier the reconstruction says the target needs, in finger px/step —
    /// in the TRIM's own units, and the value the deadzone bleeds the trim
    /// toward. Read them together: |trim| far above |carry| on a moving target
    /// means the trim is holding more than the measurement asks for, and at rest
    /// carry should read 0 and trim should follow it there. This works at kf = 0
    /// (round 16): it is a measurement, not a contribution.
    float carryPx()        const { return alphaHat > 0.0f ? ffVel / alphaHat : 0.0f; }
    /// The proximity weight in force, 0…1 (was the gate's boolean).
    float nearWValue()     const { return nearW; }
    /// Kept for the log: "on target" now means the schedule is mostly open.
    bool  settledNow()     const { return nearW > kNearFloor; }

    /// True while the STOP DETECTOR has the target held at rest (round 17) and
    /// the trim is consequently being bled toward the measured carrier — which is
    /// zero at rest. Read it against `trim` and `carryPx`, and read all three
    /// together: engaged with trim decaying toward carry is the mechanism working;
    /// engaged with trim NOT decaying means the accumulation is outrunning the
    /// bleed; not engaged while the aim sits still means carry is not reading ~0,
    /// i.e. the reconstruction is being fed a velocity that is not there (check
    /// alpha_hat against `a=` on the log line before touching ki).
    bool  stoppedNow()     const { return stopSteps >= kStopHoldSteps; }

private:
    float kp        = 0.10f;
    float ki        = 0.5f;
    float kd        = 0.20f;
    float outSmooth = 1.0f;
    /// Round 15: matches the page's default. It used to be 1.0 — the round-13
    /// fly-off value, kept only because nothing set it before the first frame.
    float kf        = 0.05f;
    float alphaHat  = kAlphaHatSeed;   // NEVER 1.0 by default — see round 13
    /// False until the first value has been adopted whole — see setAlphaHat().
    /// Deliberately NOT cleared by reset(): the plant gain belongs to the game,
    /// so a fresh engagement must not make the controller crawl back to a value
    /// it had already learned.
    bool  alphaHatInit_ = false;

    // ── State ─────────────────────────────────────────────────────────────────
    float integral  = 0.0f;
    float lastError = 0.0f;
    float outPrev   = 0.0f;
    float deriv     = 0.0f;   // filtered one-step change of the error
    float ramp      = 0.0f;   // soft-start ramp, 0..1
    float ffVel     = 0.0f;   // filtered ŵ, the target's own screen velocity
    bool  ffInhibit = false;  // one step of silence after a jump or a switch
    // Round 22: the stop detector's two new states. ffVelPrev is the previous
    // step's filtered speed — the DENOMINATOR of the fractional drop, so it has
    // to be captured before the filter is advanced. stopDrop is that ratio after
    // its short latch: 0 while the target is moving, ~1 the moment it stops.
    float ffVelPrev = 0.0f;
    float stopDrop  = 0.0f;
    /// ROUND 20. The last `de` that came off a step the tracker did NOT correct,
    /// i.e. a clean sample of its own velocity state. See the note on the gated
    /// reconstruction in update(): on a corrected step `de` carries the Kalman
    /// innovation, which is proportional to OUR OWN view rotation.
    float deClean      = 0.0f;
    bool  deCleanValid = false;
    /// Consecutive corrected steps since the last clean sample — the watchdog
    /// that stops the hold from becoming a freeze. See kReconHoldMaxSteps.
    int   reconHold    = 0;
    float outLimit  = kOutLimitPx;
    float trimLimit = kTrimLimitPx;

    // Proximity weight (the reference's gain schedule)
    float boxPx     = 0.0f;   // engaged target's box width; 0 = schedule off
    float nearW     = 0.0f;   // 0…1, rate-limited; P and I follow it

    // Stop detector (round 22): consecutive steps the drop ratio said "stopped".
    // Drives the trim bleed that kills the shake a big ki leaves behind when the
    // target stops — the one case the deadzone's own |e| <= dz trigger cannot
    // reach, because the oscillation it was meant to kill is what keeps |e| out
    // of the band. See kStopHoldSteps.
    int   stopSteps = 0;

    float uHist[kFfDelaySteps] = {0.0f};  // our own commands, for u(k−L)
    int   uHead    = 0;                   // index of the OLDEST entry
};

}  // namespace tracking
}  // namespace aimbotng
