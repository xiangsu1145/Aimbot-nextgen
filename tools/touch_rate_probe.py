"""Reproduces "menu open + a finger moving" without a hand on the screen.

The bug this was written for: with a finger down, the render loop ran at
180-200 fps against a 120 target. Real fingers cannot be scripted, and the
uinput-based DOWN/MOVE/UP commands cannot reproduce it either — the panel
reader holds an EVIOCGRAB, so synthetic events reach InputDispatcher and stop
there, never getting near ImGui. `UITOUCH` feeds the same queue the reader
feeds, which is the only way in.

    adb forward tcp:17321 tcp:<daemon-port> && python tools/touch_rate_probe.py 17321

Sends a DOWN followed by a stream of MOVEs at `--hz`, for `--seconds`. Watch
logcat next to it:

    adb logcat -v brief -s AimbotNg:* | grep fps=

A correct run tops out at the target rate (120) and drops back to the idle
tier once the stream stops.
"""

import socket
import sys
import time

HOST = "127.0.0.1"


def drain(sock, seconds):
    """Keep the socket drained; the daemon pushes TOUCH lines while grabbed."""
    sock.settimeout(0.05)
    end = time.time() + seconds
    n = 0
    while time.time() < end:
        try:
            if not sock.recv(65536):
                return n
            n += 1
        except socket.timeout:
            continue
        except OSError as exc:
            print(f"recv failed: {exc!r}")
            return n
    return n


def main():
    port = int(sys.argv[1])
    seconds = float(sys.argv[sys.argv.index("--seconds") + 1]) if "--seconds" in sys.argv else 12.0
    hz = float(sys.argv[sys.argv.index("--hz") + 1]) if "--hz" in sys.argv else 200.0

    sock = socket.create_connection((HOST, port), timeout=10)
    print(f"connected to {HOST}:{port}")

    def cmd(line):
        sock.sendall((line + "\n").encode())

    cmd("SET_RESOLUTION 3000 2120 1")
    time.sleep(1)
    cmd("UI_ON")
    time.sleep(2)
    drain(sock, 1)

    # A finger tracking across the middle of the board.
    x0, y0 = 900, 1100
    cmd(f"UITOUCH DOWN {x0} {y0}")
    print(f"finger down; streaming MOVE at {hz:.0f} Hz for {seconds:.0f}s")

    period = 1.0 / hz
    end = time.time() + seconds
    i = 0
    next_at = time.time()
    while time.time() < end:
        now = time.time()
        if now < next_at:
            drain(sock, next_at - now)
            continue
        i += 1
        cmd(f"UITOUCH MOVE {x0 + int(120 * (i % 60) / 60)} {y0 + int(80 * (i % 40) / 40)}")
        next_at += period
    cmd(f"UITOUCH UP {x0} {y0}")
    print("finger up; holding 8s so the loop can fall back to the idle tier")
    drain(sock, 8)
    sock.close()


if __name__ == "__main__":
    main()
