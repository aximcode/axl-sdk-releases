"""service.py -- the pool as a resident service (design doc §6.2, §6.3 step 4).

CONTRACT (revised after review, 2026-11-04) -- implementation follows,
test-first. The Rust service keeps this wire protocol: every name and value
is a stable string, unknown fields are ignored, unknown ops answered.

PROCESS. A systemd USER service, socket-activated: `axlpool.socket`
(ListenStream=%t/axlpool/pool.sock, SocketMode=0600, DirectoryMode=0700,
Accept=no) starts `axlpool.service` on the first connection. The service
takes the socket from systemd only when LISTEN_PID is its own pid (else it
binds the path itself: in a directory it creates 0700, or an existing one
it never chmods and refuses when another user owns it or others can write
it), and never unlinks a socket systemd owns -- nor a live one at the path
it would bind (systemd's, while the service idle-exited). A refused start
exits 3, which the unit does not restart on (`python3 -m axlpool units`). Every
connection's peer uid (SO_PEERCRED) must be the service's own: only this
user -- the self-hosted CI runner runs as the same user (checked 2026-11-04;
its jobs may lack XDG_RUNTIME_DIR, so clients fall back to
/run/user/<uid>/axlpool/pool.sock; the user lingers). Unit settings:
KillMode=mixed (SIGTERM to the service ALONE, so its ordered stop runs and
stops the guests; SIGKILL to the whole group -- guests included -- after),
TimeoutStopSec above STOP_BOUND_S, Restart=on-failure with a start limit and
RestartPreventExitStatus=3 (a refused start). State:
$XDG_STATE_HOME/axlpool (default ~/.local/state/axlpool). It exits after
`--exit-idle` seconds (default 900) with no client and no running guest:
it stops accepting first and does not drain, so systemd restarts it for
connections already queued; stopped guests' state stays on disk.

TREES. One service serves every checkout and commit, so a machine is
always that of the TREE asking: a lease names its tree (an absolute path),
and the service keeps, per tree, the ClassSet (<tree>/test/pool/
classes.toml) and the class keys -- a dry run of THAT tree's run-qemu.sh,
firmware, launcher and Shell. A CI run of another commit therefore gets
its own classes and never another tree's warm guests. No classes path is
ever accepted on the wire: classes.toml is a QEMU command line.

THE AGENT is SERVICE-PINNED (decided 2026-11-04), PER ARCH: each arch's
agent image is fixed when the service starts -- the service's own tree's
build of it (tree_agent), or `--agent [ARCH=]PATH` -- reported by `hello`
as `agents`, and part of every class key. A lease for an arch with none
misses `no-agent` rather than booting a guest whose agent cannot run. A tree that tests the
agent itself asks for its own with `"agent": "tree"` on the lease: the
image that tree's scripts/build-prefix.sh names for the arch (run as this
same user, like its classes.toml), or `bad-request` when it has none built.
The agent is part of the guest's queue key, so the two never share or
retire each other's guests. A lease reports the image as `agent_image`.

OWNERSHIP. One service per socket and one pool per state directory, each
an flock held for the process's life (`<sock>.lock`, `<state>/.lock`): a
second start fails ("another service owns ...", "another pool owns ...")
rather than unlinking a live socket or sweeping a live pool's guests.

PROTOCOL. JSON lines: UTF-8, one object per `\\n`-terminated line, at most
64 KiB (longer: `too-large`, then the connection closes; a partial line at
EOF is dropped, in both directions: a client reading a partial line before
EOF -- the service aborted a client that stopped reading -- treats it as
lost). The FIRST request on a connection must be `hello`.
Requests carry an `"id"` (string) that the response echoes; responses may
come in any order, so a pending `lease` holds up nothing else.

  {"op":"hello","id":"1","version":2,"client":"axlpool-run/1"}
      -> {"ok":true,"id":"1","version":2,"instance":"<id>","code":"<64 hex>",
          "agents":{"<arch>":{"path":"...","sha256":"..."}},"root":"<state dir>"}
      `code` is the CODE IDENTITY: a digest of the pool code the service
      loaded at start (code_identity over service_code_files), fixed for the
      process's life. A pooled skip-cache key holds it, because the
      long-lived service may run other code than the tree asking (an older
      start; another checkout's copy); `axlpool run --expect-code` misses a
      lease from a service whose identity is not the run's. Added within
      version 2 (unknown fields are ignored): a client that finds no `code`
      treats the service as unidentified -- no pooled key, a miss under
      --expect-code -- rather than refusing to talk to it. ANY successor
      service must report one, changing whenever the code it runs changes.
  {"op":"lease","id":"2","tree":"/abs/tree","class":"standard","arch":"X64",
   "timeout":120,"booted_after":null,"agent":"service"|"tree",
   "bind":"connection","traceparent":"<W3C, optional>"}
      -> {"ok":true,"id":"2","lease":{<Lease fields, paths as strings,
          "env": Lease.env() -- the token-free environment a test gets>},
          "trace_id":"<hex>"}
  {"op":"cancel","id":"3","target":"2"}   -> the pending lease answers
                                             error "cancelled"; this: ok
  {"op":"release","id":"4","lease":"<lease id>","token":"...","keep_files":false}
      -> {"ok":true,"id":"4","result":{"outcome","cause","loss","detail","kept"}}
  {"op":"guests","id":"5","tree":null}    -> {"ok":true,"id":"5","guests":[...]}
                                             (only that tree's when named)
  {"op":"revalidate","id":"6","tree":"/abs/tree"}
      -> {"ok":true,"id":"6","retired":["<class>/<arch>"]}
  {"op":"ping","id":"7"}                  -> {"ok":true,"id":"7"}
  {"op":"restore","id":"8","lease":"<lease id>","restore_token":"..."}
      -> {"ok":true,"id":"8","result":{"duration_ms":<n>}}
      Pool.restore: a HELD lease's guest back to `clean`, the lease still
      held. From ANY connection of this user: authorised by the lease's
      restore token, not by holding it. Errors: unknown-lease; permission (a
      wrong token, or a lease not held); bad-request (its class does not
      reset by snapshot); internal (the restore failed -- the lease is then
      taken back, and its release says replaced, restore-failed).

ERRORS have one shape: {"ok":false,"id":...,"error":<code>,"detail":"..."},
code one of bad-request, unknown-op, unsupported-version, permission,
unknown-lease, too-large, cancelled, shutting-down, unavailable (a
socket-activated start that was refused answers the clients it was started
for, then exits), internal -- or `miss`
with `"reason":<pool.MissReason>`. An unhandled exception answers
`internal`; it never drops or hangs the connection.

LEASE IDS carry the service instance (`<instance>-L...`), so a release
naming another instance's lease -- the service restarted since -- is
`unknown-lease`, never a misleading `permission`. The pool logs the same id.

TRACES. A lease's `trace_id` is its W3C `traceparent`'s trace id, or a
fresh one when the header is absent or malformed (never an error); it is in
the reply and in the pool's `leased` and `released` records.

STOPPING (SIGTERM, SIGINT, the idle exit): stop accepting; answer every
pending lease `shutting-down` and close every connection, whose held
leases are taken back as on EOF; THEN close the pool. A lease asked for
meanwhile is answered `shutting-down`.

LEASES ARE BOUND TO THEIR CONNECTION (`"bind":"connection"`, the only one
built; `"detached"` is reserved and will require max_hold_s). The service
keeps reading every connection while its leases are pending, so it sees
EOF: a PENDING lease is cancelled (its FIFO place given up, a boot already
started joins the pool for the next lease); a lease granted after its
client left is released at once, RETURNED (nothing ran on it); a HELD
lease is taken back -- its guest REPLACED (killed, its clean state kept;
the client may have died mid-command) and logged `client gone` with its
`axl.lease`. Pool.max_hold_s remains the backstop for a live, stuck client.
If the SERVICE dies, clients see EOF: the wrapper reports MISS, and the
next start's orphan sweep ends the guests that were leased.

THE TEST-SIDE WRAPPER -- a bash test cannot hold a socket:

  axlpool run --tree T --class C --arch A [--timeout S] [--keep-files-on-fail]
              -- test.sh [args]

holds one connection for the child's life, exports Lease.env() (never the
token), runs the child, releases (with keep_files when the child failed),
and exits with the child's status -- except that a MISS, a LOST release, or
EOF from the service exits AXLPOOL_EXIT_MISS (77, CTest's
SKIP_RETURN_CODE convention), so a test never reports FAIL for the pool.
One CTest test = one wrapper = one connection. The child's environment also
names the socket (AXL_POOL_SOCKET), so the test can ask for a mid-lease
restore (`axlpool restore`, its own short connection).

CTEST (step 6). `axlpool resource-spec --tree T` prints a CTest resource
spec from classes.toml's slots. Two CTest runs at once (a developer and
CI) each believe they own every slot, so a lease's timeout must fit inside
the test's CTest TIMEOUT: an overrun reads MISS (77), not a timeout FAIL.

CONCURRENCY. asyncio for the sockets; every blocking Pool call runs on a
dedicated executor (never the default to_thread pool, which N pending
leases would starve, leaving ping and guests unanswered). Pool gains
`lease(..., cancel=threading.Event)` and a public `expire(lease, cause)`.

LOGGING. Every Pool record plus the service's own (`started`,
`client connected`, `client gone`, `exiting`) goes to stdout as one JSON
object per line -- systemd puts it in the journal -- with OpenTelemetry
field names (§6.4) and the lease's `trace_id`.

RELEASE CERTIFICATION leases with `booted_after` set to the run's start.
"""
from __future__ import annotations

