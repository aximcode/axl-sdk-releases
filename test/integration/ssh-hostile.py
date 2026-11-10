#!/usr/bin/env python3
"""Hostile-input driver for test-ssh-hostile-qemu.sh.

Speaks just enough SSH to be RUDE. No crypto: every scenario here is refused
by the server before key exchange, and the point is what the server does
AFTERWARDS -- specifically, whether it is still alive.

ssh(1) is the conformance oracle for the happy path (test-ssh-transport-qemu.sh)
and it is useless here, because it never sends anything malformed. That gap is
how a pre-auth remote denial of service shipped: an active graceful close from
inside a receive callback wedged the whole event loop, and the only integration
test drove one successful session, which takes the passive-close path.
"""
from __future__ import annotations

import argparse
import select
import signal
import socket
import sys
import time

BAD_IDENT = b"SSH-1.99-nope\r\n"
GOOD_IDENT = b"SSH-2.0-hostile_1.0\r\n"


def connect(host: str, port: int, timeout: float = 5.0) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    return s


def scenario_bad_ident(host: str, port: int, count: int) -> None:
    """An identification line the server must refuse (SSH-1.99 is a downgrade)."""
    for _ in range(count):
        s = connect(host, port)
        try:
            s.sendall(BAD_IDENT)
            time.sleep(0.05)
        finally:
            s.close()


def scenario_bad_length(host: str, port: int, count: int) -> None:
    """A valid ident, then a packet_length no packet may carry."""
    for _ in range(count):
        s = connect(host, port)
        try:
            s.sendall(GOOD_IDENT)
            time.sleep(0.05)
            s.sendall(b"\xff\xff\xff\xff" + b"\x00" * 16)
            time.sleep(0.05)
        finally:
            s.close()


def scenario_bad_padding(host: str, port: int, count: int) -> None:
    """A well-sized frame whose padding_length cannot fit inside it."""
    for _ in range(count):
        s = connect(host, port)
        try:
            s.sendall(GOOD_IDENT)
            time.sleep(0.05)
            # packet_length 12 -> 16 total, a multiple of 8; padding_length
            # 200 is larger than the packet, which RFC 4253 section 6 forbids.
            s.sendall(b"\x00\x00\x00\x0c" + b"\xc8" + b"\x00" * 11)
            time.sleep(0.05)
        finally:
            s.close()


def scenario_flood(host: str, port: int, count: int) -> None:
    """Hold many connections open at once, then drop them all.

    The server registers loop sources per connection; without a cap this
    exhausts AXL_MAX_SOURCES and starves every other consumer of that loop.
    """
    socks: list[socket.socket] = []
    try:
        for _ in range(count):
            try:
                socks.append(connect(host, port, timeout=2.0))
            except OSError:
                # A server that REFUSES over its cap is behaving correctly;
                # the assertion is liveness afterwards, not acceptance here.
                break
        time.sleep(1.0)
    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass
    print(f"held {len(socks)} concurrent connections", file=sys.stderr)


def scenario_alive(host: str, port: int, _count: int) -> None:
    """Prove the listener still accepts and still speaks first.

    The server sends its identification line unprompted, so receiving it is
    proof the loop is running -- not merely that the TCP port is still bound,
    which a wedged guest would also show.
    """
    s = connect(host, port, timeout=10.0)
    try:
        data = b""
        deadline = time.time() + 10.0
        while b"\r\n" not in data and time.time() < deadline:
            chunk = s.recv(256)
            if not chunk:
                break
            data += chunk
        if not data.startswith(b"SSH-2.0-AxlSsh"):
            print(f"FAIL: no identification line, got {data!r}", file=sys.stderr)
            sys.exit(1)
        print(data.split(b"\r\n")[0].decode("ascii", "replace"))
    finally:
        s.close()


def first_answer(s: socket.socket, wait: float) -> tuple[str, bytes]:
    """What the server does first on a fresh connection, within @wait.

    "ident" -- it spoke (an identification line, or any byte at all);
    "closed" -- it ended the connection without a byte, by FIN or RST;
    "silent" -- neither happened in time.
    """
    s.settimeout(wait)
    try:
        data = s.recv(256)
    except socket.timeout:
        return "silent", b""
    except OSError:
        return "closed", b""
    return ("ident", data) if data else ("closed", b"")


