#!/usr/bin/env python3
"""Aim-loop probe #2: the two things aim_loop_sim.py cannot see.

aim_loop_sim.py is a faithful port of the controller, but its measurement channel
is IDEAL: the feed-forward is handed the TRUE delayed target velocity and the
derivative is handed the TRUE error difference. Grep it for "random" — there is
no noise source anywhere in the file.

On the device neither is true. The tracker's vx comes out of a Kalman filter fed
by jittering bounding boxes, and that estimate is piped STRAIGHT into the finger
motion by the feed-forward gain. So the two symptoms that survive every
sim-verified tuning round —

    "kd 随便拉一下就非常抖"
    "永远滞后，怎么样都过冲"

— are exactly the two the ideal model is blind to. This script adds:

  1. a NOISY velocity estimate (1-pole on a jittering bbox centre), so the FF
     and the D term see what they really see;
  2. a CAPACITY sweep: with the shipped ffGain = 1.0 and the trim ceiling of
     24 px/step, is there a target speed the loop physically cannot follow?
     (A ceiling is not a tuning parameter — no gain removes it.)

Faithful to the shipped code:
    tracking/pid_controller.h      kOutLimitPx=60  kTrimLimitPx=24  tau=25ms
    aim_section.cpp::driveAimToTarget
        move = pid + ff,  ff = clamp(vx * ffGain, +/-60)
"""
import argparse
import math
import random
from collections import deque

K_OUT = 60.0      # tracking::kOutLimitPx
K_TRIM = 24.0     # tracking::kTrimLimitPx
TAU_D = 0.025     # tracking::kDerivTauSec
HZ = 120.0


def soft_limit(v, limit):
    return v if limit <= 0.0 else limit * math.tanh(v / limit)


class PPID:
    """tracking/pid_controller.h, transcribed."""

    def __init__(self, kp, ki, kd):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.I = 0.0
        self.last = 0.0
        self.d = 0.0

    def update(self, e, dt):
        a = dt / (dt + TAU_D)
        self.d += a * (self.kd * (e - self.last) - self.d)

        raw = self.kp * e + self.I + self.d
        push_hi = raw >= K_OUT and e > 0.0
        push_lo = raw <= -K_OUT and e < 0.0
        if not push_hi and not push_lo:
            self.I += self.ki * e * dt
            self.I = max(-K_TRIM, min(K_TRIM, self.I))

        self.last = e
        return soft_limit(self.kp * e + self.I + self.d, K_OUT)


def run(kp, ki, kd, alpha, ff_gain, speed_px_s, l_act=1, l_meas=3,
        sigma_px=1.5, k_vel=0.42, steps=3600, tail=1200, seed=7,
        ff_ideal=False, noise=True):
    """Constant-velocity strafe `speed_px_s` (view px/s); watch the trailing error.

    The one number that matters: mean_e over the tail.
      mean_e ~ 0                  -> the aim travels WITH the target
      mean_e one sign and large    -> permanent lag, whatever the gains say
    """
    rng = random.Random(seed)
    dt = 1.0 / HZ
    v = speed_px_s * dt                      # view px per step

    pid = PPID(kp, ki, kd)
    act = deque([0.0] * (l_act + 1), maxlen=l_act + 1)
    err_hist = deque([0.0] * (l_meas + 1), maxlen=l_meas + 1)

    view = 0.0
    v_est = 0.0
    r_prev_meas = 0.0
    trace, cmds, trims, vests = [], [], [], []

    for n in range(steps):
        e_meas = err_hist[0]

        # ── what the tracker actually reports ────────────────────────────────
        r_true_now = v * n
        r_true_del = v * (n - l_meas)
        r_meas = r_true_del + (rng.gauss(0.0, sigma_px) if noise else 0.0)
        if n == 0:
            r_prev_meas = r_meas
        v_meas = r_meas - r_prev_meas         # one-step difference of a jittery box
        r_prev_meas = r_meas
        v_est += k_vel * (v_meas - v_est)     # Kalman-ish velocity estimate

        ff_v = v if ff_ideal else v_est       # IDEAL (old sim) vs REAL (noisy)
        ff = max(-K_OUT, min(K_OUT, ff_v * ff_gain))

        u = pid.update(e_meas, dt) + ff
        act.append(u)
        view += alpha * act[0]

        glow = r_true_now - view
        err_hist.append(glow)
        trace.append(glow)
        cmds.append(u)
        trims.append(pid.I)
        vests.append(v_est)

    w = trace[-tail:]
    c = cmds[-tail:]
    t = trims[-tail:]
    n_t = len(w)
    mean_e = sum(w) / n_t
    # "jerkiness": the AC part of consecutive finger steps. A steady strafe should
    # need a CONSTANT finger velocity; every oscillation about that is visible on
    # screen as shake.
    dc = sum(c) / n_t
    jerk = math.sqrt(sum((x - dc) ** 2 for x in c) / n_t)
    return dict(
        mean_e=round(mean_e, 1),
        rms_e=round(math.sqrt(sum(x * x for x in w) / n_t), 1),
        pp_e=round(max(w) - min(w), 1),
        trim=round(sum(t) / n_t, 1),
        trim_peak=round(max(abs(x) for x in t), 1),
        u_bar=round(dc, 1),
        jerk=round(jerk, 2),
    )


def hdr(title):
    print(f"\n=== {title} ===")


