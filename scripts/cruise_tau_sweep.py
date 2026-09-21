"""Sweep the cruise-filter time constant against the two motions that disagree.

A (constant strafe) is where the filter earns its keep: it removes the raw
Kalman estimate's box-noise ripple from the open-loop feed-forward path, at
zero cost in trailing error.

A2 (smooth 1 Hz reversal) is where it can lose: a filtered velocity is a LATE
velocity, and a sine has no velocity discontinuity to justify that lag.

This file answers one question: is there a tau that keeps most of A's noise
rejection without paying A2's phase lag? If no such tau exists, the filter is
the wrong tool and the honest fix is elsewhere.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("verify", os.path.join(HERE, "aim_fix_verify.py"))
m = importlib.util.module_from_spec(spec)
sys.stdout = open(os.devnull, "w")          # the module runs its report on import
spec.loader.exec_module(m)
sys.stdout = sys.__stdout__


def sweep():
    print("tau ms | strafe jerk | strafe rms | sine jerk | sine rms  pp_e")
    print("-------|-------------|------------|-----------|---------------")
    for tau_ms in [0, 10, 20, 25, 35, 50, 75, 100, 150]:
        m.CRUISE_TAU = tau_ms / 1000.0
        m.RELEASE_TAU = max(0.015, min(0.025, m.CRUISE_TAU * 0.25))
        a = m.run(kp=0.10, ki=1.0, kd=0.0, alpha=0.3, ff_gain=2.0,
                  motion="strafe", speed_px_s=1800.0, use_cruise=True)
        b = m.run(kp=0.10, ki=1.0, kd=0.0, alpha=0.6, ff_gain=1.0 / 0.6,
                  motion="sin", amp_px=300.0, period_s=1.0, use_cruise=True)
        print(f"{tau_ms:>6} | {a['jerk']:>11.2f} | {a['rms_e']:>10.1f} | "
              f"{b['jerk']:>9.2f} | {b['rms_e']:>7.1f} {b['pp_e']:>9.1f}")

    print("\nreference (no filter at all, raw Kalman vx):")
    for label, kw in [("strafe", dict(alpha=0.3, ff_gain=2.0, motion="strafe",
                                      speed_px_s=1800.0)),
                      ("sine", dict(alpha=0.6, ff_gain=1.0 / 0.6, motion="sin",
                                    amp_px=300.0, period_s=1.0))]:
        r = m.run(kp=0.10, ki=1.0, kd=0.0, use_cruise=False, **kw)
        print(f"  {label:<7} jerk {r['jerk']:>6.2f}  rms {r['rms_e']:>7.1f}  pp {r['pp_e']:>8.1f}")

    print("\nreference (ground-truth velocity, the unreachable floor):")
    for label, kw in [("strafe", dict(alpha=0.3, ff_gain=2.0, motion="strafe",
                                      speed_px_s=1800.0)),
                      ("sine", dict(alpha=0.6, ff_gain=1.0 / 0.6, motion="sin",
                                    amp_px=300.0, period_s=1.0))]:
        r = m.run(kp=0.10, ki=1.0, kd=0.0, ff_ideal=True, **kw)
        print(f"  {label:<7} jerk {r['jerk']:>6.2f}  rms {r['rms_e']:>7.1f}  pp {r['pp_e']:>8.1f}")


if __name__ == "__main__":
    sweep()
