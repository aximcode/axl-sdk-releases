#!/usr/bin/env python3
"""A scripted SSH client for AxlSsh tests: curve25519-sha256, ssh-ed25519,
chacha20-poly1305@openssh.com, password auth -- the one algorithm per slot the
server negotiates -- in pure stdlib Python.

WHY THIS EXISTS: ssh(1) cannot be told to send an arbitrary message after
login, and paramiko cannot connect at all (it has no
chacha20-poly1305@openssh.com, the server's only cipher). Before P4a the only
real-client route to SSH_MSG_UNIMPLEMENTED was `ssh -R`'s GLOBAL_REQUEST,
which P4a answers properly. P4c's channel-abuse tests (data past the window,
unknown channel ids) and P4d's rekey edge cases need the same thing.

Stdlib only, like ssh-fake-agent.py: nothing to install on a CI box. The
primitives are slow and NOT constant-time; this is a test peer, not a client.

Usage:
  ssh-peer.py selftest
  ssh-peer.py login HOST PORT --user U --password P [--script S] [--hold SEC]
                    [--rcvbuf BYTES] [--preauth S] [--tamper hash|sid]
                    [--keepalive answer|ignore]
  ssh-peer.py kexwait HOST PORT --hold SEC [--stage kex|reply|kexinit|ecdh]
  ssh-peer.py bench HOST PORT --user U --password P --n N
  ssh-peer.py x11-open-hex [--sender N]
  ssh-peer.py open-hex --type T [--sender N]
  ssh-peer.py chan-open-hex [--sender N] [--window W] [--maxpkt P]
  ssh-peer.py chan-req-hex --recipient R --name NAME [--want 0|1] [--rest HEX]
  ssh-peer.py chan-data-hex --recipient R --text TEXT [--stream out|err]
  ssh-peer.py chan-adjust-hex --recipient R --bytes N

--preauth takes the same actions as --script and runs them after key
exchange and BEFORE the SERVICE_REQUEST -- a connection that is encrypted
but not yet authenticated, such as one sending KEXINIT (`send:20:HEX`) there.

--script is a comma-separated list of actions, run in order after login:
  send:T[:HEX]        one packet of type T         `peer: sent T seq=S`
  global:NAME:0|1     GLOBAL_REQUEST, want_reply   `peer: sent 80 seq=S`
  expect:T            the next message is type T   `peer: got T`
                      (UNIMPLEMENTED: `peer: got 3 seq=S`, the seq it names)
  burst:N:T[:HEX]     N packets of type T, sealed  `peer: sent burst N x T
                      before the first is written,  seqs=A..B`
                      then ONE sendall -- so they
                      reach the server back to back
  expectn:N:T         the next N messages are all  `peer: got N x T`
                      type T
  flood:N:T[:HEX]     as burst, but WRITTEN BY A   `peer: writing N x T
                      BACKGROUND THREAD while the   seqs=A..B`, and at the
                      script goes on -- so the      end of the script
                      peer can read while that      `peer: wrote N x T`
                      write is blocked
  hold:SEC            sleep, reading nothing
  idle:SEC            read for SEC seconds         (no line of its own; a
                      (keepalives answered, see    close is `peer: closed`)
                      KEEPALIVE), keeping anything
                      else for the actions after
  batch:T:HEX+T:HEX.. as burst, but of DIFFERENT  `peer: sent batch T+T..
                      messages, in the order given  seqs=A..B`
  pour:BYTES:PKT      BYTES of 'x' as CHANNEL_DATA  `peer: poured BYTES in K
                      to the confirmed channel, in  packets seqs=A..B`
                      packets of at most PKT,
                      sealed first and written in
                      one sendall, reading nothing
  expectdata:N        read CHANNEL_DATA and         `peer: data out|err
                      EXTENDED_DATA until N bytes   "<escaped>"`, once per
                      arrived, skipping             message
                      WINDOW_ADJUST
  expectsum:N         the same, for a run too long  `peer: sum out=A err=B
                      to print                      sha256=<hex>` (the bytes
                                                    in arrival order)
  expectsum:N:K:T     ...and K messages of type T   and then `peer: got K
                      among the data, in any        x T`
                      interleaving -- replies to a
                      flood that race the output
  kexinit             our rekey KEXINIT (REKEY);    `peer: sent 20 seq=S`
                      behind a `flood` still being
                      written it is queued, so it
                      reaches the server right
                      after the flood's last packet
  kexinit:guess       ...with a WRONG guess and     and `peer: sent 30 seq=S`
                      first_kex_packet_follows,
                      then the guessed packet
  rekey               a whole key re-exchange,      `peer: rekeyed, started
                      ours sent first unless one    by us|the server`
                      is already running
  rekey:reply         the same, stopping once the   `peer: rekey reply
                      server's REPLY is verified,   verified`
                      before our NEWKEYS
  rekey:server        wait for the SERVER to start  `peer: rekeyed, started
                      one, and complete it          by the server`
  rekey:server:SEC    the same, waiting SEC between
                      our KEXINIT and our
                      KEX_ECDH_INIT, reading
                      nothing -- the server's
                      exchange held open (only for
                      a KEXINIT read inside it)
  shutdown            half-close: shutdown(SHUT_WR) `peer: shut down
                      -- reads still work; a flood  writing`, after the
                      still being written is cut    flood's line if one
                      short and anything queued     was running: `peer:
                      behind it dropped (a flood    wrote N x T` or
                      already written is joined     `peer: flood cut short`
                      first, its queue sent)
  await:PATH          wait, reading nothing, until  (none; `peer: timeout`
                      PATH exists -- the test's     after 30 s)
                      signal that what it watches
                      has happened
  chatter:SEC:PATH    as await, but sending an      `peer: chattered N`
                      IGNORE every SEC meanwhile    (N IGNOREs sent)
                      -- a peer that stops READING
                      but is still heard
  pick:N              make OUR channel N the `@`    (none)
                      channel (see CHANNELS)
--report-disconnect names a DISCONNECT the server sends, as `peer: disconnect
CODE "TEXT"`, then reads on until the server closes and prints `peer: N
packet(s) after the disconnect` before the run ends as `peer: closed`.
Without it a DISCONNECT ends the run as `peer: closed` alone, as an EOF does.

IGNORE, DEBUG and USERAUTH_BANNER are skipped wherever a message is read, and
a message of the wrong type prints `peer: unexpected T` (exit 3). Nothing can
be written after `shutdown`; a later write ends the run as `peer: closed`.

KEEPALIVE. After login, a keepalive@openssh.com the server sends is
answered inside every read, as ssh(1) answers it (clientloop.c): a
GLOBAL_REQUEST with REQUEST_FAILURE (82), a CHANNEL_REQUEST with
CHANNEL_FAILURE (100) to the server's channel number -- unless the peer has
already sent CHANNEL_CLOSE on it, or no reply is wanted, when nothing goes
back. It prints `peer: answered keepalive 80` or `peer: answered keepalive
98` (T being the REQUEST's type), or `peer: unanswered keepalive T` when
nothing goes back. An answer due inside the peer's own key re-exchange is
held until its NEWKEYS, as ssh_packet_send2 holds it. `--keepalive ignore`
reads and drops each request instead, printing `peer: ignored keepalive 80`
or `peer: ignored keepalive 98` -- a peer that has stopped answering. The
request is never handed to an action, so `expect` cannot see one.

`expect:T` prints `peer: got T`, except where the message carries what a
test needs to see:
  91 OPEN_CONFIRMATION   `peer: got 91 sender=S window=W maxpkt=P`
  92 OPEN_FAILURE        `peer: got 92 reason=R`, R the RFC 4254 5.1 code
  93 WINDOW_ADJUST       `peer: got 93 bytes=N`
  98 CHANNEL_REQUEST     `peer: got 98 name=NAME want=0|1 rest=<hex>`
  3  UNIMPLEMENTED       `peer: got 3 seq=S`, the seq it names

CHANNELS. The recipient of every message after an open is the SERVER's
channel number, which only its OPEN_CONFIRMATION says. So the peer remembers
the sender of the last OPEN_CONFIRMATION it read, and a body written as
`@HEX` -- in send, burst, flood and batch -- has that number substituted into
its first four bytes when it is sealed. `pick:N` points `@` back at an
earlier channel instead: the server's number for the one the peer opened as
sender N. The chan-*-hex helpers print exactly
that shape for `--recipient @` (`@` and the body with a zero recipient), so
no test hard-codes the server's numbering. An `@` body before any
confirmation prints `peer: no channel confirmed` (exit 3); `pour` needs one
too.

THE PEER POLICES THE SERVER'S OUTPUT. A `session` open it SENDS records the
window and maximum packet it advertised, a WINDOW_ADJUST it sends adds to
that window, and every CHANNEL_DATA or EXTENDED_DATA it reads for that
channel is checked against both: `peer: data over maxpkt N` or
`peer: data over window N` (exit 3), N the message's data length. That is
the only place a server that sends past a window or a maximum packet can be
seen, since a real client either drops the connection or buffers silently.
`expectdata`/`expectsum` print data as `"<escaped>"`: a backslash, a double
quote, LF, CR and TAB as `\\\\`, `\\"`, `\\n`, `\\r`, `\\t`; other
printable ASCII as itself; every other byte as `\\x` and two hex digits.

REKEY. After login the peer re-exchanges keys in exactly the wire shape
OpenSSH's client uses (kex.c, kexgen.c, packet.c): its KEXINIT is the first
exchange's proposal with the strict pseudo-name filtered out, a fresh
cookie and first_kex_packet_follows 0; once both KEXINITs are out it sends
KEX_ECDH_INIT with a fresh curve25519 key; it verifies the server's
ssh-ed25519 signature over THIS exchange's hash (both new KEXINITs) --
`peer: bad host signature`, exit 6, as at login -- and derives the keys
with the ORIGINAL session id; it answers with NEWKEYS, sealed under the old
key, switches its send key, and switches its receive key at the server's
NEWKEYS; under strict KEX each direction's sequence number restarts at 0
at its NEWKEYS. The engine runs inside every read after login, so a
KEXINIT the server sends while an action reads data is answered as ssh(1)
would answer it -- ours and KEX_ECDH_INIT at once -- and the action goes
on; `peer: rekeyed, started by the server` is printed where the exchange
completes, between that action's lines. Messages that are not the
exchange's, arriving while a `rekey` waits, are kept in order for the
actions after it. A NEWKEYS the script sends itself (`send:21`, or last in
a `batch`) switches the send key the same way, so a packet sealed before
it in one write travels under the OLD key -- legal, and what a pipelining
client does. A second `kexinit` in one exchange is sent but not hashed,
as the server must not hash it either. The engine's own packets are sent
from inside a read, so while a `flood` write is still running they are
queued behind it, like a keepalive answer (see `flood` below) -- an engine
that waited for a flood blocked against a server that has stopped reading
would wait for ever, since only the read it is answering from drains the
server.

THE PEER POLICES THE SERVER'S EXCHANGE TOO. From the server's KEXINIT to
its NEWKEYS, sshd sends only transport messages 1-49 but 5, 6 and 7
(ssh_packet_type_is_kex) and holds the rest until its NEWKEYS
(ssh_packet_send2). Anything else arriving there ends the run as
`peer: got T inside the server's exchange` (exit 3). ssh(1) cannot see
this: it processes channel messages mid-exchange, so a server that sent
held output under the old key would look healthy to it.

`--tamper` is a CONTROL for that engine, used once each to prove a check
can fail: `hash` hashes the FIRST exchange's KEXINITs in place of this
one's (the signature must then fail: exit 6); `sid` derives with this
exchange's H in place of the session id (the keys then disagree and the
connection dies).

WHY `flood` NEEDS A THREAD. A server that stops reading while its output is
backed up (AxlSsh's output backpressure) and a peer that writes everything
before it reads anything wait on each other for ever: the peer's write
blocks once TCP's window closes, and the window opens only when the peer
reads. `burst` is that peer -- fine while its whole run fits the window.
`flood` writes from a thread, so the script goes on to `hold` and `expectn`
and reads while the write is still blocked, as every real client (one
select loop over both directions) does. The thread is joined before any
later action writes, so packets still leave in the order they were sealed,
and at the end of the script, which is where its line is printed and where a
failed write is reported (`peer: closed`, exit 4; `peer: timeout`, exit 7).
A packet the peer must send from INSIDE a read -- a keepalive answer, the
rekey engine's reply -- does NOT join it: it is sealed at once and queued,
and the WRITER THREAD writes the queue the moment the flood's last byte is
out. Joining there was a deadlock of the peer against itself: the flood can
be blocked only because the server stopped reading until its output drains,
and the read that would drain it is the one waiting for the join. Leaving
the queue for the next join was a second one: a KEX_ECDH_INIT queued
mid-flood, a flood that then finishes, and a script whose next action is a
read -- the server waits for the INIT, the peer for the REPLY. One lock
covers the append and the writer's last look at the queue, so a packet
sealed after the writer closed it is written directly instead.

`--rcvbuf BYTES` sets the host socket's SO_RCVBUF before it connects,
which also stops Linux autotuning it. lib/ssh-idle-hostile-cases.sh's
pause-mid-rekey pins it (MR_RCVBUF, 256 KiB) so a stream of known length
is sure to outlast what the host can buffer. It is NOT needed to see a server's
output back up:
under QEMU's user-mode network the guest's output is ACKed by QEMU whatever
the peer's buffer, and backs up in QEMU's host socket. A small value is
harmful: at 4096 the Linux sender on the host loopback can lock into one
2028-byte segment per 200 ms probe, a ~24 s stall that is the harness's,
not the server's (measured 2026-10-09). Kept for experiments.

`bench` prints one line, `bench: n=N secs=S frame=F` -- see bench().
`open-hex --type T` prints the same for a CHANNEL_OPEN of type T carrying
NO type-specific data -- the well-formed shape of an open whose type the
server does not know -- for a test that needs a refused open and nothing
else about it (see _plain_open).
`x11-open-hex` prints, as hex, the body after the type byte of the
CHANNEL_OPEN "x11" that bench() sends (sender N, default 1), for a test's
`burst:N:90:HEX` -- so no test hand-types the encoding.
`chan-open-hex` prints the body of a CHANNEL_OPEN "session" (sender N,
default 0; window W, default 2097152; max packet P, default 32768).
`chan-req-hex` prints a CHANNEL_REQUEST body: recipient, the request NAME,
want_reply (default 0) and REST, the request-specific bytes, as hex.
`chan-data-hex` prints a CHANNEL_DATA body carrying TEXT, or with
`--stream err` a CHANNEL_EXTENDED_DATA body (SSH_EXTENDED_DATA_STDERR) --
send it as type 95. `chan-adjust-hex` prints a WINDOW_ADJUST body. R is a
channel number or `@` (see CHANNELS).

Every output line is an interface later tests match EXACTLY. Exit codes:
  0 ok, 1 selftest mismatch, 2 auth failed, 3 unexpected message,
  4 closed by the server, 5 refused before identification,
  6 bad host signature, 7 timed out waiting for the server,
  64 usage error (a malformed command line or --script; printed to stderr as
     `ssh-peer: usage: ...`, and raised BEFORE connecting, so a typo in a
     test never costs the server a connection or the source a penalty).
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import hmac
import io
import os
import select
import socket
import struct
import sys
import threading
import time
from collections import deque
from typing import Callable, NamedTuple, NoReturn

M32 = 0xFFFFFFFF

# --- X25519 (RFC 7748 section 5) -------------------------------------------
_P = 2**255 - 19
_A24 = 121665


def x25519(k: bytes, u: bytes) -> bytes:
    kb = bytearray(k)
    kb[0] &= 248
    kb[31] &= 127
    kb[31] |= 64
    scalar = int.from_bytes(kb, "little")
    ub = bytearray(u)
    ub[31] &= 127
    x1 = int.from_bytes(ub, "little")
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in reversed(range(255)):
        kt = (scalar >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a = (x2 + z2) % _P
        aa = a * a % _P
        b = (x2 - z2) % _P
        bb = b * b % _P
        e = (aa - bb) % _P
        c = (x3 + z3) % _P
        d = (x3 - z3) % _P
        da = d * a % _P
        cb = c * b % _P
        x3 = (da + cb) ** 2 % _P
        z3 = x1 * (da - cb) ** 2 % _P
        x2 = aa * bb % _P
        z2 = e * (aa + _A24 * e) % _P
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, _P - 2, _P) % _P).to_bytes(32, "little")


# --- ChaCha20, original 64/64 layout, and Poly1305 --------------------------
# OpenSSH uses the ORIGINAL ChaCha20 state: [const][key][counter64][nonce64],
# not RFC 8439's [counter32][nonce96]. src/net/axl-ssh-crypt.c ssh_nonce()
# documents how the server maps the same thing onto mbedTLS.
def _rotl(v: int, c: int) -> int:
    return ((v << c) & M32) | (v >> (32 - c))


def _qr(s: list[int], a: int, b: int, c: int, d: int) -> None:
    s[a] = (s[a] + s[b]) & M32
    s[d] = _rotl(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & M32
    s[b] = _rotl(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & M32
    s[d] = _rotl(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & M32
    s[b] = _rotl(s[b] ^ s[c], 7)


def chacha_block(key: bytes, counter: int, nonce8: bytes) -> bytes:
    st = (list(struct.unpack("<4I", b"expand 32-byte k"))
          + list(struct.unpack("<8I", key))
          + [counter & M32, (counter >> 32) & M32]
          + list(struct.unpack("<2I", nonce8)))
    w = st[:]
    for _ in range(10):
        _qr(w, 0, 4, 8, 12)
        _qr(w, 1, 5, 9, 13)
        _qr(w, 2, 6, 10, 14)
        _qr(w, 3, 7, 11, 15)
        _qr(w, 0, 5, 10, 15)
        _qr(w, 1, 6, 11, 12)
        _qr(w, 2, 7, 8, 13)
        _qr(w, 3, 4, 9, 14)
    return struct.pack("<16I", *[(w[i] + st[i]) & M32 for i in range(16)])


def chacha20(key: bytes, nonce8: bytes, counter: int, data: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 64):
        ks = chacha_block(key, counter + i // 64, nonce8)
        out += bytes(x ^ y for x, y in zip(data[i:i + 64], ks))
    return bytes(out)


def poly1305(key32: bytes, msg: bytes) -> bytes:
    r = int.from_bytes(key32[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key32[16:], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        acc = (acc + int.from_bytes(msg[i:i + 16] + b"\x01", "little")) * r % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


# --- Ed25519 verify (RFC 8032 section 5.1.7) --------------------------------
Point = tuple[int, int, int, int]   # extended coordinates (X, Y, Z, T)
_Q = 2**255 - 19
_D = -121665 * pow(121666, _Q - 2, _Q) % _Q
_I = pow(2, (_Q - 1) // 4, _Q)
_L = 2**252 + 27742317777372353535851937790883648493


def _xrecover(y: int, sign: int) -> int:
    xx = (y * y - 1) * pow(_D * y * y + 1, _Q - 2, _Q) % _Q
    x = pow(xx, (_Q + 3) // 8, _Q)
    if (x * x - xx) % _Q:
        x = x * _I % _Q
    if (x * x - xx) % _Q:
        raise ValueError("not a point")
    # RFC 8032 5.1.3 step 4: x = 0 with the sign bit set is not an encoding.
    # Checked HERE, before the negation below turns that 0 into _Q.
    if x == 0 and sign:
        raise ValueError("negative zero")
    if (x & 1) != sign:
        x = _Q - x
    return x


def _decode_pt(b: bytes) -> Point:
    y = int.from_bytes(b, "little") & ((1 << 255) - 1)
    if y >= _Q:
        raise ValueError("y out of range")
    x = _xrecover(y, b[31] >> 7)
    return (x, y, 1, x * y % _Q)


def _pt_add(p: Point, q: Point) -> Point:
    a = (p[1] - p[0]) * (q[1] - q[0]) % _Q
    b = (p[1] + p[0]) * (q[1] + q[0]) % _Q
    c = 2 * p[3] * q[3] * _D % _Q
    d = 2 * p[2] * q[2] % _Q
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _Q, g * h % _Q, f * g % _Q, e * h % _Q)


def _pt_mul(s: int, p: Point) -> Point:
    q: Point = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _pt_add(q, p)
        p = _pt_add(p, p)
        s >>= 1
    return q


def _pt_eq(p: Point, q: Point) -> bool:
    return ((p[0] * q[2] - q[0] * p[2]) % _Q == 0
            and (p[1] * q[2] - q[1] * p[2]) % _Q == 0)


_GY = 4 * pow(5, _Q - 2, _Q) % _Q
_GX = _xrecover(_GY, 0)
_G: Point = (_GX, _GY, 1, _GX * _GY % _Q)


def ed25519_verify(pub: bytes, msg: bytes, sig: bytes) -> bool:
    if len(pub) != 32 or len(sig) != 64:
        return False
    try:
        a = _decode_pt(pub)
        r = _decode_pt(sig[:32])
    except ValueError:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _L:
        return False
    h = int.from_bytes(hashlib.sha512(sig[:32] + pub + msg).digest(),
                       "little") % _L
    return _pt_eq(_pt_mul(s, _G), _pt_add(r, _pt_mul(h, a)))


# --- Self-test vectors, copied from the RFC text -----------------------------
# RFC 7748 section 5.2, the first X25519 vector.
X_K = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"
X_U = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"
X_OUT = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"
# RFC 8439 section 2.3.2, the serialized block (key 00..1f, block count 1,
# nonce 00:00:00:09:00:00:00:4a:00:00:00:00).
C_OUT = ("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
         "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e")
# RFC 8439 section 2.5.2.
P_KEY = "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"
P_MSG = b"Cryptographic Forum Research Group"
P_TAG = "a8061dc1305136c6c22b8baf0c0127a9"
# RFC 8032 section 7.1, TEST 1 (empty message) and TEST 2 (message 0x72).
E1_PUB = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
E1_SIG = ("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
E2_PUB = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"
E2_MSG = "72"
E2_SIG = ("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
          "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")


def _chan_track_ok() -> bool:
    """ChanTrack's policing, with no server: data within the advertised
    window and maximum packet passes, and THE CONTROLS -- one byte over
    either -- are refused, so a check that never fires cannot pass here."""
    def data(ours: int, n: int) -> bytes:
        return (bytes([MSG_CHANNEL_DATA]) + struct.pack(">I", ours)
                + sshstr(b"x" * n))

    t = ChanTrack()
    t.note_out(bytes([MSG_CHANNEL_OPEN]) + sshstr(b"session")
               + struct.pack(">III", 5, 10, 4))
    t.note_in(bytes([MSG_CHANNEL_OPEN_CONFIRMATION])
              + struct.pack(">IIII", 5, 7, 0, 32768))
    ok = t.confirmed == 7 and t.resolve(bytes(6), True)[:4] == b"\0\0\0\7"
    ok = ok and not _refused(lambda: t.note_in(data(5, 4)))
    ok = ok and _refused(lambda: t.note_in(data(5, 5)))       # maxpkt
    ok = ok and not _refused(lambda: t.note_in(data(5, 4)))   # window 2
    ok = ok and _refused(lambda: t.note_in(data(5, 3)))       # window
    t.note_out(bytes([MSG_CHANNEL_WINDOW_ADJUST])
               + struct.pack(">II", 7, 1))
    ok = ok and not _refused(lambda: t.note_in(data(5, 3)))   # 2 + 1
    ok = ok and _refused(lambda: t.note_in(data(5, 1)))
    return ok and _refused(lambda: ChanTrack().resolve(bytes(4), True))


def _behind_flood_ok() -> bool:
    """A packet sent from inside a read while a `flood` write is blocked is
    queued behind it rather than joining it (Conn.send), and leaves right
    after it, in seal order. THE CONTROL: an ordinary send in the same
    state does block -- the join that deadlocked the peer -- so a queue
    that never engaged cannot pass here. Over loopback, with a "server"
    that reads nothing until the end."""
    lst = socket.create_server(("127.0.0.1", 0))
    c = Conn("127.0.0.1", lst.getsockname()[1])
    srv, _ = lst.accept()
    lst.close()
    flood = b"f" * (32 << 20)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        c.write_bg(flood, "flood done")
        time.sleep(0.2)
        blocked = c.flood_running()
        queued = threading.Thread(
            target=lambda: c.send(bytes([MSG_REQUEST_FAILURE]),
                                  from_read=True), daemon=True)
        queued.start()
        queued.join(2.0)
        ok = blocked and not queued.is_alive() and len(c.behind_flood) == 1
        ctl = threading.Thread(target=lambda: c.send(bytes([MSG_IGNORE])),
                               daemon=True)
        ctl.start()
        ctl.join(1.0)
        ok = ok and ctl.is_alive()
        got = bytearray()
        srv.settimeout(10.0)
        while len(got) < len(flood) + 32:
            chunk = srv.recv(1 << 20)
            if not chunk:
                break
            got += chunk
        ctl.join(5.0)
    srv.close()
    c.sock.close()
    # Unencrypted, each one-byte payload is a 16-byte packet: length, pad
    # length, type -- the queued answer first, then the control's send.
    return (ok and not ctl.is_alive() and len(got) == len(flood) + 32
            and got[len(flood) + 5] == MSG_REQUEST_FAILURE
            and got[len(flood) + 21] == MSG_IGNORE
            and out.getvalue() == "flood done\n")


def _flood_drains_queue_ok() -> bool:
    """The flood FINISHES while the main thread is blocked reading: the
    packet queued behind it must leave then, with no further send or join
    -- the interleaving where the server reads the flood, then waits for
    our queued KEX_ECDH_INIT while we wait for its reply. Over loopback,
    the "server" drains everything after a pause and answers one byte
    only once the queued packet has arrived; the peer meanwhile sits in a
    blocking recv(), so nothing but the writer thread can send it. Then a
    LATE in-read send, after the writer closed its queue, must fall
    through to a direct write rather than into a queue no one drains."""
    lst = socket.create_server(("127.0.0.1", 0))
    c = Conn("127.0.0.1", lst.getsockname()[1])
    srv, _ = lst.accept()
    lst.close()
    flood = b"f" * (32 << 20)
    want = len(flood) + 16
    got = bytearray()

    def serve() -> None:
        srv.settimeout(5.0)
        try:
            while len(got) < want:
                chunk = srv.recv(1 << 20)
                if not chunk:
                    return
                got.extend(chunk)
            srv.sendall(b"x")
            while len(got) < want + 16:
                chunk = srv.recv(1 << 20)
                if not chunk:
                    return
                got.extend(chunk)
        except OSError:
            return

    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        c.write_bg(flood, "flood done")
        time.sleep(0.2)
        blocked = c.flood_running()
        c.send(bytes([MSG_REQUEST_FAILURE]), from_read=True)
        server = threading.Thread(target=serve, daemon=True)
        server.start()
        c.sock.settimeout(8.0)
        try:
            answered = c.sock.recv(1) == b"x"
        except OSError:
            answered = False
        c.join_writer()
        c.send(bytes([MSG_IGNORE]), from_read=True)
        server.join(5.0)
    srv.close()
    c.sock.close()
    return (blocked and answered and len(got) == want + 16
            and got[len(flood) + 5] == MSG_REQUEST_FAILURE
            and got[want + 5] == MSG_IGNORE
            and out.getvalue() == "flood done\n")


def selftest() -> int:
    checks: list[tuple[str, bool]] = []
    checks.append(("x25519", x25519(bytes.fromhex(X_K), bytes.fromhex(X_U))
                   == bytes.fromhex(X_OUT)))
    # The RFC's 96-bit nonce 00000009 0000004a 00000000 fills state words
    # 13-15 behind a 32-bit counter in word 12. In the 64/64 layout words
    # 12-13 are the counter, so word 13's 0x09000000 is the counter's HIGH
    # half and words 14-15 (0x4a000000, 0) are the 8-byte nonce.
    checks.append(("chacha", chacha_block(bytes(range(32)),
                                          1 | (0x09000000 << 32),
                                          bytes.fromhex("0000004a00000000"))
                   == bytes.fromhex(C_OUT)))
    checks.append(("poly1305", poly1305(bytes.fromhex(P_KEY), P_MSG)
                   == bytes.fromhex(P_TAG)))
    checks.append(("ed25519-t1", ed25519_verify(bytes.fromhex(E1_PUB), b"",
                                                bytes.fromhex(E1_SIG))))
    checks.append(("ed25519-t2", ed25519_verify(bytes.fromhex(E2_PUB),
                                                bytes.fromhex(E2_MSG),
                                                bytes.fromhex(E2_SIG))))
    # THE CONTROL: a verifier that says True to everything passes both
    # vectors above. One flipped bit must be refused.
    bad = bytearray(bytes.fromhex(E1_SIG))
    bad[0] ^= 1
    checks.append(("ed25519-control", not ed25519_verify(bytes.fromhex(E1_PUB),
                                                         b"", bytes(bad))))
    # Framing needs no server: the smallest legal length on each path is
    # accepted, and every malformed one is refused as `peer: bad length`
    # rather than surfacing as an IndexError or an empty payload.
    checks.append(("framing", all((
        not _refused(lambda: check_len(8, True)),
        not _refused(lambda: check_len(12, False)),
        _refused(lambda: check_len(0, True)),
        _refused(lambda: check_len(12, True)),
        _refused(lambda: check_len(8, False)),
        _refused(lambda: check_len(PACKET_MAX + 8, True)),
        unpad(bytes([4]) + b"xyz" + bytes(4)) == b"xyz",
        _refused(lambda: unpad(bytes([8]) + bytes(7))),
        _refused(lambda: unpad(bytes([3]) + bytes(7))),
    ))))
    checks.append(("chan-track", _chan_track_ok()))
    checks.append(("behind-flood", _behind_flood_ok()))
    checks.append(("flood-drains-queue", _flood_drains_queue_ok()))
    checks.append(("escape", escape(b'a\\"\n\r\t\x00\x7f~')
                   == 'a\\\\\\"\\n\\r\\t\\x00\\x7f~'))
    for name, ok in checks:
        if not ok:
            print(f"selftest: FAIL {name}")
            return 1
    print("selftest: ok")
    return 0


# --- SSH wire ----------------------------------------------------------------
MSG_DISCONNECT = 1
MSG_IGNORE = 2
MSG_UNIMPLEMENTED = 3
MSG_DEBUG = 4
MSG_SERVICE_REQUEST = 5
MSG_SERVICE_ACCEPT = 6
MSG_KEXINIT = 20
MSG_NEWKEYS = 21
MSG_EXT_INFO = 7
MSG_KEX_ECDH_INIT = 30
MSG_KEX_ECDH_REPLY = 31
MSG_USERAUTH_REQUEST = 50
MSG_USERAUTH_FAILURE = 51
MSG_USERAUTH_SUCCESS = 52
MSG_USERAUTH_BANNER = 53
MSG_GLOBAL_REQUEST = 80
MSG_CHANNEL_OPEN = 90
MSG_CHANNEL_OPEN_CONFIRMATION = 91
MSG_CHANNEL_OPEN_FAILURE = 92
MSG_CHANNEL_WINDOW_ADJUST = 93
MSG_CHANNEL_DATA = 94
MSG_CHANNEL_EXTENDED_DATA = 95
MSG_CHANNEL_REQUEST = 98
MSG_CHANNEL_CLOSE = 97
MSG_REQUEST_FAILURE = 82
MSG_CHANNEL_FAILURE = 100
KEEPALIVE = b"keepalive@openssh.com"
SKIPPED = (MSG_IGNORE, MSG_DEBUG, MSG_USERAUTH_BANNER)
# After login these belong to a key re-exchange, whoever started it: the
# peer's rekey engine consumes them (see REKEY in the module doc).
KEX_MSGS = frozenset([MSG_KEXINIT, MSG_NEWKEYS, *range(30, 50)])
# The strict-KEX pseudo-names (OpenSSH PROTOCOL section 1.10).
STRICT_C = b"kex-strict-c-v00@openssh.com"
STRICT_S = b"kex-strict-s-v00@openssh.com"
KEX_NAME = b"curve25519-sha256"
# The kex name a wrong-guess KEXINIT puts first: a method the server does not
# implement, so a guess for it is wrong whatever the server prefers.
WRONG_GUESS_KEX = b"diffie-hellman-group14-sha256"


def sendable_mid_kex(t: int) -> bool:
    """May a server send message T between its KEXINIT and its NEWKEYS?
    OpenSSH's ssh_packet_type_is_kex (packet.c): transport messages 1-49 but
    SERVICE_REQUEST, SERVICE_ACCEPT and EXT_INFO. ssh_packet_send2 queues
    everything else from its KEXINIT until its NEWKEYS."""
    return 1 <= t <= 49 and t not in (MSG_SERVICE_REQUEST, MSG_SERVICE_ACCEPT,
                                      MSG_EXT_INFO)

V_C = b"SSH-2.0-AxlPeer_1.0"
IO_TIMEOUT_SEC = 30.0


class PeerExit(Exception):
    """Ends the run with one interface line and an exit code."""

    def __init__(self, line: str, code: int) -> None:
        super().__init__(line)
        self.line = line
        self.code = code


class UsageError(Exception):
    """A malformed command line or --script: the test author's error, never
    the server's, so it gets its own exit code (64) instead of sharing one
    with a protocol verdict."""


class Parser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        raise UsageError(message)


PACKET_MAX = 35000   # AXL_SSH_PACKET_MAX, src/net/axl-ssh-internal.h


def check_len(n: int, encrypted: bool) -> None:
    """Refuse a packet_length before reading any of the body. Unencrypted
    framing aligns 4 + n to 8 (the length field is counted);
    chacha20-poly1305@openssh.com aligns n alone. The minimum is a padding
    byte plus 4 bytes of padding, rounded up to that alignment."""
    lo, rem = (8, 0) if encrypted else (12, 4)
    if n < lo or n > PACKET_MAX or n % 8 != rem:
        raise PeerExit(f"peer: bad length {n}", 3)


def unpad(body: bytes) -> bytes:
    """The payload of `byte padding_length || payload || padding`."""
    pad = body[0]
    if pad < 4 or 1 + pad > len(body):
        raise PeerExit(f"peer: bad length {len(body)}", 3)
    return body[1:len(body) - pad]


def _refused(f: Callable[[], object]) -> bool:
    try:
        f()
    except PeerExit:
        return True
    return False


def sshstr(b: bytes) -> bytes:
    return struct.pack(">I", len(b)) + b


def mpint_be(raw: bytes) -> bytes:
    """The shared secret as the server's axl_ssh_put_mpint encodes it: the
    32 raw X25519 bytes read BIG-endian, minimal, sign byte if needed."""
    v = raw.lstrip(b"\x00")
    if v and v[0] & 0x80:
        v = b"\x00" + v
    return sshstr(v)


def build_kexinit(lists: list[bytes], follows: bool = False) -> bytes:
    """A KEXINIT: fresh cookie, the ten name-lists, first_kex_packet_follows,
    and the reserved u32."""
    return (bytes([MSG_KEXINIT]) + os.urandom(16)
            + b"".join(sshstr(x) for x in lists)
            + bytes([1 if follows else 0]) + bytes(4))


def kex_derive(k: bytes, h: bytes, sid: bytes, letter: bytes) -> bytes:
    """RFC 4253 section 7.2, the 64 bytes chacha20-poly1305@openssh.com
    needs: K1 = HASH(K || H || letter || session_id), K2 = HASH(K || H ||
    K1). The session id is the FIRST exchange's H, kept for the connection's
    life -- a rekey changes H and K, never the session id."""
    k1 = hashlib.sha256(k + h + letter + sid).digest()
    return k1 + hashlib.sha256(k + h + k1).digest()


def verify_reply(rep: bytes, v_s: bytes, i_c: bytes, i_s: bytes,
                 priv: bytes, q_c: bytes) -> tuple[bytes, bytes]:
    """Check a KEX_ECDH_REPLY: the host key and signature are ssh-ed25519
    and the signature covers THIS exchange's hash (RFC 5656 section 4 over
    the curve25519-sha256 shared secret, RFC 8731). Returns K (mpint) and
    H; a bad signature is `peer: bad host signature` (exit 6)."""
    r = Reader(rep, 1)
    k_s, q_s, sig_blob = r.string(), r.string(), r.string()
    kr, sr = Reader(k_s), Reader(sig_blob)
    if kr.string() != b"ssh-ed25519" or sr.string() != b"ssh-ed25519":
        raise PeerExit("peer: bad host signature", 6)
    pub32, sig64 = kr.string(), sr.string()
    k = mpint_be(x25519(priv, q_s))
    h = hashlib.sha256(sshstr(V_C) + sshstr(v_s) + sshstr(i_c)
                       + sshstr(i_s) + sshstr(k_s) + sshstr(q_c)
                       + sshstr(q_s) + k).digest()
    if not ed25519_verify(pub32, h, sig64):
        raise PeerExit("peer: bad host signature", 6)
    return k, h


class Rekey:
    """One key re-exchange after login, either side's (REKEY in the module
    doc). Its i_c and i_s are the two KEXINITs the exchange hash covers:
    the FIRST of each this exchange -- a second one the peer sends on
    purpose is not hashed, as the server does not hash it either."""

    def __init__(self, ours_first: bool) -> None:
        self.ours_first = ours_first      # ours left before we read theirs
        self.i_c: bytes | None = None
        self.i_s: bytes | None = None
        self.priv = b""
        self.q_c = b""
        self.ecdh_sent = False
        self.replied = False              # the server's REPLY is verified
        self.hold_newkeys = False         # `rekey:reply`: NEWKEYS waits
        self.tx_next: bytes | None = None
        self.rx_next: bytes | None = None
        self.ours_nk = False
        self.theirs_nk = False


class Reader:
    """Walks one payload's SSH fields."""

    def __init__(self, data: bytes, off: int = 0) -> None:
        self.data = data
        self.off = off

    def u32(self) -> int:
        try:
            (v,) = struct.unpack_from(">I", self.data, self.off)
        except struct.error:
            raise PeerExit("peer: malformed message", 3) from None
        self.off += 4
        return int(v)

    def string(self) -> bytes:
        n = self.u32()
        v = self.data[self.off:self.off + n]
        if len(v) != n:
            raise PeerExit("peer: malformed message", 3)
        self.off += n
        return v


