#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_screenvel_check.py — 先把 tracker 速度的物理含义钉死，再做设计。

★ 本文件是**当前设备代码**（`tracking/pid_controller.h`，第八轮：前馈已删除）
  的判决台。`Ctl` 的默认参数就是出厂值，表 4 的 F2 / 表 5 的 G=0.10 就是终选。
  `aim_v5_verify.py` / `aim_alpha_fix.py` / `aim_ff_*.py` 里的 PPIDv5 是**上一轮
  的历史**（含已删除的前馈），只作对照，不要再照着它们改 C++。

发现：`bestVelX = track.vx`，而 tracker 的观测量是**屏幕上的框位置**。视野一转，
所有框一起在屏上动 —— 所以 tracker 报的速度里含我们自己的输出：

    ds = ΔT_world − alpha·u        （ds = 框的屏幕速度，u = 我们的手指位移）

后果有三，前两个推翻了上一轮的设计假设：
  ① `vel` 门控：瞄得越好，ds→0，门反而关上 ⇒ 自激开关。
  ② alpha 估计：Δe ≡ ds（准星固定在屏幕中心，误差就是框的屏上坐标），
     而 tracker 速度 = LPF(ds) ⇒ (tv − Δe) ≈ 0 ⇒ 除不出 alpha。
     ★ 所以环路增益 kp·alpha 只能由用户给（滑块 灵敏度补偿）。
  ③ 于是只剩"固定增益 + 用户补偿"一条路，本文件给出它的实测判决。

另：leash(`kTrimLimitPx`) 是 **56 而非 48**。48 在 alpha=0.30 / 1500px/s 下把积分
钉死在 48.00，留下 34px 永久静差 —— 因为积分要提供的不是 v/(120·alpha)，而是
经 tanh 输出上限压缩后**仍能给出** v/(120·alpha) 的那个原始值，低 alpha 端高约 1/3。
加长到 56 的代价实测为零（800px 阶跃 / 中途急停的过冲在 48~72 逐位相同，因为有
饱和判据在挡 wind-up，leash 只是余量）。
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
    """无前馈版：P + I(leash) + D，可选 alpha 归一（用于证明它做不到）。"""

    def __init__(self, kp=0.10, ki=0.5, kd=0.20, trim=K_TRIM, inv_mode="none"):
        self.kp, self.ki, self.kd, self.trim = kp, ki, kd, trim
        self.inv_mode = inv_mode          # none | est | oracle
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.ah = 1.0
        self.alpha_true = 1.0
        self.dead = 0.0

    def update(self, e, dt, box=0.0, short=1080.0, tv=0.0, screenv=0.0):
        if not math.isfinite(e) or dt <= 0:
            return 0.0
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last

        inv = 1.0
        if self.inv_mode == "oracle":
            self.ah = self.alpha_true
            inv = 1.0 / max(self.ah, 0.05)
        elif self.inv_mode == "est":
            # 用【世界速度】估计（上一轮 sim 的错误假设）——只为对照
            if abs(self.out) > 1.5:
                a = (tv - de) / self.out
                if 0.05 < a < 6.0:
                    self.ah += 0.02 * (a - self.ah)
            inv = 1.0 / max(self.ah, 0.05)
        elif self.inv_mode == "est_screen":
            # 用【屏幕速度】估计（真实可用信号）——看它是否退化成 0
            if abs(self.out) > 1.5:
                a = (screenv - de) / self.out
                if 0.05 < a < 6.0:
                    self.ah += 0.02 * (a - self.ah)
            inv = 1.0 / max(self.ah, 0.05)
            self._ascreen = (screenv - de) / self.out if abs(self.out) > 1.5 else 0.0

        a = dt / (dt + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-K_OUT, min(K_OUT, self.kd * inv * self.ramp * self.d))
        p = self.kp * inv * self.ramp * e
        raw = p + self.I + dd
        if not (raw >= K_OUT and e > 0) and not (raw <= -K_OUT and e < 0):
            self.I += self.ki * inv * e * dt
            self.I = max(-self.trim, min(self.trim, self.I))
        u = K_OUT * math.tanh((p + self.I + dd) / K_OUT)
        self.out = u
        self.last = e
        return u


def sim(ctrl, tgt, alpha, delay=3, sigma=1.5, seed=7, steps=1800, box=100.0,
        short=1080.0, k_vel=0.42):
    """真实模型：tracker 观测的是**屏幕**位置（= 目标世界位置 − 视野）。"""
    ctrl.alpha_true = alpha
    rng = random.Random(seed)
    view = 0.0
    buf = deque([0.0] * (delay + 1), maxlen=delay + 1)
    tv, prev_s = 0.0, None
    rec = {k: [] for k in ("glow", "u", "trim", "screenv", "ah")}
    for n in range(steps):
        screen_now = tgt(n) - view + rng.gauss(0.0, sigma)   # 检测器看到的东西
        glow = tgt(n) - view             # ★ 与 screen_now 同一时刻的视野状态
        if prev_s is None:
            prev_s = screen_now
        ds = screen_now - prev_s
        tv += k_vel * (ds - tv)
        prev_s = screen_now
        u = ctrl.update(buf[0], DT, box=box, short=short, tv=tv, screenv=tv)
        view += alpha * u
        buf.append(screen_now)
        rec["glow"].append(glow)
        rec["u"].append(u)
        rec["trim"].append(ctrl.I)
        rec["screenv"].append(tv)
        rec["ah"].append(ctrl.ah)
    return rec


