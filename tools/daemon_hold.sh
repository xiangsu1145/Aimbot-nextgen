#!/bin/sh
# Holds a shell daemon up for a fixed time, doing nothing.
#
# The daemon reads its command stream over TCP, not stdin — but it exits as soon
# as stdin closes, so something has to keep that pipe open. That something is
# this script, which is why it has to run in the *foreground* of an adb shell:
# a backgrounded daemon is taken down with the process group when adbd closes
# the session that started it.
#
#   adb shell "sh /data/local/tmp/daemon_hold.sh 180"
#   # then, from the host:  adb forward tcp:<port> tcp:<port>
#
# The port lands in /data/local/tmp/aimbot_shell.port a second or two in.

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
LIBDIR="$(dirname "$APK")/lib/arm64"
SECS=${1:-180}

echo "APK=$APK"
echo "LIBDIR=$LIBDIR"
echo "holding for ${SECS}s"

(sleep "$SECS") | timeout "$SECS" /system/bin/app_process \
    -Djava.class.path="$APK" \
    -Djava.library.path='/apex/com.android.runtime/lib64/bionic:/system/lib64:/system_ext/lib64:/vendor/lib64:/vendor/lib64/hw:/vendor/lib64/egl:/odm/lib64:/product/lib64' \
    /system/bin --nice-name=aimbot_hold \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR"
