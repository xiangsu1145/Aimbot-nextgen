#!/usr/bin/env python3
"""Aim-loop probe #3: verify the three fixes made this round.

Run against the SHIPPED code as of this round:

    tracking/kalman_tracker.{h,cpp}   cruise velocity (TrackedTarget::fvx)
    tracking/pid_controller.h         lock/acquire state machine
    ui/gui/sections/aim_section.cpp   sticky/live-preferred selection,
                                      position-confirmed targetChanged

Three questions, one section each:

  A. CRUISE VELOCITY. ⚠️ SUPERSEDED — READ THIS BEFORE TRUSTING SECTION A.
     The separate slow velocity estimate was built, measured, and then REMOVED:
     section A's own conclusion ("12x less shake at an identical trailing
     error") holds only for a CONSTANT strafe, which is the one motion where the
     feed-forward is already exact and the filter can only clean up its own
     ripple. Under a realistic motion suite it is a pure Bode trade with no free
     point. Section A/A2 are kept because scripts/cruise_tau_objective.py
     imports Cruise() and the module constants to make the case that killed it,
     but the shipped tracker now has ONE velocity and both the feed-forward and
     the lead read the Kalman `vx,vy` directly. See the note on TrackedTarget in
     kalman_tracker.h.

  B. LOCK/ACQUIRE. Ported from DsAi/DsAi/PID.cpp: while closing on a target the
     proportional and derivative terms are halved; once "arrived" the gains are
     full. ⚠️ The INTEGRAL is deliberately NOT rate-gated — section B2 is that
     rejection (gating it latched an 801 px error). Does the gains-only version
     reduce overshoot without costing the steady-state trim?

  C. IDENTITY. NMS in the decoder is per class, so a head/body model emits TWO
     boxes for ONE enemy. The tracker partitioned association by class, so those
     became two tracks with two ids, and the aim's nearest-target pick alternated
     between them. Count the id changes under the old rules and the new ones.

Everything is a transcription of the shipped C++, so a disagreement between
this and the device means the transcription is wrong, not the model.
"""
import argparse
import math
import random
from collections import deque

# ── Constants, mirroring the headers ─────────────────────────────────────────
K_OUT = 60.0            # tracking::kOutLimitPx
K_TRIM = 24.0           # tracking::kTrimLimitPx
TAU_D = 0.025           # tracking::kDerivTauSec
HZ = 120.0              # aim loop rate ("游戏帧数120帧, 截图120帧")

# tracking/pid_controller.h — the lock/acquire constants
NEAR_PX_MIN = 4.0
NEAR_OF_BOX = 0.06
FAR_FRAC_MIN = 1.6
FAR_FRAC_MAX = 2.5
FAR_SHARPNESS = 5.0
FAR_MID_RATIO = 0.5
ACQ_GAIN_SCALE = 0.5
STABLE_FRAMES = 5
STABLE_TOL_PX = 2.0

# tracking/kalman_tracker.cpp — the cruise filter
CRUISE_TAU = 0.100
RELEASE_TAU = 0.025
CRUISE_STOP_EPS_PX_PER_SEC = 150.0


def soft_limit(v, limit):
    return v if limit <= 0.0 else limit * math.tanh(v / limit)


