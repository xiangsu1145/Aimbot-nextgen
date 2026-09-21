#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第十二轮代码改动：kf 量程 0.20→2.0（默认 1.00）、前馈脱离门控、注释同步。

同一文件多处替换在这里是串行执行的（Edit 工具同文件多次会互相覆盖）。
"""
import io
import sys

EDITS = []


def E(path, old, new, tag):
    EDITS.append((path, old, new, tag))


CTL = 'app/src/main/cpp/tracking/pid_controller.h'
SEC_H = 'app/src/main/cpp/ui/gui/sections/aim_section.h'
SEC_CPP = 'app/src/main/cpp/ui/gui/sections/aim_section.cpp'
CFG = 'app/src/main/cpp/config/config_manager.cpp'

# ── 1. pid_controller.h ─────────────────────────────────────────────────────

E(CTL, """//  and its size is scaled by kf, which is why a small kf is now genuinely the
//  conservative choice — the opposite of round 10, where a small kf was the
//  worst choice available. Measured over alpha = 0.5…2.5 with alpha_hat off by
//  0.4×…2.0× and kf = 0.20 (表8): lag −5 px, sway 5…11 px, post-stop overshoot
//  3…6 px, in every cell. kf = 1.0 needs a good alpha_hat; kf ≤ 0.5 does not.
//  That robustness is the reason the slider stops at 0.20.
""", """//  and its size is scaled by kf, which is why a small kf is the conservative
//  choice — the opposite of round 10. Two things in the paragraph above were
//  CORRECTED by round 12 and must not be trusted: the bench that produced
//  "kf ≤ 0.5 is the robust band, so the slider stops at 0.20" never tested an
//  alpha below 0.5, and 0.20 is nowhere near the correct value — see the
//  round-12 section below. Also note that "a small kf is safe" is a statement
//  about STABILITY, not about tracking: a small kf is safe and useless.
""", "ctl: 表8 tail")

E(CTL, """//  ── THE GATE (ported from the reference PID the user pointed at) ────────────
""", """//  ── ROUND 12: THE 0.20 CEILING ON kf WAS THE BUG ────────────────────────────
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
""", "ctl: round-12 header")

E(CTL, """//  feed-forward and the integral, not the proportional gain, so the approach is
//  still driven at full authority.""", """//  the INTEGRAL, not the proportional gain, so the approach is still driven at
//  full authority. (Round 12 took the feed-forward out of its jurisdiction — see
//  the round-12 section above.)""", "ctl: gate-scope tail")

E(CTL, """// kFfStrengthMax: the top of the kf slider. 0.20 is where the residual self-term
// stays small enough that alpha_hat barely matters; above it the loop starts to
// depend on the estimate being right (表8).
constexpr float kFfTauSec      = 0.033f;
constexpr int   kFfDelaySteps  = 5;
constexpr float kFfStrengthMax = 0.20f;""", """// kFfStrengthMax: the top of the kf slider. 2.0 — NOT 0.20. Round 11 had 0.20
// and that ceiling was the bug the user was feeling: from the round-12 section,
// the feed-forward's carrier fraction is ≈ 0.8·kf, so 0.20 caps it at 16 % of
// the DC command the loop needs, no matter what the game's sensitivity is. The
// CORRECT value is ~1.0 for every game (kf = alpha_hat/alpha, and alpha_hat is
// estimated on-line), so the range has to straddle 1.0 comfortably on both
// sides. 2.0 is headroom, not a target.
constexpr float kFfTauSec      = 0.033f;
constexpr int   kFfDelaySteps  = 5;
constexpr float kFfStrengthMax = 2.0f;""", "ctl: kFfStrengthMax")

E(CTL, """    // kf        : feed-forward STRENGTH, 0…kFfStrengthMax. It is no longer the
    //             plant gain and no longer needs to be calibrated against one;
    //             0 turns F off, 0.20 is full strength. See the header.""",
  """    // kf        : feed-forward STRENGTH, 0…kFfStrengthMax. Not the plant gain,
    //             and no longer a per-game calibration: the correct value is the
    //             CONSTANT ≈1.0 (kf = alpha_hat/alpha, and alpha_hat is estimated
    //             on-line), 0 turns F off, and 2.0 is headroom. See the header.""",
  "ctl: setGains doc kf")

