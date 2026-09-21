#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_kf_bench.py — kf 前馈的"自洩"问题，以及 K_OUT 是不是低灵敏度端的真瓶颈。

═══════ 前馈的数学（这是用户问的"kf 怎么在 pid 里算"） ═══════
物理恒等式（准星固定在屏幕中心 ⇒ 误差 = 框的屏上坐标）：
        Δe(k) = ΔT(k) − alpha · u(k−1)
要前馈就必须把 alpha·u 从 Δe 里剔出去，得到目标自己的屏上速度：
        ΔT̂ = Δe + alpha_hat · u_prev
再乘上前馈增益 kf 变成指头位移（u_ss = ΔT/alpha），于是
        F = kf · LPF(ΔT̂)
**而 u_prev 里含 F 自己**，把它展开：
        F = kf·LPF( ΔT + (alpha_hat − alpha)(A + F) )        A = P+D+I
⇒ 自环增益 = kf·(alpha_hat − alpha)      ← 前馈自己喂自己。
alpha_hat 固定写 1（`pid (1).cpp` 的做法、也是我上一轮的做法）时：
        alpha=1 → 0（恰好无自洩，所以高灵敏度端它是好的）
        alpha=0.1 → 0.9·kf（kf 一过 1 就自激）
本文件的关键检验：把 alpha_hat 取成 **1/kf**（与前馈增益互为倒数，同一个旋钮），
自环增益就变成    1 − kf·alpha
⇒ kf = 1/alpha（即标定正确）时自洩**精确为 0**，前馈成为真正的前馈；
   安全窗是 kf·alpha ∈ (0, ~1.2)，刚好和环路增益同一个乘积。