def scenario_knock(host: str, port: int, count: int,
                   deadline_ms: int = 0) -> None:
    """Connect up to @count times, ONE AT A TIME, and classify each answer.
    With @deadline_ms, start no new knock once that much time has passed.

    Sequential on purpose. A burst can overrun the guest's accept backlog,
    and a connection the firmware turns away never reaches on_accept -- so a
    burst cannot tell "refused by the penalty" from "never accepted". One at
    a time, the listener is always armed when a connection arrives, so a
    connection that closes without a byte is the server's refusal. The
    latency of each answer is the cost of one refusal as the peer sees it.
    """
    counts = {"ident": 0, "closed": 0, "silent": 0, "error": 0}
    worst = 0.0
    each: list[float] = []
    start = time.monotonic()
    done = 0
    for _ in range(count):
        if deadline_ms and (time.monotonic() - start) * 1000 >= deadline_ms:
            break
        done += 1
        t0 = time.monotonic()
        try:
            s = connect(host, port, timeout=5.0)
        except OSError:
            counts["error"] += 1
            continue
        try:
            verdict, _ = first_answer(s, 5.0)
        finally:
            s.close()
        counts[verdict] += 1
        each.append(time.monotonic() - t0)
        worst = max(worst, each[-1])
    total = time.monotonic() - start
    median = sorted(each)[len(each) // 2] if each else 0.0
    slowest = max(range(len(each)), key=lambda i: each[i]) + 1 if each else 0
    print(f"knock: count={done} closed={counts['closed']} ident={counts['ident']}"
          f" silent={counts['silent']} error={counts['error']}"
          f" median_ms={int(median * 1000)} worst_ms={int(worst * 1000)}"
          f" worst_at={slowest} total_ms={int(total * 1000)}")


def scenario_hold(host: str, port: int, count: int) -> None:
    """Take the identification line, then say NOTHING until the server ends
    the connection -- a login grace running out with no attempt made. @count
    is the longest to wait, in seconds.
    """
    s = connect(host, port, timeout=5.0)
    try:
        verdict, data = first_answer(s, 10.0)
        if verdict != "ident" or not data.startswith(b"SSH-2.0-AxlSsh"):
            print(f"hold: no identification line ({verdict})")
            return
        t0 = time.monotonic()
        verdict, _ = first_answer(s, float(count))
        waited = int((time.monotonic() - t0) * 1000)
        # Anything after the ident is the rest of our own KEXINIT's peer --
        # the server's KEXINIT -- which is not an ending; keep reading.
        while verdict == "ident":
            left = count - (time.monotonic() - t0)
            if left <= 0:
                verdict = "silent"
                break
            verdict, _ = first_answer(s, left)
            waited = int((time.monotonic() - t0) * 1000)
        print(f"hold: {verdict} after_ms={waited}")
    finally:
        s.close()


def read_ident(s: socket.socket, wait: float) -> bool:
    """True once the server's identification line has arrived on @s within
    @wait; False if it closed, reset or said nothing first."""
    data = b""
    deadline = time.monotonic() + wait
    while b"\r\n" not in data:
        left = deadline - time.monotonic()
        if left <= 0:
            return False
        s.settimeout(left)
        try:
            chunk = s.recv(256)
        except OSError:
            return False
        if not chunk:
            return False
        data += chunk
    return data.startswith(b"SSH-")


def scenario_silent(host: str, port: int, count: int, hold: float) -> None:
    """Hold @count connections that never say a word, for @hold seconds, and
    report which ones the SERVER closed meanwhile.

    Opened ONE AT A TIME, each identified before the next is opened, so the
    open order is the server's accept order -- which is what makes an
    eviction order observable: none of them ever speaks, so all are equally
    far along in key exchange and the server's tie-break -- the OLDEST goes
    first -- closes index 0 first. A connection that gets no
    identification line was refused at accept (nothing is sent before one).

    Prints `silent: opened N` to stderr once every connection has been tried,
    so a test can act while they are held; stdout carries exactly one line,
    at the end:

        silent: held=H refused=R closed=C closed_idx=I0,I1,...

    H identified, R refused before identifying, C of the H closed by the
    server during the hold (a read returning b"" or raising a reset), and
    their 0-based open-order indices ascending, or `closed_idx=-` for none.
    The server's KEXINIT arriving after the ident is NOT a close; it is read
    and discarded, as scenario_hold does.
    """
    held: dict[int, socket.socket] = {}
    refused = 0
    closed: list[int] = []
    try:
        for i in range(count):
            try:
                s = connect(host, port, timeout=5.0)
            except OSError:
                refused += 1
                continue
            if read_ident(s, 10.0):
                s.setblocking(False)
                held[i] = s
            else:
                refused += 1
                s.close()
        print(f"silent: opened {count}", file=sys.stderr, flush=True)
        identified = len(held)
        deadline = time.monotonic() + hold
        while held and (left := deadline - time.monotonic()) > 0:
            ready, _, _ = select.select(list(held.values()), [], [], left)
            for s in ready:
                idx = next(k for k, v in held.items() if v is s)
                try:
                    gone = s.recv(4096) == b""
                except (BlockingIOError, InterruptedError):
                    gone = False
                except OSError:
                    gone = True
                if gone:
                    closed.append(idx)
                    del held[idx]
                    s.close()
    finally:
        for s in held.values():
            s.close()
    idx_s = ",".join(str(i) for i in sorted(closed)) if closed else "-"
    print(f"silent: held={identified} refused={refused} closed={len(closed)}"
          f" closed_idx={idx_s}", flush=True)


def scenario_churn(host: str, port: int, width: int, hold: float,
                   duration: float) -> None:
    """A SUSTAINED flood: keep @width silent connections going for @duration
    seconds, closing each @hold seconds after it connected and starting its
    replacement at once. None ever speaks or reads, so every one sits in
    ST_IDENT -- the cheapest flood there is.

    Connects are NON-BLOCKING, each slot independent. QEMU's user network
    listens with a tiny backlog and a SYN it drops is retried by the kernel
    a second later; one blocking connect per slot in turn let every such
    retry stall the whole flood (measured: a 24-wide, 1 s flood offered 9 a
    second instead of 24). A connect still pending after 2 s is counted as
    failed and replaced.

    Prints `churn: started` to stderr once the first round of connects has
    been started, so a test can act while the flood runs, and exactly one
    line to stdout at the end -- which SIGTERM brings forward, so a test can
    stop the flood the moment it has what it needs and still get the line:

        churn: opened=N failed=F seconds=S rate=R/s slowest_ms=M

    N connects that completed, F that failed or timed out, over S seconds of
    flooding; R is N / S, one decimal; M the longest a completed connect took.
    Behind QEMU's user network a connect completes whether or not the guest
    ever accepts it (slirp answers it), so R is the rate OFFERED to the
    guest; what the guest did with it is in its own log.
    """
    # Per slot: (socket, started, connected). connected is 0.0 while pending.
    slots: list[tuple[socket.socket, float, float] | None] = [None] * width
    opened = 0
    failed = 0
    slowest = 0.0
    stop = False
    start = time.monotonic()
    end = start + duration

    def on_term(_sig: int, _frame: object) -> None:
        nonlocal stop
        stop = True

    def begin(i: int) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setblocking(False)
        s.connect_ex((host, port))
        slots[i] = (s, time.monotonic(), 0.0)

    signal.signal(signal.SIGTERM, on_term)
    try:
        for i in range(width):
            begin(i)
        print("churn: started", file=sys.stderr, flush=True)
        while not stop and time.monotonic() < end:
            pending = [sl[0] for sl in slots if sl is not None and not sl[2]]
            live = [sl[2] for sl in slots if sl is not None and sl[2]]
            now = time.monotonic()
            wake = min([t + hold for t in live] + [now + 0.05, end])
            _, writable, _ = select.select([], pending, [],
                                           max(0.0, wake - now))
            now = time.monotonic()
            for i, sl in enumerate(slots):
                if sl is None:
                    begin(i)
                    continue
                s, t0, t_conn = sl
                if t_conn:
                    if now - t_conn >= hold:
                        s.close()
                        begin(i)
                    continue
                if s in writable:
                    if s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) == 0:
                        opened += 1
                        slowest = max(slowest, now - t0)
                        slots[i] = (s, t0, now)
                        continue
                    failed += 1
                    s.close()
                    begin(i)
                elif now - t0 >= 2.0:
                    failed += 1
                    s.close()
                    begin(i)
    finally:
        for sl in slots:
            if sl is not None:
                sl[0].close()
    secs = time.monotonic() - start
    print(f"churn: opened={opened} failed={failed} seconds={secs:.1f}"
          f" rate={opened / secs:.1f}/s slowest_ms={int(slowest * 1000)}",
          flush=True)


