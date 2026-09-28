#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
recoil_to_profile.py — PC 侧的高精度路线：recoil-analyser JSON → 本项目档案 JSON

为什么还要这条路：设备上的机内录制（NCC 标记跟踪）够快、够省事，但 PC 上的
视频分析器精度更高（逐帧真值 + OCR 弹药数）。换枪不频繁、或者机内录制反复
判失败时，用这条。

用法
----
    python scripts/recoil_to_profile.py --in analysis.json --name AKM
    python scripts/recoil_to_profile.py --in analysis.json --name AKM --plot
    python scripts/recoil_to_profile.py --in analysis.json --name AKM \
        --out /data/local/tmp/aimbotng/profiles      # 直接给 adb push 用

输入格式做得宽容：分析器版本不同字段摆放也不同，这里按优先级逐个尝试，
缺哪个就退化（缺 focal_px → 用 FOV 反推；缺 shots → 从 rpm 推）。

单位（和 tracking/recoil_profile.h 严格一致，不要改）
------------------------------------------------
    +x = 视角往右转（度）
    +y = 视角往下看（度）—— 枪口往上抬，世界就往下走，所以 dy 为正
    knots 相对第一发：knots[0] 恒为 (0, 0, 0)

关键：补偿比例是 S / Θpp（Θpp 是峰峰值，不是末值）。水平弹道不单调
（先右后左），用末值归一化会除到一个接近 0 的数。
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

KNOT_STEP_MS = 10.0      # 结点间隔，和 tracking/recoil_recorder.cpp 的 kKnotStepMs 一致
SCHEMA = 1
DEFAULT_FOV_DEG = 80.0


# ─────────────────────────────────────────────────────────────────────────────
# 读数：分析器字段的几种摆法
# ─────────────────────────────────────────────────────────────────────────────

def _first(d: dict, keys, default=None):
    for k in keys:
        if isinstance(d, dict) and k in d and d[k] is not None:
            return d[k]
    return default


def _focal_px(meta: dict) -> float:
    """px → 度 的换算基准。没有就用垂直 FOV 反推：f = (H/2) / tan(fov/2)。"""
    f = _first(meta, ("focal_px", "focal", "focal_length_px"))
    if f:
        return float(f)
    res = _first(meta, ("resolution", "res", "video_resolution"), [0, 0])
    h = float(res[1]) if isinstance(res, (list, tuple)) and len(res) >= 2 else 0.0
    fov = float(_first(meta, ("fov_deg", "fov", "vertical_fov"), DEFAULT_FOV_DEG))
    if h <= 0.0:
        return 0.0
    return (h * 0.5) / math.tan(math.radians(fov) * 0.5)


def _trajectory(src: dict):
    """取逐帧轨迹，统一成 [(tMs, xPx, yPx), ...]。"""
    tr = _first(src, ("trajectory", "traj", "frames_traj", "view_trajectory"))
    if tr is None:
        return None
    fps = float(_first(src, ("fps", "video_fps", "frame_rate"), 0.0) or 0.0)

    out = []
    if isinstance(tr, dict):
        # {"x": [...], "y": [...]} 或 {"t": [...], "x": [...], "y": [...]}
        xs = _first(tr, ("x", "dx", "horizontal"), [])
        ys = _first(tr, ("y", "dy", "vertical"), [])
        ts = _first(tr, ("t", "t_ms", "time_ms"), None)
        for i in range(min(len(xs), len(ys))):
            t = float(ts[i]) if ts is not None and i < len(ts) else (
                (i * 1000.0 / fps) if fps > 0 else float(i))
            out.append((t, float(xs[i]), float(ys[i])))
        return out if out else None

    for i, e in enumerate(tr):
        if isinstance(e, (list, tuple)) and len(e) >= 3:
            t, x, y = float(e[0]), float(e[1]), float(e[2])
            # 第一列是帧号而不是毫秒时，用 fps 换算
            if fps > 0 and t >= 0 and abs(t - round(t)) < 1e-9 and t > 1000.0 * len(tr) / fps:
                t = t * 1000.0 / fps
            out.append((t, x, y))
        elif isinstance(e, dict):
            t = float(_first(e, ("t", "t_ms", "time_ms"), i * 1000.0 / fps if fps > 0 else i))
            out.append((t, float(_first(e, ("x", "dx"), 0.0)), float(_first(e, ("y", "dy"), 0.0))))
    return out if out else None


