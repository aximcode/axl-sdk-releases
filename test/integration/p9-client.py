#!/usr/bin/env python3
"""Minimal 9P2000.L host CLIENT for the AXL 9P SERVER integration test.

The inverse of p9-server.py: that one serves a fixed tree so the UEFI 9P
CLIENT can be tested; this one drives a UEFI 9P SERVER (Axl9pServer, running
inside QEMU and reachable through a hostfwd port) and asserts on what comes
back over the wire. It is the first thing that ever executes the server's
handler code, so every assertion here is exact -- a mismatch is a finding,
not a tolerance to widen.

Layering, deliberately (Task 6b sends frames no real client would produce):

    frame()             builds size[4] type[1] tag[2] body, with `size`
                        OVERRIDABLE so a caller can lie about it.
    P9Conn              framing only. `send_raw` puts arbitrary bytes on the
                        wire, so two frames can go out in ONE send() (the
                        pipelined case), and several P9Conn objects can be
                        live at once (the two-connection cases).
    P9Client            message builders + one-request/one-reply rpc() on top
                        of a P9Conn. msize, tag and fid numbering are all
                        caller-choosable; nothing is hard-coded into rpc().

Usage:
    p9-client.py <host> <port> [--ro-port <n>]
                                            run the assertion suite: the
                                            functional round-trip first, then
                                            the ADVERSARIAL suite (malformed
                                            frames, pipelined segments,
                                            hostile offsets, the read-only
                                            gate on <ro-port>).
    p9-client.py --linger <sec> <host> <port>
                                            open a connection, leave a read
                                            view + a write stream + a
                                            directory iterator open on it,
                                            print `LINGER READY`, and hold it
                                            for <sec> seconds. The harness
                                            uses this so the server's free
                                            path reaps a LIVE connection.

Every assertion prints its exact line on success; on failure it prints
`MISSING <line>` plus a `DIAG` line carrying what actually came back, so the
harness's exact-line grep fails and the log says why.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass

# --- 9P2000.L message types (mirrors src/9p/axl-9p-internal.h) -------------
TVERSION, RVERSION = 100, 101
RLERROR = 7
TATTACH, RATTACH = 104, 105
TWALK, RWALK = 110, 111
TLOPEN, RLOPEN = 12, 13
TREAD, RREAD = 116, 117
TREADDIR, RREADDIR = 40, 41
TCLUNK, RCLUNK = 120, 121
TSTATFS = 8              # implemented by nobody here -- must draw Rlerror(EPROTO)
TLCREATE, RLCREATE = 14, 15
TRENAME, RRENAME = 20, 21
TMKDIR, RMKDIR = 72, 73
TWRITE, RWRITE = 118, 119
TREMOVE, RREMOVE = 122, 123
TGETATTR, RGETATTR = 24, 25
TSETATTR, RSETATTR = 26, 27
TFSYNC, RFSYNC = 50, 51

NINEP_VERSION = "9P2000.L"
DEFAULT_MSIZE = 8192
MSG_HDR_LEN = 7          # size[4] type[1] tag[2]
RDATA_HDR_LEN = 11       # size[4] type[1] tag[2] count[4]
TWRITE_HDR_LEN = 23      # ...plus fid[4] offset[8] count[4], ahead of data
QID_LEN = 13
NOFID = 0xFFFFFFFF

# Rgetattr body: valid[8] qid[13] mode[4] uid[4] gid[4] + fifteen [8] fields
# (nlink rdev size blksize blocks a/m/c/b-time sec+nsec gen data_version).
RGETATTR_BODY_LEN = 8 + QID_LEN + 4 + 4 + 4 + 15 * 8
GETATTR_BASIC = 0x000007FF

# Linux open(2) access modes / flags, as Tlopen carries them.
O_RDONLY = 0x0
O_WRONLY = 0x1
O_RDWR = 0x2
O_TRUNC = 0x200

# Tsetattr `valid` bits (9P2000.L). Only SIZE is acted on by this server.
SETATTR_MODE = 0x1
SETATTR_UID = 0x2
SETATTR_GID = 0x4
SETATTR_SIZE = 0x8
SETATTR_ATIME = 0x10
SETATTR_MTIME = 0x20

# Server-side ceilings the wire can reach, mirrored from
# src/9p/axl-9p-server-internal.h. A drift here shows up as a failed
# boundary assertion, which is the point of pinning them.
MAX_GROW = 16 * 1024 * 1024          # AXL_9P_SERVER_MAX_GROW
SERVER_MAX_MSIZE = 128 * 1024        # AXL_9P_SERVER_MAX_MSIZE
MIN_MSIZE = 512                      # AXL_9P_MIN_MSIZE
U64_MAX = 0xFFFFFFFFFFFFFFFF
U32_MAX = 0xFFFFFFFF

QTDIR = 0x80             # qid.type bit for a directory
QTFILE = 0x00
DT_DIR = 4               # dirent d_type
DT_REG = 8

S_IFMT = 0o170000
S_IFDIR = 0o040000
S_IFREG = 0o100000

# Rlerror codes this suite expects to see (Linux errno values -- the
# vocabulary src/9p/axl-9p-server-internal.h fixes).
EPERM = 1
ENOENT = 2
EIO = 5
EBADF = 9
ENOMEM = 12
EEXIST = 17
EXDEV = 18
ENOTDIR = 20
EISDIR = 21
EINVAL = 22
EFBIG = 27
EROFS = 30
ENOTEMPTY = 39
EPROTO = 71

# The tree 9p-server-selftest.c seeds before it listens.
EXPECTED_HELLO = b"hello from 9p server\n"
EXPECTED_INNER = b"inner\n"


class P9Error(Exception):
    """The server answered Rlerror."""

    def __init__(self, ecode: int) -> None:
        super().__init__(f"Rlerror errno={ecode}")
        self.ecode: int = ecode


class P9Protocol(Exception):
    """The server answered something this client cannot parse or expect."""


class P9Closed(Exception):
    """The server closed the connection mid-message."""


@dataclass(frozen=True)
class Qid:
    type: int
    version: int
    path: int


@dataclass(frozen=True)
class DirEntry:
    qid: Qid
    offset: int
    dtype: int
    name: str


@dataclass(frozen=True)
class Attr:
    valid: int
    qid: Qid
    mode: int
    uid: int
    gid: int
    nlink: int
    rdev: int
    size: int
    blksize: int
    blocks: int
    body_len: int


# --- wire primitives -------------------------------------------------------

def put_str(value: str) -> bytes:
    """Encode a 9P string[s]: len[2] followed by the UTF-8 bytes."""
    raw = value.encode("utf-8")
    return struct.pack("<H", len(raw)) + raw


def get_str(body: bytes, off: int) -> tuple[str, int]:
    """Decode a 9P string[s] at `off`; return (value, offset past it).

    STRICT decoding on purpose: a name the server could not have produced must
    surface as a protocol error, not silently become U+FFFD. 6a's names are all
    ASCII, so this can only fire on a genuine defect -- and 6b, which feeds the
    server bytes it never sees from a real client, needs the failure to be
    loud."""
    (nlen,) = struct.unpack_from("<H", body, off)
    off += 2
    raw = body[off:off + nlen]
    try:
        return raw.decode("utf-8"), off + nlen
    except UnicodeDecodeError as exc:
        raise P9Protocol(f"name is not valid UTF-8: {raw!r} ({exc})") from exc


def get_qid(body: bytes, off: int) -> tuple[Qid, int]:
    """Decode a qid (type[1] version[4] path[8]) at `off`."""
    qtype, qver, qpath = struct.unpack_from("<BIQ", body, off)
    return Qid(qtype, qver, qpath), off + QID_LEN


def dirent_names(batches: list[list[DirEntry]]) -> list[str]:
    """Flatten readdir_all's per-call batches into one name list, in order."""
    return [entry.name for batch in batches for entry in batch]


def frame(mtype: int, tag: int, body: bytes, size: int | None = None) -> bytes:
    """Build one wire frame: size[4] type[1] tag[2] body.

    `size` defaults to the truthful MSG_HDR_LEN + len(body). Passing it
    explicitly is how a caller lies about the frame length without going
    anywhere near the message builders.
    """
    declared = MSG_HDR_LEN + len(body) if size is None else size
    return struct.pack("<IBH", declared, mtype, tag) + body


# --- message bodies, separate from the request/reply round trip -----------
#
# The pipelined cases put SEVERAL requests in one TCP segment, so they need
# the body of a message without rpc()'s "now read the reply". Splitting the
# bodies out is what lets P9Client's ordinary builders and the raw pipelined
# sends share one definition of each layout instead of two that can drift.

def body_version(msize: int, version: str = NINEP_VERSION) -> bytes:
    return struct.pack("<I", msize) + put_str(version)


def body_attach(fid: int, uname: str = "axl", aname: str = "/") -> bytes:
    return (struct.pack("<II", fid, NOFID) + put_str(uname) + put_str(aname)
            + struct.pack("<I", 0))


def body_clunk(fid: int) -> bytes:
    return struct.pack("<I", fid)


def body_getattr(fid: int, mask: int = GETATTR_BASIC) -> bytes:
    return struct.pack("<IQ", fid, mask)


def body_walk(fid: int, newfid: int, names: list[str]) -> bytes:
    out = struct.pack("<IIH", fid, newfid, len(names))
    for name in names:
        out += put_str(name)
    return out


class P9Conn:
    """One TCP connection, framing only -- no message semantics.

    `send_raw` takes arbitrary bytes, so a caller can concatenate two frames
    into a single segment (`conn.send_raw(frame(...) + frame(...))`) or emit
    a deliberately malformed one. Several P9Conn objects may be open against
    the same server at once.
    """

    def __init__(self, host: str, port: int, timeout: float = 20.0) -> None:
        self.sock: socket.socket = socket.create_connection((host, port),
                                                            timeout)
        self.sock.settimeout(timeout)
        # Size prefix of the last reply read, so a caller can assert that a
        # reply the server CLAMPED really came back within the negotiated
        # msize -- the payload length alone cannot say that.
        self.last_reply_size: int = 0

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def send(self, mtype: int, tag: int, body: bytes,
             size: int | None = None) -> None:
        self.send_raw(frame(mtype, tag, body, size))

    def recv_exact(self, count: int) -> bytes:
        buf = bytearray()
        while len(buf) < count:
            chunk = self.sock.recv(count - len(buf))
            if not chunk:
                raise P9Closed(f"peer closed after {len(buf)} of {count} bytes")
            buf += chunk
        return bytes(buf)

    def recv_msg(self) -> tuple[int, int, bytes]:
        """Read one reply; return (type, tag, body)."""
        size, mtype, tag = struct.unpack("<IBH", self.recv_exact(MSG_HDR_LEN))
        if size < MSG_HDR_LEN:
            raise P9Protocol(f"reply declared size {size} < {MSG_HDR_LEN}")
        self.last_reply_size = size
        return mtype, tag, self.recv_exact(size - MSG_HDR_LEN)

    def peer_closed(self, timeout: float = 8.0) -> str:
        """Answer, FROM THE CLIENT SIDE ONLY, whether the server dropped this
        connection: "closed" when the peer sent a FIN or a reset, and a
        description of what it did instead otherwise.

        Deliberately not a log grep. A reaped connection and a server that
        merely stopped answering look identical in the guest's log, and a
        server that faulted looks identical to both -- only the socket tells
        the three apart. `timeout` bounds the wait: a server that neither
        answers nor closes is a HANG, and this reports it as one rather than
        blocking the suite."""
        old = self.sock.gettimeout()
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(1)
        except TimeoutError:
            return f"still open: no reply and no FIN within {timeout}s"
        except ConnectionResetError:
            return "closed"
        except OSError as exc:
            return f"socket error {exc}"
        finally:
            self.sock.settimeout(old)
        return "closed" if data == b"" else f"still open, sent {data!r}"

    def close(self) -> None:
        self.sock.close()