def stats(rec, tail=900):
    w, u = rec["glow"][-tail:], rec["u"][-tail:]
    n = len(w)
    mu = sum(u) / n
    return dict(mean=sum(w) / n, rms=math.sqrt(sum(x * x for x in w) / n),
                pp=max(w) - min(w),
                jerk=math.sqrt(sum((x - mu) ** 2 for x in u) / n),
                trim=sum(rec["trim"][-tail:]) / n,
                ah=sum(rec["ah"][-tail:]) / n)


def strafe(s): return lambda n: s * n * DT
def tri(a, p): return lambda n: a * (4.0 * abs(((n * DT / p) % 1.0) - 0.5) - 1.0)


def hdr(t):
    print("\n" + "=" * 92)
    print(t)
    print("=" * 92)


def main():
    hdr("1  确认：alpha 估计在真实（屏幕速度）模型下死掉")
    print(f"{'alpha':>7} | {'世界速度估计(上一轮假设)':>26} | {'屏幕速度估计(真实可用)':>26}")
    print(f"{'':>7} | {'a_hat':>12}{'jink_rms':>14} | {'a_hat':>12}{'jink_rms':>14}")
    print("-" * 92)
    for a in (0.5, 1.0, 1.6, 2.5):
        s1 = stats(sim(Ctl(kp=0.25, ki=1.0, kd=0.6, inv_mode="est"),
                       tri(150.0, 0.7), a))
        s2 = stats(sim(Ctl(kp=0.25, ki=1.0, kd=0.6, inv_mode="est_screen"),
                       tri(150.0, 0.7), a))
        print(f"{a:>7.2f} | {s1['ah']:>12.3f}{s1['rms']:>14.1f} | "
              f"{s2['ah']:>12.3f}{s2['rms']:>14.1f}")

    hdr("2  确认：tracker 屏幕速度在锁好时确实趋近 0（vel 门控为何是错的）")
    for a in (0.5, 2.5):
        rec = sim(Ctl(kp=0.25, ki=1.0, kd=0.6), strafe(600.0), a)
        sv = rec["screenv"][-900:]
        print(f"  alpha={a}: |屏幕速度| 平均 {sum(abs(x) for x in sv)/len(sv):7.2f} px/步"
              f"  (= {sum(abs(x) for x in sv)/len(sv)*HZ:7.1f} px/s)"
              f"  —— 门限 150px/s 意味着门基本是关的")

    hdr("3  无前馈：横扫 (kp, ki, kd) 网格，看固定增益能覆盖到哪")
    alphas = (0.5, 1.0, 1.6, 2.5)
    rows = []
    for kp in (0.06, 0.10, 0.16, 0.25):
        for ki in (0.5, 1.0, 2.0, 4.0):
            for kd in (0.2, 0.4, 0.7):
                jr = []
                for a in alphas:
                    jr.append(stats(sim(Ctl(kp, ki, kd), tri(150.0, 0.7), a))["rms"])
                pk = []
                for a in (1.6, 2.5):
                    s = stats(sim(Ctl(kp, ki, kd), strafe(1500.0), a))
                    pk.append(max(s["pp"], 0.0))
                st = stats(sim(Ctl(kp, ki, kd), lambda n: 0.0, 1.0, sigma=3.0,
                               steps=1200), tail=600)
                hs = []
                for a in alphas:
                    hs.append(stats(sim(Ctl(kp, ki, kd), strafe(1500.0), a))["mean"])
                score = max(jr[1:]) + 0.05 * max(pk) + 8.0 * st["jerk"]
                rows.append((score, kp, ki, kd, jr, pk, st["jerk"], hs))
    rows.sort()
    print(f"{'score':>7}{'kp':>6}{'ki':>5}{'kd':>5} | "
          + "".join(f"{'jink@'+str(a):>11}" for a in alphas)
          + f"{'pp1.6':>8}{'pp2.5':>8}{'静止jerk':>10}{'max横扫滞后':>12}")
    print("-" * 92)
    for sc, kp, ki, kd, jr, pk, jk, hs in rows[:14]:
        print(f"{sc:>7.1f}{kp:>6.2f}{ki:>5.1f}{kd:>5.1f} | "
              + "".join(f"{v:>11.1f}" for v in jr)
              + f"{pk[0]:>8.1f}{pk[1]:>8.1f}{jk:>10.3f}{max(abs(x) for x in hs):>12.1f}")


