#!/usr/bin/env python3
"""Minimal 9P2000.L host server for AXL client integration tests.

NOT production — serves a tiny in-memory tree over TCP so the AxlTestNet
`9p-client` driver mode can prove the AxlNet 9P client speaks the wire
protocol (version / attach / walk / lopen / read / readdir / clunk /
lcreate / write / mkdir / remove / rename / getattr). getattr backs
axl_9p_mount's open path (dir-vs-file comes from Rgetattr's st_mode), so
9p-mount-selftest also depends on this server.
Single-threaded, one client at a time, tag echoed, no auth. The guest
reaches this via QEMU user-net at 10.0.2.2:<port>.

Fixed tree (mutable at runtime via the write ops):
    /hello.txt        -> b"hello from 9p\n"
    /dir/a.txt        -> b"alpha\n"
    /dir/b.txt        -> b"bravo\n"
    /readonly.txt     -> b"read-only content\n" (server-side read-only: a
                          write-capable Tlopen on it gets Rlerror EACCES,
                          regardless of the flags the caller asked for)

A Trename whose target parent differs from the source's is answered
Rlerror(EXDEV), matching AXL's own 9P server on that one axis (an in-server
move would be an unbounded synchronous whole-file copy on its single event
loop). The fixture must not be more permissive than the server the client
meets in production, or the client's EXDEV copy-then-unlink fallback goes
untested. It is NOT a full behavioral match otherwise: unlike the real
server, it overwrites an existing name at the destination instead of
refusing it with EEXIST, and it answers EXDEV rather than EPERM for a
rename of the export root itself (the root has no parent to compare
against). The destination-overwrite difference is load-bearing, not an
oversight: the client REFUSES a taken destination on its EXDEV fallback
path, and a fixture that refused it too would let that guard be deleted
without any test noticing.

Every fid-taking handler resolves through `fid_node` and answers
Rlerror(EBADF) on an unknown or stale fid. Uniformly -- a handler that
still indexed `NODES[fids[fid]]` directly would raise KeyError and unwind
the whole connection loop, so a client probing one stale fid would see a
dead socket rather than the protocol error it asked for.
"""
from __future__ import annotations

import socket
import struct
import sys
from dataclasses import dataclass, field

# 9P2000.L message types (subset).
TVERSION, RVERSION = 100, 101
RLERROR = 7
TATTACH, RATTACH = 104, 105
TWALK, RWALK = 110, 111
TLOPEN, RLOPEN = 12, 13
TREAD, RREAD = 116, 117
TREADDIR, RREADDIR = 40, 41
TCLUNK, RCLUNK = 120, 121
TLCREATE, RLCREATE = 14, 15
TRENAME, RRENAME = 20, 21
TMKDIR, RMKDIR = 72, 73
TWRITE, RWRITE = 118, 119
TREMOVE, RREMOVE = 122, 123
TGETATTR, RGETATTR = 24, 25

O_WRONLY = 0x1
O_RDWR = 0x2
O_TRUNC = 0x200

EXDEV = 18     # Rlerror errno for a cross-directory rename (Linux EXDEV)
EBADF = 9      # Rlerror errno for an unknown/stale fid

QTDIR = 0x80   # qid.type for a directory
QTFILE = 0x00
DT_DIR = 4     # dirent d_type
DT_REG = 8


@dataclass
class Node:
    path: int
    is_dir: bool
    name: str
    content: bytes = b""
    children: dict[str, int] = field(default_factory=dict)


NODES: dict[int, Node] = {
    1: Node(1, True, "/", children={"hello.txt": 2, "dir": 3, "readonly.txt": 6}),
    2: Node(2, False, "hello.txt", content=b"hello from 9p\n"),
    3: Node(3, True, "dir", children={"a.txt": 4, "b.txt": 5}),
    4: Node(4, False, "a.txt", content=b"alpha\n"),
    5: Node(5, False, "b.txt", content=b"bravo\n"),
    6: Node(6, False, "readonly.txt", content=b"read-only content\n"),
}
ROOT = 1
_next_path = iter(range(7, 1 << 30))   # fixed tree above uses paths 1..6