class P9Client:
    """9P2000.L message builders over a P9Conn.

    Tags and fids come from monotonic counters by default; every method takes
    them explicitly too, so a caller can reuse, collide or invent one.
    """

    def __init__(self, conn: P9Conn, msize: int = DEFAULT_MSIZE) -> None:
        self.conn: P9Conn = conn
        self.msize: int = msize
        self._tag: int = 0
        self._fid: int = 0

    # -- allocation ---------------------------------------------------------

    def next_tag(self) -> int:
        self._tag = (self._tag + 1) & 0xFFFF
        return self._tag

    def alloc_fid(self) -> int:
        self._fid += 1
        return self._fid

    # -- one request, one reply --------------------------------------------

    def rpc(self, mtype: int, body: bytes, expect: int,
            tag: int | None = None, size: int | None = None) -> bytes:
        use_tag = self.next_tag() if tag is None else tag
        self.conn.send(mtype, use_tag, body, size)
        rtype, rtag, rbody = self.conn.recv_msg()
        if rtag != use_tag:
            raise P9Protocol(f"reply tag {rtag} != request tag {use_tag}")
        if rtype == RLERROR:
            (ecode,) = struct.unpack_from("<I", rbody, 0)
            raise P9Error(ecode)
        if rtype != expect:
            raise P9Protocol(f"reply type {rtype}, expected {expect}")
        return rbody

    # -- session ------------------------------------------------------------

    def version(self, msize: int | None = None,
                version: str = NINEP_VERSION) -> tuple[int, str]:
        """Tversion. A REFUSAL (Rversion(..., "unknown")) leaves self.msize
        alone, mirroring the server: it echoes the refused value back and
        keeps the session at whatever msize was last negotiated, so a client
        that adopted the echoed 0 would then build frames the server never
        agreed to."""
        want = self.msize if msize is None else msize
        body = self.rpc(TVERSION, body_version(want, version), RVERSION,
                        tag=0xFFFF)
        (got_msize,) = struct.unpack_from("<I", body, 0)
        got_version, _ = get_str(body, 4)
        if got_version != "unknown":
            self.msize = min(want, got_msize)
        return got_msize, got_version

    def attach(self, fid: int | None = None, uname: str = "axl",
               aname: str = "/") -> tuple[int, Qid]:
        use = self.alloc_fid() if fid is None else fid
        rbody = self.rpc(TATTACH, body_attach(use, uname, aname), RATTACH)
        qid, _ = get_qid(rbody, 0)
        return use, qid

    def walk(self, fid: int, names: list[str],
             newfid: int | None = None) -> tuple[int, list[Qid]]:
        """Twalk. Returns (newfid, qids); a PARTIAL walk leaves newfid unbound
        per walk(5), so the caller must compare len(qids) against len(names)
        before using it."""
        use = self.alloc_fid() if newfid is None else newfid
        rbody = self.rpc(TWALK, body_walk(fid, use, names), RWALK)
        (nwqid,) = struct.unpack_from("<H", rbody, 0)
        off = 2
        qids: list[Qid] = []
        for _ in range(nwqid):
            qid, off = get_qid(rbody, off)
            qids.append(qid)
        return use, qids

    def walk_to(self, fid: int, names: list[str],
                newfid: int | None = None) -> int:
        """walk() that refuses a partial result -- for paths that must exist."""
        use, qids = self.walk(fid, names, newfid)
        if len(qids) != len(names):
            raise P9Protocol(
                f"partial walk: {len(qids)} of {len(names)} components")
        return use

    def clunk(self, fid: int) -> None:
        self.rpc(TCLUNK, body_clunk(fid), RCLUNK)

    def getattr(self, fid: int, mask: int = GETATTR_BASIC) -> Attr:
        body = self.rpc(TGETATTR, body_getattr(fid, mask), RGETATTR)
        (valid,) = struct.unpack_from("<Q", body, 0)
        qid, off = get_qid(body, 8)
        mode, uid, gid = struct.unpack_from("<III", body, off)
        off += 12
        nlink, rdev, size, blksize, blocks = struct.unpack_from("<QQQQQ",
                                                                body, off)
        return Attr(valid, qid, mode, uid, gid, nlink, rdev, size, blksize,
                    blocks, len(body))

    # -- open / read / write ------------------------------------------------

    def lopen(self, fid: int, flags: int) -> tuple[Qid, int]:
        body = self.rpc(TLOPEN, struct.pack("<II", fid, flags), RLOPEN)
        qid, off = get_qid(body, 0)
        (iounit,) = struct.unpack_from("<I", body, off)
        return qid, iounit

    def tread(self, fid: int, offset: int, count: int) -> bytes:
        """ONE Tread. Returns exactly what the server sent, short reads and
        all -- the EOF and offset assertions depend on that."""
        body = self.rpc(TREAD, struct.pack("<IQI", fid, offset, count), RREAD)
        (got,) = struct.unpack_from("<I", body, 0)
        return body[4:4 + got]

    def read_all(self, fid: int, limit: int = 1 << 20) -> bytes:
        chunk = self.msize - RDATA_HDR_LEN
        out = bytearray()
        while len(out) < limit:
            part = self.tread(fid, len(out), chunk)
            if not part:
                break
            out += part
        return bytes(out)

    def twrite(self, fid: int, offset: int, data: bytes,
               count: int | None = None) -> int:
        """ONE Twrite. `count` defaults to len(data); passing it separately is
        how 6b makes the count field lie about the payload."""
        declared = len(data) if count is None else count
        body = struct.pack("<IQI", fid, offset, declared) + data
        rbody = self.rpc(TWRITE, body, RWRITE)
        (wrote,) = struct.unpack_from("<I", rbody, 0)
        return wrote

    def write_all(self, fid: int, data: bytes, offset: int = 0) -> int:
        """Twrite `data` in as few messages as the negotiated msize allows.

        Chunked at msize - TWRITE_HDR_LEN, NOT at msize - 11: a Twrite frame
        carries fid+offset+count ahead of its payload, and a frame larger than
        the negotiated msize is a framing violation the server reaps the
        connection for -- so the write bound is genuinely tighter than the
        read one."""
        sent = 0
        while sent < len(data):
            chunk = data[sent:sent + self.msize - TWRITE_HDR_LEN]
            n = self.twrite(fid, offset + sent, chunk)
            if n == 0:
                raise P9Protocol(f"Twrite of {len(chunk)} bytes wrote 0")
            sent += n
        return sent

    def setattr(self, fid: int, valid: int, size: int = 0, mode: int = 0,
                uid: int = 0, gid: int = 0) -> None:
        body = (struct.pack("<IIIII", fid, valid, mode, uid, gid)
                + struct.pack("<QQQQQ", size, 0, 0, 0, 0))
        self.rpc(TSETATTR, body, RSETATTR)

    def treaddir(self, fid: int, offset: int, count: int) -> list[DirEntry]:
        """ONE Treaddir. An empty list means end-of-directory OR that no
        record fit in `count` -- the caller distinguishes by re-asking from
        the last offset it saw."""
        body = self.rpc(TREADDIR, struct.pack("<IQI", fid, offset, count),
                        RREADDIR)
        (dcount,) = struct.unpack_from("<I", body, 0)
        data = body[4:4 + dcount]
        entries: list[DirEntry] = []
        off = 0
        while off < len(data):
            qid, off = get_qid(data, off)
            rec_off, dtype = struct.unpack_from("<QB", data, off)
            off += 9
            name, off = get_str(data, off)
            entries.append(DirEntry(qid, rec_off, dtype, name))
        return entries

    def readdir_all(self, fid: int, count: int,
                    max_calls: int = 256) -> list[list[DirEntry]]:
        """Drive Treaddir to exhaustion, resuming from the last record's
        offset. `count` small enough to truncate a reply is what exercises
        the server's cursor arithmetic across calls.

        Returns the PER-CALL batches, including the final empty one, rather
        than a flattened list: how the entries were split across round trips
        is the thing under test here, and a caller handed only the flat result
        cannot tell a genuinely resumed listing from a server that ignored
        `count` and answered it all in reply #1."""
        batches: list[list[DirEntry]] = []
        cursor = 0
        for _ in range(max_calls):
            batch = self.treaddir(fid, cursor, count)
            batches.append(batch)
            if not batch:
                return batches
            nxt = batch[-1].offset
            if nxt <= cursor:
                raise P9Protocol(
                    f"readdir cursor did not advance: {cursor} -> {nxt}")
            cursor = nxt
        raise P9Protocol(f"readdir did not terminate in {max_calls} calls")

    # -- namespace ----------------------------------------------------------

    def lcreate(self, dfid: int, name: str, flags: int,
                mode: int = 0o644, gid: int = 0) -> tuple[Qid, int]:
        """Tlcreate REBINDS dfid to the new, already-open file."""
        body = (struct.pack("<I", dfid) + put_str(name)
                + struct.pack("<III", flags, mode, gid))
        rbody = self.rpc(TLCREATE, body, RLCREATE)
        qid, off = get_qid(rbody, 0)
        (iounit,) = struct.unpack_from("<I", rbody, off)
        return qid, iounit

    def mkdir(self, dfid: int, name: str, mode: int = 0o755,
              gid: int = 0) -> Qid:
        body = (struct.pack("<I", dfid) + put_str(name)
                + struct.pack("<II", mode, gid))
        qid, _ = get_qid(self.rpc(TMKDIR, body, RMKDIR), 0)
        return qid

    def remove(self, fid: int) -> None:
        """Tremove clunks the fid whether it succeeds or not."""
        self.rpc(TREMOVE, struct.pack("<I", fid), RREMOVE)

    def rename(self, fid: int, dfid: int, name: str) -> None:
        body = struct.pack("<II", fid, dfid) + put_str(name)
        self.rpc(TRENAME, body, RRENAME)

    def fsync(self, fid: int, datasync: int = 0) -> None:
        self.rpc(TFSYNC, struct.pack("<II", fid, datasync), RFSYNC)


# Assertions this suite makes that the server currently CANNOT satisfy,
# because of a defect this task found and deliberately did not fix. The
# assertion text is NOT weakened -- it stays exactly what correct behavior
# would print -- and the harness fails the run if a listed line starts
# passing, so the defect cannot be fixed without someone coming back here.
#
# The VALUE is the failure that is expected, matched as a substring of the
# assertion's own diagnostic: an XFAIL has to mean "failed for the reason we
# know about", not merely "did not pass". An assertion that started failing
# some OTHER way is a NEW defect and must show up as a red MISSING line, not
# hide inside this one's exemption.
#
# Empty. The one entry this suite carried -- "CASE37 Tfsync on a write fid =
# Rfsync", which answered Rlerror(EIO=5) because axl_fflush() on a FILE stream
# always failed -- is fixed, and the line is now an ordinary assertion. The
# machinery stays for the next find.
KNOWN_DEFECTS: dict[str, str] = {}


class Checks:
    """Exact-line result reporting. A pass prints the line the harness greps
    for verbatim; a failure prints `MISSING <line>` plus a DIAG, so the grep
    fails AND the log explains it."""

    def __init__(self) -> None:
        self.passed: int = 0
        self.failed: int = 0
        self.xfailed: int = 0

    def check(self, cond: bool, line: str, diag: str = "") -> None:
        if cond:
            self.passed += 1
            print(line, flush=True)
            return
        want = KNOWN_DEFECTS.get(line)
        if want is not None and want in diag:
            self.xfailed += 1
            print(f"XFAIL {line}", flush=True)
            print(f"DIAG [{line}] {diag}", flush=True)
            return
        if want is not None:
            # Known-broken, but broken DIFFERENTLY. That is a new finding and
            # is reported as an ordinary failure, exemption or not.
            print(f"DIAG [{line}] known defect expected {want!r}, got a "
                  f"different failure", flush=True)
        self.failed += 1
        print(f"MISSING {line}", flush=True)
        print(f"DIAG [{line}] {diag}", flush=True)

    def abort(self, group: str, diag: str) -> None:
        """A whole group died on an exception nothing expected. Counted as a
        failure and printed loudly, but WITHOUT a pass-line of its own: the
        group's own assertions are already missing from the output, which is
        what the harness grades, and a "group completed" line that only ever
        prints on success would be a tautology dressed as coverage."""
        self.failed += 1
        print(f"GROUP-ABORT {group}: {diag}", flush=True)

    def expect_ok(self, line: str, call: Callable[[], object]) -> None:
        """Run `call` and assert the server did NOT answer Rlerror. For the
        replies that carry no field worth reading -- Rfsync, Rsetattr -- where
        "it answered at all, and answered the right message type" IS the whole
        contract (rpc() raises on any other type)."""
        try:
            call()
        except P9Error as exc:
            self.check(False, line, f"Rlerror errno={exc.ecode}")
            return
        except P9Protocol as exc:
            self.check(False, line, f"protocol error: {exc}")
            return
        self.check(True, line)

    def expect_error(self, want: int, line: str,
                     call: Callable[[], object]) -> None:
        """Run `call` and assert it raised Rlerror with errno `want`."""
        try:
            call()
        except P9Error as exc:
            self.check(exc.ecode == want, line, f"errno={exc.ecode}, want {want}")
            return
        except P9Protocol as exc:
            self.check(False, line, f"protocol error: {exc}")
            return
        self.check(False, line, f"no Rlerror at all (wanted errno {want})")


