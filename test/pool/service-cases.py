#!/usr/bin/env python3
"""service-cases.py -- axlpool.service's wire protocol, against real guests.

Usage: service-cases.py ARCH AGENT_EFI WORKDIR

Runs the service (serve()) on a socket in WORKDIR over a real pool, and
drives it with raw JSON lines: the contract in service.py's docstring.
Prints `  PASS:`/`  FAIL:` per case, then `service-cases: <p> passed, <f> failed`.
"""
from __future__ import annotations

import grp
import json
import os
import pwd
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, cast

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration" / "lib"))
from axlpool.klass import REPO  # noqa: E402
from axlpool.service import (CONN_CLOSE_WAIT_S, MAX_LINE_BYTES, Service,  # noqa: E402
                             code_identity, service_code_files)
from checks import Cases  # noqa: E402

TREE = str(Path(__file__).resolve().parents[2])
TRACE = "0af7651916cd43dd8448eb211c80319c"
NO_GUESTS: list[object] = []
NO_LEAKS: list[str] = []


class Conn:
    """One client connection speaking JSON lines."""

    def __init__(self, path: Path) -> None:
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(str(path))
        self.s.settimeout(400)
        self.buf = b""

    def send(self, obj: object) -> None:
        self.s.sendall(json.dumps(obj).encode() + b"\n")

    def raw(self, data: bytes) -> None:
        self.s.sendall(data)

    def recv(self) -> dict[str, Any]:
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                return {"eof": True}
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return cast("dict[str, Any]", json.loads(line))

    def call(self, obj: dict[str, object]) -> dict[str, Any]:
        self.send(obj)
        return self.recv()

    def close(self) -> None:
        self.s.close()


def pids_naming(d: Path) -> list[int]:
    """QEMU and virtiofsd processes with an argument under @d."""
    found: list[int] = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if args and (b"qemu-system" in args[0] or b"virtiofsd" in args[0]) \
                and any(f"{d}/".encode() in x for x in args):
            found.append(int(proc.name))
    return found


def _send_quietly(conn: Conn, data: bytes) -> None:
    """Send @data; the service closing the connection midway is expected."""
    try:
        conn.s.sendall(data)
    except OSError:
        pass


