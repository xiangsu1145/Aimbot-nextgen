#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_v5_verify.py — 第五轮控制器（tracking/pid_controller.h）的判决脚本。

`aim_fix_verify.py` 里的 PPID 是**第四轮**的移植，只作历史对照；本文件的
PPIDv5 才是设备上那份代码的镜像。改了 C++ 请同步这里。

被测症状（全部来自用户原话）：
    S1  又晃又抖 / 参数调小也晃 / 调大又晃又抖
    S2  根本跟不上枪
    S3  到目标附近就变得特别慢

本文件同时是**门控设计**的选型台：自校准估计器不知道什么时候该相信自己的
输出，门控就是那个开关。参考实现用"误差大小"开合，但那是建立在"它的误差
已经被另一个提前量模块压小了"的前提上 —— 这个前提本项目没有。
"""
import math
import os
import random
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aim_fix_verify as V          # 第四轮的 PPID，对照组

HZ  = 120.0
DT  = 1.0 / HZ
K_OUT   = 60.0        # tracking::kOutLimitPx
K_TRIM  = 24.0        # tracking::kTrimLimitPx
TAU_D   = 0.025       # tracking::kDerivTauSec

RAMP_RATE     = 0.30   # kRampRate
FF_Q          = 0.400  # kFFProcessNoise（终选：τ≈1.2 帧，见 aim_ff_gate_final.py）
FF_R          = 1.0    # kFFMeasNoise
FF_THR_MIN    = 50.0   # kFFGateThrMinPx（仅 err 门控使用）
FF_BOX        = 0.5    # kFFGateBox
FF_GATE_RATE  = 0.025  # kFFGateRate
FF_GATE_EXIT  = 0.10   # kFFGateExitRate

# ── 门控 ────────────────────────────────────────────────────────────────────
#   err    （已废弃）参考实现的规则：|e| < max(50, 半个框) 才淡入。它的前提是
#          "误差已被另一个提前量模块压小"，本项目没有这个前提 —— 实测在
#          alpha=0.15 的横移上锁死 135.9px，在 jink 上门几乎不开。
#   wide   同上去掉一个倍率，能过 A 段但 B 段依旧（门在大机动时必然关闭）。
#   open   不设门：横移/机动全对，但目标静止时指头会带上前馈的噪声。
#   vel    终选。按目标【速度】开合：目标真在动才相信"保持位移"这个量。
#          实测把静止抖动从 1.426 砍到 0.485 px/step，而 jink 一字未差
#          （37.3 vs 37.7）。物理上也说得通：前馈的职责就是跟上运动。
GATE_MODE = "vel"
GATE_VEL_FULL = 150.0   # px/s，达到该速度门全开
# 门限随目标框宽缩放，参考框 = 100px。近目标在同样 px/s 下角位移更小，远目标
# 则相反 —— 但实测"门只开一半"本身是良性的（mean_e 0.1），所以这里只做上抬、
# 不下压（max(1, ...)），且留作 box 参数唯一还在起作用的地方。
GATE_BOX_REF = 100.0


class PPIDv5:
    """tracking/pid_controller.h（第五轮）的逐行镜像。"""

    def __init__(self, kp=0.10, ki=1.0, kd=0.35, ff=1.0, out_smooth=1.0):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.ffg, self.os = ff, out_smooth
        self.I = 0.0
        self.last = 0.0
        self.out = 0.0
        self.prevU = 0.0
        self.d = 0.0
        self.ramp = 0.0
        self.fx = 0.0
        self.fp = 1.0
        self.gate = 0.0

    def update(self, e, dt, frozen=False, changed=False, box=0.0, short=1080.0,
               tv=0.0):
        if changed:
            self.I = 0.0
            self.d = 0.0
            self.last = e
            self.ramp = 0.0
            self.fx = 0.0
            self.fp = 1.0
            self.gate = 0.0
        self.ramp = min(1.0, self.ramp + RAMP_RATE)

        # ── 门控 ────────────────────────────────────────────────────────────
        if GATE_MODE == "open":
            self.gate = 1.0
        elif GATE_MODE == "vel":
            scale = max(1.0, box / GATE_BOX_REF) if box > 1.0 else 1.0
            want = min(1.0, abs(tv) / (GATE_VEL_FULL * DT * scale))
            r = FF_GATE_RATE if want > self.gate else FF_GATE_EXIT
            self.gate += (want - self.gate) * r
        else:
            scale = 4.0 if GATE_MODE == "wide" else 1.0
            hi = max(FF_THR_MIN, 0.5 * short)
            th = min(hi, max(FF_THR_MIN, FF_BOX * box)) * scale
            ae = abs(e)
            if ae < th:
                self.gate += ((1.0 - ae / th) - self.gate) * FF_GATE_RATE
            else:
                self.gate -= self.gate * FF_GATE_EXIT
        self.gate = min(1.0, max(0.0, self.gate))

        # ── 自校准前馈 ──────────────────────────────────────────────────────
        meas = (e - self.last) + self.prevU
        self.fp += FF_Q
        kf = self.fp / (self.fp + FF_R)
        self.fx += kf * (meas - self.fx)
        self.fp = (1.0 - kf) * self.fp
        ff = max(-K_OUT, min(K_OUT, self.ffg * self.gate * self.fx))

        # ── 微分：已滤波、每步无量纲 ────────────────────────────────────────
        a = dt / (dt + TAU_D)
        self.d += a * ((e - self.last) - self.d)
        dd = max(-K_OUT, min(K_OUT, self.kd * self.ramp * self.d))

        p = self.kp * self.ramp * e
        if frozen:
            p = 0.0
            dd = 0.0

        raw = p + self.I + dd
        if (not frozen) and not (raw >= K_OUT and e > 0.0) \
                and not (raw <= -K_OUT and e < 0.0):
            self.I += self.ki * e * dt
            self.I = max(-K_TRIM, min(K_TRIM, self.I))

        # 限幅只作用于【反馈】路径，前馈加在限幅之外。前馈是开环量、自带
        # ±kOutLimitPx 的钳位，而 tanh 的压缩会把它按比例吃掉 —— 高速目标
        # (v/alpha 逼近天花板时) 唯一靠得住的位移来源恰恰就是它。反饱和判据
        # 也只看反馈路径：会绕上限打转的只有积分，前馈不会。
        u = K_OUT * math.tanh((p + self.I + dd) / K_OUT) + ff
        self.out = self.os * u + (1.0 - self.os) * self.out
        self.last = e
        self.prevU = self.out
        return self.out


def sim(ctrl, tgt, alpha, delay=3, sigma=1.5, seed=7, steps=2400, tail=600,
        box=100.0, short=1080.0, k_vel=0.42):
    """一个敌人、单轴。tgt(n) 给第 n 帧目标在视野里的位置（view px）。

    控制器看到的是 delay 帧之前的误差 —— 闭环里唯一不可压缩的滞后。
    前馈已在控制器内部，外面不再另加开环项。
    """
    rng = random.Random(seed)
    view = 0.0
    err = deque([0.0] * (delay + 1), maxlen=delay + 1)
    v_est, r_prev = 0.0, None
    rec = {k: [] for k in ("glow", "u", "trim", "ff", "gate")}
    for n in range(steps):
        e_meas = err[0]           # 控制器看到的是 delay 帧前那一帧的误差
        # 检测器报的是【带噪】的框位置。噪声必须进这条误差链，否则测出来的
        # "抗噪能力" 全是假的 —— 早期版本只把噪声喂给测速通道，控制器看到的
        # 是干净误差，于是任何滤波都显得完美。
        box_now = tgt(n) + rng.gauss(0.0, sigma)
        if r_prev is None:
            r_prev = box_now
        v_est += k_vel * ((box_now - r_prev) - v_est)
        r_prev = box_now

        u = ctrl.update(e_meas, DT, box=box, short=short, tv=v_est)
        view += alpha * u
        glow = tgt(n) - view               # 真实误差，只用于评价
        err.append(box_now - view)         # delay 帧后控制器会读到它
        rec["glow"].append(glow)
        rec["u"].append(u)
        rec["trim"].append(ctrl.I)
        rec["ff"].append(ctrl.ffg * ctrl.gate * ctrl.fx)
        rec["gate"].append(ctrl.gate)
    return rec


def sim_old(ctrl, tgt, alpha, ff_gain, delay=3, sigma=1.5, seed=7,
            steps=2400, box=100.0):
    """第四轮的布置：开环前馈在控制器外面叠加，且吃的是 Kalman 速度。"""
    rng = random.Random(seed)
    view = 0.0
    err = deque([0.0] * (delay + 1), maxlen=delay + 1)
    v_est, r_prev = 0.0, None
    rec = {k: [] for k in ("glow", "u", "trim", "ff", "gate")}
    for n in range(steps):
        e_meas = err[0]
        # 同样的噪声注入点：进误差链，不只是进测速通道。
        box_now = tgt(n) + rng.gauss(0.0, sigma)
        if r_prev is None:
            r_prev = box_now
        v_est += 0.42 * ((box_now - r_prev) - v_est)
        r_prev = box_now
        ff = max(-K_OUT, min(K_OUT, v_est * ff_gain))
        u = ctrl.update(e_meas, DT, box_px=box) + ff
        view += alpha * u
        glow = tgt(n) - view
        err.append(box_now - view)
        rec["glow"].append(glow)
        rec["u"].append(u)
        rec["trim"].append(ctrl.I)
        rec["ff"].append(ff)
        rec["gate"].append(0.0)
    return rec


def stats(rec, tail=600):
    w = rec["glow"][-tail:]
    u = rec["u"][-tail:]
    n = len(w)
    mu = sum(u) / n
    return dict(
        mean_e=sum(w) / n,
        rms=math.sqrt(sum(x * x for x in w) / n),
        pp=max(w) - min(w),
        lowest=min(w),
        jerk=math.sqrt(sum((x - mu) ** 2 for x in u) / n),
        trim=sum(rec["trim"][-tail:]) / n,
        ff=sum(rec["ff"][-tail:]) / n,
        gate=sum(rec["gate"][-tail:]) / n,
    )


def strafe(spd, init=0.0):
    return lambda n: init + spd * n * DT


def tri(amp, period_s):
    return lambda n: amp * (4.0 * abs(((n * DT / period_s) % 1.0) - 0.5) - 1.0)


def step_to(d):
    return lambda n: d


def hdr(t):
    print("\n" + "=" * 78)
    print(t)
    print("=" * 78)


def use(mode):
    global GATE_MODE
    GATE_MODE = mode


# ── A：跟上能力 ─────────────────────────────────────────────────────────────
def a_alpha():
    hdr("A  跟上恒速目标 600 px/s —— 扫 alpha（游戏灵敏度），ff 滑块全用 1.0\n"
        "    第四轮必须手调 ff=1/alpha 才收敛；第五轮的估计器不知道 alpha")
    print(f"\n{'alpha':>6}{'需要':>7} | {'第四轮1.0':>10} | {'第四轮1/α':>10} | "
          + " | ".join(f"第五轮·{m:>5}" for m in ("err", "wide", "open", "vel")))
    print(f"{'':>6}{'px/st':>7} | {'mean_e':>10} | {'mean_e':>10} | "
          + " | ".join(f"{'mean_e':>10}" for _ in range(4)))
    print("-" * 78)
    for alpha in (0.15, 0.20, 0.30, 0.50, 0.80, 1.00, 1.60):
        need = 600.0 * DT / alpha
        o1 = stats(sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=False),
                           strafe(600.0), alpha, 1.0))
        o2 = stats(sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=False),
                           strafe(600.0), alpha, 1.0 / alpha))
        cells = []
        for m in ("err", "wide", "open", "vel"):
            use(m)
            cells.append(stats(sim(PPIDv5(), strafe(600.0), alpha))["mean_e"])
        print(f"{alpha:>6.2f}{need:>7.1f} | {o1['mean_e']:>10.0f} | "
              f"{o2['mean_e']:>10.0f} | "
              + " | ".join(f"{c:>10.1f}" for c in cells))


# ── B：大机动 ───────────────────────────────────────────────────────────────
def b_jink():
    hdr("B  大机动（jink）—— ±150px 三角波 @0.7Hz, alpha=0.5，拐点 ~857 px/s\n"
        "    第四轮的最优解是 ff=1/alpha=2.0；第五轮默认 1.0。\n"
        "    误差幅度 ~140px 远大于门控阈值 —— '误差大小' 型门控在这里必然是关的")
    print(f"\n{'门控':>6} | {'rms':>8}{'pp':>8}{'jerk':>8}{'mean_e':>8}"
          f"{'gate':>7}{'ff':>7}{'trim':>7}")
    print("-" * 60)
    o = stats(sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=False), tri(150.0, 0.7),
                      0.5, ff_gain=2.0, steps=1800), tail=900)
    print(f"{'第四轮':>6} | {o['rms']:>8.1f}{o['pp']:>8.1f}{o['jerk']:>8.2f}"
          f"{o['mean_e']:>8.1f}{'--':>7}{o['ff']:>7.1f}{o['trim']:>7.1f}")
    for m in ("err", "wide", "open", "vel"):
        use(m)
        s = stats(sim(PPIDv5(), tri(150.0, 0.7), 0.5, steps=1800), tail=900)
        print(f"{m:>6} | {s['rms']:>8.1f}{s['pp']:>8.1f}{s['jerk']:>8.2f}"
              f"{s['mean_e']:>8.1f}{s['gate']:>7.2f}{s['ff']:>7.1f}"
              f"{s['trim']:>7.1f}")


# ── C：接近剖面 ─────────────────────────────────────────────────────────────
def c_approach():
    hdr("C  纯阶跃接近 —— 目标静止在 250px 外\n"
        "    lowest = 最大反向越冲（负得越多越糟）；帧数 = 进入各误差带所需")
    print(f"\n{'方案':>7} | {'到100':>6}{'到20':>6}{'到5':>6}{'到1':>6}"
          f" | {'越冲':>8}{'末段trim':>9}")
    print("-" * 58)
    runs = []
    runs.append(("第四轮ON", stats(sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=True),
                                           step_to(250.0), 0.5, 0.0, steps=900),
                                   tail=120)))
    runs.append(("第四轮OFF", stats(sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=False),
                                            step_to(250.0), 0.5, 0.0, steps=900),
                                    tail=120)))
    for m in ("err", "wide", "open", "vel"):
        use(m)
        runs.append((f"第五·{m}", stats(sim(PPIDv5(), step_to(250.0), 0.5,
                                            steps=900), tail=120)))
    # 帧数要用轨迹算，单独跑一遍
    traces = {}
    traces["第四轮ON"] = sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=True),
                                 step_to(250.0), 0.5, 0.0, steps=900)
    traces["第四轮OFF"] = sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=False),
                                  step_to(250.0), 0.5, 0.0, steps=900)
    for m in ("err", "wide", "open", "vel"):
        use(m)
        traces[f"第五·{m}"] = sim(PPIDv5(), step_to(250.0), 0.5, steps=900)
    for name, s in runs:
        g = [abs(x) for x in traces[name]["glow"]]
        fr = []
        for thr in (100, 20, 5, 1):
            i = next((k for k, v in enumerate(g) if v < thr), None)
            fr.append(f"{i:>6}" if i is not None else f"{'--':>6}")
        print(f"{name:>7} | " + "".join(fr)
              + f" | {s['lowest']:>8.1f}{s['trim']:>9.1f}")


# ── D：噪声免疫 ─────────────────────────────────────────────────────────────
def d_noise():
    hdr("D  检测框噪声 —— 目标静止，看指头输出有多吵\n"
        "    第四轮把 Kalman 速度(τ=1.4 帧, 实为微分器)乘 1/alpha 直接推进去")
    print(f"\n{'sigma':>7} | {'第四轮 ff=3.3':>14}{'jerk':>8}"
          + "".join(f" | {f'第五·{m}':>12}{'jerk':>7}"
                    for m in ("open", "vel")))
    print("-" * 78)
    for sg in (0.5, 1.5, 3.0, 6.0, 12.0):
        o = stats(sim_old(V.PPID(0.10, 1.0, 0.35, use_lock=False),
                          lambda n: 0.0, 0.5, 3.33, sigma=sg, steps=1200),
                  tail=600)
        line = f"{sg:>7.1f} | {o['rms']:>14.2f}{o['jerk']:>8.2f}"
        for m in ("open", "vel"):
            use(m)
            s = stats(sim(PPIDv5(), lambda n: 0.0, 0.5, sigma=sg, steps=1200),
                      tail=600)
            line += f" | {s['rms']:>12.2f}{s['jerk']:>7.2f}"
        print(line)


if __name__ == "__main__":
    a_alpha()
    b_jink()
    c_approach()
    d_noise()
    print()
