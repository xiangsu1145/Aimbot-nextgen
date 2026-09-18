#!/system/bin/sh
# 一次性收集 aimbot daemon 诊断信息到 /sdcard/Download/
# 用法（PC 端）：
#   adb push tools/diag_collect.sh /data/local/tmp/
#   adb shell sh /data/local/tmp/diag_collect.sh
#   adb pull /sdcard/Download/aimbot_diag.tar.gz
# 然后把 aimbot_diag.tar.gz 发回来。
# 一次性脚本，跑完即弃，不留痕迹。
#
# ── 抓 logcat 的正确命令（重要） ─────────────────────────────────────────
#
# 旧命令：
#   logcat -v time AimbotReader:V AimbotInput:V AimbotNg:V AimbotCapture:V \
#          AimbotInfer:V AimbotModel:V AimbotTouch:V *:E > /sdcard/aimbot_shell.log &
#
# 它把「没被点名的 tag」压到 E 级，而 **daemon 自己的 tag 就是那个没被点名的**：
# daemon 的 Kotlin 侧全部用 Log.i，于是全被吃掉。实测 5 份 log 里
# I/aimbot_shell 与 W/aimbot_shell 都是 0 行 —— 我们因此一直看不到
# OPEN: / daemon ready / 退出序列 / 状态心跳，只能靠原生那一半猜。
#
# 正确命令（多一个 aimbot_shell:V）：
#   logcat -v time AimbotReader:V AimbotInput:V AimbotNg:V AimbotCapture:V \
#          AimbotInfer:V AimbotModel:V AimbotTouch:V aimbot_shell:V \
#          *:E > /sdcard/aimbot_shell.log &
#
# 另外：logcat 里看不到 InputReader 是否接纳了我们的克隆设备，那只有
# `dumpsys input` 有 —— 见下面第 [10] 段，别删。

set +e

OUT=/sdcard/Download/aimbot_diag
rm -rf "$OUT"
mkdir -p "$OUT"
LOG="$OUT/diag.txt"

