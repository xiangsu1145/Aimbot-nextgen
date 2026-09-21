#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把某方案在某个目标运动下的轨迹打到时间轴上。"""
import os
import random
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aim_alpha_fix import Ctl, DT, tri, strafe, zero  # noqa


def run(ctrl, tgt, alpha, delay=3, sigma=1.5, seed=7, steps=1200,
        show=(600, 780), every=4, box=100.0, short=1080.0):
    rng = random.Random(seed)
    view = 0.0
    err = deque([0.0] * (delay + 1), maxlen=delay + 1)
    v_est, r_prev = 0.0, None
    print(f"{'n':>5}{'tgt':>8}{'view':>8}{'e':>8}{'u':>8}{'I':>8}{'p':>8}{'d':>8}")
    for n in range(steps):
        e_meas = err[0]
        tv = tgt(n)
        box_now = tv + rng.gauss(0.0, sigma)
        if r_prev is None:
            r_prev = box_now
        v_est += 0.42 * ((box_now - r_prev) - v_est)
        r_prev = box_now
        u = ctrl.update(e_meas, DT, box=box, short=short, tv=v_est)
        view += alpha * u
        glow = tv - view
        err.append(box_now - view)
        if show[0] <= n < show[1] and n % every == 0:
            p = ctrl.kp * ctrl.ramp * e_meas
            print(f"{n:>5}{tv:>8.1f}{view:>8.1f}{e_meas:>8.1f}{u:>8.2f}"
                  f"{ctrl.I:>8.2f}{p:>8.2f}{ctrl.d:>8.2f}")


if __name__ == "__main__":
    print("=== jink ±150px@0.7Hz alpha=0.5 —— 无FF, ki=1, kd=0.35, trim=60 ===")
    run(Ctl(ff=0.0, trim=60.0), tri(150.0, 0.7), 0.5)
    print("\n=== 同上，ki=8 kd=1 ===")
    run(Ctl(ff=0.0, trim=60.0, ki=8.0, kd=1.0), tri(150.0, 0.7), 0.5)
