"""pool.py -- leases on resident guests, by class (design doc §6.3 step 3).

CONTRACT (revised after review) -- interface and docstrings, implementation
to follow test-first. The service (step 4) serves this same API over a Unix
socket and the Rust service keeps it, so a Lease is plain data and every
wire value is a stable string.

    pool = Pool(classes, agent, root)
    lease = pool.lease("standard", "X64", timeout=120)
    try:
        r = lease.agent().exec(f"{lease.guest_dir}\\\\test-foo.nsh")
    finally:
        result = pool.release(lease)

WHAT A LEASE GIVES: the agent's address (127.0.0.1), the QMP socket, a
directory in the guest's virtio-fs share (`guest_dir` as the guest names it,
`host_dir` on the host), the guest's serial console from this lease's start
(`serial()`), the machine it ran on (class key, guest id, how it started),
and env() for a test script. Shared QMP: a test may send anything EXCEPT
quit, stop/cont, snapshot-*, system_reset and job-* -- the pool owns those.
The pool cannot see what a test sends; one that breaks this is caught only
by its effect (a guest found hung or exited at release).

OUTCOMES. A pool adds failure modes the per-test boot never had; they must
never read as the test's failure (§6.4, KCIDB's MISS):
- lease() raises LeaseMiss (reason, a stable wire string) when the POOL
  could not provide: no slot within the deadline, the class would not boot
  (cached against its key until revalidate(), so N waiters do not each pay
  a failing boot), unknown class or arch, a guest that failed its health
  check and no replacement in time, or the pool closed. The test reports
  MISS (the runner's reserved exit code).
- A guest that dies or hangs WHILE LEASED, of its own accord, is the TEST's
  result -- the code under test may have crashed the firmware: FAIL.
  release() names what happened (ReleaseResult.cause) and replaces it.
- A guest the POOL ended while leased -- close(), a dead virtiofsd, an
  orphan sweep -- is reported by release() as LOST with the
  Loss; the test reports MISS, whatever it saw.

RELEASE returns at once: the guest is handed back and reset in the
background per its class (`snapshot`: restore `clean`; `reuse`: nothing;
`reset`: system_reset and a wait for the agent), so no test waits for the
previous one's restore. A reset that fails retires the guest and is logged;
it is not this lease's result. Before EVERY lease -- a fresh boot's too --
the guest passes a health check (QEMU alive, QMP status `running` within
the QMP deadline, the agent's /ping within HEALTH_DEADLINE_S); one that
fails is replaced -- a MISS (`unhealthy`) if none can be had in time, never
a FAIL. release() wipes the share (everything but the pool's probe file);
with `keep_files` the lease's directory is first MOVED out, to
`<root>/kept/<lease>` (ReleaseResult.kept) -- a failed test's files kept
for the reader, as TEST_KEEP_LOG keeps logs, and safe from the next
lease's wipe. Concurrent release() calls of one lease act once: the later
ones wait and return the first's result.

RESTORE (design §6.9): restore(lease) puts a HELD lease's guest back to its
`clean` snapshot and returns the milliseconds it took; the lease stays held
and its share directory is untouched. It is the release's own restore,
synchronous: what scripts/run-qemu.sh's pooled mode asks between runs, so
each run gets a fresh machine as a boot does. It is authorised by the
lease's `restore_token` -- a capability separate from the release token
(env() carries it, as AXL_POOL_RESTORE_TOKEN): a test can restore its own
guest, never end the lease its wrapper owns. PermissionError: a wrong token,
or a lease not held (released, or another pool's). ValueError: the class
does not reset by snapshot. Host forwards the lease added are removed, as a
release removes them. A restore that fails takes the lease back (expire(),
Cause.RESTORE_FAILED) and raises RuntimeError: its release says replaced.
A release waits for a restore in flight on the same lease.

CONCURRENCY. A Pool is thread-safe. `timeout` is ONE deadline covering the
wait for a slot AND any boot or cold start. Waiters for a (class, arch) are
served first come, first served; `slots` counts per arch. A guest mid-reset
is waited for only RESET_GRACE_S; after that a free slot boots. Every
guest is started by the pool's own long-lived threads (PR_SET_PDEATHSIG is
per thread). close() wakes every waiting lease() with POOL_CLOSED, waits
for boots and cold starts in flight, and stops what they produce.

LIFETIMES. `keep = "idle"`: resident until unleased for idle_timeout_s;
`keep = "lease"`: stopped at release. A stopped guest keeps its state; the
next lease cold-starts it (~0.3-0.4 s). Under mem_cap_mb, starting a guest
first stops least-recently-used IDLE guests; leased guests are never
evicted, and a lease that would need one waits (inside its deadline).
Every guest serving the pool is its child and dies with it (guest.py: a
boot hands over to a cold start). Only one caught mid-boot -- still under
run-qemu.sh's `timeout` -- can outlive a crashed pool: a Pool sweeps
orphans -- any QEMU or virtiofsd naming its root -- when it starts.
Owed (§6.3): cold-start ports through run-qemu.sh's flock scheme (today an
unlocked probe that can lose a race).

KEYS. A class's key (klass.class_key) is computed when a (class, arch) is
first leased in the pool's life and on revalidate(), which can also take a
new ClassSet (an edited classes.toml). A guest whose key changed is never
leased again -- retired at release if it is leased, at once if not.
`booted_after` (release certification, §6.2): a guest whose snapshot was
taken before that time is booted anew ONCE, and the new snapshot then
serves every later lease as usual.

LOGGING. Each lifecycle event goes to `log` as one dict with OpenTelemetry
field names -- `timestamp` (ISO 8601, UTC), `severity_text`, `body`, and
attributes `axl.class`, `axl.arch`, `axl.lease`, `axl.guest`, `axl.key`,
`axl.outcome`, `axl.cause` (an enum value), `axl.detail` (free text) and
`duration_ms` (§6.4).
"""
from __future__ import annotations

import enum
import fcntl
import json
import os
import secrets
import signal
import shutil
import threading
import time
from collections import OrderedDict, deque
from collections.abc import Callable, Mapping
import queue
from concurrent.futures import Future
from concurrent.futures import TimeoutError as FutureTimeout
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, cast

from axl_agent import AgentClient
from axlpool.classes import ARCHES, ClassError, ClassSet, load
from axlpool.guest import SHARE_PROBE, Guest, HandoverFailed, pid_alive
from axlpool.klass import DEFAULT_GUEST_RAM_MB, class_key

LogFn = Callable[[dict[str, object]], None]
OTEL_FIELDS = ("duration_ms", "trace_id")   # logged under their own names, not axl.*
QKey = tuple[Path, str, str, Path]          # tree, class, arch, agent: guests that serve alike
FileSig = tuple[int, int]                   # an agent image's (st_mtime_ns, st_size)

HEALTH_DEADLINE_S = 5.0
CANCEL_POLL_S = 0.25         # how often a wait with a cancel event looks at it
RESET_GRACE_S = 2.0          # a lease waits this long for a guest mid-reset before booting another
RESULTS_KEPT = 4096          # finished releases remembered for idempotency
KEPT_DIR = "kept"            # <root>/kept/<lease>: a failed test's files (keep_files)
BOOT_WAIT_ON_CLOSE_S = 120.0 # close() waits at most this long, in all, for boots in flight
GUEST_STOP_WAIT_S = 30.0     # ...then this long for every guest's stop, all at once
GUEST_KILL_WAIT_S = 20.0     # ...then this long for the kills of those that did not stop
CLOSE_BOUND_S = BOOT_WAIT_ON_CLOSE_S + GUEST_STOP_WAIT_S + GUEST_KILL_WAIT_S   # close()'s most
SPAWNER_THREADS = 32         # guests starting at once


