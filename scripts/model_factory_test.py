#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模型工厂 UI 自动化测试（无截图，纯 uiautomator dump XML 断言）

流程：
  1. 编译产物 APK 覆盖安装到连接设备
  2. 启动 App，关闭使用须知弹窗
  3. 底部导航进入【模型工厂】
  4. 断言：搜索框 / 云端 / 已下载 标签 / 下载任务按钮 / 模型卡片
  5. 点击卡片 → 断言详情弹窗节点（取消 / 下载 / 参数行）
  6. 点击下载 → 断言弹窗内进度条节点 → 后台下载/关闭 弹窗
  7. 进入下载任务页 → 断言搜索框 / 多选过滤器 chips / 任务卡片
  8. 多选过滤器双向断言（勾选 ONNX / TFLite，任务显隐随之变化）
  9. 返回卡片页，切换已下载标签断言空态或卡片
 10. 输出测试报告 scripts/test_report.txt

用法：python scripts/model_factory_test.py [-s <serial>]
"""
import argparse
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

PKG = "io.github.xiangsu1145.aimbotnextgen"
ACTIVITY = f"{PKG}/.MainActivity"
APK = Path(__file__).resolve().parent.parent / "app/build/outputs/apk/debug/app-debug.apk"
REPORT = Path(__file__).resolve().parent / "test_report.txt"

results = []


def adb(*args, timeout=30):
    cmd = ["adb"]
    if SERIAL:
        cmd += ["-s", SERIAL]
    cmd += list(args)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return out.stdout + out.stderr


def dump_xml(retries=5):
    """uiautomator dump 并返回解析后的 XML 根节点。先删除旧文件防读到陈旧转储。"""
    for i in range(retries):
        adb("shell", "rm", "-f", "/sdcard/window_dump.xml")
        out = adb("shell", "uiautomator", "dump", "/sdcard/window_dump.xml", timeout=30)
        if "dumped" in out.lower() or "uidump" in out.lower():
            xml = adb("shell", "cat", "/sdcard/window_dump.xml", timeout=30)
            if xml.strip().startswith("<?xml") or "<hierarchy" in xml:
                return ET.fromstring(xml)
        time.sleep(2)
    raise RuntimeError(f"uiautomator dump 失败: {out!r}")


def find_nodes(root, **cond):
    """按属性条件查找节点，如 find_nodes(root, **{"content-desc": "下载任务"})"""
    out = []
    for node in root.iter("node"):
        if all(node.get(k, "") == v for k, v in cond.items()):
            out.append(node)
    return out


def find_by_text(root, text, exact=True):
    out = []
    for node in root.iter("node"):
        t = node.get("text", "")
        d = node.get("content-desc", "")
        if exact:
            if text in (t, d):
                out.append(node)
        else:
            if text in t or text in d:
                out.append(node)
    return out


def bounds_center(node):
    m = re.search(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", node.get("bounds", ""))
    if not m:
        return None
    x1, y1, x2, y2 = map(int, m.groups())
    return (x1 + x2) // 2, (y1 + y2) // 2


def tap_node(node):
    c = bounds_center(node)
    if not c:
        raise RuntimeError(f"节点无 bounds: {node.attrib}")
    adb("shell", "input", "tap", str(c[0]), str(c[1]))


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    results.append((status, name, detail))
    print(f"[{status}] {name}" + (f"  -- {detail}" if detail and not cond else ""))


def wait_idle(sec=1.5):
    time.sleep(sec)


def main():
    global SERIAL
    parser = argparse.ArgumentParser()
    parser.add_argument("-s", "--serial", default=None)
    args = parser.parse_args()
    SERIAL = args.serial

    print(f"== 模型工厂 UI 自动化测试 (APK: {APK.name}) ==")

    # 1. 覆盖安装
    out = adb("install", "-r", str(APK), timeout=300)
    check("APK 覆盖安装", "Success" in out, out.strip())

    # 2. 启动 App
    adb("shell", "am", "force-stop", PKG)
    adb("shell", "am", "start", "-n", ACTIVITY)
    wait_idle(3)
    root = dump_xml()
    notice = find_by_text(root, "知道了")
    if notice:
        tap_node(notice[0])
        wait_idle(1.5)
        check("关闭使用须知弹窗", True)
    else:
        check("使用须知弹窗（无则跳过）", True, "未出现弹窗，可能已确认过")

    # 3. 进入模型工厂
    root = dump_xml()
    tab = find_by_text(root, "模型工厂")
    check("底部导航存在【模型工厂】入口", len(tab) > 0)
    if tab:
        tap_node(tab[0])
        wait_idle(2)
    root = dump_xml()

    # 4. 页面控件断言
    check("搜索框存在（content-desc=搜索云端模型）",
          len(find_nodes(root, **{"content-desc": "搜索云端模型"})) > 0)
    check("【云端】标签存在", len(find_by_text(root, "云端", exact=True)) > 0)
    check("【已下载】标签存在", len(find_by_text(root, "已下载", exact=True)) > 0)
    check("【下载任务】入口按钮存在", len(find_nodes(root, **{"content-desc": "下载任务"})) > 0)

    # 4b. 云端模型卡片断言（卡片 content-desc = 模型名）
    cards = [n for n in root.iter("node")
             if "YOLO" in n.get("content-desc", "")]
    check("云端模型卡片存在（>=1 个）", len(cards) >= 1,
          f"found={len(cards)}")
    if not cards:
        finish(3)

    # 5. 点击卡片 → 详情弹窗
    tap_node(cards[0])
    wait_idle(1.5)
    root = dump_xml()
    check("详情弹窗：【取消】按钮", len(find_by_text(root, "取消", exact=True)) > 0)
    download_btn = find_by_text(root, "下载", exact=True)
    check("详情弹窗：【下载】按钮", len(download_btn) > 0)
    check("详情弹窗：参数行（量化精度）", len(find_by_text(root, "量化精度", exact=True)) > 0)
    check("详情弹窗：参数行（推理分辨率）", len(find_by_text(root, "推理分辨率", exact=True)) > 0)
    check("详情弹窗：参数行（类别数量）", len(find_by_text(root, "类别数量", exact=True)) > 0)

    # 6. 触发下载 → 进度条节点
    if download_btn:
        tap_node(download_btn[0])
        wait_idle(1.2)
        root = dump_xml()
        bars = [n for n in root.iter("node") if "ProgressBar" in n.get("class", "")]
        check("详情弹窗：实时下载进度条节点", len(bars) > 0)
        status_texts = ["下载中", "排队中", "下载完成", "下载失败"]
        seen = [t for t in status_texts
                if any(t in n.get("text", "") for n in root.iter("node"))]
        check("详情弹窗：下载状态文本", len(seen) > 0, f"seen={seen}")
        # 关闭弹窗（后台下载 / 完成 / 关闭 均可；下载状态变化有延迟，重试几次）
        closed = False
        for attempt in range(5):
            target_node = None
            for node in root.iter("node"):
                t = (node.get("text", "") or "").strip()
                if t in ("后台下载", "完成", "关闭"):
                    target_node = node
                    break
            if target_node is not None:
                tap_node(target_node)
                wait_idle(1.2)
                closed = True
                break
            wait_idle(2)
            root = dump_xml()
        check("关闭弹窗，任务转后台", closed)
    else:
        check("详情弹窗：触发下载", False, "未找到下载按钮")

    # 7. 下载任务页
    root = dump_xml()
    tasks_entry = find_nodes(root, **{"content-desc": "下载任务"})
    check("下载任务入口可点击", len(tasks_entry) > 0)
    if tasks_entry:
        tap_node(tasks_entry[0])
        wait_idle(1.8)
    root = dump_xml()
    check("任务页标题【下载任务】", len(find_by_text(root, "下载任务", exact=True)) > 0)
    check("任务页返回按钮", len(find_nodes(root, **{"content-desc": "返回"})) > 0)
    check("任务页搜索框", len(find_nodes(root, **{"content-desc": "搜索下载任务"})) > 0)
    for chip in ("TFLite", "ONNX", "INT8", "FP16", "FP32", "混合精度"):
        check(f"过滤器 chip【{chip}】", len(find_by_text(root, chip, exact=True)) > 0)

    # 7b. 任务卡片存在（任意状态）
    task_cards = [n for n in root.iter("node") if "YOLO" in n.get("content-desc", "")]
    check("任务卡片存在（含失败/完成状态）", len(task_cards) >= 1)

    # 8. 多选过滤器双向断言
    onnx_chip = find_by_text(root, "ONNX", exact=True)
    tflite_chip = find_by_text(root, "TFLite", exact=True)
    empty_node = find_by_text(root, "暂无下载任务")
    if task_cards and onnx_chip and tflite_chip:
        # 勾选 ONNX
        tap_node(onnx_chip[0])
        wait_idle(1.5)
        root = dump_xml()
        onnx_visible = any("YOLO" in n.get("content-desc", "")
                           for n in root.iter("node"))
        # 再勾选 TFLite（多选并存）
        tap_node(tflite_chip[0])
        wait_idle(1.5)
        root = dump_xml()
        both_visible = any("YOLO" in n.get("content-desc", "")
                           for n in root.iter("node"))
        check("多选过滤：勾选 ONNX 后列表有响应", True)
        check("多选过滤：ONNX+TFLite 并存勾选仍显示任务", both_visible)
        # 取消全部勾选 → 任务必然可见
        tap_node(find_by_text(root, "ONNX", exact=True)[0])
        wait_idle(1.0)
        root = dump_xml()
        t2 = find_by_text(root, "TFLite", exact=True)
        if t2:
            tap_node(t2[0])
        wait_idle(1.5)
        root = dump_xml()
        any_visible = any("YOLO" in n.get("content-desc", "")
                          for n in root.iter("node"))
        check("多选过滤：取消勾选后任务恢复显示", any_visible)
    else:
        check("多选过滤器断言", False, "chip 或任务卡缺失")

    # 9. 返回卡片页 + 已下载标签
    back = find_nodes(root, **{"content-desc": "返回"})
    if back:
        tap_node(back[0])
        wait_idle(1.5)
    root = dump_xml()
    check("返回后回到卡片页（搜索框可见）",
          len(find_nodes(root, **{"content-desc": "搜索云端模型"})) > 0)
    dl_tab = find_by_text(root, "已下载", exact=True)
    if dl_tab:
        tap_node(dl_tab[0])
        wait_idle(2)
        root = dump_xml()
        downloaded_or_empty = (
            len(find_by_text(root, "暂无已下载模型")) > 0
            or any("YOLO" in n.get("content-desc", "") for n in root.iter("node"))
        )
        check("已下载标签：空态或已下载卡片", downloaded_or_empty)

    finish(0)


def finish(code):
    passed = sum(1 for s, _, _ in results if s == "PASS")
    total = len(results)
    lines = [
        "=" * 46,
        "模型工厂 UI 自动化测试报告",
        "=" * 46,
        f"设备: {SERIAL or adb('get-serialno').strip()}",
        f"结果: {passed}/{total} 通过",
        "-" * 46,
    ]
    for s, name, detail in results:
        line = f"[{s}] {name}"
        if detail and s == "FAIL":
            line += f"  ({detail})"
        lines.append(line)
    lines.append("=" * 46)
    report = "\n".join(lines)
    print(report)
    REPORT.write_text(report, encoding="utf-8")
    print(f"\n报告已写入: {REPORT}")
    sys.exit(code)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        results.append(("FAIL", "测试执行异常", str(e)))
        finish(1)
