#!/usr/bin/env python3
"""AimbotNextgen aim-loop simulator — a faithful port of tracking/pid_controller.h.

Why this exists
---------------
"Tune it by feel" has failed four times on this project. Every symptom the user
reports (sluggish AND ringing; "kd does nothing until it shakes") is a
statement about the LOOP, and the loop has two numbers nobody knows:

    alpha    plant gain  — px of view rotation per px of finger travel
                            (the game's own sensitivity; unknown and unmodelable)
    L        loop delay  — touch dispatch + game frame + composite + display +
                            capture + inference, counted in CONTROL STEPS

So the only honest way to choose gains is to sweep alpha and L and pick the
setting that stays quiet across the whole plausible range. That is what this
script does. It is not an approximation of the shipped controller: it is the
shipped controller, transcribed line for line (softLimit, the integral clamp,
the kResyncPx guard, the outSmooth EMA, the per-second ki/kd scales).

Coordinates
-----------
    view  = camera rotation accumulated so far, in view px
    r     = the target's position in view px (its strafe)
    e     = r - view                      <- the TRUE error
    e_meas= the error as seen L steps ago <- what the controller actually gets

Usage
-----
    python aim_loop_sim.py                 # the sweep that matters
    python aim_loop_sim.py --step          # target-appears step response
    python aim_loop_sim.py --sweep-l       # where the P bound lands
"""
import argparse
import math
from collections import deque


# ── tracking/pid_controller.h, transcribed ───────────────────────────────────
def soft_limit(v, limit):
    if limit <= 0.0:
        return v
    return limit * math.tanh(v / limit)


class PPID:
    """Exactly aimbotng::tracking::PPID."""

    kResyncPx = 220.0

    def __init__(self, kp, ki, kd, out_smooth=1.0, limit_px=120.0):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.out_smooth, self.limit_px = out_smooth, limit_px
        self.integral = 0.0
        self.last_error = 0.0
        self.out_prev = 0.0

    def update(self, error, dt, freeze_trim=False):
        if not math.isfinite(error) or dt <= 0.0:
            return 0.0

        # the "this is a target switch, not motion" guard
        if abs(error - self.last_error) > self.kResyncPx:
            self.integral = 0.0
            self.last_error = error

        p = self.kp * error

        if not freeze_trim:
            self.integral += self.ki * error * dt
            self.integral = max(-self.limit_px, min(self.limit_px, self.integral))

        d = self.kd * (error - self.last_error) / dt
        d = max(-self.limit_px, min(self.limit_px, d))

        u = soft_limit(p + self.integral + d, self.limit_px)
        self.out_prev = self.out_smooth * u + (1.0 - self.out_smooth) * self.out_prev
        self.last_error = error
        return self.out_prev