"""
import math
import random

HZ = 120.0
DT = 1.0 / HZ
TAU_D, RAMP = 0.025, 0.30


class Ctl2:
    def __init__(self, kp=0.10, ki=0.5, kd=0.20, kout=60.0, trim=56.0,
                 kf=0.0, ahat="one", E=0.0, rate=0.3, ff_lp=0.25):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.kout, self.trim = kout, trim
        self.kf, self.ahat, self.ff_lp = kf, ahat, ff_lp
        self.E, self.rate = E, rate
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.ffv = 0.0
        self.g = 1.0

    def update(self, e, dt):
        if not math.isfinite(e) or dt <= 0:
            return 0.0
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last
        a = dt / (dt + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-self.kout, min(self.kout, self.kd * self.ramp * self.d))
        p = self.kp * self.ramp * e

        F = 0.0
        if self.kf > 0.0:
            ah = 1.0 if self.ahat == "one" else 1.0 / self.kf
            dmeas = de + ah * self.out
            self.ffv += self.ff_lp * (dmeas - self.ffv)
            if self.E > 0.0:
                if abs(e) < self.E:
                    self.g += ((1.0 - abs(e) / self.E) - self.g) * self.rate
                else:
                    self.g += (0.0 - self.g) * 0.10
                self.g = min(1.0, max(0.0, self.g))
            F = self.kf * self.g * self.ffv

        raw = p + self.I + dd + F
        if not (raw >= self.kout and e > 0) and not (raw <= -self.kout and e < 0):
            self.I += self.ki * e * dt
            self.I = max(-self.trim, min(self.trim, self.I))
        u = self.kout * math.tanh((p + self.I + dd + F) / self.kout)
        self.out = u
        self.last = e
        return u


def run(ctrl, alpha, scenario, steps=1800, sigma=1.5, seed=7, dist=250.0,
        speed=600.0):
    rng = random.Random(seed)
    view = 0.0
    glow = []
    for n in range(steps):
        tgt = dist if scenario == "step" else (speed / HZ) * n
        e = tgt - view + rng.gauss(0.0, sigma)
        u = ctrl.update(e, DT)
        view += alpha * u
        glow.append(tgt - view)
    return glow


def rep(glow, start=8, tail=900):
    seg = glow[-tail:]
    sd = sum(seg) / len(seg)
    pp = max(seg) - min(seg)
    fin = sum(glow[-50:]) / 50.0
    peak = max(-(x - fin) for x in glow[start:])
    settle = len(glow)
    for i in range(start, len(glow)):
        if all(abs(x - fin) < 3.0 for x in glow[i:i + 40]):
            settle = i
            break
    return sd, pp, peak, settle


ALPHAS = [0.10, 0.15, 0.20, 0.30, 0.50, 1.00]

if __name__ == "__main__":
    print("=" * 92)
    print("表1  低灵敏度端：输出上限 K_OUT 是不是真瓶颈？（无前馈，600px/s 横移）")
    print("=" * 92)
    print("alpha |" + "".join("%20s" % ("K_OUT=%d" % k) for k in (60, 120, 240, 480)))
    for a in ALPHAS:
        row = "%.2f  |" % a
        for k in (60, 120, 240, 480):
            sd, pp, _, _ = rep(run(Ctl2(kout=k), a, "strafe", speed=600.0))
            row += "%20s" % ("e=%6.1f pp=%5.1f" % (sd, pp))
        print(row)

    print("\n提示：alpha=0.1 时 u_ss = 600/120/0.1 = 50 px/步，而 K_OUT=60 已经进")
    print("      tanh 压缩区（tanh(50/60)·60 = 41.7）⇒ 递不出 50 ⇒ 必然留静差。")

    print("\n" + "=" * 92)
    print("表2  前馈自洩：alpha_hat=1（pid(1).cpp / 我上一轮） vs alpha_hat=1/kf")
    print("     600px/s 横移，K_OUT=240")
    print("=" * 92)
    for mode in ("one", "inv_kf"):
        print("\n-- alpha_hat = %s --" % ("固定 1  自环增益 = kf·(1−alpha)" if mode == "one"
                                        else "1/kf    自环增益 = 1 − kf·alpha"))
        print("alpha |" + "".join("%18s" % ("kf=%.1f" % k) for k in (1.0, 3.0, 10.0)))
        for a in ALPHAS:
            row = "%.2f  |" % a
            for k in (1.0, 3.0, 10.0):
                sd, pp, _, _ = rep(run(Ctl2(kout=240, kf=k, ahat=mode), a, "strafe",
                                       speed=600.0))
                row += "%18s" % ("e=%6.0f pp=%4.0f" % (sd, pp))
            print(row)

    print("\n" + "=" * 92)
    print("表3  带上限门控（前馈只在 |e|<E 里工作，pid(1).cpp 的 integral_gain 思路）")
    print("     kf=10, alpha_hat=1/kf, K_OUT=240, 600px/s")
    print("=" * 92)
    print("alpha |" + "".join("%22s" % s for s in ("E=0 无门控", "E=100", "E=250")))
    for a in ALPHAS:
        row = "%.2f  |" % a
        for E in (0.0, 100.0, 250.0):
            sd, pp, _, _ = rep(run(Ctl2(kout=240, kf=10.0, ahat="inv_kf", E=E),
                                   a, "strafe", speed=600.0))
            row += "%22s" % ("e=%6.0f pp=%4.0f" % (sd, pp))
        print(row)

    print("\n" + "=" * 92)
    print("表4  静止目标 250px 阶跃的过冲（K_OUT=240，kf=10，alpha_hat=1/kf）")
    print("=" * 92)
    print("alpha |" + "".join("%22s" % s for s in ("E=0 无门控", "E=100", "E=250")))
    for a in ALPHAS:
        row = "%.2f  |" % a
        for E in (0.0, 100.0, 250.0):
            _, _, pk, st = rep(run(Ctl2(kout=240, kf=10.0, ahat="inv_kf", E=E),
                                   a, "step"))
            row += "%22s" % ("过=%.0f 收敛%d" % (pk, st))
        print(row)