import asyncio
import dataclasses
import enum
import fcntl
import grp
import hashlib
import json
import os
import pwd
import re
import secrets
import signal
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Awaitable, Callable, Iterable, Mapping
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, cast

from axlpool.classes import ARCHES, ClassSet, load
from axlpool.klass import REPO
from axlpool.pool import CLOSE_BOUND_S, Cause, Lease, LeaseCancelled, LeaseMiss, LogFn, Pool

AXLPOOL_EXIT_MISS = 77          # the wrapper's exit for a pool failure (CTest SKIP_RETURN_CODE)
# The line, on stderr, that says a 77 was the POOL's (the wrapper's, or a
# pooled test's own harness -- common-test.sh prints the same) rather than
# the test's own SKIP. run-integration.sh reads nothing else to tell them apart.
POOL_MISS_MARKER = "AXLPOOL-MISS:"
PROTOCOL_VERSION = 2            # 2: hello's `agents` per arch (1 had one `agent`)
MAX_LINE_BYTES = 64 * 1024
CONN_CLOSE_WAIT_S = 30.0        # stop: how long connections get to take back their leases
END_REPLY_WAIT_S = 5.0          # stop: how long one `shutting-down` reply may take to send
# The longest a stop can take: the connections ending (all at once), their
# leases taken back, then pool.close() -- its boot wait and the guests'
# stops. A systemd unit's TimeoutStopSec must exceed it.
STOP_BOUND_S = END_REPLY_WAIT_S * 2 + CONN_CLOSE_WAIT_S + CLOSE_BOUND_S
EXIT_CANNOT_START = 3           # serve: a refused start (another owner, an unsafe path)
SD_LISTEN_FD = 3                # systemd's first passed socket (sd_listen_fds)
_UCRED = struct.Struct("iII")   # struct ucred: pid_t pid; uid_t uid; gid_t gid
_TRACEPARENT = re.compile(r"[0-9a-f]{2}-([0-9a-f]{32})-[0-9a-f]{16}-[0-9a-f]{2}")


