#!/usr/bin/env python3
"""pool-cases.py -- axlpool.pool's lease contract, against real guests.

Usage: pool-cases.py ARCH AGENT_EFI WORKDIR
                     [--settings | --lifetimes | --lifetimes-mem | --state]

Every clause of pool.py's docstring has a case: leases booted (two at once,
in parallel), restored and cold-started; release returned, replaced (with
its cause), lost, idempotent under concurrent calls, keeping files; the
MISS reasons; first-come-first-served order; tokens; booted_after (once);
the health check at lease; the log record's shape; close(). --settings
runs, INSTEAD of those, the class settings (reuse, reset, keep = "lease",
agent_latency, the reset grace) and revalidate(): arch-independent, run on
X64 only by their own test, so neither test nears CI's per-test limit.
close() runs in both. --lifetimes (idle stop, max hold, the memory cap),
--lifetimes-mem (the cap counting a class's RAM and a booting guest) and
--state (the root lock and orphan sweep, a rebuilt agent, deleted state,
close() racing a boot, state reused) are phase B, each its own X64 test
for the same reason. Prints `  PASS:`/`  FAIL:` per
case, then `pool-cases: <p> passed, <f> failed`.
"""
from __future__ import annotations

import dataclasses
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration" / "lib"))
from axlpool.classes import ClassError, ClassSet, loads  # noqa: E402
from axlpool.pool import (Cause, Lease, LeaseCancelled, LeaseMiss, Loss,  # noqa: E402
                          MissReason, Pool, ReleaseOutcome, ReleaseResult, Start)
from axlpool.guest import Guest, Qmp, _free_port  # noqa: E402
from axlpool.klass import AGENT_GUEST_PORT  # noqa: E402
from checks import Cases, ppid  # noqa: E402

GUID = "6a1d3c4e-9b2f-4c7a-8e51-2d930f6ba41a"
MISSING_VAR = "0x800000000000000F"
VERDICT = "[INFO]  mem: no leaks detected"
NO_FILES: list[str] = []          # an empty list of names: no files, no misses
NO_RECORD: dict[str, object] = {}

CLASSES = """
[class.standard]
args = ["--bridges"]
slots = 2

[class.one]
extends = "standard"
slots = 1

[class.big]
extends = "standard"
args = ["--mem", "768M"]
slots = 1

[class.broken]
extends = "standard"
args = ["--qemu-arg", "-device", "--qemu-arg", "no-such-device"]

[class.reuse1]
extends = "standard"
reset = "reuse"
slots = 1

[class.reset1]
extends = "standard"
reset = "reset"
slots = 1

[class.lease1]
extends = "standard"
keep = "lease"
slots = 1

[class.grace1]
extends = "standard"
slots = 2

[class.typing1]
extends = "standard"
agent_latency = "firmware"
slots = 1
"""

LEASED_KEYS = sorted(["timestamp", "severity_text", "body", "axl.class", "axl.arch", "axl.guest",
                      "axl.key", "axl.lease", "axl.outcome", "duration_ms", "trace_id"])


def census(root: Path) -> list[str]:
    left: list[str] = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        exe = args.split(" ", 1)[0]
        if ("qemu-system" in exe or "virtiofsd" in exe) and str(root) in args:
            left.append(f"{proc.name} {exe}")
    return left


def _quiet(fn: Callable[[], object]) -> None:
    """@fn, any exception swallowed (a line cut off by a restore)."""
    try:
        fn()
    except Exception:  # noqa: BLE001 -- the restore ends it; not the case's subject
        pass


def host_forwards(qmp: Path) -> list[int]:
    """The host ports of every TCP host forward the guest's QEMU holds."""
    with Qmp(qmp) as q:
        text = str(q.cmd("human-monitor-command", {"command-line": "info usernet"}))
    return [int(ln.split()[3]) for ln in text.splitlines() if "[HOST_FORWARD]" in ln]


def qemu_pid_of(lease: Lease) -> int:
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if args and b"qemu-system" in args[0] and any(str(lease.qmp_socket).encode() in a for a in args):
            return int(proc.name)
    return 0


def ping(lease: Lease) -> str:
    """The lease's /ping answer, or why there was none (a value to compare)."""
    client = lease.agent()
    try:
        return client.ping()
    except Exception as e:  # noqa: BLE001 -- the failure IS the finding
        return f"{type(e).__name__}"
    finally:
        client.close()


def run(lease: Lease, line: str) -> tuple[str, str | None]:
    """(status, stdout) of @line through @lease's agent."""
    client = lease.agent()
    try:
        r = client.exec(line)
        return r.status, r.stdout
    finally:
        client.close()


def miss(fn: Callable[[], object]) -> tuple[str, float]:
    """(the LeaseMiss reason or "no miss", seconds taken) of calling @fn."""
    t = time.monotonic()
    try:
        fn()
        got = "no miss"
    except LeaseMiss as e:
        got = e.reason.value
    return got, time.monotonic() - t


