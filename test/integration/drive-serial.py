#!/usr/bin/env python3
"""Drive a QEMU guest interactively over its serial UNIX socket.

Connects to the socket QEMU exposes via `run-qemu.sh --background
--serial-socket <path>`, waits for the guest to settle at its (interactive)
prompt, sends each command terminated with CR, and prints everything received.

Why this exists — the interactive/`startup.nsh` distinction:
  The old EFI 1.x shell (and potentially others) behaves DIFFERENTLY while
  running a `startup.nsh` ("backward compatible mode") than at the interactive
  prompt. Notably its `map -r` omits the device-path aliases that
  `map <name> <fsN>:` needs, so a feature can work when typed by hand yet fail
  in every `startup.nsh`-driven test. Booting in `--background` (run-qemu does
  NOT inject `reset -s` there, so the shell drops to the real interactive
  prompt) and driving over this socket reproduces the interactive path in an
  automated test. UEFI is single-threaded and the console buffers type-ahead,
  so exact keystroke timing does not matter.

Usage: drive-serial.py [--ready MARKER] <socket-path> <cmd1> [cmd2 ...]
  --ready MARKER  Before sending any command, wait until MARKER appears in the
                  output (the interactive prompt, e.g. "Shell>"). Far more
                  robust than a fixed settle delay — without it a command can
                  be sent while the shell is still in its startup.nsh phase
                  (backward-compatible mode), which changes behavior and flakes.
Output: the raw serial transcript (ANSI escapes included; strip with
        `sed 's/\\x1b\\[[0-9;]*[A-Za-z]//g'` when scraping for markers).
"""
from __future__ import annotations

import socket
import sys
import time


def drain(sock: socket.socket, quiet_s: float, max_s: float = 20.0) -> bytes:
    """Read until no data arrives for quiet_s seconds (or max_s total elapses)."""
    out = b""
    deadline = time.time() + max_s
    last = time.time()
    sock.setblocking(False)
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
            if chunk:
                out += chunk
                last = time.time()
            else:
                time.sleep(0.02)
        except BlockingIOError:
            if time.time() - last > quiet_s:
                break
            time.sleep(0.05)
        except OSError:
            break
    return out


def main() -> int:
    args = sys.argv[1:]
    ready = None
    if len(args) >= 2 and args[0] == "--ready":
        ready = args[1]
        args = args[2:]
    if len(args) < 1:
        print("usage: drive-serial.py [--ready MARKER] <socket-path> <cmd> ...",
              file=sys.stderr)
        return 2
    sock_path = args[0]
    cmds = args[1:]

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(120):
        try:
            sock.connect(sock_path)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.5)
    else:
        print("ERROR: could not connect to", sock_path, file=sys.stderr)
        return 1

    captured = bytearray()
    if ready is not None:
        # Wait for the interactive prompt to appear (the shell has left its
        # startup.nsh phase) before typing anything — deterministic, not a race.
        deadline = time.time() + 40.0
        while time.time() < deadline:
            captured += drain(sock, quiet_s=1.0, max_s=4.0)
            if ready.encode() in captured:
                break
    else:
        # Let the guest boot + run startup.nsh and settle at the prompt.
        captured += drain(sock, quiet_s=4.0, max_s=25.0)
    for cmd in cmds:
        sock.sendall(cmd.encode() + b"\r")
        captured += drain(sock, quiet_s=2.0, max_s=15.0)
    captured += drain(sock, quiet_s=1.5, max_s=6.0)

    sys.stdout.write(captured.decode(errors="replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
