#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_follow_bench.py — "跟不上"的根因判决台 + 前馈/积分门控方案对比。

═══ 根因（一句话） ═══
匀速目标下，P 和 D 在 e=0 时都等于 0 —— 它们**没有记忆**。要让准星以目标速度
移动，环路必须持续输出 u_ss = ΔT / alpha（ΔT=目标的屏上速度）。所以"跟不上"不是
调不好，是**缺一个 DC 载体**。只有两种东西能提供它：
    (a) 积分项   —— 不需要知道 alpha（自标定），但会 windup ⇒ 过冲
    (b) 速度前馈 —— 立刻给出 u_ss，但必须知道 alpha ⇒ 用户滑条 (kf / 灵敏度补偿)
用户给的 `G:/qq/qq1/pid (1).cpp` 取的是 (b)，并且给出了防过冲的关键：
    integral_gain   —— "I 通路的权限"本身是 |e| 的连续函数，|e|>50 时它塌成 0。
    即：**前馈只在已经咬住目标的小误差带里才允许工作**，逼近阶段它被完全断开。
    这是"防过冲"的正确思路，本文件把它落到两条路上分别验证。

═══ 前馈的致命细节（必须知道，否则重蹈上一轮） ═══
物理恒等式：  Δe = ΔT − alpha·u_prev
⇒ ΔT̂ = Δe + alpha_hat·u_prev     （alpha_hat=1 时，u 的系数误差 = 1−alpha）
把 ΔT̂ 送进前馈 F = kf·LPF(ΔT̂)，而 u_prev 里**含 F 自己**，于是自环增益
        kf · (alpha_hat − alpha)
