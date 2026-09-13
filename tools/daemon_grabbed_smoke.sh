#!/system/bin/sh
# Smoke test for the daemon's GRABBED report, run from a plain `adb shell`
# (shell UID 2000 has the /dev/input + /dev/uinput access the daemon needs).
#
#   adb push tools/daemon_grabbed_smoke.sh /data/local/tmp/ && \
#   adb shell sh /data/local/tmp/daemon_grabbed_smoke.sh
#
# Expect: READY / OK / OK:grabbed=1 / OK:grabbed=1 / OK:grabbed=0
# The panel is grabbed for only a few milliseconds; stdin EOF makes the daemon
# release it and exit on its own. Uses a distinct nice-name so it can never be
# confused with a real session.
APK=/data/app/~~tbj2wOysn75LleKGfio1pw==/io.github.xiangsu1145.aimbotnextgen-bcPCGaqZG78s4cQK9dY8fA==/base.apk
LIB=/data/app/~~tbj2wOysn75LleKGfio1pw==/io.github.xiangsu1145.aimbotnextgen-bcPCGaqZG78s4cQK9dY8fA==/lib/arm64

printf 'SET_RESOLUTION 2120 3000 0\nOPEN\nGRABBED\nCLOSE\nGRABBED\n' | \
  /system/bin/app_process -Djava.class.path="$APK" /system/bin \
  --nice-name=aimbot_smoke \
  io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIB"
echo "exit=$?"
