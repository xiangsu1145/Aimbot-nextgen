#!/system/bin/sh
# TEMP verification helper — drives the shell daemon without the app, so the
# capture page can be built, rendered and screenshotted from adb alone.
#   usage: sh verify_capture_dev.sh <apk> <libDir> <w> <h> <seconds>
#
# stdin is fed by a device-side subshell rather than a FIFO: /data/local/tmp
# refuses mkfifo under the shell's SELinux context, and adb does not forward a
# host pipe to a non-interactive `adb shell`.
APK="$1"
LIB="$2"
W="${3:-2120}"
H="${4:-3000}"
HOLD="${5:-30}"

OUT=/data/local/tmp/aimbot_out
rm -f "$OUT"

(
  echo "SET_RESOLUTION $W $H 0"
  sleep 1
  echo "UI_ON"
  sleep "$HOLD"
  echo "UI_OFF"
  sleep 1
  echo "DESTROY"
) | /system/bin/app_process -Djava.class.path="$APK" /system/bin --nice-name=aimbot_shell \
      io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIB" > "$OUT" 2>&1

echo "daemon exited rc=$?"
echo "--- daemon stdout ---"
cat "$OUT"
