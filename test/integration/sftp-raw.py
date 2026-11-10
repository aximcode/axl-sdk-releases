#!/usr/bin/env python3
"""Raw SFTP version 3 requests over a real ssh(1) subsystem channel.

WHY THIS EXISTS: sftp(1) cannot send what the sshd SFTP tests need to pin
exactly -- a posix-rename@openssh.com onto a path of the test's choosing, a
limits@openssh.com reply's four numbers, a SETSTAT size or a WRITE at an
offset far past the end of a file -- and paramiko cannot connect to AxlSsh
at all (see ssh-peer.py). ssh(1) still carries the bytes: `ssh -s HOST sftp`
connects stdin and stdout to the subsystem, and this script frames the
packets. Stdlib only, like ssh-peer.py.

Usage:
  sftp-raw.py [--time] -- SSH-ARGV...    script on stdin, one request a line

The SSH-ARGV is the whole client command, e.g.
`ssh -s -p 2222 -o ... user@127.0.0.1 sftp`. Each script line is one request;
each reply is one line on stdout:

  init                      VERSION 3 [name=data ...]
  limits                    LIMITS packet read write handles  (or a STATUS)
  posix-rename OLD NEW      STATUS code message
  rename OLD NEW            STATUS code message
  open PATH PFLAGS          HANDLE hex (kept for the handle requests below)
                            PFLAGS is hex, e.g. 1a = WRITE|CREAT|TRUNC
  write OFFSET LEN          STATUS ...: LEN bytes of 'w' at OFFSET (decimal)
  fsetstat-size SIZE        STATUS ...: the kept handle's size
  close                     STATUS ...
  setstat-size PATH SIZE    STATUS ...
  stat PATH                 ATTRS size=N  (or a STATUS)
  pipe-setstat-size SIZE PATH...
                            one SETSTAT size per PATH, ALL sent before any
                            reply is read; one reply line per PATH, in order
  raw HEX                   an EXTENDED request carrying HEX after its id
                            (the name string, then any arguments); the
                            reply as above, or REPLY type hex

Ids count from 1 in script order. With --time, each reply line ends with
` ms=N`, the request's round trip -- for measuring, never for an exact
assertion.

Exit status: 0 when every request got a reply carrying its own id; 1 when
the channel closed before a whole reply arrived (printed as a CLOSED line)
or a reply carried another request's id (a `BADID want N got M` line), and
the script stops there; 2 on a usage error. A reply of an unexpected TYPE
is printed as it came (STATUS ... or REPLY type hex) and does not change
the exit status: the caller's exact comparison of the lines catches it.
"""
from __future__ import annotations

import struct
import subprocess
import sys
import time

FXP_INIT = 1
FXP_VERSION = 2
FXP_OPEN = 3
FXP_CLOSE = 4
FXP_WRITE = 6
FXP_SETSTAT = 9
FXP_FSETSTAT = 10
FXP_STAT = 17
FXP_RENAME = 18
FXP_STATUS = 101
FXP_HANDLE = 102
FXP_ATTRS = 105
FXP_EXTENDED = 200
FXP_EXTENDED_REPLY = 201
ATTR_SIZE = 0x1


class ChannelClosed(Exception):
    """The subsystem's output ended before a whole reply arrived."""


class WrongId(Exception):
    """A reply carried another request's id."""


def check_id(want: int, body: bytes) -> None:
    """Raise WrongId unless the reply body starts with request id want."""
    got = struct.unpack(">I", body[0:4])[0] if len(body) >= 4 else -1
    if got != want:
        raise WrongId(f"BADID want {want} got {got}")


def sstr(b: bytes) -> bytes:
    """An SFTP string: a 32-bit length, then the bytes."""
    return struct.pack(">I", len(b)) + b


def packet(ptype: int, body: bytes) -> bytes:
    """A framed SFTP packet: length, type, body."""
    return struct.pack(">IB", len(body) + 1, ptype) + body


def take_str(b: bytes, off: int) -> tuple[bytes, int]:
    """The SFTP string at off, and the offset after it."""
    n = struct.unpack(">I", b[off:off + 4])[0]
    return b[off + 4:off + 4 + n], off + 4 + n


class Session:
    """One subsystem channel: the ssh(1) process and the request id."""

    def __init__(self, argv: list[str]) -> None:
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 0
        self.handle = b""

    def send(self, ptype: int, body: bytes) -> None:
        self.write_raw(packet(ptype, body))

    def write_raw(self, data: bytes) -> None:
        """Write data to the subsystem in one write and flush it."""
        assert self.proc.stdin is not None
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def recv(self) -> tuple[int, bytes]:
        assert self.proc.stdout is not None
        head = self.proc.stdout.read(4)
        if len(head) < 4:
            raise ChannelClosed
        n = struct.unpack(">I", head)[0]
        body = self.proc.stdout.read(n)
        if len(body) < n or n == 0:
            raise ChannelClosed
        return body[0], body[1:]

    def request(self, ptype: int, rest: bytes) -> tuple[int, bytes]:
        """Send a request with the next id; its reply's type and body after the id."""
        self.next_id += 1
        self.send(ptype, struct.pack(">I", self.next_id) + rest)
        rtype, rbody = self.recv()
        check_id(self.next_id, rbody)
        return rtype, rbody[4:]

    def close(self) -> int:
        if self.proc.stdin is not None:
            self.proc.stdin.close()
        return self.proc.wait(timeout=30)