# ── the loop ─────────────────────────────────────────────────────────────────
def run(kp, ki, kd, limit_px, alpha, l_act, l_meas, ff=1.0, hz=120.0,
        tgt_f=1.0, tgt_amp=300.0, mode="track", steps=2400, tail=800, seed=0.0):
    """One axis of the real loop.

    Returns dict with the numbers a diagnosis needs.
    """
    dt = 1.0 / hz

    def target(t):
        """Absolute target position in view px, at time t."""
        if mode == "track":
            return tgt_amp * math.sin(2.0 * math.pi * tgt_f * t)
        if mode in ("step", "still"):
            return 0.0
        if mode == "ramp":
            return tgt_amp * t
        raise ValueError(mode)

    pid = PPID(kp, ki, kd, 1.0, limit_px)

    act = deque([0.0] * (l_act + 1), maxlen=l_act + 1)         # control delay
    err_hist = deque([0.0] * (l_meas + 1), maxlen=l_meas + 1)  # measurement delay

    view = -tgt_amp if mode == "step" else 0.0
    trace, cmds, trims = [], [], []

    for n in range(steps):
        t = n * dt

        # What the controller has: the error as it was l_meas steps ago.
        e_meas = err_hist[0]

        # Feed-forward: the tracker's velocity estimate. The tracker only ever
        # saw the DELAYED position, so its velocity is v(t - l_meas*dt) —
        # feeding the true, present velocity here would be a free lead that the
        # hardware does not have. `ff` scales view-px/frame straight into
        # finger-px/step, which is the unit error this script exists to expose.
        if mode == "track" or mode == "ramp":
            ff_term = ff * (target(t - l_meas * dt) - target(t - (l_meas + 1) * dt))
        else:
            ff_term = 0.0

        u = pid.update(e_meas, dt)
        u_total = u + ff_term

        # Plant: finger travel -> view rotation, delayed by the actuator path.
        act.append(u_total)
        view += alpha * act[0]

        glow = target(t) - view
        err_hist.append(glow)

        trace.append(glow)
        cmds.append(u_total)
        trims.append(pid.integral)

    w = trace[-tail:]
    wc = cmds[-tail:]
    wt = trims[-tail:]
    mean_e = sum(w) / len(w)
    rms_e = math.sqrt(sum(x * x for x in w) / len(w))
    pp_e = max(w) - min(w)
    rms_u = math.sqrt(sum(x * x for x in wc) / len(wc))
    peak_u = max(abs(x) for x in wc)
    mean_trim = sum(wt) / len(wt)

    return dict(mean_e=round(mean_e, 2), rms_e=round(rms_e, 1),
                pp_e=round(pp_e, 1), rms_u=round(rms_u, 1),
                peak_u=round(peak_u, 1), trim=round(mean_trim, 1))


class PPID2:
    """The PROPOSED controller (v4). Differences from PPID above:

      * kd is PER-STEP, not per-second. The shipped `kd*(e-last)/dt` makes the
        coefficient on the step change `kd/dt` = kd*120 at 120 Hz, which is
        invisible in the units it is labelled in and doubles if the loop rate
        halves. Per-step is the frame-rate-independent form, and it is the same
        convention the P term already uses.
      * the derivative is low-pass filtered (time constant tauD), so raising kd
        no longer raises the Nyquist gain without bound. That is what makes the
        knob usable: with the raw /dt form the Nyquist gain is 2*kd/dt, so the
        value that damps is already the value that amplifies detection jitter.
      * the integral uses CONDITIONAL integration (it stops accumulating while
        the output is saturated and the error would push it further out), so a
        long lock cannot build a reserve that fires the moment the target
        reverses.
      * the output ceiling is a constant, not a slider, and the integral clamp
        rides on it.
    """

    def __init__(self, kp, ki, kd, out_limit=60.0, tau_d=0.025,
                 i_limit=None, i_leak=0.0):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.lim, self.tau_d = out_limit, tau_d
        # The integral is the ONE term that can build a reserve and then spend a
        # long time unwinding it, and that reserve is what a "large, slow sway"
        # IS. Two independent guards: a ceiling of its own (much lower than the
        # output ceiling, because the integral only ever has to supply the
        # steady-state tracking velocity, not the transient flick) and a leak,
        # so stale velocity information decays instead of being spent later.
        self.i_lim = out_limit if i_limit is None else i_limit
        self.i_leak = i_leak
        self.I = 0.0
        self.last = 0.0
        self.d = 0.0

    def update(self, e, dt, freeze=False, target_changed=False):
        if not math.isfinite(e) or dt <= 0.0:
            return 0.0
        if target_changed:              # the CALLER knows, from the track id
            self.I = 0.0
            self.d = 0.0
            self.last = e

        a = dt / (dt + self.tau_d)
        self.d += a * (self.kd * (e - self.last) - self.d)

        raw = self.kp * e + self.I + self.d
        push_hi = raw >= self.lim and e > 0.0
        push_lo = raw <= -self.lim and e < 0.0
        if not freeze and not push_hi and not push_lo:
            self.I += self.ki * e * dt
            self.I -= self.i_leak * self.I * dt
            self.I = max(-self.i_lim, min(self.i_lim, self.I))

        raw = self.kp * e + self.I + self.d
        self.last = e
        return soft_limit(raw, self.lim)


