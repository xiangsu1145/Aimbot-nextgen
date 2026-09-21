"""Does the cruise filter's value depend on how noisy the detector is?

The feed-forward does not consume a raw difference: it consumes the Kalman
velocity state, which is itself a first-order low-pass of the measured box
difference with tau = dt / k_vel ~= 20 ms at 120 Hz. The cruise filter is
therefore a SECOND pole in series. Whether a second pole is worth its phase
lag is a question about signal-to-noise, not about taste.

The sweep flips one variable: sigma_px, the per-frame box jitter. If the
crossover sits below the detector's real jitter, the filter is justified; if
it sits far above, the filter is smoothing noise the first pole already ate.
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

MOTIONS = [("strafe", dict(alpha=0.3, ff_gain=2.0, motion="strafe", speed_px_s=1800.0)),
           ("sin", dict(alpha=0.6, ff_gain=1.0 / 0.6, motion="sin", amp_px=300.0, period_s=1.0))]


def cell(sigma, tau_ms, kw, k_vel=0.42):
    m.CRUISE_TAU = tau_ms / 1000.0
    m.RELEASE_TAU = max(0.015, min(0.025, m.CRUISE_TAU * 0.25))
    r = m.run(kp=0.10, ki=1.0, kd=0.0, sigma_px=sigma, k_vel=k_vel,
              use_cruise=(tau_ms > 0), **kw)
    return r["jerk"], r["rms_e"]


def main():
    for name, kw in MOTIONS:
        print(f"\n=== {name} ===  (jerk / rms_e)")
        print("  sigma |   raw |  tau10 |  tau25 | tau100 | raw+vel0.06")
        for sigma in [1.5, 3.0, 6.0, 12.0, 24.0]:
            cols = []
            for tau in [0, 10, 25, 100]:
                cols.append(cell(sigma, tau, kw))
            slow = cell(sigma, 0, kw, k_vel=0.06)
            print(f"  {sigma:>5} | {cols[0][0]:>5.2f}/{cols[0][1]:>5.1f} | "
                  f"{cols[1][0]:>5.2f}/{cols[1][1]:>5.1f} | "
                  f"{cols[2][0]:>5.2f}/{cols[2][1]:>5.1f} | "
                  f"{cols[3][0]:>5.2f}/{cols[3][1]:>5.1f} | "
                  f"{slow[0]:>5.2f}/{slow[1]:>5.1f}")

    print("\n'raw+vel0.06' = no cruise filter, but the KALMAN velocity bandwidth")
    print("dropped from 0.42 to 0.06 (tau ~ 140 ms). Same idea, one pole, and")
    print("the pole lives where the covariance bookkeeping can see it.")


if __name__ == "__main__":
    main()
