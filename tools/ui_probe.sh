#!/bin/sh
# Frame-rate probe: brings the menu layer up for a fixed window so the caller can
# sample `dumpsys display` / `dumpsys SurfaceFlinger` while it renders.
#
#   sh /data/local/tmp/ui_probe.sh <w> <h> <rotation> [seconds]
#
# W/H/rotation must describe the device's CURRENT orientation, because the layer
# is full-screen and its swapchain takes the surface size at face value.

W="$1"; H="$2"; ROT="$3"; HOLD="${4:-18}"

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
PKG_DIR=$(dirname "$APK")
LIBDIR="$PKG_DIR/lib/arm64"

echo "APK=$APK probing ${W}x${H} rot=$ROT for ${HOLD}s"

{
    echo "SET_RESOLUTION $W $H $ROT"
    sleep 2
    echo "UI_ON"
    sleep "$HOLD"
    echo "UI_OFF"
    sleep 2
} | /system/bin/app_process -Djava.class.path="$APK" /system/bin \
    --nice-name=aimbot_uiprobe \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR"
