#!/usr/bin/env python3
"""Simple UDP echo server for test-udp.sh.

Listens on the given port, echoes back every datagram with an
"ECHO:" prefix. Exits after 60 seconds or when killed.
"""
from __future__ import annotations

import signal
import socket
import sys

def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19000

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.bind(("0.0.0.0", port))
    print(f"UDP echo server listening on port {port}", flush=True)

    signal.alarm(60)

    while True:
        try:
            data, addr = sock.recvfrom(4096)
            reply = b"ECHO:" + data
            sock.sendto(reply, addr)
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    main()