def finalists():
    hdr("4  终选对比（无前馈，leash=56）——真实模型，全部 alpha")
    AL = (0.15, 0.30, 0.50, 1.00, 1.60, 2.50)
    fs = [
        ("F1 kp.10 ki1.0 kd.35", lambda: Ctl(0.10, 1.0, 0.35, trim=K_TRIM)),
        ("F2 kp.10 ki0.5 kd.20", lambda: Ctl(0.10, 0.5, 0.20, trim=K_TRIM)),
        ("F3 kp.06 ki2.0 kd.40", lambda: Ctl(0.06, 2.0, 0.40, trim=K_TRIM)),
        ("F4 kp.16 ki1.0 kd.35", lambda: Ctl(0.16, 1.0, 0.35, trim=K_TRIM)),
    ]
    print("\n-- a) jink ±150px@0.7Hz rms --")
    print(f"{'方案':>22} | " + "".join(f"{f'a={a}':>9}" for a in AL))
    print("-" * 92)
    for nm, mk in fs:
        print(f"{nm:>22} | " + "".join(
            f"{stats(sim(mk(), tri(150.0, 0.7), a))['rms']:>9.1f}" for a in AL))

    print("\n-- b) 1500px/s 横移 mean_e (pp) --")
    print(f"{'方案':>22} | " + "".join(f"{f'a={a}':>16}" for a in AL))
    print("-" * 92)
    for nm, mk in fs:
        cells = []
        for a in AL:
            s = stats(sim(mk(), strafe(1500.0), a))
            cells.append(f"{s['mean']:.0f}({s['pp']:.0f})")
        print(f"{nm:>22} | " + "".join(f"{c:>16}" for c in cells))

    print("\n-- c) 600px/s 横移 mean_e --")
    print(f"{'方案':>22} | " + "".join(f"{f'a={a}':>9}" for a in AL))
    print("-" * 92)
    for nm, mk in fs:
        print(f"{nm:>22} | " + "".join(
            f"{stats(sim(mk(), strafe(600.0), a))['mean']:>9.1f}" for a in AL))

    print("\n-- d) 253px 阶跃 / 静止(sigma=3 指头jerk) --")
    print(f"{'方案':>22} | {'到20':>6}{'到5':>6}{'到1':>6}{'越冲':>7}"
          f"{'jink@1.0':>10}{'静止jerk':>10}")
    print("-" * 92)
    for nm, mk in fs:
        rec = sim(mk(), lambda n: 250.0, 0.5, steps=900)
        g = [abs(x) for x in rec["glow"]]
        fr = []
        for thr in (20, 5, 1):
            i = next((k for k, v in enumerate(g) if v < thr), None)
            fr.append(f"{i:>6}" if i is not None else f"{'--':>6}")
        jk = stats(sim(mk(), tri(150.0, 0.7), 1.0))["rms"]
        st = stats(sim(mk(), lambda n: 0.0, 1.0, sigma=3.0, steps=1200),
                   tail=600)["jerk"]
        print(f"{nm:>22} | " + "".join(fr)
              + f"{min(rec['glow'][-120:]):>7.1f}"
              f"{jk:>10.1f}{st:>10.3f}")


def coupled():
    hdr("5  单旋钮：kp=G, ki=5G, kd=2G（就是 F2 那一行的比例）")
    AL = (0.30, 0.50, 1.00, 1.60, 2.50)
    print(f"{'G':>6} | " + "".join(f"{'jink@'+str(a):>10}" for a in AL)
          + f"{'横扫1.6(pp)':>13}{'到1':>6}{'越冲':>7}{'静止3':>8}{'jink1.0':>9}")
    print("-" * 92)
    for G in (0.06, 0.10, 0.16, 0.25, 0.40):
        mk = lambda: Ctl(G, 5.0 * G, 2.0 * G, trim=K_TRIM)
        jr = [stats(sim(mk(), tri(150.0, 0.7), a))["rms"] for a in AL]
        s16 = stats(sim(mk(), strafe(1500.0), 1.6))
        rec = sim(mk(), lambda n: 250.0, 0.5, steps=900)
        g = [abs(x) for x in rec["glow"]]
        i1 = next((k for k, v in enumerate(g) if v < 1.0), None)
        st = stats(sim(mk(), lambda n: 0.0, 1.0, sigma=3.0, steps=1200),
                   tail=600)["jerk"]
        print(f"{G:>6.2f} | " + "".join(f"{v:>10.1f}" for v in jr)
              + f"{s16['mean']:>7.0f}({s16['pp']:>4.0f})"
              + f"{str(i1):>6}{min(rec['glow'][-120:]):>7.1f}{st:>8.3f}"
              + f"{jr[2]:>9.1f}")


if __name__ == "__main__":
    main()
    finalists()
    coupled()
    print()