class PPID:
    """tracking/pid_controller.h, transcribed (with the state machine).

    The state machine gates the GAINS only; the integral keeps the plain
    saturation rule. See the long note in the header for why the reference's
    error-magnitude rate gate had to go: with an under-scaled feed-forward the
    steady-state trailing error sits ON the far band, so a rate gate latches the
    controller into half gain with a dead integral. Reproduced in section B2.
    """

    def __init__(self, kp, ki, kd, use_lock=True):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.use_lock = use_lock
        self.I = 0.0
        self.last = 0.0
        self.d = 0.0
        self.locked = not use_lock
        self.stable = 0

    def update(self, e, dt, box_px=0.0, short_edge=1080.0):
        if self.use_lock and box_px > 1.0:
            near = max(NEAR_PX_MIN, NEAR_OF_BOX * box_px)
            ratio = min(1.0, max(0.0, box_px / short_edge)) if short_edge > 1 else 0.0
            frac = FAR_FRAC_MIN + (FAR_FRAC_MAX - FAR_FRAC_MIN) / (
                1.0 + math.exp(-FAR_SHARPNESS * (ratio - FAR_MID_RATIO)))
            far = frac * box_px
            before = abs(e)
            quiet = abs(e - self.last) < STABLE_TOL_PX
            if before < near:
                self.locked, self.stable = True, 0
            elif self.locked:
                if before >= far and not quiet:
                    self.locked, self.stable = False, 0
            elif quiet:
                self.stable += 1
                if self.stable >= STABLE_FRAMES:
                    self.locked, self.stable = True, 0
            else:
                self.stable = 0
        scale = 1.0 if self.locked else ACQ_GAIN_SCALE

        a = dt / (dt + TAU_D)
        self.d += a * (self.kd * scale * (e - self.last) - self.d)

        p = self.kp * scale * e
        raw = p + self.I + self.d
        push_hi = raw >= K_OUT and e > 0.0
        push_lo = raw <= -K_OUT and e < 0.0
        if not push_hi and not push_lo:
            self.I += self.ki * e * dt
            self.I = max(-K_TRIM, min(K_TRIM, self.I))

        self.last = e
        return soft_limit(p + self.I + self.d, K_OUT)


class PPIDRateGated(PPID):
    """The reference rule AS LITERALLY PORTED: while acquiring, the integral
    accumulates at a tenth rate and is bled away outside the far band. Kept in
    the file as the control for section B2 — it is the version this round
    rejected, and the numbers are why."""

    ACQ_KI_SCALE = 0.1
    FAR_BLEED_TAU = 0.100

    def update(self, e, dt, box_px=0.0, short_edge=1080.0):
        if self.use_lock and box_px > 1.0:
            near = max(NEAR_PX_MIN, NEAR_OF_BOX * box_px)
            ratio = min(1.0, max(0.0, box_px / short_edge)) if short_edge > 1 else 0.0
            frac = FAR_FRAC_MIN + (FAR_FRAC_MAX - FAR_FRAC_MIN) / (
                1.0 + math.exp(-FAR_SHARPNESS * (ratio - FAR_MID_RATIO)))
            far = frac * box_px
            before = abs(e)
            beyond = False
            if before >= far:
                self.locked, self.stable, beyond = False, 0, True
            elif not self.locked:
                if before < near:
                    self.locked, self.stable = True, 0
                elif abs(e - self.last) < STABLE_TOL_PX:
                    self.stable += 1
                    if self.stable >= STABLE_FRAMES:
                        self.locked, self.stable = True, 0
                else:
                    self.stable = 0
        else:
            beyond = False
        scale = 1.0 if self.locked else ACQ_GAIN_SCALE

        a = dt / (dt + TAU_D)
        self.d += a * (self.kd * scale * (e - self.last) - self.d)

        p = self.kp * scale * e
        raw = p + self.I + self.d
        push_hi = raw >= K_OUT and e > 0.0
        push_lo = raw <= -K_OUT and e < 0.0
        if beyond:
            self.I -= self.I * min(1.0, dt / self.FAR_BLEED_TAU)
        elif not push_hi and not push_lo:
            self.I += (1.0 if self.locked else self.ACQ_KI_SCALE) * self.ki * e * dt
            self.I = max(-K_TRIM, min(K_TRIM, self.I))

        self.last = e
        return soft_limit(p + self.I + self.d, K_OUT)


class Cruise:
    """TrackedTarget::fvx — the slow estimate the feed-forward consumes."""

    def __init__(self):
        self.fx = 0.0
        self.fy = 0.0
        self.valid = False

    def step(self, vx, vy, dt):
        eps = CRUISE_STOP_EPS_PX_PER_SEC * dt
        if not self.valid:
            self.fx, self.fy, self.valid = vx, vy, True
            return self.fx, self.fy
        fast = math.hypot(vx, vy)
        slow = math.hypot(self.fx, self.fy)
        dot = self.fx * vx + self.fy * vy
        reversing = dot < 0.0 and fast > 0.5 * slow and fast > eps
        stopped = slow > eps and fast < 0.25 * slow
        tau = RELEASE_TAU if (reversing or stopped) else CRUISE_TAU
        a = dt / (dt + tau)
        self.fx += a * (vx - self.fx)
        self.fy += a * (vy - self.fy)
        if math.hypot(self.fx, self.fy) < eps:
            self.fx = self.fy = 0.0
        return self.fx, self.fy


