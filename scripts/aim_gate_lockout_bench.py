#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_gate_lockout_bench.py — 第十二轮：门控「闭锁死结」专项判决台

★ 本文件同时修掉 aim_selftrap_bench.py 里 "stop" 场景的一个量纲 bug：
      T = (speed/HZ) * min(t, tmove)      ← t 是秒，等于把"每步速度"乘了秒
  600px/s 的目标在 1.5s 里只走了 600/120/120*1.5 ≈ 0.06px —— **等于静止目标**。
  正确的写法是 T = (speed/HZ) * min(k, tmove*HZ)。第十一轮据此报出的
  "停住过冲 3~6px / ki=2.0 只剩 7.5px" 因此全部是在**几乎不动的目标**上测的，
  数值不可用。

用户现场（灵敏度 70，低 alpha）：
    kp=0.06  ki=0.1  kd=0.15  outSmooth=1  delay=0  kf=0.08  predict=1
    症状 A：目标停下会左右抖几下
    症状 B：跟不上了

根因链（本脚本逐条验证）：
  1. 前馈对"目标速度"的等效增益不是 kf，而是 kf/alpha_hat；
     需要的 DC 载波是 w/alpha。⇒ FF 供载波比例 = kf·alpha/alpha_hat ≈ 0.8·kf
     ⇒ 滑块上限 0.20 也只供 25%（kf=0.08 只供 10%）。**"kf 跟不上枪"字面成立。**
  2. 剩下的载波只能由积分扛。低 alpha 下需要 e = w/(alpha·kp) 才能由 P 单独提供：
     alpha=0.1 ⇒ 833px。
  3. 而门限 = 1.5·框宽 ≈ 96~450px。⇒ 833 > 门限 ⇒ 门控判"未稳定" ⇒
     **每帧清零积分** ⇒ 积分永远建不起来 ⇒ 死结。这才是"跟不上"。
  4. 门控是**闭锁**：settled 一旦 false，只有 |e|<tiny 或 |Δe|<2px 连续 2 帧能复位；
     匀速横移时 |Δe|≈w=5px/步，后者永远不成立；而 |e| 又永远 ≥ 门限 ⇒ 永久死结。
  5. 症状 A：门关着时 ffVel 不更新（冻结），目标停下后 |Δe| 变小 ⇒ 门开 ⇒
     前馈带着**过期的速度**回来推一把 ⇒ 过冲 ⇒ 反向 ⇒ 抖几下。