def default_socket() -> Path:
    """$XDG_RUNTIME_DIR/axlpool/pool.sock; /run/user/<uid>/... when a CI
    job's environment lacks XDG_RUNTIME_DIR."""
    run = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return Path(run) / "axlpool" / "pool.sock"


def mem_cap_mb(arg: int | None, meminfo: str) -> int | None:
    """The guest RAM the service may keep, in MiB (pool.py mem_cap_mb): @arg
    when given, None (no cap) for 0, else HALF the host's MemTotal from
    @meminfo (/proc/meminfo's text). Half because the pool shares the host
    with every booted test's own QEMU and the builds beside them; resident
    idle guests are what it evicts first. Raises ValueError when there is no
    MemTotal to halve -- a pool silently uncapped is what this exists to end."""
    if arg is not None:
        return arg or None
    for line in meminfo.splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1]) // 1024 // 2
    raise ValueError("no MemTotal line in /proc/meminfo: give --mem-cap-mb (0 for no cap)")


def _check_cap_fits(classes: ClassSet, agents: Mapping[str, Path], cap: int | None) -> None:
    """Refuse a cap below the guest RAM of one of the service's own classes:
    no lease of that class could ever fit (pool.py misses it as over-cap)."""
    if cap is None:
        return
    need = [(classes.spec(name, arch, agents[arch]).ram_mb, name)
            for name in classes.names() for arch in classes.get(name).arches if arch in agents]
    if need:
        ram, name = max(need)
        if ram > cap:
            raise ValueError(f"--mem-cap-mb {cap} is below class {name}'s {ram} MiB guest RAM: "
                             "no lease of it could fit")


def code_identity(root: Path, files: Iterable[Path]) -> str:
    """A digest of @files -- each one's path relative to @root and its own
    sha256, sorted -- so any edit, rename, addition or removal changes it,
    and their order does not."""
    h = hashlib.sha256()
    for rel, digest in sorted((str(f.relative_to(root)), _sha256(f)) for f in files):
        h.update(f"{rel}\0{digest}\n".encode())
    return h.hexdigest()


def service_code_files(root: Path) -> list[Path]:
    """The pool code this process runs, as files under @root (the tree it
    was started from): every loaded module from there (the axlpool package,
    the agent client test/integration/lib/axl_agent.py), plus every file of
    the package, which a later import would load. The script that started
    the process (__main__) is not pool code."""
    pkg = Path(__file__).resolve().parent
    out = set(pkg.glob("*.py"))
    for name, mod in list(sys.modules.items()):
        f = getattr(mod, "__file__", None)
        if name == "__main__" or not f:
            continue
        p = Path(f).resolve()
        if p.suffix == ".py" and p.is_relative_to(root):
            out.add(p)
    return sorted(out)


def default_state() -> Path:
    """$XDG_STATE_HOME/axlpool, default ~/.local/state/axlpool."""
    base = os.environ.get("XDG_STATE_HOME") or str(Path.home() / ".local" / "state")
    return Path(base) / "axlpool"


def _jsonable(v: object) -> object:
    """@v with enums, paths and tuples made JSON, at any depth."""
    if isinstance(v, enum.Enum):
        return v.value
    if isinstance(v, Path):
        return str(v)
    if isinstance(v, (tuple, list)):
        return [_jsonable(x) for x in cast("list[object]", v)]
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in cast("dict[object, object]", v).items()}
    return v


def _sha256(path: Path) -> str:
    """@path's digest NOW: a pinned image rebuilt in place is reported as it
    is (the pool keys guests on it the same way, Pool._key)."""
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return ""


def _trace_id(traceparent: object) -> str:
    """The trace id of a W3C traceparent, else a fresh one: a malformed
    header starts a new trace rather than failing the lease."""
    m = _TRACEPARENT.fullmatch(traceparent) if isinstance(traceparent, str) else None
    if m is not None and m.group(1) != "0" * 32:
        return m.group(1)
    return secrets.token_hex(16)


