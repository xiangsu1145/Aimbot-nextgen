#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 alpha>=1 时的极限环打到时间轴上，看清到底是哪一项在摆。"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aim_alpha_fix import Ctl, sim, strafe, DT


def trace(alpha, spd=1500.0, n0=1200, n1=1320, **kw):
    c = Ctl(**kw)
    rec = sim(c, strafe(spd), alpha, steps=n1)
    need = spd * DT / alpha
    print(f"\n--- alpha={alpha}  {spd:.0f}px/s  需要位移={need:.1f} 手指px/步 "
          f"(积分 leash=24) ---")
    print(f"{'n':>5}{'glow':>9}{'u':>9}{'ff':>9}{'I':>9}{'p':>9}{'gate':>7}{'fx':>9}")
    # 重跑一遍留状态（sim 不返回内部量），改用逐帧打印
    c = Ctl(**kw)
    import random
    from collections import deque
    rng = random.Random(7)
    view = 0.0
    err = deque([0.0] * 4, maxlen=4)
    v_est, r_prev = 0.0, None
    for n in range(n1):
        e_meas = err[0]
        box_now = spd * n * DT + rng.gauss(0.0, 1.5)
        if r_prev is None:
            r_prev = box_now
        v_est += 0.42 * ((box_now - r_prev) - v_est)
        r_prev = box_now
        u = c.update(e_meas, DT, box=100.0, short=1080.0, tv=v_est)
        view += alpha * u
        glow = spd * n * DT - view
        err.append(box_now - view)
        if n0 <= n < n1 and n % 4 == 0:
            p = c.kp * c.ramp * e_meas
            print(f"{n:>5}{glow:>9.1f}{u:>9.2f}{c.ffg*c.gate*c.fx:>9.2f}"
                  f"{c.I:>9.2f}{p:>9.2f}{c.gate:>7.2f}{c.fx:>9.2f}")


if __name__ == "__main__":
    trace(1.0)
    trace(2.0)
