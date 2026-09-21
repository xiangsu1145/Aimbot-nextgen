#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_pidf_bench.py — 纯 PIDF 判决台：**不做灵敏度缩放**，只留 kp/ki/kd/kf。

背景：第九轮把增益、输出上限、积分上限全部按 sensComp≈1/alpha 缩放，环路变成
alpha 无关（实测每列同一个数）。用户不接受那一层：他只要 PIDF 四个旋钮，不要
"灵敏度补偿" / "输出随 1/alpha 缩放" 这类二级参数。

本文件回答三件事（全部带 L 步传输延迟，这是上一版判决台缺的）：
  ① 固定输出上限该给多少？（不缩放 ⇒ 低 alpha 端可达画面速度 = alpha·kout·120）
  ② kf 的安全区间？（自洩 = 1 − kf·alpha，带延迟后是更高阶的环）
  ③ 固定上限下，kp=0.10 在哪个 alpha 以内还稳？（kp·alpha < 2sin(pi/(2(2L+1)))）

模型（与真机一致的三件事）：
  * 误差 = 目标屏上坐标（准星=屏幕中心）⇒ e_k = T_k − X_k
  * 执行器有 L 步传输延迟：X_{k+1} = X_k + alpha·u_{k-L}    ← 前两版判决台漏了
  * 检测 60Hz、控制 120Hz ⇒ 每隔一步才有新测量，间隔步靠 tracker 预测