def refuse_activated(detail: str) -> int:
    """A socket-activated start that cannot serve: answer every client
    already waiting on systemd's socket with `unavailable`. With Accept=no
    an unanswered connection stays queued and systemd starts the service
    again for it, and again, until the start limit -- after which the socket
    stops listening. Returns how many were answered (0 when not activated)."""
    if not (os.environ.get("LISTEN_PID") == str(os.getpid())
            and int(os.environ.get("LISTEN_FDS", "0")) >= 1):
        return 0
    listener = socket.socket(fileno=SD_LISTEN_FD)
    listener.setblocking(False)
    line = json.dumps({"id": None, "ok": False, "error": "unavailable", "detail": detail}).encode() + b"\n"
    answered = 0
    try:
        while True:
            try:
                conn, _ = listener.accept()
            except BlockingIOError:
                break                        # none waiting now
            with conn:
                try:
                    conn.settimeout(1.0)
                    conn.sendall(line)
                except OSError:
                    pass
            answered += 1
    finally:
        listener.detach()                    # systemd's fd: leave it open for the next start
    return answered


def tree_agent(tree: Path, arch: str) -> Path:
    """@tree's own build of the agent for @arch (`"agent": "tree"`), found
    the way its tests find it: that tree's scripts/build-prefix.sh. Raises
    ValueError naming why there is none."""
    native = {"X64": "x64", "AARCH64": "aa64"}.get(arch)
    script = tree / "scripts" / "build-prefix.sh"
    if native is None:
        raise ValueError(f"no agent build for arch {arch}")
    if not script.is_file():
        raise ValueError(f"{tree} has no scripts/build-prefix.sh to find its agent build")
    run = subprocess.run([str(script), "--abs", native], capture_output=True, text=True,
                         timeout=30, check=False)
    if run.returncode != 0:
        raise ValueError(f"{script} --abs {native} failed ({run.returncode}): {run.stderr.strip()[-300:]}")
    agent = Path(run.stdout.strip()) / "tools" / "agent.efi"
    if not run.stdout.strip() or not agent.is_file():
        raise ValueError(f"{tree} has no built agent at {agent} (scripts/axl-build.sh ARCH={native} agent)")
    return agent


def _group_is_private(gid: int) -> bool:
    """The user's own group with no other member (RHEL's user-private
    groups, where umask 002 makes every new directory group-writable)."""
    if gid != os.getgid():
        return False
    try:
        members = grp.getgrgid(gid).gr_mem
        me = pwd.getpwuid(os.getuid()).pw_name
    except KeyError:
        return False
    return all(m == me for m in members)


def _check_component(comp: Path, *, is_socket_dir: bool) -> None:
    """One step of _socket_dir's walk; RuntimeError if unsafe."""
    st = comp.stat()
    if st.st_uid not in (0, os.getuid()) or (is_socket_dir and st.st_uid != os.getuid()):
        raise RuntimeError(f"refusing {comp} for the socket: owned by uid {st.st_uid}")
    writable = st.st_mode & 0o002 or (st.st_mode & 0o020 and not _group_is_private(st.st_gid))
    if writable and (is_socket_dir or not st.st_mode & stat.S_ISVTX):
        raise RuntimeError(f"refusing {comp} for the socket: others can write it")


def _log_stdout(rec: dict[str, object]) -> None:
    sys.stdout.write(json.dumps(rec, default=str) + "\n")
    sys.stdout.flush()


def _lease_problem(r: dict[str, Any]) -> str | None:
    """Why a lease request is malformed, or None: checked before any work,
    so a bad field is a bad-request, never an exception in a task."""
    tree = r.get("tree")
    if not isinstance(tree, str) or not Path(tree).is_absolute():
        return "`tree` must be an absolute path"
    for f in ("class", "arch"):
        if not isinstance(r.get(f), str) or not r.get(f):
            return f"`{f}` must be a non-empty string"
    t = r.get("timeout", 300)
    if isinstance(t, bool) or not isinstance(t, (int, float)) or t < 0:
        return "`timeout` must be a non-negative number"
    b = r.get("booted_after")
    if b is not None and (isinstance(b, bool) or not isinstance(b, (int, float))):
        return "`booted_after` must be a number or null"
    if r.get("bind", "connection") != "connection":
        return "`bind` must be \"connection\" (\"detached\" is reserved)"
    if r.get("agent", "service") not in ("service", "tree"):
        return "`agent` must be \"service\" or \"tree\""
    return None



class _UdpEcho(asyncio.DatagramProtocol):
    """Every datagram straight back to its sender: the unit suite's UDP peer
    (AxlTestNet sends to 10.0.2.2:<port>, the host's loopback to a guest)."""

    def __init__(self) -> None:
        self._transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        assert isinstance(transport, asyncio.DatagramTransport)
        self._transport = transport

    def datagram_received(self, data: bytes, addr: tuple[str | Any, int]) -> None:
        if self._transport is not None:
            self._transport.sendto(data, addr)