E(CTL, """        // ── The gate ──────────────────────────────────────────────────────────
        // ── See the header: transients are the one place a velocity
        // feed-forward is worthless and an integrator can only do harm. The
        // threshold is in TARGET WIDTHS, so it means the same thing at any range,
        // and the integral is ZEROED (not frozen) on the far branch.""",
  """        // ── The gate: the INTEGRAL's transient guard ─────────────────────────
        // See the header: an integrator can only do harm in a transient, and that
        // is all this now governs — round 12 removed the feed-forward from its
        // jurisdiction (see the FF branch below), because gating a MEASUREMENT
        // protected nothing and latched the loop out of low-alpha tracking. The
        // threshold is in TARGET WIDTHS, so it means the same thing at any range,
        // and the integral is ZEROED (not frozen) on the far branch.""",
  "ctl: gate comment")

E(CTL, """        float ff = 0.0f;
        if (kf > 0.0f && alphaHat > 0.0f && allow && !ffInhibit) {""",
  """        //
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
        if (kf > 0.0f && alphaHat > 0.0f && !ffInhibit) {""",
  "ctl: FF branch ungated")

E(CTL, """    /// `ffValue()` is THE number to watch on a moving target: it should settle at
    /// about kf × ΔT/alpha and HOLD there. Near zero while the error trails the
    /// target means F is not running (kf = 0, the gate is shut, or every frame
    /// reports targetChanged).""",
  """    /// `ffValue()` is THE number to watch on a moving target: at kf ≈ 1.0 it
    /// should settle at about ΔT/alpha — the WHOLE DC carrier — and hold there.
    /// Near zero while the error trails the target means F is not running (kf = 0
    /// or every frame reports targetChanged). Before round 12 it also settled low
    /// whenever the gate was shut, which at low alpha was most of the time.""",
  "ctl: ffValue doc")

E(CTL, """    float kf        = 0.20f;""", """    float kf        = 1.0f;""", "ctl: default kf")

# ── 2. aim_section.h ────────────────────────────────────────────────────────

E(SEC_H, """    /// NOW. The reconstruction constant is derived on-line (AlphaEstimator) and
    /// kf is only a strength, 0…0.20:
    ///
    ///     ff = (kf/alpha_hat)·LPF( Δe + alpha_hat·u(k−L) )
    ///
    /// Residual self-term = kf·(1 − alpha/alpha_hat)·u(k−L), so a small kf is now
    /// genuinely the safe choice (the opposite of round 10). Measured across
    /// alpha 0.5…2.5 with alpha_hat off by 0.4×…2.0×: at kf = 0.20 the lag is
    /// −5 px, the sway 5–11 px and the post-stop overshoot 3–6 px in every cell
    /// (scripts/aim_selftrap_bench.py 表8). That robustness is why the range
    /// stops at 0.20.
    ///
    /// HOW TO SET IT. Watch the error while the target strafes at a constant
    /// speed: one sign that persists = the crosshair trails = raise kf. Sway on a
    /// STILL target at any kf means the problem is not kf at all — check
    /// kp × alpha_hat (shown in the log) against 0.285, because that is the
    /// delay's hard ceiling and no feed-forward setting will fix it.
    ///
    /// 0 turns the feed-forward off exactly and leaves the integral as the sole
    /// DC carrier. Safe everywhere; it trails a fast strafe.
    ///
    /// Range 0.00–0.20, step 0.01, default 0.20 (full strength). The range is
    /// sized to the robust band on purpose — see tracking/pid_controller.h.
    widgets::SliderState ffGain{0.20f, 0.0f, 0.20f, 0.01f};""",
  """    /// NOW. The reconstruction constant is derived on-line (AlphaEstimator) and
    /// kf is only a strength, 0…2.0:
    ///
    ///     ff = (kf/alpha_hat)·LPF( Δe + alpha_hat·u(k−L) )
    ///
    /// Residual self-term = kf·(1 − alpha/alpha_hat)·u(k−L), so the error in the
    /// constant is asymmetric: under-estimate = extra damping (safe), over-
    /// estimate = the z = 1 trap returns (bad). "A small kf is safe" is therefore
    /// true — and useless, which is the round-11 mistake this one fixes.
    ///
    /// THE RANGE WAS THE BUG. Expanding the formula at DC gives the feed-forward
    /// a gain on TARGET VELOCITY of kf/alpha_hat and a share of the DC carrier of
    ///
    ///     kf·alpha/alpha_hat  ≈ 0.8·kf
    ///
    /// — independent of the game. A ceiling of 0.20 could therefore never supply
    /// more than 16 % of the command the loop needs to hold a moving target, and
    /// the user's 0.08 supplied 6 %: that is why "kf 跟不上枪" was literally
    /// true, and why raising ki to compensate overshot instead (the integral is
    /// the wrong carrier — it has to LEARN the velocity, and wind-up is the
    /// price). Measured at his gains on a 600 px/s target, alpha = 0.1:
    /// kf 0.08 -> lag 663 px, 0.20 -> 645, 0.40 -> 126, 0.80 -> 36, 1.00 -> −12.
    ///
    /// HOW TO SET IT — it is a CONSTANT, not a calibration. Full carrier needs
    /// kf = alpha_hat/alpha, the reciprocal of the estimator's own 0.8 bias, so
    /// ~1.0 on every game and every device; the usable band is 0.8…1.5. Watch the
    /// error while the target strafes steadily: one persistent sign = trailing =
    /// raise it; an error that leads (crosshair ahead) = lower it. Sway on a STILL
    /// target is the OTHER knob — that is the feed-forward amplifying the
    /// tracker's noise by kf/alpha_hat (12.5× at alpha 0.1), so lower kf or raise
    /// kFfTauSec. And if the aim shakes at rest even with kf = 0, the cause is not
    /// this row at all: check kp × alpha_hat (in the log) against 0.285.
    ///
    /// 0 turns the feed-forward off exactly and leaves the integral as the sole
    /// DC carrier. Safe everywhere, but it trails a strafe at low sensitivity.
    ///
    /// Range 0.00–2.00, step 0.05, default 1.00. See tracking/pid_controller.h.
    widgets::SliderState ffGain{1.00f, 0.0f, 2.00f, 0.05f};""",
  "sec_h: kf slider doc")