"""
import math
import random

HZ = 120.0
DT = 1.0 / HZ
TAU_D, RAMP, TAU_FF = 0.025, 0.30, 0.033

ALPHAS = [0.05, 0.10, 0.20, 0.30, 0.50, 1.00, 2.00, 3.00]


class PIDF:
    """逐行镜像 app/src/main/cpp/tracking/pid_controller.h 的新版（无缩放）。

    ff_src 控制前馈拿"我们自己的输出"的哪一份：
      "prev"    : u_{k-1}（当前 C++ 的做法）
      "aligned" : u_{k-L}（L = 传输延迟步数，即真正产生了被观测位移的那一步）
    """

    def __init__(self, kp, ki, kd, kout, trim, kf, det_every=2,
                 L=4, ff_src="prev", ff_form="mul"):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.kout, self.trim, self.kf = kout, trim, kf
        self.det_every = det_every
        self.L, self.ff_src, self.ff_form = L, ff_src, ff_form
        self.I = self.last = self.out = 0.0
        self.d = self.ramp = self.ffv = 0.0
        self.hist = []          # 自己的输出历史，供 aligned 重建取用

    def reset(self):
        self.I = self.last = self.out = self.d = self.ramp = self.ffv = 0.0
        self.hist = []

    def update(self, e):
        self.ramp = min(1.0, self.ramp + RAMP)
        de = e - self.last
        a = DT / (DT + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-self.kout, min(self.kout, self.kd * self.ramp * self.d))
        p = self.kp * self.ramp * e

        F = 0.0
        if self.kf > 0.0:
            # alpha_hat = 1/kf —— 与增益同一个数，用两次
            if self.ff_src == "aligned":
                u_own = self.hist[-self.L] if len(self.hist) >= self.L else 0.0
            else:
                u_own = self.out
            af = DT / (DT + TAU_FF)
            if self.ff_form == "smith":
                # 恒等式 w = Δe + alpha·u_{k-L} 是对的，所以
                #     u_ss = w/alpha = u_{k-L} + Δe/alpha
                # u_{k-L} 是已知的无噪指令，不该被低通（低通它 = 白送相位滞后）；
                # 只有 Δe 是测量值，才需要滤。
                self.ffv += af * (de - self.ffv)
                F = u_own + self.kf * self.ffv
            else:
                dmeas = de + u_own / self.kf
                self.ffv += af * (dmeas - self.ffv)
                F = self.kf * self.ffv
            F = max(-self.kout, min(self.kout, F))

        raw = p + self.I + dd + F
        if not (raw >= self.kout and e > 0) and not (raw <= -self.kout and e < 0):
            self.I += self.ki * e * DT
            self.I = max(-self.trim, min(self.trim, self.I))

        u = self.kout * math.tanh((p + self.I + dd + F) / self.kout)
        self.out = u
        self.hist.append(u)
        if len(self.hist) > 64:
            self.hist.pop(0)
        self.last = e
        return u


def run(ctrl, alpha, scenario, steps=1800, L=4, sigma=1.5, seed=7,
        dist=250.0, speed=600.0, amp=150.0, freq=0.7, Lr=None):
    """L = 真传输延迟步数。Lr = 控制器重建时假设的延迟（失配测试用）。"""
    rng = random.Random(seed)
    ctrl.L = L if Lr is None else Lr
    ctrl.reset()
    X = 0.0                       # 准星位置（屏上）
    hist = [0.0] * (L + 1)        # u 的延迟队列
    detPrev = None
    detVel = 0.0
    det = 0.0
    glow = []
    for k in range(steps):
        t = k / HZ
        if scenario == "step":
            T = dist
        elif scenario == "strafe":
            T = (speed / HZ) * k
        else:                     # jink
            T = amp * math.sin(2 * math.pi * freq * t)

        # 检测 60Hz：偶数步刷新测量，奇数步用 tracker 的速度预测顶住
        if k % ctrl.det_every == 0:
            meas = T + rng.gauss(0.0, sigma)
            if detPrev is not None:
                detVel = (meas - detPrev) / ctrl.det_every
            detPrev = meas
            det = meas
        else:
            det = det + detVel      # ← 之前这里写死 speed/HZ，阶跃测试的"目标"会自己漂

        e = det - X
        u = ctrl.update(e)
        hist.append(u)
        X += alpha * hist.pop(0)  # u_{k-L}
        glow.append(T - X)
    return glow


def stat(glow, tail=900, start=12):
    seg = glow[-tail:]
    sd = sum(seg) / len(seg)
    rms = math.sqrt(sum(x * x for x in seg) / len(seg))
    pp = max(seg) - min(seg)
    fin = sum(glow[-50:]) / 50.0
    peak = max(-(x - fin) for x in glow[start:])
    settle = len(glow)
    for i in range(start, len(glow)):
        if all(abs(x - fin) < 3.0 for x in glow[i:i + 40]):
            settle = i
            break
    return sd, rms, pp, peak, settle


def sh(kp=0.10, ki=0.5, kd=0.20, kout=120.0, trim=None, kf=1.0, src="aligned",
       form="mul"):
    return PIDF(kp, ki, kd, kout, kout if trim is None else trim, kf,
                ff_src=src, ff_form=form)


def hdr(s):
    print("\n" + "=" * 104)
    print(s)
    print("=" * 104)


if __name__ == "__main__":
    print("L = 4 步传输延迟（48ms）、检测 60Hz、控制 120Hz、传感器 sigma = 1.5px")
    print("kp=0.10 / ki=0.5 / kd=0.20 / kf=1.0，只动输出上限。不缩放。")

    hdr("表1  固定输出上限该给多少？600px/s 横移，前馈按 kf=1/alpha 标定 —— 稳态误差 / 摆幅")
    caps = [60.0, 120.0, 180.0, 240.0]
    print("alpha |  需求 u_ss |" + "".join("%20s" % ("kout=%d" % c) for c in caps))
    for a in ALPHAS:
        need = (600.0 / HZ) / a
        row = "%.2f  | %9.1f |" % (a, need)
        for c in caps:
            sd, rms, pp, _, _ = stat(run(sh(kout=c, kf=1.0 / a), a, "strafe",
                                          speed=600.0))
            row += "%20s" % ("e=%6.1f pp=%4.0f" % (sd, pp))
        print(row)

    hdr("表2  1500px/s 横移（低灵敏度端的地狱），前馈按 kf=1/alpha 标定")
    print("alpha |  需求 u_ss |" + "".join("%20s" % ("kout=%d" % c) for c in caps))
    for a in ALPHAS:
        need = (1500.0 / HZ) / a
        row = "%.2f  | %9.1f |" % (a, need)
        for c in caps:
            sd, rms, pp, _, _ = stat(run(sh(kout=c, kf=1.0 / a), a, "strafe",
                                          speed=1500.0))
            row += "%20s" % ("e=%6.1f pp=%4.0f" % (sd, pp))
        print(row)

    hdr("表3  前馈该拿哪一份\"自己的输出\"做重建？ prev=u_{k-1}（现行） vs aligned=u_{k-L}")
    print("     kf=1.0, kout=120。自洩项 u_{k-1}/kf − alpha·u_{k-L} 在 u 变化时不为零：")
    print("     它把\"自己的变化率\"当成目标速度读回去 ⇒ 正反馈。aligned 用真正推动了世界的那一步。")
    print("alpha |    600px/s strafe      |   1500px/s strafe      |   jink rms   |  250px 阶跃 过/收")
    for a in ALPHAS:
        row = "%.2f  |" % a
        for src in ("prev", "aligned"):
            sd, rms, pp, _, _ = stat(run(sh(kf=1.0, src=src), a, "strafe", speed=600.0))
            row += " %s e=%6.1f pp=%5.0f |" % (src[:4], sd, pp)
        for src in ("prev", "aligned"):
            sd, rms, pp, _, _ = stat(run(sh(kf=1.0, src=src), a, "strafe", speed=1500.0))
            row += " %s pp=%5.0f |" % (src[:4], pp)
        for src in ("prev", "aligned"):
            row += " %6.1f |" % stat(run(sh(kf=1.0, src=src), a, "jink"))[1]
        for src in ("prev", "aligned"):
            _, _, _, pk, st = stat(run(sh(kf=1.0, src=src), a, "step"))
            row += " %s %4.0f/%4d |" % (src[:4], pk, st)
        print(row)

    hdr("表3b 静态漂移自检：aligned 前馈在静止目标上是否真的零输出？")
    print("     (前馈在自己身上读到速度 = 直接造假目标；这栏应该全是 0 附近)")
    for a in (0.10, 0.30, 1.00):
        for src in ("prev", "aligned"):
            g = run(sh(kf=1.0, src=src), a, "step", dist=0.0)
            seg = g[600:]
            print("  alpha=%.2f  %-7s  静止目标摆幅 pp=%6.2f" % (a, src, max(seg) - min(seg)))

    hdr("表4  kf 安全区（aligned 重建）600px/s —— 自洩已按延迟对齐")
    kfs = [0.0, 0.5, 1.0, 2.0, 4.0, 8.0]
    print("alpha |" + "".join("%18s" % ("kf=%.1f" % k) for k in kfs))
    for a in ALPHAS:
        row = "%.2f  |" % a
        for k in kfs:
            sd, rms, pp, _, _ = stat(run(sh(kf=k), a, "strafe", speed=600.0))
            row += "%18s" % ("e=%5.1f pp=%4.0f" % (sd, pp))
        print(row)

    hdr("表4  变向 ±150px@0.7Hz rms —— kf 越大越贴，但吃检测抖动")
    print("alpha |" + "".join("%18s" % ("kf=%.1f" % k) for k in kfs))
    for a in (0.10, 0.30, 1.00):
        row = "%.2f  |" % a
        for k in kfs:
            row += "%18s" % ("%6.1f" % stat(run(sh(kf=k), a, "jink"))[1])
        print(row)

    hdr("表5  静止目标 250px 阶跃 —— 过冲 / 收敛帧（\"特别严重的过冲\"有没有复发）")
    print("alpha |" + "".join("%18s" % ("kf=%.1f" % k) for k in kfs))
    for a in ALPHAS:
        row = "%.2f  |" % a
        for k in kfs:
            _, _, _, pk, st = stat(run(sh(kf=k), a, "step"))
            row += "%18s" % ("过%4.0f 收%4d" % (pk, st))
        print(row)

    hdr("表6  积分上限（leash）：固定 120 会不会让大阶跃重新出现\"慢慢拉回\"？")
    print("800px 阶跃，kf=1.0，kout=120")
    leashes = [56.0, 80.0, 120.0, 240.0]
    print("alpha |" + "".join("%22s" % ("leash=%d" % int(l)) for l in leashes))
    for a in (0.05, 0.10, 0.30, 1.00):
        row = "%.2f  |" % a
        for l in leashes:
            _, _, _, pk, st = stat(run(sh(trim=l), a, "step", dist=800.0))
            row += "%22s" % ("过=%6.0f 收敛%5d" % (pk, st))
        print(row)

    hdr("表7  固定增益的物理边界：kp·alpha 必须 < 2sin(pi/(2(2L+1))) = 0.45 (L=4)")
    print("静止目标指头抖动（sigma 噪声直通量）")
    print("alpha |   kp·alpha |  pp 抖动 | 判定")
    for a in ALPHAS:
        g = run(sh(kf=0.0), a, "step", dist=0.0)
        pp = max(g[-600:]) - min(g[-600:])
        print("%.2f  |    %6.3f  |  %6.2f  | %s" % (a, 0.10 * a, pp,
              "稳" if 0.10 * a < 0.45 else "超限 ⇒ 会晃"))

    hdr("表8  kf = 1/alpha 时低端是否归零（aligned）—— 用户端 alpha 很小，这一栏决定成败")
    print("alpha |   kf=1/a  | 600px/s              | 1500px/s            | 静止 pp")
    for a in (0.05, 0.08, 0.10, 0.15, 0.20, 0.30, 0.50, 1.00):
        k = 1.0 / a
        sd, rms, pp, _, _ = stat(run(sh(kf=k), a, "strafe", speed=600.0))
        sd2, _, pp2, _, _ = stat(run(sh(kf=k), a, "strafe", speed=1500.0))
        g = run(sh(kf=k), a, "step", dist=0.0)
        seg = g[600:]
        print("%.2f  |  %6.1f  | e=%7.1f pp=%5.0f | pp=%5.0f        | %8.2f" %
              (a, k, sd, pp, pp2, max(seg) - min(seg)))

    hdr("表9  延迟失配：真 L=4，控制器重建时假设 Lr=2..7（aligned 能不能容忍）")
    print("     取 kf=1/alpha，600px/s")
    print("alpha |" + "".join("%17s" % ("Lr=%d" % lr) for lr in (2, 3, 4, 5, 6, 7)))
    for a in (0.10, 0.30):
        row = "%.2f  |" % a
        for lr in (2, 3, 4, 5, 6, 7):
            sd, _, pp, _, _ = stat(run(sh(kf=1.0 / a), a, "strafe", speed=600.0, Lr=lr))
            row += "%17s" % ("e=%6.1f pp=%4.0f" % (sd, pp))
        print(row)

    hdr("表10 真 L 本身是多少？（kp=0.10/ki=0.5/kd=0.20, kf=0 纯 PID，不留 DC 误差干扰）")
    print("alpha |" + "".join("%14s" % ("L=%d" % l) for l in (2, 3, 4, 5, 6)))
    for a in (0.30, 1.00):
        row = "%.2f  |" % a
        for l in (2, 3, 4, 5, 6):
            g = run(sh(kf=0.0), a, "step", dist=0.0, L=l)
            seg = g[600:]
            row += "%14s" % ("pp=%7.2f" % (max(seg) - min(seg)))
        print(row)

    hdr("表11 前馈的两种写法，取 kf=1/alpha（标定正确）。600px/s / jink rms / 静止 pp")
    print("alpha | mul+prev            | mul+aligned         | smith+aligned       | smith 静止pp")
    for a in (0.08, 0.10, 0.15, 0.20, 0.30, 0.50, 1.00):
        k = 1.0 / a
        row = "%.2f  |" % a
        for src, form in (("prev", "mul"), ("aligned", "mul")):
            sd, _, pp, _, _ = stat(run(sh(kf=k, src=src, form=form), a, "strafe", speed=600.0))
            row += " e=%6.1f pp=%5.0f   |" % (sd, pp)
        sd, _, pp, _, _ = stat(run(sh(kf=k, src="aligned", form="smith"), a, "strafe", speed=600.0))
        row += " e=%6.1f pp=%5.0f   |" % (sd, pp)
        g = run(sh(kf=k, src="aligned", form="smith"), a, "step", dist=0.0)
        seg = g[600:]
        row += " %8.2f" % (max(seg) - min(seg))
        print(row)

    hdr("表12 smith 的 kf 标定敏感度：kf 偏离 1/alpha 多少还稳？（600px/s）")
    print("alpha |" + "".join("%19s" % ("kf*alpha=%.2f" % m) for m in
                              (0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0)))
    for a in (0.10, 0.30, 1.00):
        row = "%.2f  |" % a
        for m in (0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0):
            sd, _, pp, _, _ = stat(run(sh(kf=m / a, form="smith"), a, "strafe", speed=600.0))
            row += "%19s" % ("e=%6.1f pp=%4.0f" % (sd, pp))
        print(row)

    hdr("表13 静止目标上\"前馈会不会自己爬\"（smith，kf 失配）—— 爬 = kf 过了 1/alpha")
    print("alpha |" + "".join("%16s" % ("kf*alpha=%.2f" % m) for m in
                              (0.25, 0.5, 1.0, 1.5, 2.0, 3.0)))
    for a in (0.10, 0.30, 1.00):
        row = "%.2f  |" % a
        for m in (0.25, 0.5, 1.0, 1.5, 2.0, 3.0):
            g = run(sh(kf=m / a, form="smith"), a, "step", dist=0.0)
            seg = g[600:]
            row += "%16s" % ("pp=%7.2f" % (max(seg) - min(seg)))
        print(row)

    hdr("表14 kf x alpha 摆幅地图（mul+aligned, kout=180, leash=90, 600px/s）")
    print("     数字 = 稳态摆幅 pp(px)。 '  .' <8px（优）  '  *' <60px（可）  '  X' >=60px（晃）")
    kfs2 = [0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0]
    print("alpha |" + "".join("%6s" % (("%.2f" % k) if k else "0") for k in kfs2))
    for a in (0.05, 0.08, 0.10, 0.15, 0.20, 0.30, 0.50, 0.70, 1.00):
        row = "%.2f  |" % a
        for k in kfs2:
            pp = stat(run(sh(kout=180.0, trim=90.0, kf=k), a, "strafe",
                          speed=600.0, steps=2400))[2]
            sym = "  ." if pp < 8 else ("  *" if pp < 60 else "  X")
            row += "%5.0f%s" % (pp, sym)
        print(row)
    print("\n     理想线是 kf·alpha = 1 —— 也就是 kf = 1/alpha。")

    hdr("表15 出厂判决：设备上现在这版（v7：kf=1, u(k-1), 上限60/56） vs 新版纯 PIDF（kf=3, u(k-L), 180/90）")
    def before():
        return PIDF(0.10, 0.5, 0.20, 60.0, 56.0, 1.0, L=4, ff_src="prev")

    def after():
        return PIDF(0.10, 0.5, 0.20, 180.0, 90.0, 3.0, L=5, ff_src="aligned")

    print("alpha |            装置 600px/s            |            新版 600px/s            | 装置 1500px/s | 新版 1500px/s")
    print("      |      mean e        摆幅           |      mean e        摆幅           |   mean e      |   mean e")
    for a in (0.05, 0.08, 0.10, 0.20, 0.30, 0.50, 1.00):
        r = "%.2f  |" % a
        for c in (before(), after()):
            sd, _, pp, _, _ = stat(run(c, a, "strafe", speed=600.0, steps=2400))
            r += " %8.1f %9.1f        |" % (sd, pp)
        for c in (before(), after()):
            sd, _, _, _, _ = stat(run(c, a, "strafe", speed=1500.0, steps=2400))
            r += " %10.1f   |" % sd
        print(r)

    print("\n        变向 ±150px@0.7Hz  rms    |  250px 阶跃 过冲/收敛帧       |  800px 阶跃 过冲/收敛")
    for a in (0.05, 0.10, 0.30, 1.00):
        r = "alpha=%.2f" % a
        for c in (before(), after()):
            r += "  %9.1f" % stat(run(c, a, "jink", steps=2400))[1]
        for c in (before(), after()):
            _, _, _, pk, st = stat(run(c, a, "step", dist=250.0, steps=2400))
            r += "   %5.0f/%5d" % (pk, st)
        for c in (before(), after()):
            _, _, _, pk, st = stat(run(c, a, "step", dist=800.0, steps=3000))
            r += "   %5.0f/%5d" % (pk, st)
        print(r)

    print("\n        静止目标指头抖动 pp(px)      |  alpha >= 2 时两者都会晃（固定增益的物理边界）")
    for a in (0.10, 0.30, 1.00, 2.00, 3.00):
        g1 = run(before(), a, "step", dist=0.0, steps=2400)[600:]
        g2 = run(after(), a, "step", dist=0.0, steps=2400)[600:]
        print("alpha=%.2f   装置 %7.2f   新版 %7.2f" % (a, max(g1) - min(g1), max(g2) - min(g2)))

    hdr("表16 积分 leash 该给多少？新版结构（kf=3, kout=180, u(k-L)）")
    print("      250px 阶跃 过冲 / 800px 阶跃 过冲     |     600px/s 稳态误差 / 摆幅")
    print("alpha |" + "".join("%22s" % ("leash=%d" % l) for l in (45.0, 60.0, 90.0)) +
          " |" + "".join("%16s" % ("leash=%d" % l) for l in (45.0, 60.0, 90.0)))
    for a in (0.05, 0.08, 0.10, 0.20, 0.30, 1.00):
        r = "%.2f  |" % a
        for l in (45.0, 60.0, 90.0):
            _, _, _, pk2, st2 = stat(run(PIDF(0.10, 0.5, 0.20, 180.0, l, 3.0,
                                              ff_src="aligned"), a, "step",
                                          dist=250.0, steps=2400))
            _, _, _, pk8, st8 = stat(run(PIDF(0.10, 0.5, 0.20, 180.0, l, 3.0,
                                              ff_src="aligned"), a, "step",
                                          dist=800.0, steps=3000))
            r += "%22s" % ("%5.0f /%5.0f (%4d)" % (pk2, pk8, st2))
        for l in (45.0, 60.0, 90.0):
            sd, _, pp, _, _ = stat(run(PIDF(0.10, 0.5, 0.20, 180.0, l, 3.0,
                                            ff_src="aligned"), a, "strafe",
                                        speed=600.0, steps=2400))
            r += "%16s" % ("e=%6.1f pp=%4.0f" % (sd, pp))
        print(r)

    hdr("表17 leash 在 kf=0（前馈关掉，积分必须独扛 DC）时够不够？")
    print("alpha |" + "".join("%18s" % ("leash=%d" % l) for l in (45.0, 60.0, 90.0)))
    for a in (0.08, 0.10, 0.20, 0.30):
        r = "%.2f  |" % a
        for l in (45.0, 60.0, 90.0):
            sd, _, pp, _, _ = stat(run(PIDF(0.10, 0.5, 0.20, 180.0, l, 0.0,
                                            ff_src="aligned"), a, "strafe",
                                        speed=600.0, steps=2400))
            r += "%18s" % ("e=%7.1f pp=%4.0f" % (sd, pp))
        print(r)
