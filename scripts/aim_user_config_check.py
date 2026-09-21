#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_user_config_check.py — 用**设备当前的实测配置**跑判决台，看症状能不能被数字复现。

用户的配置（2026-09-21 报）：
    kp=0.05  ki=0  kd=0.2  outSmooth=0.8  延迟补偿=0  速度前馈=0  预测帧数=1
症状：跟枪特别滞后、完全跟不上；ki 一调就晃；前馈一调就抖。

本脚本只回答三件事：
  ① kp=0.05 / ki=0 的**固有稳态滞后**是多少（与其公式 e = v/(alpha·kp) 对照）
  ② 加上 ki 之后摆幅会不会爆（即"ki 一调就晃"在**当前代码**上还成立吗）
  ③ outSmooth=0.8 单独贡献多少代价（它不受 config schema 管控，升级后不会被重置）
"""
import math
import os
import random
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HZ = 120.0
DT = 1.0 / HZ
K_OUT, K_TRIM, TAU_D, RAMP = 60.0, 56.0, 0.025, 0.30


class Ctl:
    """与 tracking/pid_controller.h 同步（含 outSmooth 的真实 EMA）。"""

    def __init__(self, kp, ki, kd, out_smooth=1.0, trim=K_TRIM):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.out_smooth, self.trim = out_smooth, trim
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0

    def update(self, e, dt):
        if not math.isfinite(e) or dt <= 0:
            return 0.0
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last
        a = dt / (dt + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-K_OUT, min(K_OUT, self.kd * self.ramp * self.d))
        p = self.kp * self.ramp * e
        raw = p + self.I + dd
        if not (raw >= K_OUT and e > 0) and not (raw <= -K_OUT and e < 0):
            self.I += self.ki * e * dt
            self.I = max(-self.trim, min(self.trim, self.I))
        u = K_OUT * math.tanh((p + self.I + dd) / K_OUT)
        self.out = self.out_smooth * u + (1.0 - self.out_smooth) * self.out
        self.last = e
        return self.out


def sim(ctrl, tgt, alpha, delay=3, sigma=1.5, seed=7, steps=1800):
    rng = random.Random(seed)
    view = 0.0
    buf = deque([0.0] * (delay + 1), maxlen=delay + 1)
    rec = []
    for n in range(steps):
        glow = tgt(n) - view
        s = glow + rng.gauss(0.0, sigma)
        u = ctrl.update(buf[0], DT)
        view += alpha * u
        buf.append(s)
        rec.append(glow)
    return rec


def stats(rec, tail=900):
    w = rec[-tail:]
    n = len(w)
    return sum(w) / n, max(w) - min(w)


def strafe(s):
    return lambda n: s * n * DT


def sine(a, p):
    return lambda n: a * math.sin(2 * math.pi * n * DT / p)


CONFIGS = [
    ("U  用户现值 kp.05 ki0   kd.2 sm.8", dict(kp=0.05, ki=0.0, kd=0.20, out_smooth=0.8)),
    ("U2 同上但平滑关 sm1.0        ", dict(kp=0.05, ki=0.0, kd=0.20, out_smooth=1.0)),
    ("U3 用户值+ki=0.5            ", dict(kp=0.05, ki=0.5, kd=0.20, out_smooth=0.8)),
    ("D  出厂值 kp.10 ki.5 kd.2  ", dict(kp=0.10, ki=0.5, kd=0.20, out_smooth=1.0)),
]
ALPHAS = (0.3, 0.5, 1.0, 1.6, 2.5)


def main():
    print("=" * 100)
    print("① 600px/s 横移：稳态滞后 mean_e（px）—— 「跟枪特别滞后」就是这个数")
    print("   理论值 e = v/(alpha*kp)，v = 600/120 = 5 px/步")
    print("=" * 100)
    print("   %-28s | %s" % ("配置", "  ".join("a=%.1f " % a for a in ALPHAS)))
    print("-" * 100)
    for name, kw in CONFIGS:
        row = []
        for a in ALPHAS:
            m, _ = stats(sim(Ctl(**kw), strafe(600.0), a))
            row.append("%7.1f" % m)
        print("   %-28s | %s" % (name, "  ".join(row)))
    m, _ = stats(sim(Ctl(**CONFIGS[0][1]), strafe(600.0), 1.0))
    print("\n   验算：kp=0.05/ki=0/alpha=1.0 -> 理论 %.1f px，实测 %.1f px" % (5.0 / (1.0 * 0.05), m))

    print()
    print("=" * 100)
    print("② 摆幅（pp, px）——「ki 一调就晃」在当前代码上还成立吗")
    print("=" * 100)
    print("   %-28s | %s" % ("配置", "  ".join("a=%.1f " % a for a in ALPHAS)))
    print("-" * 100)
    for name, kw in CONFIGS:
        row = []
        for a in ALPHAS:
            _, pp = stats(sim(Ctl(**kw), strafe(600.0), a))
            row.append("%7.1f" % pp)
        print("   %-28s | %s" % (name, "  ".join(row)))

    print()
    print("=" * 100)
    print("③ 变向机动 ±150px@0.7Hz rms，与 253px 阶跃到 |e|<1 的帧数")
    print("=" * 100)
    print("   %-28s | %s | %s" % ("配置", "  变向 rms (a=0.5/1.0/2.5)",
                                "阶跃到|e|<1 (a=1.0)"))
    print("-" * 100)
    for name, kw in CONFIGS:
        rms = []
        for a in (0.5, 1.0, 2.5):
            r = sim(Ctl(**kw), sine(150.0, 1.0 / 0.7), a)
            rms.append(math.sqrt(sum(x * x for x in r[-900:]) / 900))
        r = sim(Ctl(**kw), lambda n: 253.0 if n >= 60 else 0.0, 1.0, steps=1200)
        n1 = next((i - 60 for i in range(60, len(r)) if abs(r[i]) < 1.0), -1)
        print("   %-28s | %6.1f %6.1f %6.1f | %s" % (
            name, rms[0], rms[1], rms[2], ("%d 帧" % n1) if n1 >= 0 else "never"))


if __name__ == "__main__":
    main()