def process_cases(c: Cases, agent: Path, work: Path) -> None:
    """`python3 -m axlpool serve` as a process: SIGTERM, and the idle exit."""
    pool_dir = str(Path(__file__).resolve().parent)
    spawned: list[subprocess.Popen[str]] = []
    try:
        _process_cases(c, agent, work, pool_dir, spawned)
    finally:
        for proc in spawned:          # a case that raised leaves no service behind
            if proc.poll() is None:
                proc.kill()
                proc.wait()

    # The socket's directory, by OpenSSH's safe_path rule: what the service
    # creates is 0700 (whatever the umask); an existing directory is never
    # chmodded; the directory must be ours and not writable by others -- a
    # user-private group excepted -- and every ancestor root's or ours, and
    # not writable by others unless sticky. Else someone could replace the
    # socket under the clients.
    def dir_case(name: str, mode: int | None, sub: str = "") -> tuple[str, int | None]:
        d = work / name
        if mode is not None:
            d.mkdir()
            d.chmod(mode)
        sock_dir = d / sub if sub else d
        old_umask = os.umask(0o022)     # so a created 0700 is the service's doing
        try:
            svc = Service(Path(TREE), agent, work / f"{name}-state", sock_dir / "pool.sock", exit_idle=0)
        except RuntimeError as ex:
            return str(ex), sock_dir.stat().st_mode & 0o7777 if sock_dir.exists() else None
        finally:
            os.umask(old_umask)
        svc.stop()
        svc.run()                     # stopped before running: returns at once, closes all
        return "started", sock_dir.stat().st_mode & 0o7777
    c.check("socket dir created by the service: 0700", dir_case("sd-new", None), ("started", 0o700))
    c.check("socket dirs created by the service, missing parents too: each 0700",
            (dir_case("sd-deep", None, "a/b"), (work / "sd-deep").stat().st_mode & 0o7777,
             (work / "sd-deep" / "a").stat().st_mode & 0o7777),
            (("started", 0o700), 0o700, 0o700))
    c.check("an existing socket dir only we can write: used as is, never chmodded",
            dir_case("sd-home", 0o755), ("started", 0o755))
    c.check("an existing socket dir others can write: refused, left as is",
            dir_case("sd-shared", 0o1777),
            (f"refusing {work / 'sd-shared'} for the socket: others can write it", 0o1777))
    c.check("an ancestor others can write (not sticky): refused, nothing created under it",
            dir_case("sd-open", 0o777, "inner"),
            (f"refusing {work / 'sd-open'} for the socket: others can write it", None))
    c.check("an ancestor others can write but sticky (like /tmp): fine, the dir created 0700",
            dir_case("sd-sticky", 0o1777, "inner"), ("started", 0o700))
    private = grp.getgrgid(os.getgid()).gr_mem in ([], [pwd.getpwuid(os.getuid()).pw_name])
    if private:                       # RHEL's user-private groups: umask 002 makes 0775
        c.check("a group-writable socket dir of the user's PRIVATE group: used as is",
                dir_case("sd-upg", 0o775), ("started", 0o775))
    else:
        print("  SKIP: this user's primary group is shared: no user-private-group case")

    # A LIVE socket at the path with no service lock on it -- systemd's own,
    # while the service it activates has idle-exited -- is refused, never
    # unlinked: clients would get ENOENT until the socket unit restarted.
    foreign = work / "fg" / "pool.sock"
    foreign.parent.mkdir(mode=0o700)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(foreign))
    listener.listen(1)
    try:
        Service(Path(TREE), agent, work / "fg-state", foreign, exit_idle=0)
        refused = "started"
    except RuntimeError as ex:
        refused = str(ex)
    c.check("a live socket with no service lock (systemd's): refused, left in place",
            (refused, foreign.is_socket()),
            (f"a live socket is at {foreign} (systemd's axlpool.socket?): not replacing it", True))
    listener.close()

    def start_stop(sock_path: Path, name: str) -> str:
        """A Service on @sock_path started and stopped at once, or why not."""
        try:
            svc = Service(Path(TREE), agent, work / f"{name}-state", sock_path, exit_idle=0)
        except RuntimeError as ex:
            return str(ex)
        svc.stop()
        svc.run()
        return "started"

    # A STALE socket file (its service crashed: nothing listens) is replaced.
    stale = work / "st" / "pool.sock"
    stale.parent.mkdir(mode=0o700)
    dead = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    dead.bind(str(stale))
    dead.close()                                  # the file stays; nobody listens
    c.check("a stale socket file (nothing listens): replaced, the service starts",
            start_stop(stale, "st"), "started")
    # Anything at the path that is NOT a socket is refused, never deleted.
    notsock = work / "ns" / "pool.sock"
    notsock.parent.mkdir(mode=0o700)
    notsock.write_text("someone's file\n")
    c.check("a regular file at the socket path: refused, left as is",
            (start_stop(notsock, "ns"), notsock.read_text()),
            (f"{notsock} exists and is not a socket: not replacing it", "someone's file\n"))

    # stop() before run() is not lost: run() returns at once.
    early = Service(Path(TREE), agent, work / "early-state", work / "early.sock", exit_idle=0)
    early.stop()
    te = threading.Thread(target=early.run, daemon=True)
    te.start()
    te.join(30)
    c.check("stop() before run(): run() returns at once, not on the next signal", te.is_alive(), False)