class ChanTrack:
    """The channels this peer opened: the server's number for the last one
    confirmed (`@`), and, per channel, the window and maximum packet the
    peer ADVERTISED -- every data message the server sends is checked
    against both (see CHANNELS in the module doc). Fed every payload the
    peer seals and every one it reads, so no action has to remember to."""

    def __init__(self) -> None:
        self.confirmed: int | None = None
        self.ours: dict[int, int] = {}          # server's number -> ours
        self.rx: dict[int, list[int]] = {}      # ours -> [window, maxpkt]
        self.closed: set[int] = set()           # server's numbers we CLOSEd

    def note_out(self, payload: bytes) -> None:
        """A payload the peer is about to send."""
        try:
            if payload[:1] == bytes([MSG_CHANNEL_OPEN]):
                r = Reader(payload, 1)
                if r.string() == b"session":
                    sender, window, maxpkt = r.u32(), r.u32(), r.u32()
                    self.rx[sender] = [window, maxpkt]
            elif payload[:1] == bytes([MSG_CHANNEL_CLOSE]):
                self.closed.add(Reader(payload, 1).u32())
            elif payload[:1] == bytes([MSG_CHANNEL_WINDOW_ADJUST]):
                r = Reader(payload, 1)
                ours = self.ours.get(r.u32())
                if ours is not None and ours in self.rx:
                    self.rx[ours][0] += r.u32()
        except PeerExit:
            # A deliberately malformed message has nothing to track.
            return

    def note_in(self, payload: bytes) -> None:
        """A payload the peer read: remember a confirmation, police data."""
        t = payload[0]
        if t == MSG_CHANNEL_OPEN_CONFIRMATION:
            r = Reader(payload, 1)
            ours, theirs = r.u32(), r.u32()
            self.ours[theirs] = ours
            self.confirmed = theirs
            # A number the server reuses after both CLOSEs is a new channel.
            self.closed.discard(theirs)
        elif t in (MSG_CHANNEL_DATA, MSG_CHANNEL_EXTENDED_DATA):
            r = Reader(payload, 1)
            ours = r.u32()
            if t == MSG_CHANNEL_EXTENDED_DATA:
                r.u32()
            n = len(r.string())
            w = self.rx.get(ours)
            if w is None:
                return
            if n > w[1]:
                raise PeerExit(f"peer: data over maxpkt {n}", 3)
            if n > w[0]:
                raise PeerExit(f"peer: data over window {n}", 3)
            w[0] -= n

    def theirs(self, ours: int) -> int | None:
        """The server's number for our channel OURS, if it confirmed one."""
        for t, o in self.ours.items():
            if o == ours:
                return t
        return None

    def resolve(self, body: bytes, at: bool) -> bytes:
        """BODY with the confirmed channel in its first four bytes, if AT."""
        if not at:
            return body
        if self.confirmed is None:
            raise PeerExit("peer: no channel confirmed", 3)
        return struct.pack(">I", self.confirmed) + body[4:]


