#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_ff_q_sweep.py — 自校准估计器的带宽（FF_Q）该取多少？

背景：估计器的输入是 (Δe + u_prev)，它的固有含义是"让误差停止变化所需的指头
位移"。对一个【速度恒定】的目标，这个量有唯一确定的值 = v/alpha，估计器收敛
到它 —— 所以 A 段（横移）在 alpha=0.15 也能零误差（trim≈0.5 说明积分几乎没
干活，全靠前馈）。

但对一个【零均值往复】的目标（jink），这个量的周期平均是 0 —— 估计器就"看不
见"目标在飞。B 段里 ff 只剩 0.4 就是这个原因。补它的唯一办法是让估计器更快，
代价是它开始吃检测噪声。

本脚本扫 FF_Q（1 维 Kalman 的过程噪声），同时量三件事：
    横移稳态误差   —— 慢了就抓不住（但 q=0.01 已经够）
    jink 误差      —— 快了才跟得上
    静止时的输出抖动 —— 快了就抖
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aim_v5_verify as M          # noqa: E402

HZ, DT = M.HZ, M.DT


def row(q, mode, target, **kw):
    M.FF_Q = q
    M.GATE_MODE = mode
    c = M.PPIDv5()
    rec = M.sim(c, target, kw.pop("alpha"), **kw)
    return M.stats(rec, tail=kw.get("tail", 600))


def hdr(t):
    print("\n" + "=" * 78)
    print(t)
    print("=" * 78)


print("\n" + "=" * 78)
print("估计器带宽 FF_Q —— 三件事必须同时成立")
print("  q 决定 1 维 Kalman 的稳态增益 kf，等效时间常数 tau=(1-kf)/kf 帧")
print("=" * 78)
print(f"\n{'q':>7}{'kf':>7}{'tau(帧)':>8} | {'横移600':>9}{'trim':>7}"
      f" | {'jink rms':>9}{'jerk':>7} | {'静止抖动':>9}{'jerk':>7}")
print("-" * 78)

for q in (0.005, 0.010, 0.020, 0.050, 0.100, 0.200, 0.400):
    # 稳态 1 维 Kalman 增益
    fp = 0.0001
    for _ in range(400):
        fp += q
        kf = fp / (fp + M.FF_R)
        fp = (1.0 - kf) * fp
    tau = (1.0 - kf) / kf if kf > 0 else 1e9

    st = row(q, "open", M.strafe(600.0), alpha=0.3)
    jk = row(q, "open", M.tri(150.0, 0.7), alpha=0.5, steps=1800, tail=900)
    nz = row(q, "open", (lambda n: 0.0), alpha=0.5, sigma=1.5, steps=1200,
             tail=600)
    print(f"{q:>7.3f}{kf:>7.3f}{tau:>8.1f} | {st['mean_e']:>9.1f}{st['trim']:>7.1f}"
          f" | {jk['rms']:>9.1f}{jk['jerk']:>7.2f}"
          f" | {nz['rms']:>9.3f}{nz['jerk']:>7.3f}")

print()
print("=" * 78)
print("参考点：第四轮（吃 Kalman 速度 τ=1.4 帧 的微分器）")
print("=" * 78)
o = M.stats(M.sim_old(M.V.PPID(0.10, 1.0, 0.35, use_lock=False),
                      M.tri(150.0, 0.7), 0.5, ff_gain=2.0, steps=1800), tail=900)
print(f"  jink, ff=1/α=2.0 : rms {o['rms']:.1f}  jerk {o['jerk']:.2f}")
o = M.stats(M.sim_old(M.V.PPID(0.10, 1.0, 0.35, use_lock=False),
                      M.tri(150.0, 0.7), 0.5, ff_gain=1.0, steps=1800), tail=900)
print(f"  jink, ff=1.0(默认): rms {o['rms']:.1f}  jerk {o['jerk']:.2f}")

print()
print("=" * 78)
print("多动作平价：q=0.02~0.05（开环门）vs 第四轮默认(ff=1.0)")
print("=" * 78)
print(f"\n{'动作':>22} | {'第四轮 ff=1.0':>16}{'jerk':>8}"
      f" | {'第五 q=0.02':>16}{'jerk':>8} | {'第五 q=0.05':>16}{'jerk':>8}")
print("-" * 92)
cases = [
    ("横移 300 px/s",   M.strafe(300.0), 0.3),
    ("横移 600 px/s",   M.strafe(600.0), 0.3),
    ("横移 1200 px/s",  M.strafe(1200.0), 0.3),
    ("正弦 ±120 @1.0Hz", M.tri(120.0, 1.0), 0.5),
    ("正弦 ±200 @0.6Hz", M.tri(200.0, 0.6), 0.5),
    ("正弦 ±80 @1.5Hz",  M.tri(80.0, 1.5), 0.5),
]
for name, tgt, alpha in cases:
    o = M.stats(M.sim_old(M.V.PPID(0.10, 1.0, 0.35, use_lock=False), tgt,
                          alpha, 1.0, steps=1800), tail=900)
    cells = []
    for q in (0.02, 0.05):
        s = row(q, "open", tgt, alpha=alpha, steps=1800, tail=900)
        cells.append((s["rms"], s["jerk"]))
    print(f"{name:>22} | {o['rms']:>16.1f}{o['jerk']:>8.2f}"
          f" | {cells[0][0]:>16.1f}{cells[0][1]:>8.2f}"
          f" | {cells[1][0]:>16.1f}{cells[1][1]:>8.2f}")
print()