def _shot_times(src: dict, traj, fps: float):
    """发数时刻（ms）。显式给了就用，没给就从 rpm 均匀推。"""
    m = _first(src, ("shots", "shots_marks", "shot_marks", "shots_ms"))
    if isinstance(m, list) and m:
        out = []
        for e in m:
            if isinstance(e, dict):
                out.append(float(_first(e, ("t", "t_ms", "time_ms"), 0.0)))
            elif isinstance(e, (list, tuple)) and e:
                out.append(float(e[0]))
            else:
                out.append(float(e))
        return sorted(out)

    rpm = float(_first(src, ("rpm", "fire_rate", "rate"), 0.0) or 0.0)
    n = int(_first(src, ("magazine", "mag_size", "shots_count"), 0) or 0)
    if rpm > 0 and n > 0:
        period = 60000.0 / rpm
        return [i * period for i in range(n)]
    return []


# ─────────────────────────────────────────────────────────────────────────────
# 转换
# ─────────────────────────────────────────────────────────────────────────────

def px_to_deg(px: float, focal: float) -> float:
    """小角用 px/f 就够，但开阔角下 atan 更准；f<=0 时退化为原样（不做换算）。"""
    if focal <= 0.0:
        return px
    return math.degrees(math.atan(px / focal))


def resample(traj_deg, step_ms=KNOT_STEP_MS):
    """把逐帧序列降采样到固定间隔的结点；时间不连续（丢帧）也能扛。"""
    if not traj_deg:
        return []
    knots = []
    next_t = traj_deg[0][0]
    i = 0
    while i < len(traj_deg):
        t = next_t
        # 找到跨越 t 的那一段
        while i + 1 < len(traj_deg) and traj_deg[i + 1][0] < t:
            i += 1
        if i + 1 < len(traj_deg):
            t0, x0, y0 = traj_deg[i]
            t1, x1, y1 = traj_deg[i + 1]
            span = t1 - t0
            u = 0.0 if span <= 1e-6 else (t - t0) / span
            u = min(1.0, max(0.0, u))
            knots.append((t, x0 + (x1 - x0) * u, y0 + (y1 - y0) * u))
        else:
            knots.append(traj_deg[-1])
            break
        next_t += step_ms
        if next_t > traj_deg[-1][0]:
            break
    # 末点总是收进来，否则最后一小段永远缺一个结点
    if knots and abs(knots[-1][0] - traj_deg[-1][0]) > 1e-3:
        knots.append(traj_deg[-1])
    return knots