def escape(b: bytes) -> str:
    """Data bytes as one printable line (see the module doc)."""
    named = {0x5C: "\\\\", 0x22: '\\"', 0x0A: "\\n", 0x0D: "\\r",
             0x09: "\\t"}
    return "".join(named.get(x) or (chr(x) if 0x20 <= x < 0x7F
                                    else f"\\x{x:02x}") for x in b)


# How long a `flood` write may stay blocked in all: it is blocked by design
# while the script holds and then while it reads the replies that reopen the
# window. Checked before every send(), so a write that never completes ends
# within this plus one I/O timeout.
FLOOD_WRITE_SEC = 30.0


# How long `--report-disconnect` waits for the server to close after its
# DISCONNECT. AxlSsh resets a connection whose peer does not close within its
# linger bound (1 s); this is well past that.
DISCONNECT_DRAIN_SEC = 5.0


class Conn:
    def __init__(self, host: str, port: int, rcvbuf: int = 0) -> None:
        try:
            if rcvbuf:
                # Before connect: the window scale is fixed in the SYN.
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                     rcvbuf)
                self.sock.settimeout(IO_TIMEOUT_SEC)
                self.sock.connect((host, port))
            else:
                self.sock = socket.create_connection((host, port),
                                                     timeout=IO_TIMEOUT_SEC)
        except OSError as e:
            raise PeerExit("peer: refused", 5) from e
        self.writer: threading.Thread | None = None
        self.writer_err: PeerExit | None = None
        self.writer_line = ""
        # Packets sealed from inside a read while a flood write is still
        # running, written by the writer thread right after it (see send()).
        # bg_lock guards the queue AND bg_open: an append can never land
        # after the writer has decided the queue is empty and finished.
        self.behind_flood: list[bytes] = []
        self.bg_lock = threading.Lock()
        self.bg_open = False                # the writer still takes appends
        self.flood_writing = False          # the flood's own bytes not all out
        self.ecdh_stall = 0.0               # `rekey:server:SEC`
        self.buf = b""
        self.tx_seq = 0
        self.rx_seq = 0
        self.tx_key: bytes | None = None
        self.rx_key: bytes | None = None
        self.v_s = b""
        self.last_frame = 0
        self.chans = ChanTrack()
        # Key re-exchange (REKEY in the module doc). Set by kex() and login().
        self.authed = False
        self.strict = False
        self.session_id = b""
        self.my_lists: list[bytes] = []    # our proposal, as a rekey sends it
        self.first_i_c = b""
        self.first_i_s = b""
        self.kx: Rekey | None = None
        self.rekeys = 0                     # exchanges completed after login
        self.backlog: deque[bytes] = deque()
        self.tamper = ""                    # --tamper: a control, see doc
        self.keepalive = "answer"           # --keepalive (KEEPALIVE in doc)
        self.report_disconnect = False      # --report-disconnect
        self.ka_held: list[bytes] = []      # answers our own rekey holds

    # -- raw bytes --
    def _fill(self) -> None:
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout as e:
            raise PeerExit("peer: timeout", 7) from e
        except OSError:
            chunk = b""
        if not chunk:
            raise EOFError
        self.buf += chunk

    def _take(self, n: int) -> bytes:
        while len(self.buf) < n:
            self._fill()
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def ident(self) -> None:
        """Exchange identification lines. Anything that ends the connection
        before the server's SSH- line is a refusal (a penalty or admission
        drop happens at accept, before any byte is sent)."""
        try:
            self.sock.sendall(V_C + b"\r\n")
            while True:
                while b"\n" not in self.buf:
                    self._fill()
                line, self.buf = self.buf.split(b"\n", 1)
                if line.startswith(b"SSH-"):
                    self.v_s = line.rstrip(b"\r")
                    return
        except (EOFError, OSError) as e:
            raise PeerExit("peer: refused", 5) from e

    # -- packets --
    def write(self, data: bytes) -> None:
        self.join_writer()
        try:
            self.sock.sendall(data)
        except OSError as e:
            raise PeerExit("peer: closed", 4) from e

    def write_bg(self, data: bytes, done_line: str) -> None:
        """Write DATA from a thread (see `flood` in the module doc); the
        socket's I/O timeout bounds each send() call, FLOOD_WRITE_SEC
        (checked before each) the whole write. sendall() is not used: its
        timeout is the whole write's, and this write is blocked on
        purpose."""
        self.join_writer()

        def flood() -> None:
            view = memoryview(data)
            deadline = time.monotonic() + FLOOD_WRITE_SEC
            while view:
                if time.monotonic() > deadline:
                    self.writer_err = PeerExit("peer: timeout", 7)
                    return
                try:
                    k = self.sock.send(view)
                except socket.timeout:
                    continue
                except OSError:
                    self.writer_err = PeerExit("peer: closed", 4)
                    return
                view = view[k:]

        def drain() -> None:
            # The queue leaves the moment the flood does, not at the next
            # join: the script's next action may be a read that waits for
            # the server's answer to a queued packet.
            while True:
                with self.bg_lock:
                    self.flood_writing = False
                    queued, self.behind_flood = self.behind_flood, []
                    if not queued or self.writer_err is not None:
                        self.bg_open = False
                        return
                try:
                    self.sock.sendall(b"".join(queued))
                except OSError:
                    self.writer_err = PeerExit("peer: closed", 4)

        def run() -> None:
            flood()
            drain()

        self.writer_line = done_line
        self.bg_open = True
        self.flood_writing = True
        self.writer = threading.Thread(target=run, daemon=True)
        self.writer.start()

    def join_writer(self) -> None:
        """Wait for a `flood` write, print its line, raise its failure."""
        if self.writer is None:
            return
        self.writer.join()
        self.writer = None
        if self.writer_err is not None:
            raise self.writer_err
        say(self.writer_line)

    def flood_running(self) -> bool:
        """A `flood` write has not finished yet."""
        return self.writer is not None and self.writer.is_alive()

    def shutdown(self) -> None:
        """`shutdown`: half-close -- no more bytes from us, reading still
        possible. A flood still being written is cut short by it (its
        blocked send() fails at once), and anything queued behind it is
        dropped: the line says which, so a transcript is exact either way.
        A flood already written is joined FIRST, so what was queued behind
        it leaves before the half-close rather than racing it."""
        with self.bg_lock:
            cutting = self.flood_writing
        if self.writer is not None and not cutting:
            self.join_writer()
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError as e:
            raise PeerExit("peer: closed", 4) from e
        if self.writer is not None:
            self.writer.join()
            self.writer = None
            self.behind_flood = []
            say(self.writer_line if self.writer_err is None
                else "peer: flood cut short")
        say("peer: shut down writing")

    def await_file(self, path: str, every: float = 0.0) -> int:
        """`await:PATH`: reading nothing, wait until PATH exists -- the
        test's signal that something it watches has happened. Bounded by
        IO_TIMEOUT_SEC (`peer: timeout`, exit 7). With EVERY (`chatter`),
        an IGNORE goes out every EVERY seconds meanwhile, so the server
        keeps hearing a peer that has stopped reading. Returns how many."""
        deadline = time.monotonic() + IO_TIMEOUT_SEC
        sent = 0
        next_at = time.monotonic() + every
        while not os.path.exists(path):
            now = time.monotonic()
            if now > deadline:
                raise PeerExit("peer: timeout", 7)
            if every > 0 and now >= next_at:
                self.send(bytes([MSG_IGNORE]) + sshstr(b""))
                sent += 1
                next_at = now + every
            time.sleep(0.05)
        return sent

    def send(self, payload: bytes, from_read: bool = False) -> int:
        """Frame, seal if keys are active, send. Returns this packet's seq.

        FROM_READ is a packet the peer sends from INSIDE a read -- a
        keepalive answer or the rekey engine's reply. While a `flood` write
        is still running it is queued behind that write instead of joining
        it: the flood may be blocked because the server stopped reading
        until its own output drains, and only this read drains it, so
        joining would deadlock the peer against itself. Sealed now and
        written by the writer thread as soon as the flood's last byte is
        out -- not at the next join, which may never come before the
        server needs it -- so packets still leave in seal order. Once the
        writer has drained its queue and closed it (bg_open), the packet
        falls through to a direct write."""
        seq, pkt = self.seal(payload)
        if from_read:
            with self.bg_lock:
                if self.bg_open:
                    self.behind_flood.append(pkt)
                    return seq
        self.write(pkt)
        self._flush_keepalive()
        return seq

    def seal(self, payload: bytes) -> tuple[int, bytes]:
        """Frame and, if keys are active, encrypt one packet WITHOUT writing
        it, so a burst can be built before any of it is sent. Consumes a
        sequence number: sealed packets must be written in the order they
        were sealed, because the seq is the chacha nonce."""
        seq = self.tx_seq
        self.chans.note_out(payload)
        if self.tx_key is None:
            pad = 8 - (5 + len(payload)) % 8
            if pad < 4:
                pad += 8
            body = bytes([pad]) + payload + os.urandom(pad)
            pkt = struct.pack(">I", len(body)) + body
        else:
            # The length field is EXCLUDED from the alignment here.
            pad = 8 - (1 + len(payload)) % 8
            if pad < 4:
                pad += 8
            body = bytes([pad]) + payload + os.urandom(pad)
            seqb = struct.pack(">Q", seq)
            k_main, k_len = self.tx_key[:32], self.tx_key[32:]
            enc_len = chacha20(k_len, seqb, 0, struct.pack(">I", len(body)))
            enc_body = chacha20(k_main, seqb, 1, body)
            polykey = chacha20(k_main, seqb, 0, bytes(32))
            pkt = enc_len + enc_body + poly1305(polykey, enc_len + enc_body)
        self.tx_seq = (self.tx_seq + 1) & M32
        kx = self.kx
        if (payload[:1] == bytes([MSG_NEWKEYS]) and kx is not None
                and kx.tx_next is not None and not kx.ours_nk):
            # A rekey's NEWKEYS, sealed under the OLD key like everything
            # before it; everything after it is under the new one, and
            # under strict KEX numbered from 0 (OpenSSH ssh_packet_send2).
            self.tx_key = kx.tx_next
            if self.strict:
                self.tx_seq = 0
            kx.ours_nk = True
            self._rekey_done()
        return seq, pkt

    def recv(self) -> bytes:
        """One payload. EOF (or a reset) raises EOFError. Records the frame's
        size on the wire in last_frame, which bench() needs."""
        if self.rx_key is None:
            (n,) = struct.unpack(">I", self._take(4))
            check_len(n, False)
            body = self._take(n)
            self.last_frame = 4 + n
        else:
            seqb = struct.pack(">Q", self.rx_seq)
            k_main, k_len = self.rx_key[:32], self.rx_key[32:]
            enc_len = self._take(4)
            (n,) = struct.unpack(">I", chacha20(k_len, seqb, 0, enc_len))
            check_len(n, True)
            rest = self._take(n + 16)
            enc_body, tag = rest[:n], rest[n:]
            polykey = chacha20(k_main, seqb, 0, bytes(32))
            if not hmac.compare_digest(poly1305(polykey, enc_len + enc_body),
                                       tag):
                raise PeerExit("peer: bad mac", 3)
            body = chacha20(k_main, seqb, 1, enc_body)
            self.last_frame = 4 + n + 16
        self.rx_seq = (self.rx_seq + 1) & M32
        return unpad(body)

    def recv_msg(self) -> bytes:
        """The next payload that is not IGNORE/DEBUG/BANNER -- one a rekey
        set aside first (REKEY in the module doc). DISCONNECT and EOF both
        end the run as `peer: closed`."""
        if self.backlog:
            return self.backlog.popleft()
        while True:
            p = self._next()
            if p is not None:
                return p

    def _next(self) -> bytes | None:
        """Read one packet. A rekey message after login goes to the engine
        and IGNORE/DEBUG/BANNER are skipped (None); anything else is
        returned -- after the wire check: between the server's KEXINIT and
        its NEWKEYS only what sendable_mid_kex() allows may arrive."""
        try:
            p = self.recv()
        except EOFError as e:
            raise PeerExit("peer: closed", 4) from e
        if p and p[0] == MSG_DISCONNECT and self.report_disconnect:
            self._report_disconnect(p)
        if not p or p[0] == MSG_DISCONNECT:
            raise PeerExit("peer: closed", 4)
        kx = self.kx
        if (kx is not None and kx.i_s is not None and not kx.theirs_nk
                and not sendable_mid_kex(p[0])):
            raise PeerExit(f"peer: got {p[0]} inside the server's exchange", 3)
        if p[0] in SKIPPED:
            return None
        if self.authed and p[0] in KEX_MSGS:
            self._rekey_input(p)
            return None
        if self.authed and self._keepalive(p):
            return None
        self.chans.note_in(p)
        return p

    def _report_disconnect(self, p: bytes) -> None:
        """`--report-disconnect`: name the DISCONNECT, then read on until the
        server closes and count what it sent AFTER it -- RFC 4253 11.1 says
        nothing should follow, and AxlSsh promises nothing does."""
        r = Reader(p, 1)
        code = r.u32()
        text = r.string().decode("utf-8", "replace")
        say(f'peer: disconnect {code} "{escape(text.encode())}"')
        after = 0
        self.sock.settimeout(DISCONNECT_DRAIN_SEC)
        try:
            while True:
                self.recv()
                after += 1
        except (EOFError, PeerExit):
            pass
        say(f"peer: {after} packet(s) after the disconnect")

    def _keepalive(self, p: bytes) -> bool:
        """A keepalive@openssh.com from the server, answered as ssh(1)
        answers it (KEEPALIVE in the module doc) or, under `--keepalive
        ignore`, read and dropped. False for anything else."""
        if p[0] not in (MSG_GLOBAL_REQUEST, MSG_CHANNEL_REQUEST):
            return False
        r = Reader(p, 1)
        ours = r.u32() if p[0] == MSG_CHANNEL_REQUEST else -1
        if r.string() != KEEPALIVE:
            return False
        want = r.off < len(p) and p[r.off] != 0
        if self.keepalive == "ignore":
            say(f"peer: ignored keepalive {p[0]}")
            return True
        if p[0] == MSG_GLOBAL_REQUEST:
            # clientloop.c client_input_global_request: REQUEST_FAILURE
            # whenever a reply is wanted.
            answer = bytes([MSG_REQUEST_FAILURE]) if want else b""
        else:
            # clientloop.c client_input_channel_req: CHANNEL_FAILURE to the
            # server's number, unless we have already sent our CLOSE.
            theirs = self.chans.theirs(ours)
            answer = (bytes([MSG_CHANNEL_FAILURE]) + struct.pack(">I", theirs)
                      if want and theirs is not None
                      and theirs not in self.chans.closed else b"")
        if not answer:
            say(f"peer: unanswered keepalive {p[0]}")
            return True
        say(f"peer: answered keepalive {p[0]}")
        kx = self.kx
        if kx is not None and kx.i_c is not None and not kx.ours_nk:
            # Inside our own exchange: held until our NEWKEYS, as
            # ssh_packet_send2 holds every non-kex packet.
            self.ka_held.append(answer)
        else:
            self.send(answer, from_read=True)
        return True

    def _flush_keepalive(self) -> None:
        kx = self.kx
        if self.ka_held and (kx is None or kx.ours_nk):
            self.send(self.ka_held.pop(0), from_read=True)

    def idle(self, sec: float) -> None:
        """`idle:SEC`: read for SEC seconds, keepalives handled as every
        read handles them and anything else kept, in order, for the
        actions after. A close ends the run as `peer: closed`."""
        deadline = time.monotonic() + sec
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return
            if not self.buf:
                ready, _, _ = select.select([self.sock], [], [], left)
                if not ready:
                    return
            p = self._next()
            if p is not None:
                self.backlog.append(p)

    def drive(self, done: Callable[[], bool]) -> None:
        """Read until DONE, the engine answering every rekey message and
        everything else kept, in order, for the actions after."""
        while not done():
            p = self._next()
            if p is not None:
                self.backlog.append(p)

    # -- key re-exchange (REKEY in the module doc) --
    def rekey_kexinit(self, guess: bool = False,
                      from_read: bool = False) -> int:
        """Send a rekey KEXINIT in the shape OpenSSH sends one: the proposal
        of the first exchange with ext-info-c and the strict pseudo-name
        filtered out (kex.c kex_input_newkeys), first_kex_packet_follows 0.
        The first of an exchange is the one hashed. With GUESS, a method the
        server lacks goes first and the flag is set -- the caller sends the
        guessed packet. Returns its seq."""
        kx = self.kx
        if kx is None:
            kx = self.kx = Rekey(ours_first=True)
        lists = list(self.my_lists)
        if guess:
            lists[0] = WRONG_GUESS_KEX + b"," + lists[0]
        i_c = build_kexinit(lists, guess)
        if kx.i_c is None:
            kx.i_c = i_c
        return self.send(i_c, from_read)

    def _rekey_input(self, p: bytes) -> None:
        """One message of a key re-exchange, as OpenSSH's client answers it
        (kex.c, kexgen.c): the server's KEXINIT starts one if we have not
        (ours goes back at once), and with both KEXINITs out ours is
        followed by KEX_ECDH_INIT; the REPLY is verified over THIS
        exchange's hash and answered with NEWKEYS; the server's NEWKEYS
        switches the receive key. Anything else is `peer: unexpected T`."""
        t, kx = p[0], self.kx
        if t == MSG_KEXINIT and (kx is None or kx.i_s is None):
            if kx is None:
                kx = self.kx = Rekey(ours_first=False)
            kx.i_s = p
            if kx.i_c is None:
                self.rekey_kexinit(from_read=True)
            if not kx.ours_first and self.ecdh_stall > 0:
                # `rekey:server:SEC`: the server's exchange held open with
                # our KEXINIT out and our KEX_ECDH_INIT not yet sent.
                time.sleep(self.ecdh_stall)
            kx.priv, kx.q_c = self.ecdh_init(from_read=True)
            kx.ecdh_sent = True
        elif (t == MSG_KEX_ECDH_REPLY and kx is not None and kx.ecdh_sent
              and not kx.replied and kx.i_c is not None
              and kx.i_s is not None):
            # --tamper hash: the FIRST exchange's KEXINITs, which a peer
            # that never replaced them would hash; the signature must fail.
            stale = self.tamper == "hash"
            k, h = verify_reply(p, self.v_s,
                                self.first_i_c if stale else kx.i_c,
                                self.first_i_s if stale else kx.i_s,
                                kx.priv, kx.q_c)
            # --tamper sid: this exchange's H where the session id belongs.
            sid = h if self.tamper == "sid" else self.session_id
            kx.tx_next = kex_derive(k, h, sid, b"C")
            kx.rx_next = kex_derive(k, h, sid, b"D")
            kx.replied = True
            if not kx.hold_newkeys:
                self.send(bytes([MSG_NEWKEYS]), from_read=True)
        elif (t == MSG_NEWKEYS and kx is not None and kx.rx_next is not None
              and not kx.theirs_nk):
            self.rx_key = kx.rx_next
            if self.strict:
                self.rx_seq = 0
            kx.theirs_nk = True
            self._rekey_done()
        else:
            raise PeerExit(f"peer: unexpected {t}", 3)

    def _rekey_done(self) -> None:
        kx = self.kx
        if kx is None or not (kx.ours_nk and kx.theirs_nk):
            return
        self.kx = None
        self.rekeys += 1
        say(f"peer: rekeyed, started by "
            f"{'us' if kx.ours_first else 'the server'}")

    # -- key exchange --
    def kexinit(self) -> tuple[bytes, bytes]:
        """Read the server's KEXINIT and answer with ours; returns both, as
        the exchange hash needs them. After this the server waits for
        KEX_ECDH_INIT."""
        i_s = self.recv_msg()
        if i_s[0] != MSG_KEXINIT:
            raise PeerExit(f"peer: unexpected {i_s[0]}", 3)
        # Ten name-lists COPIED from the server's own, so its selector finds
        # its one algorithm per slot; only the kex list is ours, because it
        # carries the strict-KEX marker (Terrapin, OpenSSH PROTOCOL 1.10).
        r = Reader(i_s, 17)
        lists = [r.string() for _ in range(10)]
        self.strict = STRICT_S in lists[0].split(b",")
        lists[0] = KEX_NAME + b"," + STRICT_C
        i_c = build_kexinit(lists)
        self.send(i_c)
        # A rekey offers the same, without the pseudo-name.
        self.my_lists = [KEX_NAME] + lists[1:]
        self.first_i_c, self.first_i_s = i_c, i_s
        return i_s, i_c

    def ecdh_init(self, from_read: bool = False) -> tuple[bytes, bytes]:
        """Send KEX_ECDH_INIT with a fresh ephemeral key; returns the private
        scalar and the public value, as the exchange hash needs both. After
        this the server owes its KEX_ECDH_REPLY and NEWKEYS."""
        priv = os.urandom(32)
        q_c = x25519(priv, bytes([9]) + bytes(31))
        self.send(bytes([MSG_KEX_ECDH_INIT]) + sshstr(q_c), from_read)
        return priv, q_c

    def kex(self, until_reply: bool = False) -> None:
        """The whole key exchange; with @until_reply, stop once the server's
        KEX_ECDH_REPLY is verified, before our NEWKEYS -- where ssh(1) sits
        while its user answers the host-key prompt."""
        i_s, i_c = self.kexinit()
        priv, q_c = self.ecdh_init()
        rep = self.recv_msg()
        if rep[0] != MSG_KEX_ECDH_REPLY:
            raise PeerExit(f"peer: unexpected {rep[0]}", 3)
        k, h = verify_reply(rep, self.v_s, i_c, i_s, priv, q_c)
        if until_reply:
            return
        # The session id is H at the first kex (RFC 4253 section 7.2).
        self.session_id = h

        # Strict KEX: each direction's sequence number resets at its own
        # NEWKEYS -- transmit right after sending ours, receive right after
        # the server's arrives.
        self.send(bytes([MSG_NEWKEYS]))
        self.tx_seq = 0
        self.tx_key = kex_derive(k, h, h, b"C")
        nk = self.recv_msg()
        if nk[0] != MSG_NEWKEYS:
            raise PeerExit(f"peer: unexpected {nk[0]}", 3)
        self.rx_seq = 0
        self.rx_key = kex_derive(k, h, h, b"D")

    def login(self, user: str, password: str) -> None:
        self.send(bytes([MSG_SERVICE_REQUEST]) + sshstr(b"ssh-userauth"))
        p = self.recv_msg()
        if p[0] != MSG_SERVICE_ACCEPT:
            raise PeerExit(f"peer: unexpected {p[0]}", 3)
        self.send(bytes([MSG_USERAUTH_REQUEST]) + sshstr(user.encode())
                  + sshstr(b"ssh-connection") + sshstr(b"password")
                  + b"\x00" + sshstr(password.encode()))
        p = self.recv_msg()
        if p[0] == MSG_USERAUTH_FAILURE:
            raise PeerExit("peer: auth failed", 2)
        if p[0] != MSG_USERAUTH_SUCCESS:
            raise PeerExit(f"peer: unexpected {p[0]}", 3)
        # From here a KEXINIT is a rekey (the server rekeys only after
        # authentication, as sshd does).
        self.authed = True
        say("peer: authenticated")


