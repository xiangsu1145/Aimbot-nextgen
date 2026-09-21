"""Pick tau by objective, in PIXELS (what is felt), not ratios.

Ratios are the wrong unit here: a 150 px/s walk has a near-zero no-filter
error, so any tau makes its ratio explode while the absolute change is a few
px. Absolute px is what the user sees as "behind" or "buzzing", so that is
the unit of the objective.

  cost(tau) = mean over the suite of [ rms(tau) - rms(0) ]   in px

and we want the largest tau whose cost is under ~10 px (a tenth of a box),
because a tenth of a box is the threshold at which "it trails" becomes
visible rather than theoretical.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("verify", os.path.join(HERE, "aim_fix_verify.py"))
m = importlib.util.module_from_spec(spec)
sys.stdout = open(os.devnull, "w")
spec.loader.exec_module(m)
sys.stdout = sys.__stdout__

ALPHA = 0.4
BOX = 100.0

SUITE = [
    ("walk 150",     dict(motion="strafe",   speed_px_s=150.0)),
    ("strafe 300",   dict(motion="strafe",   speed_px_s=300.0)),
    ("sprint 600",   dict(motion="strafe",   speed_px_s=600.0)),
    ("jink300/1.0",  dict(motion="triangle", speed_px_s=300.0, period_s=1.0)),
    ("jink600/0.5",  dict(motion="triangle", speed_px_s=600.0, period_s=0.5)),
    ("jink600/2.0",  dict(motion="triangle", speed_px_s=600.0, period_s=2.0)),
    ("weave300",     dict(motion="sin", amp_px=48.0, period_s=1.0)),
    ("weave400",     dict(motion="sin", amp_px=64.0, period_s=1.0)),
    ("weave600",     dict(motion="sin", amp_px=96.0, period_s=1.0)),
]


def run_one(tau_ms, kw, sigma=3.0):
    m.CRUISE_TAU = tau_ms / 1000.0
    m.RELEASE_TAU = max(0.015, min(0.030, m.CRUISE_TAU * 0.30))
    return m.run(kp=0.10, ki=1.0, kd=0.0, alpha=ALPHA, ff_gain=1.0 / ALPHA,
                 sigma_px=sigma, box_px=BOX, use_cruise=(tau_ms > 0), **kw)


def report(sigma):
    base = {label: run_one(0, kw, sigma) for label, kw in SUITE}
    print(f"\n=== box noise sigma = {sigma} px ===")
    print("tau ms | " + " ".join(f"{l:>11}" for l, _ in SUITE) + " |  mean_d  worst_d  jerk")
    best = None
    for tau in [0, 5, 10, 15, 20, 25, 35, 50, 75, 100]:
        ds, jerks = [], []
        for label, kw in SUITE:
            r = run_one(tau, kw, sigma)
            ds.append(r["rms_e"] - base[label]["rms_e"])
            jerks.append(r["jerk"])
        mean_d = sum(ds) / len(ds)
        worst = max(ds)
        jerk = sum(jerks) / len(jerks)
        print(f"{tau:>6} | " + " ".join(f"{d:>+11.1f}" for d in ds)
              + f" | {mean_d:>+7.1f} {worst:>+8.1f} {jerk:>5.2f}")
        if best is None or jerk < best[2] - 1e-9:
            best = (tau, mean_d, jerk, worst)

    ok = [(t, md, j, w) for t in [5, 10, 15, 20, 25, 35, 50, 75, 100]
          for md, j, w in [(0, 0, 0)]]
    rows = []
    for tau in [5, 10, 15, 20, 25, 35, 50, 75, 100]:
        ds, jerks = [], []
        for label, kw in SUITE:
            r = run_one(tau, kw, sigma)
            ds.append(r["rms_e"] - base[label]["rms_e"])
            jerks.append(r["jerk"])
        rows.append((tau, sum(ds) / len(ds), max(ds), sum(jerks) / len(jerks)))
    free = [r for r in rows if r[2] <= 10.0]
    print(f"  largest tau with WORST-motion cost <= 10 px : "
          f"{max(r[0] for r in free) if free else 'none'} ms")
    if free:
        b = max(free, key=lambda r: r[3])
        print(f"  (that tau is {b[0]} ms, worst {b[2]:+.1f} px, mean jerk {b[3]:.2f}"
              f" vs {sum(run_one(0, kw, sigma)['jerk'] for _, kw in SUITE) / len(SUITE):.2f} raw)")


if __name__ == "__main__":
    for s in (1.5, 3.0, 8.0):
        report(s)
    print("\nIf the free zone is empty at every noise level, the honest reading is")
    print("that the second filter is not earning its phase lag and should go.")
