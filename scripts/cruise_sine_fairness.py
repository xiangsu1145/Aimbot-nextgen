"""Fairness re-test: the sine at realistic amplitude.

The A2 sine was +/-300 px at 1 Hz = 1885 px/s peak. That is faster than the
loop can follow at all (a 1800 px/s triangle diverges to 5000 px of error), so
condemning the filter on it proves nothing -- it condemned the loop.

A real player's smooth weave is more like +/-60..150 px at 0.5..2 Hz, i.e. a
peak of 190..940 px/s: inside the band where the loop actually works. This
sweep re-asks the same question there.

peak = amp * 2*pi / period
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


def one(amp, period, tau_ms, sigma=1.5):
    m.CRUISE_TAU = tau_ms / 1000.0
    m.RELEASE_TAU = max(0.015, min(0.025, m.CRUISE_TAU * 0.25))
    r = m.run(kp=0.10, ki=1.0, kd=0.0, alpha=ALPHA, ff_gain=1.0 / ALPHA, sigma_px=sigma,
              motion="sin", amp_px=amp, period_s=period, box_px=BOX,
              use_cruise=(tau_ms > 0))
    return r["rms_e"]


def main():
    print("Smooth weave (sine). Cell = rms error px; paren = peak speed px/s.")
    print("   amp  period |  peak |   raw   tau10   tau25  tau100")
    for amp, period in [(40, 0.5), (60, 0.5), (60, 1.0), (100, 1.0),
                        (150, 1.0), (100, 2.0), (150, 0.5), (300, 1.0)]:
        peak = amp * 2 * 3.14159 / period
        row = [one(amp, period, t) for t in [0, 10, 25, 100]]
        star = "  <-- diverged" if row[0] > 400 else ""
        print(f"  {amp:>4}  {period:>5.2f} | {peak:>5.0f} | {row[0]:>5.1f}  {row[1]:>6.1f}  {row[2]:>6.1f}  {row[3]:>6.1f}{star}")

    print("\nSame table as PERCENT OF A BOX (100 px), tau100 vs raw:")
    print("   amp  period |  peak |  raw(px) | tau100(px) |  delta")
    for amp, period in [(40, 0.5), (60, 0.5), (60, 1.0), (100, 1.0),
                        (150, 1.0), (100, 2.0), (150, 0.5)]:
        peak = amp * 2 * 3.14159 / period
        r = one(amp, period, 0)
        c = one(amp, period, 100)
        print(f"  {amp:>4}  {period:>5.2f} | {peak:>5.0f} | {r:>7.1f} | {c:>10.1f} | "
              f"{(c - r) / BOX * 100:>+5.0f}% of box")


if __name__ == "__main__":
    main()