def say(line: str) -> None:
    # Flushed per line: the admission tests run peers in the background and
    # wait for a line to appear before starting the next one.
    print(line, flush=True)


class Part(NamedTuple):
    """One message of a `batch`."""
    num: int             # message type
    data: bytes          # body after the type byte
    at: bool             # the body was written `@HEX` (see CHANNELS)


class Action(NamedTuple):
    verb: str
    num: int = 0         # message type, or want_reply for `global`, or the
                         # byte count of `expectdata`/`expectsum`/`pour`
    data: bytes = b""    # body after the type byte, or the global's name
    sec: float = 0.0     # `hold`
    reps: int = 1        # `burst` / `expectn`; `pour`'s packet size
    at: bool = False     # the body was written `@HEX` (see CHANNELS)
    parts: tuple[Part, ...] = ()   # `batch`
    mode: str = ""       # `kexinit` / `rekey`: the argument after the
                         # colon; `await`: the path
    among: int = -1      # `expectsum:N:K:T`: T (K is reps)


def _byte(v: str, what: str) -> int:
    try:
        n = int(v)
    except ValueError:
        raise UsageError(f"{what} {v!r} is not an integer") from None
    if not 0 <= n <= 255:
        raise UsageError(f"{what} {n} is not a byte")
    return n