def _process_cases(c: Cases, agent: Path, work: Path, pool_dir: str,
                   spawned: list[subprocess.Popen[str]]) -> None:

    def serve(name: str, *extra: str) -> tuple[subprocess.Popen[str], Path]:
        sk = work / f"{name}.sock"
        proc = subprocess.Popen(
            [sys.executable, "-m", "axlpool", "serve", "--udp-echo-port", "0", "--tree", TREE, "--agent", str(agent),
             "--state", str(work / f"{name}-state"), "--socket", str(sk), *extra],
            cwd=pool_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        spawned.append(proc)
        for _ in range(200):
            if sk.exists() or proc.poll() is not None:
                break
            time.sleep(0.05)
        return proc, sk

    def bodies(out: str) -> list[str]:
        got: list[str] = []
        for line in out.splitlines():
            try:
                got.append(str(json.loads(line).get("body")))
            except ValueError:
                got.append(line)
        return got

    proc, sk = serve("term", "--exit-idle", "0")
    conn = Conn(sk)
    r = conn.call({"op": "hello", "id": "1", "version": 2, "client": "term"})
    # A start refused (here: another service owns the socket) exits 3 -- the
    # code the systemd unit does NOT restart on, so it cannot loop until
    # systemd stops listening.
    second = subprocess.run(
        [sys.executable, "-m", "axlpool", "serve", "--udp-echo-port", "0", "--tree", TREE, "--agent", str(agent),
         "--state", str(work / "term2-state"), "--socket", str(sk), "--exit-idle", "0"],
        cwd=pool_dir, capture_output=True, text=True, timeout=60, check=False)
    c.check("serve: a refused start exits 3, saying why",
            (second.returncode, second.stderr.strip()), (3, f"axlpool serve: another service owns {sk}"))
    proc.send_signal(signal.SIGTERM)
    try:
        out, _ = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
    c.check("serve: SIGTERM ends the service cleanly -- its client sees EOF, exit 0, logged",
            (r.get("ok"), conn.recv().get("eof"), proc.returncode, bodies(out)),
            (True, True, 0, ["started", "closed", "exiting"]))
    conn.close()

    def activation() -> None:
        # Socket activation (systemd's LISTEN_FDS): the service answers on the
        # socket it was handed, binds and locks nothing of its own, and the
        # guests it starts inherit neither the LISTEN_* variables nor the
        # listening socket.
        act, own = work / "act.sock", work / "own.sock"
        proc = subprocess.Popen(
            ["systemd-socket-activate", "-l", str(act), sys.executable, "-m", "axlpool", "serve", "--udp-echo-port", "0",
             "--tree", TREE, "--agent", str(agent), "--state", str(work / "act-state"),
             "--socket", str(own), "--exit-idle", "0"],
            cwd=pool_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        spawned.append(proc)
        for _ in range(200):
            if act.exists():
                break
            time.sleep(0.05)
        conn = Conn(act)
        r = conn.call({"op": "hello", "id": "1", "version": 2, "client": "act"})
        lr = conn.call({"op": "lease", "id": "2", "tree": TREE, "class": "standard", "arch": "X64",
                        "timeout": 300})
        listener = os.readlink(f"/proc/{proc.pid}/fd/3")
        leaks: list[str] = []
        guest_pids = pids_naming(work / "act-state")
        for pid in guest_pids:
            env = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
            leaks += [f"{pid} env {v.decode()}" for v in env if v.startswith(b"LISTEN_")]
            fd_dir = Path(f"/proc/{pid}/fd")
            leaks += [f"{pid} fd {f.name}" for f in fd_dir.iterdir()
                      if os.readlink(f) == listener]
        proc.send_signal(signal.SIGTERM)
        try:
            proc.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
        c.check("socket-activated: answers on systemd's socket, a guest leased on it",
                (r.get("ok"), lr.get("ok"), listener.startswith("socket:")), (True, True, True))
        c.check("socket-activated: its guests (some running) inherit no LISTEN_* and not the "
                "listening socket", (len(guest_pids) > 0, leaks), (True, NO_LEAKS))
        # Activated but refused (another pool owns the state): the waiting client
        # is ANSWERED before the exit -- with Accept=no an unanswered connection
        # stays queued and systemd would start the service again, and again,
        # until it gave up listening.
        held = Service(Path(TREE), agent, work / "held-state", work / "held.sock", exit_idle=0)
        try:
            act2 = work / "act2.sock"
            proc2 = subprocess.Popen(
                ["systemd-socket-activate", "-l", str(act2), sys.executable, "-m", "axlpool", "serve", "--udp-echo-port", "0",
                 "--tree", TREE, "--agent", str(agent), "--state", str(work / "held-state"),
                 "--socket", str(work / "own2.sock"), "--exit-idle", "0"],
                cwd=pool_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            spawned.append(proc2)
            for _ in range(200):
                if act2.exists():
                    break
                time.sleep(0.05)
            waiting = Conn(act2)
            waiting.s.settimeout(30)
            try:
                ans = waiting.recv()
            except OSError as ex:                     # unanswered: reset, or a timeout
                ans = {"unanswered": type(ex).__name__}
            try:
                out2, _ = proc2.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                proc2.kill()
                out2, _ = proc2.communicate()
            c.check("socket-activated and refused: the waiting client is answered `unavailable`, exit 3",
                    (ans.get("ok"), ans.get("error"), proc2.returncode),
                    (False, "unavailable", 3))
            print(f"    ({out2.strip()[-200:]})")
            waiting.close()
        finally:
            held.stop()
            held.run()

        c.check("socket-activated: binds and locks nothing of its own; exits 0 on SIGTERM",
                (own.exists(), Path(f"{own}.lock").exists(), proc.returncode), (False, False, 0))
        conn.close()

    if shutil.which("systemd-socket-activate"):
        activation()
    else:                             # balanced: one SKIP per check the block makes
        for what in ("answers on systemd's socket", "guests inherit no LISTEN_*",
                     "refused start answers `unavailable`", "binds and locks nothing of its own"):
            print(f"  SKIP: socket-activated: {what} (no systemd-socket-activate)")

    proc, sk = serve("idle", "--exit-idle", "1")
    try:
        out, _ = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
    c.check("serve --exit-idle 1: with no client and no guest it exits by itself",
            (proc.returncode, bodies(out)), (0, ["started", "closed", "exiting"]))
    c.check("serve: an unknown option is a usage error (exit 2)",
            subprocess.run([sys.executable, "-m", "axlpool", "serve", "--udp-echo-port", "0", "--frob"], cwd=pool_dir,
                           capture_output=True, env={**os.environ}).returncode, 2)


def _udp_ask(port: int, payload: bytes, wait: float = 0.5) -> bytes | None:
    """One datagram to 127.0.0.1:@port; the reply, or None within @wait s."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(wait)
    try:
        u.sendto(payload, ("127.0.0.1", port))
        return u.recvfrom(65536)[0]
    except OSError:
        return None
    finally:
        u.close()


# The `serve` runs above and below pass --udp-echo-port 0: their exact log
# lists must not depend on whether this host already has the unit suite's
# echo port bound (the user's own service does).
def _hold(port: int, reuse: bool) -> socket.socket:
    """A UDP socket bound on 127.0.0.1:@port -- with SO_REUSEPORT (@reuse), as
    echo-stream.py --udp binds it for a booted run, or without."""
    h = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if reuse:
        h.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        h.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    h.bind(("127.0.0.1", port))
    return h


def _free_port() -> int:
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    port = int(probe.getsockname()[1])
    probe.close()
    return port


def udp_echo(c: Cases, agent: Path, work: Path) -> None:
    """The service serves the unit suite's UDP echo for its whole life: the
    guests send to 10.0.2.2:<port> (the host's loopback), and an echo owned
    by ONE test run could be killed by its cleanup while another run's
    AxlTestNet still used it. It binds with SO_REUSEPORT, as a booted run's
    echo-stream.py does, so the two SHARE the port instead of locking each
    other out (Linux lets a port be shared only when every holder set it).
    A private free port here; the CLI's default is the binaries' pinned one."""
    def serve(name: str, port: int, logs: list[dict[str, object]]) -> tuple[Service, threading.Thread]:
        svc = Service(Path(TREE), agent, work / f"{name}-state", work / f"{name}.sock", exit_idle=0,
                      udp_echo_port=port, log=logs.append)
        t = threading.Thread(target=svc.run, daemon=True)
        t.start()
        for _ in range(100):
            if any(e.get("body") == "started" for e in logs):
                break
            time.sleep(0.05)
        return svc, t

    port = _free_port()
    c.check("udp echo: control -- nothing answers the port before the service", _udp_ask(port, b"x"), None)
    logs: list[dict[str, object]] = []
    svc, t = serve("udp", port, logs)
    try:
        c.check("udp echo: the running service echoes a datagram back",
                _udp_ask(port, b"axl-udp-echo", wait=2.0), b"axl-udp-echo")
    finally:
        svc.stop()
        t.join(60)
    try:
        _hold(port, reuse=False).close()
        freed = True
    except OSError:
        freed = False
    c.check("udp echo: and releases the port when the service stops (a plain bind succeeds)", freed, True)

    # A booted run's echo (SO_REUSEPORT) already on the port: the service
    # still binds and serves -- they share it.
    port = _free_port()
    booted = _hold(port, reuse=True)
    logs = []
    svc, t = serve("udp-shared", port, logs)
    try:
        c.check("udp echo: beside a booted run's reuseport echo, the service binds too (no 'not served')",
                [e.get("body") for e in logs if e.get("body") == "udp echo not served"], [])
    finally:
        svc.stop()
        t.join(60)
        booted.close()

    # A holder that did NOT set SO_REUSEPORT: the service cannot bind, says so
    # at WARN, and serves leases anyway.
    port = _free_port()
    plain = _hold(port, reuse=False)
    logs = []
    svc, t = serve("udp-taken", port, logs)
    try:
        c.check("udp echo: a port held without SO_REUSEPORT is a WARN, and the service still starts",
                ([e.get("severity_text") for e in logs if e.get("body") == "udp echo not served"],
                 any(e.get("body") == "started" for e in logs)),
                (["WARN"], True))
    finally:
        svc.stop()
        t.join(60)
        plain.close()


def code_cases(c: Cases, work: Path) -> None:
    """code_identity: a digest of each file's path and bytes, so any edit,
    rename, addition or removal changes it and nothing else does; and the
    files are the pool code the process runs, the agent client included."""
    root = work / "code-id"
    (root / "pkg").mkdir(parents=True)
    a, b = root / "pkg" / "a.py", root / "pkg" / "b.py"
    a.write_text("x = 1\n")
    b.write_text("y = 2\n")
    base = code_identity(root, [a, b])
    same = code_identity(root, [b, a])
    a.write_text("x = 2\n")
    edited = code_identity(root, [a, b])
    a.write_text("x = 1\n")
    a.rename(root / "pkg" / "c.py")
    renamed = code_identity(root, [root / "pkg" / "c.py", b])
    (root / "pkg" / "c.py").rename(a)
    d = root / "pkg" / "d.py"
    d.write_text("")
    added = code_identity(root, [a, b, d])
    c.check("code_identity: order-free; an edit, a rename and an added file each change it",
            (len(base), same == base, edited != base, renamed != base, added != base,
             code_identity(root, [a, b]) == base),
            (64, True, True, True, True, True))
    files = {str(p.relative_to(REPO)) for p in service_code_files(REPO)}
    c.check("service_code_files: the pool package and the agent client it imports",
            ({"test/pool/axlpool/service.py", "test/pool/axlpool/pool.py",
              "test/pool/axlpool/guest.py", "test/integration/lib/axl_agent.py"} <= files,
             any(f.startswith(("/", "..")) for f in files)),
            (True, False))


def main() -> int:
    arch, agent, work = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    c = Cases()
    udp_echo(c, agent, work)
    sock = work / "pool.sock"
    logs: list[dict[str, object]] = []
    # The service's PINNED agent is a copy, so a lease of the tree's own
    # agent ("agent": "tree") is told apart by path.
    pinned = work / "pinned-agent.efi"
    shutil.copy(agent, pinned)
    # Pinned for THIS arch only: a lease for the other arch has no agent.
    svc = Service(Path(TREE), {arch: pinned}, work / "state", sock, exit_idle=0, log=logs.append)
    other_arch = "AARCH64" if arch == "X64" else "X64"
    t = threading.Thread(target=svc.run, daemon=True)
    t.start()
    for _ in range(100):
        if sock.exists():
            break
        time.sleep(0.05)
    try:
        a = Conn(sock)
        r = a.call({"op": "ping", "id": "0"})
        c.check("hello comes first: anything else is bad-request",
                (r.get("ok"), r.get("error"), r.get("id")), (False, "bad-request", "0"))
        # One service per socket, one pool per state directory: a second
        # start is refused, and the live service keeps answering.
        for what, st, sk, want in (
                ("a second service on a live socket", work / "state2", sock,
                 f"another service owns {sock}"),
                ("a second service on a live state directory", work / "state", work / "other.sock",
                 f"another pool owns {work / 'state'}")):
            try:
                Service(Path(TREE), agent, st, sk, exit_idle=0)
                err = "not refused"
            except RuntimeError as ex:
                err = str(ex)
            c.check(f"{what} is refused", err, want)
        c.check("the live service still answers after both refusals",
                a.call({"op": "ping", "id": "p0"}), {"ok": False, "id": "p0", "error": "bad-request",
                                                     "detail": "the first request must be hello"})
        r = a.call({"op": "hello", "id": "1", "version": 2, "client": "service-cases"})
        agents = cast("dict[str, dict[str, Any]]", r.get("agents", {}))
        c.check("hello: version, instance, the pinned agent of each arch",
                (r.get("ok"), r.get("version"), isinstance(r.get("instance"), str),
                 sorted(agents), agents.get(arch, {}).get("path"),
                 len(str(agents.get(arch, {}).get("sha256", "")))),
                (True, 2, True, [arch], str(pinned), 64))
        # The code the service RUNS, as a digest taken at its start: a pooled
        # skip-cache key holds it, since the service may run other code than
        # the asking tree (an older start; the main checkout's copy).
        c.check("hello: the code identity it loaded at start (its `code`, 64 hex)",
                (r.get("code") == svc.code, bool(re.fullmatch(r"[0-9a-f]{64}", str(r.get("code"))))),
                (True, True))
        code_cases(c, work)
        r = a.call({"op": "lease", "id": "na", "tree": TREE, "class": "standard", "arch": other_arch,
                    "timeout": 5})
        c.check("a lease for an arch with no pinned agent: miss no-agent (nothing booted)",
                (r.get("ok"), r.get("error"), r.get("reason")), (False, "miss", "no-agent"))
        r = a.call({"op": "frobnicate", "id": "2"})
        c.check("an unknown op: unknown-op", (r.get("ok"), r.get("error")), (False, "unknown-op"))
        a.raw(b"{not json\n")
        r = a.recv()
        c.check("a line that is not JSON: bad-request", (r.get("ok"), r.get("error")), (False, "bad-request"))

        # Bad requests are answered (bad-request, the id echoed), never hung.
        for rid, req, what in (
                ("v1", {"timeout": "x"}, "a timeout that is not a number"),
                ("v2", {"tree": None}, "no tree"),
                ("v3", {"tree": "relative/tree"}, "a relative tree"),
                ("v4", {"class": None}, "no class"),
                ("v5", {"bind": "detached"}, "a bind other than connection"),
                ("v6", {"booted_after": "x"}, "a booted_after that is not a number")):
            base: dict[str, object] = {"op": "lease", "id": rid, "tree": TREE, "class": "standard",
                                       "arch": arch, "timeout": 5}
            base.update(req)
            base = {k: v for k, v in base.items() if v is not None}
            r = a.call(base)
            c.check(f"lease with {what}: bad-request, the id echoed",
                    (r.get("ok"), r.get("error"), r.get("id")), (False, "bad-request", rid))
        r = a.call({"op": "ping", "id": 3})
        c.check("an id that is not a string: bad-request", (r.get("ok"), r.get("error")),
                (False, "bad-request"))
        svc.allowed_uid = os.getuid() + 1          # as if this peer were another user
        try:
            other = Conn(sock)
            other.s.settimeout(10)
            try:
                got_other = ((lambda r: (r.get("ok"), r.get("error")))(other.recv()), other.recv().get("eof"))
            except TimeoutError:
                got_other = ("no answer within 10 s", None)
            c.check("a peer of another uid: permission, then the connection closes",
                    got_other, ((False, "permission"), True))
            other.close()
        finally:
            svc.allowed_uid = os.getuid()
        vc = Conn(sock)
        r = vc.call({"op": "hello", "id": "h", "version": True, "client": "v"})
        c.check("hello with version true (not 2): unsupported-version", (r.get("ok"), r.get("error")),
                (False, "unsupported-version"))
        r = vc.call({"op": "hello", "id": "h1", "version": 1, "client": "v1"})
        c.check("hello from a version-1 client (one `agent`, not `agents`): unsupported-version",
                (r.get("ok"), r.get("error")), (False, "unsupported-version"))
        vc.close()

        r = a.call({"op": "lease", "id": "3", "tree": TREE, "class": "standard", "arch": arch,
                    "timeout": 300, "traceparent": f"00-{TRACE}-b7ad6b7169203331-01"})
        lease = cast("dict[str, Any] | None", r.get("lease"))
        c.check("lease: a lease, its id carrying the instance",
                (r.get("ok"), isinstance(lease, dict) and str(lease.get("id", "")).count("-") >= 1),
                (True, True))
        assert isinstance(lease, dict)
        c.check("lease: the traceparent's trace id is the lease's", r.get("trace_id"), TRACE)
        c.check("lease: by default the service's pinned agent", lease.get("agent_image"), str(pinned))
        c.check("the pool's `leased` record names the SAME lease id and the trace id",
                [(x.get("axl.lease"), x.get("trace_id")) for x in logs if x.get("body") == "leased"],
                [(lease["id"], TRACE)])
        bad = a.call({"op": "release", "id": "4", "lease": lease["id"], "token": "forged"})
        c.check("release with a wrong token: permission", (bad.get("ok"), bad.get("error")),
                (False, "permission"))
        foreign = a.call({"op": "release", "id": "5", "lease": "otherinst-L1", "token": "x"})
        c.check("release of another instance's lease: unknown-lease",
                (foreign.get("ok"), foreign.get("error")), (False, "unknown-lease"))

        # Out of order: a lease still pending (its guest booting, ~11 s) does
        # not hold up a ping sent after it on the same connection; a cancel
        # answers it at once (the guest, once booted, joins the pool).
        a.send({"op": "lease", "id": "6", "tree": TREE, "class": "standard", "arch": arch,
                "timeout": 300})
        held_by_b = Conn(sock)
        held_by_b.call({"op": "hello", "id": "h", "version": 2, "client": "b"})
        r = a.call({"op": "ping", "id": "7"})
        c.check("a ping is answered while a lease waits", (r.get("id"), r.get("ok")), ("7", True))
        r = a.call({"op": "cancel", "id": "8", "target": "6"})
        got = [r, a.recv()]
        ids = sorted((str(x.get("id")), x.get("ok"), x.get("error")) for x in got)
        c.check("cancel: the pending lease answers cancelled; the cancel itself ok",
                ids, [("6", False, "cancelled"), ("8", True, None)])

        # restore: from ANOTHER connection (a test's `axlpool restore`, not
        # the wrapper holding the lease), authorised by the restore token.
        rtok = str(cast("dict[str, Any]", lease.get("env", {})).get("AXL_POOL_RESTORE_TOKEN", ""))
        c.check("the lease's env carries its restore token, the wire lease the same one",
                (len(rtok) >= 32, rtok == lease.get("restore_token")), (True, True))
        r = held_by_b.call({"op": "restore", "id": "r1", "lease": lease["id"], "restore_token": "forged"})
        c.check("restore with a wrong token: permission", (r.get("ok"), r.get("error")),
                (False, "permission"))
        r = held_by_b.call({"op": "restore", "id": "r2", "lease": lease["id"],
                            "restore_token": lease["token"]})
        c.check("restore with the RELEASE token: permission (it is not a restore token)",
                (r.get("ok"), r.get("error")), (False, "permission"))
        r = held_by_b.call({"op": "restore", "id": "r3", "lease": "otherinst-L1", "restore_token": "x"})
        c.check("restore of another instance's lease: unknown-lease", (r.get("ok"), r.get("error")),
                (False, "unknown-lease"))
        r = held_by_b.call({"op": "restore", "id": "r4", "lease": lease["id"], "restore_token": rtok})
        rres = cast("dict[str, Any]", r.get("result", {}))
        c.check("restore from another connection: ok, how long it took",
                (r.get("ok"), isinstance(rres.get("duration_ms"), int) and 0 < rres["duration_ms"] < 5000),
                (True, True))

        r = a.call({"op": "release", "id": "9", "lease": lease["id"], "token": lease["token"]})
        result = cast("dict[str, Any] | None", r.get("result"))
        c.check("release: the result", (r.get("ok"), isinstance(result, dict) and result.get("outcome")),
                (True, "returned"))
        c.check("the pool's `released` record names the lease and its trace id",
                [(x.get("axl.lease"), x.get("trace_id")) for x in logs if x.get("body") == "released"],
                [(lease["id"], TRACE)])
        r = held_by_b.call({"op": "restore", "id": "r5", "lease": lease["id"], "restore_token": rtok})
        c.check("restore of a lease already released: permission", (r.get("ok"), r.get("error")),
                (False, "permission"))
        # "agent": "tree" -- the tree's own build of the agent (a tree that
        # tests the agent itself): a guest of its own, never a pinned one.
        r = a.call({"op": "lease", "id": "ta", "tree": TREE, "class": "standard", "arch": arch,
                    "timeout": 300, "agent": "tree"})
        tl = cast("dict[str, Any]", r.get("lease", {}))
        c.check('lease "agent": "tree": the tree\'s built agent, on a guest booted for it',
                (r.get("ok"), tl.get("agent_image"), tl.get("start")), (True, str(agent.resolve()), "booted"))
        if tl:
            a.call({"op": "release", "id": "tr", "lease": tl["id"], "token": tl["token"]})
        fake = work / "fake-tree"
        (fake / "test" / "pool").mkdir(parents=True)
        shutil.copy(Path(TREE) / "test" / "pool" / "classes.toml", fake / "test" / "pool" / "classes.toml")
        r = a.call({"op": "lease", "id": "tf", "tree": str(fake), "class": "standard", "arch": arch,
                    "timeout": 30, "agent": "tree"})
        c.check('lease "agent": "tree" of a tree with no built agent: bad-request',
                (r.get("ok"), r.get("error")), (False, "bad-request"))
        everyone = cast("list[dict[str, Any]]", a.call({"op": "guests", "id": "gt"}).get("guests", []))
        mine = cast("list[dict[str, Any]]",
                    a.call({"op": "guests", "id": "gm", "tree": TREE}).get("guests", []))
        none = cast("list[object]", a.call({"op": "guests", "id": "gn", "tree": "/no/such/tree"})
                    .get("guests", ["no guests field"]))
        trees: set[str] = {str(g.get("tree")) for g in mine}
        c.check("guests: filtered by tree, each naming its tree",
                (len(mine) == len(everyone) > 0, trees, none), (True, {TREE}, NO_GUESTS))

        # A client that leaves while its lease's guest boots: nothing is
        # answered on the dead connection (the script fails on any
        # unhandled exception), and the service serves on.
        leaver = Conn(sock)
        leaver.call({"op": "hello", "id": "h", "version": 2, "client": "leaver"})
        leaver.send({"op": "lease", "id": "lv", "tree": TREE, "class": "standard", "arch": arch,
                     "timeout": 4, "booted_after": time.time()})
        time.sleep(1.5)
        leaver.close()
        time.sleep(5)                                      # past its deadline
        r = a.call({"op": "ping", "id": "after-leaver"})
        c.check("a client gone mid-boot: the service serves on", (r.get("id"), r.get("ok")),
                ("after-leaver", True))
        # ...and one that sends release and leaves before its answer: the
        # release still happens (the guest is reset and idle again), and its
        # answer goes nowhere -- quietly.
        quitter = Conn(sock)
        quitter.call({"op": "hello", "id": "h", "version": 2, "client": "quitter"})
        r = quitter.call({"op": "lease", "id": "q1", "tree": TREE, "class": "standard", "arch": arch,
                          "timeout": 300})
        ql = cast("dict[str, Any]", r.get("lease", {}))
        quitter.send({"op": "release", "id": "q2", "lease": ql.get("id"), "token": ql.get("token")})
        quitter.close()
        back = False
        for _ in range(100):
            gs = cast("list[dict[str, Any]]", a.call({"op": "guests", "id": "gq"}).get("guests", []))
            if any(g.get("guest") == ql.get("guest") and g.get("status") == "idle" for g in gs):
                back = True
                break
            time.sleep(0.2)
        c.check("a client gone before its release's answer: the guest is released, idle again",
                (r.get("ok"), back), (True, True))

        # A held lease whose connection closes is taken back.
        r = held_by_b.call({"op": "lease", "id": "10", "tree": TREE, "class": "standard", "arch": arch,
                            "timeout": 300, "traceparent": "not-a-traceparent"})
        gone = cast("dict[str, Any] | None", r.get("lease"))
        tid = str(r.get("trace_id"))
        c.check("a malformed traceparent: the lease is granted with a fresh trace id",
                (r.get("ok"), bool(re.fullmatch(r"[0-9a-f]{32}", tid)), tid != TRACE), (True, True, True))
        assert isinstance(gone, dict)
        held_by_b.close()
        # Taken back by the SERVICE: the guest stops with no client release.
        stopped = False
        for _ in range(100):
            gs = cast("list[dict[str, Any]]", a.call({"op": "guests", "id": "g"}).get("guests", []))
            if any(g.get("guest") == gone["guest"] and g.get("status") == "stopped" for g in gs):
                stopped = True
                break
            time.sleep(0.2)
        r = a.call({"op": "release", "id": "11", "lease": gone["id"], "token": gone["token"]})
        res = cast("dict[str, Any] | None", r.get("result"))
        c.check("a held lease whose connection closed: released by the service, its guest stopped; "
                "a later release says replaced, client-gone",
                (stopped, r.get("ok"), isinstance(res, dict) and (res.get("outcome"), res.get("cause"))),
                (True, True, ("replaced", "client-gone")))

        # Shutdown with clients connected: a pending lease is answered
        # shutting-down, every connection then closes (a holder sees EOF:
        # its wrapper reports MISS), and the pool closes after them.
        holder = Conn(sock)
        holder.call({"op": "hello", "id": "h", "version": 2, "client": "holder"})
        r = holder.call({"op": "lease", "id": "s1", "tree": TREE, "class": "standard", "arch": arch,
                         "timeout": 300})
        held_id = str(cast("dict[str, Any]", r.get("lease", {})).get("id"))
        # A client that stopped READING: its replies fill the socket, so the
        # service is blocked writing to it -- stop must not wait on it forever.
        stalled = Conn(sock)
        stalled.call({"op": "hello", "id": "h", "version": 2, "client": "stalled"})
        stalled.send({"op": "lease", "id": "st", "tree": TREE, "class": "standard", "arch": arch,
                      "timeout": 300, "booted_after": time.time()})   # pending: stop must answer it
        flood = b"".join(json.dumps({"op": "ping", "id": f"f{i}"}).encode() + b"\n" for i in range(40000))
        threading.Thread(target=lambda: _send_quietly(stalled, flood), daemon=True).start()
        time.sleep(2)
        waiter = Conn(sock)
        waiter.call({"op": "hello", "id": "h", "version": 2, "client": "waiter"})
        waiter.send({"op": "lease", "id": "s2", "tree": TREE, "class": "standard", "arch": arch,
                     "timeout": 300, "booted_after": time.time()})   # a fresh boot: pending
        waiter.call({"op": "ping", "id": "s3"})                      # the lease is in
        at_limit = json.dumps({"op": "ping", "id": "lim", "pad": ""}).encode()
        at_limit = at_limit[:-2] + b"x" * (MAX_LINE_BYTES - len(at_limit) - 1) + b'"}\n'
        a.raw(at_limit)
        r = a.recv()
        c.check("a line of exactly the limit (its newline included) is served",
                (len(at_limit), r.get("id"), r.get("ok")), (MAX_LINE_BYTES, "lim", True))
        a.raw(at_limit[:-1] + b"x\n")                       # one byte over
        r = a.recv()
        c.check("a line over the limit: too-large, then the connection closes",
                (r.get("error"), a.recv().get("eof")), ("too-large", True))
        a.close()
        t0 = time.monotonic()
        svc.stop()
        w = waiter.recv()
        c.check("stop: a pending lease is answered shutting-down, then the connection closes",
                (w.get("id"), w.get("ok"), w.get("error"), waiter.recv().get("eof")),
                ("s2", False, "shutting-down", True))
        c.check("stop: a connection holding a lease sees EOF", holder.recv().get("eof"), True)
        t.join(180)
        c.check("stop: the service exits -- a client that stopped reading does not hold it -- "
                "every guest stopped",
                (t.is_alive(), sorted({g.status for g in svc.pool.guests()} - {"retired"})),
                (False, ["stopped"]))
        took = time.monotonic() - t0
        print(f"  (stop took {took:.1f} s)")
        c.check(f"stop: within CONN_CLOSE_WAIT_S ({CONN_CLOSE_WAIT_S:.0f} s) -- the stalled client "
                "is cut off, not waited out", took < CONN_CLOSE_WAIT_S, True)
        stalled.close()
        bodies = [(x.get("body"), x.get("axl.lease"), x.get("axl.cause")) for x in logs]
        took_back = ("released", held_id, "client-gone")
        c.check("stop: the held lease is taken back BEFORE the pool closes",
                (took_back in bodies and ("closed", None, None) in bodies
                 and bodies.index(took_back) < bodies.index(("closed", None, None))), True)
        process_cases(c, agent, work)
    except Exception as e:  # a protocol that throws is a failure, reported
        c.check(f"the cases ran without raising ({type(e).__name__}: {e})", False, True)
    finally:
        svc.stop()
        t.join(180)
    print(f"service-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
