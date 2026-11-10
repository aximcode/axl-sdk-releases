#!/usr/bin/env python3
"""tcp-rtt.py -- time request/response round trips against a line echo server.

Connects to HOST:PORT (retrying until the server listens or the deadline
passes), sends N short lines one at a time, waits for each to come back, and
prints one line:

    RTT n=<N> median_ms=<m.mmm> p90_ms=<m.mmm> first_ms=<m.mmm>

`first_ms` is the first exchange on its own: after an idle spell a WINDOW
policy pays the firmware's pace for it, and only the rest are fast. Sends
`quit` at the end. Exit status 1 when it cannot connect, an echo is wrong, or
no round trip was timed (--count 0).

Usage: tcp-rtt.py HOST PORT [--count N] [--gap-ms MS] [--connect-timeout S]

--gap-ms waits that long between exchanges (an interactive peer's pace).
--upload-mib N instead sends N MiB, half-closes, waits for the server to
close (it has everything), and prints `UPLOAD mib=<N> secs=<s> mb_s=<r>`.
"""
from __future__ import annotations

import argparse
import socket
import statistics
import sys
import time


def connect(host: str, port: int, deadline: float) -> socket.socket:
    last: OSError | None = None
    while time.monotonic() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return s
        except OSError as e:
            last = e
            time.sleep(0.2)
    raise OSError(f"cannot connect to {host}:{port}: {last}")


def recv_line(s: socket.socket, want: bytes) -> bytes:
    got = b""
    while len(got) < len(want):
        chunk = s.recv(len(want) - len(got))
        if not chunk:
            break
        got += chunk
    return got


def upload(host: str, port: int, mib: int, connect_timeout: float) -> int:
    try:
        s = connect(host, port, time.monotonic() + connect_timeout)
    except OSError as e:
        print(f"UPLOAD error: {e}")
        return 1
    chunk = bytes(65536)
    with s:
        s.settimeout(120)
        t0 = time.perf_counter()
        for _ in range(mib * 16):
            s.sendall(chunk)
        s.shutdown(socket.SHUT_WR)
        while s.recv(4096):
            pass
        secs = time.perf_counter() - t0
    print(f"UPLOAD mib={mib} secs={secs:.3f} mb_s={mib * 1.048576 / secs:.2f}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--connect-timeout", type=float, default=60.0)
    ap.add_argument("--gap-ms", type=float, default=0.0)
    ap.add_argument("--upload-mib", type=int, default=0)
    a = ap.parse_args()
    if a.upload_mib > 0:
        return upload(a.host, a.port, a.upload_mib, a.connect_timeout)

    try:
        s = connect(a.host, a.port, time.monotonic() + a.connect_timeout)
    except OSError as e:
        print(f"RTT error: {e}")
        return 1
    rtts: list[float] = []
    with s:
        for i in range(a.count):
            if i > 0 and a.gap_ms > 0:
                time.sleep(a.gap_ms / 1000.0)
            msg = f"ping {i:04d}\n".encode()
            t0 = time.perf_counter()
            s.sendall(msg)
            back = recv_line(s, msg)
            rtts.append((time.perf_counter() - t0) * 1000.0)
            if back != msg:
                print(f"RTT error: echo {i} was {back!r}, sent {msg!r}")
                return 1
        s.sendall(b"quit\n")
    if not rtts:
        print("RTT error: no round trips timed (--count 0)")
        return 1
    rest = rtts[1:] if len(rtts) > 1 else rtts
    p90 = statistics.quantiles(rest, n=10)[-1] if len(rest) >= 2 else rest[0]
    print(f"RTT n={len(rtts)} median_ms={statistics.median(rest):.3f} "
          f"p90_ms={p90:.3f} first_ms={rtts[0]:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