def _count(v: str, what: str) -> int:
    try:
        n = int(v)
    except ValueError:
        raise UsageError(f"{what} {v!r} is not an integer") from None
    if n < 1:
        raise UsageError(f"{what} {n} is not a positive count")
    return n


def _hex(hx: str, action: str) -> bytes:
    try:
        return bytes.fromhex(hx)
    except ValueError:
        raise UsageError(f"{action!r}: bad hex") from None


def _body(hx: str, action: str) -> tuple[bytes, bool]:
    """A message body, `HEX` or `@HEX` (see CHANNELS). An `@` body must
    have room for the recipient it stands for."""
    if not hx.startswith("@"):
        return _hex(hx, action), False
    body = _hex(hx[1:], action)
    if len(body) < 4:
        raise UsageError(f"{action!r}: an @ body needs its 4-byte recipient")
    return body, True


def parse_script(script: str) -> list[Action]:
    """Validate the whole script BEFORE connecting: a malformed action is a
    usage error (exit 64), never a half-run connection."""
    out: list[Action] = []
    for action in (a for a in script.split(",") if a):
        verb, _, arg = action.partition(":")
        if verb == "send":
            t, _, hx = arg.partition(":")
            body, at = _body(hx, action)
            out.append(Action(verb, _byte(t, "message type"), body, at=at))
        elif verb in ("burst", "flood"):
            n, _, rest = arg.partition(":")
            t, _, hx = rest.partition(":")
            body, at = _body(hx, action)
            out.append(Action(verb, _byte(t, "message type"), body,
                              reps=_count(n, f"{verb} count"), at=at))
        elif verb == "batch":
            parts: list[Part] = []
            for item in arg.split("+"):
                t, sep, hx = item.partition(":")
                if not sep:
                    raise UsageError(f"{action!r}: want batch:T:HEX+T:HEX")
                body, at = _body(hx, action)
                parts.append(Part(_byte(t, "message type"), body, at))
            out.append(Action(verb, parts=tuple(parts)))
        elif verb == "pour":
            n, sep, k = arg.partition(":")
            if not sep:
                raise UsageError(f"{action!r}: want pour:BYTES:PKT")
            out.append(Action(verb, _count(n, "pour bytes"),
                              reps=_count(k, "pour packet size")))
        elif verb == "expectsum" and arg.count(":") == 2:
            n, k, t = arg.split(":")
            out.append(Action(verb, _count(n, "expectsum bytes"),
                              reps=_count(k, "expectsum count"),
                              among=_byte(t, "message type")))
        elif verb in ("expectdata", "expectsum"):
            out.append(Action(verb, _count(arg, f"{verb} bytes")))
        elif verb == "global":
            name, _, want = arg.rpartition(":")
            if not name or want not in ("0", "1"):
                raise UsageError(f"{action!r}: want global:NAME:0|1")
            out.append(Action(verb, int(want), name.encode()))
        elif verb == "expect":
            out.append(Action(verb, _byte(arg, "message type")))
        elif verb == "expectn":
            n, sep, t = arg.partition(":")
            if not sep:
                raise UsageError(f"{action!r}: want expectn:N:T")
            out.append(Action(verb, _byte(t, "message type"),
                              reps=_count(n, "expectn count")))
        elif verb == "kexinit":
            if arg not in ("", "guess"):
                raise UsageError(f"{action!r}: want kexinit or kexinit:guess")
            out.append(Action(verb, mode=arg))
        elif verb == "rekey":
            mode, _, stall = arg.partition(":")
            if mode not in ("", "reply", "server") or (stall
                                                        and mode != "server"):
                raise UsageError(f"{action!r}: want rekey, rekey:reply, "
                                 "rekey:server or rekey:server:SEC")
            sec = 0.0
            if stall:
                try:
                    sec = float(stall)
                except ValueError:
                    raise UsageError(f"{action!r}: bad seconds") from None
                if sec <= 0:
                    raise UsageError(f"{action!r}: seconds must be positive")
            out.append(Action(verb, mode=mode, sec=sec))
        elif verb == "shutdown":
            if arg:
                raise UsageError(f"{action!r}: shutdown takes no argument")
            out.append(Action(verb))
        elif verb == "await":
            if not arg:
                raise UsageError(f"{action!r}: want await:PATH")
            out.append(Action(verb, mode=arg))
        elif verb == "chatter":
            every, sep, path = arg.partition(":")
            try:
                sec = float(every)
            except ValueError:
                raise UsageError(f"{action!r}: want chatter:SEC:PATH") from None
            if not sep or not path or sec <= 0:
                raise UsageError(f"{action!r}: want chatter:SEC:PATH")
            out.append(Action(verb, mode=path, sec=sec))
        elif verb == "pick":
            try:
                n = int(arg)
            except ValueError:
                raise UsageError(f"{action!r}: want pick:N") from None
            if not 0 <= n <= M32:
                raise UsageError(f"{action!r}: {n} is not a u32")
            out.append(Action(verb, n))
        elif verb in ("hold", "idle"):
            try:
                sec = float(arg)
            except ValueError:
                raise UsageError(f"{action!r}: bad seconds") from None
            if sec < 0:
                raise UsageError(f"{action!r}: negative seconds")
            out.append(Action(verb, sec=sec))
        else:
            raise UsageError(f"unknown script action {action!r}")
    return out


