#!/bin/sh
# A/B demo for the daemon-owned layer + anti-capture flag.
#
# The point of this run is to answer ONE question with eyes, not with screencap:
#   when setSkipScreenshot(true) is on, does the block STILL show on the physical
#   panel (=> capture-only hiding, the win we want), or does it vanish from the
#   display too (=> useless for our purpose)?
#
# The layer is created ONCE and never released until the end, so the only thing
# that changes between phases is the skip-screenshot flag. If the block comes
# BACK at phase D, that proves it was never removed — only the flag changed.
#
#   phase A (t=2s  .. 14s)  skip OFF   -> block visible          (baseline)
#   phase B (t=14s .. 36s)  skip ON    -> still visible? <-- THE QUESTION
#   phase C (t=36s .. 48s)  skip OFF   -> visible again
#   phase D (t=48s)         released   -> gone
#
#   adb push tools/daemon_layer_ab.sh /data/local/tmp/
#   adb shell "sh /data/local/tmp/daemon_layer_ab.sh"

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
PKG_DIR=$(dirname "$APK")
LIBDIR="$PKG_DIR/lib/arm64"

echo "APK=$APK"

{
    echo "SET_RESOLUTION 2120 3000 0"
    sleep 2
    echo "TESTLAYER"
    echo "PHASE A skip=OFF  (block should be VISIBLE)   t=2"
    sleep 12
    echo "TESTLAYER_SKIP 1"
    echo "PHASE B skip=ON   (still visible?)            t=14"
    sleep 22
    echo "TESTLAYER_SKIP 0"
    echo "PHASE C skip=OFF  (visible again?)            t=36"
    sleep 12
    echo "PHASE D releasing"
    echo "TESTLAYER_OFF"
    sleep 3
} | /system/bin/app_process -Djava.class.path="$APK" /system/bin \
    --nice-name=aimbot_layertest \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR"
