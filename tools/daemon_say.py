#!/usr/bin/env python3
"""Send one line to the aimbot daemon over adb-forwarded TCP and print the reply.

Usage:
    python daemon_say.py <port> <command> [more commands...]

The daemon speaks line protocol on 127.0.0.1:<port> (the port it wrote to
/data/local/tmp/aimbot_shell.port). Pair it with:

    adb forward tcp:<port> tcp:<port>

so this script can reach it as 127.0.0.1:<port> from the PC. Used for on-device
verification of the menu layer without going through the app UI.
"""

import socket
import sys
import time


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    port = int(sys.argv[1])
    commands = sys.argv[2:]

    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.settimeout(1.0)
        # The daemon greets every new client with a READY banner (and a STATE
        # line). Drain until it goes quiet, otherwise every command below reads
        # the previous line and the whole session is off by one.
        drain(s, quiet=1.0, total=4.0)
        for cmd in commands:
            print(f">>> {cmd}")
            s.sendall((cmd + "\n").encode())
            for line in drain(s, quiet=0.8, total=20.0):
                print(f"    {line}")
    return 0


def drain(s, quiet: float, total: float):
    """Read lines until the socket has been silent for `quiet` seconds."""
    deadline = time.time() + total
    last = time.time()
    buf = b""
    while time.time() < deadline and (time.time() - last) < quiet:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        last = time.time()
    return buf.decode(errors="replace").splitlines()


if __name__ == "__main__":
    sys.exit(main())