{
echo "=========================================="
echo "  Aimbot Nextgen - 设备诊断收集"
echo "  生成时间: $(date)"
echo "=========================================="
echo

echo "[1] 进程身份"
echo "uid/gid: $(id)"
echo

echo "[2] 设备指纹"
echo "ro.build.fingerprint = $(getprop ro.build.fingerprint)"
echo "ro.product.brand     = $(getprop ro.product.brand)"
echo "ro.product.model     = $(getprop ro.product.model)"
echo "ro.product.device    = $(getprop ro.product.device)"
echo "ro.build.version.release = $(getprop ro.build.version.release)"
echo "ro.build.version.sdk     = $(getprop ro.build.version.sdk)"
echo

echo "[3] aimbot_shell 进程"
echo "pidof aimbot_shell = $(pidof aimbot_shell)"
ps -A 2>/dev/null | grep -iE 'aimbot|app_process' | grep -v grep
echo

echo "[4] 端口文件"
ls -l /data/local/tmp/aimbot_shell.port 2>&1
echo "内容: $(cat /data/local/tmp/aimbot_shell.port 2>&1)"
echo

echo "[5] /dev/input 设备列表与权限"
ls -l /dev/input/ 2>&1
echo

echo "[6] shell uid 读取 input 设备测试"
echo "    (timeout 0.5 cat, rc=0=cat 正常等 EOF, rc=124=读到数据被超时打断=可读, 其他=权限拒绝)"
for ev in /dev/input/event*; do
    [ -e "$ev" ] || continue
    timeout 0.5 cat "$ev" >/dev/null 2>&1
    rc=$?
    if [ "$rc" = "0" ] || [ "$rc" = "124" ]; then
        echo "$ev : READABLE (rc=$rc)"
    else
        echo "$ev : NOT READABLE (rc=$rc)"
    fi
done
echo

echo "[7] getevent -lp (前 200 行)"
getevent -lp 2>&1 | head -200
echo

echo "[8] aimbot_daemon.log (完整, 文件不存在会标注)"
if [ -f /data/local/tmp/aimbot_daemon.log ]; then
    echo "文件大小: $(wc -c < /data/local/tmp/aimbot_daemon.log) bytes, $(wc -l < /data/local/tmp/aimbot_daemon.log) lines"
    echo "--- BEGIN aimbot_daemon.log ---"
    cat /data/local/tmp/aimbot_daemon.log
    echo "--- END aimbot_daemon.log ---"
else
    echo "(NO aimbot_daemon.log at /data/local/tmp/)"
fi
echo

echo "[9] 最近 logcat 中含 aimbot 的行 (最近 500 行内)"
logcat -d -t 500 2>/dev/null | grep -iE 'aimbot|ShellNative|ShellServer|ShellLayer' | tail -200
echo

# ── 关键区：系统到底有没有接纳我们克隆出来的那个设备 ──────────────────────
#
# 这是唯一能区分「我们的帧没送达」和「帧送达了系统不理会」的数据，而且它不在
# logcat 里。以前这里只打 `head -50`，那 50 行是 Disabled/Enabled 开关表，
# 设备清单在几百行之后 —— 等于什么都没抓到。
echo "[10] dumpsys input（完整落盘 + 关键切片）"
dumpsys input 2>/dev/null > "$OUT/dumpsys_input.txt"
echo "  完整输出: $OUT/dumpsys_input.txt ($(wc -l < "$OUT/dumpsys_input.txt") 行)"
echo
echo "  ---- 10.1 /dev/input 节点与 SELinux 标签 ----"
echo "  （我们克隆的节点应该和真面板同标签 u:object_r:input_device:s0；不同就是问题）"
ls -lZ /dev/input/ 2>&1
echo
echo "  ---- 10.2 InputReader 认到的设备清单 ----"
echo "  （找第二个 name 与真面板相同的条目，看它的 Classes / Enabled / Path）"
sed -n '/^  Devices:/,/^  Unattached video devices/p' "$OUT/dumpsys_input.txt" | head -300
echo
echo "  ---- 10.3 每个 InputReader 设备的 Classes / Sources / 关联显示 ----"
echo "  （我们那个必须有 Sources: ... TOUCHSCREEN、AssociatedDisplay true、Enabled true）"
grep -nE "^  Device [0-9]+:|Classes:|Enabled:|Path: /dev/input|Synced|AssociatedDisplay|Sources:|DeviceType:|Touch Input Mapper|Raw Touch Axes" \
    "$OUT/dumpsys_input.txt" | head -200
echo
echo "  ---- 10.4 InputDispatcher 触摸状态 ----"
sed -n '/^Input Dispatcher State/,/^$/p' "$OUT/dumpsys_input.txt" | head -140
grep -n "mCanceledDevices\|TouchStates\|touchableRegion\|mInboundQueue" "$OUT/dumpsys_input.txt" | head -30
echo
echo "  ---- 10.5 uinput 设备在 sysfs 里的样子 ----"
for d in /sys/class/input/input*; do
    [ -e "$d/name" ] || continue
    echo "$(basename "$d"): name=$(cat "$d/name" 2>/dev/null) phys=$(cat "$d/phys" 2>/dev/null)"
done
echo

echo "[11] SELinux 模式 (Enforcing/Permissive 影响 shell uid 权限)"
getenforce
echo

# AVC 不进 logcat 的 main buffer，只看得到内核侧。shell uid 读 dmesg 通常被拒，
# 所以两种都试，读不到就明确写出来，别让人以为"没有拒绝"。
echo "[12] SELinux AVC 拒绝（uinput 节点被 inputflinger/system_server 打开失败会记在这）"
echo "--- dmesg ---"
dmesg 2>/dev/null | grep -i "avc:" | tail -60
echo "--- logcat events buffer ---"
logcat -b events -d 2>/dev/null | grep -iE "avc|selinux" | tail -40
echo "(以上为空且 dmesg 被拒时不代表没有拒绝，只是读不到；用 root 再跑一次 dmesg | grep avc)"
echo

echo "=========================================="
echo "  收集完成"
echo "=========================================="

} > "$LOG" 2>&1

# 打包
cd /sdcard/Download
tar czf aimbot_diag.tar.gz aimbot_diag/

echo
echo "[done] 输出文件: /sdcard/Download/aimbot_diag.tar.gz"
ls -lh /sdcard/Download/aimbot_diag.tar.gz