# --- the disconnect linger (AxlSsh P5) ---------------------------------------
#
# Every linger scenario makes the server end the connection with a DISCONNECT
# the same way: a valid identification line, then a packet length no packet
# may carry, which the server answers with SSH_MSG_DISCONNECT ("protocol
# error") and then waits, up to AXL_SSH_DISCONNECT_LINGER_MS, for the peer to
# hang up. Everything the server sends before key exchange is plaintext, so
# this peer can read the DISCONNECT without any crypto.
PROVOKE = GOOD_IDENT + b"\xff\xff\xff\xff" + b"\x00" * 16


def plain_disconnect(data: bytes) -> tuple[int, str] | None:
    """The (reason, text) of the first plaintext DISCONNECT in @data -- the
    server's identification line, its KEXINIT, then the DISCONNECT -- or
    None if it has not arrived (whole) yet."""
    end = data.find(b"\r\n")
    if end < 0:
        return None
    off = end + 2
    while len(data) - off >= 5:
        plen = int.from_bytes(data[off:off + 4], "big")
        if len(data) - off - 4 < plen:
            return None
        pad = data[off + 4]
        payload = data[off + 5:off + 4 + plen - pad]
        off += 4 + plen
        if payload[:1] == b"\x01":
            code = int.from_bytes(payload[1:5], "big")
            n = int.from_bytes(payload[5:9], "big")
            return code, payload[9:9 + n].decode("utf-8", "replace")
    return None


