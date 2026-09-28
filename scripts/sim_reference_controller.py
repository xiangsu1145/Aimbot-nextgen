#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Faithful line-by-line port of the user's reference controller
(G:\\qq\\qq1\\锁死控制器.cpp) + a CLOSED-LOOP harness.

Purpose: answer "how fast does the reference lock onto a moving target, and
what plant gain (alpha) is it implicitly tuned for?"  Nothing here talks to the
phone; it is the reference's own arithmetic, driven by a synthetic plant.

The port keeps the original's names and order so it can be diffed by eye.
"""

BANDWIDTH = 10000.0
NUM = 4.0 / 15.0
DEN = 3.0 / 5.0


def smooth_term(value, bandwidth, output_scale):
    ratio = value / bandwidth
    sq = ratio * ratio
    return (ratio * (1.0 + NUM * sq) / (1.0 + DEN * sq)) * output_scale


class P_PID(object):
    def __init__(self, kp_, kd_, predict_, rate_, smooth_):
        self.kp = kp_
        self.kd = kd_
        self.bandwidth = BANDWIDTH
        self.smooth = smooth_
        self.kp_gain_rate = rate_
        self.predict = predict_
        self.ki_deadband = 0.5
        # member defaults (NOT set by init in the original)
        self.kp_gain = 0.0
        self.kp_gain_threshold = 1920.0
        self.integral_gain = 0.0
        self.integral_gain_threshold = 50.0
        self.integral_gain_rate = 0.025
        self.u_filtered = 0.0
        self.integral_term = 0.0
        self.last_error = 0.0
        self.last_u = 0.0
        self.velocity_filter_x = 0.0
        self.velocity_filter_p = 1.0
        self.integral_filter_x = 0.0
        self.integral_filter_p = 1.0

    def reset(self):
        self.kp_gain = 0.0
        self.integral_gain = 0.0
        self.u_filtered = 0.0
        self.last_error = 0.0
        self.integral_term = 0.0
        self.last_u = 0.0
        self.velocity_filter_x = 0.0
        self.velocity_filter_p = 0.0
        self.integral_filter_x = 0.0
        self.integral_filter_p = 0.0

    def adjust_integral(self, error):
        ae = abs(error)
        if ae < self.integral_gain_threshold:
            ratio = 1.0 - (ae / self.integral_gain_threshold)
            self.integral_gain += (ratio - self.integral_gain) * self.integral_gain_rate
        else:
            ratio = self.integral_gain_threshold / ae
            self.integral_gain += (ratio * self.integral_gain - self.integral_gain) * 0.1
        self.integral_gain = min(max(self.integral_gain, 0.0), 1.0)

    def kp_integral(self, error):
        ae = abs(error)
        if ae < self.kp_gain_threshold:
            ratio = 1.0 - (ae / self.kp_gain_threshold)
            self.kp_gain += (ratio - self.kp_gain) * self.kp_gain_rate
        else:
            ratio = self.kp_gain_threshold / ae
            self.kp_gain += (ratio * self.kp_gain - self.kp_gain) * 0.1
        self.kp_gain = min(max(self.kp_gain, 0.0), 1.0)

    def update_velocity_filter(self, measurement):
        q, r = 0.01, 1.0
        px = self.velocity_filter_x
        pp = self.velocity_filter_p + q
        k = pp / (pp + r)
        self.velocity_filter_x = px + k * (measurement - px)
        self.velocity_filter_p = (1 - k) * pp
        return self.velocity_filter_x

    def update_integral_filter(self, measurement):
        q, r = 0.5, 1.0
        px = self.integral_filter_x
        pp = self.integral_filter_p + q
        k = pp / (pp + r)
        self.integral_filter_x = px + k * (measurement - px)
        self.integral_filter_p = (1 - k) * pp
        return self.integral_filter_x

    def update(self, error):
        if abs(error) < 0.3:
            error = 0.0
        if abs(error - self.last_error) > 30.0:
            self.reset()

        self.adjust_integral(error)
        self.kp_integral(error)

        error_diff = error - self.last_error
        target_velocity = error_diff + self.last_u
        target_velocity = self.update_velocity_filter(target_velocity)

        raw_velocity_input = target_velocity
        if abs(error) < 1.0 and abs(error_diff) < 0.1:
            raw_velocity_input = error_diff + self.last_u * 0.5

        ki_raw = raw_velocity_input
        ki_raw = ki_raw if abs(ki_raw) > 0.5 else 0.0
        ki_raw = (ki_raw * self.predict) * self.integral_gain
        ki_raw = self.update_integral_filter(ki_raw)

        K_p = self.kp * error
        K_i = ki_raw
        K_d = self.kd * (error - self.last_error)

        if self.smooth:
            K_p = smooth_term(K_p, self.bandwidth, self.bandwidth - self.smooth)
            K_i = smooth_term(K_i, self.bandwidth, self.bandwidth - 1000.0)
            K_d = smooth_term(K_d, self.bandwidth, self.bandwidth - self.smooth)

        u = K_p + K_i + K_d
        self.u_filtered = u * self.kp_gain
        self.last_u = self.u_filtered
        self.last_error = error
        return self.u_filtered, K_p, K_i, K_d


# ── Closed loop: the reference's own main() parameters ──────────────────────
REF = dict(kp_=15.0, kd_=25.0, predict_=3.0, rate_=0.3, smooth_=9900.0)

WARM = 20          # frames of stationary target before it starts moving
N = 70             # frames to run


def run(alpha, v_target, label):
    pid = P_PID(**REF)
    tp, cp = 0.0, 0.0
    need = abs(v_target) / alpha            # u_ss required for zero standing error
    rows = []
    for n in range(N):
        tp += 0.0 if n < WARM else v_target
        e = tp - cp
        u, Kp, Ki, Kd = pid.update(e)
        cp += alpha * u
        rows.append((n, n - WARM, e, u, Kp, Ki, Kd, pid.integral_gain))
        if n == WARM + 19:
            pass
    verr = tp - cp
    print("\n=== %s   alpha=%.3f  v_target=%.1f  =>  u_ss needed = %.2f finger px ===" %
          (label, alpha, v_target, need))
    print(" frame tgt  err      u      K_p     K_i     K_d    ig      u/u_ss")
    for (n, t, e, u, Kp, Ki, Kd, ig) in rows:
        if t >= -2 and t <= 24:
            print("%5d %4d %7.2f %7.2f %8.1f %7.2f %6.2f %5.2f   %5.2f" %
                  (n, t, e, u, Kp, Ki, Kd, ig, u / need if need else 0.0))
    # convergence: first frame where |u| >= 0.95*need and stays |err| small
    ok = None
    for (n, t, e, u, Kp, Ki, Kd, ig) in rows:
        if t >= 0 and abs(u) >= 0.95 * need:
            ok = t
            break
    print(" -> reaches 95%% of required carrier at t = %s frames after the move"
          % ("never" if ok is None else str(ok)))
    print(" -> standing error at end: %.2f screen px" % verr)
    print(" -> K_i / u at end: %.3f   (this is the share the velocity path supplies)"
          % (rows[-1][5] / rows[-1][3] if rows[-1][3] else 0.0))


if __name__ == "__main__":
    print("Reference controller (G:\\qq\\qq1\\锁死控制器.cpp) — closed loop")
    print("kp=15 kd=25 predict=3.0 rate=0.3 smooth=9900 (its own main())")
    print("Crosshair moves alpha * u screen px per frame; u is the controller output.")
    for alpha in (0.30, 0.37, 0.50, 1.00):
        run(alpha, 3.0, "strafe 3.0 screen px/frame (360 px/s)")