def run_checks(ck: Checks, host: str, port: int) -> None:
    """Drive the guest server's functional surface and assert on every reply."""
    conn = P9Conn(host, port)
    client = P9Client(conn, msize=DEFAULT_MSIZE)

    # --- session -----------------------------------------------------------
    got_msize, got_version = client.version()
    ck.check(got_msize == DEFAULT_MSIZE and got_version == NINEP_VERSION,
             f"VERSION msize={DEFAULT_MSIZE} version={NINEP_VERSION}",
             f"got msize={got_msize} version={got_version!r}")

    root, root_qid = client.attach()
    ck.check(root_qid.type == QTDIR, "ATTACH root isdir=1",
             f"qid.type=0x{root_qid.type:02x}")

    # --- getattr on a file and on a directory ------------------------------
    hfid = client.walk_to(root, ["hello.txt"])
    hattr = client.getattr(hfid)
    ck.check(hattr.body_len == RGETATTR_BODY_LEN,
             f"GETATTR body={RGETATTR_BODY_LEN}",
             f"got {hattr.body_len} bytes")
    ck.check(hattr.size == len(EXPECTED_HELLO),
             f"GETATTR hello.txt size={len(EXPECTED_HELLO)}",
             f"got size={hattr.size}")
    ck.check(hattr.mode & S_IFMT == S_IFREG, "GETATTR hello.txt isdir=0",
             f"mode=0o{hattr.mode:o}")

    sfid = client.walk_to(root, ["sub"])
    sattr = client.getattr(sfid)
    ck.check(sattr.mode & S_IFMT == S_IFDIR, "GETATTR sub isdir=1",
             f"mode=0o{sattr.mode:o}")
    client.clunk(sfid)

    # --- read: whole file, non-zero offset, at EOF, past EOF ---------------
    client.lopen(hfid, O_RDONLY)
    data = client.read_all(hfid)
    ck.check(data == EXPECTED_HELLO, "READ hello.txt exact = ok",
             f"got {data!r}")
    ck.check(data.decode("utf-8", "replace").rstrip("\n")
             == "hello from 9p server",
             "READ hello.txt = hello from 9p server", f"got {data!r}")

    at_off = client.tread(hfid, 6, 4)
    ck.check(at_off == b"from", "READ offset = ok", f"got {at_off!r}")

    at_eof = client.tread(hfid, len(EXPECTED_HELLO), 4096)
    ck.check(at_eof == b"", "READ at-eof = 0", f"got {len(at_eof)} bytes")

    past_eof = client.tread(hfid, 1 << 20, 4096)
    ck.check(past_eof == b"", "READ past-eof = 0",
             f"got {len(past_eof)} bytes")
    client.clunk(hfid)

    # --- walk: nested path, and a name that does not exist ----------------
    nfid, nqids = client.walk(root, ["sub", "inner.txt"])
    nested_ok = (len(nqids) == 2 and nqids[0].type == QTDIR
                 and nqids[1].type == QTFILE)
    ck.check(nested_ok, "WALK sub/inner.txt = ok", f"qids={nqids}")
    if nested_ok:
        client.lopen(nfid, O_RDONLY)
        inner = client.read_all(nfid)
        ck.check(inner == EXPECTED_INNER, "WALK sub/inner.txt read = ok",
                 f"got {inner!r}")
        client.clunk(nfid)
    else:
        ck.check(False, "WALK sub/inner.txt read = ok", "nested walk failed")

    ck.expect_error(ENOENT, "WALK missing = ENOENT",
                    lambda: client.walk(root, ["nosuchfile"]))

    # --- readdir: synthetic . / .., full listing, and a RESUMED one -------
    dfid, _ = client.walk(root, [])
    client.lopen(dfid, O_RDONLY)
    full = client.readdir_all(dfid, client.msize - RDATA_HDR_LEN)
    client.clunk(dfid)
    names = dirent_names(full)
    entries = [entry for batch in full for entry in batch]
    kinds = {entry.name: entry.dtype for entry in entries}
    offsets = {entry.name: entry.offset for entry in entries}

    ck.check("." in names, "READDIR contains .", f"names={names}")
    ck.check(".." in names, "READDIR contains ..", f"names={names}")
    ck.check("hello.txt" in names, "READDIR contains hello.txt",
             f"names={names}")
    ck.check("sub" in names, "READDIR contains sub", f"names={names}")
    ck.check(len(set(names)) == len(names), "READDIR no duplicates",
             f"names={names}")
    ck.check(kinds.get("hello.txt") == DT_REG and kinds.get("sub") == DT_DIR
             and kinds.get(".") == DT_DIR and kinds.get("..") == DT_DIR,
             "READDIR types = ok", f"kinds={kinds}")
    # The two synthetic records are emitted at fixed cursors 0+1 and 1+1,
    # independent of the backing -- only the REAL entries' cursors shift when
    # the export root is a subdirectory carrying on-disk "." / ".." entries.
    ck.check(offsets.get(".") == 1, "READDIR . offset=1",
             f"offsets={offsets}")
    ck.check(offsets.get("..") == 2, "READDIR .. offset=2",
             f"offsets={offsets}")

    # A count that cannot hold the whole listing forces the server's cursor
    # arithmetic to carry across calls. 64 bytes fits "." (25 B) and ".."
    # (26 B) and nothing more, so at least two more round trips follow.
    #
    # The result alone does NOT prove resumption happened -- a server that
    # ignored `count` and answered everything in reply #1 would produce the
    # same flat list. So the SHAPE of the exchange is asserted too: reply #1
    # must carry exactly [".", ".."], and the exchange must take at least
    # three Treaddir round trips (two carrying records, one empty terminator).
    rfid, _ = client.walk(root, [])
    client.lopen(rfid, O_RDONLY)
    batches = client.readdir_all(rfid, 64)
    client.clunk(rfid)
    chunked = dirent_names(batches)
    shapes = [[entry.name for entry in batch] for batch in batches]
    ck.check(chunked == names, "READDIR resumed = ok",
             f"full={names} chunked={chunked}")
    ck.check(len(batches) >= 1 and shapes[0] == [".", ".."],
             "READDIR chunked first reply = . ..", f"batches={shapes}")
    ck.check(len(batches) >= 3, "READDIR chunked round-trips >= 3",
             f"batches={shapes}")

    # --- write: at 0, read back, then at a non-zero offset ----------------
    wfid, _ = client.walk(root, [])
    client.lcreate(wfid, "wtest.txt", O_RDWR)
    payload = b"hello-write"
    wrote = client.twrite(wfid, 0, payload)
    ck.check(wrote == len(payload), f"WRITE count={len(payload)}",
             f"got {wrote}")
    back = client.tread(wfid, 0, 4096)
    ck.check(back == payload, "WRITE+READBACK ok", f"got {back!r}")

    tail = b"XYZ"
    wrote2 = client.twrite(wfid, 20, tail)
    ck.check(wrote2 == len(tail), "WRITE offset count = ok", f"got {wrote2}")
    back2 = client.tread(wfid, 20, 4096)
    ck.check(back2 == tail, "WRITE offset = ok", f"got {back2!r}")
    wattr = client.getattr(wfid)
    ck.check(wattr.size == 23, "WRITE offset size=23", f"got {wattr.size}")
    client.clunk(wfid)

    # --- mkdir, then see it in a fresh listing ----------------------------
    mfid, _ = client.walk(root, [])
    new_qid = client.mkdir(mfid, "newdir")
    ck.check(new_qid.type == QTDIR, "MKDIR qid isdir=1",
             f"qid.type=0x{new_qid.type:02x}")

    d2, _ = client.walk(root, [])
    client.lopen(d2, O_RDONLY)
    names2 = dirent_names(client.readdir_all(d2,
                                             client.msize - RDATA_HDR_LEN))
    client.clunk(d2)
    ck.check("newdir" in names2 and "wtest.txt" in names2,
             "MKDIR+READDIR ok", f"names={names2}")

    # --- clunk, then reuse the clunked fid --------------------------------
    client.clunk(mfid)
    ck.expect_error(EBADF, "CLUNK reuse = EBADF",
                    lambda: client.getattr(mfid))

    # --- remove a file, and refuse to remove a non-empty directory --------
    rmfid = client.walk_to(root, ["wtest.txt"])
    client.remove(rmfid)
    ck.expect_error(ENOENT, "REMOVE file = ok",
                    lambda: client.walk(root, ["wtest.txt"]))

    sub2 = client.walk_to(root, ["sub"])
    ck.expect_error(ENOTEMPTY, "REMOVE nonempty-dir = ENOTEMPTY",
                    lambda: client.remove(sub2))

    client.clunk(root)
    conn.close()


# ===========================================================================
# Adversarial suite
# ===========================================================================
#
# Everything below feeds the server input no conforming client produces:
# frames that lie about their own length, several requests in ONE TCP
# segment, 64-bit offsets, made-up dirent cursors, msizes below the floor,
# and every mutating message type against a read-only export. Case numbers
# refer to the accumulated list in .superpowers/sdd/task-3-report.md; cases
# the list does not number are labelled EXTRA.
#
# Three rules hold throughout, and the assertions are shaped by them:
#   1. Every assertion reads bytes that came back over the socket. The guest
#      asserts nothing and its log is never graded.
#   2. A reaped connection is proven reaped from the CLIENT side
#      (P9Conn.peer_closed), never from a log line.
#   3. Every hostile case is followed by a known-good request -- on a fresh
#      connection where the case reaped the old one. A server that died
#      quietly and a server that correctly refused the input look identical
#      without it.

def session(host: str, port: int, msize: int = DEFAULT_MSIZE,
            timeout: float = 20.0) -> tuple[P9Conn, P9Client, int]:
    """A fresh connection, versioned and attached. Returns (conn, client,
    root fid)."""
    conn = P9Conn(host, port, timeout)
    client = P9Client(conn, msize=msize)
    client.version()
    root, _ = client.attach()
    return conn, client, root


def still_alive(ck: Checks, client: P9Client, dfid: int, line: str) -> None:
    """Rule 3: drive a known-good request and assert the RIGHT answer comes
    back -- not merely that something did."""
    try:
        attr = client.getattr(dfid)
    except (P9Error, P9Protocol, P9Closed, OSError, struct.error) as exc:
        ck.check(False, line, f"{type(exc).__name__}: {exc}")
        return
    ck.check(attr.mode & S_IFMT == S_IFDIR
             and attr.body_len == RGETATTR_BODY_LEN,
             line, f"mode=0o{attr.mode:o} body={attr.body_len}")


def create_file(client: P9Client, parent: int, name: str, data: bytes) -> int:
    """Clone `parent`, Tlcreate `name` on the clone -- which REBINDS it to the
    new file, already open O_RDWR -- and write `data`. Returns that fid."""
    fid, _ = client.walk(parent, [])
    client.lcreate(fid, name, O_RDWR)
    if data:
        client.write_all(fid, data)
    return fid


def size_at(client: P9Client, parent: int, names: list[str]) -> int:
    """Size of a path, read through a throw-away fid, so a case can prove a
    refused write changed nothing."""
    fid = client.walk_to(parent, names)
    size = client.getattr(fid).size
    client.clunk(fid)
    return size


def exists(client: P9Client, parent: int, names: list[str]) -> bool:
    """Whether a path resolves, without leaving a fid behind either way."""
    try:
        fid, qids = client.walk(parent, names)
    except P9Error:
        return False
    if len(qids) != len(names):
        return False
    client.clunk(fid)
    return True


def pattern(n: int, seed: int = 0) -> bytes:
    """Deterministic non-repeating-ish payload, so a byte-exact comparison
    catches a reply assembled from the wrong offset."""
    return bytes(((i * 31 + seed * 17 + 7) & 0xFF) for i in range(n))


# --- framing: malformed, oversized, split and unknown ----------------------

def adv_framing(ck: Checks, host: str, port: int) -> None:
    # Case 3 -- an msize below AXL_9P_MIN_MSIZE must be REFUSED, with the
    # client's own value echoed back and the version string "unknown".
    conn = P9Conn(host, port)
    client = P9Client(conn)
    got, ver = client.version(msize=0)
    ck.check(got == 0 and ver == "unknown",
             "CASE03 Tversion msize=0 = Rversion 0/unknown",
             f"got msize={got} version={ver!r}")
    got, ver = client.version(msize=MIN_MSIZE - 1)
    ck.check(got == MIN_MSIZE - 1 and ver == "unknown",
             "CASE03 Tversion msize=511 = Rversion 511/unknown",
             f"got msize={got} version={ver!r}")

    # ...and the session is left UNNEGOTIATED, i.e. still at the 8192 the
    # connection was accepted with. Proven from the wire rather than from the
    # client's own bookkeeping: a frame of exactly 8192 bytes is accepted. Had
    # the server adopted the refused 511, this frame would exceed the
    # negotiated msize and the connection would be reaped instead. The
    # trailing padding is never read -- the reader stops after fid+mask.
    root, _ = client.attach()
    padded = body_getattr(root) + b"\x00" * (DEFAULT_MSIZE - MSG_HDR_LEN - 12)
    rbody = client.rpc(TGETATTR, padded, RGETATTR)
    ck.check(len(rbody) == RGETATTR_BODY_LEN
             and conn.last_reply_size == MSG_HDR_LEN + RGETATTR_BODY_LEN,
             "CASE03 refused msize leaves the session at 8192 = ok",
             f"body={len(rbody)} reply_size={conn.last_reply_size}")

    # Case 4 -- one byte over the negotiated msize is a framing violation the
    # server cannot resync from, so the connection is dropped. Only 19 bytes
    # go out; the server decides on the size PREFIX alone, before it ever
    # requires the body to be present.
    conn.send(TGETATTR, 1, body_getattr(root), size=DEFAULT_MSIZE + 1)
    verdict = conn.peer_closed()
    ck.check(verdict == "closed",
             "CASE04 frame one byte over msize = connection closed", verdict)
    conn.close()

    # EXTRA -- a size prefix below the 7-byte header is the other framing
    # violation, and takes the same path.
    conn2, _, _ = session(host, port)
    conn2.send_raw(struct.pack("<I", 3) + b"\x00\x00\x00")
    verdict = conn2.peer_closed()
    ck.check(verdict == "closed",
             "EXTRA frame size below the 7-byte header = connection closed",
             verdict)
    conn2.close()

    # Rule 3, for both reaps above.
    conn3, client3, root3 = session(host, port)
    still_alive(ck, client3, root3,
                "CASE04 server still serving after two reaped connections")

    # EXTRA -- one message arriving as two TCP segments must be reassembled,
    # not treated as two malformed ones. The mirror image of the pipelined
    # case: one frame in two segments rather than two frames in one.
    raw = frame(TGETATTR, 0x4242, body_getattr(root3))
    conn3.send_raw(raw[:5])
    time.sleep(0.3)
    conn3.send_raw(raw[5:])
    mtype, rtag, rbody = conn3.recv_msg()
    ck.check(mtype == RGETATTR and rtag == 0x4242
             and len(rbody) == RGETATTR_BODY_LEN,
             "EXTRA one frame split across two segments = reassembled",
             f"type={mtype} tag={rtag} body={len(rbody)}")

    # EXTRA -- a type this server does not implement is a clean Rlerror, not
    # a reap and not silence.
    ck.expect_error(EPROTO, "EXTRA unimplemented message type = EPROTO",
                    lambda: client3.rpc(TSTATFS, struct.pack("<I", root3),
                                        RLERROR))
    # EXTRA -- a body too short for the fields the handler reads.
    ck.expect_error(EINVAL, "EXTRA truncated Tclunk body = EINVAL",
                    lambda: client3.rpc(TCLUNK, b"\x01\x02", RLERROR))
    # EXTRA -- a string whose length field runs past the end of the frame.
    ck.expect_error(EINVAL, "EXTRA string length past end of frame = EINVAL",
                    lambda: client3.rpc(TWALK,
                                        struct.pack("<IIH", root3, 900, 1)
                                        + struct.pack("<H", 500) + b"abc",
                                        RLERROR))
    still_alive(ck, client3, root3,
                "EXTRA server alive after three malformed messages")
    conn3.close()