# ── A / B: the closed loop ───────────────────────────────────────────────────
def run(kp=0.10, ki=1.0, kd=0.0, alpha=0.3, ff_gain=1.0, motion="strafe",
        speed_px_s=1800.0, period_s=1.0, amp_px=200.0, sigma_px=1.5,
        k_vel=0.42, use_cruise=True, use_lock=True, box_px=100.0,
        steps=3600, tail=1200, seed=7, ff_ideal=False, init_offset_px=0.0,
        pid_class=None):
    """One enemy, one axis. Returns the numbers that describe the feel.

    motion: "strafe"   — constant velocity, so the trailing error is a
                         steady-state property of the loop, not a transient
             "triangle" — instantaneous reversals at the peaks, the overshoot
                          test ("target turns around")
             "sin"      — a smooth back-and-forth, the "丝滑" test

    `init_offset_px` puts the target that far down the axis at t=0 without any
    motion, which turns the run into a pure step response.
    """
    rng = random.Random(seed)
    dt = 1.0 / HZ
    # Per-step target position in VIEW px. Note the tracker reports the target
    # MEASUREMENT delayed by l_meas detector frames; the controller sees that.
    def tgt(n):
        t = n * dt
        if motion == "strafe":
            return init_offset_px + speed_px_s * t
        if motion == "triangle":
            half = period_s * 0.5
            return init_offset_px + speed_px_s * (t - 2 * half * int(t / half))
        if motion == "sin":
            return init_offset_px + amp_px * math.sin(2 * math.pi * t / period_s)
        raise ValueError(motion)

    def tgt_vel(n):
        t = n * dt
        if motion == "strafe":
            return speed_px_s
        if motion == "triangle":
            half = period_s * 0.5
            return speed_px_s if int(t / half) % 2 == 0 else -speed_px_s
        return amp_px * 2 * math.pi / period_s * math.cos(2 * math.pi * t / period_s)

    pid = (pid_class or PPID)(kp, ki, kd, use_lock=use_lock)
    cruise = Cruise()
    l_meas = 3                      # detector frames of latency
    act = deque([0.0] * 2, maxlen=2)
    view = 0.0
    err_hist = deque([0.0] * (l_meas + 1), maxlen=l_meas + 1)

    v_est = 0.0                     # the Kalman-ish velocity (fast, noisy)
    r_prev = None
    trace, cmds, trims, vels = [], [], [], []

    for n in range(steps):
        e_meas = err_hist[0]

        # ── the measurement the tracker sees, and its velocity estimate ──────
        r_meas = tgt(n - l_meas) + rng.gauss(0.0, sigma_px)
        if r_prev is None:
            r_prev = r_meas
        v_meas = r_meas - r_prev
        r_prev = r_meas
        v_est += k_vel * (v_meas - v_est)

        if ff_ideal:
            ff_v = tgt_vel(n - l_meas) * dt
        elif use_cruise:
            ff_v, _ = cruise.step(v_est, 0.0, dt)
        else:
            ff_v = v_est

        ff = max(-K_OUT, min(K_OUT, ff_v * ff_gain))
        u = pid.update(e_meas, dt, box_px=box_px) + ff
        act.append(u)
        view += alpha * act[0]

        glow = tgt(n) - view
        err_hist.append(glow)
        trace.append(glow)
        cmds.append(u)
        trims.append(pid.I)
        vels.append(v_est)

    w = trace[-tail:]
    c = cmds[-tail:]
    n_t = len(w)
    mean_e = sum(w) / n_t
    dc = sum(c) / n_t
    jerk = math.sqrt(sum((x - dc) ** 2 for x in c) / n_t)
    # Overshoot: how far PAST the target the error goes. For a step response the
    # error starts large and positive and should decay to zero, so overshoot is
    # the NEGATIVE excursion past zero.
    return dict(
        mean_e=round(mean_e, 1),
        rms_e=round(math.sqrt(sum(x * x for x in w) / n_t), 1),
        pp_e=round(max(w) - min(w), 1),
        min_e=round(min(w), 1),
        jerk=round(jerk, 2),
        trim=round(sum(trims[-tail:]) / n_t, 1),
    )