def trilemma(alpha, speed, k_vel_list):
    """The FF knob's two-sided cost, at one sensitivity and one target speed.

    Raising 速度前馈 is the ONLY way to remove a saturated trim's lag -- but the
    gain multiplies the velocity estimate's noise straight into the finger. So
    the same knob buys lag on one end and shake on the other. Show whether a
    better estimate can move that whole curve, or only slide the pair along it.
    """
    print(f"\n  alpha={alpha}  target {speed} px/s  "
          f"(need {speed / alpha:.0f} px/s of finger = {speed / alpha / HZ:.1f} px/step)")
    print(f"   {'v-est':>12} {'ff':>5} | {'mean_e':>7} {'pp_e':>7} | {'jerk':>7}")
    for k_vel in k_vel_list:
        for ff in (0.5, 1.0, 2.0, 3.33, 5.0):
            r = run(kp=0.10, ki=1.0, kd=0.0, alpha=alpha, ff_gain=ff,
                    speed_px_s=speed, k_vel=k_vel)
            tag = "k_vel=%.2f" % k_vel
            print(f"   {tag:>12} {ff:>5} | {r['mean_e']:>7} {r['pp_e']:>7}"
                  f" | {r['jerk']:>7}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ideal", action="store_true",
                    help="use the TRUE velocity for FF (what the old sim does)")
    args = ap.parse_args()

    # ---------------------------------------------------------------- capacity
    # ffGain = shipped 1.0, kp/kd = shipped defaults. Only the target speed and
    # the game's sensitivity vary. Required finger speed is v/alpha; the loop can
    # supply ffGain*v + (<=24 trim) + kp*e, and nothing more.
    hdr("CAPACITY: shipped ffGain=1.0 vs the same loop with ffGain=1/alpha")
    print("   (constant strafe; mean_e = the permanent trailing error in view px)")
    print(f"   {'alpha':>5} {'speed':>7} {'need':>6} {'ff=1.0 mean_e':>14}"
          f" {'pp':>6} {'trim':>6} | {'ff=1/a mean_e':>13} {'pp':>6} {'trim':>6}")
    for alpha in (0.2, 0.3, 0.5, 1.0):
        for speed in (600, 1200, 1800, 2400):
            need = round(speed / alpha)
            a = run(kp=0.10, ki=1.0, kd=0.35, alpha=alpha, ff_gain=1.0,
                    speed_px_s=speed)
            b = run(kp=0.10, ki=1.0, kd=0.35, alpha=alpha, ff_gain=1.0 / alpha,
                    speed_px_s=speed)
            print(f"   {alpha:>5} {speed:>7} {need:>6} {a['mean_e']:>14}"
                  f" {a['pp_e']:>6} {a['trim']:>6} | {b['mean_e']:>13}"
                  f" {b['pp_e']:>6} {b['trim']:>6}")

    hdr("TRILEMMA: the feed-forward knob buys lag at one end and shake at the other")
    trilemma(alpha=0.3, speed=1800, k_vel_list=[0.42, 0.15, 0.06])

    # ------------------------------------------------------------------- noise
    # The D term and the FF both differentiate a jittering box centre. Sweep kd
    # with a REALISTIC velocity estimate and watch the finger's AC content.
    hdr("NOISE: what kd does when the velocity estimate is not ideal")
    print("   (alpha=0.6, 1.0 Hz +/-300 strafe, bbox jitter sigma=1.5px)")
    print("   jerk = AC amplitude of the finger's per-step motion (screen shake)")
    print(f"   {'kd':>5} {'ff':>5} | {'jerk':>7} {'rms_e':>7} {'pp_e':>7}")
    for kd in (0.0, 0.15, 0.35, 0.7, 1.2):
        r = run(kp=0.10, ki=1.0, kd=kd, alpha=0.6, ff_gain=1.0, speed_px_s=0.0)
        print(f"   {kd:>5} {1.0:>5} | {r['jerk']:>7} {r['rms_e']:>7} {r['pp_e']:>7}")

    hdr("NOISE: what the feed-forward gain injects (kd fixed at 0)")
    print(f"   {'kd':>5} {'ff':>5} | {'jerk':>7} {'rms_e':>7} {'pp_e':>7}")
    for ff in (0.0, 0.5, 1.0, 2.0, 4.0):
        r = run(kp=0.10, ki=1.0, kd=0.0, alpha=0.6, ff_gain=ff, speed_px_s=0.0)
        print(f"   {0.0:>5} {ff:>5} | {r['jerk']:>7} {r['rms_e']:>7} {r['pp_e']:>7}")

    hdr("VALIDATION: the same kd sweep with the IDEAL velocity the old sim uses")
    print("   (identical code path, only the estimator replaced by ground truth)")
    print(f"   {'kd':>5} {'ff':>5} | {'jerk':>7} {'rms_e':>7} {'pp_e':>7}")
    for kd in (0.0, 0.15, 0.35, 0.7, 1.2):
        r = run(kp=0.10, ki=1.0, kd=kd, alpha=0.6, ff_gain=1.0, speed_px_s=0.0,
                ff_ideal=True, noise=False)
        print(f"   {kd:>5} {1.0:>5} | {r['jerk']:>7} {r['rms_e']:>7} {r['pp_e']:>7}")


if __name__ == "__main__":
    main()