# --- THE pipelined case: two shipped Criticals had exactly this shape ------

def adv_pipelined(ck: Checks, host: str, port: int) -> None:
    """[Tclunk][Tversion(msize=131072)][Tattach][Tgetattr] in ONE TCP segment.

    The first Critical reallocated a txbuf that a pending axl_tcp_send_async
    was still reading; the second re-armed a recv into an rbuf the resumed
    drain then compacted and reallocated. Both are reached by exactly this
    shape, and the two trailing frames are deliberate: they sit in rbuf
    ACROSS Tversion's realloc, so the bytes that answer them have to survive
    both the realloc and the compaction that follows it."""
    conn = P9Conn(host, port)
    client = P9Client(conn)
    client.version()
    root, _ = client.attach()

    burst = (frame(TCLUNK, 10, body_clunk(root))
             + frame(TVERSION, 11, body_version(SERVER_MAX_MSIZE))
             + frame(TATTACH, 12, body_attach(5))
             + frame(TGETATTR, 13, body_getattr(5)))
    conn.send_raw(burst)
    replies = [conn.recv_msg() for _ in range(4)]

    t0, g0, b0 = replies[0]
    ck.check(t0 == RCLUNK and g0 == 10 and b0 == b"",
             "CASE01 pipelined 1/4 = Rclunk tag=10",
             f"type={t0} tag={g0} body={b0!r}")

    t1, g1, b1 = replies[1]
    v_msize = struct.unpack_from("<I", b1, 0)[0] if len(b1) >= 4 else -1
    v_str = get_str(b1, 4)[0] if len(b1) >= 6 else ""
    ck.check(t1 == RVERSION and g1 == 11 and v_msize == SERVER_MAX_MSIZE
             and v_str == NINEP_VERSION,
             "CASE01 pipelined 2/4 = Rversion msize=131072 tag=11",
             f"type={t1} tag={g1} msize={v_msize} version={v_str!r}")

    t2, g2, b2 = replies[2]
    a_qid = get_qid(b2, 0)[0] if len(b2) >= QID_LEN else Qid(0xFF, 0, 0)
    ck.check(t2 == RATTACH and g2 == 12 and a_qid.type == QTDIR,
             "CASE01 pipelined 3/4 = Rattach isdir=1 tag=12",
             f"type={t2} tag={g2} qid.type=0x{a_qid.type:02x}")

    t3, g3, b3 = replies[3]
    ck.check(t3 == RGETATTR and g3 == 13 and len(b3) == RGETATTR_BODY_LEN,
             "CASE01 pipelined 4/4 = Rgetattr body=153 tag=13",
             f"type={t3} tag={g3} body={len(b3)}")

    # The grown buffers now have to work. A max-msize Twrite is a 131072-byte
    # frame -- the largest the server will accept without reaping -- landing
    # in the rbuf that was reallocated mid-segment above.
    client.msize = SERVER_MAX_MSIZE
    payload = pattern(SERVER_MAX_MSIZE - TWRITE_HDR_LEN, seed=3)
    wfid = create_file(client, 5, "adv-big.bin", b"")
    wrote = client.twrite(wfid, 0, payload)
    ck.check(wrote == len(payload),
             f"CASE01 max-msize Twrite accepted count={len(payload)}",
             f"got {wrote}")
    back = client.tread(wfid, 0, SERVER_MAX_MSIZE - RDATA_HDR_LEN)
    ck.check(back == payload, "CASE01 max-msize write read back byte-exact",
             f"got {len(back)} bytes, first mismatch "
             f"{next((i for i, (x, y) in enumerate(zip(back, payload)) if x != y), None)}")
    # EXACT, not `<= SERVER_MAX_MSIZE`: that bound also holds at msize 8192,
    # so it could not tell a grown session from a refused grow on its own.
    # The whole payload came back in ONE reply or this number is wrong.
    ck.check(conn.last_reply_size == MSG_HDR_LEN + 4 + len(payload),
             f"CASE01 grown-msize Rread is exactly "
             f"{MSG_HDR_LEN + 4 + len(payload)} bytes on the wire",
             f"reply size {conn.last_reply_size}")
    client.clunk(wfid)
    still_alive(ck, client, 5, "CASE01 connection usable after the burst")
    conn.close()


def adv_burst(ck: Checks, host: str, port: int) -> None:
    """Case 2 -- a segment that fills the 8192-byte receive buffer with
    pipelined requests, so the drain pauses and resumes many times over and
    the recv can only be re-armed once the buffer has room again."""
    conn = P9Conn(host, port)
    client = P9Client(conn)
    client.version()
    root, _ = client.attach()

    each = MSG_HDR_LEN + 12                      # a Tgetattr frame is 19 bytes
    count = DEFAULT_MSIZE // each                # 431 -> 8189 bytes, one send
    blob = b"".join(frame(TGETATTR, i + 1, body_getattr(root))
                    for i in range(count))
    conn.send_raw(blob)

    good = 0
    for i in range(count):
        mtype, rtag, rbody = conn.recv_msg()
        if (mtype == RGETATTR and rtag == i + 1
                and len(rbody) == RGETATTR_BODY_LEN):
            good += 1
    ck.check(good == count,
             f"CASE02 {count} requests in one {len(blob)}-byte segment, all "
             "answered in order",
             f"{good} of {count} matched")
    still_alive(ck, client, root, "CASE02 connection usable after the burst")
    conn.close()


# --- walk: partial binding, escapes, caps, table exhaustion ----------------

def adv_walk(ck: Checks, host: str, port: int) -> None:
    conn, client, root = session(host, port)

    # Case 5 -- a PARTIAL walk replies Rwalk with the qids it managed, and
    # leaves newfid unbound. Proven by using the fid, not by trusting the
    # count: an unbound fid answers EBADF.
    part = client.alloc_fid()
    _, qids = client.walk(root, ["sub", "nosuchleaf"], newfid=part)
    ck.check(len(qids) == 1 and qids[0].type == QTDIR,
             "CASE05 partial walk = Rwalk with 1 of 2 qids",
             f"qids={qids}")
    ck.expect_error(EBADF, "CASE05 partial walk left newfid unbound = EBADF",
                    lambda: client.getattr(part))
    ck.expect_error(EBADF, "CASE05 clunk of the unbound newfid = EBADF",
                    lambda: client.clunk(part))

    # Root-escape attempts. All are ordinary partial-walk failures, so a walk
    # whose FIRST component is refused comes back ENOENT.
    ck.expect_error(ENOENT, "EXTRA walk .. at the export root = ENOENT",
                    lambda: client.walk(root, [".."]))
    ck.expect_error(ENOENT, "EXTRA walk a backslash-escaping name = ENOENT",
                    lambda: client.walk(root, ["..\\..\\secret"]))
    ck.expect_error(ENOENT, "EXTRA walk a slash-bearing name = ENOENT",
                    lambda: client.walk(root, ["sub/inner.txt"]))
    ck.expect_error(ENOENT, "EXTRA walk a NUL-bearing name = ENOENT",
                    lambda: client.walk(root, ["sub\x00x"]))
    # ..from a subdirectory, ".." is legal exactly once: the second one would
    # step above the root, so the walk stops there rather than escaping.
    up_fid = client.alloc_fid()
    _, up_qids = client.walk(root, ["sub", "..", ".."], newfid=up_fid)
    ck.check(len(up_qids) == 2,
             "EXTRA walk sub/../.. stops at the root = 2 qids",
             f"qids={len(up_qids)}")
    ck.expect_error(EBADF, "EXTRA that escaping walk bound nothing = EBADF",
                    lambda: client.getattr(up_fid))

    # fid-arithmetic refusals.
    dup = client.walk_to(root, ["sub"])
    ck.expect_error(EINVAL, "EXTRA Twalk onto an in-use newfid = EINVAL",
                    lambda: client.walk(root, ["sub"], newfid=dup))
    ck.expect_error(EINVAL, "EXTRA Twalk newfid==fid with nwname>0 = EINVAL",
                    lambda: client.walk(root, ["sub"], newfid=root))
    ck.expect_error(EBADF, "EXTRA Twalk from an unbound fid = EBADF",
                    lambda: client.walk(4242, ["sub"]))
    client.clunk(dup)

    # Case 6 -- nwname beyond what one Rwalk can carry within the negotiated
    # msize. (8192 - 9) / 13 = 629 qids fit; 630 must be refused UP FRONT,
    # never answered with a truncated reply.
    cap = (DEFAULT_MSIZE - 9) // QID_LEN
    ck.expect_error(EINVAL,
                    f"CASE06 nwname over the msize cap ({cap + 1}) = EINVAL",
                    lambda: client.walk(root, ["sub"] * (cap + 1)))
    still_alive(ck, client, root, "CASE06 connection alive after the over-cap walk")
    conn.close()


def adv_deep_walk(ck: Checks, host: str, port: int) -> None:
    """Case 6 -- a path deeper than Plan 9's traditional 16-component
    MAXWELEM, which this server deliberately does not enforce (our own client
    sends every component in ONE Twalk)."""
    conn, client, root = session(host, port)
    depth = 17
    names = [f"d{i}" for i in range(depth)]

    parent, _ = client.walk(root, [])
    for name in names:
        qid = client.mkdir(parent, name)
        if qid.type != QTDIR:
            raise P9Protocol(f"mkdir {name} did not answer a directory qid")
        nxt = client.walk_to(parent, [name])
        client.clunk(parent)
        parent = nxt
    client.clunk(parent)

    deep = client.alloc_fid()
    _, qids = client.walk(root, names, newfid=deep)
    ck.check(len(qids) == depth and all(q.type == QTDIR for q in qids),
             f"CASE06 walk of {depth} components in one Twalk = ok",
             f"{len(qids)} qids")
    attr = client.getattr(deep)
    ck.check(attr.mode & S_IFMT == S_IFDIR,
             "CASE06 the deep newfid is bound = ok", f"mode=0o{attr.mode:o}")
    client.clunk(deep)
    conn.close()


def adv_fid_table(ck: Checks, host: str, port: int) -> None:
    """EXTRA -- 128 fids is the per-connection table cap. The 129th must be
    refused with ENOMEM and the connection must stay usable, which is the
    difference between a bounded table and an overflow."""
    conn, client, root = session(host, port)
    held: list[int] = []
    for _ in range(127):                 # + the attach fid == 128 in use
        fid, _ = client.walk(root, [])
        held.append(fid)
    # Not "the loop ran 127 times" -- that is arithmetic, not an observation.
    # What the server has to be true for is that all 128 bindings are live at
    # once: the FIRST and the LAST both still resolve, which a table that
    # recycled a slot under pressure would not manage.
    first = client.getattr(held[0])
    last = client.getattr(held[-1])
    ck.check(first.mode & S_IFMT == S_IFDIR and last.mode & S_IFMT == S_IFDIR,
             "EXTRA all 128 fids in a full table are live at once = ok",
             f"first=0o{first.mode:o} last=0o{last.mode:o}")
    ck.expect_error(ENOMEM, "EXTRA the 129th fid = ENOMEM",
                    lambda: client.walk(root, []))
    still_alive(ck, client, root, "EXTRA connection alive with a full fid table")
    client.clunk(held[0])
    extra, _ = client.walk(root, [])
    reused = client.getattr(extra)
    ck.check(reused.mode & S_IFMT == S_IFDIR,
             "EXTRA a clunked table slot is reusable = ok",
             f"mode=0o{reused.mode:o}")
    conn.close()


# --- open / read: hostile offsets, wrong kinds, clamping -------------------