def hdr(t):
    print(f"\n=== {t} ===")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--alpha", type=float, default=0.3)
    ap.add_argument("--speed", type=float, default=1800.0)
    args = ap.parse_args()
    alpha, speed = args.alpha, args.speed

    # ─────────────────────────────────────────────────────────────────── A
    hdr("A. THE FEED-FORWARD'S VELOCITY: raw Kalman vx vs the cruise estimate")
    print(f"   constant strafe {speed:.0f} px/s, alpha={alpha}, "
          f"need {speed/alpha:.0f} px/s of finger; bbox jitter sigma=1.5 px")
    print("   mean_e = permanent trailing error (px)   jerk = finger shake")
    print(f"   {'ff gain':>8} | {'raw vx: e':>10} {'jerk':>7}"
          f" | {'cruise: e':>10} {'jerk':>7}")
    for ff in (1.0, 2.0, 3.33, 4.0, 6.0):
        a = run(alpha=alpha, ff_gain=ff, speed_px_s=speed, use_cruise=False)
        b = run(alpha=alpha, ff_gain=ff, speed_px_s=speed, use_cruise=True)
        print(f"   {ff:>8} | {a['mean_e']:>10} {a['jerk']:>7}"
              f" | {b['mean_e']:>10} {b['jerk']:>7}")
    print("   Read it down the gain column: at the gain that removes the lag"
          "\n   (2.0 and up) the raw estimate's shake is 12x the cruise"
          "\n   estimate's, at an IDENTICAL trailing error. That is the whole"
          "\n   point — the slow estimate costs no lag because feed-forward only"
          "\n   ever needed an average velocity.")

    # ─────────────────────────────────────────────────────────────────── A2
    hdr("A2. THE CASE THE SLOW FILTER COULD HAVE RUINED: a smooth reversal")
    print("   1 Hz sine, +/-300 px (peak 1885 px/s), alpha=0.6, ff=1/alpha")
    print("   A sine has NO velocity discontinuity, so this measures the"
          "\n   filter's own phase lag against its noise rejection, which is the"
          "\n   real trade — unlike a triangle, whose reversal swamps everything.")
    print(f"   {'estimator':>22} | {'jerk':>7} {'rms_e':>7} {'pp_e':>7}")
    for name, kw in (
        ("raw Kalman vx", dict(use_cruise=False)),
        ("cruise + release", dict(use_cruise=True)),
        ("ideal velocity", dict(ff_ideal=True)),
    ):
        r = run(kp=0.10, ki=1.0, kd=0.0, alpha=0.6, ff_gain=1.0 / 0.6,
                motion="sin", amp_px=300.0, period_s=1.0,
                box_px=100.0, **kw)
        print(f"   {name:>22} | {r['jerk']:>7} {r['rms_e']:>7} {r['pp_e']:>7}")
    print("   'ideal velocity' is the ground truth the old simulator handed the"
          "\n   feed-forward (scripts/aim_loop_sim.py has no noise source at all)."
          "\n   It is the floor this can reach, and it is what every previous"
          "\n   round of tuning was secretly being validated against.")

    # ─────────────────────────────────────────────────────────────────── B
    hdr("B. STATE MACHINE: does halving the gain while closing cut overshoot?")
    print("   stationary target 300 px off, stepped on at t=0; ki=1.0 kd=0.35")
    print("   box 100 px. overshoot = how far past the target the error goes (px)")
    print("   The gains that overshoot are the ones the instinct reaches for")
    print("   first when the aim feels slow, which is exactly why they need this.")
    print(f"   {'kp':>5} | {'no machine: over':>17} {'settle rms':>11}"
          f" | {'machine: over':>14} {'settle rms':>11} {'lock%':>6}")
    for kp in (0.10, 0.20, 0.35, 0.50):
        a = run(**keyword_approach(kp, 100.0, use_lock=False))
        b = run(**keyword_approach(kp, 100.0, use_lock=True))
        print(f"   {kp:>5} | {a['min_e']:>17} {a['rms_e']:>11}"
              f" | {b['min_e']:>14} {b['rms_e']:>11}"
              f" {approach_lock_pct(kp, 100.0):>6}")

    hdr("B2. AND THE VERSION THAT WAS REJECTED — why the rate gate had to go")
    print("   constant strafe 1800 px/s, alpha=0.3, ff=1.0, box 100 px")
    print("   (an under-scaled feed-forward, i.e. the state the user is actually")
    print("    in on a new game. farPx = 1.7*box = 170 px, and the honest")
    print("    trailing error at this gain is 160 px — ON the band.)")
    print(f"   {'rule':>34} | {'mean_e':>8} {'rms_e':>8} {'trim':>6} {'pp_e':>7}")
    for label, cls in (("reference rate gate + far bleed", PPIDRateGated),
                       ("this file (gains only)", PPID)):
        r = run(kp=0.10, ki=1.0, kd=0.35, alpha=0.3, ff_gain=1.0,
                speed_px_s=1800, box_px=100.0, pid_class=cls)
        print(f"   {label:>34} | {r['mean_e']:>8} {r['rms_e']:>8}"
              f" {r['trim']:>6} {r['pp_e']:>7}")

    hdr("B3. AND THAT THE TRIM SURVIVES IN BOTH REGIMES")
    print("   constant strafe, the case the integral exists for; alpha=0.3 ff=1.0")
    print(f"   {'speed':>7} | {'no machine: mean_e':>18} {'trim':>7}"
          f" | {'machine: mean_e':>15} {'trim':>7}")
    for sp in (600.0, 1200.0, 1800.0):
        a = run(kp=0.10, ki=1.0, kd=0.35, alpha=0.3, ff_gain=1.0,
                speed_px_s=sp, use_lock=False)
        b = run(kp=0.10, ki=1.0, kd=0.35, alpha=0.3, ff_gain=1.0,
                speed_px_s=sp, use_lock=True)
        print(f"   {sp:>7} | {a['mean_e']:>18} {a['trim']:>7}"
              f" | {b['mean_e']:>15} {b['trim']:>7}")

    # ─────────────────────────────────────────────────────────────────── C
    hdr("C. IDENTITY: one enemy, a head box and a body box, how many ids?")
    identity()