def linger_peer(host: str, port: int, mode: str, hold: float) -> None:
    """Provoke a DISCONNECT, then behave as @mode says, and report.

    norecv   read everything, never close: the server must reset at its bound
    flood    the same, writing junk the whole time: the deadline must not move
    close    close 200 ms after the DISCONNECT has been read whole
    earlyfin provoke and half-close IN ONE GO, then read: the FIN reaches the
             server before its DISCONNECT can have been read

    Prints `linger: disconnect CODE "TEXT"` once the DISCONNECT has arrived
    (or `linger: no disconnect`), then `linger: ended after S.SSS s`,
    measured from the provoking write to the server's close (FIN or RST) --
    or, for close, `linger: closed by us`.
    """
    s = connect(host, port, timeout=hold + 5.0)
    data = b""
    seen: tuple[int, str] | None = None
    try:
        if mode == "earlyfin":
            s.sendall(PROVOKE)
            s.shutdown(socket.SHUT_WR)
        else:
            s.sendall(PROVOKE)
        t0 = time.monotonic()
        s.setblocking(False)
        junk = b"\x00" * 512
        ended: float | None = None
        while time.monotonic() - t0 < hold:
            wfds = [s] if mode == "flood" else []
            ready_r, ready_w, _ = select.select([s], wfds, [], 0.05)
            if ready_w:
                try:
                    s.send(junk)
                except OSError:
                    pass
            if ready_r:
                try:
                    chunk = s.recv(65536)
                except OSError:
                    chunk = b""
                if not chunk:
                    ended = time.monotonic() - t0
                    break
                data += chunk
                if seen is None:
                    seen = plain_disconnect(data)
                    if seen is not None:
                        print(f'linger: disconnect {seen[0]} "{seen[1]}"', flush=True)
                        if mode == "close":
                            # Not at once: 200 ms, well inside the bound,
                            # lets the server see its send complete first,
                            # so the hang-up is one it can call delivered.
                            time.sleep(0.2)
                            print("linger: closed by us", flush=True)
                            return
        if seen is None:
            print("linger: no disconnect", flush=True)
        if ended is None:
            print(f"linger: still open after {hold:.0f} s", flush=True)
        else:
            print(f"linger: ended after {ended:.3f} s", flush=True)
    finally:
        s.close()


