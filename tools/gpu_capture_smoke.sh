#!/bin/sh
# Times the CPU capture path against the GPU one, headlessly.
#
# Runs the shell daemon, drives capture + inference from its command stream,
# and flips debug.aimbotng.gpuproc halfway so both halves land in one log.
#
#   adb push tools/gpu_capture_smoke.sh /data/local/tmp/
#   adb shell "sh /data/local/tmp/gpu_capture_smoke.sh"
#
# Watch it with:
#   adb shell logcat -v brief -s AimbotCapture:V AimbotGpuProc:V AimbotInfer:V AimbotNg:V '*:S'

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
LIBDIR="$(dirname "$APK")/lib/arm64"

W=${1:-2120}
H=${2:-3000}
SECS=${3:-12}

echo "APK=$APK"
echo "LIBDIR=$LIBDIR"
echo "screen=${W}x${H} phase=${SECS}s"

{
    echo "SET_RESOLUTION $W $H 0"
    sleep 2
    echo "CAPTURE ON"
    echo "INFER ON"
    echo "GPU OFF"
    echo "=== PHASE A: cpu ==="
    sleep "$SECS"
    echo "INFER ?"
    echo "GPU ?"

    echo "GPU ON"
    echo "=== PHASE B: gpu ==="
    sleep "$SECS"
    echo "INFER ?"
    echo "GPU ?"

    echo "=== PHASE C: cpu again ==="
    echo "GPU OFF"
    sleep "$SECS"
    echo "INFER ?"
    echo "GPU ?"

    echo "INFER OFF"
    echo "CAPTURE OFF"
    sleep 2
# `timeout` matters: the daemon has a listening socket and a panel grab, so it
# does not always wind down on its own when stdin closes — and an adb shell
# waiting on it would hang the run.
} | timeout $((SECS * 3 + 30)) /system/bin/app_process \
    -Djava.class.path="$APK" \
    -Djava.library.path='/apex/com.android.runtime/lib64/bionic:/system/lib64:/system_ext/lib64:/vendor/lib64:/vendor/lib64/hw:/vendor/lib64/egl:/odm/lib64:/product/lib64' \
    /system/bin --nice-name=aimbot_gputest \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR"
