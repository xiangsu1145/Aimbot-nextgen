#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_selftrap_bench.py — 「前馈自陷阱」专项判决台（第二版，量程已修正）

第一版把量程设错（alpha=2.5 时 kp=0.10 ⇒ kp·alpha=0.25，己经贴着延迟给的稳定上界
2sin(pi/(2(2L+1)))=0.285），所有结论都被"P 环自己在临界点"污染了。本版按
kp·alpha ≈ 0.08 选增益，让 P 环有裕度，才看得清前馈结构本身的好坏。

用户现场（灵敏度 70）：
  kf=0.05 能跟枪但左右乱晃；猜"拉小到 0.02 会好"；
  ki 一开就"目标停下之后过冲"；kd 感觉没刹车。

要回答：
  表1  现行公式 ff = kf·LPF(Δe + u_{k−L}/kf) 里那份"我们自己的延迟输出"增益是多少？
       它是不是一个 z=1 的积分极点（所以"能跟枪"和"乱晃"是同一件事）？
  表2  目标停下之后，**ki=0** 会不会过冲？ki 再叠上去放大多少？
  表3  几种前馈结构在干净量程下的对比（含参考实现的门控）。
  表4  门控（参考那把状态机）对 ki 过冲的作用，门限用目标框宽当单位。
  表5  在线辨识 alpha。
