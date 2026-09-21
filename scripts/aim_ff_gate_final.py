#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_ff_gate_final.py — 最后一组选型：估计器带宽 FF_Q × 门控方式 × 测速带宽。

约束（三条必须同时成立）：
    横移稳态误差  —— 目标匀速走，必须贴住（用户的"根本跟不上枪"）
    jink 误差     —— 目标来回拉锯，必须跟得上（代价项，估计器天生是直流量）
    静止抖动      —— 目标不动，指头必须安静（用户的"抖"）
    （横移与静止是同一台秤的两端：门控的作用就是在这里做取舍）

测速带宽 k_vel 一并扫，因为门控要用到"目标在不在动"，而这个判断的干净程度
完全由它决定 —— 设备上它就是 tracker 的 processNoise/measNoise 之比。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aim_v5_verify as M          # noqa: E402


def run(q, gate, kv, target, alpha, **kw):
    M.FF_Q = q
    M.GATE_MODE = gate
    kw.setdefault("k_vel", kv)
    return M.stats(M.sim(M.PPIDv5(), target, alpha, **kw),
                   tail=kw.get("tail", 600))


print("\n" + "=" * 88)
print("选型矩阵   横移1200px/s(mean_e) / jink±150@0.7Hz(rms) / 静止(sigma=1.5 输出rms)")
print("           第四轮默认(ff=1.0) = 横移 6.3 / jink 72.2 / 静止 ~0.43")
print("           第四轮手调最优(ff=1/α)= 横移 ~0.0 / jink 14.8 / 静止 ~1.29")
print("=" * 88)
print(f"\n{'k_vel':>6}{'q':>7}{'门控':>7} | {'横移1200':>10}{'trim':>6}"
      f" | {'jink rms':>9}{'jerk':>7} | {'静止抖动':>9}{'jerk':>7}")
print("-" * 88)

for kv in (0.42, 0.06):
    for q in (0.05, 0.10, 0.20):
        for gate in ("open", "vel"):
            st = run(q, gate, kv, M.strafe(1200.0), 0.3, steps=1800, tail=900)
            jk = run(q, gate, kv, M.tri(150.0, 0.7), 0.5, steps=1800, tail=900)
            nz = run(q, gate, kv, (lambda n: 0.0), 0.5, sigma=1.5,
                     steps=1200, tail=600)
            print(f"{kv:>6.2f}{q:>7.2f}{gate:>7} | {st['mean_e']:>10.1f}"
                  f"{st['trim']:>6.1f} | {jk['rms']:>9.1f}{jk['jerk']:>7.2f}"
                  f" | {nz['rms']:>9.3f}{nz['jerk']:>7.3f}")
        print()

print("=" * 88)
print("多动作平价：入选组合 vs 第四轮两种设置")
print("=" * 88)
M.FF_Q, M.GATE_MODE = 0.20, "vel"
print(f"\n{'动作':>20} | {'第四轮1.0':>11}{'jerk':>7} | {'第四轮1/α':>11}{'jerk':>7}"
      f" | {'第五 q=.2 vel':>14}{'jerk':>7}")
print("-" * 88)
cases = [("横移 300 px/s", M.strafe(300.0), 0.3),
         ("横移 600 px/s", M.strafe(600.0), 0.3),
         ("横移 1200 px/s", M.strafe(1200.0), 0.3),
         ("横移 2000 px/s", M.strafe(2000.0), 0.3),
         ("往复 ±120 @1.0Hz", M.tri(120.0, 1.0), 0.5),
         ("往复 ±200 @0.6Hz", M.tri(200.0, 0.6), 0.5),
         ("往复 ±80 @1.5Hz", M.tri(80.0, 1.5), 0.5)]
for name, tgt, alpha in cases:
    cells = []
    for g in (1.0, 1.0 / alpha):
        s = M.stats(M.sim_old(M.V.PPID(0.10, 1.0, 0.35, use_lock=False), tgt,
                              alpha, g, steps=1800), tail=900)
        cells.append((s["rms"], s["jerk"]))
    s = run(0.20, "vel", 0.06, tgt, alpha, steps=1800, tail=900)
    print(f"{name:>20} | {cells[0][0]:>11.1f}{cells[0][1]:>7.2f}"
          f" | {cells[1][0]:>11.1f}{cells[1][1]:>7.2f}"
          f" | {s['rms']:>14.1f}{s['jerk']:>7.2f}")
print()