E(SEC_H, """    /// `kf` is the feed-forward STRENGTH, 0…0.20 — it is NOT the plant gain any
    /// more. The reconstruction constant is derived on-line (see `alphaEst`) and
    /// pushed into both axes; round 10's demand that the user calibrate kf
    /// against their game's sensitivity is what made a small kf actively worse
    /// than zero. See the header of tracking/pid_controller.h.""",
  """    /// `kf` is the feed-forward STRENGTH, 0…2.0 — it is NOT the plant gain. The
    /// reconstruction constant is derived on-line (see `alphaEst`) and pushed
    /// into both axes, which is what makes the correct kf a CONSTANT (~1.0)
    /// instead of a per-game calibration, and what makes 1.0 rather than 0.20 the
    /// working value. Round 11 capped it at 0.20 and that cap — not the user's
    /// tuning — is why the aim could not keep up. See the header of
    /// tracking/pid_controller.h, round-12 section.""",
  "sec_h: setGains doc")

E(SEC_H, """    float ffGain = 0.20f;""", """    float ffGain = 1.00f;""", "sec_h: ffGain default")

# ── 3. aim_section.cpp ──────────────────────────────────────────────────────

E(SEC_CPP, """    // The reconstruction constant is now derived on-line (AlphaEstimator, in
    // tracking/pid_controller.h) and kf is a plain strength, so a SMALL kf is
    // genuinely the conservative choice — the reverse of round 10. Measured
    // across alpha 0.5…2.5 with the constant off by 0.4×…2.0×: at 0.20 the lag
    // is −5 px, the sway 5–11 px, the post-stop overshoot 3–6 px, in every cell.""",
  """    // The reconstruction constant is now derived on-line (AlphaEstimator, in
    // tracking/pid_controller.h) and kf is a plain strength — but the round-11
    // range of 0…0.20 was still wrong, and it was the thing the user felt. At DC
    // the feed-forward's share of the DC carrier is kf·alpha/alpha_hat ≈ 0.8·kf,
    // independent of the game, so 0.20 could never supply more than 16 % of what
    // a moving target needs and his 0.08 supplied 6 %. Measured at his gains,
    // alpha = 0.1, 600 px/s: kf 0.08 -> lag 663 px, 0.20 -> 645, 0.40 -> 126,
    // 0.80 -> 36, 1.00 -> −12. The correct value is a CONSTANT ≈1.0 (full carrier
    // needs kf = alpha_hat/alpha, and alpha_hat is estimated on-line), so this row
    // is a trim with a known setting rather than a calibration. See
    // scripts/aim_gate_lockout_bench.py 表0/表6.""",
  "sec_cpp: kf row doc")