def build(name: str, src: dict, fov_deg: float, note: str = "") -> dict:
    fps = float(_first(src, ("fps", "video_fps", "frame_rate"), 0.0) or 0.0)
    focal = _focal_px(src)
    res = _first(src, ("resolution", "res", "video_resolution"), [0, 0])
    res = [int(res[0]), int(res[1])] if isinstance(res, (list, tuple)) and len(res) >= 2 else [0, 0]

    traj_px = _trajectory(src)
    if not traj_px:
        raise SystemExit("输入里找不到 trajectory —— 这台分析器版本不认识，先看一眼 JSON 的顶层键。")

    # 相对第一发：压枪补的是「这一梭相对开火前的位移」，绝对位置无意义
    t0 = traj_px[0][0]
    x0 = traj_px[0][1]
    y0 = traj_px[0][2]
    traj_deg = [(t - t0, px_to_deg(x - x0, focal), px_to_deg(y - y0, focal))
                for (t, x, y) in traj_px]

    knots = resample(traj_deg)

    shots_ms = _shot_times(src, traj_deg, fps)
    # 发数时刻也相对第一发
    shots_ms = [max(0.0, s - (shots_ms[0] if shots_ms else 0.0)) for s in shots_ms]
    marks = [{"n": i + 1, "t": round(s, 3)} for i, s in enumerate(shots_ms)]

    xs = [k[1] for k in knots]
    ys = [k[2] for k in knots]
    dx_peak = (max(xs) - min(xs)) if xs else 0.0     # 峰峰值，不是末值
    dy_peak = (max(ys) - min(ys)) if ys else 0.0

    # ── 入库闸门（计划 §1.5）：不合格的档案要看得出来，不能悄悄缺一块 ──────────
    cls = "standard"
    reasons = []
    if len(knots) < 4:
        cls = "too_short"
        reasons.append("结点太少（%d）" % len(knots))
    if dx_peak < 1e-3 and dy_peak < 1e-3:
        cls = "flat"
        reasons.append("整条弹道是平的 —— 多半是跟踪目标选错了")
    if not marks:
        cls = "no_shots"
        reasons.append("没有发数标记，无法做逐发微调")
    if focal <= 0.0:
        reasons.append("focal_px 缺失且无法从 FOV 反推，输出的是像素不是度")
    # 发数时刻超出轨迹跨度 = 轨迹比弹夹短（录早停了）或 rpm/弹容对不上。
    # 不做修正：静默挪动发数标记比留着让人看见更糟。
    if marks and knots:
        span = knots[-1][0]   # 此处 knots 还是 (t, x, y) 元组，尚未转成 dict
        if marks[-1]["t"] > span * 1.5:
            reasons.append("最后一发在 %.0f ms，但轨迹只到 %.0f ms —— "
                           "弹容/rpm 与轨迹长度对不上，逐发微调会错位"
                           % (marks[-1]["t"], span))
    # 垂直方向：枪口上抬 ⇒ 视角被顶下去 ⇒ dy 应为正。整条为负多半是分析器的
    # y 轴朝上，设备上用「上下翻转」开关救，这里只提醒。
    if knots and max(k[2] for k in knots) < 1e-3:
        reasons.append("垂直弹道整体为负 —— 分析器的 y 轴可能朝上；"
                       "设备上把「上下翻转」打开即可，不必改档案")

    return {
        "schema": SCHEMA,
        "name": name,
        "source": "analyser",
        "fps": fps,
        "res": res,
        "fov_deg": fov_deg,
        "focal_px": focal,
        "note": note or _first(src, ("note", "weapon", "gun"), ""),
        "quality": {
            "recoil_class": cls,
            "min_conf": float(_first(src, ("min_conf",), 1.0) or 1.0),
            "mean_conf": float(_first(src, ("mean_conf",), 1.0) or 1.0),
            "shots": len(marks),
            "shots_match_magazine": bool(_first(src, ("shots_match_magazine",), True)),
        },
        "peak_deg": {"x": round(dx_peak, 5), "y": round(dy_peak, 5)},
        "knots": [{"t": round(t, 3), "x": round(x, 5), "y": round(y, 5)} for (t, x, y) in knots],
        "marks": marks,
    }, reasons


