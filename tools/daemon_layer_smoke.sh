#!/bin/sh
# Runs ShellServerEntry as shell (uid 2000) and drives the throw-away layer
# experiment without needing the app UI. Paths are resolved at run time, so this
# keeps working after every reinstall.
#
# Phase A (t≈2s .. 27s): layer on screen, screenshot flag OFF
# Phase B (t≈27s .. 47s): screenshot flag ON
# Then the layer is released.
#
#   adb push tools/daemon_layer_smoke.sh /data/local/tmp/
#   adb shell "sh /data/local/tmp/daemon_layer_smoke.sh"

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
PKG_DIR=$(dirname "$APK")
LIBDIR="$PKG_DIR/lib/arm64"

echo "APK=$APK"
echo "LIBDIR=$LIBDIR"

{
    echo "SET_RESOLUTION 2120 3000 0"
    sleep 2
    echo "TESTLAYER"
    sleep 25
    echo "TESTLAYER_SKIP 1"
    sleep 20
    echo "TESTLAYER_OFF"
    sleep 2
} | /system/bin/app_process -Djava.class.path="$APK" /system/bin \
    --nice-name=aimbot_layertest \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR"
