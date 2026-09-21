#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_defect_verdict.py — 用数字回答本轮用户报的三个症状。

    S1  参数都调特别小了都晃 / 调大就又晃又抖
    S2  根本跟不上枪
    S3  到目标附近就变得特别慢

plant 与 aim_fix_verify.py 同源（view += alpha * u，检测延迟 3 帧，
检测框噪声 sigma px），PPID 直接 import 那个文件里的逐行移植，保证被测的
就是设备上跑的那份代码。

T1  状态机自激：Acquire(半增益) <-> Track(全增益) 的硬切换是否自己造出
    一个"降低 kp 也消不掉"的极限环？切换次数是判据。
T2  权限天花板：kTrimLimitPx=24 与 FF 标定误差叠加后，稳态误差的下限由谁
    决定？扫描 ff 与目标速度，看 trim 是否钉死在 24。
T3  接近剖面：纯阶跃（目标静止在 250px 外），看状态机把接近过程切成几段、
    到 10px 要多花多少帧。
T4  自校准前馈：DsAi 的 (Δe + u_prev) 估计器 —— 不告诉它 alpha，它能不能
    自己找到正确的指头位移？与手工 ff=1.0 / ff=1/alpha 对比。
"""
import math
import os
import random
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aim_fix_verify as V          # noqa: E402  (PPID 与本项目同源的移植)

PPID   = V.PPID
K_OUT  = V.K_OUT      # 60
K_TRIM = V.K_TRIM     # 24
HZ     = V.HZ         # 120
DT     = 1.0 / HZ


# ── 仿真骨架 ────────────────────────────────────────────────────────────────
def sim(ctrl, tgt, alpha, ff_gain=0.0, auto=None, delay=3, sigma=1.5, seed=7,
        steps=2400, tail=600, box=100.0, short_edge=1080.0, k_vel=0.42):
    """一个敌人，单轴。tgt(n) 给出第 n 帧目标在视野里的位置（view px）。

    delay 是检测延迟（帧）。控制器看到的是 delay 帧之前的误差，这是闭环里
    唯一不可压缩的滞后来源。
    """
    rng = random.Random(seed)
    view = 0.0
    err = deque([0.0] * (delay + 1), maxlen=delay + 1)
    v_est = 0.0
    r_prev = None
    u_prev = 0.0
    rec = {k: [] for k in ("glow", "meas", "u", "trim", "p", "d", "ff", "lock")}
    flips = 0
    was_locked = ctrl.locked

    for n in range(steps):
        e_meas = err[0]

        # 跟踪器会交给自瞄的那一路：带噪测量 + 速度估计
        r_meas = tgt(n - delay) + rng.gauss(0.0, sigma)
        if r_prev is None:
            r_prev = r_meas
        v_est += k_vel * ((r_meas - r_prev) - v_est)
        r_prev = r_meas

        # ── 开环项 ──────────────────────────────────────────────────────────
        ff = 0.0
        if auto is not None:
            ff += auto.step(e_meas, u_prev, DT, box)
        if ff_gain:
            ff += v_est * ff_gain
        ff = max(-K_OUT, min(K_OUT, ff))

        u = ctrl.update(e_meas, DT, box_px=box) + ff
        if ctrl.use_lock and box > 1.0 and ctrl.locked != was_locked:
            flips += 1
        was_locked = ctrl.locked
        u_prev = u

        view += alpha * u
        glow = tgt(n) - view
        err.append(glow)

        rec["glow"].append(glow)
        rec["meas"].append(e_meas)
        rec["u"].append(u)
        rec["trim"].append(ctrl.I)
        rec["ff"].append(ff)
        rec["lock"].append(1.0 if ctrl.locked else 0.0)

    return rec, flips


def stats(rec, tail=600):
    w = rec["glow"][-tail:]
    u = rec["u"][-tail:]
    n = len(w)
    mean_u = sum(u) / n
    return dict(
        mean_e=sum(w) / n,
        rms=math.sqrt(sum(x * x for x in w) / n),
        pp=max(w) - min(w),
        jerk=math.sqrt(sum((x - mean_u) ** 2 for x in u) / n),
        trim=sum(rec["trim"][-tail:]) / n,
        lock_pct=100.0 * sum(rec["lock"][-tail:]) / n,
    )


def hdr(t):
    print("\n" + "=" * 78)
    print(t)
    print("=" * 78)


# ── T4 用：DsAi 真正的机制 ──────────────────────────────────────────────────
class AutoFF:
    """DsAi 的滞后杀手，pid.cpp:46-57 的忠实移植。

    机理：e = T - F，指头每步走 u，所以
        Δe = ΔT - u_prev        =>        ΔT = Δe + u_prev
    也就是说 **Δe + u_prev 直接估出了"让误差停止变化所需的指头位移"**，单位
    就是指头 px/step —— 正是前馈要吃的那个量。它不需要知道游戏灵敏度 alpha，
    因为它测的是"这个环路为了让误差不动实际付出了多少输出"。

    固定点：稳态时 Δe→0 且 u→u*，于是估计值 → u*。这就是自标定。

    参考实现用一维 Kalman 低通（q=0.01, r=1.0），并用 integral_gain 做门控：
    |e| 大于阈值时让估计器闭嘴（避免接近过程被过冲），|e| 小时淡入。
    门控阈值 50px 是按 1080p 屏幕定的，这里改成随目标框自适应。
    """

    def __init__(self, q=0.01, r=1.0, gate_rate=0.025, gate_box=0.5):
        self.q, self.r = q, r
        self.gate_rate = gate_rate
        self.gate_box = gate_box
        self.x = 0.0
        self.p = 1.0
        self.last_e = 0.0
        self.raw = 0.0
        self.gate = 0.0

    def step(self, e, u_prev, dt, box=100.0):
        thr = max(50.0, self.gate_box * box)
        ae = abs(e)
        if ae < thr:
            target = 1.0 - ae / thr
            self.gate += (target - self.gate) * self.gate_rate
        else:
            self.gate += (0.0 - self.gate) * 0.1
        self.gate = max(0.0, min(1.0, self.gate))

        meas = (e - self.last_e) + u_prev
        self.last_e = e
        self.p += self.q
        k = self.p / (self.p + self.r)
        self.x += k * (meas - self.x)
        self.p = (1.0 - k) * self.p
        self.raw = self.x

        v = self.x * self.gate
        return max(-K_OUT, min(K_OUT, v))


# ── 目标轨迹 ────────────────────────────────────────────────────────────────
def strafe(speed, init=0.0):
    return lambda n: init + speed * n * DT


def step_to(dist):
    return lambda n: dist


# ── T1 ──────────────────────────────────────────────────────────────────────
def t1():
    hdr("T1  状态机自激 —— '参数调小也晃' 的嫌疑犯\n"
        "    恒速横移 400 px/s, alpha=0.5, ff=1.0(标定错 2 倍), ki=1, kd=0.35\n"
        "    flips = Acquire<->Track 切换次数 / 2400 帧")
    print(f"\n{'kp':>6} | {'半增益锁定ON':^38} | {'状态机关闭':^30}")
    print(f"{'':>6} | {'mean_e':>8}{'pp':>7}{'rms':>7}{'flips':>8}{'lock%':>7}"
          f" | {'mean_e':>8}{'pp':>7}{'rms':>7}{'jerk':>6}")
    print("-" * 78)
    for kp in (0.02, 0.04, 0.06, 0.08, 0.10, 0.14, 0.20, 0.30):
        on = PPID(kp, 1.0, 0.35, use_lock=True)
        r1, f1 = sim(on, strafe(400.0), 0.5, ff_gain=1.0)
        s1 = stats(r1)
        off = PPID(kp, 1.0, 0.35, use_lock=False)
        r0, _ = sim(off, strafe(400.0), 0.5, ff_gain=1.0)
        s0 = stats(r0)
        print(f"{kp:>6.2f} | {s1['mean_e']:>8.1f}{s1['pp']:>7.1f}{s1['rms']:>7.1f}"
              f"{f1:>8d}{s1['lock_pct']:>7.0f}"
              f" | {s0['mean_e']:>8.1f}{s0['pp']:>7.1f}{s0['rms']:>7.1f}"
              f"{s0['jerk']:>6.2f}")


# ── T2 ──────────────────────────────────────────────────────────────────────
def t2():
    hdr("T2  输出权限天花板 —— '根本跟不上枪'\n"
        "    alpha=0.3 (细灵敏度), kp=0.10 ki=1 kd=0.35 状态机关闭\n"
        "    紧跟一个恒速目标所需的指头位移 = v_view / alpha；该值超过 24\n"
        "    (kTrimLimitPx) 时积分就已经没权限了，P 只能补差价")
    print(f"\n{'速度':>7} | {'需要':>7} | " + " | ".join(
        f"{f'ff={g}':>16}" for g in ("0", "1.0", "2.0", "3.33")))
    print(f"{'px/s':>7} | {'px/step':>7} | " + " | ".join(
        f"{'mean_e':>8}{'trim':>8}" for _ in range(4)))
    print("-" * 78)
    for spd in (300, 600, 1000, 1500, 2000):
        need = (spd * DT) / 0.3
        row = f"{spd:>7} | {need:>7.1f} | "
        cells = []
        for g in (0.0, 1.0, 2.0, 3.33):
            c = PPID(0.10, 1.0, 0.35, use_lock=False)
            r, _ = sim(c, strafe(spd), 0.3, ff_gain=g)
            s = stats(r)
            cells.append(f"{s['mean_e']:>8.1f}{s['trim']:>8.1f}")
        print(row + " | ".join(cells))


# ── T3 ──────────────────────────────────────────────────────────────────────
def t3():
    hdr("T3  接近剖面 —— '到目标附近就变得特别慢'\n"
        "    纯阶跃：目标静止在 250 px 外。测量到达各误差带所需帧数\n"
        "    (状态机默认态是 Acquire = 半增益；要升到 Track 必须 |e|<6px\n"
        "     或者连续 5 帧安静，而接近过程中误差一直在变 -> 一直安静不了)")
    print(f"\n{'阈值':>7} | {'ON 帧数':>9} | {'OFF 帧数':>9} | 倍数")
    print("-" * 46)
    res = {}
    for use_lock in (True, False):
        c = PPID(0.10, 1.0, 0.35, use_lock=use_lock)
        r, _ = sim(c, step_to(250.0), 0.5, ff_gain=0.0, steps=900)
        g = [abs(x) for x in r["glow"]]
        res[use_lock] = g
    for thr in (100, 50, 20, 10, 5, 2):
        t_on = next((i for i, v in enumerate(res[True]) if v < thr), None)
        t_off = next((i for i, v in enumerate(res[False]) if v < thr), None)
        if t_on is None or t_off is None:
            print(f"{thr:>7} | {'未达到':>9} | {'未达到':>9} |")
            continue
        ratio = (t_on / t_off) if t_off else float("inf")
        print(f"{thr:>7} | {t_on:>9d} | {t_off:>9d} | {ratio:>5.2f}x")


# ── T4 ──────────────────────────────────────────────────────────────────────
def t4():
    hdr("T4  自校准前馈 —— DsAi 真正解决'跟不上'的机制\n"
        "    恒速横移 600 px/s，扫 alpha。ff=1.0 是出厂默认；ff=1/alpha 是\n"
        "    几何正确值(需要知道游戏灵敏度)；auto 是 (Δe+u_prev) 估计器，\n"
        "    它不知道 alpha")
    print(f"\n{'alpha':>6} | {'需要':>7} | {'ff=1.0':>9}{'trim':>7}"
          f" | {'ff=1/α':>9}{'trim':>7} | {'auto':>9}{'trim':>7}")
    print("-" * 78)
    for alpha in (0.3, 0.5, 0.8, 1.0, 1.6):
        need = (600.0 * DT) / alpha
        row = f"{alpha:>6.2f} | {need:>7.1f} | "
        cells = []
        for label, ff, mk in (("man1", 1.0, None),
                              ("ideal", 1.0 / alpha, None),
                              ("auto", 0.0, AutoFF)):
            c = PPID(0.10, 1.0, 0.35, use_lock=False)
            r, _ = sim(c, strafe(600.0), alpha, ff_gain=ff,
                       auto=(mk() if mk else None))
            s = stats(r)
            cells.append(f"{s['mean_e']:>9.1f}{s['trim']:>7.1f}")
        print(row + " | ".join(cells))


# ── T5：估计器带宽 ──────────────────────────────────────────────────────────
def t5():
    hdr("T5  auto 估计器的带宽 —— q 决定它多快学会目标速度\n"
        "    q=0.01 是参考实现的出厂值(等效时间常数约 100 帧)。太快会吃\n"
        "    噪声，太慢则在目标变向后来不及。恒速横移 600 px/s, alpha=0.5，\n"
        "    并给出'学会一半'所需帧数")
    print(f"\n{'q':>8} | {'tau(帧)':>8} | {'mean_e':>9}{'rms':>8}{'jerk':>8}"
          f"{'trim':>8}")
    print("-" * 60)
    for q in (0.001, 0.005, 0.01, 0.02, 0.05, 0.1):
        # 一维 Kalman 稳态增益 -> 等效 tau
        p = 0.0001
        for _ in range(200):
            p = (1.0 - p / (p + 1.0)) * (p + q)
        k = p / (p + 1.0)
        tau = (1.0 - k) / k if k > 0 else float("inf")
        c = PPID(0.10, 1.0, 0.35, use_lock=False)
        r, _ = sim(c, strafe(600.0), 0.5, ff_gain=0.0, auto=AutoFF(q=q))
        s = stats(r)
        print(f"{q:>8.3f} | {tau:>8.1f} | {s['mean_e']:>9.1f}{s['rms']:>8.1f}"
              f"{s['jerk']:>8.2f}{s['trim']:>8.1f}")


# ── T6：状态机在最坏工况 ────────────────────────────────────────────────────
def t6():
    hdr("T6  状态机在最坏工况 —— 远目标(小框) + 高速\n"
        "    alpha=0.3, ff=1.0(标定错 3.3 倍)。far 带 = 1.7 x 框宽，框越小\n"
        "    越容易被稳态滞后误差顶穿 -> 降级成半增益。flips 是判据")
    print(f"\n{'框宽':>6}{'速度':>8} | {'ON mean_e':>10}{'pp':>7}{'flips':>7}"
          f"{'lock%':>7} | {'OFF mean_e':>11}{'pp':>7}{'flips':>7}")
    print("-" * 78)
    for box in (24.0, 40.0, 60.0, 100.0):
        for spd in (600.0, 1200.0, 1800.0):
            on = PPID(0.10, 1.0, 0.35, use_lock=True)
            r1, f1 = sim(on, strafe(spd), 0.3, ff_gain=1.0, box=box)
            s1 = stats(r1)
            off = PPID(0.10, 1.0, 0.35, use_lock=False)
            r0, f0 = sim(off, strafe(spd), 0.3, ff_gain=1.0, box=box)
            s0 = stats(r0)
            print(f"{box:>6.0f}{spd:>8.0f} | {s1['mean_e']:>10.1f}{s1['pp']:>7.1f}"
                  f"{f1:>7d}{s1['lock_pct']:>7.0f}"
                  f" | {s0['mean_e']:>11.1f}{s0['pp']:>7.1f}{f0:>7d}")


# ── T7：前馈旋钮的两面代价 ──────────────────────────────────────────────────
def tri_amp(amp, period_s):
    """三角波：瞬时反向的横移，最像玩家 jink。"""
    return lambda n: amp * (4.0 * abs(((n * DT / period_s) % 1.0) - 0.5) - 1.0)


def t7():
    hdr("T7  前馈旋钮的两面 —— '调小跟不上, 调大又晃又抖'\n"
        "    jink: 三角波 +-150px @0.7Hz (拐点速度 ~857 px/s), alpha=0.5\n"
        "    几何正确值 = 1/alpha = 2.0。看误差与输出抖动怎么随 ff 变")
    print(f"\n{'ff':>6} | {'mean_e':>8}{'rms':>8}{'pp':>8}{'jerk':>8}{'trim':>8}")
    print("-" * 52)
    for g in (0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0):
        c = PPID(0.10, 1.0, 0.35, use_lock=False)
        r, _ = sim(c, tri_amp(150.0, 0.7), 0.5, ff_gain=g, steps=1800, tail=900)
        s = stats(r, tail=900)
        mark = "  <- 1/alpha" if abs(g - 2.0) < 1e-9 else ""
        print(f"{g:>6.2f} | {s['mean_e']:>8.1f}{s['rms']:>8.1f}{s['pp']:>8.1f}"
              f"{s['jerk']:>8.2f}{s['trim']:>8.1f}{mark}")


# ── T8：前馈吃进去的速度有多脏 ──────────────────────────────────────────────
def t8():
    hdr("T8  前馈一路的速度估计有多脏 —— '调大就抖' 的物理来源\n"
        "    目标静止, 检测框噪声 sigma=1.5px。跟踪器的 Kalman 速度带宽由\n"
        "    k_vel 决定；v_est 的抖动乘上 ff 就是指头的抖动。120Hz 下\n"
        "    k_vel=0.42 的时间常数只有 1.4 帧 —— 那不是估计器, 是微分器")
    print(f"\n{'k_vel':>7}{'tau(帧)':>8} | {'ff=1 抖动':>11}{'ff=2':>9}{'ff=4':>9}"
          f"{'ff=6':>9}   (px/step 标准差)")
    print("-" * 66)
    for kv in (0.42, 0.20, 0.10, 0.06, 0.04, 0.02):
        row = []
        for g in (1.0, 2.0, 4.0, 6.0):
            rng = random.Random(11)
            view = 0.0
            err = deque([0.0] * 4, maxlen=4)
            v_est = 0.0
            r_prev = None
            ffs = []
            for n in range(1800):
                e_meas = err[0]
                r_meas = 0.0 + rng.gauss(0.0, 1.5)
                if r_prev is None:
                    r_prev = r_meas
                v_est += kv * ((r_meas - r_prev) - v_est)
                r_prev = r_meas
                ff = max(-K_OUT, min(K_OUT, v_est * g))
                if n > 600:
                    ffs.append(ff)
                view += 0.5 * (0.1 * e_meas)
                err.append(-view)
            m = sum(ffs) / len(ffs)
            sd = math.sqrt(sum((x - m) ** 2 for x in ffs) / len(ffs))
            row.append(sd)
        tau = (1.0 - kv) / kv
        print(f"{kv:>7.2f}{tau:>8.1f} | {row[0]:>11.3f}{row[1]:>9.3f}"
              f"{row[2]:>9.3f}{row[3]:>9.3f}")


if __name__ == "__main__":
    t1()
    t2()
    t3()
    t4()
    t5()
    t6()
    t7()
    t8()
    print()
