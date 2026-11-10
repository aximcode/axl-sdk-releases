#!/usr/bin/env python3
"""Host side of the test-tcp-close-pending, test-tcp-send-cancel and
test-tcp-close-armed QEMU tests.

Three subcommands:

  client PORT TRIGGER OUT
      Connect to the guest with a small receive buffer and read NOTHING until
      the file TRIGGER exists, so the guest's sends stall against a full
      window. Then read everything that ever arrives, until end of stream, a
      reset, or silence, and write one summary line to OUT:
          RECV total=N a=N z=N q=N other=N end=<eof|reset|timeout>

  cancel-client PORT TRIGGER1 TRIGGER2 OUT
      Connect with a small receive buffer and read NOTHING until TRIGGER1
      exists. Then read until the stream ends with b"MARK", answer b"PONG",
      and stop reading until TRIGGER2 exists. Then read until the stream ends
      with b"MRK2", answer b"PONG2", and read to end of stream. Write one
      summary line of the whole stream's structure to OUT:
          STREAM a=N mark=N b=N mrk2=N rest=N z=N q=N end=<eof|reset|timeout>
      a/b are the lengths of the leading run of 'A' before "MARK" and of 'B'
      before "MRK2"; rest counts bytes after "MRK2"; z/q count every 'Z'/'Q'
      anywhere. A section that never arrived reads mark=0 / mrk2=0.

  pcap FILE GUEST_IP GUEST_PORT
      Parse a QEMU filter-dump capture and summarise every TCP segment the
      guest sent from GUEST_PORT:
          WIRE a=N z=N q=N other=N rst=N fin=N
      a/z/other count payload bytes, retransmissions included -- the capture
      sees what the firmware actually put on the wire, independent of any
      buffering between the NIC and the host socket.
"""

from __future__ import annotations

import socket
import struct
import sys
import time
from pathlib import Path

RCVBUF_BYTES = 4096
READ_IDLE_S = 5.0
TRIGGER_WAIT_S = 120.0

PCAP_GLOBAL_LEN = 24
PCAP_RECORD_LEN = 16
ETH_LEN = 14
ETHERTYPE_IPV4 = 0x0800
IPPROTO_TCP = 6
TCP_FIN = 0x01
TCP_RST = 0x04


class ByteTally:
    """Counts of the fixture's two fill bytes and everything else."""

    def __init__(self) -> None:
        self.a: int = 0
        self.z: int = 0
        self.q: int = 0
        self.other: int = 0

    def add(self, data: bytes) -> None:
        a = data.count(b"A")
        z = data.count(b"Z")
        q = data.count(b"Q")
        self.a += a
        self.z += z
        self.q += q
        self.other += len(data) - a - z - q

    @property
    def total(self) -> int:
        return self.a + self.z + self.q + self.other


def run_client(port: int, trigger: Path, out: Path) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RCVBUF_BYTES)
    sock.settimeout(30.0)
    sock.connect(("127.0.0.1", port))

    wait_for(trigger)

    tally = ByteTally()
    end = "timeout"
    sock.settimeout(READ_IDLE_S)
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            end = "reset"
            break
        except TimeoutError:
            break
        if not chunk:
            end = "eof"
            break
        tally.add(chunk)
    sock.close()

    out.write_text(
        f"RECV total={tally.total} a={tally.a} z={tally.z} q={tally.q} "
        f"other={tally.other} end={end}\n"
    )
    return 0


def read_until_suffix(sock: socket.socket, stream: bytearray,
                      suffix: bytes, timeout_s: float) -> str:
    """Append to STREAM until it ends with SUFFIX; return how reading ended."""
    deadline = time.monotonic() + timeout_s
    sock.settimeout(1.0)
    while not stream.endswith(suffix):
        if time.monotonic() > deadline:
            return "timeout"
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            return "reset"
        except TimeoutError:
            continue
        if not chunk:
            return "eof"
        stream += chunk
    return "ok"


def wait_for(trigger: Path) -> None:
    deadline = time.monotonic() + TRIGGER_WAIT_S
    while not trigger.exists() and time.monotonic() < deadline:
        time.sleep(0.1)


