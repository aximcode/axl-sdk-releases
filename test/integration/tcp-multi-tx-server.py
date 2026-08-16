#!/usr/bin/env python3
"""Wire-order recorder for the AXL-Tcp-Queue-Design §7 spike.

Accepts ONE connection, reads to EOF, and reports the order the marker
bytes actually arrived in. That is the half of the question the guest
cannot answer: its completion events say when the firmware retired each
token, not what it put on the wire, and "completed in order" would still
permit interleaved bytes.

The guest (AxlTestNet.efi tcp-multi-tx) submits four concurrent
EFI_TCP4.Transmit tokens whose payloads are runs of 'A', 'B', 'C' and 'D'.
Four clean runs in that order means the firmware serialized the tokens in
submission order; anything else is the reordering the design doc's §2
feared, in the shape it feared it.
"""
from __future__ import annotations

import pathlib
import socket
import sys

ACCEPT_TIMEOUT_S = 90.0
READ_TIMEOUT_S = 60.0


def summarize(data: bytes) -> str:
    """Run-length summary of the byte stream, one entry per RUN.

    Deliberately not a per-marker total: totals would average interleaving
    away, and interleaving is exactly what this is looking for. 'A:16384'
    four times over is the clean answer; 'A:4096,B:4096,A:12288,...' is the
    interesting one.
    """
    runs: list[list[object]] = []
    for byte in data:
        char = chr(byte)
        if runs and runs[-1][0] == char:
            runs[-1][1] = int(runs[-1][1]) + 1  # type: ignore[assignment]
        else:
            runs.append([char, 1])
    return ",".join(f"{run[0]}:{run[1]}" for run in runs)


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19100
    out_path = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(ACCEPT_TIMEOUT_S)
    sock.bind(("0.0.0.0", port))
    sock.listen(1)
    print(f"multi-tx recorder listening on port {port}", flush=True)

    try:
        conn, addr = sock.accept()
    except socket.timeout:
        print("MTX-WIRE:FAIL no connection", flush=True)
        if out_path is not None:
            out_path.write_text("MTX-WIRE:FAIL no connection\n")
        return 1

    print(f"  connected: {addr[0]}:{addr[1]}", flush=True)
    conn.settimeout(READ_TIMEOUT_S)
    chunks: list[bytes] = []
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            chunks.append(data)
    except socket.timeout:
        # Report what did arrive: a partial stream is a result, not an error.
        print("  read timed out; reporting what arrived", flush=True)
    finally:
        conn.close()

    payload = b"".join(chunks)
    line = f"MTX-WIRE:bytes={len(payload)} runs={summarize(payload)}"
    print(line, flush=True)
    if out_path is not None:
        out_path.write_text(line + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
