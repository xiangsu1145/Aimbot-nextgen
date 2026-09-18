#!/bin/sh
# Dynamic shell-daemon launcher for the SF-projection A/B work.
#
# The device's old /data/local/tmp/aimbot_launch.sh hard-codes a stale APK path
# (the hash changes on every `adb install -r`), so it breaks after a reinstall.
# This one resolves the live path with `pm path` every run, and detaches with
# setsid so the daemon outlives the adb shell session.
#
#   adb push tools/aimbot_launch_dyn.sh /data/local/tmp/
#   adb shell "sh /data/local/tmp/aimbot_launch_dyn.sh"
#
# The listening port lands in /data/local/tmp/aimbot_shell.port a moment later.

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
if [ -z "$APK" ]; then
  echo "APK not found for $PKG — is the app installed?"
  exit 1
fi
LIBDIR="$(dirname "$APK")/lib/arm64"
echo "APK=$APK"
echo "LIBDIR=$LIBDIR"

setsid /system/bin/app_process \
  -Djava.class.path="$APK" \
  -Djava.library.path='/apex/com.android.runtime/lib64/bionic:/system/lib64:/system_ext/lib64:/vendor/lib64:/vendor/lib64/hw:/vendor/lib64/egl:/odm/lib64:/product/lib64' \
  /system/bin --nice-name=aimbot_shell \
  io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR" \
  >/data/local/tmp/aimbot_daemon.log 2>&1 &

sleep 3
echo "launched (detached). port -> /data/local/tmp/aimbot_shell.port"
