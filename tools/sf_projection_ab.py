#!/usr/bin/env python3
"""A/B the capture producers on a real device over the daemon's loopback port.

    MODE 0 = legacy  — full-panel virtual display + GPU/CPU crop
    MODE 1 = sfp     — SurfaceFlinger projection crops + scales into a small display

For each mode we flip `CAPTURE MODE`, wait for the mirror to come up, stir the
touchscreen so SurfaceFlinger actually composites frames, then read
`CAPTURE STATS` (handleMedianUs / fps / frames). The two numbers answer the
user's question directly: does the SF-projection path deliver frames faster and
without the GPU shader's one-frame latency?

    python tools/sf_projection_ab.py [--secs 10] [--install] [--launch]

Requires a device over adb. By default it expects a daemon already up
(use tools/aimbot_launch_dyn.sh). Pass --launch to start one here, and --install
to reinstall the debug APK first.
"""
import argparse
import socket
import subprocess
import sys
import threading
import time


def sh(cmd, check=False):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, check=check)


def adb(cmd, check=False):
    return sh("adb shell \"%s\"" % cmd.replace('"', '\\"'), check=check)


class Daemon:
    def __init__(self, port):
        self.port = port
        self.sock = None

    def connect(self, tries=40, delay=0.5):
        for _ in range(tries):
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                s.settimeout(5)
                self.sock = s
                return self._ready()
            except OSError:
                time.sleep(delay)
        raise SystemExit("daemon never answered on port %d" % self.port)

    def _ready(self):
        buf = b""
        while b"READY" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", "replace")

    def send(self, line, quiet=False):
        self.sock.sendall((line + "\n").encode())
        time.sleep(0.35)
        try:
            reply = self.sock.recv(8192).decode("utf-8", "replace")
        except socket.timeout:
            return ""
        if not quiet:
            for r in reply.splitlines():
                if r.strip():
                    print("   <- %s" % r.strip())
        return reply

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def stir(stop_after, event):
    """Keep SurfaceFlinger compositing by poking the touchscreen across a grid.

    A virtual display only receives a frame when the real panel actually
    re-composites, so a static home screen delivers almost nothing. Tapping a
    grid of points rapidly forces continuous input-driven compositing.
    """
    spots = [(300, 800), (900, 600), (500, 1200), (1200, 900),
             (700, 400), (200, 1400), (1000, 1100), (600, 700)]
    i = 0
    end = time.time() + stop_after
    while time.time() < end and not event.is_set():
        x, y = spots[i % len(spots)]
        i += 1
        try:
            subprocess.run("adb shell input tap %d %d" % (x, y), shell=True,
                           capture_output=True, timeout=5)
        except Exception:
            pass
        time.sleep(0.06)


def parse_stats(reply):
    d = {}
    for tok in reply.replace("\n", " ").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=10, help="stir seconds per mode")
    ap.add_argument("--port", type=int, default=0, help="0 = read port file")
    ap.add_argument("--launch", action="store_true",
                    help="start the daemon via aimbot_launch_dyn.sh")
    ap.add_argument("--install", action="store_true",
                    help="adb install -r the debug APK first")
    ap.add_argument("--apk", default="app/build/outputs/apk/debug/app-debug.apk")
    args = ap.parse_args()

    if args.install:
        print("== installing %s ==" % args.apk)
        adb("am force-stop io.github.xiangsu1145.aimbotnextgen")
        adb("for p in $(ps -A -o PID,NAME | grep aimbot_shell | awk '{print $1}'); do kill $p 2>/dev/null; done")
        time.sleep(1)
        r = sh("adb install -r %s" % args.apk)
        out = (r.stdout or r.stderr).strip().splitlines()
        print("   " + (out[-1] if out else "no output"))

    if args.launch:
        print("== launching daemon (dynamic) ==")
        adb("for p in $(ps -A -o PID,NAME | grep aimbot_shell | awk '{print $1}'); do kill $p 2>/dev/null; done")
        time.sleep(1)
        adb("sh /data/local/tmp/aimbot_launch_dyn.sh")
        time.sleep(3)

    port_file = adb("cat /data/local/tmp/aimbot_shell.port").stdout.strip()
    port = int(port_file) if port_file.isdigit() else args.port
    if not port:
        raise SystemExit("no port (port file empty and no --port)")
    print("   port = %d" % port)
    sh("adb forward tcp:%d tcp:%d" % (port, port))

    d = Daemon(port)
    print("   handshake: %s" % d.connect().replace("\n", " | "))

    adb("input keyevent KEYCODE_WAKEUP")
    adb("svc power stayon true")

    results = {}
    for mode in (0, 1):
        label = "legacy" if mode == 0 else "sfp"
        print("\n== MODE %d (%s) ==" % (mode, label))
        d.send("CAPTURE OFF", quiet=True)
        time.sleep(0.5)
        d.send("CAPTURE MODE %d" % mode, quiet=True)
        time.sleep(1.5)
        d.send("CAPTURE ON", quiet=True)
        up = False
        for _ in range(24):
            st = d.send("CAPTURE ?", quiet=True)
            if "alive=true" in st:
                up = True
                break
            time.sleep(0.5)
        if not up:
            print("   !! mirror did not report alive — STATS may show a fallback")
        time.sleep(1)
        stop = threading.Event()
        stirrer = threading.Thread(target=stir, args=(args.secs + 6, stop), daemon=True)
        stirrer.start()
        time.sleep(args.secs)
        stats = d.send("CAPTURE STATS", quiet=True)
        stop.set()
        d.send("CAPTURE OFF", quiet=True)
        results[mode] = parse_stats(stats)
        print("   %s" % stats.strip().replace("\n", " "))
        time.sleep(1)

    d.close()

    print("\n== A/B summary ==")
    print("  %-6s %-14s %-15s %-8s %s" % ("req", "producer", "handleMedianUs", "fps", "frames"))
    for mode in (0, 1):
        s = results.get(mode, {})
        print("  %-6d %-14s %-15s %-8s %s" % (
            mode, s.get("mode", "?"), s.get("handleMedianUs", "?"),
            s.get("fps", "?"), s.get("frames", "?")))
    print("\nlegacy = full-panel virtual display rasterised every frame + crop.")
    print("sfp    = SurfaceFlinger projects the crop into a small display; no EGL context, no 1-frame latency.")
    print("Lower handleMedianUs and higher fps are better.")


if __name__ == "__main__":
    sys.exit(main())