def leading_run(data: bytes, byte: int) -> int:
    n = 0
    while n < len(data) and data[n] == byte:
        n += 1
    return n


def run_cancel_client(port: int, trigger1: Path, trigger2: Path,
                      out: Path) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RCVBUF_BYTES)
    sock.settimeout(30.0)
    sock.connect(("127.0.0.1", port))

    stream = bytearray()
    end = "timeout"
    wait_for(trigger1)
    how = read_until_suffix(sock, stream, b"MARK", TRIGGER_WAIT_S)
    if how == "ok":
        sock.sendall(b"PONG")
        wait_for(trigger2)
        how = read_until_suffix(sock, stream, b"MRK2", TRIGGER_WAIT_S)
        if how == "ok":
            sock.sendall(b"PONG2")
            end = "timeout"
            sock.settimeout(READ_IDLE_S)
            while True:
                try:
                    chunk = sock.recv(65536)
                except ConnectionResetError:
                    end = "reset"
                    break
                except TimeoutError:
                    break
                if not chunk:
                    end = "eof"
                    break
                stream += chunk
        else:
            end = how
    else:
        end = how
    sock.close()

    data = bytes(stream)
    a = leading_run(data, ord("A"))
    mark = 1 if data[a:a + 4] == b"MARK" else 0
    b = 0
    mrk2 = 0
    rest = 0
    if mark:
        tail = data[a + 4:]
        b = leading_run(tail, ord("B"))
        mrk2 = 1 if tail[b:b + 4] == b"MRK2" else 0
        if mrk2:
            rest = len(tail) - b - 4
    out.write_text(
        f"STREAM a={a} mark={mark} b={b} mrk2={mrk2} rest={rest} "
        f"z={data.count(b'Z')} q={data.count(b'Q')} end={end}\n"
    )
    return 0


def run_pcap(capture: Path, guest_ip: str, guest_port: int) -> int:
    data = capture.read_bytes()
    if len(data) < PCAP_GLOBAL_LEN:
        print(f"ERROR: capture {capture} is {len(data)} bytes, no pcap header")
        return 1
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == 0xA1B2C3D4:
        endian = "<"
    elif magic == 0xD4C3B2A1:
        endian = ">"
    else:
        print(f"ERROR: capture {capture} has bad magic {magic:#x}")
        return 1

    guest = socket.inet_aton(guest_ip)
    tally = ByteTally()
    rst = 0
    fin = 0
    frames = 0
    off = PCAP_GLOBAL_LEN
    while off + PCAP_RECORD_LEN <= len(data):
        incl_len = struct.unpack_from(f"{endian}I", data, off + 8)[0]
        frame = data[off + PCAP_RECORD_LEN: off + PCAP_RECORD_LEN + incl_len]
        off += PCAP_RECORD_LEN + incl_len
        frames += 1
        if len(frame) < ETH_LEN + 20:
            continue
        if struct.unpack_from("!H", frame, 12)[0] != ETHERTYPE_IPV4:
            continue
        ip = frame[ETH_LEN:]
        ihl = (ip[0] & 0x0F) * 4
        total_len = struct.unpack_from("!H", ip, 2)[0]
        if ip[9] != IPPROTO_TCP or ip[12:16] != guest:
            continue
        tcp = ip[ihl:total_len]
        if len(tcp) < 20:
            continue
        if struct.unpack_from("!H", tcp, 0)[0] != guest_port:
            continue
        doff = (tcp[12] >> 4) * 4
        flags = tcp[13]
        if flags & TCP_RST:
            rst += 1
        if flags & TCP_FIN:
            fin += 1
        tally.add(tcp[doff:])

    print(f"WIRE a={tally.a} z={tally.z} q={tally.q} other={tally.other} "
          f"rst={rst} fin={fin} frames={frames}")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 5 and argv[1] == "client":
        return run_client(int(argv[2]), Path(argv[3]), Path(argv[4]))
    if len(argv) == 6 and argv[1] == "cancel-client":
        return run_cancel_client(int(argv[2]), Path(argv[3]), Path(argv[4]),
                                 Path(argv[5]))
    if len(argv) == 5 and argv[1] == "pcap":
        return run_pcap(Path(argv[2]), argv[3], int(argv[4]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
