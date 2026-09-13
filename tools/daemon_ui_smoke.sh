#!/bin/sh
# Drives the daemon-side menu layer + ImGui renderer without the app UI.
#
#   UI_ON  -> ShellLayerHost builds the layer on SurfaceFlinger and starts the
#             Vulkan/ImGui renderer inside the shell process (uid 2000).
#   UI_OFF -> tears it down.
#
# No OPEN is sent on purpose: without EVIOCGRAB the panel is untouched, so this
# test cannot interfere with the device's own touch. The layer is not a window
# and has no input channel, so it cannot swallow touches either.
#
#   adb push tools/daemon_ui_smoke.sh /data/local/tmp/
#   adb shell "sh /data/local/tmp/daemon_ui_smoke.sh"

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
PKG_DIR=$(dirname "$APK")
LIBDIR="$PKG_DIR/lib/arm64"

echo "APK=$APK"

{
    echo "SET_RESOLUTION 3000 2120 1"
    sleep 2
    echo "UI_ON"
    sleep 20
    echo "UI_OFF"
    sleep 2
} | /system/bin/app_process -Djava.class.path="$APK" /system/bin \
    --nice-name=aimbot_uitest \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR"
