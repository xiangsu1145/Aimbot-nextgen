"""Drives the inference pipeline through the daemon's loopback port.

Exists because the Model page's switch is an ImGui control the daemon paints onto
its own SurfaceFlinger layer: neither `adb shell input` nor uinput injection can
reach it, so the only scriptable way in is the daemon's own INFER command.

    python tools/infer_probe.py <port> [--seconds N]

Opens the layer, flips the inference switch on, polls the live status for
`seconds`, then asks what the model bound and flips the switch back off.
"""

import socket
import sys
import time

HOST = "127.0.0.1"


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=10)
        self.buf = b""

    def send(self, line):
        print(f" -> {line}")
        self.sock.sendall((line + "\n").encode())

    def lines(self, seconds, answer_probes=True):
        """Everything that arrives for `seconds`."""
        self.sock.settimeout(1.0)
        end = time.time() + seconds
        out = []
        while time.time() < end:
            try:
                chunk = self.sock.recv(8192)
            except socket.timeout:
                continue
            except OSError as exc:
                print(f"recv failed: {exc!r}")
                break
            if not chunk:
                print("daemon closed the connection")
                break
            self.buf += chunk
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if not text:
                    continue
                # TOUCH is a per-finger stream at hundreds of lines a second and
                # would drown everything else; the probe never asks for it.
                if text.startswith("TOUCH"):
                    continue
                print(f" <- {text}")
                out.append(text)
                if text == "PING?" and answer_probes:
                    self.sock.sendall(b"PONG\n")
        return out

    def ask(self, line, wait=2.0):
        """Sends `line` and returns the first reply that is not a touch event."""
        self.send(line)
        got = self.lines(wait)
        for text in got:
            if text.startswith("OK:") or text.startswith("ERR:"):
                return text
        return None

    def close(self):
        self.sock.close()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    port = int(sys.argv[1])
    seconds = 20
    if "--seconds" in sys.argv:
        seconds = int(sys.argv[sys.argv.index("--seconds") + 1])

    c = Client(port)
    print(f"connected to {HOST}:{port}")

    c.lines(2)                                    # greeting
    c.send("SET_RESOLUTION 3000 2120 1")
    time.sleep(0.5)
    c.send("OPEN")
    time.sleep(0.5)
    c.send("UI_ON")
    # The render thread is what publishes the switches and pumps frames, so
    # nothing below this line happens until the layer has come up.
    time.sleep(3)

    # The list is reloaded from disk on every start, so the hand-written
    # /data/local/tmp/aimbot_models.tsv is already in effect.
    print("\n=== flip inference on ===")
    print("   ", c.ask("INFER ON", wait=3.0))

    print(f"\n=== polling for {seconds}s ===")
    for _ in range(max(1, seconds // 3)):
        time.sleep(3)
        print("   ", c.ask("INFER ?", wait=2.0))

    print("\n=== what the model bound ===")
    print("   ", c.ask("INFER DESC", wait=2.0))

    print("\n=== flip inference off ===")
    print("   ", c.ask("INFER OFF", wait=2.0))

    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