def alloc_path() -> int:
    """Allocate a fresh NODES key for a newly created file or directory."""
    return next(_next_path)


def _unlink(path: int) -> None:
    """Remove the child entry pointing at `path` from its parent's children map."""
    for node in NODES.values():
        for name, cpath in list(node.children.items()):
            if cpath == path:
                del node.children[name]
                return


def _parent_path_of(path: int) -> int | None:
    """Return the NODES key of the directory holding `path`, or None if unparented."""
    for node in NODES.values():
        if path in node.children.values():
            return node.path
    return None


def fid_node(fids: dict[int, int], fid: int) -> Node | None:
    """Resolve a fid to its Node, or None when the fid is unknown or stale.

    EVERY handler goes through this. A bare `NODES[fids[fid]]` raises
    KeyError, which unwinds this connection's whole serve loop -- caught only
    by main()'s outer except, which just drops the socket. The client then
    sees a dead connection instead of a clean Rlerror(EBADF), and the test
    that hit it reports a truncated log rather than the protocol error it
    was actually testing.
    """
    path = fids.get(fid)
    if path is None:
        return None
    return NODES.get(path)


def qid(node: Node) -> bytes:
    """qid = type[1] version[4] path[8]."""
    return struct.pack("<BIQ", QTDIR if node.is_dir else QTFILE, 0, node.path)


def put_str(s: str) -> bytes:
    b = s.encode()
    return struct.pack("<H", len(b)) + b


def get_str(body: bytes, off: int) -> tuple[str, int]:
    """Read a 9P string (len[2] + utf8 bytes) at `off`; return (value, new_off)."""
    nl = struct.unpack_from("<H", body, off)[0]
    return body[off + 2:off + 2 + nl].decode(), off + 2 + nl


def recv_exact(sock: socket.socket, n: int) -> bytes | None:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def reply(sock: socket.socket, rtype: int, tag: int, body: bytes) -> None:
    msg = struct.pack("<IBH", 7 + len(body), rtype, tag) + body
    sock.sendall(msg)


def rlerror(sock: socket.socket, tag: int, ecode: int) -> None:
    reply(sock, RLERROR, tag, struct.pack("<I", ecode))