class MissReason(enum.StrEnum):
    """Why the pool could not provide a lease (a stable wire string)."""

    NO_SLOT = "no-slot"                  # no guest of the class within the deadline
    BOOT_FAILED = "boot-failed"          # the class would not boot or cold-start
    UNHEALTHY = "unhealthy"              # guests failed the health check; none replaced in time
    UNKNOWN_CLASS = "unknown-class"      # no such class, or not for that arch
    NO_AGENT = "no-agent"                # no agent image pinned for that arch
    OVER_CAP = "over-cap"                # the class's guest RAM exceeds mem_cap_mb: never fits
    POOL_CLOSED = "pool-closed"


class LeaseMiss(Exception):
    """lease() could not provide a guest: report MISS, not FAIL."""

    def __init__(self, reason: MissReason, detail: str) -> None:
        super().__init__(f"{reason.value}: {detail}")
        self.reason = reason
        self.detail = detail


class LeaseCancelled(Exception):
    """lease() gave up because its `cancel` event was set (the service: the
    client cancelled, or went away). Not a MISS: nobody is waiting."""


class Loss(enum.StrEnum):
    """The POOL ended a guest while it was leased: the test reports MISS."""

    CLOSED = "closed"                    # close()
    VIRTIOFSD = "virtiofsd-exited"       # the share's daemon died
    SWEPT = "swept"                      # ended as an orphan / by the host


class Start(enum.StrEnum):
    """How the leased guest came to be at `clean`."""

    RESTORED = "restored"                # an idle resident guest, reset after its last lease
    COLD = "cold"                        # a stopped guest, cold-started from its state
    BOOTED = "booted"                    # a new guest, booted and snapshotted


class ReleaseOutcome(enum.StrEnum):
    """What release() found and did."""

    RETURNED = "returned"                # healthy: handed back, reset in the background
    REPLACED = "replaced"                # dead, hung or misused while leased: a new one stands in
    LOST = "lost"                        # the pool had ended it (lease.lost says why)


class Cause(enum.StrEnum):
    """Why a released guest was replaced."""

    QEMU_EXITED = "qemu-exited"          # detail: its exit status or signal
    GUEST_HUNG = "guest-hung"            # QMP answers, the vCPUs are not running
    AGENT_UNRESPONSIVE = "agent-unresponsive"  # running, but /ping timed out
    EXPIRED_HOLD = "expired-hold"        # held past max_hold_s
    CLIENT_GONE = "client-gone"          # its client's connection closed while holding it
    GUEST_RESET = "guest-reset"          # QMP RESET: the guest left its clean state by a reset
                                         # (a test's `reset`, or a QMP system_reset it must not send)
    RESTORE_FAILED = "restore-failed"    # a mid-lease restore() failed: the lease was taken back


@dataclass(frozen=True)
class ReleaseResult:
    """What release() found and did."""

    outcome: ReleaseOutcome
    cause: Cause | None = None           # REPLACED: why
    loss: Loss | None = None             # LOST: how the pool ended it
    detail: str = ""
    kept: Path | None = None             # keep_files: where the lease's files went


@dataclass(frozen=True)
class Lease:
    """One guest held by one test: plain data, serialisable."""

    id: str                              # unique in the pool's life
    token: str                           # unguessable: release() requires it
    class_name: str
    arch: str
    key: str                             # the class key (klass.class_key)
    guest: str                           # the guest's id within the pool
    start: Start
    start_ms: int                        # wait + boot/cold-start/restore, as this lease saw it
    agent_addr: tuple[str, int]          # ("127.0.0.1", port)
    qmp_socket: Path
    host_dir: Path                       # the lease's directory, on the host
    guest_dir: str                       # the same, as the guest names it: "fs1:\\lease-3f9a"
    serial_log: Path                     # this QEMU process's serial log
    serial_offset: int                   # where this lease's part of it begins
    trace_id: str = ""                   # OpenTelemetry trace id: one trace per lease (§6.4)
    agent_image: str = ""                # the agent image the guest runs (agent() is its client)
    qemu_pid: int = 0                    # the guest's QEMU, for a harness that samples it
    restore_token: str = ""              # unguessable: restore() requires it (env() carries it)

    def agent(self, timeout: float = 60.0) -> AgentClient:
        """A client for the lease's agent; @timeout bounds every socket wait
        (a caller that expects no answer -- a line that resets the guest --
        passes a short one instead of waiting the default out)."""
        return AgentClient(*self.agent_addr, timeout=timeout)

    def serial(self) -> bytes:
        """The guest's serial output from this lease's start."""
        with self.serial_log.open("rb") as f:
            f.seek(self.serial_offset)
            return f.read()

    def env(self) -> dict[str, str]:
        """AXL_POOL_AGENT (host:port), AXL_POOL_QMP, AXL_POOL_HOST_DIR,
        AXL_POOL_GUEST_DIR, AXL_POOL_SERIAL, AXL_POOL_SERIAL_OFFSET,
        AXL_POOL_LEASE, AXL_POOL_CLASS, AXL_POOL_ARCH, AXL_POOL_TRACE_ID,
        AXL_POOL_AGENT_IMAGE, AXL_POOL_QEMU_PID, AXL_POOL_START,
        AXL_POOL_START_MS: what a test script needs, as environment. The pid is
        the guest's QEMU process (on this host, the service's user), for a
        harness that samples it -- the quiet slot's vCPU-starvation witness
        (scripts/qloop-verdict.sh); 0 if unknown. START is how the guest came
        to be clean (booted|restored|cold) and START_MS what that cost this
        lease, wait included -- for the test's TIMING summary (common-test.sh).
        AXL_POOL_RESTORE_TOKEN is the lease's restore capability (restore():
        a fresh machine mid-lease); the release token is never in it."""
        return {
            "AXL_POOL_AGENT": f"{self.agent_addr[0]}:{self.agent_addr[1]}",
            "AXL_POOL_QMP": str(self.qmp_socket),
            "AXL_POOL_HOST_DIR": str(self.host_dir),
            "AXL_POOL_GUEST_DIR": self.guest_dir,
            "AXL_POOL_SERIAL": str(self.serial_log),
            "AXL_POOL_SERIAL_OFFSET": str(self.serial_offset),
            "AXL_POOL_LEASE": self.id,
            "AXL_POOL_CLASS": self.class_name,
            "AXL_POOL_ARCH": self.arch,
            "AXL_POOL_TRACE_ID": self.trace_id,
            "AXL_POOL_AGENT_IMAGE": self.agent_image,
            "AXL_POOL_QEMU_PID": str(self.qemu_pid),
            "AXL_POOL_START": str(self.start),
            "AXL_POOL_START_MS": str(self.start_ms),
            "AXL_POOL_RESTORE_TOKEN": self.restore_token,
        }


@dataclass(frozen=True)
class GuestInfo:
    """One guest as Pool.guests() reports it."""

    guest: str
    class_name: str
    arch: str
    status: str                      # booting, idle, leased, resetting, stopped, retired
    key: str
    tree: str = ""                   # the tree whose class this guest is


class _Status(enum.Enum):
    BOOTING = "booting"      # being booted or cold-started for a lease
    IDLE = "idle"            # running at `clean`, leasable
    LEASED = "leased"
    RESETTING = "resetting"  # released; its class's reset runs in the background
    STOPPING = "stopping"    # being stopped (idle, evicted, held too long): not leasable
    STOPPED = "stopped"      # no process; state kept, a lease cold-starts it
    RETIRED = "retired"      # never leased again; terminal


RUNNING = (_Status.BOOTING, _Status.IDLE, _Status.LEASED, _Status.RESETTING, _Status.STOPPING)


