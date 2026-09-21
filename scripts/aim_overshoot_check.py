#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_overshoot_check.py — 复现"超慢 + 严重过冲 + 慢慢拉回"，并检验积分分离。

背景：用户实测（kp0.10/ki0.5/kd0.20，出厂值）报"特别严重的过冲，然后还慢慢的拉回来"。
老项目 `G:/ai/Aimbot-ai/android-client/.../AimController.kt` 的 PID 有三个本控制器
**没有**的东西，其中一个直接写着防过冲：

    var integralSeparationThresh = 200f   // 误差大于此值时**禁用 Ki**
    ...
    if (|errorX| < sep) { integral += errorX } else { integral *= 0.5f }   // 超界还主动衰减

本文件检验：把这条路补上，能不能消掉低 alpha 端的过冲。

模型（与 pid_controller.h / aim_screenvel_check.py 一致）：
    view += alpha*u        alpha = 画面 px / 手指 px（游戏灵敏度）
    e      = 目标屏上位置 − 准星(屏幕中心)
"""
import math
import random
from collections import deque

HZ = 120.0
DT = 1.0 / HZ
K_OUT, K_TRIM, TAU_D, RAMP = 60.0, 56.0, 0.025, 0.30


class Ctl:
    """当前设备代码（含可选的积分分离）。sep=0 表示关闭分离。"""

    def __init__(self, kp=0.10, ki=0.5, kd=0.20, trim=K_TRIM, sep=0.0, decay=0.5):
        self.kp, self.ki, self.kd, self.trim = kp, ki, kd, trim
        self.sep, self.decay = sep, decay
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.history = []          # 记录 I 便于诊断

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

        # 积分分离（新增）：误差在带内才累积，带外主动衰减
        if self.sep > 0.0 and abs(e) >= self.sep:
            self.I *= self.decay
        else:
            if not (raw >= K_OUT and e > 0) and not (raw <= -K_OUT and e < 0):
                self.I += self.ki * e * dt
                self.I = max(-self.trim, min(self.trim, self.I))

        u = K_OUT * math.tanh((p + self.I + dd) / K_OUT)
        self.out = u
        self.last = e
        self.history.append(self.I)
        return u


def sim_step(ctrl, alpha, dist=250.0, steps=900, det_hold=1, sigma=1.5, seed=7):
    """静止目标阶跃：第 1 帧目标出现在离准星 dist px 处。
    det_hold=2 模拟"控制 120Hz / 检测 60Hz"：同一份测量被用两次。"""
    rng = random.Random(seed)
    view = 0.0
    meas_screen = None
    glow, us, Is = [], [], []
    for n in range(steps):
        if n % det_hold == 0 or meas_screen is None:
            meas_screen = dist - view + rng.gauss(0.0, sigma)
        e = meas_screen                     # 准星=屏幕中心 ⇒ 误差就是目标的屏上坐标
        u = ctrl.update(e, DT)
        view += alpha * u
        glow.append(dist - view)
        us.append(u)
        Is.append(ctrl.I)
    return {"glow": glow, "u": us, "I": Is}


def metrics(r, start=8):
    g = r["glow"]
    fin = sum(g[-50:]) / 50.0
    peak = max(-(x - fin) for x in g[start:])      # 最大反向越界 = 过冲
    # 首次进入 ±3px 并保持的帧号
    settle = len(g)
    for i in range(start, len(g)):
        if all(abs(x - fin) < 3.0 for x in g[i:i + 40]):
            settle = i
            break
    return peak, settle, fin


def strafe(alpha, speed, ctrl, steps=1800, sigma=1.5, seed=7):
    """匀速横移目标：先看能不能跟上（稳态误差），再看摆幅。"""
    rng = random.Random(seed)
    v = speed / HZ                      # px/步
    view = 0.0
    glow = []
    for n in range(steps):
        tgt = v * n + rng.gauss(0.0, sigma)
        e = tgt - view
        u = ctrl.update(e, DT)
        view += alpha * u
        glow.append(tgt - view)
    tail = glow[-900:]
    mean = sum(tail) / len(tail)
    pp = max(tail) - min(tail)
    return mean, pp


def hdr(t):
    print("\n" + "=" * 74)
    print(t)
    print("=" * 74)


if __name__ == "__main__":
    ALPHAS = [0.10, 0.15, 0.20, 0.30, 0.50, 1.00, 2.00]

    hdr("表1  静止目标 250px 阶跃：过冲 / 收敛帧数     （出厂 kp.10 ki.5 kd.20）")
    print("alpha   |  分离关(现在)          |  分离=200px           |  分离=80px")
    print("        |  过冲px 收敛帧  I_peak |  过冲px 收敛帧  I_peak|  过冲px 收敛帧  I_peak")
    for a in ALPHAS:
        row = "%.2f    |" % a
        for sep in (0.0, 200.0, 80.0):
            c = Ctl(sep=sep)
            r = sim_step(c, a)
            pk, st, fin = metrics(r)
            ipeak = max(abs(x) for x in r["I"])
            row += "  %6.1f %5d %6.1f |" % (pk, st, ipeak)
        print(row)

    hdr("表2  同一阶跃，检测 60Hz（同一份测量用两次）—— 量化「陈旧测量」的代价")
    print("alpha   |  分离关               |  分离=200px")
    print("        |  过冲px 收敛帧         |  过冲px 收敛帧")
    for a in ALPHAS:
        row = "%.2f    |" % a
        for sep in (0.0, 200.0):
            c = Ctl(sep=sep)
            r = sim_step(c, a, det_hold=2)
            pk, st, fin = metrics(r)
            row += "  %6.1f %5d       |" % (pk, st)
        print(row)

    hdr("表3  匀速横移 600px/s：稳态误差 / 摆幅")
    print("alpha   |  分离关            |  分离=200px         |  分离=80px")
    for a in ALPHAS:
        row = "%.2f    |" % a
        for sep in (0.0, 200.0, 80.0):
            c = Ctl(sep=sep)
            mean, pp = strafe(a, 600.0, c)
            row += "  e=%6.1f pp=%5.1f  |" % (mean, pp)
        print(row)

    hdr("表4  匀速横移 1500px/s（低 alpha 端需要大积分）")
    print("alpha   |  分离关            |  分离=200px         |  分离=80px")
    for a in ALPHAS:
        row = "%.2f    |" % a
        for sep in (0.0, 200.0, 80.0):
            c = Ctl(sep=sep)
            mean, pp = strafe(a, 1500.0, c)
            row += "  e=%6.1f pp=%5.1f  |" % (mean, pp)
        print(row)