def run2(kp, ki, kd, out_limit, tau_d, alpha, l_act, l_meas, ff=0.0, hz=120.0,
         tgt_f=1.0, tgt_amp=300.0, mode="track", steps=2400, tail=800):
    dt = 1.0 / hz

    def target(t):
        if mode == "track":
            return tgt_amp * math.sin(2.0 * math.pi * tgt_f * t)
        if mode in ("step", "still"):
            return 0.0
        raise ValueError(mode)

    pid = PPID2(kp, ki, kd, out_limit, tau_d)
    act = deque([0.0] * (l_act + 1), maxlen=l_act + 1)
    err_hist = deque([0.0] * (l_meas + 1), maxlen=l_meas + 1)

    view = -tgt_amp if mode == "step" else 0.0
    trace, cmds, trims = [], [], []

    for n in range(steps):
        t = n * dt
        e_meas = err_hist[0]
        if mode == "track":
            ff_term = ff * (target(t - l_meas * dt) - target(t - (l_meas + 1) * dt))
        else:
            ff_term = 0.0

        u = pid.update(e_meas, dt) + ff_term
        act.append(u)
        view += alpha * act[0]
        glow = target(t) - view
        err_hist.append(glow)
        trace.append(glow)
        cmds.append(u)
        trims.append(pid.I)

    w, wc, wt = trace[-tail:], cmds[-tail:], trims[-tail:]
    return dict(
        mean_e=round(sum(w) / len(w), 2),
        rms_e=round(math.sqrt(sum(x * x for x in w) / len(w)), 1),
        pp_e=round(max(w) - min(w), 1),
        rms_u=round(math.sqrt(sum(x * x for x in wc) / len(wc)), 1),
        peak_u=round(max(abs(x) for x in wc), 1),
        trim=round(sum(wt) / len(wt), 1),
    )


def show(tag, res):
    print(f"    {tag:<34} rms_e={res['rms_e']:>7} pp_e={res['pp_e']:>9} "
          f"rms_u={res['rms_u']:>7} peak_u={res['peak_u']:>7} trim={res['trim']:>7}")


# ── experiments ──────────────────────────────────────────────────────────────
ALPHAS = [0.3, 0.6, 1.0, 1.6]
# l_act = frames from "we move the finger" to "the view has moved"
# l_meas = frames from "the view moved" to "the detector result reaches us"
LACTS = [1, 2]
LMEAS = [3, 5, 7]

CONFIGS = [
    ("A shipped  kp.30 ki1.0 kd.002", dict(kp=0.30, ki=1.0, kd=0.002, limit_px=120)),
    ("B user     kp.20 ki1.0 kd.002", dict(kp=0.20, ki=1.0, kd=0.002, limit_px=120)),
    ("C user     kp.20 ki1.0 kd0    ", dict(kp=0.20, ki=1.0, kd=0.0,   limit_px=120)),
    ("D p-only   kp.20 ki0   kd.002", dict(kp=0.20, ki=0.0, kd=0.002, limit_px=120)),
    ("E shipped  kp.30 ki1.0 kd.002 ff0", dict(kp=0.30, ki=1.0, kd=0.002, limit_px=120, ff=0.0)),
]


def sweep(args):
    print("\n=== TRACKING a 1 Hz +/-300 px strafe, tail RMS error ===")
    print("    (rms_e in view px; lower is better. pp_e is the visible sway.)\n")
    for l_act in LACTS:
        for l_meas in LMEAS:
            L = l_act + l_meas
            print(f"  -- actuator delay {l_act}, measurement delay {l_meas} "
                  f"(total {L} steps = {L * 1000 / 120:.0f} ms) --")
            for a in ALPHAS:
                print(f"   alpha={a}")
                for tag, kw in CONFIGS:
                    cfg = dict(kw)
                    ff = cfg.pop("ff", 1.0)
                    res = run(alpha=a, l_act=l_act, l_meas=l_meas, ff=ff,
                              mode="track", hz=args.hz, tgt_f=args.f, **cfg)
                    show(tag, res)
            print()