def adv_read(ck: Checks, host: str, port: int) -> None:
    conn, client, root = session(host, port)
    body = pattern(20000, seed=1)
    big = create_file(client, root, "adv-read.bin", body)
    client.clunk(big)

    # Case 7 -- the qid TYPE byte distinguishes the two kinds of open.
    ffid = client.walk_to(root, ["adv-read.bin"])
    fqid, iounit = client.lopen(ffid, O_RDONLY)
    ck.check(fqid.type == QTFILE and iounit == 0,
             "CASE07 Tlopen file = qid type 0x00, iounit 0",
             f"qid.type=0x{fqid.type:02x} iounit={iounit}")
    dfid = client.walk_to(root, ["sub"])
    dqid, diounit = client.lopen(dfid, O_RDONLY)
    ck.check(dqid.type == QTDIR and diounit == 0,
             "CASE07 Tlopen directory = qid type 0x80, iounit 0",
             f"qid.type=0x{dqid.type:02x} iounit={diounit}")

    # Case 8 -- an unbound fid, and a fid whose path went away after the walk.
    ck.expect_error(EBADF, "CASE08 Tlopen of an unbound fid = EBADF",
                    lambda: client.lopen(7777, O_RDONLY))
    doomed = create_file(client, root, "adv-doomed.bin", b"gone soon")
    client.clunk(doomed)
    stale = client.walk_to(root, ["adv-doomed.bin"])
    victim = client.walk_to(root, ["adv-doomed.bin"])
    client.remove(victim)
    ck.expect_error(ENOENT, "CASE08 Tlopen of a path deleted since the walk = ENOENT",
                    lambda: client.lopen(stale, O_RDONLY))
    client.clunk(stale)

    # EXTRA -- a directory cannot be opened for writing.
    wdir = client.walk_to(root, ["sub"])
    ck.expect_error(EISDIR, "EXTRA Tlopen directory O_WRONLY = EISDIR",
                    lambda: client.lopen(wdir, O_WRONLY))
    client.clunk(wdir)

    # Case 12 -- hostile read offsets are Rread(count=0), never an error.
    at_size = client.tread(ffid, len(body), 4096)
    ck.check(at_size == b"", "CASE12 Tread at offset==size = 0 bytes",
             f"{len(at_size)} bytes")
    past = client.tread(ffid, len(body) + 1_000_000, 4096)
    ck.check(past == b"", "CASE12 Tread past EOF = 0 bytes", f"{len(past)} bytes")
    huge = client.tread(ffid, U64_MAX, 4096)
    ck.check(huge == b"", "CASE12 Tread at offset 2^64-1 = 0 bytes",
             f"{len(huge)} bytes")

    # Case 13 -- a count larger than the msize is clamped to what the reply
    # can carry, and the REPLY's own size prefix stays within the msize.
    clamp = client.tread(ffid, 0, U32_MAX)
    ck.check(len(clamp) == DEFAULT_MSIZE - RDATA_HDR_LEN
             and clamp == body[:len(clamp)],
             f"CASE13 Tread count=2^32-1 clamped to {DEFAULT_MSIZE - RDATA_HDR_LEN}",
             f"got {len(clamp)} bytes")
    ck.check(conn.last_reply_size == DEFAULT_MSIZE,
             "CASE13 the clamped Rread is exactly msize on the wire",
             f"reply size {conn.last_reply_size}")

    # Case 11 -- the whole file, chunked at msize-11, byte-exact to EOF.
    whole = client.read_all(ffid, limit=len(body) + 4096)
    ck.check(whole == body, "CASE11 multi-chunk read to EOF byte-exact = ok",
             f"got {len(whole)} of {len(body)} bytes")

    # Case 14 / 17 -- the wrong kind of fid, and a fid never opened.
    ck.expect_error(EISDIR, "CASE14 Tread on a directory fid = EISDIR",
                    lambda: client.tread(dfid, 0, 16))
    unopened = client.walk_to(root, ["adv-read.bin"])
    ck.expect_error(EBADF, "CASE14 Tread on a never-opened fid = EBADF",
                    lambda: client.tread(unopened, 0, 16))
    ck.expect_error(ENOTDIR, "CASE17 Treaddir on a file fid = ENOTDIR",
                    lambda: client.treaddir(ffid, 0, 4096))
    ck.expect_error(EBADF, "CASE17 Treaddir on a never-opened fid = EBADF",
                    lambda: client.treaddir(unopened, 0, 4096))
    client.clunk(unopened)
    client.clunk(ffid)

    # Case 15 -- a SUBDIRECTORY carries on-disk "." and ".." of its own, and
    # they must not be emitted alongside the synthetic pair. Cursors are
    # opaque cookies, so only their ORDER and their relation to the two
    # synthetic records is asserted -- never a literal value.
    entries = [e for batch in client.readdir_all(dfid, 4096) for e in batch]
    names = [e.name for e in entries]
    reals = [e for e in entries if e.name not in (".", "..")]
    ck.check(names.count(".") == 1 and names.count("..") == 1,
             "CASE15 subdirectory listing has exactly one . and one ..",
             f"names={names}")
    ck.check([e.offset for e in entries if e.name == "."] == [1]
             and [e.offset for e in entries if e.name == ".."] == [2],
             "CASE15 synthetic . and .. carry cursors 1 and 2",
             f"offsets={[(e.name, e.offset) for e in entries]}")
    ck.check(all(e.offset >= 3 for e in reals)
             and [e.offset for e in reals] == sorted(e.offset for e in reals)
             and "inner.txt" in [e.name for e in reals],
             "CASE15 real entries follow with strictly later cursors",
             f"reals={[(e.name, e.offset) for e in reals]}")

    # Case 18 -- made-up cursors. Past the end is empty; 2^64-1 is empty and
    # harmless; going BACKWARDS re-reads the listing from the reopen path.
    beyond = client.treaddir(dfid, 1000, 4096)
    ck.check(beyond == [], "CASE18 Treaddir cursor past the end = empty",
             f"{len(beyond)} records")
    insane = client.treaddir(dfid, U64_MAX, 4096)
    ck.check(insane == [], "CASE18 Treaddir cursor 2^64-1 = empty",
             f"{len(insane)} records")
    rewound = [e.name for e in client.treaddir(dfid, 0, 4096)]
    ck.check(rewound == names,
             "CASE18 Treaddir rewound to cursor 0 = the same listing",
             f"rewound={rewound} first={names}")
    still_alive(ck, client, root, "CASE18 connection alive after made-up cursors")
    client.clunk(dfid)
    conn.close()


def adv_readdir_paging(ck: Checks, host: str, port: int) -> None:
    """Case 16 -- more entries than one reply can carry. Every entry must
    appear exactly once across the pages, including the ones whose record did
    not fit and left dir_pos ahead of the cursor the client comes back with
    (the reopen-and-re-skip path)."""
    conn, client, root = session(host, port)
    made = [f"page-{i:02d}.txt" for i in range(20)]
    dirfid, _ = client.walk(root, [])
    client.mkdir(dirfid, "pagedir")
    pdir = client.walk_to(root, ["pagedir"])
    for name in made:
        fid = create_file(client, pdir, name, b"x")
        client.clunk(fid)

    listing = client.walk_to(root, ["pagedir"])
    client.lopen(listing, O_RDONLY)
    batches = client.readdir_all(listing, 96)
    flat = dirent_names(batches)
    ck.check(len(batches) >= 4,
             "CASE16 the paged listing took at least 4 round trips",
             f"{len(batches)} batches: {[len(b) for b in batches]}")
    ck.check(sorted(n for n in flat if n not in (".", "..")) == sorted(made),
             "CASE16 every entry appears exactly once across the pages",
             f"got {sorted(n for n in flat if n not in ('.', '..'))}")
    ck.check(len(flat) == len(set(flat)), "CASE16 no entry repeated across pages",
             f"names={flat}")
    client.clunk(listing)

    # Case 19 -- Tversion mid-listing restarts the session: every fid is
    # dropped, including the one holding the open directory iterator.
    mid = client.walk_to(root, ["pagedir"])
    client.lopen(mid, O_RDONLY)
    first = client.treaddir(mid, 0, 96)
    ck.check(len(first) >= 1, "CASE19 one page taken before the renegotiation",
             f"{len(first)} records")
    got, ver = client.version(msize=DEFAULT_MSIZE)
    ck.check(got == DEFAULT_MSIZE and ver == NINEP_VERSION,
             "CASE19 Tversion mid-readdir = Rversion 8192/9P2000.L",
             f"msize={got} version={ver!r}")
    ck.expect_error(EBADF, "CASE19 the open directory fid is gone = EBADF",
                    lambda: client.treaddir(mid, first[-1].offset, 96))
    root2, _ = client.attach(fid=901)
    still_alive(ck, client, root2, "CASE19 session usable after the reset")
    conn.close()


# --- write: lying counts, far offsets, wrong kinds, staleness --------------

def adv_write(ck: Checks, host: str, port: int) -> None:
    conn, client, root = session(host, port)
    base = pattern(64, seed=5)
    wfid = create_file(client, root, "adv-write.bin", base)

    # Case 25 -- a count that lies about the frame. The payload pointer must
    # never be formed: EINVAL, connection intact, file untouched.
    def lying_write(claimed: int) -> Callable[[], object]:
        return lambda: client.twrite(wfid, 0, base, count=claimed)

    ck.expect_error(EINVAL, "CASE25 Twrite count overstated by 1 = EINVAL",
                    lying_write(len(base) + 1))
    ck.expect_error(EINVAL, "CASE25 Twrite count 0xFFFFFFFF = EINVAL",
                    lying_write(U32_MAX))
    after = size_at(client, root, ["adv-write.bin"])
    ck.check(after == len(base),
             f"CASE25 the lying writes changed nothing (size={len(base)})",
             f"size={after}")
    still_alive(ck, client, root, "CASE25 connection usable after a lying count")

    # Case 40 -- the far-offset denial of service. 24 bytes on the wire that
    # would otherwise zero-fill 4 GiB through a synchronous FAT driver on the
    # server's one loop.
    start = time.monotonic()
    ck.expect_error(EFBIG, "CASE40 Twrite at offset 0xFFFFFFFE = EFBIG",
                    lambda: client.twrite(wfid, 0xFFFFFFFE, b"x"))
    elapsed = time.monotonic() - start
    ck.check(elapsed < 2.0,
             "CASE40 the EFBIG refusal came back promptly (< 2s)",
             f"took {elapsed:.2f}s")
    unchanged = size_at(client, root, ["adv-write.bin"])
    ck.check(unchanged == len(base),
             "CASE40 the refused far write grew nothing",
             f"size={unchanged}")
    # One byte past the ceiling, measured from the file's own length.
    ck.expect_error(EFBIG,
                    "CASE40 Twrite ending one byte past MAX_GROW = EFBIG",
                    lambda: client.twrite(wfid, len(base) + MAX_GROW, b"x"))
    # ...and an ordinary grow well inside it still works.
    grew = client.twrite(wfid, len(base) + 4096, b"abcd")
    ck.check(grew == 4, "CASE40 a bounded grow past EOF still writes = 4",
             f"wrote {grew}")
    bounded = size_at(client, root, ["adv-write.bin"])
    ck.check(bounded == len(base) + 4100,
             f"CASE40 the bounded grow left size={len(base) + 4100}",
             f"size={bounded}")

    # Case 26 -- positional writes, deliberately out of order.
    ooo = create_file(client, root, "adv-ooo.bin", b"")
    client.twrite(ooo, 8, b"SECOND")
    client.twrite(ooo, 0, b"FIRST---")
    ooo_back = client.tread(ooo, 0, 64)
    ck.check(ooo_back == b"FIRST---SECOND",
             "CASE26 out-of-order positional writes reassemble = ok",
             f"got {ooo_back!r}")
    client.clunk(ooo)

    # Case 28 -- write past the old EOF and read it back through the SAME
    # O_RDWR fid, with no reopen: the view-staleness path end to end.
    tail = b"PAST-THE-OLD-END"
    tail_at = len(base) + 4100
    client.twrite(wfid, tail_at, tail)
    tail_back = client.tread(wfid, tail_at, 64)
    ck.check(tail_back == tail,
             "CASE28 write past EOF read back through the same fid = ok",
             f"got {tail_back!r}")
    client.clunk(wfid)

    # Case 27 -- writes through fids that cannot take them.
    ro_fid = client.walk_to(root, ["adv-write.bin"])
    client.lopen(ro_fid, O_RDONLY)
    ck.expect_error(EBADF, "CASE27 Twrite on an O_RDONLY fid = EBADF",
                    lambda: client.twrite(ro_fid, 0, b"nope"))
    client.clunk(ro_fid)
    d_fid = client.walk_to(root, ["sub"])
    client.lopen(d_fid, O_RDONLY)
    ck.expect_error(EISDIR, "CASE27 Twrite on a directory fid = EISDIR",
                    lambda: client.twrite(d_fid, 0, b"nope"))
    client.clunk(d_fid)
    never = client.walk_to(root, ["adv-write.bin"])
    ck.expect_error(EBADF, "CASE27 Twrite on a never-opened fid = EBADF",
                    lambda: client.twrite(never, 0, b"nope"))
    client.lopen(never, O_WRONLY)
    ck.expect_error(EBADF, "CASE27 Tread on an O_WRONLY fid = EBADF",
                    lambda: client.tread(never, 0, 4))
    client.clunk(never)

    # Case 30 -- O_TRUNC must leave no tail of the old content.
    trunc = create_file(client, root, "adv-trunc.bin", pattern(4096, seed=9))
    client.clunk(trunc)
    reopened = client.walk_to(root, ["adv-trunc.bin"])
    client.lopen(reopened, O_WRONLY | O_TRUNC)
    client.twrite(reopened, 0, b"short")
    client.clunk(reopened)
    check = client.walk_to(root, ["adv-trunc.bin"])
    client.lopen(check, O_RDONLY)
    trunc_back = client.read_all(check, limit=8192)
    trunc_size = client.getattr(check).size
    ck.check(trunc_back == b"short",
             "CASE30 O_TRUNC left no tail of the old content = ok",
             f"got {trunc_back!r}")
    ck.check(trunc_size == 5, "CASE30 the truncated file is 5 bytes",
             f"size={trunc_size}")
    client.clunk(check)

    # Case 37 -- Tfsync on each kind of fid. A write fid flushes; a reader and
    # a directory answer Rfsync without touching anything.
    fs_w = client.walk_to(root, ["adv-write.bin"])
    client.lopen(fs_w, O_RDWR)
    ck.expect_ok("CASE37 Tfsync on a write fid = Rfsync",
                 lambda: client.fsync(fs_w))
    client.clunk(fs_w)
    fs_r = client.walk_to(root, ["adv-write.bin"])
    client.lopen(fs_r, O_RDONLY)
    ck.expect_ok("CASE37 Tfsync on a read fid = Rfsync",
                 lambda: client.fsync(fs_r))
    fs_d = client.walk_to(root, ["sub"])
    client.lopen(fs_d, O_RDONLY)
    ck.expect_ok("CASE37 Tfsync on a directory fid = Rfsync",
                 lambda: client.fsync(fs_d))
    client.clunk(fs_r)
    client.clunk(fs_d)
    ck.expect_error(EBADF, "CASE37 Tfsync on an unbound fid = EBADF",
                    lambda: client.fsync(6543))
    # A fid that IS bound but was never Tlopen'd. Distinct from the unbound
    # case above: the fid resolves, it just has no open file description
    # behind it -- which is exactly what Tread, Treaddir and Twrite refuse,
    # and what Tfsync used to answer Rfsync for.
    fs_bound = client.walk_to(root, ["adv-write.bin"])
    ck.expect_error(EBADF, "CASE37 Tfsync on a bound but never-opened fid = EBADF",
                    lambda: client.fsync(fs_bound))
    client.clunk(fs_bound)
    conn.close()


