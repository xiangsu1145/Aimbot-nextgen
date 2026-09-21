#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_ring_guard.py — 环路增益不可能自动按 alpha 归一（见 aim_screenvel_check.py：
tracker 报的是屏幕速度，alpha 不可辨识）。于是换成**不管 alpha 也能保证不震荡**：

    增益往前顶，一旦环自己开始震荡就退回来。

判据必须是"震荡"而不是"噪声"：噪声（白）的自相关在 lag>=1 上 ≈0，震荡在
半个周期上 ≈ −1，目标机动在低频上 > 0。所以取 【lag 3~5 的最小归一化自相关】
作判据，噪声不会误触发，目标横移/机动也不会。

这样出厂增益可以按"低 alpha 也要跟得上"来定（高），高 alpha 时环路自己会退。
"""
import math
import os
import random
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HZ = 120.0
DT = 1.0 / HZ
K_OUT, K_TRIM, TAU_D, RAMP = 60.0, 48.0, 0.025, 0.30


class Ctl:
    def __init__(self, kp=0.10, ki=0.5, kd=0.20, trim=K_TRIM, guard=False,
                 thr=-0.35, down=0.030, up=0.003, floor=0.25):
        self.kp, self.ki, self.kd, self.trim = kp, ki, kd, trim
        self.guard, self.thr = guard, thr
        self.down, self.up, self.floor = down, up, floor
        self.gm = 1.0
        self.hist = deque([0.0] * 13, maxlen=13)
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.rmin = 0.0

    def update(self, e, dt, **kw):
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last

        # ── 震荡守卫 ────────────────────────────────────────────────────────
        if self.guard:
            den = sum(x * x for x in self.hist)
            if den > 1e-3:
                r = min(sum(self.hist[i] * self.hist[i + l]
                            for i in range(len(self.hist) - l)) / den
                        for l in (3, 4, 5))
                self.rmin = r
                self.gm = min(1.0, max(self.floor,
                                       self.gm + (-self.down if r < self.thr
                                                  else self.up)))
        g = self.gm

        a = dt / (dt + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-K_OUT, min(K_OUT, self.kd * g * self.ramp * self.d))
        p = self.kp * g * self.ramp * e
        raw = p + self.I + dd
        if not (raw >= K_OUT and e > 0) and not (raw <= -K_OUT and e < 0):
            self.I += self.ki * g * e * dt
            self.I = max(-self.trim, min(self.trim, self.I))
        self.out = K_OUT * math.tanh((p + self.I + dd) / K_OUT)
        self.last = e
        self.hist.append(e)
        return self.out


def sim(ctrl, tgt, alpha, delay=3, sigma=1.5, seed=7, steps=2400, k_vel=0.42):
    rng = random.Random(seed)
    view = 0.0
    buf = deque([0.0] * (delay + 1), maxlen=delay + 1)
    rec = {k: [] for k in ("glow", "u", "trim", "gm")}
    for n in range(steps):
        screen_now = tgt(n) - view + rng.gauss(0.0, sigma)
        glow = tgt(n) - view
        u = ctrl.update(buf[0], DT)
        view += alpha * u
        buf.append(screen_now)
        rec["glow"].append(glow)
        rec["u"].append(u)
        rec["trim"].append(ctrl.I)
        rec["gm"].append(ctrl.gm)
    return rec


def stats(rec, tail=900):
    w, u = rec["glow"][-tail:], rec["u"][-tail:]
    n = len(w)
    mu = sum(u) / n
    return dict(mean=sum(w) / n, rms=math.sqrt(sum(x * x for x in w) / n),
                pp=max(w) - min(w),
                jerk=math.sqrt(sum((x - mu) ** 2 for x in u) / n),
                gm=sum(rec["gm"][-tail:]) / n)


def strafe(s): return lambda n: s * n * DT
def tri(a, p): return lambda n: a * (4.0 * abs(((n * DT / p) % 1.0) - 0.5) - 1.0)


def hdr(t):
    print("\n" + "=" * 94)
    print(t)
    print("=" * 94)


def main():
    AL = (0.30, 0.50, 1.00, 1.60, 2.50)
    hdr("1  守卫开关对比：jink ±150px@0.7Hz rms（括号 = 守卫稳定后的增益乘子）")
    print(f"{'G':>6}{'守卫':>5} | " + "".join(f"{'@'+str(a):>14}" for a in AL)
          + f"{'横扫1.6(pp)':>13}{'静止3':>8}")
    print("-" * 94)
    for G in (0.10, 0.16, 0.25, 0.40):
        for gd in (False, True):
            mk = lambda: Ctl(G, 5 * G, 2 * G, guard=gd)
            cells = []
            for a in AL:
                s = stats(sim(mk(), tri(150.0, 0.7), a))
                cells.append(f"{s['rms']:>8.1f}({s['gm']:.2f})" if gd
                             else f"{s['rms']:>14.1f}")
            s16 = stats(sim(mk(), strafe(1500.0), 1.6))
            st = stats(sim(mk(), lambda n: 0.0, 1.0, sigma=3.0, steps=1200),
                       tail=600)
            print(f"{G:>6.2f}{'开' if gd else '关':>5} | "
                  + "".join(f"{c:>14}" for c in cells)
                  + f"{s16['mean']:>7.0f}({s16['pp']:>4.0f})"
                  + f"{st['jerk']:>8.3f}")

    hdr("2  守卫误触发检查：目标横移 / 静止 / 阶跃时 gm 应保持 1.00")
    for nm, tgt, al in (("600px/s 横移", strafe(600.0), 1.0),
                        ("±150 jink", tri(150.0, 0.7), 1.0),
                        ("静止+噪声", lambda n: 0.0, 1.0),
                        ("253 阶跃", lambda n: 250.0, 1.0)):
        for G in (0.10, 0.40):
            s = stats(sim(Ctl(G, 5 * G, 2 * G, guard=True), tgt, al,
                          steps=1800), tail=900)
            print(f"  {nm:>12} G={G:.2f}: gm={s['gm']:.2f}  rms={s['rms']:7.2f}"
                  f"  横扫滞后={s['mean']:6.1f}")

    hdr("3  最终方案全表：G=0.25 + 守卫（对比 G=0.10 无守卫）")
    for nm, mk in (("G.10 无守卫", lambda: Ctl(0.10, 0.5, 0.20)),
                   ("G.25 +守卫", lambda: Ctl(0.25, 1.25, 0.50, guard=True))):
        print(f"\n  [{nm}]")
        print(f"    jink rms   : " + "".join(
            f"{stats(sim(mk(), tri(150.0, 0.7), a))['rms']:>8.1f}" for a in AL))
        print(f"    1500 横移  : " + "".join(
            f"{stats(sim(mk(), strafe(1500.0), a))['mean']:>8.0f}" for a in AL))
        print(f"    600 横移   : " + "".join(
            f"{stats(sim(mk(), strafe(600.0), a))['mean']:>8.0f}" for a in AL))
        rec = sim(mk(), lambda n: 250.0, 0.5, steps=900)
        g = [abs(x) for x in rec["glow"]]
        i1 = next((k for k, v in enumerate(g) if v < 1.0), None)
        jk = stats(sim(mk(), tri(150.0, 0.7), 1.0))["rms"]
        st = stats(sim(mk(), lambda n: 0.0, 1.0, sigma=3.0, steps=1200),
                   tail=600)["jerk"]
        print(f"    253 阶跃   : 到1={i1} 帧, 越冲={min(rec['glow'][-120:]):.1f}"
              f"   静止 jerk={st:.3f}   jink@1.0={jk:.1f}")


if __name__ == "__main__":
    main()
    print()