def status_line(body: bytes) -> str:
    code = struct.unpack(">I", body[0:4])[0]
    msg, _ = take_str(body, 4)
    return f"STATUS {code} {msg.decode('utf-8', 'replace')}"


def reply_line(rtype: int, body: bytes) -> str:
    """A reply as one line; STATUS is spelled out, anything else in hex."""
    if rtype == FXP_STATUS:
        return status_line(body)
    if rtype == FXP_HANDLE:
        h, _ = take_str(body, 0)
        return f"HANDLE {h.hex()}"
    return f"REPLY {rtype} {body.hex()}"


def run_line(s: Session, words: list[str]) -> str:
    cmd = words[0]
    if cmd == "init":
        s.send(FXP_INIT, struct.pack(">I", 3))
        rtype, body = s.recv()
        if rtype != FXP_VERSION:
            return f"REPLY {rtype} {body.hex()}"
        exts: list[str] = []
        off = 4
        while off < len(body):
            name, off = take_str(body, off)
            data, off = take_str(body, off)
            exts.append(f"{name.decode()}={data.decode()}")
        return " ".join([f"VERSION {struct.unpack('>I', body[0:4])[0]}", *exts])
    if cmd == "limits":
        rtype, body = s.request(FXP_EXTENDED, sstr(b"limits@openssh.com"))
        if rtype == FXP_EXTENDED_REPLY and len(body) == 32:
            return "LIMITS " + " ".join(str(v) for v in struct.unpack(">QQQQ", body))
        return reply_line(rtype, body)
    if cmd in ("posix-rename", "rename"):
        args = sstr(words[1].encode()) + sstr(words[2].encode())
        if cmd == "rename":
            return reply_line(*s.request(FXP_RENAME, args))
        return reply_line(*s.request(FXP_EXTENDED, sstr(b"posix-rename@openssh.com") + args))
    if cmd == "open":
        rtype, body = s.request(FXP_OPEN, sstr(words[1].encode())
                                + struct.pack(">II", int(words[2], 16), 0))
        if rtype == FXP_HANDLE:
            s.handle, _ = take_str(body, 0)
        return reply_line(rtype, body)
    if cmd == "write":
        data = b"w" * int(words[2])
        return reply_line(*s.request(FXP_WRITE, sstr(s.handle)
                                     + struct.pack(">Q", int(words[1])) + sstr(data)))
    if cmd == "fsetstat-size":
        return reply_line(*s.request(FXP_FSETSTAT, sstr(s.handle)
                                     + struct.pack(">IQ", ATTR_SIZE, int(words[1]))))
    if cmd == "close":
        return reply_line(*s.request(FXP_CLOSE, sstr(s.handle)))
    if cmd == "setstat-size":
        return reply_line(*s.request(FXP_SETSTAT, sstr(words[1].encode())
                                     + struct.pack(">IQ", ATTR_SIZE, int(words[2]))))
    if cmd == "pipe-setstat-size":
        # Every SETSTAT is written before any reply is read, so the server
        # holds them all at once (a pipelining client's shape).
        # ONE write carries them all, so they reach the server back to
        # back, as a pipelining client's requests do.
        paths = words[2:]
        first = s.next_id + 1
        batch = b""
        for path in paths:
            s.next_id += 1
            batch += packet(FXP_SETSTAT, struct.pack(">I", s.next_id) + sstr(path.encode())
                            + struct.pack(">IQ", ATTR_SIZE, int(words[1])))
        s.write_raw(batch)
        lines: list[str] = []
        for want in range(first, first + len(paths)):
            rtype, body = s.recv()
            check_id(want, body)
            lines.append(reply_line(rtype, body[4:]))
        return "\n".join(lines)
    if cmd == "stat":
        rtype, body = s.request(FXP_STAT, sstr(words[1].encode()))
        if rtype == FXP_ATTRS and struct.unpack(">I", body[0:4])[0] & ATTR_SIZE:
            return f"ATTRS size={struct.unpack('>Q', body[4:12])[0]}"
        return reply_line(rtype, body)
    if cmd == "raw":
        return reply_line(*s.request(FXP_EXTENDED, bytes.fromhex(words[1])))
    raise ValueError(f"unknown request: {cmd}")


def main() -> int:
    argv = sys.argv[1:]
    timed = False
    if argv and argv[0] == "--time":
        timed = True
        argv = argv[1:]
    if not argv or argv[0] != "--" or len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    s = Session(argv[1:])
    rc = 0
    try:
        for line in sys.stdin:
            words = line.split()
            if not words:
                continue
            t0 = time.monotonic()
            out = run_line(s, words)
            if timed:
                out += f" ms={int((time.monotonic() - t0) * 1000)}"
            print(out, flush=True)
    except ChannelClosed:
        print("CLOSED", flush=True)
        rc = 1
    except WrongId as e:
        print(str(e), flush=True)
        rc = 1
    s.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
