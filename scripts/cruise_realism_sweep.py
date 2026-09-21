"""The sine test is 1885 px/s peak. Is that a jink, or a strawman?

Everything above rests on one question: how fast does a real enemy actually
move across the screen? If a reversal is a 300 px/s event, a 100 ms estimate
is only 30 px stale -- a third of a box -- and the filter's jitter rejection
is nearly free. If it is a 1885 px/s event, the estimate is 190 px stale and
the filter is indefensible.

So: hold the motion shape fixed at "instantaneous reversal" (a triangle, the
literal "target turns around" case) and sweep the SPEED. The answer is the
speed at which the filter's cost becomes visible, and the question for the
user is whether their enemies live above or below it.

Also swept: how long the reversal lasts. A 0.25 s jink is a twitch; a 2 s
strafe is a commitment. A slow filter is only a liability for the first.
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

BOX = 100.0


def one(speed, period, tau_ms, sigma=1.5):
    m.CRUISE_TAU = tau_ms / 1000.0
    m.RELEASE_TAU = max(0.015, min(0.025, m.CRUISE_TAU * 0.25))
    r = m.run(kp=0.10, ki=1.0, kd=0.0, alpha=0.4, ff_gain=1.0 / 0.4, sigma_px=sigma,
              motion="triangle", speed_px_s=speed, period_s=period,
              use_cruise=(tau_ms > 0), box_px=BOX)
    return r["rms_e"]


def main():
    print("Triangle = instantaneous reversal (the literal 'target turns').")
    print("Box = 100 px.  Cell = rms tracking error in px.")
    print("  speed |  px/s |  raw   tau10  tau25 tau100     (period 1.0 s)")
    for speed in [150, 300, 500, 800, 1200, 1800]:
        row = [one(speed, 1.0, t) for t in [0, 10, 25, 100]]
        print(f"  {speed:>5} | {speed:>5} | {row[0]:>5.1f} {row[1]:>6.1f} {row[2]:>6.1f} {row[3]:>6.1f}")

    print("\n  speed |  px/s |  raw   tau10  tau25 tau100     (period 0.25 s = twitch)")
    for speed in [150, 300, 500, 800, 1200, 1800]:
        row = [one(speed, 0.25, t) for t in [0, 10, 25, 100]]
        print(f"  {speed:>5} | {speed:>5} | {row[0]:>5.1f} {row[1]:>6.1f} {row[2]:>6.1f} {row[3]:>6.1f}")

    print("\n  speed |  px/s |  raw   tau10  tau25 tau100     (period 2.0 s = long strafe)")
    for speed in [150, 300, 500, 800, 1200, 1800]:
        row = [one(speed, 2.0, t) for t in [0, 10, 25, 100]]
        print(f"  {speed:>5} | {speed:>5} | {row[0]:>5.1f} {row[1]:>6.1f} {row[2]:>6.1f} {row[3]:>6.1f}")

    print("\ncost of tau100 relative to raw, as a fraction of the BOX (100 px):")
    for speed, period in [(300, 0.25), (300, 1.0), (800, 0.25), (800, 1.0), (1800, 1.0)]:
        r = one(speed, period, 0)
        c = one(speed, period, 100)
        print(f"  speed {speed:>4} period {period:.2f}s : raw {r:>5.1f} -> tau100 {c:>6.1f}"
              f"  = {(c - r) / BOX * 100:+.0f}% of a box")


if __name__ == "__main__":
    main()
