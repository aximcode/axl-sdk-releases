#!/usr/bin/env python3
"""Minimal TCP echo server for test-echo-client.sh.

Accepts connections, prefixes every received chunk with "ECHO:" and
writes it back, closes when the peer closes. Exits after 60 seconds
or when killed.

The prefix matches the UDP echo server's convention; it also lets
the integration test distinguish "client saw its own reflected bytes
off some proxy" from "client actually talked to this server".
"""
from __future__ import annotations

import signal
import socket
import sys


def handle(conn: socket.socket, addr: tuple[str, int]) -> None:
    print(f"  connected: {addr[0]}:{addr[1]}", flush=True)
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            conn.sendall(b"ECHO:" + data)
    finally:
        conn.close()
        print("  closed", flush=True)


def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19001

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(1.0)
    sock.bind(("0.0.0.0", port))
    sock.listen(4)
    print(f"TCP echo server listening on port {port}", flush=True)

    signal.alarm(60)

    while True:
        try:
            conn, addr = sock.accept()
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break
        handle(conn, addr)


if __name__ == "__main__":
    main()