def expect_type(c: Conn, want: int) -> bytes:
    p = c.recv_msg()
    if p[0] != want:
        raise PeerExit(f"peer: unexpected {p[0]}", 3)
    return p


def read_data(c: Conn, n: int, show: bool, among: int = -1,
              k: int = 0) -> None:
    """`expectdata` (SHOW) and `expectsum`: read channel data until N
    bytes have arrived, skipping WINDOW_ADJUST -- and, for
    `expectsum:N:K:T`, until K messages of type AMONG have too."""
    got = 0
    seen = 0
    count = {"out": 0, "err": 0}
    digest = hashlib.sha256()
    while got < n or seen < k:
        p = c.recv_msg()
        if p[0] == MSG_CHANNEL_WINDOW_ADJUST:
            continue
        if p[0] == among and seen < k:
            seen += 1
            continue
        if got >= n:
            raise PeerExit(f"peer: unexpected {p[0]}", 3)
        r = Reader(p, 1)
        r.u32()
        if p[0] == MSG_CHANNEL_DATA:
            stream = "out"
        elif p[0] == MSG_CHANNEL_EXTENDED_DATA and r.u32() == 1:
            stream = "err"
        else:
            raise PeerExit(f"peer: unexpected {p[0]}", 3)
        data = r.string()
        got += len(data)
        count[stream] += len(data)
        digest.update(data)
        if show:
            say(f'peer: data {stream} "{escape(data)}"')
    if not show:
        say(f"peer: sum out={count['out']} err={count['err']} "
            f"sha256={digest.hexdigest()}")
    if k:
        say(f"peer: got {k} x {among}")


