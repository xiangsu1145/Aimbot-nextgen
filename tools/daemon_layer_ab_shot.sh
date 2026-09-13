#!/bin/sh
# Synchronised A/B demo: the screenshot is taken from INSIDE the same shell
# sequence that drives the daemon, so there is no host-side timing race at all.
#
#   ab_A.png  skip OFF -> must CONTAIN the block
#   ab_B.png  skip ON  -> must NOT contain the block
#   ab_C.png  skip OFF -> must CONTAIN the block again
#
# A and C both showing the block proves the layer was never removed: only the
# capture flag changed. The panel is woken first, because screencap of a
# sleeping display is all black.
#
#   adb push tools/daemon_layer_ab_shot.sh /data/local/tmp/
#   adb shell "sh /data/local/tmp/daemon_layer_ab_shot.sh"

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
PKG_DIR=$(dirname "$APK")
LIBDIR="$PKG_DIR/lib/arm64"

rm -f /data/local/tmp/ab_*.png /data/local/tmp/ab.log

input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
sleep 1

{
    echo "SET_RESOLUTION 2120 3000 0"
    sleep 3

    echo "TESTLAYER"
    sleep 5
    screencap -p /data/local/tmp/ab_A.png

    echo "TESTLAYER_SKIP 1"
    sleep 5
    screencap -p /data/local/tmp/ab_B.png

    echo "TESTLAYER_SKIP 0"
    sleep 5
    screencap -p /data/local/tmp/ab_C.png

    echo "TESTLAYER_OFF"
    sleep 3
} | /system/bin/app_process -Djava.class.path="$APK" /system/bin \
    --nice-name=aimbot_layertest \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR" \
    > /data/local/tmp/ab.log 2>&1

echo "DONE"
ls -l /data/local/tmp/ab_*.png
echo "==== daemon log ===="
cat /data/local/tmp/ab.log
