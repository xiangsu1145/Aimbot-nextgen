#!/bin/sh
# Verifies the daemon survives losing its client.
#
# The daemon used to exit the moment its single TCP connection died, which took
# the panel grab and the menu down with it. It now keeps running and goes back
# to accept(). This drives that path:
#
#   1. launch the daemon detached, read the port it published
#   2. forward it to the host so a plain client can dial in
#   3. connect, drive the menu up, then drop the connection
#   4. the daemon must still be alive, holding the menu; reconnect and it
#      should hand back its live state
#
# Run from the host with adb connected:
#   adb push tools/verify_session_recovery.sh /data/local/tmp/
#   adb shell "sh /data/local/tmp/verify_session_recovery.sh"

PKG="io.github.xiangsu1145.aimbotnextgen"
APK=$(pm path "$PKG" | head -1 | sed 's/^package://')
LIBDIR="$(dirname "$APK")/lib/arm64"

echo "APK=$APK"
echo "LIBDIR=$LIBDIR"

pkill -f aimbot_shell
sleep 1
rm -f /data/local/tmp/aimbot_shell.port

setsid /system/bin/app_process -Djava.class.path="$APK" /system/bin \
    --nice-name=aimbot_shell \
    io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry "$LIBDIR" \
    >/dev/null 2>&1 &

for i in 1 2 3 4 5 6 7 8 9 10; do
    PORT=$(cat /data/local/tmp/aimbot_shell.port 2>/dev/null)
    [ -n "$PORT" ] && break
    sleep 1
done

if [ -z "$PORT" ]; then
    echo "FAIL: daemon never published a port"
    exit 1
fi

echo "PORT=$PORT"
echo "PID=$(pidof aimbot_shell)"
echo "Now from the host:  adb forward tcp:17321 tcp:$PORT"
