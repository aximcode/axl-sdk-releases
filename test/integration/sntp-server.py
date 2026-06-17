#!/usr/bin/env python3
"""Mock SNTP/NTP responder for test-sntp-qemu.sh.

Replies to any UDP request on the given port with a 48-byte NTP packet
whose transmit timestamp is a FIXED, known Unix time — so the guest's
parse (axl_sntp_query) is deterministic and the test can assert an exact
value. Exits after 60 seconds or on SIGTERM.

Usage: sntp-server.py <port> [unix_secs]
"""

from __future__ import annotations

import signal
import socket
import struct
import sys
import time

# NTP epoch (1900) -> Unix epoch (1970) offset, in seconds.
NTP_UNIX_DELTA = 2208988800


def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19123
    unix_secs = int(sys.argv[2]) if len(sys.argv) > 2 else 1700000000
    ntp_secs = unix_secs + NTP_UNIX_DELTA

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.bind(("0.0.0.0", port))
    print(f"SNTP responder on {port}, serving unix_secs={unix_secs}", flush=True)

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    # A server-mode (LI=0, VN=4, Mode=4) reply with the fixed time in the
    # Receive (offset 32) and Transmit (offset 40) timestamp fields.
    ts = struct.pack(">II", ntp_secs, 0)
    resp = bytearray(48)
    resp[0] = 0x24
    resp[1] = 1  # stratum 1
    resp[32:40] = ts
    resp[40:48] = ts

    end = time.monotonic() + 60
    while time.monotonic() < end:
        try:
            data, addr = sock.recvfrom(512)
        except socket.timeout:
            continue
        if len(data) >= 48:
            sock.sendto(bytes(resp), addr)


if __name__ == "__main__":
    main()