def adv_namespace(ck: Checks, host: str, port: int) -> None:
    conn, client, root = session(host, port)

    # Case 22 -- Tlcreate REBINDS dfid to the new file, already open.
    reb, _ = client.walk(root, [])
    qid, iounit = client.lcreate(reb, "adv-rebind.txt", O_RDWR)
    ck.check(qid.type == QTFILE and iounit == 0,
             "CASE22 Tlcreate = qid type 0x00, iounit 0",
             f"qid.type=0x{qid.type:02x} iounit={iounit}")
    wrote = client.twrite(reb, 0, b"rebound")
    reb_back = client.tread(reb, 0, 32)
    ck.check(wrote == 7 and reb_back == b"rebound",
             "CASE22 Twrite on the rebound dfid lands in the new file = ok",
             f"wrote={wrote} read back {reb_back!r}")
    ck.expect_error(ENOTDIR,
                    "CASE22 the rebound fid no longer names the directory = ENOTDIR",
                    lambda: client.treaddir(reb, 0, 512))
    client.clunk(reb)

    # Case 23 -- the create refusals.
    par, _ = client.walk(root, [])
    ck.expect_error(EEXIST, "CASE23 Tlcreate onto an existing name = EEXIST",
                    lambda: client.lcreate(par, "adv-rebind.txt", O_RDWR))
    ck.expect_error(EINVAL, "CASE23 Tlcreate with a slash in the name = EINVAL",
                    lambda: client.lcreate(par, "a/b", O_RDWR))
    ck.expect_error(EINVAL, "CASE23 Tlcreate with a backslash in the name = EINVAL",
                    lambda: client.lcreate(par, "a\\b", O_RDWR))
    ck.expect_error(EINVAL, "CASE23 Tlcreate named .. = EINVAL",
                    lambda: client.lcreate(par, "..", O_RDWR))
    ck.expect_error(EINVAL, "CASE23 Tlcreate named . = EINVAL",
                    lambda: client.lcreate(par, ".", O_RDWR))
    filefid = client.walk_to(root, ["adv-rebind.txt"])
    ck.expect_error(ENOTDIR, "CASE23 Tlcreate with dfid naming a file = ENOTDIR",
                    lambda: client.lcreate(filefid, "x.txt", O_RDWR))
    client.clunk(filefid)

    # Case 31 -- Tmkdir, and its non-idempotence.
    made = client.mkdir(par, "adv-dir")
    ck.check(made.type == QTDIR, "CASE31 Tmkdir = qid type 0x80",
             f"qid.type=0x{made.type:02x}")
    ck.expect_error(EEXIST, "CASE31 Tmkdir onto an existing name = EEXIST",
                    lambda: client.mkdir(par, "adv-dir"))
    listing, _ = client.walk(root, [])
    client.lopen(listing, O_RDONLY)
    seen = dirent_names(client.readdir_all(listing, 4096))
    ck.check("adv-dir" in seen, "CASE31 the new directory is in the listing",
             f"names={seen}")
    client.clunk(listing)

    # Case 32 -- Tremove always clunks, whatever it answers.
    doomed = create_file(client, root, "adv-rm.txt", b"bye")
    client.clunk(doomed)
    rmfid = client.walk_to(root, ["adv-rm.txt"])
    client.remove(rmfid)
    ck.expect_error(EBADF, "CASE32 Tremove clunked the fid = EBADF",
                    lambda: client.getattr(rmfid))
    ck.check(not exists(client, root, ["adv-rm.txt"]),
             "CASE32 the removed file is gone = ok", "it still resolves")
    emptyfid = client.walk_to(root, ["adv-dir"])
    client.remove(emptyfid)
    ck.check(not exists(client, root, ["adv-dir"]),
             "CASE32 Tremove of an empty directory = ok", "it still resolves")
    nonempty = client.walk_to(root, ["sub"])
    ck.expect_error(ENOTEMPTY, "CASE32 Tremove of a non-empty directory = ENOTEMPTY",
                    lambda: client.remove(nonempty))
    ck.expect_error(EBADF, "CASE32 the refused Tremove still clunked the fid = EBADF",
                    lambda: client.getattr(nonempty))
    rootclone, _ = client.walk(root, [])
    ck.expect_error(EPERM, "CASE32 Tremove of the export root = EPERM",
                    lambda: client.remove(rootclone))
    ck.expect_error(EBADF, "CASE32 the refused root Tremove still clunked = EBADF",
                    lambda: client.getattr(rootclone))

    # Case 33 -- remove a file THIS fid holds open for writing. Only the
    # clunk-before-delete ordering makes that work at all.
    openrm = create_file(client, root, "adv-openrm.bin", b"still open")
    client.remove(openrm)
    ck.check(not exists(client, root, ["adv-openrm.bin"]),
             "CASE33 Tremove of a file this fid had open for writing = ok",
             "it still resolves")
    still_alive(ck, client, root, "CASE33 connection alive after the removes")
    conn.close()


def adv_rename(ck: Checks, host: str, port: int) -> None:
    conn, client, root = session(host, port)
    holder, _ = client.walk(root, [])
    client.mkdir(holder, "renA")
    client.mkdir(holder, "renB")
    dirA = client.walk_to(root, ["renA"])
    dirB = client.walk_to(root, ["renB"])
    payload = b"rename me"
    src = create_file(client, dirA, "one.txt", payload)
    client.clunk(src)

    # Case 34 -- same-directory rename: the fid follows the file.
    fid = client.walk_to(dirA, ["one.txt"])
    client.rename(fid, dirA, "two.txt")
    moved = client.getattr(fid)
    ck.check(moved.size == len(payload),
             "CASE34 same-directory Trename = the fid names the new path",
             f"size={moved.size}")
    ck.check(exists(client, dirA, ["two.txt"])
             and not exists(client, dirA, ["one.txt"]),
             "CASE34 the rename moved the name = ok",
             "one.txt still resolves, or two.txt does not")
    check = client.walk_to(dirA, ["two.txt"])
    client.lopen(check, O_RDONLY)
    kept = client.read_all(check, limit=4096)
    ck.check(kept == payload,
             "CASE34 the renamed file kept its content = ok", f"got {kept!r}")
    client.clunk(check)

    # ...onto itself is a no-op success, not an EEXIST against its own name.
    client.rename(fid, dirA, "two.txt")
    self_size = client.getattr(fid).size
    ck.check(self_size == len(payload),
             "CASE34 Trename onto itself = Rrename, no-op",
             f"size={self_size}")
    other = create_file(client, dirA, "three.txt", b"other")
    client.clunk(other)
    ck.expect_error(EEXIST, "CASE34 Trename onto an existing name = EEXIST",
                    lambda: client.rename(fid, dirA, "three.txt"))
    rootclone, _ = client.walk(root, [])
    ck.expect_error(EPERM, "CASE34 Trename of the export root = EPERM",
                    lambda: client.rename(rootclone, dirA, "nope"))
    client.clunk(rootclone)

    # Case 42 -- cross-directory is EXDEV, and nothing moves.
    ck.expect_error(EXDEV, "CASE42 cross-directory Trename = EXDEV",
                    lambda: client.rename(fid, dirB, "two.txt"))
    ck.check(exists(client, dirA, ["two.txt"])
             and not exists(client, dirB, ["two.txt"]),
             "CASE42 the EXDEV rename moved nothing", "the tree changed")
    src_size = client.getattr(fid).size
    ck.check(src_size == len(payload),
             "CASE42 the fid still names the source after EXDEV",
             f"size={src_size}")

    # ...and the copy+unlink fallback every rename(2) caller already has.
    reader = client.walk_to(dirA, ["two.txt"])
    client.lopen(reader, O_RDONLY)
    content = client.read_all(reader, limit=4096)
    client.clunk(reader)
    dest = create_file(client, dirB, "two.txt", content)
    client.clunk(dest)
    client.remove(fid)
    ck.check(exists(client, dirB, ["two.txt"])
             and not exists(client, dirA, ["two.txt"]),
             "CASE42 the client's copy+unlink fallback then works = ok",
             "the fallback left the tree wrong")
    still_alive(ck, client, root, "CASE42 connection alive after the renames")
    conn.close()


def adv_setattr(ck: Checks, host: str, port: int) -> None:
    conn, client, root = session(host, port)
    body = pattern(4096, seed=11)
    fid = create_file(client, root, "adv-attr.bin", body)
    client.clunk(fid)

    # Case 35 -- a SHRINK, seen through a fid that was already open.
    live = client.walk_to(root, ["adv-attr.bin"])
    client.lopen(live, O_RDONLY)
    head = client.tread(live, 0, 16)
    setter = client.walk_to(root, ["adv-attr.bin"])
    client.setattr(setter, SETATTR_SIZE, size=100)
    shrunk = client.read_all(live, limit=8192)
    ck.check(head == body[:16] and shrunk == body[:100],
             "CASE35 an already-open fid sees the shrunk file = ok",
             f"got {len(shrunk)} bytes, wanted 100")
    client.clunk(live)

    # ...a GROW inside the ceiling reads back as zeros.
    client.setattr(setter, SETATTR_SIZE, size=100 + 2048)
    grown = client.walk_to(root, ["adv-attr.bin"])
    client.lopen(grown, O_RDONLY)
    tail = client.tread(grown, 100, 2048)
    ck.check(tail == b"\x00" * 2048,
             "CASE35 the grown region reads back as 2048 zeros",
             f"got {len(tail)} bytes, {tail[:8]!r}")
    client.clunk(grown)

    # Case 41 -- the ceiling itself, from both sides of one byte.
    cur = size_at(client, root, ["adv-attr.bin"])
    ck.expect_error(EFBIG,
                    "CASE41 Tsetattr one byte past MAX_GROW = EFBIG",
                    lambda: client.setattr(setter, SETATTR_SIZE,
                                           size=cur + MAX_GROW + 1))
    ck.expect_error(EFBIG, "CASE41 Tsetattr size=2^64-1 = EFBIG",
                    lambda: client.setattr(setter, SETATTR_SIZE, size=U64_MAX))
    after_efbig = size_at(client, root, ["adv-attr.bin"])
    ck.check(after_efbig == cur, f"CASE41 the refused grows left size={cur}",
             f"size={after_efbig}")

    # Case 35 -- on a directory.
    dfid = client.walk_to(root, ["sub"])
    ck.expect_error(EISDIR, "CASE35 Tsetattr(SIZE) on a directory = EISDIR",
                    lambda: client.setattr(dfid, SETATTR_SIZE, size=0))
    client.clunk(dfid)

    # Case 36 -- what Linux sends constantly: valid=0, and the bits this
    # server has no model for. Both are accepted, and change nothing.
    client.setattr(setter, 0, size=999999)
    client.setattr(setter, SETATTR_MODE | SETATTR_UID | SETATTR_GID
                   | SETATTR_ATIME | SETATTR_MTIME, mode=0o777, size=999999)
    after_noop = size_at(client, root, ["adv-attr.bin"])
    ck.check(after_noop == cur,
             "CASE36 Tsetattr with valid=0 and with no-op bits changed nothing",
             f"size={after_noop}")
    ck.expect_error(EBADF, "CASE36 Tsetattr on an unbound fid = EBADF",
                    lambda: client.setattr(5150, SETATTR_SIZE, size=0))
    client.clunk(setter)
    still_alive(ck, client, root, "CASE36 connection alive after the setattrs")
    conn.close()


