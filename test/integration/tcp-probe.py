#!/usr/bin/env python3
"""Tiny TCP probe used by test-echo-server-sync.sh.

Connects to host:port, sends a message, reads whatever the server
replies until EOF or timeout, and prints the reply to stdout.
Replaces a `nc -q 1 -w 5` call that would otherwise differ across
nc variants (openbsd-nc has -q, nmap-ncat does not).

Usage: tcp-probe.py <host> <port> <message> [timeout_sec]
"""
from __future__ import annotations

import socket
import sys


def main() -> None:
    if len(sys.argv) < 4:
        print("usage: tcp-probe.py <host> <port> <message> [timeout_sec]",
              file=sys.stderr)
        sys.exit(2)

    host = sys.argv[1]
    port = int(sys.argv[2])
    msg = sys.argv[3].encode()
    timeout = float(sys.argv[4]) if len(sys.argv) > 4 else 5.0

    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(msg)
        sock.shutdown(socket.SHUT_WR)

        sock.settimeout(timeout)
        buf = b""
        while True:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk

    sys.stdout.buffer.write(buf)


if __name__ == "__main__":
    main()
