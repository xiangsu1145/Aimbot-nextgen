#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aim_alpha_fix.py — 第五轮控制器在 alpha > 1 区间的极限环定位与候选修法判决。

背景：`aim_v5_verify.py` 的终选组合（q=0.4 + 速度门控）在 alpha<=1 全区 0.0px，
但在 alpha>=1.6 出现 pp 900~1100px 的极限环（第四轮/关前馈是 0.0px）。

alpha = 视野 px / 手指 px（游戏灵敏度）。高灵敏度下 alpha>1 是**常见**区间，
不是边角：一根手指横扫 1080px 转 180°、水平 FOV 90° 时 alpha ≈ 2.0。
所以必须修，不能"接受回归"。

本文件用同一套 sim 跑一个候选矩阵，对每个候选同时看四个症状面：
  跟上   1500px/s 横移（超出积分 leash=24 的场景，前馈唯一存在的理由）
  jink   ±150px@0.7Hz（大机动）
  静止   只有检测框噪声（指头抖动量 = 代价）
  接近   250px 纯阶跃（"到目标附近变慢"）
"""
import math
import os
import random
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HZ = 120.0
DT = 1.0 / HZ
K_OUT, TAU_D, RAMP = 60.0, 0.025, 0.30
GATE_VEL_FULL, GATE_BOX_REF = 150.0, 100.0
GATE_RATE, GATE_EXIT = 0.025, 0.10


class Ctl:
    """参数化的 pid_controller.h 镜像。c_mode / trim / q / 两个守卫可换。"""

    def __init__(self, kp=0.10, ki=1.0, kd=0.35, ff=1.0, q=0.400, c_mode="total",
                 trim=24.0, osc_guard=False, alpha_est=False, gate=True,
                 fade_i=False, dmatch=0, ac_only=False, anorm=False,
                 anorm_oracle=False, ah_rate=0.004):
        self.kp, self.ki, self.kd, self.ffg = kp, ki, kd, ff
        self.q, self.c_mode, self.trim = q, c_mode, trim
        self.osc_guard, self.alpha_est, self.gate_on = osc_guard, alpha_est, gate
        self.fade_i, self.dmatch = fade_i, dmatch
        self.ac_only = ac_only
        self.anorm, self.anorm_oracle = anorm, anorm_oracle
        self.ah_rate = ah_rate
        self.alpha_true = 1.0    # anorm_oracle 用；sim 会填真值
        self.fdc = 0.0           # 仅 ac_only 用：估计量的直流分量
        self.fbPrev = 0.0        # 仅 c_mode="fb" 用：上一步的纯反馈输出
        self.I = self.last = self.out = self.prevU = 0.0
        self.d = self.ramp = self.fx = 0.0
        self.fp, self.gate = 1.0, 0.0
        self.ah = 1.0            # alpha 估计（仅 alpha_est 用）
        self.flip = 0            # 连续变号计数（仅 osc_guard 用）
        self.ubuf = [0.0] * max(1, dmatch + 1)   # 仅 dmatch 用

    def update(self, e, dt, frozen=False, changed=False, box=0.0, short=1080.0,
               tv=0.0):
        if changed:
            self.I = self.d = self.fx = self.gate = 0.0
            self.fdc = 0.0
            self.last = e
            self.ramp, self.fp, self.flip = 0.0, 1.0, 0
        self.ramp = min(1.0, self.ramp + RAMP)

        # ── 门控（速度型） ──────────────────────────────────────────────────
        if self.gate_on:
            scale = max(1.0, box / GATE_BOX_REF) if box > 1.0 else 1.0
            want = min(1.0, abs(tv) / (GATE_VEL_FULL * DT * scale))
            r = GATE_RATE if want > self.gate else GATE_EXIT
            self.gate = min(1.0, max(0.0, self.gate + (want - self.gate) * r))
        else:
            self.gate = 1.0

        de = e - self.last

        # ── 环路增益归一化（把 alpha 除掉） ─────────────────────────────────
        # 手指 px → 视野 px 的增益就是 alpha，所以整条环路的开环增益是
        # kp·alpha。alpha 在不同游戏/灵敏度下差一个数量级 ⇒ 同一组滑块在
        # alpha=0.3 时"跟不上"、在 alpha=2.5 时"又晃又抖"，中间没有可用档位。
        # alpha 本身不可测，但可以【测出来】：de = ΔT - alpha·u ⇒
        #     alpha = (ΔT - de) / u      （ΔT 用 tracker 的视野速度）
        # 这是个缓变量（只随游戏设置变），长时常数滤波即可；实测收敛到
        # 0.302/0.570/1.305/1.783/2.033/2.351（真值 0.30/0.50/1.00/1.60/2.00/2.50）。
        inv = 1.0
        if self.anorm or self.anorm_oracle:
            if self.anorm_oracle:
                self.ah = self.alpha_true
            else:
                if abs(self.prevU) > 1.5:
                    a_meas = (tv - de) / self.prevU
                    if 0.05 < a_meas < 6.0:
                        self.ah += self.ah_rate * (a_meas - self.ah)
            inv = 1.0 / max(self.ah, 0.05)

        # ── 变号守卫：误差在相邻几步里来回变号 = 环路自激，不是目标在动 ─────
        hold = False
        if self.osc_guard:
            if de * getattr(self, "_lde", 0.0) < 0.0:
                self._updown = getattr(self, "_updown", 0) + 1
            else:
                self._updown = 0
            hold = self._updown >= 5
        self._lde = de

        # ── 自校准前馈 ──────────────────────────────────────────────────────
        if self.c_mode == "ff":
            meas_src = self.fx          # 只用自己的输出当参考 ⇒ 同样自消项
        elif self.c_mode == "none":
            meas_src = 0.0
        elif self.c_mode == "fb":
            # ★ 关键修正：参考量只取【反馈】部分的输出，不含前馈自己。
            #   代数：fx_k = (1-kf)fx_{k-1} + kf·meas，若 meas 里含 +fx_{k-1}
            #   （出厂用 total u，而 u 含前馈），则 (1-kf)fx + kf·fx = fx
            #   —— 阻尼项精确抵消，滤波器退化成纯积分器。纯积分器串上环路
            #   延迟 L 就是振荡器（3 项差分方程，周期由 L 定、与 kf 无关，
            #   所以 q 怎么调都不管用，实测 q=0.1 反而更差）。
            #   换成只含反馈量后：不动点是 fx = ΔT/α（精确，无系数），
            #   且 (1-kf) 的恢复项回来了。
            meas_src = self.fbPrev
        elif self.dmatch:
            meas_src = self.ubuf[0]
        else:
            meas_src = self.prevU       # 出厂：总输出

        if self.alpha_est:
            # alpha = (ΔT - Δe)/u；ΔT 用 tracker 速度。alpha 只在 u 明显非零时
            # 可测，且是缓变量（只随游戏内灵敏度变）⇒ 用长时常数滤波。
            if abs(self.prevU) > 1.5:
                a_meas = (tv - de) / self.prevU
                if 0.05 < a_meas < 6.0:
                    self.ah += 0.004 * (a_meas - self.ah)
            tgt_ff = (de + self.ah * self.prevU) / max(self.ah, 0.05)
            self.fx += 0.35 * (tgt_ff - self.fx)
            ff = self.fx
        else:
            meas = de + meas_src
            self.fp += self.q
            kf = self.fp / (self.fp + 1.0)
            self.fx += kf * (meas - self.fx)
            self.fp = (1.0 - kf) * self.fp
            ff = self.fx

        if hold:
            # 自激时把前馈往 0 折，但不瞬间清零（免得变成另一个开关）
            self.fx *= 0.80

        # ── 直流/交流分工 + 积分饱和淡入 ────────────────────────────────────
        # 症状：前馈和积分是两个并联积分器，都去扛同一个稳态速度 ⇒ 互相顶，
        # 极限环（trace 里 I 安静在 ~10，fx 却摆 ±200）。分工后各管一段：
        #   积分  → 直流（慢，但无条件稳定）
        #   前馈  → 交流（反向、机动这些"快要变"的部分，积分来不及）
        # 前馈的直流只在【积分顶到自己的 leash】时才放出来 —— 需求超过 leash
        # 时积分确实扛不住（alpha=0.3 / 1500px/s 需要 41.7 > 24），那才是前馈
        # 直流唯一的存在理由。不需要知道 alpha，这个判据自己就在量 v/alpha。
        w = 1.0
        if self.fade_i:
            lo, hi = 0.5 * self.trim, self.trim
            w = min(1.0, max(0.0, (abs(self.I) - lo) / max(1e-6, hi - lo)))
        if self.ac_only:
            self.fdc += 0.02 * (self.fx - self.fdc)   # τ≈0.42s @120Hz
            ffv = (self.fx - self.fdc) + (w if self.fade_i else 0.0) * self.fdc
        else:
            ffv = self.fx * w
        ff = max(-K_OUT, min(K_OUT, self.ffg * self.gate * ffv))

        # ── 微分 / 比例 / 积分 ──────────────────────────────────────────────
        a = dt / (dt + TAU_D)
        self.d += a * (de - self.d)
        dd = max(-K_OUT, min(K_OUT, self.kd * inv * self.ramp * self.d))
        p = self.kp * inv * self.ramp * e
        if frozen:
            p = dd = 0.0

        raw = p + self.I + dd
        if (not frozen) and not (raw >= K_OUT and e > 0) \
                and not (raw <= -K_OUT and e < 0):
            self.I += self.ki * inv * e * dt
            self.I = max(-self.trim, min(self.trim, self.I))

        u = K_OUT * math.tanh((p + self.I + dd) / K_OUT) + ff
        self.fbPrev = u - ff
        self.out = u
        self.last, self.prevU = e, self.out
        if self.dmatch:
            self.ubuf = self.ubuf[1:] + [self.out]
        return self.out


def sim(ctrl, tgt, alpha, delay=3, sigma=1.5, seed=7, steps=2400, box=100.0,
        short=1080.0, k_vel=0.42):
    ctrl.alpha_true = alpha
    rng = random.Random(seed)
    view = 0.0
    err = deque([0.0] * (delay + 1), maxlen=delay + 1)
    v_est, r_prev = 0.0, None
    rec = {k: [] for k in ("glow", "u", "trim", "ff", "gate", "ah")}
    for n in range(steps):
        e_meas = err[0]
        box_now = tgt(n) + rng.gauss(0.0, sigma)
        if r_prev is None:
            r_prev = box_now
        v_est += k_vel * ((box_now - r_prev) - v_est)
        r_prev = box_now
        u = ctrl.update(e_meas, DT, box=box, short=short, tv=v_est)
        view += alpha * u
        glow = tgt(n) - view
        err.append(box_now - view)
        rec["glow"].append(glow)
        rec["u"].append(u)
        rec["trim"].append(ctrl.I)
        rec["ff"].append(ctrl.ffg * ctrl.gate * (ctrl.fx if not ctrl.alpha_est else 0))
        rec["gate"].append(ctrl.gate)
        rec["ah"].append(ctrl.ah)
    return rec


def stats(rec, tail=600):
    w, u = rec["glow"][-tail:], rec["u"][-tail:]
    n = len(w)
    mu = sum(u) / n
    return dict(mean=sum(w) / n, rms=math.sqrt(sum(x * x for x in w) / n),
                pp=max(w) - min(w), low=min(w),
                jerk=math.sqrt(sum((x - mu) ** 2 for x in u) / n),
                trim=sum(rec["trim"][-tail:]) / n,
                ff=sum(rec["ff"][-tail:]) / n,
                gate=sum(rec["gate"][-tail:]) / n,
                ah=sum(rec["ah"][-tail:]) / n)


def strafe(s): return lambda n: s * n * DT
def tri(a, p): return lambda n: a * (4.0 * abs(((n * DT / p) % 1.0) - 0.5) - 1.0)
def zero(): return lambda n: 0.0


ALPHAS = (0.30, 0.50, 1.00, 1.60, 2.00, 2.50)


def variants():
    return [
        ("A 出厂 v5 (有FF)",        lambda: Ctl()),
        ("C 无FF t60 ki1 kd.35",   lambda: Ctl(ff=0.0, trim=60.0)),
        ("U1 α归一 kp.25 ki1 kd.6", lambda: Ctl(ff=0.0, trim=60.0, anorm=True,
                                                kp=0.25, ki=1.0, kd=0.6)),
        ("U2 同上(α真值) 天花板",    lambda: Ctl(ff=0.0, trim=60.0, anorm_oracle=True,
                                                kp=0.25, ki=1.0, kd=0.6)),
        ("U3 α归一 kp.35 ki1.5 kd.8", lambda: Ctl(ff=0.0, trim=60.0, anorm=True,
                                                  kp=0.35, ki=1.5, kd=0.8)),
        ("U4 α归一 kp.20 ki.6 kd.5", lambda: Ctl(ff=0.0, trim=60.0, anorm=True,
                                                 kp=0.20, ki=0.6, kd=0.5)),
    ]


def hdr(t):
    print("\n" + "=" * 92)
    print(t)
    print("=" * 92)


def main():
    vs = variants()

    # ── 1. 跟上 1500px/s（前馈唯一的存在理由：需求超过积分 leash） ──────────
    hdr("1  跟上：1500 px/s 横移，看 mean_e (px)。这一档需求 = 12.5/alpha 手指 px/步,\n"
        "   积分 leash=24 ⇒ alpha<0.52 时积分已到顶，必须靠前馈")
    print(f"{'方案':>24} | " + "".join(f"{f'a={a}':>9}" for a in ALPHAS))
    print("-" * 92)
    for name, mk in vs:
        row = []
        for a in ALPHAS:
            row.append(stats(sim(mk(), strafe(1500.0), a))["mean"])
        print(f"{name:>24} | " + "".join(f"{v:>9.1f}" for v in row))

    # ── 2. 600px/s 横移（全区间基线） ────────────────────────────────────────
    hdr("2  基线：600 px/s 横移，mean_e (px)")
    print(f"{'方案':>24} | " + "".join(f"{f'a={a}':>9}" for a in ALPHAS))
    print("-" * 92)
    for name, mk in vs:
        row = [stats(sim(mk(), strafe(600.0), a))["mean"] for a in ALPHAS]
        print(f"{name:>24} | " + "".join(f"{v:>9.1f}" for v in row))

    # ── 3. alpha>1 的极限环：pp / rms 才是判据 ──────────────────────────────
    hdr("3  极限环：1500px/s 横移，看 pp (px)。pp 上千 = 目标在屏上来回摆")
    print(f"{'方案':>24} | " + "".join(f"{f'a={a}':>12}" for a in (1.0, 1.6, 2.0, 2.5)))
    print(f"{'':>24} | " + "".join(f"{'pp/rms':>12}" for _ in range(4)))
    print("-" * 92)
    for name, mk in vs:
        row = []
        for a in (1.0, 1.6, 2.0, 2.5):
            s = stats(sim(mk(), strafe(1500.0), a))
            row.append(f"{s['pp']:.0f}/{s['rms']:.0f}")
        print(f"{name:>24} | " + "".join(f"{v:>12}" for v in row))

    # ── 4. jink ─────────────────────────────────────────────────────────────
    hdr("4  大机动：±150px 三角波 @0.7Hz, alpha=0.5 —— rms / jerk")
    print(f"{'方案':>24} | {'rms':>8}{'pp':>8}{'jerk':>8}{'mean':>8}{'gate':>7}")
    print("-" * 92)
    for name, mk in vs:
        s = stats(sim(mk(), tri(150.0, 0.7), 0.5, steps=1800), tail=900)
        print(f"{name:>24} | {s['rms']:>8.1f}{s['pp']:>8.1f}{s['jerk']:>8.2f}"
              f"{s['mean']:>8.1f}{s['gate']:>7.2f}")

    # ── 5. 静止抖动（代价） ─────────────────────────────────────────────────
    hdr("5  静止目标：指头输出 rms（代价）—— sigma 扫")
    print(f"{'方案':>24} | " + "".join(f"{'s=%.1f' % s:>9}" for s in (0.5, 1.5, 3.0, 6.0)))
    print("-" * 92)
    for name, mk in vs:
        row = []
        for sg in (0.5, 1.5, 3.0, 6.0):
            s = stats(sim(mk(), zero(), 0.5, sigma=sg, steps=1200), tail=600)
            row.append(s["jerk"])
        print(f"{name:>24} | " + "".join(f"{v:>9.3f}" for v in row))

    # ── 4b. 大机动 × alpha（"跟不上枪"真正发生的地方） ──────────────────────
    hdr("4b 大机动 rms × alpha（±150px@0.7Hz）—— 环路带宽必须够跟反向")
    allA = (0.15, 0.30, 0.50, 1.00, 1.60, 2.50)
    print(f"{'方案':>24} | " + "".join(f"{f'a={a}':>9}" for a in allA))
    print("-" * 92)
    for name, mk in vs:
        row = []
        for a in allA:
            s = stats(sim(mk(), tri(150.0, 0.7), a, steps=1800), tail=900)
            row.append(s["rms"])
        print(f"{name:>24} | " + "".join(f"{v:>9.1f}" for v in row))

    # ── 6. 接近 ─────────────────────────────────────────────────────────────
    hdr("6  253px 纯阶跃：进入 |e|<20 / <5 的帧数，越冲")
    print(f"{'方案':>24} | {'到100':>7}{'到20':>7}{'到5':>7}{'到1':>7}{'越冲':>9}")
    print("-" * 92)
    for name, mk in vs:
        rec = sim(mk(), lambda n: 250.0, 0.5, steps=900)
        g = [abs(x) for x in rec["glow"]]
        fr = []
        for thr in (100, 20, 5, 1):
            i = next((k for k, v in enumerate(g) if v < thr), None)
            fr.append(f"{i:>7}" if i is not None else f"{'--':>7}")
        s = stats(rec, tail=120)
        print(f"{name:>24} | " + "".join(fr) + f"{s['low']:>9.1f}")

    # ── 7. alpha 估计收敛性（若选了 alpha_est） ─────────────────────────────
    hdr("7  alpha_est 收敛：1500px/s，看估计到的 alpha（真值见列名）")
    print(f"{'真 alpha':>9} | " + "".join(f"{'a_hat':>9}{'mean':>9}" for _ in range(1)))
    print("-" * 92)
    for a in ALPHAS:
        s = stats(sim(Ctl(alpha_est=True), strafe(1500.0), a))
        print(f"{a:>9.2f} | {s['ah']:>9.3f}{s['mean']:>9.1f}")


# ── 8. α 估计的稳健性：收敛速率 × tracker 速度质量 ─────────────────────────
def h_robust():
    hdr("8  U1 的 α 估计：收敛速率 rate × tracker 速度带宽 k_vel\n"
        "    每格 = 大机动 rms@α=1.6 / 估计到的 α（真值 1.60）；(首) = 前 300 帧 rms")
    rates = (0.002, 0.004, 0.020)
    kvels = (0.30, 0.42, 0.70)
    print(f"{'rate\\\\k_vel':>12} | " + "".join(f"{f'k={k}':>22}" for k in kvels))
    print("-" * 92)
    for r in rates:
        cells = []
        for kv in kvels:
            rec = sim(Ctl(ff=0.0, trim=60.0, anorm=True, kp=0.25, ki=1.0, kd=0.6,
                          ah_rate=r), tri(150.0, 0.7), 1.6, steps=1800, k_vel=kv)
            s = stats(rec, tail=900)
            early = rec["glow"][:300]
            er = math.sqrt(sum(x * x for x in early) / len(early))
            cells.append(f"{s['rms']:>7.1f}/{s['ah']:>4.2f}({er:>5.0f})")
        print(f"{r:>12.3f} | " + "".join(f"{c:>22}" for c in cells))


# ── 9. 积分 leash：它现在是直流的唯一承运人 ────────────────────────────────
def i_leash():
    hdr("9  U1 的积分 leash 选型（需求 = v/120/alpha 手指 px/步）\n"
        "    低速+低敏是最吃 leash 的组合：600px/s & alpha=0.15 需要 33 px/步")
    cases = [(600.0, 0.15), (600.0, 0.20), (600.0, 0.30), (600.0, 0.50),
             (1500.0, 0.30), (1500.0, 0.50)]
    print(f"{'leash':>7} | " + "".join(f"{f'{int(s)}@{a}':>11}" for s, a in cases)
          + f"{'jink.5':>9}{'接近到1':>9}{'静止3':>8}")
    print("-" * 92)
    for t in (24.0, 36.0, 48.0, 60.0):
        mk = lambda: Ctl(ff=0.0, trim=t, anorm=True, kp=0.25, ki=1.0, kd=0.6,
                         ah_rate=0.020)
        row = []
        for s, a in cases:
            row.append(stats(sim(mk(), strafe(s), a))["mean"])
        jk = stats(sim(mk(), tri(150.0, 0.7), 0.5, steps=1800), tail=900)["rms"]
        rec = sim(mk(), lambda n: 250.0, 0.5, steps=900)
        g = [abs(x) for x in rec["glow"]]
        i1 = next((k for k, v in enumerate(g) if v < 1.0), None)
        st = stats(sim(mk(), zero(), 0.5, sigma=3.0, steps=1200), tail=600)
        print(f"{t:>7.0f} | " + "".join(f"{v:>11.1f}" for v in row)
              + f"{jk:>9.1f}{str(i1):>9}{st['jerk']:>8.3f}")


# ── 10. leash 的代价：大阶跃 + 目标中途急停（积分反绕才是它的风险） ─────────
def j_windup():
    hdr("10 leash 的风险面：800px 大阶跃 与 目标急停，leash 48 vs 60")
    print(f"{'leash':>7}{'场景':>18} | {'到20':>6}{'到5':>6}{'越冲':>8}"
          f"{'尾段max':>9}{'尾段rms':>9}")
    print("-" * 92)

    def stop_at(kk):
        return lambda n: strafe(600.0)(min(n, kk))

    def tail_of(rec, nstop):
        return rec["glow"][nstop + 60:nstop + 360] if nstop \
            else rec["glow"][300:600]

    for t in (48.0, 60.0):
        for label, tg, alpha, nstop in (
                ("800px阶跃 α=0.3", lambda n: 800.0, 0.3, None),
                ("800px阶跃 α=1.0", lambda n: 800.0, 1.0, None),
                ("600px/s 900帧后急停 α=0.3", stop_at(900), 0.3, 900),
                ("600px/s 900帧后急停 α=1.6", stop_at(900), 1.6, 900)):
            mk = lambda: Ctl(ff=0.0, trim=t, anorm=True, kp=0.25, ki=1.0, kd=0.6,
                             ah_rate=0.020)
            rec = sim(mk(), tg, alpha, steps=1500)
            g = [abs(x) for x in rec["glow"]]
            fr = []
            for thr in (20, 5):
                i = next((k for k, v in enumerate(g) if v < thr), None)
                fr.append(f"{i:>6}" if i is not None else f"{'--':>6}")
            tl = tail_of(rec, nstop)
            mx = max(abs(x) for x in tl)
            rm = math.sqrt(sum(x * x for x in tl) / len(tl))
            print(f"{t:>7.0f}{label:>18} | " + "".join(fr)
                  + f"{-mx if nstop else min(tl):>8.1f}{mx:>9.1f}{rm:>9.1f}")


if __name__ == "__main__":
    main()
    h_robust()
    i_leash()
    j_windup()
    print()