"""
import math
import random

HZ = 120.0
DT = 1.0 / HZ
TAU_D, RAMP, TAU_FF = 0.025, 0.30, 0.033
L = 5
BOUND = 2 * math.sin(math.pi / (2 * (2 * L + 1)))

U_KP, U_KI, U_KD, U_KF = 0.06, 0.10, 0.15, 0.08
KOUT, TRIM = 180.0, 90.0
GATE_K, TINY_FRAC, STAB_TOL, STAB_NEED = 1.5, 0.06, 2.0, 2


class Ctrl:
    """忠实复刻 pid_controller.h（v11），可切换四件事做对照：
      ff_gated : 前馈是否也受门控管辖（现状 True）
      i_guard  : |e|>=门限 时积分 "zero"(现状) / "freeze" / "none"
      stab_mode: 稳定判据 "abs"=|Δe|<2px(现状) / "rel"=|Δe|<max(2px,½w)
      ff_stale : 门关着时前馈状态是否冻结（现状 True）
    """

    def __init__(self, kp, ki, kd, kf, alpha_hat, gate_k=GATE_K, box_px=0.0,
                 ff_gated=True, i_guard="zero", stab_mode="abs", ff_stale=True):
        self.kp, self.ki, self.kd, self.kf = kp, ki, kd, kf
        self.ah = alpha_hat
        self.gate_k, self.box_px = gate_k, box_px
        self.ff_gated, self.i_guard, self.stab_mode, self.ff_stale = \
            ff_gated, i_guard, stab_mode, ff_stale
        self.reset()

    def reset(self):
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.fv = 0.0
        self.hist = [0.0] * L
        self.stab_n = 0
        self.settled = True
        self.gate_shut = 0
        self.n = 0

    def update(self, e, w_meas=0.0):
        self.n += 1
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last
        a = DT / (DT + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-KOUT, min(KOUT, self.kd * self.ramp * self.d))
        p = self.kp * self.ramp * e

        allow = True
        if self.box_px > 0.0:
            gate = self.gate_k * self.box_px
            tiny = TINY_FRAC * self.box_px
            ae = abs(e)
            if ae < tiny:
                self.settled, self.stab_n = True, 0
            elif ae >= gate:
                self.settled, self.stab_n = False, 0
                if self.i_guard == "zero":
                    self.I = 0.0
            else:
                ok = (abs(de) < STAB_TOL) if self.stab_mode == "abs" \
                    else (abs(de) < max(STAB_TOL, 0.5 * w_meas))
                self.stab_n = self.stab_n + 1 if ok else 0
                if self.stab_n >= STAB_NEED:
                    self.settled, self.stab_n = True, 0
            allow = self.settled
            if not allow:
                self.gate_shut += 1

        F = 0.0
        if self.kf > 0.0 and (allow or not self.ff_gated):
            if allow or not self.ff_stale:
                af = DT / (DT + TAU_FF)
                self.fv += af * ((de + self.ah * self.hist[0]) - self.fv)
            F = max(-KOUT, min(KOUT, self.kf * self.fv / self.ah))

        raw = p + self.I + dd + F
        if (allow or self.i_guard == "none") and \
           not (raw >= KOUT and e > 0) and not (raw <= -KOUT and e < 0):
            self.I += self.ki * e * DT
            self.I = max(-TRIM, min(TRIM, self.I))

        u = KOUT * math.tanh((p + self.I + dd + F) / KOUT)
        self.out = u
        self.hist.append(u)
        self.hist.pop(0)
        self.last = e
        return u


def run(c, alpha, scen, steps=2400, sigma=1.5, seed=7, speed=600.0, tmove=1.5):
    """stop 场景的量纲已修正：目标在 tmove 秒里真的走 speed*tmove 像素。"""
    rng = random.Random(seed)
    c.reset()
    X = 0.0
    q = [0.0] * L
    dprev, dvel, det = None, 0.0, 0.0
    err = []
    for k in range(steps):
        w = speed / HZ
        if scen == "static":
            T = 0.0
        elif scen == "strafe":
            T = w * k
        else:
            T = w * min(k, tmove * HZ)
        if k % 2 == 0:
            mm = T + rng.gauss(0.0, sigma)
            if dprev is not None:
                dvel = (mm - dprev) / 2.0
            dprev, det = mm, mm
        else:
            det += dvel
        e = det - X
        u = c.update(e, w)
        q.append(u)
        X += alpha * q.pop(0)
        err.append(T - X)
    return err


def stats(tr, tmove=1.5):
    """停在 tmove 秒。返回 (稳态滞后, 稳态摆幅, 瞬态峰值, 停住过冲, 抖几下)"""
    ne = int(tmove * HZ)
    seg = tr[ne - 60:ne]                       # 停前最后 0.5s
    lag = sum(seg) / len(seg)
    pp = max(seg) - min(seg)
    peak = max(abs(x) for x in tr[:ne])
    over = max(max(tr[ne:ne + int(2.5 * HZ)]), 0.0)
    tail = tr[ne:ne + int(1.5 * HZ)]
    sign = sum(1 for i in range(1, len(tail))
               if (tail[i - 1] < 0 <= tail[i]) or (tail[i - 1] > 0 >= tail[i]))
    return lag, pp, peak, over, sign


def hdr(s):
    print("\n" + "=" * 108)
    print(s)
    print("=" * 108)


if __name__ == "__main__":
    print("L=%d | 60/120Hz | sigma=1.5px | kout=%.0f trim=%.0f | P 环上界 kp·alpha<%.3f"
          % (L, KOUT, TRIM, BOUND))
    print("用户参数 kp=%.2f ki=%.2f kd=%.2f kf=%.2f | alpha_hat = 0.8×alpha"
          % (U_KP, U_KI, U_KD, U_KF))

    hdr("表0  为什么「kf 跟不上枪」是字面成立的：kf 供的载波比例")
    print("  需要的 DC 载波 = w/alpha。前馈 DC 增益 = kf/alpha_hat ⇒ 比例 = kf·alpha/alpha_hat"
          " ≈ 0.8·kf\n")
    print("  alpha | 需要载波@600px/s | kf=0.08 | 比例 | kf=0.20 | 比例 | kf=1.00 | 比例")
    for a in (0.10, 0.30, 1.00, 2.50):
        need = (600.0 / HZ) / a
        row = "  %5.2f |     %6.1f      |" % (a, need)
        for kf in (0.08, 0.20, 1.00):
            row += " %6.1f  | %4.0f%% |" % ((kf / (0.8 * a)) * (600.0 / HZ),
                                            100 * kf / (0.8 * a) * a)
        print(row)
    print("\n  ⇒ 比例与 alpha 无关，只由 kf 决定（=0.8·kf）。滑块上限 0.20 ⇒ **封顶 25%**。")
    print("  ⇒ 这就解释了「kf 拉满也不够」：kf 在这套结构里是『滞后修剪』，不是载波。\n")
    print("  剩下 75% 只能靠积分。而低 alpha 下 P 项单独扛载波所需误差：")
    print("  alpha | w/(alpha·kp) | 门限@框64 | @框150 | @框300")
    for a in (0.10, 0.30, 1.00, 2.50):
        print("  %5.2f |    %7.0f px  |   %4.0f    |  %4.0f  |  %4.0f"
              % (a, (600.0 / HZ) / (a * U_KP), 1.5 * 64, 1.5 * 150, 1.5 * 300))
    print("\n  ⇒ alpha=0.10 要 833px，门限只有 96~450 ⇒ **瞬态必然越过门限**。")

    hdr("表1  复现用户现场（量纲修正后）：门控开 vs 关，alpha=0.10，框宽150")
    print("  目标 600px/s 走 1.5s（共 900px）后停住\n")
    print("  配置                       | 稳态滞后 | 稳态摆幅 | 瞬态峰值 | 停住过冲 | 抖几下 | 门关占比")
    for tag, kw in (("门控开（现状）", dict(box_px=150.0)),
                    ("门控关（前馈+积分全开）", dict(box_px=0.0))):
        c = Ctrl(U_KP, U_KI, U_KD, U_KF, 0.8 * 0.10, **kw)
        tr = run(c, 0.10, "stop")
        lag, pp, peak, over, sign = stats(tr)
        print("  %-26s | %8.1f | %8.1f | %8.1f | %8.1f | %6d | %5.1f%%"
              % (tag, lag, pp, peak, over, sign, 100.0 * c.gate_shut / c.n))

    hdr("表2  闭锁诊断：门关帧占比 与 mean e / 3·yard（日志里能直接读的两个数）")
    print("  yard = 0.5·框宽，门限 = 1.5·框宽 = 3·yard。2.5s 匀速横移\n")
    print("  alpha | 框宽 | 门关占比 | 稳态 mean e | 3·yard | 断言")
    for a in (0.10, 0.30, 1.00):
        for bw in (64.0, 150.0, 300.0):
            c = Ctrl(U_KP, U_KI, U_KD, U_KF, 0.8 * a, box_px=bw)
            tr = run(c, a, "strafe", steps=300, tmove=99.0)
            lag = sum(tr[-60:]) / 60
            print("  %5.2f | %5.0f |  %5.1f%%  |   %8.1f  | %6.0f | %s"
                  % (a, bw, 100.0 * c.gate_shut / c.n, lag, 3 * bw / 2,
                     "★死结：mean e > 3·yard" if lag > 3 * bw / 2 else "门开着"))

    hdr("表3  门控的四个开关逐个拆（alpha=0.10，框宽150，600px/s 停 1.5s）")
    print("  前馈受门控 | 积分处理 | 稳定判据 | 前馈冻结 | 稳态滞后 | 停住过冲 | 抖几下")
    combos = [("现状", dict(ff_gated=True, i_guard="zero", stab_mode="abs", ff_stale=True)),
              ("前馈不受门控", dict(ff_gated=False, i_guard="zero", stab_mode="abs", ff_stale=True)),
              ("＋积分不清零", dict(ff_gated=False, i_guard="freeze", stab_mode="abs", ff_stale=True)),
              ("＋积分完全不限", dict(ff_gated=False, i_guard="none", stab_mode="abs", ff_stale=True)),
              ("＋稳定判据相对速度", dict(ff_gated=False, i_guard="none", stab_mode="rel", ff_stale=True)),
              ("＋门关时前馈不冻结", dict(ff_gated=False, i_guard="none", stab_mode="rel", ff_stale=False))]
    for tag, kw in combos:
        c = Ctrl(U_KP, U_KI, U_KD, U_KF, 0.8 * 0.10, box_px=150.0, **kw)
        tr = run(c, 0.10, "stop")
        lag, pp, peak, over, sign = stats(tr)
        print("  %-20s | %8.1f | %8.1f | %6d" % (tag, lag, over, sign))

    hdr("表4  停住过冲到底是谁造成的：逐项关掉（alpha=0.10，框宽150）")
    print("  稳态滞后 | 停住过冲 | 抖几下 | 说明")
    for tag, kf, ki, in (("kf=0.08 ki=0.10（现状）", 0.08, 0.10),
                         ("kf=0.00 ki=0.10", 0.00, 0.10),
                         ("kf=0.08 ki=0.00", 0.08, 0.00),
                         ("kf=0.00 ki=0.00", 0.00, 0.00),
                         ("kf=0.08 ki=0.30", 0.08, 0.30),
                         ("kf=0.20 ki=0.30", 0.20, 0.30),
                         ("kf=1.00 ki=0.30", 1.00, 0.30)):
        c = Ctrl(U_KP, ki, U_KD, kf, 0.8 * 0.10, box_px=150.0,
                 ff_gated=False, i_guard="none", stab_mode="rel")
        tr = run(c, 0.10, "stop")
        lag, pp, peak, over, sign = stats(tr)
        print("  %-24s | %8.1f | %8.1f | %6d" % (tag, lag, over, sign))

    hdr("表5  修正后跨 alpha 鲁棒性（门控仍在，但前馈不受它管、积分不清零、判据相对）")
    print("  kp=0.06 ki=0.10 kd=0.15 kf=0.08 框宽150\n")
    print("  alpha | 稳态滞后 | 稳态摆幅 | 停住过冲 | 抖几下 |        旧（现状全部）")
    for a in (0.05, 0.10, 0.20, 0.50, 1.00, 2.50):
        c1 = Ctrl(U_KP, U_KI, U_KD, U_KF, 0.8 * a, box_px=150.0)
        s1 = stats(run(c1, a, "stop"))
        c2 = Ctrl(U_KP, U_KI, U_KD, U_KF, 0.8 * a, box_px=150.0,
                  ff_gated=False, i_guard="none", stab_mode="rel")
        s2 = stats(run(c2, a, "stop"))
        print("  %5.2f | %8.1f | %8.1f | %8.1f | %6d | %7.1f / %7.1f / %d"
              % (a, s2[0], s2[1], s2[3], s2[4], s1[0], s1[3], s1[4]))

    hdr("表6  kf 上限该给到多少（修正后，alpha=0.10，ki=0.10，框宽150）")
    print("  kf   | 稳态滞后 | 停住过冲 | 抖几下 | 前馈供载波比例")
    for kf in (0.00, 0.08, 0.20, 0.40, 0.60, 0.80, 1.00, 1.50):
        c = Ctrl(U_KP, U_KI, U_KD, kf, 0.8 * 0.10, box_px=150.0,
                 ff_gated=False, i_guard="none", stab_mode="rel")
        tr = run(c, 0.10, "stop")
        lag, pp, peak, over, sign = stats(tr)
        print("  %4.2f | %8.1f | %8.1f | %6d |    %4.0f%%" % (kf, lag, over, sign, 100 * kf))

    # ── 表7/表8：出厂结构（前馈不受门控 + 积分仍被门控清零 + kf 默认 1.00） ────
    SHIP = dict(ff_gated=False, i_guard="zero", stab_mode="abs")

    hdr("表7  ★出厂结构下 kf 的扫描（前馈不受门控、积分仍清零、稳定判据照旧）")
    print("  kp=0.06 ki=0.10 kd=0.15 框宽150，目标 600px/s 走 2.5s 后停\n")
    print("  kf   | alpha=0.10：滞后/摆幅/过冲/抖 | alpha=0.30：滞后/过冲 | alpha=1.00：滞后/过冲")
    for kf in (0.08, 0.20, 0.40, 0.60, 0.80, 1.00, 1.20, 1.50, 2.00):
        row = "  %4.2f |" % kf
        for a in (0.10, 0.30, 1.00):
            c = Ctrl(U_KP, U_KI, U_KD, kf, 0.8 * a, box_px=150.0, **SHIP)
            s = stats(run(c, a, "stop", steps=2400, tmove=2.5), tmove=2.5)
            row += " %6.0f/%5.0f/%5.0f/%d |" % (s[0], s[1], s[3], s[4])
        print(row)
    print("\n  ⇒ kf=1.00 是三档 alpha 共同的甜点；0.20 以下（旧滑条全量程）全部滞后数百 px。")

    hdr("表8  代价：前馈的噪声增益也是 kf/alpha_hat ⇒ 静止目标的抖动")
    print("  检测 sigma=1.5px（轨迹器之前；实际误差用的是轨迹器平滑后的位置，会更小）\n")
    print("  kf   | alpha=0.10 噪声增益 | 静止抖动pp | alpha=0.50 噪声增益 | 静止抖动pp")
    for kf in (0.00, 0.08, 0.20, 0.40, 0.80, 1.00, 1.50):
        out = []
        for a in (0.10, 0.50):
            c = Ctrl(U_KP, U_KI, U_KD, kf, 0.8 * a, box_px=150.0, **SHIP)
            tr = run(c, a, "static", steps=1200)
            out.append((kf / (0.8 * a), max(tr[-240:]) - min(tr[-240:])))
        print("  %4.2f |       %5.1f x       |   %7.1f   |      %5.1f x        |   %7.1f"
              % (kf, out[0][0], out[0][1], out[1][0], out[1][1]))
    print("\n  ⇒ kf=1.00 换来 ~5px 静止抖动（sigma=1.5px 下）。要更平滑就加长 kFfTauSec，"
          "不要降 kf —— 降 kf 是把跟踪还回去。")