alpha_hat=1 时它就是 kf·(1−alpha)。alpha=0.1 ⇒ 自环 0.9·kf，kf 稍大就自激。
**所以低频端前馈不能当主力，积分才是。** 本文件把这个代价量化出来。
"""
import math
import random

HZ = 120.0
DT = 1.0 / HZ
K_OUT, K_TRIM, TAU_D, RAMP = 60.0, 56.0, 0.025, 0.30


def _gate(e, g, E, rate):
    """pid (1).cpp 的 kp_gain/integral_gain 结构：带内按 (1−|e|/E) 软升，
    带外按 10%/帧 快塌。"""
    if abs(e) < E:
        g += ((1.0 - abs(e) / E) - g) * rate
    else:
        g += (0.0 - g) * 0.10
    return min(1.0, max(0.0, g))


class Ctl:
    """mode: base      = 当前出厂（kp.10 ki.5 kd.20, 无门控）
             isep      = 积分按 |e|<E 分离（老项目 / pid(1).cpp 思路）
             conv      = 积分只在"误差没有在收敛"时累积（alpha 无关的判据）
             ff        = 速度前馈，alpha_hat=1，带内门控
             conv_ff   = conv + ff
    """

    def __init__(self, kp=0.10, ki=0.5, kd=0.20, trim=K_TRIM, mode="base",
                 E=100.0, kf=0.0, rate=0.3, ff_alpha=0.25, i_decay=0.5):
        self.kp, self.ki, self.kd, self.trim = kp, ki, kd, trim
        self.mode, self.E, self.kf, self.rate = mode, E, kf, rate
        self.ff_alpha, self.i_decay = ff_alpha, i_decay
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.ffv = 0.0
        self.gate = 0.0

    def update(self, e, dt):
        if not math.isfinite(e) or dt <= 0:
            return 0.0
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last
        a = dt / (dt + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-K_OUT, min(K_OUT, self.kd * self.ramp * self.d))
        p = self.kp * self.ramp * e

        # ── 前馈：重建目标屏上速度，门控在带内 ──────────────────────────────
        F = 0.0
        if self.mode in ("ff", "conv_ff"):
            dmeas = de + self.out                    # ≈ ΔT + (1−alpha)·u
            self.ffv += self.ff_alpha * (dmeas - self.ffv)
            self.gate = _gate(e, self.gate, self.E, self.rate)
            F = self.kf * self.gate * self.ffv

        raw = p + self.I + dd + F

        # ── 积分 ────────────────────────────────────────────────────────────
        allow = True
        if self.mode in ("isep",):
            if abs(e) >= self.E:
                self.I *= self.i_decay
                allow = False
        elif self.mode in ("conv", "conv_ff"):
            # 误差正在朝零收敛 ⇒ 冻结；误差停住不动 ⇒ 这才需要积分去补 DC
            converging = (de * e < 0.0) and (abs(de) > 0.3)
            if converging:
                allow = False
                self.I *= 0.995
        if allow and not (raw >= K_OUT and e > 0) and not (raw <= -K_OUT and e < 0):
            self.I += self.ki * e * dt
            self.I = max(-self.trim, min(self.trim, self.I))

        u = K_OUT * math.tanh((p + self.I + dd + F) / K_OUT)
        self.out = u
        self.last = e
        return u


def run(ctrl, alpha, scenario, steps=1800, sigma=1.5, seed=7, dist=250.0,
        speed=600.0, det_hold=1):
    rng = random.Random(seed)
    view = 0.0
    meas = None
    glow = []
    for n in range(steps):
        if scenario == "step":
            tgt = dist
        else:
            tgt = (speed / HZ) * n
        if n % det_hold == 0 or meas is None:
            meas = tgt - view + rng.gauss(0.0, sigma)
        e = meas
        u = ctrl.update(e, DT)
        view += alpha * u
        glow.append(tgt - view)
    return glow


def rep(glow, start=8, tail=900):
    fin = sum(glow[-50:]) / 50.0
    if len(glow) >= tail:
        seg = glow[-tail:]
        steady = sum(seg) / len(seg)
        pp = max(seg) - min(seg)
    else:
        steady = fin
        pp = max(glow[start:]) - min(glow[start:])
    peak = max(-(x - fin) for x in glow[start:])
    settle = len(glow)
    for i in range(start, len(glow)):
        if all(abs(x - fin) < 3.0 for x in glow[i:i + 40]):
            settle = i
            break
    return peak, settle, steady, pp


ALPHAS = [0.10, 0.15, 0.20, 0.30, 0.50, 1.00, 2.00]


def show(title, makers, scenarios):
    print("\n" + "=" * 96)
    print(title)
    print("=" * 96)
    for sc, kw in scenarios:
        print("\n-- %s --" % kw.pop("label"))
        print("alpha |" + "".join("%22s" % m[0] for m in makers))
        for a in ALPHAS:
            row = "%.2f  |" % a
            for _, mk in makers:
                pk, st, sd, pp = rep(run(mk(), a, sc, **kw))
                row += "%22s" % ("e=%6.1f 过=%5.0f" % (sd, pk))
            print(row)
        kw["label"] = kw.get("label", "")


if __name__ == "__main__":
    M = [
        ("出厂 无门控", lambda: Ctl()),
        ("积分分离 E=100", lambda: Ctl(mode="isep", E=100.0)),
        ("收敛门控", lambda: Ctl(mode="conv")),
        ("前馈 kf=1", lambda: Ctl(mode="ff", kf=1.0, E=100.0)),
        ("前馈 kf=3", lambda: Ctl(mode="ff", kf=3.0, E=100.0)),
    ]

    show("表A  匀速横移 600px/s —— 「跟不上」的主指标：稳态误差 e",
         M, [("strafe", {"label": "600px/s", "speed": 600.0})])

    show("表B  静止目标 250px 阶跃 —— 主指标：过冲",
         M, [("step", {"label": "250px 阶跃, 静止目标"})])

    show("表C  匀速横移 1500px/s（低 alpha 端）",
         M, [("strafe", {"label": "1500px/s", "speed": 1500.0})])

    print("\n" + "=" * 96)
    print("表D  前馈自环增益 kf·(1−alpha) 的实测代价：kf 扫描（600px/s）")
    print("=" * 96)
    print("alpha |" + "".join("%14s" % ("kf=%.1f" % k) for k in (0.5, 1.0, 2.0, 3.0, 5.0)))
    for a in ALPHAS:
        row = "%.2f  |" % a
        for k in (0.5, 1.0, 2.0, 3.0, 5.0):
            _, _, sd, pp = rep(run(Ctl(mode="ff", kf=k, E=100.0), a, "strafe", speed=600.0))
            row += "%14s" % ("e=%5.0f pp=%3.0f" % (sd, pp))
        print(row)