def keyword_approach(kp, box_px, use_lock):
    """A stationary target 300 px from the crosshair, stepped on at t=0."""
    return dict(kp=kp, ki=1.0, kd=0.35, alpha=1.0, ff_gain=0.0,
                motion="strafe", speed_px_s=0.0,
                init_offset_px=300.0,
                box_px=box_px, use_lock=use_lock, steps=1200, tail=400)


def approach_lock_pct(kp, box_px):
    """Fraction of the settling window the controller spends locked."""
    dt = 1.0 / HZ
    pid = PPID(kp, 1.0, 0.35, use_lock=True)
    view = 0.0
    locked = 0
    n = 0
    for i in range(1200):
        e = 300.0 - view
        u = pid.update(e, dt, box_px=box_px)
        view += u
        if i >= 800:
            locked += 1 if pid.locked else 0
            n += 1
    return f"{100.0*locked/max(1,n):.0f}%"


def identity():
    """The head/body problem, with the old and the new association rules.

    One enemy, two boxes (a 40x40 head 70 px above a 100x200 body), both above
    threshold every frame, both jittering. NMS in the decoder is per class, so
    both survive. Y is the axis that matters: the two box centres are 70 px
    apart, and the aim picks the box whose centre is nearest the crosshair.
    """
    rng = random.Random(11)
    W, H = 1080, 2340
    cx0, cy0 = W / 2, H / 2
    speed = 3.0                       # view px per frame
    sigma = 2.0
    frames = 1200

    def dets_for(n, cls_flip_p):
        cy = cy0 + speed * n
        out = []
        # body
        out.append(dict(cx=cx0 + rng.gauss(0, sigma), cy=cy + rng.gauss(0, sigma),
                        w=100, h=200, cls=1))
        # head
        out.append(dict(cx=cx0 + rng.gauss(0, sigma),
                        cy=cy - 70 + rng.gauss(0, sigma), w=40, h=40, cls=0))
        if cls_flip_p > 0:
            for d in out:
                if rng.random() < cls_flip_p:
                    d["cls"] = 1 - d["cls"]
        return out

    def simulate(per_class, predicted_gate, recycle, sticky, count_miss=5,
                 predict_hold=3, cls_flip_p=0.0, warmup=300):
        tracks = []                   # dict: id, cx, cy, w, h, vx, vy, cls, lost, lost_frames, term
        anchor = {}                   # id -> (cx, cy, w, h) last measured
        next_id = [1]
        free = []
        created = 0
        engaged = []
        picked = set()
        engaged_id = None

        def new_id():
            if recycle and free:
                return free.pop()
            i = next_id[0]
            next_id[0] += 1
            return i

        for n in range(frames):
            ds = dets_for(n, cls_flip_p)
            # predict
            for t in tracks:
                if t["lost"]:
                    t["lost_frames"] += 1
                    if t["lost_frames"] > predict_hold:
                        t["vx"] = t["vy"] = 0.0
                else:
                    t["cx"] += t["vx"]
                    t["cy"] += t["vy"]
                    t["w"] += 0.0
                    t["h"] += 0.0

            # associate
            matchedT, matchedD = set(), set()
            groups = []
            if per_class:
                for c in set([t["cls"] for t in tracks] + [d["cls"] for d in ds]):
                    groups.append(([i for i, t in enumerate(tracks) if t["cls"] == c],
                                   [j for j, d in enumerate(ds) if d["cls"] == c]))
            else:
                groups.append((list(range(len(tracks))), list(range(len(ds)))))

            for t_idx, d_idx in groups:
                while True:
                    best, bs = None, 0.15
                    for i in t_idx:
                        if i in matchedT:
                            continue
                        t = tracks[i]
                        if predicted_gate:
                            tcx, tcy, tw2, th2 = t["cx"], t["cy"], t["w"], t["h"]
                        else:
                            tcx, tcy, tw2, th2 = anchor[i]
                        scale = 0.5 * (tw2 + th2)
                        for j in d_idx:
                            if j in matchedD:
                                continue
                            d = ds[j]
                            dist = math.hypot(d["cx"] - tcx, d["cy"] - tcy)
                            if scale > 1.0 and dist > 1.5 * scale:
                                continue
                            inter_w = max(0.0, min(tcx + tw2 / 2, d["cx"] + d["w"] / 2)
                                          - max(tcx - tw2 / 2, d["cx"] - d["w"] / 2))
                            inter_h = max(0.0, min(tcy + th2 / 2, d["cy"] + d["h"] / 2)
                                          - max(tcy - th2 / 2, d["cy"] - d["h"] / 2))
                            inter = inter_w * inter_h
                            iou = inter / (tw2 * th2 + d["w"] * d["h"] - inter) \
                                if inter > 0 else 0.0
                            nd = dist / scale if scale > 1.0 else 0.0
                            s = iou + 0.3 * max(0.0, 1.0 - nd)
                            if not per_class and d["cls"] == t["cls"]:
                                s += 0.06          # kClassBonus
                            if s > bs:
                                bs, best = s, (i, j)
                    if best is None:
                        break
                    i, j = best
                    t, d = tracks[i], ds[j]
                    if predicted_gate:
                        # Kalman-ish correction toward the measurement
                        t["vx"] += 0.42 * ((d["cx"] - t["cx"]) - t["vx"]) * 1.0
                        t["vy"] += 0.42 * ((d["cy"] - t["cy"]) - t["vy"]) * 1.0
                    t["cx"] = t["cx"] + 0.82 * (d["cx"] - t["cx"])
                    t["cy"] = t["cy"] + 0.82 * (d["cy"] - t["cy"])
                    t["w"] = d["w"]
                    t["h"] = d["h"]
                    t["cls"] = d["cls"]
                    t["lost"] = False
                    t["lost_frames"] = 0
                    t["term"] = max(count_miss, predict_hold + 1)
                    anchor[i] = (d["cx"], d["cy"], d["w"], d["h"])
                    matchedT.add(i)
                    matchedD.add(j)

            # spawn / inherit
            for j in range(len(ds)):
                if j in matchedD:
                    continue
                d = ds[j]
                inh = None
                if not per_class:
                    for i in range(len(tracks)):
                        if i in matchedT or tracks[i]["lost_frames"] > 2:
                            continue
                        ax, ay, aw, ah = anchor[i]
                        scale = 0.5 * (aw + ah)
                        if scale <= 1.0:
                            continue
                        if math.hypot(d["cx"] - ax, d["cy"] - ay) > 2.5 * scale:
                            continue
                        inh = i
                        break
                if inh is not None:
                    t = tracks[inh]
                    t.update(cx=d["cx"], cy=d["cy"], w=d["w"], h=d["h"],
                             vx=0.0, vy=0.0, cls=d["cls"], lost=False,
                             lost_frames=0, term=max(count_miss, predict_hold + 1))
                    anchor[inh] = (d["cx"], d["cy"], d["w"], d["h"])
                    matchedT.add(inh)
                    matchedD.add(j)
                    continue
                tid = new_id()
                created += 1
                tracks.append(dict(id=tid, cx=d["cx"], cy=d["cy"], w=d["w"],
                                   h=d["h"], vx=0.0, vy=0.0, cls=d["cls"],
                                   lost=False, lost_frames=0,
                                   term=max(count_miss, predict_hold + 1)))
                anchor[len(tracks) - 1] = (d["cx"], d["cy"], d["w"], d["h"])
                matchedD.add(j)

            for i, t in enumerate(tracks):
                if i not in matchedT:
                    t["lost"] = True
                    t["term"] -= 1
            keep = [i for i, t in enumerate(tracks) if t["term"] > 0]
            remap = {}
            newtracks = []
            for nk, i in enumerate(keep):
                remap[i] = nk
                newtracks.append(tracks[i])
            anchor = {remap[i]: anchor[i] for i in keep}
            for i in range(len(tracks)):
                if i not in remap:
                    if recycle:
                        free.append(tracks[i]["id"])
            tracks = newtracks

            # ── the aim's pick: nearest to the crosshair ──────────────────────
            xhair_y = cy0
            cands = []
            for i, t in enumerate(tracks):
                cands.append((i, t))
            pick = None
            for want_lost in ((False, True) if sticky else (False, True)):
                best = None
                best_eff = 1e18
                for i, t in cands:
                    if t["lost"] != want_lost:
                        continue
                    d2 = (t["cx"] - cx0) ** 2 + (t["cy"] - xhair_y) ** 2
                    eff = d2 / (1.5 * 1.5) if (t["id"] == engaged_id) else d2
                    if sticky:
                        if eff < best_eff:
                            best_eff, best = eff, (i, t)
                    else:
                        if d2 < best_eff:
                            best_eff, best = d2, (i, t)
                if best is not None:
                    pick = best[1]
                    break
            if pick is not None:
                # Only count switches AFTER the first lock has settled. The
                # initial acquisition legitimately picks one of the two boxes
                # and may adjust once; what the user is complaining about is the
                # churn DURING normal play, which is the warm-up-free tail.
                if n >= warmup:
                    if engaged_id is not None and pick["id"] != engaged_id:
                        engaged.append(pick["id"])
                    picked.add(pick["id"])
                engaged_id = pick["id"]

        return dict(created=created, live=max(1, len(tracks)),
                    flips=len(engaged), distinct=len(picked))

    print(f"   {'rules':>46} {'ids minted':>11} {'distinct ids':>13}"
          f" {'id switches':>12}")
    cases = [
        ("OLD: per-class partition, predicted gate, id recycling", dict(
            per_class=True, predicted_gate=True, recycle=True, sticky=False)),
        ("  + one pool (new association)", dict(
            per_class=False, predicted_gate=True, recycle=True, sticky=False)),
        ("  + anchor gate (new)", dict(
            per_class=False, predicted_gate=False, recycle=True, sticky=False)),
        ("  + monotonic ids (new)", dict(
            per_class=False, predicted_gate=False, recycle=False, sticky=False)),
        ("  + sticky/live-preferred pick (new)", dict(
            per_class=False, predicted_gate=False, recycle=False, sticky=True)),
    ]
    for label, kw in cases:
        r = simulate(**kw)
        print(f"   {label:>46} {r['created']:>11} {r['distinct']:>13}"
              f" {r['flips']:>12}")

    print("\n   And with the class label itself flickering 10% of the time"
          "\n   (a 2-class model whose head/body confidence sits near the"
          "\n   threshold — the case the per-class partition cannot survive):")
    for label, kw in cases:
        r = simulate(cls_flip_p=0.10, **kw)
        print(f"   {label:>46} {r['created']:>11} {r['distinct']:>13}"
              f" {r['flips']:>12}")


if __name__ == "__main__":
    main()