def step(args):
    print("\n=== STEP: a new target appears 300 px off the crosshair ===")
    print("    (how far it overshoots and how long it rings)\n")
    for a in ALPHAS:
        for l_act, l_meas in [(1, 3), (2, 5), (2, 7)]:
            print(f"  -- alpha={a}  delay {l_act + l_meas} steps --")
            for tag, kw in CONFIGS:
                cfg = dict(kw)
                ff = cfg.pop("ff", 1.0)
                res = run(alpha=a, l_act=l_act, l_meas=l_meas, ff=ff,
                          mode="step", hz=args.hz, tgt_amp=300.0, **cfg)
                show(tag, res)
            print()


def bound(args):
    print("\n=== Discrete proportional-law stability bound ===")
    print("    kp < 2*sin(pi / (2*(2L+1)))   with L = total delay in steps\n")
    print("      L    bound      L    bound")
    for L in range(1, 13):
        b = 2.0 * math.sin(math.pi / (2 * (2 * L + 1)))
        print(f"     {L:>2}   {b:5.3f}", end="")
        if L % 2 == 0:
            print()
        else:
            print("     ", end="")
    print("\n")


def grid(tag, cfgs, mode="track", alphas=None, lats=None, hz=120.0, f=1.0):
    """Print one line per (alpha, L) cell for each config, so a config that is
    quiet in EVERY cell can be picked. That robustness, not best-case speed, is
    the objective — alpha and L are both unknown."""
    alphas = alphas or [0.3, 0.6, 1.0, 1.6]
    lats = lats or [(1, 3), (2, 5), (2, 7)]
    print(f"\n=== {tag} ===")
    print(f"{'alpha':>6} {'L':>3} " + " ".join(f"{c[0]:>22}" for c in cfgs))
    for a in alphas:
        for la, lm in lats:
            row = f"{a:>6} {la + lm:>3} "
            for _, kw, runner in cfgs:
                res = runner(alpha=a, l_act=la, l_meas=lm, mode=mode,
                             hz=hz, tgt_f=f, **kw)
                row += f" {res['rms_e']:>10} {res['pp_e']:>10}"
            print(row)
    print("    (each cell is: rms_error  peak-to-peak_error, in view px)")


def proposed(args):
    shipped = dict(kp=0.20, ki=1.0, kd=0.002, limit_px=120)
    cfgs = [
        ("shipped", shipped, lambda **k: run(**k)),
        ("P2 kp.25 ki1.5 kd.35", dict(kp=0.25, ki=1.5, kd=0.35, out_limit=60, tau_d=0.025),
         lambda **k: run2(**k)),
        ("P2 kp.35 ki2.0 kd.50", dict(kp=0.35, ki=2.0, kd=0.50, out_limit=60, tau_d=0.025),
         lambda **k: run2(**k)),
        ("P2 kp.45 ki2.5 kd.70", dict(kp=0.45, ki=2.5, kd=0.70, out_limit=60, tau_d=0.025),
         lambda **k: run2(**k)),
    ]
    grid("TRACKING 1 Hz +/-300 px strafe", cfgs)

    if args.scan:
        print("\n=== kp scan (ki=2.0 kd=0.5 L=7 steps) — where does it break? ===")
        for a in [0.3, 1.0, 1.6]:
            print(f"  alpha={a}")
            for kp in [0.15, 0.25, 0.35, 0.45, 0.55, 0.7]:
                r = run2(kp=kp, ki=2.0, kd=0.5, out_limit=60, tau_d=0.025,
                         alpha=a, l_act=2, l_meas=5)
                print(f"    kp={kp:<5} rms_e={r['rms_e']:>7} pp_e={r['pp_e']:>9}")
        print("\n=== kd scan (kp=0.35 ki=2.0 L=7) — does the filter make it usable? ===")
        for a in [0.3, 1.0, 1.6]:
            print(f"  alpha={a}")
            for kd in [0.0, 0.2, 0.35, 0.5, 0.8, 1.2]:
                r = run2(kp=0.35, ki=2.0, kd=kd, out_limit=60, tau_d=0.025,
                         alpha=a, l_act=2, l_meas=5)
                print(f"    kd={kd:<5} rms_e={r['rms_e']:>7} pp_e={r['pp_e']:>9}")
        print("\n=== ki scan (kp=0.35 kd=0.5 L=7) ===")
        for a in [0.3, 1.0, 1.6]:
            print(f"  alpha={a}")
            for ki in [0.0, 0.5, 1.0, 2.0, 3.0, 4.0]:
                r = run2(kp=0.35, ki=ki, kd=0.5, out_limit=60, tau_d=0.025,
                         alpha=a, l_act=2, l_meas=5)
                print(f"    ki={ki:<5} rms_e={r['rms_e']:>7} pp_e={r['pp_e']:>9}")


