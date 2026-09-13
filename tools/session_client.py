"""Minimal stand-in for the app's ShellManager.

Drives the daemon over its loopback port (forwarded to the host by adb) the way
the app does, then drops the connection on purpose, so the daemon's behaviour
on losing a client can be observed.

    python tools/session_client.py <port> [--open] [--hold SECONDS]
"""

import socket
import sys
import time

HOST = "127.0.0.1"


def read_lines(sock, seconds, label, answer_probes=True):
    """Print whatever arrives for `seconds`.

    With `answer_probes` off the client reads but never speaks, which is what
    a frozen app looks like from the daemon's side: the socket is fine, but
    nothing ever comes back. That is the case the read deadline has to catch.
    """
    sock.settimeout(1.0)
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError as exc:
            print(f"[{label}] recv failed: {exc!r}")
            return
        if not chunk:
            print(f"[{label}] daemon closed the connection")
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            print(f"[{label}] <- {text}")
            if text == "PING?" and answer_probes:
                sock.sendall(b"PONG\n")
                print(f"[{label}] -> PONG")


def main():
    port = int(sys.argv[1])
    do_open = "--open" in sys.argv
    silent = "--silent" in sys.argv
    hold = 20
    if "--hold" in sys.argv:
        hold = int(sys.argv[sys.argv.index("--hold") + 1])

    sock = socket.create_connection((HOST, port), timeout=10)
    print(f"connected to {HOST}:{port}")

    read_lines(sock, 2, "greeting")

    def cmd(line):
        print(f" -> {line}")
        sock.sendall((line + "\n").encode())

    cmd("SET_RESOLUTION 3000 2120 1")
    time.sleep(1)
    if do_open:
        cmd("OPEN")
        time.sleep(1)
    cmd("UI_ON")
    time.sleep(2)

    print(f"holding the session for {hold}s")
    read_lines(sock, hold, "live", answer_probes=not silent)

    print("dropping the connection now")
    sock.close()


if __name__ == "__main__":
    main()