@dataclass
class _Slot:
    """One guest the pool keeps: its directory, process and status. Its
    status changes only through Pool._to()."""

    id: str
    class_name: str
    arch: str
    key: str
    dir: Path
    status: _Status
    guest: Guest | None = None
    snapshot_at: float = 0.0          # when its `clean` snapshot was taken (time.time())
    last_used: float = 0.0            # last became IDLE (time.monotonic())
    retire_on_release: bool = False   # revalidate() found its key stale while busy
    evict: list[_Slot] = field(default_factory=lambda: [])  # IDLE guests to stop before this one starts
    tree: Path = Path()               # the checkout whose machine it is (service.py TREES)
    agent: Path = Path()              # the agent image it runs (part of its queue key)
    ram_mb: int = DEFAULT_GUEST_RAM_MB  # its class's guest RAM: what mem_cap_mb counts it as

    @property
    def qkey(self) -> QKey:
        return (self.tree, self.class_name, self.arch, self.agent)

    @property
    def state(self) -> Path:
        return self.dir / "state"

    @property
    def share(self) -> Path:
        return self.dir / "share"


@dataclass
class _Held:
    lease: Lease
    slot: _Slot
    granted_at: float                 # time.monotonic()


class _Retry(Exception):
    """Choose a guest again: the one reserved could not be made ready."""

    def __init__(self, unhealthy: bool) -> None:
        super().__init__()
        self.unhealthy = unhealthy


def _procs_naming(d: Path) -> list[int]:
    """Every QEMU or virtiofsd with an argument under @d -- `d/`, so not a
    sibling directory that shares the prefix."""
    mine = str(d).encode() + b"/"
    found: list[int] = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if args and (b"qemu-system" in args[0] or b"virtiofsd" in args[0]) \
                and any(mine in a for a in args):
            found.append(int(proc.name))
    return found


class _Spawner:
    """Long-lived DAEMON threads that start every guest: PR_SET_PDEATHSIG
    ties a guest to the thread that spawned it, so the threads must live as
    long as the pool -- and, unlike ThreadPoolExecutor's (joined at
    interpreter exit), never keep the process alive for a boot that
    outlived close()'s deadline. Its guest dies with the process instead."""

    def __init__(self, n: int) -> None:
        self._q: queue.SimpleQueue[tuple[Future[Any], Callable[..., Any], tuple[Any, ...]] | None] = \
            queue.SimpleQueue()
        self._threads = [threading.Thread(target=self._work, name=f"axlpool-{i}", daemon=True)
                         for i in range(n)]
        for t in self._threads:
            t.start()

    def submit(self, fn: Callable[..., Any], *args: Any) -> Future[Any]:
        fut: Future[Any] = Future()
        self._q.put((fut, fn, args))
        return fut

    def shutdown(self) -> None:
        """Workers exit once idle; nothing waits for one still busy."""
        for _ in self._threads:
            self._q.put(None)

    def _work(self) -> None:
        while (job := self._q.get()) is not None:
            fut, fn, args = job
            if not fut.set_running_or_notify_cancel():
                continue
            try:
                fut.set_result(fn(*args))
            except BaseException as e:  # noqa: BLE001 -- handed to the waiter
                fut.set_exception(e)


def _stop_or_kill(g: Guest) -> None:
    try:
        g.stop()
    except Exception:  # noqa: BLE001 -- stop failed: kill
        g.kill()