def trace_step(tag, kw, alpha, l_act, l_meas, hz=120.0, steps=360, every=6):
    """Print the step-response ERROR as a sequence.

    The symptom to identify by eye is "晃两下": a decaying ring after the
    crosshair reaches the target. Numbers alone cannot separate that from a
    merely slow approach, and they are treated very differently — a ring is a
    phase-margin problem, a slow approach is a gain problem.
    """
    dt = 1.0 / hz
    if "out_limit" in kw:                       # PPID2
        pid = PPID2(kw["kp"], kw["ki"], kw["kd"], kw["out_limit"], kw["tau_d"])
    else:                                       # shipped PPID
        pid = PPID(kw["kp"], kw["ki"], kw["kd"], 1.0, kw["limit_px"])

    act = deque([0.0] * (l_act + 1), maxlen=l_act + 1)
    hist = deque([0.0] * (l_meas + 1), maxlen=l_meas + 1)
    view = -300.0
    seq = []
    for _ in range(steps):
        u = pid.update(hist[0], dt)
        act.append(u)
        view += alpha * act[0]
        e = 0.0 - view
        hist.append(e)
        seq.append(e)

    peak = max(seq)
    print(f"  {tag:<30} alpha={alpha} L={l_act + l_meas}  "
          f"peak overshoot={peak:6.0f} px")
    print("    " + " ".join(f"{seq[i]:>6.0f}" for i in range(0, steps, every)))


def steptrace(args):
    cfgs = [
        ("shipped kp.20 ki1 kd.002",   dict(kp=0.20, ki=1.0, kd=0.002, limit_px=120)),
        ("shipped kp.30 ki1 kd.002",   dict(kp=0.30, ki=1.0, kd=0.002, limit_px=120)),
        ("P2 kp.25 ki1.5 kd.35 lim60", dict(kp=0.25, ki=1.5, kd=0.35, out_limit=60,  tau_d=0.025)),
        ("P2 kp.25 ki1.5 kd.35 lim120", dict(kp=0.25, ki=1.5, kd=0.35, out_limit=120, tau_d=0.025)),
        ("P2 kp.35 ki2.0 kd.50 lim120", dict(kp=0.35, ki=2.0, kd=0.50, out_limit=120, tau_d=0.025)),
    ]
    for alpha, l_act, l_meas in [(0.3, 2, 5), (1.0, 2, 5), (1.0, 2, 7), (1.6, 2, 5)]:
        print(f"\n--- alpha={alpha}, delay {l_act}+{l_meas} steps "
              f"({(l_act + l_meas) * 8.33:.0f} ms), target 300 px away ---")
        for tag, kw in cfgs:
            trace_step(tag, kw, alpha, l_act, l_meas)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hz", type=float, default=120.0)
    ap.add_argument("--f", type=float, default=1.0, help="strafe frequency, Hz")
    ap.add_argument("--step", action="store_true")
    ap.add_argument("--bound", action="store_true")
    ap.add_argument("--sweep-l", action="store_true")
    ap.add_argument("--proposed", action="store_true")
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--steptrace", action="store_true")
    args = ap.parse_args()

    if args.steptrace:
        steptrace(args)
        return
    if args.proposed:
        proposed(args)
        return
    bound(args)
    if not args.sweep_l and not args.step:
        sweep(args)
    if args.step:
        step(args)


if __name__ == "__main__":
    main()