def until_status(pool: Pool, guest: str, status: str, timeout: float = 30.0) -> bool:
    """Poll until @guest reports @status (never a fixed sleep)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if any(g.guest == guest and g.status == status for g in pool.guests()):
            return True
        time.sleep(0.05)
    return False


def _crasher(agent: Path, root: Path, arch: str) -> subprocess.Popen[str]:
    """A pool in its own process, leasing one guest -- to be killed."""
    return subprocess.Popen(
        [sys.executable, "-c",
         "import sys, time; from pathlib import Path; "
         f"sys.path[:0] = {[str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1] / 'integration' / 'lib')]!r}; "
         "from axlpool.classes import loads; from axlpool.pool import Pool; "
         f"p = Pool(loads({CLASSES!r}, 'crasher'), Path({str(agent)!r}), Path({str(root)!r})); "
         f"print('LEASING', flush=True); p.lease('one', {arch!r}, timeout=300); "
         "print('LEASED', flush=True); time.sleep(600)"],
        stdout=subprocess.PIPE, text=True)


def sweep(c: Cases, arch: str, agent: Path, work: Path, classes: ClassSet) -> None:
    """The root lock, and what a crashed pool leaves: a guest it SERVED dies
    with it (the pool's child), one still booting under run-qemu.sh's
    `timeout` does not, and the next pool on the root sweeps that one."""
    root = work / "sweep"
    crasher = _crasher(agent, root, arch)
    try:
        assert crasher.stdout is not None
        crasher.stdout.readline()
        leased = crasher.stdout.readline().strip()
        try:
            Pool(classes, agent, root)
            refused = "not refused"
        except RuntimeError as ex:
            refused = str(ex)
        left = census(root)
        sibling = Pool(classes, agent, work / root.name[:-1])   # "swee": a prefix of "sweep"
        sibling.close()
        c.check("a pool on a root that is a string PREFIX of another's sweeps none of its guests",
                census(root) == left, True)
        # A RELATIVE root names "sweep/" -- a substring of the live root's
        # paths. It is resolved against its own cwd, so it sweeps nothing else.
        elsewhere = work / "elsewhere"
        elsewhere.mkdir()
        cwd = Path.cwd()
        os.chdir(elsewhere)
        try:
            relative = Pool(classes, agent, Path(root.name))
            relative.close()
        finally:
            os.chdir(cwd)
        c.check("a pool on a RELATIVE root sweeps none of another root's guests",
                census(root) == left, True)
        c.check("a second pool on a LIVE pool's root is refused, the live pool's guest untouched",
                (leased, refused, len(left) == 2), ("LEASED", f"another pool owns {root}", True))
        crasher.kill()
        crasher.wait()
        gone = False
        for _ in range(100):
            if not census(root):
                gone = True
                break
            time.sleep(0.05)
        c.check("a guest the pool SERVED dies with the pool when it crashes (the pool's child)",
                gone, True)
    finally:
        crasher.kill()
        crasher.wait()

    # Crashed MID-BOOT: the guest run-qemu.sh started is under its own
    # `timeout`, not the pool's -- it outlives the crash; the next pool on the
    # root ends it.
    root = work / "sweep-boot"
    crasher = _crasher(agent, root, arch)
    try:
        # Waited for QEMU itself, not virtiofsd (which run-qemu.sh starts
        # first, and which alone would prove no guest outlived anything).
        booting = False
        for _ in range(300):
            if any("qemu-system" in p for p in census(root)):
                booting = True
                break
            time.sleep(0.05)
        crasher.kill()
        crasher.wait()
        time.sleep(1)
        outlived = any("qemu-system" in p for p in census(root))
    finally:
        crasher.kill()
        crasher.wait()
    second = Pool(classes, agent, root)
    time.sleep(1)
    c.check("a pool crashed MID-BOOT leaves run-qemu.sh's guest; the next pool on its root sweeps it",
            (booting, outlived, census(root)), (True, True, NO_FILES))
    second.close()


def agent_rebuilt(c: Cases, arch: str, agent: Path, work: Path, classes: ClassSet) -> None:
    """A guest is never served with an agent image other than the one asked for."""
    root = work / "rebuilt"
    image = work / "rebuilt-agent.efi"
    shutil.copy(agent, image)
    pool = Pool(classes, agent, root)
    try:
        x = pool.lease("one", arch, timeout=300, agent=image)
        pool.release(x)
        # Rebuilt in place: same path, other bytes (a PE ignores a trailing
        # overlay, so it still runs). The idle guest runs the OLD build.
        with image.open("ab") as f:
            f.write(b"\0" * 16)
        y = pool.lease("one", arch, timeout=300, agent=image)
        c.check("an agent rebuilt at the same path: a NEW guest boots it (healthy: its agent "
                "answered), not the idle guest of the old build",
                (x.agent_image, y.agent_image, y.start, y.guest != x.guest, y.key != x.key),
                (str(image), str(image), Start.BOOTED, True, True))
        pool.release(y)
    finally:
        pool.close()
    c.check("rebuilt: nothing left running", census(root), NO_FILES)


def cancel_boot(c: Cases, arch: str, agent: Path, work: Path, classes: ClassSet) -> None:
    """A lease cancelled while its guest boots gives up at once; the guest
    joins the pool when its boot ends."""
    root = work / "cancelboot"
    pool = Pool(classes, agent, root)
    cancel = threading.Event()
    outcome: list[str] = []

    def booting() -> None:
        try:
            pool.lease("one", arch, timeout=300, cancel=cancel)
            outcome.append("leased")
        except LeaseCancelled:
            outcome.append("cancelled")
    try:
        tb = threading.Thread(target=booting)
        tb.start()
        # Inside the boot (10-22 s) -- not before it: the class key's dry
        # run alone takes ~2 s, and a cancel seen then proves nothing here.
        booting_seen = False
        for _ in range(200):
            if [g.status for g in pool.guests()] == ["booting"]:
                booting_seen = True
                break
            time.sleep(0.05)
        time.sleep(1)
        t_c = time.monotonic()
        cancel.set()
        tb.join(60)
        took = time.monotonic() - t_c
        joined = False
        for _ in range(300):
            if [g.status for g in pool.guests()] == ["idle"]:
                joined = True
                break
            time.sleep(0.1)
        c.check("cancel during a boot: the lease gives up at once; the guest, booted, joins idle",
                (booting_seen, outcome, took < 3, joined), (True, ["cancelled"], True, True))
    finally:
        pool.close()
    c.check("cancelboot: nothing left running", census(root), NO_FILES)


def closeboot(c: Cases, arch: str, agent: Path, work: Path, classes: ClassSet) -> None:
    """close() racing a boot."""
    # close() while a lease's guest is still booting: that lease misses,
    # pool-closed, and the guest it booted is ended -- never left running.
    root = work / "closeboot"
    pool = Pool(classes, agent, root)
    # A stop that takes a while (a guest slow to quit) widens the window in
    # which close() could return while another thread is still stopping.
    real_stop = Guest.stop

    def slow_stop(g: Guest) -> None:
        time.sleep(1.0)
        real_stop(g)
    got: list[str] = []

    def booting() -> None:
        try:
            pool.lease("one", arch, timeout=300)
            got.append("leased")
        except LeaseMiss as ex:
            got.append(ex.reason.value)
    tb = threading.Thread(target=booting)
    tb.start()
    time.sleep(3)
    setattr(Guest, "stop", slow_stop)
    try:
        pool.close()
        at_return = census(root)       # close() returning means ended: not after the lease thread
    finally:
        setattr(Guest, "stop", real_stop)
    tb.join(200)
    c.check("close() while a lease boots: that lease misses pool-closed, nothing running "
            "once close() returns",
            (got, at_return), ([MissReason.POOL_CLOSED.value], NO_FILES))


def handover_fail(c: Cases, arch: str, agent: Path, work: Path, classes: ClassSet) -> None:
    """A boot whose HAND-OVER (the cold start that makes its guest the pool's
    child) fails is not a boot failure -- the class boots fine, and caching
    it as broken would make it unleasable until revalidate(). Its snapshot is
    good, so the slot is kept STOPPED and the retry cold-starts it (~0.4 s),
    not boots it again (10-22 s)."""
    root = work / "handover"
    logs: list[dict[str, object]] = []
    pool = Pool(classes, agent, root, log=logs.append)
    real = Guest.__dict__["cold_start"]       # the descriptor itself, restored exactly
    failed = [0]

    def flaky(state: Path, share: Path, timeout: float = 60.0) -> Guest:
        if failed[0] == 0:
            failed[0] += 1
            raise RuntimeError("injected: the hand-over's cold start failed")
        return Guest.__dict__["_real_cold_start"].__func__(Guest, state, share, timeout)
    setattr(Guest, "_real_cold_start", real)
    setattr(Guest, "cold_start", staticmethod(flaky))
    try:
        x = pool.lease("one", arch, timeout=300)
        bodies = [str(r.get("body")) for r in logs]
        c.check("a failed hand-over: the slot is kept, and the retry COLD-starts it (no second "
                "boot); logged as such, never as a boot failure",
                (x.start, failed[0], bodies.count("hand-over failed"), bodies.count("boot failed"),
                 bodies.count("booted")),
                (Start.COLD, 1, 1, 0, 0))     # the one boot logged as its failed hand-over
        pool.release(x)
    finally:
        setattr(Guest, "cold_start", real)
        delattr(Guest, "_real_cold_start")
        pool.close()
    c.check("handover: nothing left running, cold_start restored exactly",
            (census(root), Guest.__dict__["cold_start"] is real), (NO_FILES, True))


def disk(c: Cases, arch: str, agent: Path, work: Path, classes: ClassSet) -> None:
    """State that can never serve again is deleted, not kept forever."""
    root = work / "disk"

    def files() -> list[str]:
        return sorted(str(f.relative_to(root)) for f in root.rglob("*")
                      if f.is_file() and f.name != ".lock")

    def slot_dir(key: str, sid: str, meta: str | None) -> None:
        d = root / key / sid
        (d / "state").mkdir(parents=True)
        (d / "state" / "launch.json").write_text("{}")
        if meta is not None:
            (d / "slot.json").write_text(meta)

    def meta(sid: str, key: str, tree: str) -> str:
        return (f'{{"id": "{sid}", "class": "one", "arch": "{arch}", "key": "{key}", '
                f'"snapshot_at": 1.0, "tree": "{tree}", "agent": "{agent}"}}')

    stale_key = "0" * 64
    slot_dir("aaaa", "gnometa", None)                       # a start that never finished
    slot_dir("aaaa", "gbad", '{"id": 1}')                  # malformed
    slot_dir("aaaa", "gnotree", meta("gnotree", stale_key, str(work / "no-such-tree")))
    slot_dir(stale_key[:16], "gold", meta("gold", stale_key, str(classes.tree)))
    (root / "kept" / "Lfail").mkdir(parents=True)
    (root / "kept" / "Lfail" / "out.log").write_text("a failed test's files\n")
    pool = Pool(classes, agent, root)
    try:
        c.check("load: a malformed, unfinished or treeless slot is deleted; kept/ is not; "
                "a loadable one stays",
                (files(), [(g.guest, g.status) for g in pool.guests()]),
                (sorted(["kept/Lfail/out.log", f"{stale_key[:16]}/gold/slot.json",
                         f"{stale_key[:16]}/gold/state/launch.json"]), [("gold", "stopped")]))
        x = pool.lease("one", arch, timeout=300)
        gone = False
        for _ in range(100):                                # the reaper's next tick
            if not (root / stale_key[:16] / "gold").exists() and \
                    [g.guest for g in pool.guests()] == [x.guest]:
                gone = True
                break
            time.sleep(0.2)
        c.check("a stopped guest of an OLD key for the leased class is retired and its "
                "state deleted", (x.start, gone), (Start.BOOTED, True))
        pool.release(x)
    finally:
        pool.close()
    c.check("disk: nothing left running", census(root), NO_FILES)
    # The reaper leaves an emptied key directory (a boot may be creating a
    # sibling in it); the next pool's load prunes it.
    again = Pool(classes, agent, root)
    again.close()
    c.check("disk: the next load prunes the emptied key directory",
            (root / stale_key[:16]).exists(), False)


def lifetimes(c: Cases, arch: str, agent: Path, work: Path) -> None:
    """Phase B, lifetimes: idle stop, max hold, the memory cap.
    Each case its own pool and root, each closed and checked empty."""
    classes = loads(CLASSES, "pool-cases")

    # Idle timeout: a released guest stops by itself; the next lease
    # cold-starts it from the state it kept.
    root = work / "idle"
    pool = Pool(classes, agent, root, idle_timeout_s=3)
    try:
        x = pool.lease("one", arch, timeout=300)
        pool.release(x)
        stopped = until_status(pool, x.guest, "stopped", timeout=30)
        y = pool.lease("one", arch, timeout=60)
        c.check("idle_timeout_s: an unused guest stops by itself; the next lease cold-starts it",
                (stopped, y.guest, y.start), (True, x.guest, Start.COLD))
        pool.release(y)
    finally:
        pool.close()
    c.check("idle: nothing left running", census(root), NO_FILES)

    # max_hold_s: a lease held too long is taken back; its release says so.
    # Released once the reaper has taken it back -- its "lease held too
    # long" record -- not after a fixed sleep; a reaper that never does
    # leaves the release RETURNED.
    root = work / "hold"
    logs: list[dict[str, object]] = []
    pool = Pool(classes, agent, root, max_hold_s=3, log=logs.append)
    try:
        x = pool.lease("one", arch, timeout=300)
        end = time.monotonic() + 15
        while time.monotonic() < end and not any(
                r.get("body") == "lease held too long" and r.get("axl.lease") == x.id
                for r in list(logs)):
            time.sleep(0.05)
        res = pool.release(x)
        c.check("max_hold_s: a lease held past it is taken back -- replaced, expired-hold",
                (res.outcome, res.cause, ping(x)),
                (ReleaseOutcome.REPLACED, Cause.EXPIRED_HOLD, "ConnectionRefusedError"))
    finally:
        pool.close()
    c.check("hold: nothing left running", census(root), NO_FILES)

    # mem_cap_mb: starting a guest of another class first stops the least
    # recently used IDLE guest when the cap would be passed.
    root = work / "mem"
    pool = Pool(classes, agent, root, mem_cap_mb=700)
    try:
        x = pool.lease("one", arch, timeout=300)
        pool.release(x)
        until_status(pool, x.guest, "idle")
        y = pool.lease("standard", arch, timeout=300)
        statuses = {g.guest: g.status for g in pool.guests()}
        c.check("mem_cap_mb: the LRU idle guest is stopped to make room for another",
                (statuses.get(x.guest), y.start), ("stopped", Start.BOOTED))
        pool.release(y)
    finally:
        pool.close()
    c.check("mem: nothing left running", census(root), NO_FILES)


def lifetimes_mem(c: Cases, arch: str, agent: Path, work: Path) -> None:
    """Phase B, the memory cap's accounting: a class's RAM, a booting guest.
    (Split from lifetimes() so neither test is one long serial chain.)"""
    classes = loads(CLASSES, "pool-cases")

    # ...counting each guest as its CLASS's RAM: under a 1200 MB cap an idle
    # 512M guest and a 768M one do not both fit (1280), so starting the 768M
    # class stops the idle one. Counted as the 512M default, both "fit".
    root = work / "membig"
    pool = Pool(classes, agent, root, mem_cap_mb=1200)
    try:
        x = pool.lease("one", arch, timeout=300)
        pool.release(x)
        until_status(pool, x.guest, "idle")
        y = pool.lease("big", arch, timeout=300)
        statuses = {g.guest: g.status for g in pool.guests()}
        c.check("mem_cap_mb: a guest counts its class's --mem (768M), not the 512M default",
                (statuses.get(x.guest), y.start), ("stopped", Start.BOOTED))
        pool.release(y)
    finally:
        pool.close()
    c.check("membig: nothing left running", census(root), NO_FILES)

    # Two starts AT ONCE under the cap: a guest still booting counts its
    # RAM, so only one fits; the other waits and misses at its deadline.
    root = work / "memrace"
    pool = Pool(classes, agent, root, mem_cap_mb=700)
    outcomes: list[str] = []
    lock = threading.Lock()

    def start(cls: str, t: float) -> None:
        try:
            got_l = pool.lease(cls, arch, timeout=t)
            with lock:
                outcomes.append("leased")
            del got_l
        except LeaseMiss as ex:
            with lock:
                outcomes.append(ex.reason.value)
    try:
        # ONE deadline for both: either may win the boot, and the loser
        # must miss within it (with 300 s for one, the race cost 300 s; the
        # loser waits all of it). A boot to agent-ready is ~10 s.
        ts = [threading.Thread(target=start, args=("one", 25)),
              threading.Thread(target=start, args=("standard", 25))]
        for th in ts:
            th.start()
        for th in ts:
            th.join(400)
        c.check("mem_cap_mb: two starts at once -- one boots, the other misses (a booting guest counts)",
                sorted(outcomes), sorted(["leased", MissReason.NO_SLOT.value]))
    finally:
        pool.close()
    c.check("memrace: nothing left running", census(root), NO_FILES)


def state(c: Cases, arch: str, agent: Path, work: Path) -> None:
    """Phase B, ownership and state: the root lock and orphan sweep, a
    rebuilt agent, deleted state, close() racing a boot, state reused.
    (Split from lifetimes() to keep each test inside CI's limit.)"""
    classes = loads(CLASSES, "pool-cases")
    sweep(c, arch, agent, work, classes)
    agent_rebuilt(c, arch, agent, work, classes)
    cancel_boot(c, arch, agent, work, classes)
    handover_fail(c, arch, agent, work, classes)
    disk(c, arch, agent, work, classes)

    closeboot(c, arch, agent, work, classes)

    # State across pool lifetimes: a pool started after another closed
    # cold-starts from the state left, rather than booting.
    root = work / "reuse"
    first = Pool(classes, agent, root)
    try:
        x = first.lease("one", arch, timeout=300)
        first.release(x)
    finally:
        first.close()
    second = Pool(classes, agent, root)
    try:
        y = second.lease("one", arch, timeout=60)
        c.check("a later pool on the same root cold-starts the state left, not a boot",
                (y.guest, y.start), (x.guest, Start.COLD))
        second.release(y)
    finally:
        second.close()
    c.check("reuse: nothing left running", census(root), NO_FILES)


def main() -> int:
    arch, agent, work = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    phase = {"--lifetimes": lifetimes, "--lifetimes-mem": lifetimes_mem, "--state": state}
    for flag, phase_cases in phase.items():
        if flag in sys.argv[4:]:
            c = Cases()
            try:
                phase_cases(c, arch, agent, work)
            except Exception as e:  # a contract that throws is a failure, reported
                c.check(f"the {flag[2:]} cases ran without raising ({type(e).__name__}: {e})", False, True)
            print(f"pool-cases: {c.passed} passed, {c.failed} failed")
            return 0 if c.failed == 0 and c.passed > 0 else 1
    settings = "--settings" in sys.argv[4:]
    c = Cases()
    root = work / "pool"
    logs: list[dict[str, object]] = []
    pool = Pool(loads(CLASSES, "pool-cases"), agent, root, log=logs.append)
    try:
        if not settings:
            r, _ = miss(lambda: pool.lease("nope", arch, timeout=5))
            c.check("an unknown class: MISS unknown-class", r, MissReason.UNKNOWN_CLASS.value)
            r, _ = miss(lambda: pool.lease("standard", arch, timeout=5, tree=work / "no-such-tree"))
            c.check("a tree with no classes file: MISS unknown-class", r, MissReason.UNKNOWN_CLASS.value)

            # An EDITED classes.toml is picked up without a restart: the pool
            # re-reads a tree's classes when the file's signature changes (as
            # it does an agent image's). It used to cache the first read for
            # its whole life -- adding a class meant restarting the service.
            ctree = work / "edit-tree"
            (ctree / "test" / "pool").mkdir(parents=True, exist_ok=True)
            cfile = ctree / "test" / "pool" / "classes.toml"
            cfile.write_text(CLASSES)
            r, _ = miss(lambda: pool.lease("added", arch, timeout=5, tree=ctree))
            c.check("classes reload: a class the file does not name is MISS unknown-class", r,
                    MissReason.UNKNOWN_CLASS.value)
            cfile.write_text(CLASSES + '\n[class.added]\nextends = "standard"\n')
            st = cfile.stat()
            os.utime(cfile, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000_000))
            try:
                known = pool._tree_classes(ctree).get("added").name   # pyright: ignore[reportPrivateUsage]
            except ClassError as e:
                known = f"ClassError: {e}"
            c.check("classes reload: the edited file's new class is known, no restart", known, "added")

            # Two first leases at once boot IN PARALLEL: neither waits for the
            # other's boot (the head of the queue wakes the next as it reserves).
            got: list[Lease] = []
            missed: list[str] = []

            def first() -> None:
                try:
                    got.append(pool.lease("standard", arch, timeout=300))
                except LeaseMiss as ex:     # a value the case compares, not a crash
                    missed.append(ex.reason.value)
            both = [threading.Thread(target=first) for _ in range(2)]
            for t in both:
                t.start()
            for t in both:
                t.join(400)
            times = sorted(x.start_ms for x in got)
            c.check("two first leases at once both boot, in parallel (neither waits out the other)",
                    (len(got), missed, [x.start for x in got], len(times) == 2 and times[1] < 1.5 * times[0]),
                    (2, NO_FILES, [Start.BOOTED, Start.BOOTED], True))
            if len(got) < 2:
                raise RuntimeError("the parallel boots failed; the rest depends on them")
            # Every guest serving a lease is the POOL's child, whatever started
            # it: a freshly booted one too, so it dies with the pool (a booted
            # guest under run-qemu.sh's `timeout` used to outlive a crashed one).
            c.check("a freshly booted guest's QEMU is the pool process's child",
                    sorted(ppid(qemu_pid_of(x)) for x in got), [os.getpid(), os.getpid()])
            # `other` stays leased until the slot cases, so the replacement and
            # health cases below see ONE standard guest of their own.
            a, other = got[0], got[1]

            env = a.env()
            c.check("its env names everything a test script needs",
                    sorted(env), sorted(["AXL_POOL_AGENT", "AXL_POOL_QMP", "AXL_POOL_HOST_DIR",
                                         "AXL_POOL_GUEST_DIR", "AXL_POOL_SERIAL",
                                         "AXL_POOL_SERIAL_OFFSET", "AXL_POOL_LEASE",
                                         "AXL_POOL_CLASS", "AXL_POOL_ARCH", "AXL_POOL_TRACE_ID",
                                         "AXL_POOL_AGENT_IMAGE", "AXL_POOL_QEMU_PID",
                                         "AXL_POOL_START", "AXL_POOL_START_MS",
                                         "AXL_POOL_RESTORE_TOKEN"]))
            c.check("its env carries the RESTORE capability, never the release token",
                    (env["AXL_POOL_RESTORE_TOKEN"] == a.restore_token, len(a.restore_token) >= 32,
                     a.restore_token != a.token, a.token in env.values()), (True, True, True, False))
            c.check("its env says how the guest started and what it cost",
                    (env["AXL_POOL_START"], env["AXL_POOL_START_MS"]), (str(a.start), str(a.start_ms)))
            c.check("env values", (env["AXL_POOL_AGENT"], env["AXL_POOL_GUEST_DIR"], env["AXL_POOL_LEASE"]),
                    (f"127.0.0.1:{a.agent_addr[1]}", a.guest_dir, a.id))
            # The guest's own QEMU, for a harness that samples it (the quiet
            # slot's vCPU-starvation witness): not a wrapper's pid.
            c.check("AXL_POOL_QEMU_PID is the lease's guest's QEMU",
                    env["AXL_POOL_QEMU_PID"], str(qemu_pid_of(a)))
            c.check("serial(): starts at the lease (the boot's ready line is before it)",
                    b"AXL-AGENT-READY" in a.serial(), False)
            leased = next((x for x in logs if x.get("body") == "leased"), NO_RECORD)
            c.check("a log record: OpenTelemetry fields and the axl.* attributes",
                    sorted(leased), LEASED_KEYS)
            (a.host_dir / "x.txt").write_text("X1\r\n")
            c.check("the lease directory is the guest's, through the share",
                    run(a, f"type {a.guest_dir}\\x.txt"), ("0x0", "X1\r\n\r\n"))
            run(a, f"setvar AXLPOOLL -guid {GUID} -nv -bs =01")
            first_guest = a.guest
            # A host port forward a test adds to its lease (as pooled
            # test_run_background does) lives in QEMU, outside the snapshot:
            # the release must take it away, or the guest keeps the host port
            # bound into the next lease. The agent's own forward stays.
            extra, anyaddr, udp = _free_port(), _free_port(), _free_port()
            with Qmp(a.qmp_socket) as q:
                for rule in (f"tcp:127.0.0.1:{extra}-:{AGENT_GUEST_PORT}", f"tcp::{anyaddr}-:7000",
                             f"udp:127.0.0.1:{udp}-:5000"):
                    q.cmd("human-monitor-command", {"command-line": f"hostfwd_add net0 {rule}"})
                # The shell side reads QEMU's answer to tell a refused forward
                # from an added one: "" added, a reason refused (a host port
                # held -- here the agent's own).
                held = q.cmd("human-monitor-command",
                             {"command-line": f"hostfwd_add net0 tcp:127.0.0.1:{a.agent_addr[1]}-:7001"})
            c.check("control: forwards added during the lease are there -- TCP, any-address TCP, UDP",
                    sorted(host_forwards(a.qmp_socket)), sorted([a.agent_addr[1], extra, anyaddr, udp]))
            c.check("a forward QEMU refuses (its host port held) answers a reason, not \"\"",
                    (held != "", "Could not set up host forwarding rule" in str(held)), (True, True))
            results: list[ReleaseResult] = []
            racers = [threading.Thread(target=lambda: results.append(pool.release(a))) for _ in range(2)]
            for t in racers:
                t.start()
            for t in racers:
                t.join(60)
            released = [x for x in logs if x.get("body") == "released" and x.get("axl.lease") == a.id]
            c.check("two concurrent releases: one acts, both get its result",
                    (len(results), len(set(results)), results[0].outcome if results else None, len(released)),
                    (2, 1, ReleaseOutcome.RETURNED, 1))
            c.check("release is idempotent afterwards too", pool.release(a), results[0] if results else None)

            until_status(pool, first_guest, "idle")
            c.check("the release removes the lease's host forwards, keeping only the agent's",
                    host_forwards(a.qmp_socket), [a.agent_addr[1]])
            # The tree named explicitly keys as the default: the same guest.
            b = pool.lease("standard", arch, timeout=60, tree=Path(__file__).resolve().parents[2])
            c.check("the next lease: the same guest, restored",
                    (b.start, b.guest, b.start_ms < 5000), (Start.RESTORED, first_guest, True))
            c.check("... its variable reverted, the share wiped, a new directory",
                    (run(b, f"setvar AXLPOOLL -guid {GUID}")[0], (a.host_dir / "x.txt").exists(),
                     b.host_dir != a.host_dir, sorted(p.name for p in b.host_dir.iterdir())),
                    (MISSING_VAR, False, True, NO_FILES))
            forged = dataclasses.replace(b, token="forged")
            try:
                pool.release(forged)
                c.check("a forged token is refused", "released", "PermissionError")
            except PermissionError:
                c.check("a forged token is refused", "PermissionError", "PermissionError")

            # restore(): a HELD lease's guest back to `clean`, the lease kept
            # (run-qemu.sh's pooled mode: a fresh machine per run). Guest RAM
            # (a volatile variable), NVRAM (a non-volatile one) and a host
            # forward the lease added are gone; the lease's files stay.
            run(b, "set -v AXLRQ 1")
            run(b, f"setvar AXLPOOLR -guid {GUID} -nv -bs =01")
            (b.host_dir / "kept.txt").write_text("K\n")
            fwd = _free_port()
            with Qmp(b.qmp_socket) as q:
                q.cmd("human-monitor-command",
                      {"command-line": f"hostfwd_add net0 tcp:127.0.0.1:{fwd}-:7000"})
            c.check("control: before the restore, the variables are set and the forward is there",
                    (run(b, "echo %AXLRQ%")[1], run(b, f"setvar AXLPOOLR -guid {GUID}")[0],
                     fwd in host_forwards(b.qmp_socket)), ("1\r\n", "0x0", True))
            try:
                pool.restore(dataclasses.replace(b, restore_token="forged"))
                forged_r = "restored"
            except PermissionError:
                forged_r = "PermissionError"
            c.check("restore(): a forged restore token is refused", forged_r, "PermissionError")
            try:
                pool.restore(dataclasses.replace(b, restore_token=b.token))
                swapped = "restored"
            except PermissionError:
                swapped = "PermissionError"
            c.check("restore(): the RELEASE token is not a restore token", swapped, "PermissionError")
            took_ms = pool.restore(b)
            restored_log = next((x for x in logs if x.get("body") == "restored"
                                 and x.get("axl.lease") == b.id), NO_RECORD)
            c.check("restore(): back to clean -- volatile and NV variables gone, the forward gone, "
                    "the lease's files kept, the agent answering",
                    (run(b, "echo %AXLRQ%")[1], run(b, f"setvar AXLPOOLR -guid {GUID}")[0],
                     host_forwards(b.qmp_socket), (b.host_dir / "kept.txt").read_text(), ping(b)),
                    ("Echo is on.\r\n", MISSING_VAR, [b.agent_addr[1]], "K\n", "pong\n"))
            c.check("restore(): the lease is still held, and it took well under a boot",
                    (next((g.status for g in pool.guests() if g.guest == b.guest), ""),
                     0 < took_ms < 5000, restored_log.get("duration_ms") == took_ms),
                    ("leased", True, True))

            # The agent's own exit shows in this lease's console; the agent is
            # then gone with QEMU running. QEMU and its vCPUs are fine, so the
            # snapshot restore -- which replaces all guest state, the agent
            # with it -- is the remedy: RETURNED, said, and the next lease
            # restores the same guest. (A pooled background test leaves its
            # server running and the agent busy in exactly this way.)
            bg = b.agent()
            bg.quit()
            time.sleep(3)
            verdicts = [ln for ln in b.serial().decode(errors="replace").replace("\r", "").splitlines()
                        if ln.endswith(VERDICT)]
            c.check("serial(): this lease's console, the agent's exit in it", len(verdicts), 1)
            res = pool.release(b)
            c.check("an agent that went away under a lease: restored from its snapshot (returned)",
                    res.outcome, ReleaseOutcome.RETURNED)
            try:
                pool.restore(b)
                late = "restored"
            except PermissionError:
                late = "PermissionError"
            c.check("restore(): a lease no longer held is refused, even with its own token",
                    late, "PermissionError")
            until_status(pool, b.guest, "idle")
            d = pool.lease("standard", arch, timeout=60)
            c.check("... and the next lease restores the same guest, its agent answering",
                    (d.start, d.guest, ping(d)), (Start.RESTORED, b.guest, "pong\n"))
            # BUSY, not gone: a line still running when the lease is released
            # (a pooled background test's server). Restored as well.
            # Its client gives up 5 s in: the restore cuts the line off and
            # its socket never answers -- the 60 s default was waited out.
            busy = threading.Thread(target=lambda: _quiet(lambda: d.agent(timeout=5).exec("stall 30000000")))
            busy.start()
            time.sleep(2)
            t_rel = time.monotonic()
            res = pool.release(d)
            took_rel = time.monotonic() - t_rel
            busy.join(30)
            until_status(pool, d.guest, "idle")
            d = pool.lease("standard", arch, timeout=60)
            c.check("a lease released while its agent runs a line: restored, the guest reused",
                    (res.outcome, d.start, ping(d)), (ReleaseOutcome.RETURNED, Start.RESTORED, "pong\n"))
            # ...at once: a snapshot class's release asks QEMU (alive, no
            # reset, vCPUs running), never the busy agent -- whose /ping would
            # wait out HEALTH_DEADLINE_S inside the test's own time. The
            # restore's wait for the agent is the health check.
            c.check("...and that release returns at once, not after a /ping deadline",
                    took_rel < 2.0, True)

            os.kill(qemu_pid_of(d), signal.SIGKILL)
            time.sleep(1)
            res = pool.release(d)
            c.check("QEMU killed under a lease: replaced, qemu-exited",
                    (res.outcome, res.cause), (ReleaseOutcome.REPLACED, Cause.QEMU_EXITED))

            # A guest that RESET itself under a lease (a test ran `reset`): its
            # QMP RESET event names the cause exactly, rather than the agent
            # merely having gone quiet.
            e = pool.lease("standard", arch, timeout=60)
            try:
                e.agent(timeout=3).exec("reset")   # no answer comes: the guest resets
            except Exception:  # noqa: BLE001 -- the reset drops the agent's connection
                pass
            time.sleep(2)
            res = pool.release(e)
            c.check("a guest that reset itself under a lease: replaced, guest-reset (its QMP event)",
                    (res.outcome, res.cause, res.detail.startswith("QMP RESET")),
                    (ReleaseOutcome.REPLACED, Cause.GUEST_RESET, True))

            # The health check at lease: an IDLE guest that died is never
            # handed out; the lease cold-starts it.
            f = pool.lease("standard", arch, timeout=60)
            f_pid, f_guest = qemu_pid_of(f), f.guest
            pool.release(f)
            until_status(pool, f_guest, "idle")
            os.kill(f_pid, signal.SIGKILL)
            time.sleep(1)
            g2 = pool.lease("standard", arch, timeout=60)
            c.check("an idle guest that died is never handed out: the lease cold-starts it",
                    (g2.guest, g2.start, ping(g2)), (f_guest, Start.COLD, "pong\n"))

            # keep_files: the lease's files are moved out of the share, safe
            # from the next lease's wipe.
            (g2.host_dir / "evidence.txt").write_text("E\n")
            res = pool.release(g2, keep_files=True)
            kept = res.kept
            c.check("keep_files: the lease's files moved out of the share",
                    (kept is not None and (kept / "evidence.txt").read_text() == "E\n",
                     g2.host_dir.exists()), (True, False))

            # A restore that FAILS takes the lease back: the guest is not
            # known clean, so its release replaces it (restore-failed) --
            # never hands it on.
            rf = pool.lease("standard", arch, timeout=120)
            real_restore = Guest.restore

            def failing_restore(self: Guest) -> None:
                raise RuntimeError("injected snapshot-load failure")
            Guest.restore = failing_restore  # pyright: ignore[reportAttributeAccessIssue]
            try:
                pool.restore(rf)
                rf_raised = "restored"
            except RuntimeError as ex:
                rf_raised = str(ex)
            finally:
                Guest.restore = real_restore  # pyright: ignore[reportAttributeAccessIssue]
            res = pool.release(rf)
            c.check("a failed restore raises, and its lease's release replaces the guest (restore-failed)",
                    (rf_raised, res.outcome, res.cause),
                    ("the restore failed: injected snapshot-load failure", ReleaseOutcome.REPLACED,
                     Cause.RESTORE_FAILED))

            pool.release(other)

            # Slots, deadlines and first-come-first-served order.
            h = pool.lease("one", arch, timeout=300)
            r, took = miss(lambda: pool.lease("one", arch, timeout=2))
            c.check("no slot within the deadline: MISS no-slot, at the deadline",
                    (r, 1.5 < took < 6), (MissReason.NO_SLOT.value, True))
            order: list[tuple[str, Lease]] = []
            lock = threading.Lock()

            def waiter(name: str) -> None:
                lease = pool.lease("one", arch, timeout=120)
                with lock:
                    order.append((name, lease))
            w1 = threading.Thread(target=waiter, args=("first",))
            w1.start()
            time.sleep(0.5)
            w2 = threading.Thread(target=waiter, args=("second",))
            w2.start()
            time.sleep(0.5)
            pool.release(h)
            w1.join(60)
            if order:
                pool.release(order[0][1])
            w2.join(60)
            if len(order) > 1:
                pool.release(order[1][1])
            c.check("waiters are served first come, first served",
                    [n for n, _ in order], ["first", "second"])

            # cancel: a waiting lease whose event is set gives up its place
            # at once; expire(): a held lease is taken back.
            h2 = pool.lease("one", arch, timeout=60)
            cancel = threading.Event()
            outcome: list[str] = []

            def cancelled_waiter() -> None:
                try:
                    pool.lease("one", arch, timeout=120, cancel=cancel)
                    outcome.append("leased")
                except LeaseCancelled:
                    outcome.append("cancelled")
            w4 = threading.Thread(target=cancelled_waiter)
            w4.start()
            time.sleep(1)
            t_c = time.monotonic()
            cancel.set()
            w4.join(10)
            c.check("cancel: a waiting lease gives up at once (LeaseCancelled)",
                    (outcome, time.monotonic() - t_c < 3), (["cancelled"], True))
            pool.expire(h2, Cause.EXPIRED_HOLD)
            res = pool.release(h2)
            c.check("expire(): a held lease is taken back -- its release says replaced, the cause given",
                    (res.outcome, res.cause, ping(h2)),
                    (ReleaseOutcome.REPLACED, Cause.EXPIRED_HOLD, "ConnectionRefusedError"))

            # booted_after: one fresh boot, which then serves later leases.
            t = time.time()
            e = pool.lease("one", arch, timeout=300, booted_after=t)
            pool.release(e)
            until_status(pool, e.guest, "idle")
            e2 = pool.lease("one", arch, timeout=60, booted_after=t)
            c.check("booted_after: booted ONCE, then restored like any guest",
                    (e.start, e2.start, e2.guest), (Start.BOOTED, Start.RESTORED, e.guest))
            pool.release(e2)

            r1, _ = miss(lambda: pool.lease("broken", arch, timeout=120))
            r2, took2 = miss(lambda: pool.lease("broken", arch, timeout=120))
            c.check("a class that will not boot: boot-failed, then cached (at once)",
                    (r1, r2, took2 < 1.0), (MissReason.BOOT_FAILED.value, MissReason.BOOT_FAILED.value, True))

        if settings:
            # reuse keeps state between leases; reset reboots; keep = "lease"
            # stops the guest, and the next lease cold-starts it.
            # (`echo` of an unset variable prints the Shell's "Echo is on.")
            for cls, want in (("reuse1", "1\r\n"), ("reset1", "Echo is on.\r\n")):
                x = pool.lease(cls, arch, timeout=300)
                run(x, "set -v AXLR 1")
                pool.release(x)
                until_status(pool, x.guest, "idle", timeout=120)
                y = pool.lease(cls, arch, timeout=120)
                c.check(f"reset = {cls[:-1]}: a volatile variable after the release",
                        (y.guest, run(y, "echo %AXLR%")[1]), (x.guest, want))
                try:
                    pool.restore(y)
                    no_snap = "restored"
                except ValueError:
                    no_snap = "ValueError"
                c.check(f"reset = {cls[:-1]}: restore() is refused (the class has no snapshot to "
                        "restore to)", no_snap, "ValueError")
                pool.release(y)
            # agent_latency = "firmware": the agent leaves the firmware's
            # 10 ms tick alone (a 1 ms tick loses USB keystrokes).
            x = pool.lease("typing1", arch, timeout=300)
            client = x.agent()
            c.check('agent_latency = "firmware": the leased agent keeps a 10 ms tick',
                    client.info().get("tick_us"), 10000)
            client.close()
            pool.release(x)
            # A lease right after a release, with room for another guest:
            # it waits out the ~0.2 s reset rather than booting (10-22 s).
            x = pool.lease("grace1", arch, timeout=300)
            pool.release(x)
            y = pool.lease("grace1", arch, timeout=60)
            c.check("a guest mid-reset is waited for (briefly), not booted around",
                    (y.guest, y.start), (x.guest, Start.RESTORED))
            pool.release(y)
            x = pool.lease("lease1", arch, timeout=300)
            pool.release(x)
            until_status(pool, x.guest, "stopped", timeout=60)
            y = pool.lease("lease1", arch, timeout=60)
            c.check('keep = "lease": stopped at release, cold-started by the next lease',
                    (y.guest, y.start), (x.guest, Start.COLD))
            pool.release(y)
            # revalidate with an edited class: its guests retire; the next
            # lease boots under the new key.
            old = pool.lease("one", arch, timeout=60)
            pool.release(old)
            edited = CLASSES.replace('[class.one]\nextends = "standard"',
                                     '[class.one]\nextends = "standard"\nargs = ["--cpu", "Haswell"]')
            retired = pool.revalidate(loads(edited, "pool-cases-edited"))
            new = pool.lease("one", arch, timeout=300)
            c.check("revalidate(): an edited class retires its guests; the next lease boots anew",
                    (retired, new.start, new.key != old.key), ([f"one/{arch}"], Start.BOOTED, True))
            pool.release(new)

        # close(): a held lease is lost, a waiter is woken, later leases refused.
        held = pool.lease("one", arch, timeout=60)
        woken: list[str] = []

        def wait_one() -> None:
            try:
                pool.lease("one", arch, timeout=120)
                woken.append("leased")
            except LeaseMiss as ex:
                woken.append(ex.reason.value)
        w3 = threading.Thread(target=wait_one)
        w3.start()
        time.sleep(1)
        t = time.monotonic()
        pool.close()
        w3.join(30)
        c.check("close(): a waiting lease is woken with pool-closed, promptly",
                (woken, time.monotonic() - t < 30), ([MissReason.POOL_CLOSED.value], True))
        try:
            res_c = pool.release(held)
            closed_res: object = (res_c.outcome, res_c.loss)
        except Exception as ex:  # noqa: BLE001 -- a release that raises is the wrong answer
            closed_res = f"{type(ex).__name__}: {ex}"
        c.check("close(): a held lease is lost (closed)", closed_res,
                (ReleaseOutcome.LOST, Loss.CLOSED))
        r, _ = miss(lambda: pool.lease("standard", arch, timeout=5))
        c.check("close(): later leases miss, pool-closed", r, MissReason.POOL_CLOSED.value)
        c.check("close(): nothing of the pool is left running", census(root), [])
    except Exception as e:  # a contract that throws is a failure, reported
        c.check(f"the cases ran without raising ({type(e).__name__}: {e})", False, True)
    finally:
        pool.close()
    print(f"pool-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