def plot(profile: dict, travel_x: float, travel_y: float):
    """θ(t) 与 c(t) 画在一张图上 —— 符号写反、相位错，这一步肉眼就能抓出来。"""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        print("[warn] 没有 matplotlib，跳过回放图（不影响 JSON 输出）")
        return None

    k = profile["knots"]
    t = [e["t"] for e in k]
    x = [e["x"] for e in k]
    y = [e["y"] for e in k]
    sx = travel_x / profile["peak_deg"]["x"] if profile["peak_deg"]["x"] > 1e-6 else 0.0
    sy = travel_y / profile["peak_deg"]["y"] if profile["peak_deg"]["y"] > 1e-6 else 0.0

    fig, ax = plt.subplots(1, 2, figsize=(11, 5))
    ax[0].plot(x, t, "b-", lw=1.4, label="水平 θ")
    ax[0].plot([-sx * v for v in x], t, "b--", lw=1.0, label="补偿 c（finger px）")
    ax[0].set_ylabel("ms")
    ax[0].invert_yaxis()
    ax[0].set_title("水平：θ 与 c 应当**镜像**（一正一负）")
    ax[0].legend(fontsize=8)
    ax[0].grid(alpha=0.3)

    ax[1].plot(y, t, "r-", lw=1.4, label="垂直 θ（正=往下看，符合枪口上抬）")
    ax[1].plot([-sy * v for v in y], t, "r--", lw=1.0, label="补偿 c")
    ax[1].invert_yaxis()
    ax[1].set_title("垂直：θ 为正 = 视角被顶下去")
    ax[1].legend(fontsize=8)
    ax[1].grid(alpha=0.3)

    fig.suptitle("%s · %d 发 · 水平峰峰 %.3f° / 垂直峰峰 %.3f°"
                 % (profile["name"], profile["quality"]["shots"],
                    profile["peak_deg"]["x"], profile["peak_deg"]["y"]))
    fig.tight_layout()
    p = "%s_recoil_check.png" % profile["name"]
    fig.savefig(p, dpi=110)
    print("回放图 -> %s" % p)
    return p


def main():
    ap = argparse.ArgumentParser(description="recoil-analyser JSON → aimbotng 档案 JSON")
    ap.add_argument("--in", dest="inp", required=True, help="分析器输出的 JSON")
    ap.add_argument("--name", required=True, help="档案名，例如 AKM")
    ap.add_argument("--out", default=".", help="输出目录（默认当前目录）")
    ap.add_argument("--fov", type=float, default=DEFAULT_FOV_DEG,
                    help="垂直 FOV，仅当分析器没给 focal_px 时用来反推（默认 80）")
    ap.add_argument("--note", default="", help="备注，会显示在下拉旁边")
    ap.add_argument("--plot", action="store_true", help="顺手画一张 c(t)/θ(t) 对照图")
    ap.add_argument("--travel-x", type=float, default=220.0, help="画图用的整梭行程·水平")
    ap.add_argument("--travel-y", type=float, default=0.0, help="画图用的整梭行程·垂直")
    a = ap.parse_args()

    with open(a.inp, "r", encoding="utf-8") as f:
        src = json.load(f)

    profile, reasons = build(a.name, src, a.fov, a.note)

    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, "%s.json" % a.name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(profile, f, ensure_ascii=False, indent=1)

    q = profile["quality"]
    print("写入 %s" % path)
    print("  %d 个结点，跨 %.0f ms；%d 发" % (len(profile["knots"]),
                                            profile["knots"][-1]["t"], q["shots"]))
    print("  峰峰 水平 %.3f° / 垂直 %.3f°；类 = %s"
          % (profile["peak_deg"]["x"], profile["peak_deg"]["y"], q["recoil_class"]))
    for r in reasons:
        print("  ! %s" % r)
    if q["recoil_class"] != "standard":
        print("  → 这个类在设备上是**不可驱动**的，先解决上面的问题")

    alpha_x = a.travel_x / profile["peak_deg"]["x"] if profile["peak_deg"]["x"] > 1e-6 else 0.0
    if alpha_x > 0:
        print("  反解 α（水平）= %.4f 度/finger px —— 同一灵敏度下可直接套别的枪"
              % (1.0 / alpha_x if alpha_x else 0.0))

    if a.plot:
        plot(profile, a.travel_x, a.travel_y)

    print("\n推到设备：")
    print("  adb push %s /data/local/tmp/aimbotng/profiles/" % path)


if __name__ == "__main__":
    sys.exit(main())