def say_got(p: bytes) -> None:
    """`expect:T`'s line for message P (see the module doc)."""
    r = Reader(p, 1)
    if p[0] == MSG_UNIMPLEMENTED:
        say(f"peer: got 3 seq={r.u32()}")
    elif p[0] == MSG_CHANNEL_OPEN_CONFIRMATION:
        r.u32()
        say(f"peer: got 91 sender={r.u32()} window={r.u32()} "
            f"maxpkt={r.u32()}")
    elif p[0] == MSG_CHANNEL_OPEN_FAILURE:
        r.u32()
        say(f"peer: got 92 reason={r.u32()}")
    elif p[0] == MSG_CHANNEL_WINDOW_ADJUST:
        r.u32()
        say(f"peer: got 93 bytes={r.u32()}")
    elif p[0] == MSG_CHANNEL_REQUEST:
        r.u32()
        name = r.string()
        want = p[r.off] if r.off < len(p) else -1
        say(f"peer: got 98 name={escape(name)} want={want} "
            f"rest={p[r.off + 1:].hex()}")
    else:
        say(f"peer: got {p[0]}")


def run_script(c: Conn, actions: list[Action]) -> None:
    for a in actions:
        if a.verb == "send":
            seq = c.send(bytes([a.num]) + c.chans.resolve(a.data, a.at))
            say(f"peer: sent {a.num} seq={seq}")
        elif a.verb == "burst":
            # Every packet is sealed before the first byte is written, so
            # the host's pure-Python crypto cannot space them out: the server
            # sees one back-to-back run, as many to a TCP segment as fit.
            body = c.chans.resolve(a.data, a.at)
            sealed = [c.seal(bytes([a.num]) + body) for _ in range(a.reps)]
            c.write(b"".join(pkt for _, pkt in sealed))
            say(f"peer: sent burst {a.reps} x {a.num} "
                f"seqs={sealed[0][0]}..{sealed[-1][0]}")
        elif a.verb == "batch":
            sealed = [c.seal(bytes([m.num]) + c.chans.resolve(m.data, m.at))
                      for m in a.parts]
            c.write(b"".join(pkt for _, pkt in sealed))
            say(f"peer: sent batch {'+'.join(str(m.num) for m in a.parts)} "
                f"seqs={sealed[0][0]}..{sealed[-1][0]}")
        elif a.verb == "pour":
            rcpt = c.chans.resolve(bytes(4), True)
            sizes = [min(a.reps, a.num - i) for i in range(0, a.num, a.reps)]
            sealed = [c.seal(bytes([MSG_CHANNEL_DATA]) + rcpt
                             + sshstr(b"x" * k)) for k in sizes]
            c.write(b"".join(pkt for _, pkt in sealed))
            say(f"peer: poured {a.num} in {len(sizes)} packets "
                f"seqs={sealed[0][0]}..{sealed[-1][0]}")
        elif a.verb in ("expectdata", "expectsum"):
            read_data(c, a.num, a.verb == "expectdata", a.among, a.reps
                      if a.among >= 0 else 0)
        elif a.verb == "flood":
            body = c.chans.resolve(a.data, a.at)
            sealed = [c.seal(bytes([a.num]) + body) for _ in range(a.reps)]
            say(f"peer: writing {a.reps} x {a.num} "
                f"seqs={sealed[0][0]}..{sealed[-1][0]}")
            c.write_bg(b"".join(pkt for _, pkt in sealed),
                       f"peer: wrote {a.reps} x {a.num}")
        elif a.verb == "global":
            seq = c.send(bytes([MSG_GLOBAL_REQUEST]) + sshstr(a.data)
                         + bytes([a.num]))
            say(f"peer: sent {MSG_GLOBAL_REQUEST} seq={seq}")
        elif a.verb == "expect":
            say_got(expect_type(c, a.num))
        elif a.verb == "expectn":
            for _ in range(a.reps):
                expect_type(c, a.num)
            say(f"peer: got {a.reps} x {a.num}")
        elif a.verb == "kexinit":
            # In-read semantics (Conn.send): behind a running flood it is
            # queued, not joined -- otherwise exactly a direct write.
            seq = c.rekey_kexinit(a.mode == "guess", from_read=True)
            say(f"peer: sent 20 seq={seq}")
            if a.mode == "guess":
                # The packet the guess promised: a group14 KEXDH_INIT (type
                # 30, mpint e), meaningless as the KEX_ECDH_INIT the server
                # will take type 30 for unless it drops it.
                e = b"\x00" + os.urandom(256)
                seq = c.send(bytes([MSG_KEX_ECDH_INIT]) + sshstr(e))
                say(f"peer: sent {MSG_KEX_ECDH_INIT} seq={seq}")
        elif a.verb == "rekey":
            rekey(c, a.mode, a.sec)
        elif a.verb == "shutdown":
            c.shutdown()
        elif a.verb == "await":
            c.await_file(a.mode)
        elif a.verb == "chatter":
            say(f"peer: chattered {c.await_file(a.mode, a.sec)}")
        elif a.verb == "pick":
            theirs = c.chans.theirs(a.num)
            if theirs is None:
                raise PeerExit("peer: no channel confirmed", 3)
            c.chans.confirmed = theirs
        elif a.verb == "idle":
            c.idle(a.sec)
        else:
            time.sleep(a.sec)
    c.join_writer()


def rekey(c: Conn, mode: str, stall: float = 0.0) -> None:
    """The `rekey` actions (REKEY in the module doc). STALL is
    `rekey:server:SEC`'s wait before our KEX_ECDH_INIT."""
    done = c.rekeys
    if mode == "server":
        if c.kx is not None and c.kx.ours_first:
            raise PeerExit("peer: rekey:server with our exchange open", 3)
        c.ecdh_stall = stall
        try:
            c.drive(lambda: c.rekeys > done)
        finally:
            c.ecdh_stall = 0.0
        return
    if c.kx is None:
        c.rekey_kexinit()
    kx = c.kx
    assert kx is not None
    if mode == "reply":
        kx.hold_newkeys = True
        c.drive(lambda: kx.replied)
        say("peer: rekey reply verified")
        return
    if kx.replied and not kx.ours_nk:
        c.send(bytes([MSG_NEWKEYS]))
    c.drive(lambda: c.rekeys > done)


def _x11_open(sender: int) -> bytes:
    """CHANNEL_OPEN "x11" exactly as RFC 4254 section 6.3.2 shapes it:
    sender channel, initial window 32768, max packet 32768, then the
    originator address "127.0.0.1" and port 6010. The originator fields are
    REQUIRED: an earlier version left them out because AxlSsh only read the
    type and sender before refusing, and that malformed open hid a server
    that ended the connection on every well-formed one. The fixed address
    keeps every request the same size; the reply's size does not depend on
    the request at all."""
    return (bytes([MSG_CHANNEL_OPEN]) + sshstr(b"x11")
            + struct.pack(">III", sender, 32768, 32768)
            + sshstr(b"127.0.0.1") + struct.pack(">I", 6010))


def _plain_open(ctype: bytes, sender: int) -> bytes:
    """CHANNEL_OPEN of type CTYPE with nothing after max packet (RFC 4254
    section 5.1). Well-formed for a type the server does not know -- it
    cannot know what data such a type carries -- and refused with
    UNKNOWN_CHANNEL_TYPE like any other. test-ssh-admission-qemu.sh uses it
    with "nosuch", whose request frame is 52 bytes: the size that test's
    flood was measured with."""
    return (bytes([MSG_CHANNEL_OPEN]) + sshstr(ctype)
            + struct.pack(">III", sender, 32768, 32768))