# --- several connections at once -------------------------------------------

def adv_multiconn(ck: Checks, host: str, port: int) -> None:
    # Case 29 -- two fids on one file, then two CONNECTIONS on one file. The
    # second is why a reader must notice a write it did not make itself:
    # AxlFileView carries that now (src/fs/axl-file-gen.h), and CASE44
    # pushes the same property out to two SERVERS over one root.
    connA, clientA, rootA = session(host, port)
    seed = b"original-content"
    made = create_file(clientA, rootA, "adv-shared.bin", seed)
    clientA.clunk(made)

    readerA = clientA.walk_to(rootA, ["adv-shared.bin"])
    clientA.lopen(readerA, O_RDONLY)
    first = clientA.tread(readerA, 0, 256)
    ck.check(first == seed,
             "CASE29 reader fid sees the original content", f"got {first!r}")
    writerA = clientA.walk_to(rootA, ["adv-shared.bin"])
    clientA.lopen(writerA, O_WRONLY)
    clientA.twrite(writerA, 0, b"SAME-CONN-WRITE!")
    same = clientA.tread(readerA, 0, 256)
    ck.check(same == b"SAME-CONN-WRITE!",
             "CASE29 the other fid on the same connection sees the write",
             f"got {same!r}")

    connB, clientB, rootB = session(host, port)
    writerB = clientB.walk_to(rootB, ["adv-shared.bin"])
    clientB.lopen(writerB, O_WRONLY)
    clientB.twrite(writerB, 0, b"OTHER-CONN-WRIT!")
    cross = clientA.tread(readerA, 0, 256)
    ck.check(cross == b"OTHER-CONN-WRIT!",
             "CASE29 a reader on connection A sees connection B's write",
             f"got {cross!r}")
    clientB.clunk(writerB)
    clientA.clunk(writerA)
    clientA.clunk(readerA)
    connB.close()
    connA.close()


def adv_rename_crossconn(ck: Checks, host: str, port: int,
                         wronly: bool) -> None:
    """Case 43, as far as this firmware allows it to be driven.

    The case was written expecting the UEFI FAT driver to REFUSE to move a
    node another connection still holds open, so that axl_file_rename fails
    and s9p_fid_restore_open rolls B's handles back. MEASURED on both OVMF
    (x64) and AAVMF (aa64): the driver allows it -- the rename SUCCEEDS with
    A's read handle live. The rollback is therefore reached a different way
    (adv_restore, below), and what this function pins instead is the
    cross-connection consequence: A's fid was marked stale by the rename and
    its next Tread cannot reopen the path it names, so A is told EIO rather
    than served bytes from a file that is no longer there.

    What IS driven here, and is worth pinning either way, is the documented
    Trename contract across two connections: the fid that renamed is left
    BOUND BUT CLOSED (s9p_handle_trename drops its handles and deliberately
    does not reopen them), so the next Tread through it must answer EBADF and
    a re-Tlopen must make it work again."""
    kind = "O_WRONLY" if wronly else "O_RDONLY"
    tag = "w" if wronly else "r"
    name = f"adv-cross-{tag}.bin"
    moved_name = f"adv-cross-{tag}-moved.bin"
    payload = b"restore-me-please"

    connA, clientA, rootA = session(host, port)
    seedfid = create_file(clientA, rootA, name, payload)
    clientA.clunk(seedfid)
    holdA = clientA.walk_to(rootA, [name])
    clientA.lopen(holdA, O_RDONLY)          # A's handle stays live throughout
    a_read = clientA.tread(holdA, 0, 64)
    ck.check(a_read == payload,
             f"CASE43 [{kind}] connection A holds the file open = ok",
             f"A read {a_read!r}")

    connB, clientB, rootB = session(host, port)
    fidB = clientB.walk_to(rootB, [name])
    clientB.lopen(fidB, O_WRONLY if wronly else O_RDONLY)
    parentB, _ = clientB.walk(rootB, [])
    # NOTE, if this line ever goes red: it pins MEASURED firmware behavior,
    # not a rule. Case 43 was written assuming the opposite -- that FAT would
    # refuse this rename -- and a future OVMF/AAVMF that does refuse it would
    # fail here for a GOOD reason: the original premise would be back, and the
    # two-connection route to s9p_fid_restore_open would work after all. In
    # that case restore this function to expecting EIO and check whether
    # adv_restore's substitute (a destination name FAT refuses) is still
    # needed. See "Case 43: the premise was wrong" in the Task 6b report.
    ck.expect_ok(
        f"CASE43 [{kind}] Trename with another connection's handle live = Rrename",
        lambda: clientB.rename(fidB, parentB, moved_name))
    ck.check(exists(clientB, rootB, [moved_name])
             and not exists(clientB, rootB, [name]),
             f"CASE43 [{kind}] the rename moved the file",
             "the tree did not change")

    # The renamed fid is bound but closed -- the documented contract.
    ck.expect_error(EBADF,
                    f"CASE43 [{kind}] the renamed fid is bound but closed = EBADF",
                    lambda: (clientB.twrite(fidB, 0, b"x") if wronly
                             else clientB.tread(fidB, 0, 8)))
    clientB.lopen(fidB, O_WRONLY if wronly else O_RDONLY)
    if wronly:
        wrote = clientB.twrite(fidB, 0, b"AFTER-THE-RENAME")
        ck.check(wrote == 16,
                 f"CASE43 [{kind}] the renamed fid works again after Tlopen",
                 f"wrote {wrote}")
    else:
        b_read = clientB.tread(fidB, 0, 64)
        ck.check(b_read == payload,
                 f"CASE43 [{kind}] the renamed fid works again after Tlopen",
                 f"B read {b_read!r}")

    # A held a handle on the node the whole time, and its fid still names the
    # OLD path. B's rename marked A's view stale across the connection
    # boundary, so A's next Tread tries to reopen a name that is gone: the
    # honest answer is EIO, not bytes cached from before the move.
    ck.expect_error(EIO,
                    f"CASE43 [{kind}] connection A is told EIO once its file moved",
                    lambda: clientA.tread(holdA, 0, 64))
    clientB.clunk(fidB)
    still_alive(ck, clientB, rootB,
                f"CASE43 [{kind}] both connections alive after the rename")
    clientA.clunk(holdA)
    connA.close()
    connB.close()


# A destination name the UEFI FAT driver refuses outright. s9p_comp_is_safe
# passes it (it bars only the two separators and NUL), s9p_resolve_child
# builds it, the destination does not exist so the EEXIST check does not fire,
# and the parent matches so it is not EXDEV -- so axl_file_rename is reached
# and FAILS, which is the ONE thing s9p_fid_restore_open exists to recover
# from. MEASURED: every one of * ? < | " : behaves this way on both OVMF and
# AAVMF, and the fid stays usable afterwards, which is the rollback running.
FAT_ILLEGAL_NAME = "illegal*name.bin"


def adv_restore(ck: Checks, host: str, port: int, flavour: str) -> None:
    """Case 43's actual target: s9p_fid_restore_open, the rollback that puts a
    fid's handles back when a rename fails after they were let go.

    The case list expected to reach it by having a SECOND connection hold the
    file open; this firmware permits that rename (see adv_rename_crossconn),
    so the failure is provoked with a destination name the backing store
    refuses instead. Driven in all FOUR shapes the rollback can be asked to
    reconstruct -- read view, write stream, both, and a directory iterator --
    because it picks between them from three captured booleans and nothing
    else ever executes that choice."""
    name = f"adv-restore-{flavour}.bin"
    payload = b"restore-me-please"
    conn, client, root = session(host, port)

    parent, _ = client.walk(root, [])
    if flavour == "dir":
        name = "adv-restore-dir"
        client.mkdir(parent, name)
        seeded = create_file(client, client.walk_to(root, [name]),
                             "inside.txt", b"in a directory")
        client.clunk(seeded)
        fid = client.walk_to(root, [name])
        client.lopen(fid, O_RDONLY)
    else:
        made = create_file(client, root, name, payload)
        client.clunk(made)
        fid = client.walk_to(root, [name])
        client.lopen(fid, {"rd": O_RDONLY, "wr": O_WRONLY,
                           "rdwr": O_RDWR}[flavour])

    ck.expect_error(
        EIO,
        f"CASE43 [{flavour}] Trename the backing store refuses = EIO",
        lambda: client.rename(fid, parent, FAT_ILLEGAL_NAME))
    ck.check(exists(client, root, [name])
             and not exists(client, root, [FAT_ILLEGAL_NAME]),
             f"CASE43 [{flavour}] the failed rename left the tree alone",
             "the tree changed")

    # THE assertion: the fid still works with no re-Tlopen. Without the
    # rollback its handles were dropped for a move that never happened, and
    # every one of these would answer EBADF.
    if flavour == "dir":
        entries = [e.name for e in client.treaddir(fid, 0, 4096)]
        ck.check("inside.txt" in entries and entries.count(".") == 1,
                 f"CASE43 [{flavour}] the rolled-back directory fid still lists",
                 f"entries={entries}")
    if flavour in ("rd", "rdwr"):
        got = client.tread(fid, 0, 64)
        ck.check(got == payload,
                 f"CASE43 [{flavour}] the rolled-back fid still reads",
                 f"got {got!r}")
    if flavour in ("wr", "rdwr"):
        wrote = client.twrite(fid, 0, b"AFTER-THE-FAILED")
        checker = client.walk_to(root, [name])
        client.lopen(checker, O_RDONLY)
        landed = client.tread(checker, 0, 16)
        client.clunk(checker)
        ck.check(wrote == 16 and landed == b"AFTER-THE-FAILED",
                 f"CASE43 [{flavour}] the rolled-back fid still writes",
                 f"wrote={wrote} landed={landed!r}")

    client.clunk(fid)
    still_alive(ck, client, root,
                f"CASE43 [{flavour}] connection alive after the rollback")
    conn.close()


def adv_reap_cycle(ck: Checks, host: str, port: int) -> None:
    """Case 20's wire-observable half. Each cycle leaves a read view, a write
    stream and a directory iterator open and then drops the socket without a
    clunk, so the server reaps a LIVE connection. The pool is eight slots
    wide: if a reap failed to release its slot, cycle nine would be refused
    outright -- which is exactly what this asserts by continuing to serve."""
    cycles = 12
    served = 0
    for _ in range(cycles):
        conn, client, root = session(host, port)
        dirfid, _ = client.walk(root, [])
        client.lopen(dirfid, O_RDONLY)
        ffid = client.walk_to(root, ["hello.txt"])
        client.lopen(ffid, O_RDONLY)
        if client.tread(ffid, 0, 64) == EXPECTED_HELLO:
            served += 1
        conn.close()                       # no clunk, no Tversion -- just gone
        time.sleep(0.15)
    ck.check(served == cycles,
             f"CASE20 {cycles} abrupt disconnects, every one served = ok",
             f"{served} of {cycles}")
    conn, client, root = session(host, port)
    still_alive(ck, client, root, "CASE20 connection slots were all recycled")
    conn.close()


def adv_page_cache(ck: Checks, host: str, port: int) -> None:
    """Case 21's wire-observable half -- more open file fids than the ONE
    shared page cache has frames (16), read round-robin. A per-fid pool would
    grow without bound; a shared one has to evict and refill correctly, and a
    wrong eviction shows up as one fid serving another's bytes."""
    conn, client, root = session(host, port)
    holder, _ = client.walk(root, [])
    client.mkdir(holder, "pagecache")
    pdir = client.walk_to(root, ["pagecache"])

    count = 20
    want: dict[int, bytes] = {}
    fids: list[int] = []
    for i in range(count):
        data = pattern(200, seed=i + 40)
        fid = create_file(client, pdir, f"pc{i:02d}.bin", data)
        want[fid] = data
        fids.append(fid)

    good = 0
    for _ in range(3):
        for fid in fids:
            if client.tread(fid, 0, 4096) == want[fid]:
                good += 1
    ck.check(good == count * 3,
             f"CASE21 {count} open file fids over 16 frames, 3 passes = ok",
             f"{good} of {count * 3} reads matched")
    for fid in fids:
        client.clunk(fid)
    still_alive(ck, client, root, "CASE21 connection alive after the round-robin")
    conn.close()


