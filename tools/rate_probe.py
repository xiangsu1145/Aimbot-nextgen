"""Drive the daemon's UI on/off and report what the render loop did.

Used to verify the adaptive frame pacing without a real finger: closing the
board should drop the loop to its dormant rate, reopening it should snap back
to the refresh rate for the ease and then settle again. Real touches cannot be
simulated this way — `input tap` goes through InputDispatcher and never reaches
the physical device the daemon has grabbed.
"""

import socket
import sys
import time


def drain(sock, seconds):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError as exc:
            print(f"recv failed: {exc!r}")
            return
        if not chunk:
            print("daemon closed the connection")
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            print(f"  <- {text}")
            if text == "PING?":
                sock.sendall(b"PONG\n")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 17321
    sock = socket.create_connection(("127.0.0.1", port))
    sock.settimeout(0.5)
    drain(sock, 1.0)

    for step, wait in (("UI_OFF", 10), ("UI_ON", 8), ("UI_OFF", 10)):
        print(f"-> {step}   (watching {wait}s)")
        sock.sendall((step + "\n").encode())
        drain(sock, wait)

    sock.close()


if __name__ == "__main__":
    main()
