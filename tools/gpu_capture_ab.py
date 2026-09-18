#!/usr/bin/env python3
"""Drive the shell daemon over its loopback port and time the capture paths.

The daemon's command protocol lives on a 127.0.0.1 TCP socket (see
ShellServerEntry.serveClient), not on stdin — a stdin pipe is read by nothing.
This connects to it through `adb forward` and runs an A/B/A schedule:

    A  cpu path   (debug.aimbotng.gpuproc = 0)
    B  gpu path   (debug.aimbotng.gpuproc = 1)
    C  cpu path again, to show the machine did not just warm up

Between phases it asks the daemon for `INFER ?` and `GPU ?`, which are the two
lines that say how fast frames are moving and what the GPU spent.

    python tools/gpu_capture_ab.py [--secs 10] [--port 44989]

Print a phase summary at the end; the per-frame numbers are in logcat under
AimbotCapture (cpu) and AimbotGpuProc (gpu).
"""
import argparse
import socket
import subprocess
import sys
import threading
import time

TAGS = "AimbotCapture:V AimbotGpuProc:V AimbotInfer:V AimbotModel:V AimbotNg:V"


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
                return self.recv_until_ready()
            except OSError:
                time.sleep(delay)
        raise SystemExit("daemon never answered on port %d" % self.port)

    def recv_until_ready(self):
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
    """Keeps SurfaceFlinger producing frames.

    A virtual display only receives a frame when the screen actually changes, so
    a static home screen delivers well under one frame a second and there is
    nothing to measure. Poking the touchscreen forces a composite.
    """
    end = time.time() + stop_after
    while time.time() < end and not event.is_set():
        try:
            subprocess.run("adb shell input tap 500 900", shell=True,
                           capture_output=True, timeout=5)
        except Exception:
            pass
        time.sleep(0.15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=10, help="seconds per phase")
    ap.add_argument("--port", type=int, default=44989)
    ap.add_argument("--w", type=int, default=2120)
    ap.add_argument("--h", type=int, default=3000)
    ap.add_argument("--existing", action="store_true",
                    help="use a daemon that is already up (see daemon_hold.sh)")
    ap.add_argument("--model", type=int, default=9,
                    help="model id to mark loaded (0 = leave it alone)")
    args = ap.parse_args()

    holder = None
    if args.existing:
        print("== using the daemon that is already up ==")
        adb("logcat -c")
        adb("(nohup logcat -v brief -s %s > /data/local/tmp/gpu_ab.log 2>&1 </dev/null &)" % TAGS)
    else:
        print("== cleaning up ==")
        adb("am force-stop io.github.xiangsu1145.aimbotnextgen")
        adb("for p in $(ps -A -o PID,NAME | grep aimbot_shell | awk '{print $1}'); do kill $p; done")
        time.sleep(2)

        print("== starting logcat ==")
        adb("logcat -c")
        adb("(nohup logcat -v brief -s %s > /data/local/tmp/gpu_ab.log 2>&1 </dev/null &)" % TAGS)

        print("== starting daemon ==")
        adb("sh /data/local/tmp/aimbot_launch.sh")
        time.sleep(4)

    port_file = adb("cat /data/local/tmp/aimbot_shell.port").stdout.strip()
    port = int(port_file) if port_file.isdigit() else args.port
    print("   port = %s" % port)
    sh("adb forward tcp:%d tcp:%d" % (port, port))

    d = Daemon(port)
    print("   handshake: %s" % d.connect().replace("\n", " | "))

    adb("input keyevent KEYCODE_WAKEUP")
    adb("svc power stayon true")
    d.send("SET_RESOLUTION %d %d 0" % (args.w, args.h))
    if args.model:
        d.send("MODEL ?")
        d.send("MODEL LOAD %d" % args.model)
    d.send("CAPTURE ON")
    d.send("INFER ON")
    time.sleep(3)
    d.send("INFER DESC")

    stop = threading.Event()
    stirrer = threading.Thread(
        target=stir, args=(args.secs * 3 + 12, stop), daemon=True)
    stirrer.start()

    results = {}
    for name, gpu in (("A cpu", False), ("B gpu", True), ("C cpu", False)):
        print("\n== phase %s (gpuproc=%d) ==" % (name, 1 if gpu else 0))
        d.send("GPU %s" % ("ON" if gpu else "OFF"), quiet=True)
        time.sleep(args.secs)
        infer = d.send("INFER ?", quiet=True)
        gput = d.send("GPU ?", quiet=True)
        results[name] = (infer.strip().splitlines(), gput.strip().splitlines())
        for line in infer.splitlines() + gput.splitlines():
            if line.strip():
                print("   %s" % line.strip())

    stop.set()
    print("\n== stopping ==")
    d.send("INFER OFF", quiet=True)
    d.send("CAPTURE OFF", quiet=True)
    d.close()
    if holder: holder.terminate()

    print("\n== phase summary ==")
    for name, (infer, gput) in results.items():
        print("-- %s" % name)
        for line in infer:
            print("   %s" % line)
        for line in gput:
            print("   %s" % line)

    print("\nlog on device: /data/local/tmp/gpu_ab.log")
    print("pull with: adb shell cat /data/local/tmp/gpu_ab.log")


if __name__ == "__main__":
    sys.exit(main())
