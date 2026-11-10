#!/usr/bin/env python3
"""A deliberately misbehaving ssh-agent, for AxlSsh P2 auth-phase tests.

WHY THIS EXISTS: ssh-hostile.py speaks no crypto by design, so it cannot get
past key exchange and cannot reach the authentication phase at all. Reaching
it hostilely would otherwise mean implementing curve25519 and
chacha20-poly1305 in Python. The agent protocol needs NONE of that -- it is
length-prefixed messages over a Unix socket -- and a real ssh(1) performs the
whole handshake on our behalf. Two behaviours, two attacks:

  forge  -- advertise an AUTHORIZED public key and answer the sign request
            with 64 bytes of garbage. The server said PK_OK to this key, so a
            signature that does not verify is forgery and must end the
            connection.
  stall  -- advertise the key and never answer the sign request. ssh sits
            there with a completed key exchange and no authentication, which
            is exactly the post-kex idle connection the pre-auth deadline now
            has to cover.

Stdlib only: no paramiko, no cryptography, nothing to install on a CI box.

Protocol: RFC 4251 framing (uint32 length || payload) over AF_UNIX.
  SSH_AGENTC_REQUEST_IDENTITIES = 11 -> SSH_AGENT_IDENTITIES_ANSWER = 12
  SSH_AGENTC_SIGN_REQUEST       = 13 -> SSH_AGENT_SIGN_RESPONSE     = 14

LIFETIME, not a connection count. An earlier draft took --accepts N and ran
`for _ in range(N): srv.accept()`, which has two failure modes and no correct
value of N: fewer agent connections than N and the process blocks in accept()
forever holding the test open; more than N and it unbinds the socket under a
client that is still using it. A deadline serves every connection ssh opens
and terminates on its own either way.
"""
from __future__ import annotations

import argparse
import base64
import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path

SSH_AGENTC_REQUEST_IDENTITIES = 11
SSH_AGENT_IDENTITIES_ANSWER = 12
SSH_AGENTC_SIGN_REQUEST = 13
SSH_AGENT_SIGN_RESPONSE = 14
SSH_AGENT_FAILURE = 5


def sshstr(b: bytes) -> bytes:
    """RFC 4251 string: uint32 length followed by the bytes."""
    return struct.pack(">I", len(b)) + b


def read_exactly(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("agent client closed")
        buf += chunk
    return buf


def read_message(sock: socket.socket) -> bytes:
    (length,) = struct.unpack(">I", read_exactly(sock, 4))
    if length == 0 or length > 65536:
        raise ValueError(f"implausible agent message length {length}")
    return read_exactly(sock, length)


def send_message(sock: socket.socket, payload: bytes) -> None:
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def load_pub_blob(path: Path) -> bytes:
    """Read an OpenSSH .pub line and return its raw key blob."""
    fields = path.read_text(encoding="utf-8").split()
    if len(fields) < 2 or fields[0] != "ssh-ed25519":
        raise ValueError(f"{path} is not an ssh-ed25519 public key line")
    return base64.b64decode(fields[1])


def serve_one(conn: socket.socket, blob: bytes, mode: str,
              deadline: float) -> None:
    with conn:
        while True:
            try:
                msg = read_message(conn)
            except (EOFError, OSError, ValueError):
                return
            kind = msg[0]
            if kind == SSH_AGENTC_REQUEST_IDENTITIES:
                body = (bytes([SSH_AGENT_IDENTITIES_ANSWER])
                        + struct.pack(">I", 1)
                        + sshstr(blob)
                        + sshstr(b"axl-fake-agent"))
                send_message(conn, body)
                print("agent: advertised 1 identity", flush=True)
            elif kind == SSH_AGENTC_SIGN_REQUEST:
                if mode == "stall":
                    # Never answer. ssh blocks reading this socket, holding a
                    # post-kex, unauthenticated connection to the server open
                    # -- which is the whole point of this mode. OpenSSH's
                    # agent RPC is synchronous, so ssh will not even notice
                    # the server dropping it until we let go.
                    print("agent: stalling the sign request", flush=True)
                    while time.monotonic() < deadline:
                        time.sleep(0.25)
                    return
                # 64 bytes that are structurally a signature and
                # cryptographically nothing.
                forged = sshstr(b"ssh-ed25519") + sshstr(b"\xa5" * 64)
                send_message(conn,
                             bytes([SSH_AGENT_SIGN_RESPONSE]) + sshstr(forged))
                print("agent: returned a forged signature", flush=True)
            else:
                # Everything else -- notably the session-bind@openssh.com
                # extension ssh(1) 8.9+ sends first -- is refused. A refused
                # extension is a supported outcome for a real agent too, so
                # ssh carries on to the identity list.
                send_message(conn, bytes([SSH_AGENT_FAILURE]))


def main() -> int:
    ap = argparse.ArgumentParser(description="a misbehaving ssh-agent")
    ap.add_argument("--pub", required=True, type=Path,
                    help="an ssh-ed25519 .pub file to advertise")
    ap.add_argument("--socket", required=True, type=Path,
                    help="AF_UNIX path to bind")
    ap.add_argument("--mode", choices=("forge", "stall"), required=True)
    ap.add_argument("--lifetime", type=float, default=120.0,
                    help="seconds to serve before exiting")
    args = ap.parse_args()

    blob = load_pub_blob(args.pub)
    sock_path = args.socket.resolve()
    sock_path.unlink(missing_ok=True)
    # AF_UNIX paths are capped at ~108 bytes and the caller's temp directory
    # is not ours to shorten -- a test scratch path blew that limit on the
    # first run of this file. Binding a RELATIVE name from the socket's own
    # directory makes the cap depend on the basename alone. Nothing else in
    # this process uses a relative path (the .pub is already read), so the
    # chdir is safe.
    os.chdir(sock_path.parent)

    deadline = time.monotonic() + args.lifetime
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        srv.bind(sock_path.name)
        srv.listen(4)
        srv.settimeout(0.5)
        # Tell the caller the socket is ready, so the test never races the
        # bind. A test that sleeps instead of synchronising here is a flake
        # waiting to happen.
        print("agent: ready", flush=True)
        while time.monotonic() < deadline:
            try:
                conn, _addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(
                target=serve_one,
                args=(conn, blob, args.mode, deadline),
                daemon=True,
            ).start()
        print("agent: lifetime expired", flush=True)
    finally:
        srv.close()
        sock_path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