def serve_one(sock: socket.socket) -> None:
    fids: dict[int, int] = {}   # fid -> node.path
    while True:
        hdr = recv_exact(sock, 7)
        if hdr is None:
            return
        size, mtype, tag = struct.unpack("<IBH", hdr)
        body = recv_exact(sock, size - 7) if size > 7 else b""
        if body is None:
            return

        if mtype == TVERSION:
            msize = struct.unpack_from("<I", body, 0)[0]
            reply(sock, RVERSION, tag,
                  struct.pack("<I", min(msize, 8192)) + put_str("9P2000.L"))
        elif mtype == TATTACH:
            fid = struct.unpack_from("<I", body, 0)[0]
            fids[fid] = ROOT
            reply(sock, RATTACH, tag, qid(NODES[ROOT]))
        elif mtype == TWALK:
            # 9P2000.L: a walk that resolves SOME-but-not-all components is
            # not an error -- reply Rwalk with the partial qid list and bind
            # newfid to the last-walked node. Only a fully-failed walk (zero
            # components resolved, when at least one was requested) is
            # Rlerror. nwname == 0 is the "clone fid" case (0 qids, always
            # succeeds).
            fid, newfid, nwname = struct.unpack_from("<IIH", body, 0)
            off = 10
            node = fid_node(fids, fid)
            if node is None:
                rlerror(sock, tag, EBADF)
                continue
            qids = b""
            nwqid = 0
            for _ in range(nwname):
                nl = struct.unpack_from("<H", body, off)[0]
                name = body[off + 2:off + 2 + nl].decode()
                off += 2 + nl
                child = node.children.get(name)
                if child is None:
                    break
                node = NODES[child]
                qids += qid(node)
                nwqid += 1
            if nwqid == 0 and nwname > 0:
                rlerror(sock, tag, 2)   # ENOENT -- nothing resolved
            else:
                fids[newfid] = node.path   # partial or full walk both bind newfid
                reply(sock, RWALK, tag, struct.pack("<H", nwqid) + qids)
        elif mtype == TLOPEN:
            fid, flags = struct.unpack_from("<II", body, 0)
            node = fid_node(fids, fid)
            if node is None:
                rlerror(sock, tag, EBADF)
                continue
            if node.name == "readonly.txt" and (flags & 0x3) in (O_WRONLY, O_RDWR):
                rlerror(sock, tag, 13)  # EACCES -- readonly.txt rejects write opens
                continue
            if flags & O_TRUNC and not node.is_dir:
                node.content = b""
            reply(sock, RLOPEN, tag, qid(node) + struct.pack("<I", 0))
        elif mtype == TREAD:
            fid, offset, count = struct.unpack_from("<IQI", body, 0)
            node = fid_node(fids, fid)
            if node is None:
                rlerror(sock, tag, EBADF)
                continue
            data = node.content[offset:offset + count]
            reply(sock, RREAD, tag, struct.pack("<I", len(data)) + data)
        elif mtype == TREADDIR:
            fid, offset, count = struct.unpack_from("<IQI", body, 0)
            node = fid_node(fids, fid)
            if node is None:
                rlerror(sock, tag, EBADF)
                continue
            data = b""
            if offset == 0:            # whole listing on the first read
                for i, (name, cpath) in enumerate(node.children.items(), start=1):
                    child = NODES[cpath]
                    dt = DT_DIR if child.is_dir else DT_REG
                    data += qid(child) + struct.pack("<QB", i, dt) + put_str(name)
            reply(sock, RREADDIR, tag, struct.pack("<I", len(data)) + data)
        elif mtype == TGETATTR:            # fid[4] request_mask[8]
            fid = struct.unpack_from("<I", body, 0)[0]
            node = fid_node(fids, fid)
            if node is None:
                rlerror(sock, tag, EBADF)
                continue
            valid = 0x000007ff              # P9_GETATTR_BASIC
            # A real server reports a nonzero directory size from stat()
            # (4096 is the common ext4/xfs directory-block size) -- not 0.
            # axl_9p_list's directory-skip is what is supposed to turn this
            # into the "0 for directories" AxlFsEntry.size contract; a fake
            # server that already answers 0 here would let that skip regress
            # unnoticed, since the wrong output and the right output would
            # look identical downstream.
            size = 4096 if node.is_dir else len(node.content)
            # S_IFDIR / S_IFREG bit matters: axl_9p_mount's mount_open()
            # branches dir-vs-file on this field from Rgetattr.
            st_mode = 0o040755 if node.is_dir else 0o100644
            blocks = (size + 511) // 512
            resp = struct.pack("<Q", valid) + qid(node) + struct.pack(
                "<III", st_mode, 0, 0)          # mode, uid, gid
            resp += struct.pack("<Q", 1)        # nlink
            resp += struct.pack("<Q", 0)        # rdev
            resp += struct.pack("<Q", size)     # size
            resp += struct.pack("<Q", 512)      # blksize
            resp += struct.pack("<Q", blocks)   # blocks
            resp += struct.pack("<QQ", 0, 0)    # atime_sec, atime_nsec
            resp += struct.pack("<QQ", 0, 0)    # mtime_sec, mtime_nsec
            resp += struct.pack("<QQ", 0, 0)    # ctime_sec, ctime_nsec
            resp += struct.pack("<QQ", 0, 0)    # btime_sec, btime_nsec
            resp += struct.pack("<QQ", 0, 0)    # gen, data_version
            reply(sock, RGETATTR, tag, resp)
        elif mtype == TCLUNK:
            fid = struct.unpack_from("<I", body, 0)[0]
            fids.pop(fid, None)
            reply(sock, RCLUNK, tag, b"")
        elif mtype == TLCREATE:            # fid[4] name[s] flags[4] mode[4] gid[4]
            fid = struct.unpack_from("<I", body, 0)[0]
            name, off = get_str(body, 4)
            _flags, _mode, _gid = struct.unpack_from("<III", body, off)
            parent = fid_node(fids, fid)
            if parent is None:
                rlerror(sock, tag, EBADF)
                continue
            if name in parent.children:
                rlerror(sock, tag, 17)      # EEXIST
            else:
                path = alloc_path()
                NODES[path] = Node(path, False, name)
                parent.children[name] = path
                fids[fid] = path            # fid now refers to the new file
                reply(sock, RLCREATE, tag, qid(NODES[path]) + struct.pack("<I", 0))
        elif mtype == TWRITE:               # fid[4] offset[8] count[4] data[count]
            fid, offset, count = struct.unpack_from("<IQI", body, 0)
            data = body[16:16 + count]
            node = fid_node(fids, fid)
            if node is None:
                rlerror(sock, tag, EBADF)
                continue
            content = node.content
            if offset > len(content):
                content += b"\x00" * (offset - len(content))
            node.content = content[:offset] + data + content[offset + len(data):]
            reply(sock, RWRITE, tag, struct.pack("<I", len(data)))
        elif mtype == TMKDIR:               # dfid[4] name[s] mode[4] gid[4]
            dfid = struct.unpack_from("<I", body, 0)[0]
            name, off = get_str(body, 4)
            _mode, _gid = struct.unpack_from("<II", body, off)
            parent = fid_node(fids, dfid)
            if parent is None:
                rlerror(sock, tag, EBADF)
                continue
            if name in parent.children:
                rlerror(sock, tag, 17)      # EEXIST
            else:
                path = alloc_path()
                NODES[path] = Node(path, True, name)
                parent.children[name] = path
                reply(sock, RMKDIR, tag, qid(NODES[path]))
        elif mtype == TREMOVE:              # fid[4]  (removes + clunks the fid)
            fid = struct.unpack_from("<I", body, 0)[0]
            path = fids.pop(fid, None)
            if path is None:
                rlerror(sock, tag, EBADF)
                continue
            _unlink(path)
            NODES.pop(path, None)
            reply(sock, RREMOVE, tag, b"")
        elif mtype == TRENAME:              # fid[4] dfid[4] name[s]
            fid, dfid = struct.unpack_from("<II", body, 0)
            name, _ = get_str(body, 8)
            src = fid_node(fids, fid)
            dst = fid_node(fids, dfid)
            if src is None or dst is None:
                rlerror(sock, tag, EBADF)
                continue
            path, newparent = src.path, dst.path
            # A cross-directory rename is refused with EXDEV, exactly as AXL's
            # own 9P server does (an in-server move would be an unbounded
            # synchronous whole-file copy on its single event loop). Without
            # this the fixture is more permissive than any server the client
            # meets in production, and the client's EXDEV fallback would never
            # be exercised.
            oldparent = _parent_path_of(path)
            if oldparent is None or oldparent != newparent:
                rlerror(sock, tag, EXDEV)
                continue
            _unlink(path)
            NODES[newparent].children[name] = path
            NODES[path].name = name
            reply(sock, RRENAME, tag, b"")
        else:
            rlerror(sock, tag, 38)     # ENOSYS


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: p9-server.py <port>", file=sys.stderr)
        return 2
    port = int(sys.argv[1])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", port))
    srv.listen(4)
    print(f"p9-server listening on :{port}", flush=True)
    while True:
        conn, _ = srv.accept()
        try:
            serve_one(conn)
        except (ConnectionError, struct.error, KeyError):
            pass
        finally:
            conn.close()


if __name__ == "__main__":
    sys.exit(main())