class Service:
    """One pool behind a Unix socket (the module docstring is the contract)."""

    def __init__(self, tree: Path, agents: Path | Mapping[str, Path], state: Path, sock: Path, *,
                 exit_idle: float = 900.0, log: LogFn = _log_stdout,
                 udp_echo_port: int | None = None, mem_cap_mb: int | None = None) -> None:
        self.instance = secrets.token_hex(4)
        # The code this process runs, digested once, now: what it LOADED,
        # which an edit to the checkout afterwards does not change. Reported
        # by hello (`code`); a pooled skip-cache key holds it.
        self.code = code_identity(REPO, service_code_files(REPO))
        self.allowed_uid = os.getuid()     # the only peer served (SO_PEERCRED)
        self.sock = sock
        self.exit_idle = exit_idle
        # The unit suite's UDP echo, served for the service's whole life (None:
        # none). OWNED here, not by a test run: a run's echo could be killed by
        # its cleanup while another run's AxlTestNet still used it.
        self.udp_echo_port = udp_echo_port
        self._log_fn = log
        # The pinned agent of each arch (one path: every arch's), and its digest.
        self.agents: dict[str, Path] = ({a: agents for a in ARCHES} if isinstance(agents, Path)
                                        else dict(agents))

        self._bound: socket.socket | None = None    # our own listener (None when activated)
        self._sock_lock = self._own_socket()
        try:
            classes = load(tree / "test" / "pool" / "classes.toml")
            _check_cap_fits(classes, self.agents, mem_cap_mb)
            self.pool = Pool(classes, self.agents, state, log=log,
                             lease_prefix=f"{self.instance}-", mem_cap_mb=mem_cap_mb)
        except BaseException:
            if self._bound is not None:
                self._bound.close()
            if self._sock_lock is not None:
                os.close(self._sock_lock)
            raise
        self.state = state
        # Blocking Pool calls run on the service's own executors, never the
        # default one. Leases WAIT (for a slot, a boot), so they get theirs:
        # a queue of waiting leases must never starve the release, expire or
        # guests call that would free a slot.
        self._lease_exec = ThreadPoolExecutor(max_workers=256, thread_name_prefix="axlpool-lease")
        self._exec = ThreadPoolExecutor(max_workers=16, thread_name_prefix="axlpool-svc")
        self._leases: dict[str, Lease] = {}          # wire id -> lease
        self._loop: asyncio.AbstractEventLoop | None = None
        self._stop: asyncio.Event | None = None
        self._clients = 0
        # Each live connection's handler and how to end it (on stop).
        # Each live connection's handler, how to end it, and its lease and
        # release tasks (stop waits for both).
        self._conns: dict[asyncio.Task[None],
                          tuple[Callable[[], Awaitable[None]], set[asyncio.Task[None]]]] = {}
        self._stopping = False
        self._stop_requested = False

    def _log(self, severity: str, body: str, **attrs: object) -> None:
        rec: dict[str, object] = {
            "timestamp": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
            "severity_text": severity, "body": body}
        rec.update({f"axl.{k}": v for k, v in attrs.items()})
        self._log_fn(rec)

    # -- lifecycle --------------------------------------------------------

    def run(self) -> None:
        asyncio.run(self._main())

    def stop(self) -> None:
        """Thread-safe; a no-op once the service has exited. Before run() it
        makes run() return at once."""
        self._stop_requested = True
        if self._loop is not None and self._stop is not None:
            try:
                self._loop.call_soon_threadsafe(self._stop.set)
            except RuntimeError:     # the loop is closed: already exited
                pass

    def _systemd_socket(self) -> bool:
        return os.environ.get("LISTEN_PID") == str(os.getpid()) and int(os.environ.get("LISTEN_FDS", "0")) >= 1

    def _socket_dir(self) -> None:
        """Make the socket's directory safe to bind in, by OpenSSH's
        safe_path rule (the StrictModes walk): every EXISTING component from
        the root down must be root's or ours and not writable by others --
        an ancestor may be if sticky (/tmp), a user-private group's write
        bit is not "others" -- since whoever can write a component can swap
        the socket under the clients. Missing components are then created
        0700 by us. Nothing existing is ever chmodded (a --socket in $HOME
        must not lock $HOME down). Raises RuntimeError naming the culprit."""
        d = self.sock.parent.absolute()
        chain = [*reversed(d.parents), d]
        missing = [c for c in chain if not c.exists()]
        for comp in chain:
            if comp in missing:
                break
            _check_component(comp, is_socket_dir=comp == d)
        for comp in missing:
            try:
                comp.mkdir()
            except FileExistsError:          # another start made it first: check it
                _check_component(comp, is_socket_dir=comp == d)
                continue
            comp.chmod(0o700)                # whatever the umask

    def _own_socket(self) -> int | None:
        """Lock `<sock>.lock` for the service's life, so the path is unlinked
        only by its owner -- a second start is refused rather than stealing a
        live socket (a connect probe would race two starts). None when
        systemd owns the socket: it never unlinks one."""
        if self._systemd_socket():
            return None
        self._socket_dir()
        fd = os.open(f"{self.sock}.lock", os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(fd)
            raise RuntimeError(f"another service owns {self.sock}") from None
        bound: socket.socket | None = None
        try:
            self._check_path()
            # Bound NOW, right after the check: no window for a socket unit
            # to appear between the check and the unlink.
            self.sock.unlink(missing_ok=True)
            bound = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            bound.bind(str(self.sock))
            self.sock.chmod(0o600)
            bound.listen(64)
        except BaseException:
            if bound is not None:
                bound.close()
            os.close(fd)
            raise
        self._bound = bound
        return fd

    def _check_path(self) -> None:
        """What is at the socket path may be replaced: nothing, or a STALE
        socket (its service crashed; a connect is refused). Refused: a file
        that is not a socket, and a LIVE socket while no service holds the
        lock -- systemd's socket unit after its service idle-exited. (The
        probe's connect activates that service, which then serves; harmless,
        and it logs a client that said no hello.)"""
        if not self.sock.exists() and not self.sock.is_symlink():
            return
        if not self.sock.is_socket():
            raise RuntimeError(f"{self.sock} exists and is not a socket: not replacing it")
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        probe.settimeout(1.0)
        try:
            probe.connect(str(self.sock))
        except (ConnectionRefusedError, FileNotFoundError):
            return                           # stale
        except OSError:
            pass                             # a timeout: a full backlog is still a live socket
        finally:
            probe.close()
        raise RuntimeError(f"a live socket is at {self.sock} (systemd's axlpool.socket?): "
                           f"not replacing it")

    def _listen(self) -> socket.socket:
        """systemd's socket when LISTEN_PID names us; else bind it."""
        if self._sock_lock is None:
            # Taken: nothing this service starts (run-qemu.sh, its guests)
            # may believe it was socket-activated, or hold the listener open.
            for var in ("LISTEN_PID", "LISTEN_FDS", "LISTEN_FDNAMES"):
                os.environ.pop(var, None)
            os.set_inheritable(SD_LISTEN_FD, False)
            return socket.socket(fileno=SD_LISTEN_FD)
        assert self._bound is not None      # bound by _own_socket, under its lock
        return self._bound

    async def _main(self) -> None:
        self._stop = asyncio.Event()
        self._loop = asyncio.get_running_loop()
        if self._stop_requested:             # stop() ran before the loop existed
            self._stop.set()
        server = await asyncio.start_unix_server(self._client, sock=self._listen(),
                                                 limit=MAX_LINE_BYTES - 1)   # content; + its newline
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                self._loop.add_signal_handler(sig, self._stop.set)
            except (ValueError, RuntimeError):   # not the main thread (tests run it in one)
                pass
        echo: asyncio.DatagramTransport | None = None
        if self.udp_echo_port is not None:
            try:
                # SO_REUSEPORT, as a booted run's echo-stream.py --udp binds
                # it: the echo is stateless, so the two SHARE the port. Linux
                # lets a port be shared only when every holder set it --
                # without, whichever bound second was locked out.
                echo, _ = await self._loop.create_datagram_endpoint(
                    _UdpEcho, local_addr=("127.0.0.1", self.udp_echo_port), reuse_port=True)
            except OSError as e:
                # Held by something that did NOT set SO_REUSEPORT: the pooled
                # unit runs (whose UDP peer this is) would have none of ours.
                self._log("WARN", "udp echo not served", port=self.udp_echo_port, reason=str(e))
        self._log("INFO", "started", instance=self.instance, code=self.code,
                  mem_cap_mb=self.pool.mem_cap_mb)
        watch = asyncio.create_task(self._idle_watch()) if self.exit_idle > 0 else None
        await self._stop.wait()
        server.close()                       # stop accepting; queued connections wait for a restart
        if echo is not None:
            echo.close()
        if watch is not None:
            watch.cancel()
        # Connections end BEFORE the pool closes, while the executors still
        # run: pending leases answered shutting-down, held ones taken back.
        self._stopping = True
        conns = list(self._conns.items())
        try:
            # All at once: one stalled client costs END_REPLY_WAIT_S * 2, not one each.
            ended = await asyncio.gather(*(asyncio.wait_for(end(), END_REPLY_WAIT_S * 2)
                                           for _task, (end, _tasks) in conns), return_exceptions=True)
            for e in ended:
                if isinstance(e, BaseException):
                    self._log("WARN", "connection did not end cleanly", detail=f"{type(e).__name__}: {e}")
            waits = [t for t, _ in conns] + [lt for _, (_, tasks) in conns for lt in tasks]
            if waits:
                await asyncio.wait(waits, timeout=CONN_CLOSE_WAIT_S)
        finally:
            # Whatever happened above, the guests end with the service.
            await self._loop.run_in_executor(self._exec, self.pool.close)
            self._exec.shutdown(wait=False)
            self._lease_exec.shutdown(wait=False)
            if self._sock_lock is not None:
                os.close(self._sock_lock)
            self._log("INFO", "exiting", instance=self.instance)

    async def _idle_watch(self) -> None:
        quiet_since = time.monotonic()
        while True:
            await asyncio.sleep(min(30.0, self.exit_idle / 4))
            # Off the loop: guests() takes the pool's lock, which a kill holds.
            gs = await asyncio.get_running_loop().run_in_executor(self._exec, self.pool.guests)
            running = any(g.status not in ("stopped", "retired") for g in gs)
            if self._clients or running:
                quiet_since = time.monotonic()
            elif time.monotonic() - quiet_since >= self.exit_idle and self._stop is not None:
                self._stop.set()
                return

    # -- one connection -----------------------------------------------------

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        # Only this user (the module docstring): fail CLOSED when the peer's
        # credentials cannot be read.
        sk = writer.get_extra_info("socket")
        pid, uid = -1, -1
        if sk is not None:
            try:
                pid, uid, _gid = _UCRED.unpack(sk.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                             _UCRED.size))
            except OSError:
                pass
        if uid != self.allowed_uid:
            self._log("WARN", "connection refused", peer_pid=pid, peer_uid=uid)
            try:
                writer.write(json.dumps({"id": None, "ok": False, "error": "permission",
                                         "detail": "this service serves only its own user"}).encode()
                             + b"\n")
                await writer.drain()
            except OSError:
                pass
            writer.close()
            return
        self._clients += 1
        me = asyncio.current_task()
        wlock = asyncio.Lock()
        hello = False
        pending: dict[str, threading.Event] = {}
        answered: set[str] = set()
        held: dict[str, Lease] = {}

        async def reply(rid: object, **body: object) -> None:
            """Answer @rid. A connection gone meanwhile is not an error: there
            is nobody to tell (the read loop sees it end and cleans up)."""
            async with wlock:
                if writer.is_closing():
                    return
                try:
                    writer.write(json.dumps({"id": rid, **body}).encode() + b"\n")
                    await writer.drain()
                except OSError:
                    pass

        async def error(rid: object, code: str, detail: str, **more: object) -> None:
            await reply(rid, ok=False, error=code, detail=detail, **more)

        async def do_lease(rid: str, req: dict[str, Any]) -> None:
            try:
                await do_lease_body(rid, req)
            except Exception as e:  # noqa: BLE001 -- answered, never hung
                pending.pop(rid, None)
                if rid not in answered:
                    await error(rid, "internal", f"{type(e).__name__}: {e}")

        async def do_lease_body(rid: str, req: dict[str, Any]) -> None:
            cancel = pending[rid]
            loop = asyncio.get_running_loop()
            agent: Path | None = None
            if req.get("agent") == "tree":
                try:
                    agent = await loop.run_in_executor(self._exec, tree_agent, Path(str(req["tree"])),
                                                       str(req.get("arch")))
                except ValueError as e:
                    pending.pop(rid, None)
                    if rid not in answered:
                        await error(rid, "bad-request", str(e))
                    return
            try:
                lease = await loop.run_in_executor(self._lease_exec, lambda: self.pool.lease(
                    str(req.get("class")), str(req.get("arch")), timeout=float(req.get("timeout", 300)),
                    booted_after=req.get("booted_after"), cancel=cancel,
                    tree=Path(str(req["tree"])) if req.get("tree") else None,
                    trace_id=_trace_id(req.get("traceparent")), agent=agent))
            except LeaseCancelled:
                return
            except LeaseMiss as e:
                if rid not in answered:
                    await error(rid, "miss", e.detail, reason=e.reason.value)
                return
            finally:
                pending.pop(rid, None)
            wire = lease.id                  # the pool's id, prefixed with this instance
            if rid in answered or cancel.is_set():
                # Granted after the client cancelled or left: nothing ran on it.
                await loop.run_in_executor(self._exec, self.pool.release, lease)
                return
            self._leases[wire] = lease
            held[wire] = lease
            fields = {k: _jsonable(v) for k, v in dataclasses.asdict(lease).items()}
            fields["id"] = wire
            fields["env"] = lease.env()          # what a test needs, token-free: one source
            await reply(rid, ok=True, lease=fields, trace_id=lease.trace_id)

        async def do_release(rid: str, req: dict[str, Any]) -> None:
            try:
                await do_release_body(rid, req)
            except Exception as e:  # noqa: BLE001 -- answered, never hung
                await error(rid, "internal", f"{type(e).__name__}: {e}")

        async def do_release_body(rid: str, req: dict[str, Any]) -> None:
            wire = str(req.get("lease", ""))
            lease = self._leases.get(wire)
            if not wire.startswith(f"{self.instance}-") or lease is None:
                await error(rid, "unknown-lease", f"no lease {wire} in this service instance")
                return
            asked = dataclasses.replace(lease, token=str(req.get("token", "")))
            loop = asyncio.get_running_loop()
            try:
                res = await loop.run_in_executor(self._exec, lambda: self.pool.release(
                    asked, keep_files=bool(req.get("keep_files", False))))
            except PermissionError as e:
                await error(rid, "permission", str(e))
                return
            held.pop(wire, None)
            await reply(rid, ok=True, result={k: _jsonable(v) for k, v in dataclasses.asdict(res).items()})

        async def do_restore(rid: str, req: dict[str, Any]) -> None:
            try:
                await do_restore_body(rid, req)
            except Exception as e:  # noqa: BLE001 -- answered, never hung
                await error(rid, "internal", f"{type(e).__name__}: {e}")

        async def do_restore_body(rid: str, req: dict[str, Any]) -> None:
            # Any connection of this user may ask: the restore token is the
            # authority, not holding the lease (a test asks; its wrapper holds).
            wire = str(req.get("lease", ""))
            lease = self._leases.get(wire)
            if not wire.startswith(f"{self.instance}-") or lease is None:
                await error(rid, "unknown-lease", f"no lease {wire} in this service instance")
                return
            asked = dataclasses.replace(lease, restore_token=str(req.get("restore_token", "")))
            loop = asyncio.get_running_loop()
            try:
                # The lease executor: a restore blocks ~0.4 s, and many may run at once.
                took = await loop.run_in_executor(self._lease_exec, self.pool.restore, asked)
            except PermissionError as e:
                await error(rid, "permission", str(e))
                return
            except ValueError as e:
                await error(rid, "bad-request", str(e))
                return
            except RuntimeError as e:
                await error(rid, "internal", str(e))
                return
            await reply(rid, ok=True, result={"duration_ms": took})

        async def end() -> None:
            """The service is stopping: answer what waits, close the line --
            bounded, since a client that stopped reading has the write lock
            held by a blocked drain."""
            waiting = [rid for rid in pending if rid not in answered]
            for rid in waiting:
                pending[rid].set()
                answered.add(rid)
            try:
                for rid in waiting:
                    try:
                        await asyncio.wait_for(error(rid, "shutting-down", "the service is stopping"),
                                               END_REPLY_WAIT_S)
                    except TimeoutError:
                        break                # it is not reading: abort below
            finally:                         # even if this end() is itself cancelled
                if writer.transport.get_write_buffer_size() > 0:
                    writer.transport.abort() # close() would wait to flush to a stalled reader
                else:
                    writer.close()

        tasks: set[asyncio.Task[None]] = set()
        if me is not None:
            self._conns[me] = (end, tasks)
        try:
            while True:
                try:
                    line = await reader.readline()
                except (ValueError, asyncio.LimitOverrunError):
                    await error(None, "too-large", f"a line over {MAX_LINE_BYTES} bytes")
                    break
                if not line:
                    break
                if not line.endswith(b"\n"):
                    break                   # a partial line at EOF is dropped
                try:
                    req = json.loads(line)
                    if not isinstance(req, dict):
                        raise ValueError("not an object")
                except ValueError as e:
                    await error(None, "bad-request", f"not a JSON object: {e}")
                    continue
                r: dict[str, Any] = req  # pyright: ignore[reportUnknownVariableType]
                rid, op = r.get("id"), r.get("op")
                try:
                    if not isinstance(rid, str):
                        await error(None, "bad-request", "`id` must be a string")
                    elif not hello and op != "hello":
                        await error(rid, "bad-request", "the first request must be hello")
                    elif op == "hello":
                        ver = r.get("version")
                        if isinstance(ver, bool) or ver != PROTOCOL_VERSION:
                            await error(rid, "unsupported-version", f"this service speaks {PROTOCOL_VERSION}")
                            continue
                        hello = True
                        await reply(rid, ok=True, version=PROTOCOL_VERSION, instance=self.instance,
                                    code=self.code,
                                    agents={a: {"path": str(p), "sha256": _sha256(p)}
                                            for a, p in self.agents.items()},
                                    root=str(self.state))
                    elif op == "ping":
                        await reply(rid, ok=True)
                    elif op == "lease" and self._stopping:
                        await error(rid, "shutting-down", "the service is stopping")
                    elif op == "lease" and (bad := _lease_problem(r)) is not None:
                        await error(rid, "bad-request", bad)
                    elif op == "lease":
                        pending[str(rid)] = threading.Event()
                        task = asyncio.create_task(do_lease(str(rid), r))
                        tasks.add(task)
                        task.add_done_callback(tasks.discard)
                    elif op == "cancel":
                        target = str(r.get("target"))
                        ev = pending.get(target)
                        if ev is not None and target not in answered:
                            ev.set()
                            answered.add(target)
                            await error(target, "cancelled", "cancelled by the client")
                        await reply(rid, ok=True)
                    elif op == "release":
                        task = asyncio.create_task(do_release(str(rid), r))
                        tasks.add(task)
                        task.add_done_callback(tasks.discard)
                    elif op == "restore":
                        task = asyncio.create_task(do_restore(str(rid), r))
                        tasks.add(task)
                        task.add_done_callback(tasks.discard)
                    elif op == "guests":
                        gs = await asyncio.get_running_loop().run_in_executor(self._exec, self.pool.guests)
                        want = r.get("tree")
                        await reply(rid, ok=True, guests=[dataclasses.asdict(g) for g in gs
                                                          if want is None or g.tree == str(want)])
                    elif op == "revalidate":
                        tree = Path(str(r.get("tree")))
                        retired = await asyncio.get_running_loop().run_in_executor(
                            self._exec, lambda: self.pool.revalidate(load(tree / "test" / "pool" / "classes.toml")))
                        await reply(rid, ok=True, retired=retired)
                    else:
                        await error(rid, "unknown-op", f"no op {op!r}")
                except Exception as e:  # noqa: BLE001 -- answered, never dropped
                    await error(rid, "internal", f"{type(e).__name__}: {e}")
        except OSError:
            pass                            # lost mid-read (stop aborts a stalled one)
        finally:
            # The connection is gone: pending leases cancelled (and answered:
            # nothing is sent to a closed line), held ones taken back.
            for rid, ev in list(pending.items()):
                ev.set()
                answered.add(rid)
            # A snapshot: a release task still running pops from `held`
            # while the take-backs below await (the client sent release, then left).
            for wire, lease in list(held.items()):
                self._log("WARN", "client gone", lease=wire)
                # Taken back AND released: the slot is freed now, and a later
                # release of it answers replaced, client-gone.
                loop = asyncio.get_running_loop()
                await loop.run_in_executor(self._exec, self.pool.expire, lease, Cause.CLIENT_GONE)
                await loop.run_in_executor(self._exec, self.pool.release, lease)
            self._clients -= 1
            if me is not None:
                self._conns.pop(me, None)
            writer.close()