def adv_big_msize(ck: Checks, host: str, port: int) -> None:
    """Cases 11 and 24 at BOTH msizes: the 8 KiB default and a negotiated
    128 KiB, which straddle the server's 64 KiB page size."""
    conn = P9Conn(host, port)
    client = P9Client(conn, msize=SERVER_MAX_MSIZE)
    got, ver = client.version()
    ck.check(got == SERVER_MAX_MSIZE and ver == NINEP_VERSION,
             "CASE11 negotiated msize=131072 = ok",
             f"msize={got} version={ver!r}")
    root, _ = client.attach()

    body = pattern(200_000, seed=13)
    fid = create_file(client, root, "adv-200k.bin", body)
    written = client.getattr(fid).size
    ck.check(written == len(body),
             f"CASE24 multi-chunk write at msize=131072 = {len(body)} bytes",
             f"size={written}")
    client.clunk(fid)

    reader = client.walk_to(root, ["adv-200k.bin"])
    client.lopen(reader, O_RDONLY)
    got_big = client.read_all(reader, limit=len(body) + 8192)
    ck.check(got_big == body, "CASE11 read at msize=131072 byte-exact = ok",
             f"got {len(got_big)} of {len(body)} bytes")
    client.clunk(reader)
    conn.close()

    conn2, client2, root2 = session(host, port)
    small = client2.walk_to(root2, ["adv-200k.bin"])
    client2.lopen(small, O_RDONLY)
    got_small = client2.read_all(small, limit=len(body) + 8192)
    ck.check(got_small == body,
             "CASE11 the same file read at msize=8192 byte-exact = ok",
             f"got {len(got_small)} of {len(body)} bytes")
    client2.clunk(small)
    conn2.close()


# --- the read-only export --------------------------------------------------

def adv_readonly(ck: Checks, host: str, ro_port: int) -> None:
    """Cases 38 and 39 -- the dispatch gate. Every mutating type must answer
    EROFS, and must do so BEFORE any filesystem call: before the name is
    resolved, before the fid is even looked up."""
    conn, client, root = session(host, ro_port)

    dfid, _ = client.walk(root, [])
    ck.expect_error(EROFS, "CASE38 Tlcreate on a read-only export = EROFS",
                    lambda: client.lcreate(dfid, "nope.txt", O_RDWR))
    ck.expect_error(EROFS, "CASE38 Tmkdir on a read-only export = EROFS",
                    lambda: client.mkdir(dfid, "nopedir"))
    ck.expect_error(EROFS, "CASE38 Trename on a read-only export = EROFS",
                    lambda: client.rename(dfid, dfid, "nope"))
    ck.expect_error(EROFS, "CASE38 Tsetattr on a read-only export = EROFS",
                    lambda: client.setattr(dfid, SETATTR_SIZE, size=0))
    hfid = client.walk_to(root, ["hello.txt"])
    ck.expect_error(EROFS, "CASE38 Twrite on a read-only export = EROFS",
                    lambda: client.twrite(hfid, 0, b"nope"))
    ck.expect_error(EROFS, "CASE38 Tremove on a read-only export = EROFS",
                    lambda: client.remove(hfid))
    # Tremove ALWAYS clunks its fid -- so a fid that still resolves proves the
    # gate answered before the handler ran at all.
    ck.check(client.getattr(hfid).size == len(EXPECTED_HELLO),
             "CASE38 the refused Tremove did NOT clunk the fid = ok",
             "the fid is gone, so the handler ran")

    # Case 39 -- EROFS even when the request names something that does not
    # exist, or a fid that was never bound: the gate leaks nothing.
    ck.expect_error(EROFS, "CASE39 Tmkdir under an UNBOUND dfid = EROFS",
                    lambda: client.mkdir(9999, "still-erofs"))
    ck.expect_error(EROFS, "CASE39 Tlcreate with an invalid name = EROFS",
                    lambda: client.lcreate(dfid, "a/b", O_RDWR))
    ck.expect_error(EROFS, "CASE39 Twrite on an unbound fid = EROFS",
                    lambda: client.twrite(9999, 0, b"nope"))

    # ...and everything non-mutating still works on the same server.
    client.lopen(hfid, O_RDONLY)
    ck.check(client.read_all(hfid, limit=4096) == EXPECTED_HELLO,
             "CASE38 Tlopen(O_RDONLY) + Tread still work read-only = ok",
             "the read came back wrong")
    # Tfsync is on case 38's must-still-succeed list, and it is deliberately
    # absent from s9p_type_is_mutating -- so it has to be GRADED, not merely
    # called. (This fid is O_RDONLY: no write stream, so it takes the "nothing
    # of ours behind this fid" path rather than the flush.)
    ck.expect_ok("CASE38 Tfsync still works read-only = Rfsync",
                 lambda: client.fsync(hfid))
    ck.expect_error(EROFS, "CASE38 Tlopen(O_WRONLY) on a read-only export = EROFS",
                    lambda: client.lopen(hfid, O_WRONLY))
    listing, _ = client.walk(root, [])
    client.lopen(listing, O_RDONLY)
    names = dirent_names(client.readdir_all(listing, 4096))
    ck.check("hello.txt" in names and "sub" in names,
             "CASE38 Twalk + Treaddir still work read-only = ok",
             f"names={names}")
    client.clunk(listing)
    got, ver = client.version()
    ck.check(got == DEFAULT_MSIZE and ver == NINEP_VERSION,
             "CASE38 Tversion still works read-only = ok",
             f"msize={got} version={ver!r}")
    root2, qid2 = client.attach(fid=700)
    ck.check(qid2.type == QTDIR, "CASE38 Tattach still works read-only = ok",
             f"qid.type=0x{qid2.type:02x}")
    still_alive(ck, client, root2, "CASE38 read-only export still serving")
    conn.close()


# --- two servers, one root ------------------------------------------------

def adv_cross_export(ck: Checks, host: str, port: int, ro_port: int) -> None:
    """Case 44 -- coherence across SERVER instances.

    The selftest publishes two Axl9pServer objects over the SAME root: the
    read-write export on `port` and the read-only one on `ro_port`. They
    share nothing -- separate connection tables, separate fid tables,
    separate page caches. A reader holding a fid open on the ro export while
    a writer changes the file through the rw export is therefore the exact
    shape the server's own staleness marking could never see, because that
    marking only ever scanned one server's connections.

    Linux v9fs over trans=tcp defaults to cache=none, so every client read
    reaches the server: stale bytes here are stale bytes in userspace."""
    rw_conn, rw, rw_root = session(host, port)
    ro_conn, ro, ro_root = session(host, ro_port)
    name = "crossexport.txt"
    first = b"first-generation"
    second = b"SECOND-GENERATION-and-longer"

    # Create and fill the file through the READ-WRITE export.
    wfid, _ = rw.walk(rw_root, [])
    rw.lcreate(wfid, name, O_RDWR)
    rw.twrite(wfid, 0, first)
    rw.fsync(wfid)

    # Open it on the READ-ONLY export and read it, so that server's view has
    # the file's length cached AND its first page resident. Everything after
    # this has to defeat both.
    rfid = ro.walk_to(ro_root, [name])
    ro.lopen(rfid, O_RDONLY)
    ck.check(ro.read_all(rfid, limit=4096) == first,
             "CASE44 the ro export reads the file the rw export wrote",
             "the initial cross-export read came back wrong")

    # Rewrite it through the OTHER server, longer than before so a stale
    # length truncates and a stale page mismatches.
    rw.twrite(wfid, 0, second)
    rw.fsync(wfid)

    got = ro.read_all(rfid, limit=4096)
    ck.check(got == second,
             "CASE44 the ALREADY-OPEN ro fid sees the rw export's write",
             f"got {got!r}, wanted {second!r}")

    # And the same fid keeps tracking, rather than catching up exactly once.
    third = b"third"
    rw.twrite(wfid, 0, third)
    rw.setattr(wfid, SETATTR_SIZE, size=len(third))
    rw.fsync(wfid)
    got3 = ro.read_all(rfid, limit=4096)
    ck.check(got3 == third,
             "CASE44 the ro fid tracks a SECOND write, and the shrink with it",
             f"got {got3!r}, wanted {third!r}")

    # Removed through the rw export while the ro fid is still open: the ro
    # side must report an error, and the SAME error on the next try.
    rw.remove(wfid)
    ck.expect_error(EIO, "CASE44 a read through the ro fid after the remove = EIO",
                    lambda: ro.read_all(rfid, limit=4096))
    ck.expect_error(EIO, "CASE44 the NEXT read gives the same EIO, not EBADF",
                    lambda: ro.read_all(rfid, limit=4096))

    ro.clunk(rfid)
    still_alive(ck, ro, ro_root, "CASE44 the ro export is still serving")
    still_alive(ck, rw, rw_root, "CASE44 the rw export is still serving")
    ro_conn.close()
    rw_conn.close()


def run_group(ck: Checks, name: str, fn: Callable[[], None]) -> None:
    """Run one adversarial group. An unexpected exception aborts that group
    only -- its assertions go MISSING and the run continues, so ONE QEMU boot
    still reports on every other group."""
    try:
        fn()
    except (P9Error, P9Protocol, P9Closed, OSError, struct.error) as exc:
        ck.abort(name, f"{type(exc).__name__}: {exc}")


def run_adversarial(ck: Checks, host: str, port: int, ro_port: int) -> None:
    run_group(ck, "framing", lambda: adv_framing(ck, host, port))
    run_group(ck, "pipelined", lambda: adv_pipelined(ck, host, port))
    run_group(ck, "burst", lambda: adv_burst(ck, host, port))
    run_group(ck, "walk", lambda: adv_walk(ck, host, port))
    run_group(ck, "deep-walk", lambda: adv_deep_walk(ck, host, port))
    run_group(ck, "fid-table", lambda: adv_fid_table(ck, host, port))
    run_group(ck, "read", lambda: adv_read(ck, host, port))
    run_group(ck, "readdir-paging", lambda: adv_readdir_paging(ck, host, port))
    run_group(ck, "write", lambda: adv_write(ck, host, port))
    run_group(ck, "namespace", lambda: adv_namespace(ck, host, port))
    run_group(ck, "rename", lambda: adv_rename(ck, host, port))
    run_group(ck, "setattr", lambda: adv_setattr(ck, host, port))
    run_group(ck, "multiconn", lambda: adv_multiconn(ck, host, port))
    run_group(ck, "rename-crossconn-rd",
              lambda: adv_rename_crossconn(ck, host, port, wronly=False))
    run_group(ck, "rename-crossconn-wr",
              lambda: adv_rename_crossconn(ck, host, port, wronly=True))
    def restore_group(flavour: str) -> Callable[[], None]:
        return lambda: adv_restore(ck, host, port, flavour)

    for flavour in ("rd", "wr", "rdwr", "dir"):
        run_group(ck, f"restore-{flavour}", restore_group(flavour))
    run_group(ck, "reap-cycle", lambda: adv_reap_cycle(ck, host, port))
    run_group(ck, "page-cache", lambda: adv_page_cache(ck, host, port))
    run_group(ck, "big-msize", lambda: adv_big_msize(ck, host, port))
    run_group(ck, "read-only", lambda: adv_readonly(ck, host, ro_port))
    run_group(ck, "cross-export",
              lambda: adv_cross_export(ck, host, port, ro_port))


def run_linger(host: str, port: int, seconds: float) -> int:
    """Hold a connection open with a read view, a write stream and a
    directory iterator live on its fids, so the server's free path has a
    LIVE connection to reap rather than an already-drained one.

    Deliberately touches only sub/inner.txt (never the root entries the
    assertion suite counts), so the two connections cannot perturb each
    other's listings."""
    conn = P9Conn(host, port)
    client = P9Client(conn)
    client.version()
    root, _ = client.attach()

    dfid, _ = client.walk(root, [])
    client.lopen(dfid, O_RDONLY)          # AxlDir iterator

    ffid = client.walk_to(root, ["sub", "inner.txt"])
    client.lopen(ffid, O_RDWR)            # AxlFileView + write AxlStream

    print("LINGER READY", flush=True)
    time.sleep(seconds)
    print("LINGER DONE", flush=True)
    conn.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="9P2000.L client for the "
                                                 "AXL 9P server test")
    parser.add_argument("host", help="server address")
    parser.add_argument("port", type=int, help="server TCP port")
    parser.add_argument("--linger", type=float, default=0.0,
                        help="hold a connection open for N seconds instead "
                             "of running the assertion suite")
    parser.add_argument("--ro-port", type=int, default=0,
                        help="port of the guest's READ-ONLY export. Required "
                             "for the ADVERSARIAL suite, all of which is "
                             "skipped without it -- not just the read-only "
                             "gate cases. The functional suite runs either "
                             "way; the harness's derived CLIENT OK count "
                             "turns a silent skip red.")
    args = parser.parse_args()
    host = str(args.host)
    port = int(args.port)
    linger = float(args.linger)
    ro_port = int(args.ro_port)

    if linger > 0.0:
        try:
            return run_linger(host, port, linger)
        except (P9Error, P9Protocol, P9Closed, OSError, struct.error) as exc:
            print(f"CLIENT ERROR {type(exc).__name__}: {exc}", flush=True)
            return 2

    ck = Checks()
    try:
        run_checks(ck, host, port)
    except (P9Protocol, P9Closed, OSError, struct.error) as exc:
        print(f"CLIENT ERROR {type(exc).__name__}: {exc}", flush=True)
        return 2
    except P9Error as exc:
        print(f"CLIENT ERROR unexpected Rlerror errno={exc.ecode}", flush=True)
        return 2

    # The adversarial groups catch their own exceptions (run_group), so one
    # hostile case that goes wrong cannot cost the run every later group's
    # evidence -- a single QEMU boot still reports on all of them.
    if ro_port > 0:
        run_adversarial(ck, host, port, ro_port)

    if ck.failed == 0:
        print(f"CLIENT OK {ck.passed} (xfail {ck.xfailed})", flush=True)
        return 0
    print(f"CLIENT FAIL {ck.failed} (xfail {ck.xfailed})", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