E(SEC_CPP, """    // Four gains and the output EMA: that is the whole parameter set. There is no
    // sensitivity compensation argument any more and nothing is scaled — the
    // fifth argument is 前馈增益, which is both the feed-forward gain and the
    // reciprocal of the reconstruction constant, so it carries the plant gain
    // inside itself (kf = 1/alpha). The output ceiling and the derivative's
    // filter constant are loop constants: tracking::kOutLimitPx / kTrimLimitPx /
    // kDerivTauSec. See AimController::setGains().""",
  """    // Four gains and the output EMA: that is the whole parameter set. There is no
    // sensitivity compensation argument any more and nothing is scaled — the
    // fifth argument is kf, the feed-forward STRENGTH. It is not the plant gain:
    // the reconstruction constant is estimated on-line inside the controller, so
    // kf is a plain multiplier whose correct value is the constant ≈1.0. The
    // output ceiling and the derivative's filter constant are loop constants:
    // tracking::kOutLimitPx / kTrimLimitPx / kDerivTauSec. See
    // AimController::setGains().""",
  "sec_cpp: setGains call doc")

# ── 4. config_manager.cpp ───────────────────────────────────────────────────

E(CFG, """//        In 9 the reconstruction constant is estimated on-line (AlphaEstimator,
//        derived from the fact that our own command history is known exactly and
//        is the one regressor a target's smooth motion does not mimic) and kf is
//        a plain STRENGTH, 0…0.20, so a small kf is the conservative choice
//        rather than the worst one. The old default (3) would clamp to 0.20 and
//        behave, but re-seeding makes the intent explicit: default 0.20.
//        Measured: scripts/aim_selftrap_bench.py 表2/表8.
constexpr int kCtlSchema = 9;""",
  """//        In 9 the reconstruction constant is estimated on-line (AlphaEstimator,
//        derived from the fact that our own command history is known exactly and
//        is the one regressor a target's smooth motion does not mimic) and kf is
//        a plain STRENGTH.
//
//        In 10 that strength's RANGE changes from 0…0.20 to 0…2.0 and the default
//        from 0.20 to 1.00. The reconstruction made kf scale-free — its share of
//        the DC carrier is kf·alpha/alpha_hat ≈ 0.8·kf, the same on every game —
//        so the correct value is the CONSTANT ≈1.0 and 0.20 was simply below it:
//        that ceiling is why the user's aim could not keep up while showing no
//        overshoot. A stored 9 value (0.08…0.20) is a strength on the same scale
//        but a wrong one, and reading it back would keep the user in the failure
//        zone, so the set is re-seeded rather than migrated. See the round-12
//        section of tracking/pid_controller.h and 表6 of
//        scripts/aim_gate_lockout_bench.py.
constexpr int kCtlSchema = 10;""",
  "cfg: schema 10")

E(CFG, """            // 0.20 is full feed-forward strength. A stored v8 value was a
            // RECIPROCAL of the plant gain (calibrated to be 1/alpha, and the
            // user's own value was 0.05 on a game where the correct figure was
            // larger), so it cannot be carried across — the same digits would
            // now mean "5% strength". Re-seed, do not migrate.
            a.ffGain.value = 0.20f;""",
  """            // 1.00 is the working value: a full DC carrier needs
            // kf = alpha_hat/alpha (the reciprocal of the estimator's 0.8 bias).
            // A stored v8 value was a RECIPROCAL of the plant gain (the user's own
            // was 0.05) and a stored v9 value was a strength on a range that could
            // not exceed 0.20 (his was 0.08) — either way the same digits are
            // wrong here, and a small kf trails a strafe by hundreds of px. The
            // set is therefore re-seeded, not migrated.
            a.ffGain.value = 1.00f;""",
  "cfg: re-seed kf")

E(CFG, """        // kf changes MEANING with the schema (v7 scaled it by 灵敏度补偿; v8 made
        // it 1/alpha; v9 makes it a bare strength), so it is read only from a
        // same-schema file.""",
  """        // kf changes MEANING with the schema (v7 scaled it by 灵敏度补偿; v8 made
        // it 1/alpha; v9 made it a strength whose range stopped at 0.20; v10 is a
        // strength that actually reaches the working value of 1.0), so it is read
        // only from a same-schema file.""",
  "cfg: load-side comment")


def main():
    fails = 0
    for path, old, new, tag in EDITS:
        s = io.open(path, encoding='utf-8').read()
        n = s.count(old)
        if n != 1:
            print("FAIL %-28s 命中 %d 次" % (tag, n))
            fails += 1
            continue
        io.open(path, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
        print("ok   %-28s (%d B -> %d B)" % (tag, len(old), len(new)))
    print("\n失败 %d 项 / 共 %d 项" % (fails, len(EDITS)))
    return 1 if fails else 0


sys.exit(main())