def _file_sig(path: Path) -> FileSig:
    try:
        st = path.stat()
    except OSError:
        return (0, 0)                    # missing: its dry run fails and says why
    return (st.st_mtime_ns, st.st_size)


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class Pool:
    """Guests by class, leased one test at a time. Thread-safe."""

    def __init__(self, classes: ClassSet, agent: Path | Mapping[str, Path], root: Path, *,
                 idle_timeout_s: float = 600.0, mem_cap_mb: int | None = None,
                 max_hold_s: float | None = None, log: LogFn | None = None,
                 lease_prefix: str = "") -> None:
        """@agent: the PINNED agent image the guests run (§5) -- per arch, or
        one path for every arch; a lease for an arch with none misses NO_AGENT. @root: the
        pool's state, a directory per guest, owned by ONE live pool: a second
        Pool on it raises RuntimeError("another pool owns <root>") rather than
        sweeping the first one's guests. Sweeps orphans naming @root, made
        absolute first: a relative root would match OTHER roots' paths."""
        root = root.resolve()
        # Trees served: one pool for every checkout, so the memory cap, the
        # reaper and the spawner threads are shared (service.py TREES).
        self._trees: dict[Path, ClassSet] = {classes.tree: classes}
        self._default_tree = classes.tree
        # The agent image each arch's guests run (one path: every arch's).
        self._agents: dict[str, Path] = ({a: agent for a in ARCHES} if isinstance(agent, Path)
                                         else dict(agent))
        self._root = root
        self.idle_timeout_s = idle_timeout_s
        self.mem_cap_mb = mem_cap_mb
        self.max_hold_s = max_hold_s
        self._log_fn = log
        self._lease_prefix = lease_prefix   # a service's instance: ids that join its logs
        root.mkdir(parents=True, exist_ok=True)
        # Held for the pool's life; the kernel drops it if the process dies,
        # which is exactly when the sweep below is safe. Not inherited.
        self._lock_fd = os.open(root / ".lock", os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self._lock_fd)
            raise RuntimeError(f"another pool owns {root}") from None
        # Long-lived threads start every guest: PR_SET_PDEATHSIG ties a
        # cold-started guest to the thread that spawned it.
        self._spawner = _Spawner(SPAWNER_THREADS)
        self._cond = threading.Condition()
        self._slots: list[_Slot] = []
        self._keys: dict[QKey, tuple[FileSig, str]] = {}   # with the agent image it was keyed on
        # Each FILE-BACKED tree's classes.toml, as last read (_tree_classes):
        # a ClassSet handed in from memory (loads(), not load()) is no file's
        # and is never re-read.
        self._tree_sigs: dict[Path, FileSig] = {}
        self._note_file(classes)
        self._boot_failed: dict[str, str] = {}
        self._queues: dict[QKey, deque[object]] = {}
        self._held: dict[str, _Held] = {}
        self._releasing: set[str] = set()
        self._done: OrderedDict[str, tuple[str, ReleaseResult]] = OrderedDict()
        self._futures: set[Future[Any]] = set()
        self._expired: set[str] = set()      # leases taken back (max_hold_s, expire())
        self._expire_cause: dict[str, Cause] = {}
        self._closed = False
        # Lease threads holding a boot or cold start whose guest is not yet
        # in its slot: close() waits for them, since such a thread is the
        # one that ends its guest if the pool closed meanwhile.
        self._starting = 0
        try:
            self._sweep_orphans()
            self._load_slots()
        except BaseException:
            self._spawner.shutdown()
            os.close(self._lock_fd)      # a retry in this process must not read "another pool"
            raise
        self._reaper = threading.Thread(target=self._reap, name="axlpool-reaper", daemon=True)
        self._reaper.start()

    # -- lifetimes ------------------------------------------------------------

    def _sweep_orphans(self) -> None:
        """End every QEMU or virtiofsd naming this root: what a pool that
        crashed MID-BOOT left (run-qemu.sh's guest, under its `timeout`)."""
        victims = _procs_naming(self._root)
        for pid in victims:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        end = time.monotonic() + 10
        while time.monotonic() < end and any(pid_alive(v) for v in victims):
            time.sleep(0.05)
        if victims:
            self._log("WARN", "swept orphans", detail=f"{len(victims)} process(es) naming {self._root}")

    def _load_slots(self) -> None:
        """Guests an earlier pool left on this root, as STOPPED: a lease of a
        matching key cold-starts one instead of booting. Any other guest
        directory -- a start that never finished, a malformed slot.json, a
        tree that no longer exists -- can never serve and is deleted (the
        root is ours: the lock is held and the sweep has run)."""
        for d in sorted(self._root.glob("*/*")):
            if not d.is_dir() or d.parent.name == KEPT_DIR:
                continue
            slot = self._read_slot(d)
            if slot is not None:
                self._slots.append(slot)
            else:
                self._delete_dir(d)
        # Key directories left empty (here, or by the reaper, which leaves
        # them since a boot may be creating a sibling in one).
        for k in self._root.iterdir():
            if k.is_dir() and k.name != KEPT_DIR and not any(k.iterdir()):
                k.rmdir()

    @staticmethod
    def _read_slot(d: Path) -> _Slot | None:
        """The STOPPED slot @d's slot.json describes, or None if it cannot serve."""
        try:
            m = json.loads((d / "slot.json").read_text())
            slot = _Slot(str(m["id"]), str(m["class"]), str(m["arch"]), str(m["key"]),
                         d, _Status.STOPPED, snapshot_at=float(m["snapshot_at"]),
                         tree=Path(str(m["tree"])), agent=Path(str(m["agent"])))
        except (OSError, ValueError, KeyError, TypeError):
            return None
        if not (d / "state" / "launch.json").exists() or not slot.tree.is_dir():
            return None
        return slot

    @staticmethod
    def _delete_dir(d: Path) -> None:
        """Delete a guest directory; its key directory stays (pruned at load)."""
        shutil.rmtree(d, ignore_errors=True)

    def _reap_tick(self) -> float:
        ticks = [5.0, self.idle_timeout_s / 4]
        if self.max_hold_s is not None:
            ticks.append(self.max_hold_s / 4)
        return max(0.2, min(ticks))

    def _reap(self) -> None:
        """Stop guests idle past idle_timeout_s; take back leases held past
        max_hold_s. Runs until close()."""
        while True:
            with self._cond:
                if self._closed:
                    return
                self._cond.wait(self._reap_tick())
                if self._closed:
                    return
                now = time.monotonic()
                idle = [sl for sl in self._slots if sl.status is _Status.IDLE
                        and self._keep(sl) == "idle"
                        and now - sl.last_used >= self.idle_timeout_s]
                held = [h for lid, h in self._held.items()
                        if self.max_hold_s is not None and lid not in self._expired
                        and lid not in self._releasing and now - h.granted_at >= self.max_hold_s]
                for sl in idle:
                    self._to(sl, _Status.STOPPING)
                for h in held:
                    self._expired.add(h.lease.id)
                retired = [sl for sl in self._slots if sl.status is _Status.RETIRED and sl.guest is None]
            self._collect(retired)
            for sl in idle:
                self._stop_slot(sl, "idle timeout")
            for h in held:
                self._log("WARN", "lease held too long", **self._slot_attrs(h.slot), lease=h.lease.id,
                          cause=Cause.EXPIRED_HOLD.value)
                g = h.slot.guest
                if g is not None:
                    g.kill()      # the holder may be mid-command: replaced, not restored

    def _collect(self, retired: list[_Slot]) -> None:
        """Delete RETIRED slots' state and forget them: they never serve
        again. One whose directory a process still names waits a tick."""
        done: list[_Slot] = []
        for sl in retired:
            if not _procs_naming(sl.dir):
                self._delete_dir(sl.dir)
                done.append(sl)
        if done:
            with self._cond:
                self._slots = [sl for sl in self._slots if sl not in done]

    def _stop_slot(self, slot: _Slot, why: str) -> None:
        """Stop a STOPPING slot's guest, its state kept for a cold start."""
        g = slot.guest
        if g is not None:
            try:
                g.stop()
            except Exception:  # noqa: BLE001 -- a stop that fails is a kill
                g.kill()
        with self._cond:
            slot.guest = None
            if slot.status is _Status.STOPPING:
                self._to(slot, _Status.STOPPED)
        self._log("INFO", "stopped", **self._slot_attrs(slot), detail=why)

    def _rss_mb(self, slot: _Slot) -> int:
        g = slot.guest
        if g is None:
            return 0
        try:
            for line in Path(f"/proc/{g.qemu_pid}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) // 1024
        except (OSError, ValueError):
            pass
        return 0

    def _evict_for(self, need_mb: int, starting: _Slot) -> list[_Slot] | None:
        """(cond held) Least-recently-used IDLE guests to stop so @need_mb
        more fits mem_cap_mb: [] when it fits, None when it cannot."""
        if self.mem_cap_mb is None:
            return []
        # A guest still booting has no RSS yet but will: count every running
        # guest as at least its class's RAM, or concurrent starts all pass
        # the check (and a 768M class must not count as the 512M default).
        used = sum(max(self._rss_mb(sl), sl.ram_mb) for sl in self._slots
                   if sl.status in RUNNING and sl is not starting)   # not the one asking
        victims: list[_Slot] = []
        for sl in sorted((x for x in self._slots if x.status is _Status.IDLE),
                         key=lambda x: x.last_used):
            if used + need_mb <= self.mem_cap_mb:
                break
            victims.append(sl)
            used -= max(self._rss_mb(sl), sl.ram_mb)
        return victims if used + need_mb <= self.mem_cap_mb else None

    # -- helpers ------------------------------------------------------------

    def _log(self, severity: str, body: str, **attrs: object) -> None:
        if self._log_fn is None:
            return
        rec: dict[str, object] = {"timestamp": _now_iso(), "severity_text": severity, "body": body}
        rec.update({k if k in OTEL_FIELDS else f"axl.{k}": v for k, v in attrs.items()})
        self._log_fn(rec)

    def _slot_attrs(self, slot: _Slot) -> dict[str, object]:
        return {"class": slot.class_name, "arch": slot.arch, "guest": slot.id, "key": slot.key}

    def _note_file(self, classes: ClassSet) -> None:
        """Record @classes's file signature when it was read FROM its tree's
        classes.toml (load(), whose source is that path, however spelled) --
        the sets _tree_classes re-reads when the file changes."""
        path = classes.tree / "test" / "pool" / "classes.toml"
        try:
            backed = Path(classes.source).resolve() == path.resolve()
        except OSError:
            backed = False
        if backed:
            with self._cond:
                self._tree_sigs[classes.tree] = _file_sig(path)

    def _tree_classes(self, tree: Path) -> ClassSet:
        """The ClassSet of @tree, loaded from <tree>/test/pool/classes.toml on
        first use and AGAIN whenever the file's signature changes -- an edited
        classes file reaches the pool without a restart, as a rebuilt agent
        image does. A change to a tree already known goes through
        revalidate(): its guests whose class key changed retire (KEYS).
        Raises ClassError."""
        tree = tree.resolve()            # one spelling: the ClassSet's own (load())
        path = tree / "test" / "pool" / "classes.toml"
        sig = _file_sig(path)            # (0, 0) when missing: load() below says why
        with self._cond:
            cached = self._trees.get(tree)
            if cached is not None and (tree not in self._tree_sigs or self._tree_sigs[tree] == sig):
                return cached            # in memory (never re-read), or the file unchanged
        try:
            classes = load(path)         # unlocked: a file read
        except OSError as e:
            raise ClassError(f"{path}: cannot read the classes file ({e.strerror})") from None
        if cached is not None:
            self.revalidate(classes)     # takes the lock; stores the set, retires changed keys
        with self._cond:
            self._trees[tree] = classes
            self._tree_sigs[tree] = sig
        return classes

    def _keep(self, slot: _Slot) -> str:
        try:
            return self._trees[slot.tree].get(slot.class_name).keep
        except (KeyError, ClassError):
            return "idle"

    def _key(self, k: QKey) -> str:
        """@k's class key, recomputed when its agent image changed on disk: a
        rebuilt agent at the same path is another machine (its idle guests
        run the old build)."""
        sig = _file_sig(k[3])
        with self._cond:
            cached = self._keys.get(k)
        if cached is None or cached[0] != sig:
            tree, class_name, arch, agent = k
            classes = self._tree_classes(tree)
            cached = (sig, class_key(classes.spec(class_name, arch, agent)))   # a dry run: unlocked
            with self._cond:
                # Stored only if revalidate() has not replaced the classes
                # meanwhile -- else every waiter would adopt a stale key.
                if self._trees.get(tree) is classes:
                    self._keys[k] = cached
        return cached[1]

    def _to(self, slot: _Slot, status: _Status) -> bool:
        """(cond held) The ONE way a slot's status changes: never out of
        RETIRED. True when the change was made."""
        if slot.status is _Status.RETIRED:
            return False
        slot.status = status
        if status is _Status.IDLE:
            slot.last_used = time.monotonic()
        self._cond.notify_all()
        return True

    def _retire(self, slot: _Slot) -> None:
        """(cond held) Never lease @slot again; end its processes."""
        g, slot.guest = slot.guest, None
        slot.status = _Status.RETIRED
        self._cond.notify_all()
        if g is not None:
            g.kill()

    def _submit(self, fn: Callable[..., Any], *args: Any) -> Future[Any]:
        """Run @fn on a pool thread. LeaseMiss POOL_CLOSED once closed."""
        with self._cond:
            if self._closed:
                raise LeaseMiss(MissReason.POOL_CLOSED, "the pool is closed")
            fut = self._spawner.submit(fn, *args)
            self._futures.add(fut)
        fut.add_done_callback(self._forget)
        return fut

    def _forget(self, fut: Future[Any]) -> None:
        with self._cond:
            self._futures.discard(fut)

    @staticmethod
    def _healthy(g: Guest, *, ask_agent: bool = True) -> tuple[Cause | None, str]:
        """None if QEMU runs, its vCPUs run and (@ask_agent) the agent
        answers; else why not."""
        if not g.alive():
            return Cause.QEMU_EXITED, g.exit_detail()
        # Its QMP events say what the guest did, exactly and at once -- no
        # probe to time out (an IDLE guest at lease, a held one at release).
        events = g.guest_events()
        if "RESET" in events:
            return Cause.GUEST_RESET, f"QMP {' '.join(events)} since its clean state"
        try:
            status = g.qmp_status()
        except Exception as e:  # noqa: BLE001 -- QMP gone or silent with QEMU alive
            return Cause.GUEST_HUNG, f"QMP: {type(e).__name__}"
        if status != "running":
            return Cause.GUEST_HUNG, f"QMP status {status}"
        if not ask_agent:
            return None, ""
        # A probe: one attempt, so the verdict comes within the deadline
        # (a ping() would retry, waiting it twice).
        probe = AgentClient("127.0.0.1", g.port)
        try:
            if probe.probe(HEALTH_DEADLINE_S, cap_s=HEALTH_DEADLINE_S) != "pong\n":
                return Cause.AGENT_UNRESPONSIVE, "unexpected /ping answer"
        except Exception as e:  # noqa: BLE001 -- not answering is the finding
            return Cause.AGENT_UNRESPONSIVE, f"/ping: {type(e).__name__}"
        return None, ""

    def _loss(self, slot: _Slot) -> tuple[Loss | None, str]:
        """Did the POOL's side end this guest? A dead virtiofsd. (Every guest
        is the pool's own child, with no lifetime of its own to run out.)"""
        g = slot.guest
        if g is None:
            return None, ""
        if not g.alive():
            # QEMU gone: not the pool's doing. (Its virtiofsd exits BECAUSE
            # QEMU went: no finding there.)
            return None, ""
        if any(not pid_alive(v) for v in g.virtiofsd_pids):
            return Loss.VIRTIOFSD, "the share's virtiofsd exited under a live QEMU"
        return None, ""

    def _wipe_share(self, slot: _Slot) -> None:
        if not slot.share.is_dir():
            return
        for p in slot.share.iterdir():
            if p.name == SHARE_PROBE:
                continue
            if p.is_dir() and not p.is_symlink():
                shutil.rmtree(p, ignore_errors=True)
            else:
                p.unlink(missing_ok=True)

    # -- lease ----------------------------------------------------------------

    def lease(self, class_name: str, arch: str, *, timeout: float = 300.0,
              booted_after: float | None = None,
              cancel: threading.Event | None = None, tree: Path | None = None,
              trace_id: str = "", agent: Path | None = None) -> Lease:
        """A healthy guest of @class_name on @arch at `clean`, with an empty
        share directory, within ONE deadline of @timeout seconds. Raises
        LeaseMiss (OUTCOMES)."""
        t0 = time.monotonic()
        deadline = t0 + timeout
        try:
            t = tree.resolve() if tree is not None else self._default_tree
            classes = self._tree_classes(t)
            gclass = classes.get(class_name)
            image = agent if agent is not None else self._agents.get(arch)
            if image is None:
                raise LeaseMiss(MissReason.NO_AGENT, f"no agent image pinned for {arch}")
            qkey: QKey = (t, class_name, arch, image)
            ram_mb = classes.spec(class_name, arch, qkey[3]).ram_mb
            if self.mem_cap_mb is not None and ram_mb > self.mem_cap_mb:
                # Eviction cannot make room that is not there: missed now,
                # said, before the class key's dry run -- not waited out to a
                # reasonless no-slot at the deadline.
                raise LeaseMiss(MissReason.OVER_CAP,
                                f"class {class_name} needs {ram_mb} MiB, over the pool's memory "
                                f"cap of {self.mem_cap_mb} MiB")
            key = self._key(qkey)
        except ClassError as e:
            raise LeaseMiss(MissReason.UNKNOWN_CLASS, str(e)) from None
        ticket = object()
        with self._cond:
            if self._closed:
                raise LeaseMiss(MissReason.POOL_CLOSED, "the pool is closed")
            queue = self._queues.setdefault(qkey, deque())
            queue.append(ticket)
        unhealthy = False
        try:
            while True:
                slot, action = self._reserve(qkey, key, gclass.slots, ram_mb, ticket, deadline,
                                             booted_after, unhealthy, cancel)
                try:
                    start = self._make_ready(slot, action, deadline, cancel)
                    return self._grant(slot, start, t0, trace_id)
                except _Retry as again:
                    unhealthy = unhealthy or again.unhealthy
                    with self._cond:
                        queue.appendleft(ticket)    # keep our place
                except (LeaseMiss, LeaseCancelled):
                    raise                       # the slot is already safe (_late owns a start)
                except BaseException:
                    self._abandon(slot)
                    raise
        finally:
            with self._cond:
                if ticket in queue:
                    queue.remove(ticket)
                self._cond.notify_all()

    def _reserve(self, qkey: QKey, key: str, slots: int, ram_mb: int, ticket: object,
                 deadline: float, booted_after: float | None,
                 unhealthy: bool, cancel: threading.Event | None = None) -> tuple[_Slot, str]:
        """Wait for our turn and a guest: an IDLE one ("idle"), a STOPPED one
        to cold-start ("cold"), or room for a new one ("boot")."""
        queue = self._queues[qkey]
        resetting_since: float | None = None
        with self._cond:
            while True:
                if self._closed:
                    raise LeaseMiss(MissReason.POOL_CLOSED, "the pool closed")
                if cancel is not None and cancel.is_set():
                    raise LeaseCancelled()
                # The CURRENT key: revalidate() may have changed it while we
                # waited, and a guest of the key we started with is stale now.
                cur = self._keys.get(qkey)
                key = cur[1] if cur is not None else key
                if key in self._boot_failed:
                    raise LeaseMiss(MissReason.BOOT_FAILED, self._boot_failed[key])
                wait_for = deadline - time.monotonic()
                if queue and queue[0] is ticket:
                    mine = [s for s in self._slots if s.qkey == qkey]
                    for s in mine:
                        if s.status is _Status.STOPPED and s.key != key:
                            self._retire(s)    # an OLD key's state: never serves again
                    usable = [s for s in mine if s.key == key and not s.retire_on_release
                              and (booted_after is None or s.snapshot_at >= booted_after)]
                    running = sum(1 for s in mine if s.status in RUNNING)
                    pick = next((s for s in usable if s.status is _Status.IDLE), None)
                    action = "idle"
                    # A guest mid-reset (~0.2 s) is worth a short wait before
                    # a cold start or a boot (10-22 s) -- but only a short one.
                    if pick is None and any(s.status is _Status.RESETTING for s in usable):
                        resetting_since = resetting_since or time.monotonic()
                        grace = resetting_since + RESET_GRACE_S - time.monotonic()
                        if grace > 0:
                            wait_for = min(wait_for, grace)
                            pick, action = None, "wait"
                    if pick is None and action != "wait":
                        if running < slots:
                            pick = next((s for s in usable if s.status is _Status.STOPPED), None)
                            action = "cold"
                            if pick is None:
                                sid = f"g{secrets.token_hex(4)}"
                                pick = _Slot(sid, qkey[1], qkey[2], key,
                                             self._root / key[:16] / sid, _Status.BOOTING,
                                             tree=qkey[0], agent=qkey[3])
                                self._slots.append(pick)
                                action = "boot"
                        else:
                            # Room only by retiring an IDLE guest that cannot
                            # serve this lease (stale for booted_after, old key).
                            stale = next((s for s in mine if s.status is _Status.IDLE
                                          and s not in usable), None)
                            if stale is not None:
                                self._retire(stale)
                                continue
                    if pick is not None and action in ("cold", "boot"):
                        pick.ram_mb = ram_mb     # a slot read back from disk did not know it
                        victims = self._evict_for(ram_mb, pick)
                        if victims is None:      # only leased guests: wait for room
                            if action == "boot":
                                self._slots.remove(pick)
                            pick = None
                        else:
                            for v in victims:
                                self._to(v, _Status.STOPPING)
                            pick.evict = victims
                    if pick is not None:
                        queue.popleft()
                        self._to(pick, _Status.LEASED if action == "idle" else _Status.BOOTING)
                        return pick, action      # _to() woke the next waiter
                if deadline - time.monotonic() <= 0:
                    if unhealthy:
                        raise LeaseMiss(MissReason.UNHEALTHY,
                                        "guests failed the health check; none replaced in time")
                    raise LeaseMiss(MissReason.NO_SLOT,
                                    f"no {qkey[1]}/{qkey[2]} guest within the deadline")
                # A cancel event wakes nobody: wait in short steps while one can arrive.
                self._cond.wait(max(min(wait_for, CANCEL_POLL_S) if cancel is not None else wait_for, 0.0))

    def _make_ready(self, slot: _Slot, action: str, deadline: float,
                    cancel: threading.Event | None = None) -> Start:
        """Bring @slot to a healthy `clean`. Raises _Retry to choose again,
        LeaseMiss when the deadline passes or the class fails; leaves @slot
        in a safe status either way."""
        if action in ("boot", "cold"):
            with self._cond:
                victims, slot.evict = slot.evict, []
                self._starting += 1
            try:
                for v in victims:                # room first: the LRU idle guests stop
                    self._stop_slot(v, "evicted under mem_cap_mb")
                g = self._start(slot, action == "boot", deadline, cancel)
            finally:
                with self._cond:
                    self._starting -= 1
                    self._cond.notify_all()
        else:
            with self._cond:
                found = slot.guest
            if found is None:               # stopped under us (close, the reaper)
                raise _Retry(unhealthy=False)
            g = found
        cause, detail = self._healthy(g)
        if cause is None:
            return {"boot": Start.BOOTED, "cold": Start.COLD}.get(action, Start.RESTORED)
        self._log("WARN", "unhealthy at lease", **self._slot_attrs(slot), cause=cause.value,
                  detail=detail)
        with self._cond:
            g, slot.guest = slot.guest, None
            # A guest just started from its state and already unhealthy says
            # the state is bad: retire it. An idle one cold-starts next.
            if action == "idle":
                self._to(slot, _Status.STOPPED)
            else:
                self._retire(slot)
        if g is not None:
            g.kill()
        raise _Retry(unhealthy=True)

    @staticmethod
    def _await_start(fut: Future[Any], deadline: float, cancel: threading.Event | None) -> Guest:
        """@fut's guest; FutureTimeout at @deadline, LeaseCancelled as soon as
        @cancel is set (in steps: an Event wakes nobody)."""
        while True:
            left = deadline - time.monotonic()
            step = left if cancel is None else min(left, CANCEL_POLL_S)
            try:
                return cast(Guest, fut.result(timeout=max(step, 0.0)))
            except FutureTimeout:
                if cancel is not None and cancel.is_set():
                    raise LeaseCancelled() from None
                if deadline - time.monotonic() <= 0:
                    raise

    def _start(self, slot: _Slot, booting: bool, deadline: float,
               cancel: threading.Event | None = None) -> Guest:
        """Boot or cold-start @slot's guest and put it in the slot. Counted in
        _starting by the caller: if the pool closes meanwhile, THIS thread
        ends the guest, and close() waits for it to."""
        if booting:
            spec = self._trees[slot.tree].spec(slot.class_name, slot.arch, slot.agent)
            fut = self._submit(Guest.boot, spec, slot.state, slot.share)
        else:
            fut = self._submit(Guest.cold_start, slot.state, slot.share)
        try:
            g = self._await_start(fut, deadline, cancel)
        except (FutureTimeout, LeaseCancelled) as e:
            # It finishes after our deadline or our cancel: it joins the pool then.
            fut.add_done_callback(lambda f: self._late(slot, f, booting))
            if isinstance(e, LeaseCancelled):
                raise
            raise LeaseMiss(MissReason.NO_SLOT,
                            f"the {'boot' if booting else 'cold'} did not finish within the deadline") from None
        except Exception as e:  # noqa: BLE001 -- the boot or cold start failed
            if isinstance(e, HandoverFailed):
                self._handover_failed(slot, e)
                raise _Retry(unhealthy=False) from None
            self._failed_start(slot, booting, e)
            if booting:
                raise LeaseMiss(MissReason.BOOT_FAILED, self._boot_failed[slot.key]) from None
            raise _Retry(unhealthy=False) from None
        with self._cond:
            closed = self._closed
            if not closed:
                slot.guest = g
                if booting:
                    slot.snapshot_at = time.time()
        if closed:
            # close() ran while this guest started: it is ours to end.
            g.stop()
            with self._cond:
                self._retire(slot)
            raise LeaseMiss(MissReason.POOL_CLOSED, "the pool closed while the guest started")
        if booting:
            self._write_meta(slot)
        if not g.events_live:
            self._log("WARN", "QMP events unavailable", **self._slot_attrs(slot),
                      detail="its health check falls back to probes alone")
        self._log("INFO", "booted" if booting else "cold-started",
                  **self._slot_attrs(slot), duration_ms=int(g.boot_s * 1000))
        return g

    @staticmethod
    def _write_meta(slot: _Slot) -> None:
        """slot.json: what a later pool on this root needs to reuse the state."""
        (slot.dir / "slot.json").write_text(json.dumps({
            "id": slot.id, "class": slot.class_name, "arch": slot.arch, "key": slot.key,
            "snapshot_at": slot.snapshot_at, "tree": str(slot.tree), "agent": str(slot.agent)}))

    def _handover_failed(self, slot: _Slot, e: BaseException) -> None:
        """A boot that failed only in its hand-over: the class boots and its
        state is good (snapshot, launch.json), so the slot is kept STOPPED --
        the next lease cold-starts it (~0.4 s) instead of booting again. A
        hand-over that keeps failing then fails as a cold start, and retires."""
        with self._cond:
            slot.guest = None
            slot.snapshot_at = time.time()
            self._to(slot, _Status.STOPPED)
        self._write_meta(slot)
        self._log("WARN", "hand-over failed", **self._slot_attrs(slot),
                  detail=f"{type(e).__name__}: {str(e)[-400:]}")

    def _failed_start(self, slot: _Slot, booting: bool, e: BaseException) -> None:
        detail = f"{type(e).__name__}: {str(e)[-400:]}"
        with self._cond:
            if booting:
                self._boot_failed[slot.key] = detail   # until revalidate()
            self._retire(slot)
        self._log("ERROR", "boot failed" if booting else "cold start failed",
                  **self._slot_attrs(slot), detail=detail)

    def _late(self, slot: _Slot, fut: Future[Any], booting: bool) -> None:
        """A boot or cold start that outlived its lease's deadline: the guest
        joins the pool IDLE, or is ended if the pool closed or retired it."""
        try:
            g = cast(Guest, fut.result())
        except Exception as e:  # noqa: BLE001 -- a failed late start, cached like any other
            if isinstance(e, HandoverFailed):
                self._handover_failed(slot, e)
            else:
                self._failed_start(slot, booting, e)
            return
        with self._cond:
            slot.guest = g
            if booting:
                slot.snapshot_at = time.time()
            keep = not self._closed and not slot.retire_on_release and self._to(slot, _Status.IDLE)
            if not keep:
                self._retire(slot)

    def _abandon(self, slot: _Slot) -> None:
        """An unexpected exception while making @slot ready or granting it:
        never leak the slot. Its state, if a clean snapshot was recorded,
        serves a later cold start."""
        with self._cond:
            g, slot.guest = slot.guest, None
            if (slot.state / "launch.json").exists() and not slot.retire_on_release:
                self._to(slot, _Status.STOPPED)
            else:
                self._retire(slot)
        if g is not None:
            g.kill()

    def _grant(self, slot: _Slot, start: Start, t0: float, trace_id: str) -> Lease:
        g = slot.guest
        assert g is not None
        start_ms = int((time.monotonic() - t0) * 1000)
        with self._cond:
            if self._closed:
                self._retire(slot)
                raise LeaseMiss(MissReason.POOL_CLOSED, "the pool closed")
            lid = f"{self._lease_prefix}L{secrets.token_hex(6)}"
            host_dir = slot.share / f"lease-{lid}"
            host_dir.mkdir(parents=True)
            lease = Lease(id=lid, token=secrets.token_hex(16), class_name=slot.class_name,
                          arch=slot.arch, key=slot.key, guest=slot.id, start=start,
                          start_ms=start_ms, agent_addr=("127.0.0.1", g.port),
                          qmp_socket=slot.state / "qmp.sock", host_dir=host_dir,
                          guest_dir=f"{g.share_volume}:\\lease-{lid}", serial_log=g.serial_log,
                          serial_offset=g.serial_log.stat().st_size if g.serial_log.exists() else 0,
                          trace_id=trace_id, agent_image=str(slot.agent), qemu_pid=g.qemu_pid,
                          restore_token=secrets.token_hex(16))
            self._to(slot, _Status.LEASED)
            self._held[lid] = _Held(lease, slot, time.monotonic())
        self._log("INFO", "leased", **self._slot_attrs(slot), lease=lid, outcome=start.value,
                  duration_ms=start_ms, trace_id=trace_id)
        return lease

    # -- release ----------------------------------------------------------------

    def release(self, lease: Lease, *, keep_files: bool = False) -> ReleaseResult:
        """Hand @lease's guest back (RELEASE). Idempotent: a second call
        returns the first's result. A wrong token raises PermissionError."""
        with self._cond:
            while lease.id in self._releasing:
                self._cond.wait()
            done = self._done.get(lease.id)
            if done is not None:
                if not secrets.compare_digest(done[0], lease.token):
                    raise PermissionError(f"no lease {lease.id} with that token")
                return done[1]
            held = self._held.get(lease.id)
            if held is None or not secrets.compare_digest(held.lease.token, lease.token):
                raise PermissionError(f"no lease {lease.id} with that token")
            self._releasing.add(lease.id)
            slot, closed = held.slot, self._closed
        result = ReleaseResult(ReleaseOutcome.LOST, loss=Loss.CLOSED)
        try:
            if not closed:
                result = self._release(lease, slot, keep_files)
        finally:
            with self._cond:
                self._done[lease.id] = (lease.token, result)
                while len(self._done) > RESULTS_KEPT:
                    self._done.popitem(last=False)
                self._held.pop(lease.id, None)
                self._releasing.discard(lease.id)
                self._cond.notify_all()
        self._log("INFO", "released", **self._slot_attrs(slot), lease=lease.id,
                  outcome=result.outcome.value,
                  cause=(result.cause or result.loss or ""), detail=result.detail,
                  duration_ms=int((time.monotonic() - held.granted_at) * 1000),
                  trace_id=held.lease.trace_id)
        return result

    def _release(self, lease: Lease, slot: _Slot, keep_files: bool) -> ReleaseResult:
        kept: Path | None = None
        if keep_files and lease.host_dir.is_dir():
            kept = self._root / KEPT_DIR / lease.id
            kept.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(lease.host_dir), str(kept))
        self._wipe_share(slot)
        loss, detail = self._loss(slot)
        cause: Cause | None = None
        with self._cond:
            expired = lease.id in self._expired
            self._expired.discard(lease.id)
        if expired:
            with self._cond:
                given = self._expire_cause.pop(lease.id, None)
            loss, cause = None, given or Cause.EXPIRED_HOLD
            detail = "taken back" if given else f"held past max_hold_s ({self.max_hold_s} s)"
        if loss is None and cause is None:
            g = slot.guest
            # A snapshot class's release does NOT ask the agent: a snapshot
            # restore replaces ALL guest state, the agent with it, and
            # restore() waits for the agent to answer -- the real health
            # check. Asking first cost a busy agent's whole /ping deadline
            # (a pooled background test leaves its server running and the
            # agent inside it): ~10 s of every such test's own time, measured.
            # QEMU, a guest reset and the vCPUs are still asked, over QMP.
            # (The class is asked about only while it can still serve: a
            # revalidate() that dropped it retires the guest below.)
            restores = not slot.retire_on_release and self._snapshot_class(slot)
            cause, detail = (Cause.QEMU_EXITED, "no process") if g is None \
                else self._healthy(g, ask_agent=not restores)
            if cause is None:
                with self._cond:
                    self._to(slot, _Status.RESETTING)
                try:
                    self._submit(self._reset, slot)
                except LeaseMiss:          # the pool closed under us
                    return ReleaseResult(ReleaseOutcome.LOST, loss=Loss.CLOSED, kept=kept)
                return ReleaseResult(ReleaseOutcome.RETURNED, kept=kept)
        # Replaced (the guest died, hung or went silent) or lost (the pool's
        # side ended it): the clean snapshot is intact -- the next lease
        # cold-starts it -- unless revalidate() wants it gone.
        with self._cond:
            g, slot.guest = slot.guest, None
            if slot.retire_on_release:
                self._retire(slot)
            else:
                self._to(slot, _Status.STOPPED)
        if g is not None:
            g.kill()
        if loss is not None:
            return ReleaseResult(ReleaseOutcome.LOST, loss=loss, detail=detail, kept=kept)
        return ReleaseResult(ReleaseOutcome.REPLACED, cause=cause, detail=detail, kept=kept)

    def _snapshot_class(self, slot: _Slot) -> bool:
        """Whether @slot's class resets by snapshot; False when the class is
        no longer known (a revalidate() dropped it)."""
        try:
            return self._trees[slot.tree].get(slot.class_name).reset == "snapshot"
        except (KeyError, ClassError):
            return False

    def _reset(self, slot: _Slot) -> None:
        """Background: the class's reset, then IDLE (or STOPPED for keep =
        lease); a failure, or a revalidate() meanwhile, retires the guest."""
        g = slot.guest
        try:
            gclass = self._trees[slot.tree].get(slot.class_name)
            assert g is not None
            if slot.retire_on_release:
                raise RuntimeError("its key went stale while it was leased")
            # Every reset mode: a forward lives outside the guest's state.
            dropped = g.drop_forwards()
            if dropped:
                self._log("INFO", "forwards dropped", **self._slot_attrs(slot), detail=" ".join(dropped))
            if gclass.reset == "snapshot":
                g.restore()
            elif gclass.reset == "reset":
                g.system_reset()
            if gclass.keep == "lease":
                g.stop()
            with self._cond:
                if gclass.keep == "lease":
                    slot.guest = None
                    self._to(slot, _Status.STOPPED)
                elif not self._to(slot, _Status.IDLE):
                    self._retire(slot)          # retired meanwhile: end it
        except Exception as e:  # noqa: BLE001 -- not this lease's result: logged, guest retired
            self._log("ERROR", "reset failed", **self._slot_attrs(slot), detail=str(e)[-200:])
            with self._cond:
                self._retire(slot)

    def restore(self, lease: Lease) -> int:
        """RESTORE (module docstring): @lease's guest back to `clean`, the
        lease still held. Returns the milliseconds it took (logged
        `restored`). PermissionError, ValueError, RuntimeError as there."""
        t0 = time.monotonic()
        with self._cond:
            while lease.id in self._releasing:
                self._cond.wait()
            held = self._held.get(lease.id)
            if held is None or lease.id in self._expired or not held.lease.restore_token \
                    or not secrets.compare_digest(held.lease.restore_token, lease.restore_token):
                raise PermissionError(f"no held lease {lease.id} with that restore token")
            slot = held.slot
            if not self._snapshot_class(slot):
                raise ValueError(f"class {slot.class_name} does not reset by snapshot: "
                                 "there is no clean state to restore")
            self._releasing.add(lease.id)       # a release waits for it
        failed: Exception | None = None
        try:
            g = slot.guest
            if g is None:
                raise RuntimeError("the guest has no process")
            dropped = g.drop_forwards()
            if dropped:
                self._log("INFO", "forwards dropped", **self._slot_attrs(slot), detail=" ".join(dropped))
            g.restore()
        except Exception as e:  # noqa: BLE001 -- the guest is not clean: the lease is taken back below
            failed = e
        finally:
            with self._cond:
                if failed is not None:
                    # Taken back BEFORE the lease leaves _releasing: a release
                    # waiting on it must find it expired, never a guest half
                    # restored (expire() itself skips a lease being released).
                    self._expired.add(lease.id)
                    self._expire_cause[lease.id] = Cause.RESTORE_FAILED
                self._releasing.discard(lease.id)
                self._cond.notify_all()
        if failed is not None:
            g = slot.guest
            if g is not None:
                g.kill()
            self._log("ERROR", "restore failed", **self._slot_attrs(slot), lease=lease.id,
                      detail=str(failed)[-200:], trace_id=held.lease.trace_id)
            raise RuntimeError(f"the restore failed: {failed}") from failed
        took = int((time.monotonic() - t0) * 1000)
        self._log("INFO", "restored", **self._slot_attrs(slot), lease=lease.id, duration_ms=took,
                  trace_id=held.lease.trace_id)
        return took

    def expire(self, lease: Lease, cause: Cause) -> None:
        """Take a HELD lease back now: its guest is killed (its holder may be
        mid-command), its clean state kept; its release then reports
        REPLACED with @cause. A lease not held is ignored."""
        with self._cond:
            held = self._held.get(lease.id)
            if held is None or lease.id in self._releasing:
                return
            self._expired.add(lease.id)
            self._expire_cause[lease.id] = cause
            g = held.slot.guest
        if g is not None:
            g.kill()
        self._log("WARN", "lease taken back", **self._slot_attrs(held.slot), lease=lease.id,
                  cause=cause.value)

    def guests(self) -> list[GuestInfo]:
        """Every guest the pool keeps, and its status, now. A RETIRED one is
        listed until the reaper deletes its state (its next tick)."""
        with self._cond:
            return [GuestInfo(s.id, s.class_name, s.arch, s.status.value, s.key, str(s.tree))
                    for s in self._slots]

    # -- keys, close ------------------------------------------------------------

    def revalidate(self, classes: ClassSet | None = None) -> list[str]:
        """Recompute every known key of a tree -- @classes's (replacing the
        ClassSet the pool had for it) or the default tree's -- and retire
        that tree's guests whose key changed (KEYS). Returns the
        `<class>/<arch>` retired."""
        with self._cond:
            tree = classes.tree if classes is not None else self._default_tree
            if classes is not None:
                self._trees[tree] = classes
                self._note_file(classes)
            known = [k for k in self._keys if k[0] == tree]
        fresh: dict[QKey, tuple[FileSig, str]] = {}
        for k in known:
            t, name, arch, agent = k
            try:
                fresh[k] = (_file_sig(agent), class_key(self._trees[t].spec(name, arch, agent)))
            except ClassError:
                pass
        retired: list[str] = []
        with self._cond:
            for k in known:
                self._keys.pop(k, None)
            self._keys.update(fresh)
            self._boot_failed.clear()
            for s in self._slots:
                if s.tree != tree or s.status is _Status.RETIRED \
                        or fresh.get(s.qkey, ((0, 0), ""))[1] == s.key:
                    continue
                retired.append(f"{s.class_name}/{s.arch}")
                if s.status in (_Status.IDLE, _Status.STOPPED):
                    self._retire(s)
                else:
                    s.retire_on_release = True   # busy: retired when it comes back
            self._cond.notify_all()
        return sorted(set(retired))

    def close(self) -> None:
        """Stop every guest, states kept; leased ones are LOST (CLOSED).
        Waiting and later lease() calls raise LeaseMiss POOL_CLOSED."""
        with self._cond:
            if self._closed:
                return
            self._closed = True
            inflight = list(self._futures)
            self._cond.notify_all()
        # ONE deadline for all of it, not one per boot: a service's stop is
        # bounded (its unit's TimeoutStopSec is derived from this).
        end = time.monotonic() + BOOT_WAIT_ON_CLOSE_S
        for fut in inflight:          # boots and cold starts finish, then are stopped
            try:
                fut.result(timeout=max(end - time.monotonic(), 0.0))
            except Exception:  # noqa: BLE001 -- a failed one has nothing to stop
                pass
        with self._cond:
            # The lease threads that started them end them (_start): wait.
            self._cond.wait_for(lambda: self._starting == 0, timeout=max(end - time.monotonic(), 0.0))
            guests = [s.guest for s in self._slots if s.guest is not None]
            for s in self._slots:
                s.guest = None
                if s.status is not _Status.RETIRED:
                    s.status = _Status.STOPPED
        # All at once, within GUEST_STOP_WAIT_S; one that has not stopped by
        # then is killed (its state kept from its last clean snapshot).
        stoppers = [threading.Thread(target=_stop_or_kill, args=(g,), daemon=True) for g in guests]
        for t in stoppers:
            t.start()
        stop_end = time.monotonic() + GUEST_STOP_WAIT_S
        for t in stoppers:
            t.join(max(stop_end - time.monotonic(), 0.0))
        killers = [threading.Thread(target=g.kill, daemon=True)
                   for g, t in zip(guests, stoppers) if t.is_alive()]
        for k in killers:                    # all at once, like the stops
            k.start()
        for k in killers:
            k.join(GUEST_KILL_WAIT_S)
        # A boot that outlived the deadline is not waited for: its guest is
        # an orphan the next pool on this root sweeps (and a service's unit
        # kills its whole group after its stop).
        self._spawner.shutdown()
        os.close(self._lock_fd)
        self._log("INFO", "closed")