def scenario_linger_hold(host: str, port: int, count: int, hold: float) -> None:
    """@count connections, each provoked into a DISCONNECT at once and then
    held open, reading nothing, for @hold seconds -- @count lingering
    connections at the same moment -- and ONE more connection made the
    moment they are provoked, inside their linger second, whose first answer
    is printed: `linger-hold: knock ident|closed|silent`."""
    socks: list[socket.socket] = []
    bufs: dict[int, bytes] = {}
    try:
        # One at a time, each waited for until the SERVER speaks: QEMU's user
        # network accepts on the host at once and connects to the guest
        # later, and sixteen at once overran the guest's accept backlog --
        # four were served only when this closed them.
        for _ in range(count):
            s = connect(host, port)
            socks.append(s)
            bufs[id(s)] = b""
            while b"\r\n" not in bufs[id(s)]:
                chunk = s.recv(4096)
                if not chunk:
                    break
                bufs[id(s)] += chunk
        for s in socks:
            s.sendall(PROVOKE)
        print(f"linger-hold: {len(socks)} provoked", flush=True)
        # Knock only once EVERY one has its DISCONNECT -- each is then
        # lingering. Knocking after a fixed pause instead raced a loaded
        # server still reading the provocations: admission then evicted a
        # connection that had not been provoked yet, and no lingerer.
        # Reading them does not end their linger; only a close would.
        waiting = list(socks)
        deadline = time.monotonic() + 0.8
        while waiting and time.monotonic() < deadline:
            ready, _, _ = select.select(waiting, [], [], 0.05)
            for s in ready:
                try:
                    chunk = s.recv(65536)
                except OSError:
                    chunk = b""
                bufs[id(s)] += chunk
                if not chunk or plain_disconnect(bufs[id(s)]) is not None:
                    waiting.remove(s)
        print(f"linger-hold: {len(socks) - len(waiting)} disconnected",
              flush=True)
        k = connect(host, port)
        try:
            verdict, _ = first_answer(k, 5.0)
        finally:
            k.close()
        print(f"linger-hold: knock {verdict}", flush=True)
        time.sleep(hold)
    finally:
        for s in socks:
            s.close()


SCENARIOS = {
    "knock": scenario_knock,
    "hold": scenario_hold,
    "bad-ident": scenario_bad_ident,
    "bad-length": scenario_bad_length,
    "bad-padding": scenario_bad_padding,
    "flood": scenario_flood,
    "alive": scenario_alive,
}
# Not in SCENARIOS: they take --hold (and churn --duration) as well, so each
# has a signature of its own.
EXTRA_SCENARIOS = ("silent", "churn", "linger-norecv", "linger-flood",
                   "linger-close", "linger-earlyfin", "linger-hold")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scenario",
                    choices=sorted([*SCENARIOS, *EXTRA_SCENARIOS]))
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--count", type=int, default=5)
    ap.add_argument("--deadline-ms", type=int, default=0,
                    help="knock only: start no new knock after this long")
    ap.add_argument("--hold", type=float, default=30.0,
                    help="silent, churn, linger-*: seconds to hold each"
                         " connection")
    ap.add_argument("--duration", type=float, default=10.0,
                    help="churn only: seconds to keep the flood going")
    args = ap.parse_args()
    if args.scenario == "knock":
        scenario_knock(args.host, args.port, args.count, args.deadline_ms)
    elif args.scenario == "silent":
        scenario_silent(args.host, args.port, args.count, args.hold)
    elif args.scenario.startswith("linger-") and args.scenario != "linger-hold":
        linger_peer(args.host, args.port, args.scenario[len("linger-"):],
                    args.hold)
    elif args.scenario == "linger-hold":
        scenario_linger_hold(args.host, args.port, args.count, args.hold)
    elif args.scenario == "churn":
        scenario_churn(args.host, args.port, args.count, args.hold,
                       args.duration)
    else:
        SCENARIOS[args.scenario](args.host, args.port, args.count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