"""
import math
import random

HZ = 120.0
DT = 1.0 / HZ
TAU_D, RAMP = 0.025, 0.30
TAU_FF = 0.033
L = 5                      # 传输延迟步数（42ms）
BOUND = 2 * math.sin(math.pi / (2 * (2 * L + 1)))   # P 环稳定上界 kp·alpha


# ══════════════════════════════════════════════════════════════════════════════
#  统一控制器：不同前馈结构 + 参考实现的门控状态机
# ══════════════════════════════════════════════════════════════════════════════
class Ctrl:
    """
    form:
      "mul"     ff = kf·LPF( Δe + u_{k−L}/kf )        ← 现行 C++
      "dlead"   ff = kf·LPF( Δe )                      ← 零自洩，纯超前
      "carrier" ff = kf·LPF( u_{k−L} )                 ← 部分载波，自环增益 = kf
      "both"    ff = kf·LPF( u_{k−L} ) + kf·LPF( Δe )
      "exact"   ff = LPF( Δe + alpha·u_{k−L} )/alpha    ← 上帝视角（知道真 alpha）
    gate: 参考实现那套（门限 = gate_k · 目标框宽）
    """

    def __init__(self, kp, ki, kd, kout=180.0, trim=90.0, kf=0.0,
                 form="mul", gate_k=0.0, stab_need=2, stab_tol=2.0,
                 box_px=64.0, alpha_hat=0.0):
        self.kp, self.ki, self.kd, self.kout, self.trim, self.kf = \
            kp, ki, kd, kout, trim, kf
        self.form, self.gate_k, self.stab_need = form, gate_k, stab_need
        self.stab_tol, self.box_px, self.alpha_hat = stab_tol, box_px, alpha_hat
        self.hist = [0.0] * L
        self.reset()

    def reset(self):
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = 0.0
        self.fv = 0.0              # LPF(Δe)
        self.fu = 0.0              # LPF(u_{k−L})
        self.hist = [0.0] * L
        self.settle_n = 0
        self.settled = False
        self.ff_on_steps = 0

    def update(self, e):
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last
        a = DT / (DT + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-self.kout, min(self.kout, self.kd * self.ramp * self.d))
        p = self.kp * self.ramp * e

        # ── 参考实现的门控：门限以「目标自身宽度」为单位 ─────────────────────
        allow = True
        if self.gate_k > 0.0 and self.box_px > 0.0:
            gate = self.gate_k * self.box_px
            tiny = 0.06 * self.box_px
            if abs(e) < tiny:
                self.settled, self.settle_n = True, 0
            elif abs(e) >= gate:
                self.settled, self.settle_n = False, 0
                self.I = 0.0                     # 硬清零（不是冻结）
            else:
                self.settle_n = self.settle_n + 1 if abs(de) < self.stab_tol else 0
                if self.settle_n >= self.stab_need:
                    self.settled, self.settle_n = True, 0
            allow = self.settled
        else:
            self.settled = True

        F = 0.0
        if self.kf > 0.0 and allow:
            af = DT / (DT + TAU_FF)
            uL = self.hist[0]
            if self.form == "mul":
                src = de + uL / self.kf
                self.fv += af * (src - self.fv)
                F = self.kf * self.fv
            elif self.form == "dlead":
                self.fv += af * (de - self.fv)
                F = self.kf * self.fv
            elif self.form == "carrier":
                self.fu += af * (uL - self.fu)
                F = self.kf * self.fu
            elif self.form == "both":
                self.fv += af * (de - self.fv)
                self.fu += af * (uL - self.fu)
                F = self.kf * (self.fv + self.fu)
            elif self.form == "exact":
                self.fv += af * ((de + self.alpha_hat * uL) - self.fv)
                F = self.fv / self.alpha_hat
            elif self.form == "gmul":
                # 把「重建常数 alpha_hat」与「前馈力度 kf」正式拆开：
                #   ff = (kf/alpha_hat)·LPF( Δe + alpha_hat·u_{k−L} )
                # kf=1 且 alpha_hat=alpha 时 = 理想速度前馈 w/alpha；
                # 自洩系数 = kf·(1 − alpha/alpha_hat)，欠估 ⇒ 负 ⇒ 阻尼。
                self.fv += af * ((de + self.alpha_hat * uL) - self.fv)
                F = self.kf * self.fv / self.alpha_hat
            F = max(-self.kout, min(self.kout, F))
            self.ff_on_steps += 1

        raw = p + self.I + dd + F
        if allow and not (raw >= self.kout and e > 0) and not (raw <= -self.kout and e < 0):
            self.I += self.ki * e * DT
            self.I = max(-self.trim, min(self.trim, self.I))

        u = self.kout * math.tanh((p + self.I + dd + F) / self.kout)
        self.out = u
        self.hist.append(u)
        self.hist.pop(0)
        self.last = e
        return u


# ══════════════════════════════════════════════════════════════════════════════
def run(ctrl, alpha, scen, steps=2400, sigma=1.5, seed=7,
        speed=600.0, tmove=1.5, box_px=64.0):
    rng = random.Random(seed)
    ctrl.reset()
    ctrl.box_px = box_px
    X = 0.0
    q = [0.0] * L
    dprev, dvel, det = None, 0.0, 0.0
    err = []
    for k in range(steps):
        t = k / HZ
        if scen == "strafe":
            T = (speed / HZ) * k
        elif scen == "stop":
            # ★★ 量纲修正（第十二轮）：这里原本写的是 (speed/HZ) * min(t, tmove)，
            # 而 t 是**秒**，等于把"每步速度"又乘了一次秒。600px/s 的目标在 1.5s
            # 里只走了 600/120/120*1.5 ≈ 0.06px，**等于静止目标**。
            # 表2/表4/表8 里所有"停住过冲 3~6px / ki=2.0 只剩 7.5px"都是这样测出来的，
            # 数值不可用。正确写法：min(k, tmove*HZ)。
            T = (speed / HZ) * min(k, tmove * HZ)
        elif scen == "step":
            T = 250.0
        else:
            T = 150.0 * math.sin(2 * math.pi * 0.7 * t)
        if k % 2 == 0:
            m = T + rng.gauss(0.0, sigma)
            if dprev is not None:
                dvel = (m - dprev) / 2.0
            dprev, det = m, m
        else:
            det += dvel
        e = det - X
        u = ctrl.update(e)
        q.append(u)
        X += alpha * q.pop(0)
        err.append(T - X)
    return err


def m(tr, tail=900):
    seg = tr[-tail:]
    return sum(seg) / len(seg), math.sqrt(sum(x * x for x in seg) / len(seg)), \
        max(seg) - min(seg)


def hover(tr, tmove=1.5):
    i0 = int(tmove * HZ) + 20
    return max(max(tr[i0:]), 0.0)


def hdr(s):
    print("\n" + "=" * 100)
    print(s)
    print("=" * 100)


def kp_for(alpha, target=0.08):
    return round(target / alpha, 3)


if __name__ == "__main__":
    print("L=%d 步延迟 | 60/120Hz | sigma=1.5px | kout=180 | P 环稳定上界 kp·alpha < %.3f"
          % (L, BOUND))
    print("量程：kp·alpha ≈ 0.08（P 环有 3.5 倍裕度），kd=0.20")

    hdr("表1  「能跟枪」和「乱晃」是不是同一件事？逐项拆开现行公式")
    print("  ff = kf·LPF(Δe) + LPF(u_{k−L})  ← 后一项系数恒为 1，与 kf 无关；")
    print("  它就是 z=1 的积分极点：既白送一个 DC 载波（所以跟得上），又是无阻尼漂移源。\n")
    a = 1.0
    print("  alpha=%.2f, kp=%.3f, ki=0, kd=0.20，600px/s 横移：" % (a, kp_for(a)))
    print("  kf 取值  | 自洩系数 1−kf·alpha | e(滞后) / pp(摆幅)")
    for kf in (0.02, 0.05, 0.1, 0.2, 0.5, 1.0):
        tr = run(Ctrl(kp_for(a), 0.0, 0.20, kf=kf, form="mul"), a, "strafe")
        sd, _, pp = m(tr)
        print("  %7.2f  |       %+.2f          | e=%6.1f pp=%5.0f" % (kf, 1 - kf * a, sd, pp))
    print("\n  对照：把自洩项整项拿掉（form=dlead，ff 只剩 kf·LPF(Δe)）")
    for kf in (0.02, 0.05, 0.1, 0.2, 0.5, 1.0):
        tr = run(Ctrl(kp_for(a), 0.0, 0.20, kf=kf, form="dlead"), a, "strafe")
        sd, _, pp = m(tr)
        print("  %7.2f  |          0 (无自洩)  | e=%6.1f pp=%5.0f" % (kf, sd, pp))

    hdr("表2  目标匀速 1.5s 后停住。ki=0 会不会过冲？（用户说的核心问题）")
    print("  alpha=1.0, kp=0.08, kd=0.20。过冲 = 停下后越过目标点的最大值(px)\n")
    print("  kf    | mul形式 ki=0 / 0.1 / 0.5 / 2.0   | dlead形式 ki=0 / 0.5")
    for kf in (0.05, 0.2, 1.0):
        row = "  %4.2f  |" % kf
        for ki in (0.0, 0.1, 0.5, 2.0):
            tr = run(Ctrl(kp_for(1.0), ki, 0.20, kf=kf, form="mul"), 1.0, "stop",
                     speed=400.0)
            row += " %7.1f" % hover(tr)
        for ki in (0.0, 0.5):
            tr = run(Ctrl(kp_for(1.0), ki, 0.20, kf=kf, form="dlead"), 1.0, "stop",
                     speed=400.0)
            row += " %7.1f" % hover(tr)
        print(row)
    print("\n  ⇒ ki=0 就有过冲 ⇒ 过冲的主力和 ki 无关，是前馈那份自洩（等价积分器）。")

    hdr("表3  几种前馈结构对比（600px/s 横移 / 停住过冲），门控关")
    print("  alpha=1.0, kp=0.08, kd=0.20, ki=0.5\n")
    print("  结构                        |   kf   |  e(滞后) |  pp(摆幅) |  停住过冲")
    for form, kf, tag in (("dlead", 0.30, "纯超前 kf·LPF(Δe)"),
                          ("dlead", 1.00, "纯超前 kf·LPF(Δe)"),
                          ("carrier", 0.60, "部分载波 kf·LPF(u)"),
                          ("carrier", 0.90, "部分载波 kf·LPF(u)"),
                          ("both", 0.60, "载波+超前"),
                          ("mul", 0.05, "现行 mul"),
                          ("exact", 1.00, "上帝视角（知真 alpha）")):
        c = Ctrl(kp_for(1.0), 0.5, 0.20, kf=kf, form=form,
                 alpha_hat=1.0 if form == "exact" else 0.0)
        sd, _, pp = m(run(c, 1.0, "strafe"))
        ho = hover(run(Ctrl(kp_for(1.0), 0.5, 0.20, kf=kf, form=form,
                            alpha_hat=1.0 if form == "exact" else 0.0),
                       1.0, "stop", speed=400.0))
        print("  %-26s | %5.2f | %7.1f | %7.0f | %8.1f" % (tag, kf, sd, pp, ho))

    hdr("表4  参考实现的尺寸化门控（门限 = k·目标框宽）加在上面最好的那个结构上")
    print("  目标 400px/s 1.5s 后停住。框宽 64px。\n")
    print("  结构        kf    gate  | e(滞后) | 停住过冲 | ki=2.0 时过冲")
    for form, kf in (("carrier", 0.90), ("both", 0.60), ("dlead", 1.0)):
        for gk in (0.0, 1.5, 3.0):
            tr = run(Ctrl(kp_for(1.0), 0.5, 0.20, kf=kf, form=form,
                          gate_k=gk, box_px=64.0), 1.0, "stop", speed=400.0)
            h5 = hover(tr)
            c2 = Ctrl(kp_for(1.0), 2.0, 0.20, kf=kf, form=form,
                      gate_k=gk, box_px=64.0)
            h2 = hover(run(c2, 1.0, "stop", speed=400.0))
            print("  %-10s %4.2f  %3.1f   | %7.1f | %8.1f | %10.1f" % (
                form, kf, gk, m(tr[:int(1.5 * HZ) - 60])[0], h5, h2))
    print("\n  ⇒ 门控把「逼近/停下瞬态」里的前馈与积分一起掐掉，过冲不再由 ki 放大。")

    hdr("表5  在线辨识 alpha：窗内最小二乘 e(k) = c0 + c1·k + c2·k² − alpha·U(k)")

    def ident(alpha, kf_used, N=90):
        c = Ctrl(kp_for(alpha), 0.0, 0.20, kf=kf_used, form="dlead")
        rng = random.Random(11)
        X, dprev, dvel, det = 0.0, None, 0.0, 0.0
        q = [0.0] * L
        U = 0.0
        buf = []
        out = []
        for k in range(2400):
            T = (600.0 / HZ) * k
            if k % 2 == 0:
                mm = T + rng.gauss(0.0, 1.5)
                if dprev is not None:
                    dvel = (mm - dprev) / 2.0
                dprev, det = mm, mm
            else:
                det += dvel
            e = det - X
            u = c.update(e)
            q.append(u)
            X += alpha * q.pop(0)
            U += q[0]
            buf.append((k, U, e))
            if len(buf) > N:
                buf.pop(0)
            if len(buf) == N and k % 20 == 0:
                A = [[1.0, kk, kk * kk, -uu] for kk, uu, _ in buf]
                y = [ee for _, _, ee in buf]
                M = [[sum(A[r][i] * A[r][j] for r in range(N)) for j in range(4)]
                     for i in range(4)]
                b = [sum(A[r][i] * y[r] for r in range(N)) for i in range(4)]
                try:
                    for i in range(4):
                        pv = max(range(i, 4), key=lambda r: abs(M[r][i]))
                        if abs(M[pv][i]) < 1e-9:
                            raise ZeroDivisionError
                        M[i], M[pv] = M[pv], M[i]
                        b[i], b[pv] = b[pv], b[i]
                        for r in range(i + 1, 4):
                            f = M[r][i] / M[i][i]
                            for cc in range(i, 4):
                                M[r][cc] -= f * M[i][cc]
                            b[r] -= f * b[i]
                    x = [0.0] * 4
                    for i in range(3, -1, -1):
                        x[i] = (b[i] - sum(M[i][j] * x[j] for j in range(i + 1, 4))) / M[i][i]
                    if 0.02 < x[3] < 8.0:
                        out.append(x[3])
                except ZeroDivisionError:
                    pass
        return out

    for a in (0.5, 1.0, 2.5, 5.0):
        g = ident(a, kp_for(a))
        if g:
            g2 = g[len(g) // 2:]
            avg = sum(g2) / len(g2)
            print("  真 alpha=%4.2f | 有效解 %3d 个 | 后半段 alpha_hat=%5.2f (误差 %+5.0f%%) "
                  "| 0.5x 用作重建常数 = %5.2f" % (a, len(g), avg, 100 * (avg / a - 1), 0.5 * avg))
        else:
            print("  真 alpha=%4.2f | 无解（激励不足/矩阵病态）" % a)

    hdr("表6  「不调 ki 也晃」是怎么来的？把 kp·alpha 从安全区推到延迟上界")
    print("  alpha=2.5 固定，ki=0, kd=0.20, kf=0.05（现场值）。上界 kp·alpha < %.3f\n" % BOUND)
    print("  kp     | kp·alpha | 600px/s 横移 e / pp")
    for kp in (0.02, 0.04, 0.06, 0.08, 0.10, 0.12):
        tr = run(Ctrl(kp, 0.0, 0.20, kf=0.05, form="mul"), 2.5, "strafe")
        sd, _, pp = m(tr)
        flag = "  ← 贴界" if kp * 2.5 > 0.8 * BOUND else ""
        print("  %5.3f |  %6.3f  | e=%6.1f pp=%6.0f%s" % (kp, kp * 2.5, sd, pp, flag))

    hdr("表7  候选新方案：ff = kf·LPF(Δe) + 门控 ki 独扛 DC 载波")
    print("  kf=1.0, ki=0.5, kd=0.20, 门限 = 1.5·框宽(64px)。跨 alpha、跨 kp·alpha 看鲁棒性\n")
    print("  alpha | kp·alpha | 安全区 kp | e(滞后) / pp(摆幅) / 停住过冲 | 贴界 kp | e / pp / 过冲")
    for a in (0.5, 1.0, 2.5, 5.0):
        ks = kp_for(a, 0.08)
        kn = kp_for(a, 0.22)
        c1 = Ctrl(ks, 0.5, 0.20, kf=1.0, form="dlead", gate_k=1.5, box_px=64.0)
        tr1 = run(c1, a, "strafe")
        s1, _, p1 = m(tr1)
        h1 = hover(run(Ctrl(ks, 0.5, 0.20, kf=1.0, form="dlead", gate_k=1.5,
                            box_px=64.0), a, "stop", speed=400.0))
        c2 = Ctrl(kn, 0.5, 0.20, kf=1.0, form="dlead", gate_k=1.5, box_px=64.0)
        tr2 = run(c2, a, "strafe")
        s2, _, p2 = m(tr2)
        h2 = hover(run(Ctrl(kn, 0.5, 0.20, kf=1.0, form="dlead", gate_k=1.5,
                            box_px=64.0), a, "stop", speed=400.0))
        print("  %5.2f |  %5.2f   |  %6.3f  | e=%6.1f pp=%5.0f 过冲%6.1f |  %6.3f  "
              "| e=%6.1f pp=%5.0f 过冲%6.1f" % (
                  a, 0.08, ks, s1, p1, h1, kn, s2, p2, h2))
    print("\n  对照：同样量程，现行 mul + kf=0.05（现场值）")
    print("  alpha | 安全区 kp | e / pp / 过冲        | 贴界 kp | e / pp / 过冲")
    for a in (0.5, 1.0, 2.5, 5.0):
        ks, kn = kp_for(a, 0.08), kp_for(a, 0.22)
        c1 = Ctrl(ks, 0.5, 0.20, kf=0.05, form="mul")
        tr1 = run(c1, a, "strafe")
        s1, _, p1 = m(tr1)
        h1 = hover(run(Ctrl(ks, 0.5, 0.20, kf=0.05, form="mul"), a, "stop", speed=400.0))
        c2 = Ctrl(kn, 0.5, 0.20, kf=0.05, form="mul")
        tr2 = run(c2, a, "strafe")
        s2, _, p2 = m(tr2)
        h2 = hover(run(Ctrl(kn, 0.5, 0.20, kf=0.05, form="mul"), a, "stop", speed=400.0))
        print("  %5.2f |  %6.3f  | e=%6.1f pp=%5.0f 过冲%6.1f |  %6.3f  "
              "| e=%6.1f pp=%5.0f 过冲%6.1f" % (a, ks, s1, p1, h1, kn, s2, p2, h2))

    hdr("表8  决定性扫描：ff = (kf/alpha_hat)·LPF( Δe + alpha_hat·u_{k−L} )")
    print("  自洩系数 = kf·(1 − alpha/alpha_hat)。欠估 alpha_hat ⇒ 负值 ⇒ 阻尼。")
    print("  ki=0.5 kd=0.20 kp·alpha=0.10 门控开(1.5·框宽)\n")
    print("  alpha | a_hat误差 | kf=0.20              | kf=0.50              | kf=1.00")
    for a in (0.5, 1.0, 2.5):
        for ratio in (0.4, 0.5, 0.7, 1.0, 1.5, 2.0):
            ah = a * ratio
            row = "  %5.2f |   %3.2fx   |" % (a, ratio)
            for kf in (0.20, 0.50, 1.00):
                def mk():
                    return Ctrl(kp_for(a, 0.10), 0.5, 0.20, kf=kf, form="gmul",
                                gate_k=1.5, box_px=64.0, alpha_hat=ah)
                tr = run(mk(), a, "strafe")
                sd, _, pp = m(tr)
                ho = hover(run(mk(), a, "stop", speed=400.0))
                row += " %13s" % ("e=%.0f pp=%.0f 过%.0f" % (sd, pp, ho))
            print(row + ("   ← 安全侧" if ratio <= 0.7 else ""))
    print("\n  自洩系数一览（kf=0.5）：")
    for a in (0.5, 1.0, 2.5):
        print("    alpha=%.1f : " % a + "  ".join(
            "%.1fx→%+.2f" % (r, 0.5 * (1 - a / (a * r))) for r in (0.5, 0.7, 1.0, 1.5)))
    print("\n  固定 alpha_hat=1.0（不辨识，纯拍）在各 alpha 下的等价误差倍率：")
    for a in (0.5, 1.0, 2.5, 5.0):
        print("    alpha=%.1f → alpha_hat/alpha = %.2fx, 自洩@kf=0.5 = %+.2f"
              % (a, 1.0 / a, 0.5 * (1 - a / 1.0)))
