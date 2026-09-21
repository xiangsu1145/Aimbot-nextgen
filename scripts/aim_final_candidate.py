#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_final_candidate.py — 终方案：kf 前馈（alpha_hat = 1/kf）+ 上限随 1/alpha 缩放。

三个根因（本文件逐条给数）：
 ① 输出上限 60px/步 = 画面速度 alpha·60·120 px/s。alpha=0.1 时只有 720px/s，
    而 u_ss = ΔT/alpha = 50px/步 已进 tanh 压缩区 ⇒ 永久静差 306px。
    ⇒ **上限是低灵敏度端的真瓶颈，不是增益。**
 ② P/D 在 e=0 时为零，环路缺 DC 载体 ⇒ 必须给 u_ss。
 ③ 前馈的重建常数 alpha_hat 与前馈增益 kf 必须是同一个数（互为倒数）。
    同了 ⇒ kf=1/alpha 时自洩精确为 0；不同（alpha_hat 固定 1）⇒ 低频端 pp 冲到 4740。
"""
import math
import random

HZ = 120.0
DT = 1.0 / HZ
TAU_D, RAMP = 0.025, 0.30


class Ctl3:
    def __init__(self, kp, ki, kd, kout, trim, kf=0.0, ahat="inv_kf", ff_lp=0.25,
                 kff_ref=1.0):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.kout, self.trim = kout, trim
        self.kf, self.ahat, self.ff_lp = kf, ahat, ff_lp
        self.kff_ref = kff_ref
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.ffv = 0.0

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
            F = self.kf * self.ffv * self.kff_ref

        raw = p + self.I + dd + F
        if not (raw >= self.kout and e > 0) and not (raw <= -self.kout and e < 0):
            self.I += self.ki * e * dt
            self.I = max(-self.trim, min(self.trim, self.I))
        u = self.kout * math.tanh((p + self.I + dd + F) / self.kout)
        self.out = u
        self.last = e
        return u


def shipped():
    """当前设备上的出厂值（无前馈，上限 60/56，增益不缩放）。"""
    return Ctl3(0.10, 0.5, 0.20, 60.0, 56.0)


def candidate(alpha, s=None, kff_ref=1.0):
    """s = 灵敏度补偿 ≈ 1/alpha；增益、上限、前馈全部按它缩放。"""
    s = (1.0 / alpha) if s is None else s
    return Ctl3(0.10 * s, 0.5 * s, 0.20 * s, 60.0 * s, 56.0 * s,
                kf=s, ahat="inv_kf", kff_ref=kff_ref)


def run(ctrl, alpha, scenario, steps=1800, sigma=1.5, seed=7, dist=250.0,
        speed=600.0, amp=150.0, freq=0.7):
    rng = random.Random(seed)
    view = 0.0
    glow = []
    for n in range(steps):
        t = n / HZ
        if scenario == "step":
            tgt = dist
        elif scenario == "strafe":
            tgt = (speed / HZ) * n
        else:  # jink: 正弦变向
            tgt = amp * math.sin(2 * math.pi * freq * t)
        e = tgt - view + rng.gauss(0.0, sigma)
        u = ctrl.update(e, DT)
        view += alpha * u
        glow.append(tgt - view)
    return glow


def stat(glow, tail=900, start=8):
    seg = glow[-tail:]
    sd = sum(seg) / len(seg)
    rms = math.sqrt(sum(x * x for x in seg) / len(seg))
    pp = max(seg) - min(seg)
    fin = sum(glow[-50:]) / 50.0
    peak = max(-(x - fin) for x in glow[start:])
    settle = len(glow)
    for i in range(start, len(glow)):
        if all(abs(x - fin) < 3.0 for x in glow[i:i + 40]):
            settle = i
            break
    return sd, rms, pp, peak, settle


ALPHAS = [0.05, 0.10, 0.15, 0.20, 0.30, 0.50, 1.00, 2.00]

if __name__ == "__main__":
    print("=" * 100)
    print("表1  匀速横移 600px/s —— 稳态误差(跟枪) / 摆幅。  出厂 = 现在设备上的；终方案 = kf 前馈+上限缩放")
    print("=" * 100)
    print("alpha |            出厂（现在）          |            终方案 s=1/alpha        |    终方案 s 偏小一半")
    print("      |      e        rms      pp       |      e        rms      pp          |      e        rms      pp")
    for a in ALPHAS:
        row = "%.2f  |" % a
        for c in (shipped(), candidate(a), candidate(a, s=0.5 / a)):
            sd, rms, pp, _, _ = stat(run(c, a, "strafe", speed=600.0))
            row += " %7.1f %8.1f %7.1f  |" % (sd, rms, pp)
        print(row)

    print("\n" + "=" * 100)
    print("表2  匀速横移 1500px/s（低灵敏度端真正的地狱）")
    print("=" * 100)
    print("alpha |            出厂（现在）          |            终方案 s=1/alpha")
    for a in ALPHAS:
        row = "%.2f  |" % a
        for c in (shipped(), candidate(a)):
            sd, rms, pp, _, _ = stat(run(c, a, "strafe", speed=1500.0))
            row += " %7.1f %8.1f %7.1f  |" % (sd, rms, pp)
        print(row)

    print("\n" + "=" * 100)
    print("表3  静止目标 250px 阶跃 —— 过冲 / 收敛帧（用户报的\"特别严重的过冲\"）")
    print("=" * 100)
    print("alpha |        出厂（现在）        |        终方案 s=1/alpha     |  终方案 s 偏大一倍")
    for a in ALPHAS:
        row = "%.2f  |" % a
        for c in (shipped(), candidate(a), candidate(a, s=2.0 / a)):
            _, _, _, pk, st = stat(run(c, a, "step"))
            row += "  过=%6.0f 收敛%5d |" % (pk, st)
        print(row)

    print("\n" + "=" * 100)
    print("表4  正弦变向（jink ±150px @0.7Hz）rms")
    print("=" * 100)
    print("alpha |     出厂     |   终方案")
    for a in ALPHAS:
        r1 = stat(run(shipped(), a, "jink"))[1]
        r2 = stat(run(candidate(a), a, "jink"))[1]
        print("%.2f  | %10.1f   | %9.1f" % (a, r1, r2))

    print("\n" + "=" * 100)
    print("表5  标定误差：s 偏离 1/alpha 多少会坏？（600px/s，终方案结构）")
    print("=" * 100)
    devs = [0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0]
    print("alpha |" + "".join("%16s" % ("s·alpha=%.2f" % d) for d in devs))
    for a in ALPHAS:
        row = "%.2f  |" % a
        for d in devs:
            sd, rms, pp, _, _ = stat(run(candidate(a, s=d / a), a, "strafe", speed=600.0))
            row += "%16s" % ("e=%5.0f pp=%4.0f" % (sd, pp))
        print(row)
    print("\n(s·alpha = 1 表示标定正确；前馈自洩 = 1 − kf·alpha = 1 − s·alpha)")

    print("\n" + "=" * 100)
    print("表6  手指位移上限的物理封顶：opLimit 按 s 缩放后截到多少才不伤低 alpha 端？")
    print("     600px/s 横移，终方案结构（s=1/alpha）")
    print("=" * 100)
    caps = [60.0, 120.0, 180.0, 240.0, 480.0, 99999.0]
    print("alpha |" + "".join("%16s" % ("cap=%s" % (int(c) if c < 99999 else "∞"))
                            for c in caps))
    for a in (0.05, 0.10, 0.15, 0.20, 0.30):
        row = "%.2f  |" % a
        s = 1.0 / a
        for c in caps:
            ctl = Ctl3(0.10 * s, 0.5 * s, 0.20 * s, min(60.0 * s, c),
                       min(56.0 * s, c), kf=s, ahat="inv_kf")
            sd, rms, pp, _, _ = stat(run(ctl, a, "strafe", speed=600.0))
            row += "%16s" % ("e=%5.0f pp=%4.0f" % (sd, pp))
        print(row)

    print("\n" + "=" * 100)
    print("表7  前馈增益独立于灵敏度补偿：kf 单独偏了多少还安全？")
    print("     环路与上限保持标定正确（s=1/alpha），只动 kf。前馈自洩 = 1 − kf·alpha")
    print("=" * 100)
    print("alpha |" + "".join("%16s" % ("kf·alpha=%.2f" % k) for k in (0.25, 0.5, 1.0, 2.0, 5.0)))
    for a in ALPHAS:
        s = 1.0 / a
        row = "%.2f  |" % a
        for k in (0.25, 0.5, 1.0, 2.0, 5.0):
            ctl = Ctl3(0.10 * s, 0.5 * s, 0.20 * s, 60.0 * s, 56.0 * s,
                       kf=k * s, ahat="inv_kf")
            sd, rms, pp, _, _ = stat(run(ctl, a, "strafe", speed=600.0))
            row += "%16s" % ("e=%5.0f pp=%4.0f" % (sd, pp))
        print(row)
