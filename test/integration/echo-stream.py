#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
"""Host-side TCP echo server used by AxlTestNet.

Listens on a host port (default 55555) on 127.0.0.1. Each incoming
connection is handled by a fresh thread that reads bytes and writes
them straight back until the peer closes. Used by the unit-test
runner: QEMU's slirp is configured with `guestfwd` rules that
intercept guest connections to the guest's own IP and redirect them
to this server, giving the client-side TCP path a working remote
peer (which slirp itself cannot provide — it does not loopback the
guest's address back to the guest).

Runs until SIGTERM / SIGINT.
"""

from __future__ import annotations

import argparse
import signal
import socket
import sys
import threading


def handle(conn: socket.socket) -> None:
    try:
        with conn:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                conn.sendall(chunk)
    except OSError:
        return


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=55555)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--udp", action="store_true",
                        help="UDP datagram echo instead of TCP stream")
    args = parser.parse_args()

    if args.udp:
        return run_udp(args.bind, args.port)
    return run_tcp(args.bind, args.port)


def run_tcp(bind: str, port: int) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind, port))
    sock.listen(64)

    stop = threading.Event()
    def on_signal(_sig: int, _frame: object | None) -> None:
        stop.set()
        try:
            sock.close()
        except OSError:
            pass
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    print(f"echo-stream: TCP listening on {bind}:{port}", flush=True)
    try:
        while not stop.is_set():
            try:
                conn, _ = sock.accept()
            except OSError:
                break
            t = threading.Thread(target=handle, args=(conn,), daemon=True)
            t.start()
    finally:
        try:
            sock.close()
        except OSError:
            pass
    return 0


def run_udp(bind: str, port: int) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # SO_REUSEPORT so two INDEPENDENT unit-test runs can coexist on this
    # port. Unlike every other host port in the harness this one cannot be
    # allocated dynamically: the guest targets it as a compile-time constant
    # (AXL_TEST_UDP_ECHO_PORT in test/unit/axl-test-net.c) and slirp delivers
    # the datagram to host loopback on exactly that number, so there is
    # nowhere to plumb a runtime choice through. Sharing the bind is safe
    # here because the echo is stateless: the kernel steers each datagram to
    # one of the bound sockets, whichever server receives it echoes back to
    # the sender's address, and the reply reaches the VM it came from.
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except (AttributeError, OSError):
        pass  # not Linux/BSD — single-runner behaviour, as before
    sock.bind((bind, port))

    def on_signal(_sig: int, _frame: object | None) -> None:
        try:
            sock.close()
        except OSError:
            pass
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    print(f"echo-stream: UDP listening on {bind}:{port}", flush=True)
    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except OSError:
                return 0
            try:
                sock.sendto(data, addr)
            except OSError:
                pass
    finally:
        try:
            sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