def _u32(v: str, what: str) -> int:
    try:
        n = int(v)
    except ValueError:
        raise UsageError(f"{what} {v!r} is not an integer") from None
    if not 0 <= n <= M32:
        raise UsageError(f"{what} {n} is not a u32")
    return n


def _recipient(v: str) -> tuple[bytes, str]:
    """`--recipient`: the 4-byte field, and the prefix the printed hex
    carries -- `@` for the confirmed channel, whose field is then zero."""
    if v == "@":
        return bytes(4), "@"
    return struct.pack(">I", _u32(v, "--recipient")), ""


def chan_hex(args: argparse.Namespace) -> str:
    """The chan-*-hex helpers: one message body, as the module doc says."""
    if args.cmd == "chan-open-hex":
        return (sshstr(b"session")
                + struct.pack(">III", _u32(args.sender, "--sender"),
                              _u32(args.window, "--window"),
                              _u32(args.maxpkt, "--maxpkt"))).hex()
    rcpt, at = _recipient(args.recipient)
    if args.cmd == "chan-req-hex":
        if args.want not in ("0", "1"):
            raise UsageError(f"--want {args.want!r} is not 0 or 1")
        body = (rcpt + sshstr(args.name.encode()) + bytes([int(args.want)])
                + _hex(args.rest, "--rest"))
    elif args.cmd == "chan-data-hex":
        ext = struct.pack(">I", 1) if args.stream == "err" else b""
        body = rcpt + ext + sshstr(args.text.encode())
    else:
        body = rcpt + struct.pack(">I", _u32(args.bytes, "--bytes"))
    return at + body.hex()


def bench(c: Conn, n: int) -> None:
    """Time N pipelined CHANNEL_OPEN round trips and print
    `bench: n=N secs=S frame=F`.

    THE METHOD is the P3 throughput spike's outbound measurement (spike
    report section 6): every channel type but `session` is refused now and
    in every later phase (spec section 12a.3), with a CHANNEL_OPEN_FAILURE
    that no cap limits, so N opens draw exactly N replies and the server
    needs no probe. What it measures is the OUTBOUND path: an inbound open
    parses in a few microseconds, while each reply pays a whole TCP send.

    THE HOST'S CRYPTO STAYS OFF THE CLOCK. Pure-Python chacha20-poly1305 is
    around a millisecond a packet -- ten times what is being measured. So all
    N opens are sealed BEFORE the clock starts and leave in one sendall, and
    the replies are not decrypted while it runs: the first one is, to learn
    the frame size F (the server's padding is deterministic, so every reply
    to an identical-length open is the same size), and then exactly
    (N-1)*F more bytes are counted in. S runs from the first send to the
    last byte (time.perf_counter). After the clock stops, the LAST reply is
    decrypted at its offset and sequence number and must name sender N-1:
    one frame of a different size anywhere in between would put it at the
    wrong offset and fail its length check or its MAC, so the byte count
    cannot stand in for N replies by accident.

    USE THE SLOPE, NOT ONE S. Sealing takes up to a second, the connection
    sits idle meanwhile, and a burst after idle meets the stall of spike
    section 4 (host TCP slow-start-after-idle and loss recovery; about 0.5 s
    with this tool on x64, 1.7 s with the spike's warm-up round trips, absent
    on aa64). That stall and the one decrypt above are the same at every N,
    so (S(6000) - S(1000)) / 5000 is the cost of one round trip with both
    cancelled. A single S is not a rate.

    AND COMPARE MEDIANS, NOT ONE PAIR. On x64 the stall hits most runs but
    not all: a pair where it hit only one run gives a slope that is garbage
    (negative, or twice the truth). Take several runs at each N and use the
    slope between the medians."""
    opens = b"".join(c.seal(_x11_open(i))[1] for i in range(n))
    rx0 = c.rx_seq
    t0 = time.perf_counter()
    c.write(opens)
    first = expect_type(c, MSG_CHANNEL_OPEN_FAILURE)
    frame = c.last_frame
    need = (n - 1) * frame
    raw = bytearray(need)
    view = memoryview(raw)
    got = min(len(c.buf), need)
    view[:got] = c.buf[:got]
    try:
        while got < need:
            k = c.sock.recv_into(view[got:])
            if k == 0:
                raise PeerExit("peer: closed", 4)
            got += k
    except socket.timeout as e:
        raise PeerExit("peer: timeout", 7) from e
    except OSError as e:     # as _fill(): any other socket error is a close
        raise PeerExit("peer: closed", 4) from e
    secs = time.perf_counter() - t0
    if Reader(first, 1).u32() != 0:
        raise PeerExit("peer: malformed message", 3)
    if n > 1:
        c.buf = bytes(view[(n - 2) * frame:])
        c.rx_seq = (rx0 + n - 1) & M32
        last = expect_type(c, MSG_CHANNEL_OPEN_FAILURE)
        if Reader(last, 1).u32() != n - 1:
            raise PeerExit("peer: malformed message", 3)
    say(f"bench: n={n} secs={secs:.4f} frame={frame}")


def kexwait(c: Conn, hold: float, stage: str) -> None:
    """Hold a connection at a chosen point and send nothing more. `kex`
    finishes key exchange, so the server keeps it unauthenticated in
    ST_AUTH; `reply` stops after verifying the server's KEX_ECDH_REPLY, so
    the server waits in ST_NEWKEYS -- a user at ssh(1)'s host-key prompt;
    `kexinit` stops after our KEXINIT, so the server waits in ST_KEX for a
    KEX_ECDH_INIT that never comes; `ecdh` sends that KEX_ECDH_INIT too and
    never reads the reply, so the server has packets to send the moment the
    peer goes. All three are still in key exchange, but further along than a
    peer that has said nothing.

    Ends with `peer: closed after_ms=MS` if the server closes first, else
    `peer: held rx=N`, N the bytes the server sent during the hold."""
    if stage == "kexinit":
        c.kexinit()
        say("peer: kexinit sent")
    elif stage == "ecdh":
        c.kexinit()
        c.ecdh_init()
        say("peer: ecdh init sent")
    elif stage == "reply":
        c.kex(until_reply=True)
        say("peer: host key verified")
    else:
        c.kex()
        say("peer: kex complete")
    start = time.monotonic()
    deadline = start + hold
    rx = 0
    while (left := deadline - time.monotonic()) > 0:
        ready, _, _ = select.select([c.sock], [], [], left)
        if not ready:
            continue
        try:
            chunk = c.sock.recv(65536)
        except OSError:
            chunk = b""
        if not chunk:
            ms = int((time.monotonic() - start) * 1000)
            say(f"peer: closed after_ms={ms}")
            return
        # Anything the server sends here (a DISCONNECT, say) is followed by
        # the close this loop reports; the bytes themselves are not graded,
        # only COUNTED: a peer that holds must be able to say the server
        # stayed silent, not merely that it did not close.
        rx += len(chunk)
    say(f"peer: held rx={rx}")


def main() -> int:
    try:
        return run(sys.argv[1:])
    except UsageError as e:
        print(f"ssh-peer: usage: {e}", file=sys.stderr)
        return 64


def run(argv: list[str]) -> int:
    ap = Parser(description="A scripted SSH client for AxlSsh tests.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest")
    lp = sub.add_parser("login")
    lp.add_argument("host")
    lp.add_argument("port", type=int)
    lp.add_argument("--user", required=True)
    lp.add_argument("--password", required=True)
    lp.add_argument("--script", default="")
    lp.add_argument("--hold", type=float, default=0.0)
    lp.add_argument("--rcvbuf", type=int, default=0)
    lp.add_argument("--preauth", default="")
    lp.add_argument("--tamper", choices=("hash", "sid"), default="")
    lp.add_argument("--keepalive", choices=("answer", "ignore"),
                    default="answer")
    lp.add_argument("--report-disconnect", action="store_true")
    kp = sub.add_parser("kexwait")
    kp.add_argument("host")
    kp.add_argument("port", type=int)
    kp.add_argument("--hold", type=float, required=True)
    kp.add_argument("--stage", choices=("kex", "reply", "kexinit", "ecdh"),
                    default="kex")
    xp = sub.add_parser("x11-open-hex")
    xp.add_argument("--sender", default="1")
    op = sub.add_parser("open-hex")
    op.add_argument("--type", required=True)
    op.add_argument("--sender", default="1")
    cp = sub.add_parser("chan-open-hex")
    cp.add_argument("--sender", default="0")
    cp.add_argument("--window", default="2097152")
    cp.add_argument("--maxpkt", default="32768")
    rp = sub.add_parser("chan-req-hex")
    rp.add_argument("--recipient", required=True)
    rp.add_argument("--name", required=True)
    rp.add_argument("--want", default="0")
    rp.add_argument("--rest", default="")
    dp = sub.add_parser("chan-data-hex")
    dp.add_argument("--recipient", required=True)
    dp.add_argument("--text", required=True)
    dp.add_argument("--stream", choices=("out", "err"), default="out")
    ap2 = sub.add_parser("chan-adjust-hex")
    ap2.add_argument("--recipient", required=True)
    ap2.add_argument("--bytes", required=True)
    bp = sub.add_parser("bench")
    bp.add_argument("host")
    bp.add_argument("port", type=int)
    bp.add_argument("--user", required=True)
    bp.add_argument("--password", required=True)
    bp.add_argument("--n", required=True)
    args = ap.parse_args(argv)

    if args.cmd == "selftest":
        return selftest()
    if args.cmd in ("x11-open-hex", "open-hex"):
        try:
            sender = int(args.sender)
        except ValueError:
            raise UsageError(f"--sender {args.sender!r} is not an integer") \
                from None
        if not 0 <= sender <= M32:
            raise UsageError(f"--sender {sender} is not a u32")
        if args.cmd == "open-hex":
            print(_plain_open(args.type.encode(), sender)[1:].hex())
        else:
            print(_x11_open(sender)[1:].hex())
        return 0
    if args.cmd.startswith("chan-"):
        print(chan_hex(args))
        return 0
    actions = parse_script(args.script) if args.cmd == "login" else []
    preauth = parse_script(args.preauth) if args.cmd == "login" else []
    if any(a.verb in ("kexinit", "rekey") for a in preauth):
        # The server rekeys only after authentication; before it a KEXINIT
        # is `send:20:HEX`, a message like any other.
        raise UsageError("kexinit and rekey are --script actions")
    n = _count(args.n, "--n") if args.cmd == "bench" else 0
    if getattr(args, "hold", 0.0) < 0:
        raise UsageError("--hold must not be negative")
    if getattr(args, "rcvbuf", 0) < 0:
        raise UsageError("--rcvbuf must not be negative")
    try:
        c = Conn(args.host, args.port, getattr(args, "rcvbuf", 0))
        c.tamper = getattr(args, "tamper", "")
        c.keepalive = getattr(args, "keepalive", "answer")
        c.report_disconnect = getattr(args, "report_disconnect", False)
        c.ident()
        if args.cmd == "kexwait":
            kexwait(c, args.hold, args.stage)
        else:
            c.kex()
            run_script(c, preauth)
            c.login(args.user, args.password)
            if args.cmd == "bench":
                bench(c, n)
            else:
                run_script(c, actions)
                time.sleep(args.hold)
        c.sock.close()
    except PeerExit as e:
        say(e.line)
        return e.code
    return 0


if __name__ == "__main__":
    sys.exit(main())
