"""guest.py -- one pooled guest's life: boot, snapshot, restore, stop, cold start.

The mechanics measured in docs/AXL-Test-Guest-Pool-Design.md §6.3 step 1:

- Booted ONCE through run-qemu.sh with klass.launch_args -- the same
  arguments the class key is a dry run of -- and snapshot `clean` taken over
  QMP once the agent answers.
- A restore is an in-process QMP `snapshot-load` (~110-180 ms).
- A cold start (after stop) is a NEW QEMU from the recorded argv, started
  PAUSED and restored over QMP, then continued -- never `-loadvm`, which
  reads the VM state from the first snapshot-capable disk -- with a NEW
  virtiofsd at the same socket path (the guest's virtio-fs session survives
  it) and a fresh forwarded port (~0.36-0.44 s).

Process ownership: EVERY guest this module returns is the caller's child,
killed with the THREAD that started it (PR_SET_PDEATHSIG is per thread) --
a boot ends by handing its guest over to a cold start, since the one
run-qemu.sh started lives under its own `timeout` and would outlive the
caller. So a service starts guests from long-lived threads, and sweeps
orphans (by state directory) when it starts: a crash mid-boot can still
leave run-qemu.sh's guest behind.
"""
from __future__ import annotations

import asyncio
import ctypes
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path
from typing import Any, cast

from qemu.qmp import QMPClient

from axlpool.klass import AGENT_GUEST_PORT, POOL_ENV, GuestSpec, launch_args
from axl_agent import AgentClient

SNAPSHOT_TAG = "clean"
SHARE_PROBE = ".axlpool-probe"
BOOT_LIFETIME_S = 86400        # run-qemu.sh's --timeout for a booted guest
SUN_PATH_MAX = 107             # a UNIX socket path's longest, without its NUL
# The vmstate image's VIRTUAL size: qcow2 allocates as it is written, so a
# size no guest RAM can outgrow costs nothing. It must exist before QEMU
# starts (the -blockdev opens it).
VMSTATE_VIRTUAL_SIZE = "64G"
_PR_SET_PDEATHSIG = 1


# The guest's SECOND QMP monitor, held by the pool for events: QMP serves one
# client per monitor, so qmp.sock stays free for a test's own commands.
EVENTS_SOCK = "events.sock"
# The events that are the GUEST's doing -- it left its clean state (STOP and
# RESUME are the pool's own snapshot loads, and are not recorded).
GUEST_EVENTS = frozenset({"RESET", "SHUTDOWN", "POWERDOWN", "GUEST_PANICKED"})


class _EventLog:
    """A thread holding the events monitor open, recording GUEST_EVENTS'
    names in arrival order. mark() starts the record over (after the pool's
    own restore or reset); stop() ends it."""

    def __init__(self, path: Path) -> None:
        self._path = path
        self._names: list[str] = []
        self._lock = threading.Lock()
        self._loop = asyncio.new_event_loop()
        self._task: asyncio.Task[None] | None = None
        self._ready = threading.Event()
        self.connected = False
        self._thread = threading.Thread(target=self._run, name="axlpool-events", daemon=True)
        self._thread.start()
        self._ready.wait(QMP_TIMEOUT_S)

    def _run(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._task = self._loop.create_task(self._listen())
        try:
            self._loop.run_until_complete(self._task)
        except (asyncio.CancelledError, Exception):  # noqa: BLE001 -- the guest ended; so do we
            pass
        finally:
            self._ready.set()
            self._loop.close()

    async def _listen(self) -> None:
        client = QMPClient("axlpool-events")
        try:
            await asyncio.wait_for(client.connect(str(self._path)), QMP_TIMEOUT_S)
            self.connected = True
            self._ready.set()
            async for ev in client.events:
                name = str(ev.get("event", ""))
                if name in GUEST_EVENTS:
                    with self._lock:
                        self._names.append(name)
        finally:
            self._ready.set()
            try:
                await client.disconnect()
            except Exception:  # noqa: BLE001 -- closing a dead session
                pass

    def names(self) -> list[str]:
        with self._lock:
            return list(self._names)

    def mark(self) -> None:
        with self._lock:
            self._names.clear()

    def stop(self) -> None:
        task = self._task
        if task is not None and self._loop.is_running():
            try:
                self._loop.call_soon_threadsafe(task.cancel)
            except RuntimeError:     # the loop already closed
                pass
        self._thread.join(QMP_TIMEOUT_S)


class HandoverFailed(RuntimeError):
    """Guest.boot booted and snapshotted the guest, but the cold start that
    hands it over to the caller failed: the CLASS boots, and the state
    (snapshot, launch.json) is good -- never a boot failure."""


# Every QMP connect and command has a deadline: a stuck or stopped QEMU
# accepts the socket and never greets, and must not hang the pool with it.
QMP_TIMEOUT_S = 10.0


# run-qemu.sh's user-network netdev (--net: NETDEV="user,id=net0").
NETDEV_ID = "net0"


class Qmp:
    """One QMP session whose connect and every command have a deadline.
    Synchronous, over qemu.qmp's async client: the legacy client cannot
    time out a connect."""

    def __init__(self, path: Path, timeout: float = QMP_TIMEOUT_S) -> None:
        self._timeout = timeout
        self._loop = asyncio.new_event_loop()
        self._client = QMPClient("axlpool")
        try:
            self._run(self._client.connect(str(path)))
        except BaseException:
            self._loop.close()
            raise

    def _run(self, aw: Any) -> Any:
        return self._loop.run_until_complete(asyncio.wait_for(aw, self._timeout))

    def cmd(self, name: str, args: dict[str, object] | None = None) -> Any:
        """Execute @name; raises on a QMP error or the deadline."""
        return self._run(self._client.execute(name, args))

    def close(self) -> None:
        try:
            self._run(self._client.disconnect())
        except Exception:  # noqa: BLE001 -- closing a dead session
            pass
        finally:
            self._loop.close()

    def __enter__(self) -> Qmp:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port: int = s.getsockname()[1]
        return port


def _die_with_parent() -> None:
    """preexec_fn: SIGKILL this child when the thread that started it ends."""
    ctypes.CDLL(None, use_errno=True).prctl(_PR_SET_PDEATHSIG, signal.SIGKILL)


def _cmdline(pid: int) -> list[str]:
    return Path(f"/proc/{pid}/cmdline").read_bytes().rstrip(b"\0").decode().split("\0")


def pid_alive(pid: int) -> bool:
    """Whether @pid is a live process. A zombie -- dead, its parent yet to
    reap it, as a cold-started QEMU is until its Popen is polled -- is not."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    return stat[stat.rindex(")") + 2] != "Z"


def _pid_by_arg(exe_part: str, arg_part: str, timeout: float = 10.0) -> int:
    """The one process whose executable path holds @exe_part and whose
    command line holds @arg_part."""
    end = time.monotonic() + timeout
    while True:
        found: list[int] = []
        for proc in Path("/proc").iterdir():
            if not proc.name.isdigit():
                continue
            try:
                args = (proc / "cmdline").read_bytes().split(b"\0")
            except OSError:
                continue
            if args and exe_part.encode() in args[0] and any(arg_part.encode() in a for a in args):
                found.append(int(proc.name))
        if len(found) == 1:
            return found[0]
        if time.monotonic() > end:
            raise RuntimeError(f"expected one {exe_part} naming {arg_part}, found {found}")
        time.sleep(0.05)


def _next_serial_log(state: Path) -> Path:
    """A new serial log per QEMU process (QEMU truncates the file it opens):
    serial-0.log for the boot, then serial-1.log, ... per cold start."""
    n = sum(1 for _ in state.glob("serial-*.log"))
    return state / f"serial-{n}.log"


def _qemu_img(qemu: Path) -> str:
    """The qemu-img beside @qemu, else the one on PATH."""
    beside = qemu.with_name("qemu-img")
    return str(beside) if beside.is_file() else (shutil.which("qemu-img") or "qemu-img")


class Guest:
    """A guest at its agent, with snapshot `clean` in its state directory."""

    def __init__(self, state: Path, share: Path, port: int, qemu_pid: int,
                 virtiofsd_pids: list[int], share_volume: str, boot_s: float,
                 owned: list[subprocess.Popen[bytes]], serial_log: Path) -> None:
        self.state = state
        self.share = share
        self.port = port
        self.qemu_pid = qemu_pid
        self.virtiofsd_pids = virtiofsd_pids
        self.share_volume = share_volume
        self.boot_s = boot_s
        self._owned = owned
        self.serial_log = serial_log      # this QEMU process's console (QEMU truncates on open)
        self.agent = AgentClient("127.0.0.1", port)
        self._events: _EventLog | None = None   # a cold-started guest's (every served one)

    def guest_events(self) -> list[str]:
        """GUEST_EVENTS this guest raised since its clean state was last
        restored (or the pool's own system_reset), in order."""
        ev = self._events                 # a local: kill() may clear it concurrently
        return ev.names() if ev is not None else []

    @property
    def events_live(self) -> bool:
        """Whether the events monitor is connected (False: a launch from
        before it existed, or someone else holding events.sock)."""
        ev = self._events
        return ev is not None and ev.connected

    # -- QMP --------------------------------------------------------------

    def _qmp(self) -> Qmp:
        return Qmp(self.state / "qmp.sock")

    @staticmethod
    def _devices(q: Qmp) -> list[str]:
        """The snapshot's devices: vmstate and the two qcow2 drives. Node
        names are regenerated per process, so they are found by file."""
        nodes = cast("list[dict[str, Any]]", q.cmd("query-named-block-nodes", {"flat": True}))
        ours = [str(n["node-name"]) for n in nodes
                if n["drv"] == "qcow2" and str(n["file"]).endswith(("/vars.qcow2", "/disk.qcow2"))]
        if len(ours) != 2:
            raise RuntimeError(f"expected the vars and disk qcow2 nodes, found {ours}")
        return ["vmstate", *ours]

    @staticmethod
    def _job(q: Qmp, verb: str, job: str, timeout: float = 120.0) -> None:
        q.cmd(verb, {"job-id": job, "tag": SNAPSHOT_TAG, "vmstate": "vmstate",
                     "devices": Guest._devices(q)})
        end = time.monotonic() + timeout
        while True:
            jobs = cast("list[dict[str, Any]]", q.cmd("query-jobs"))
            mine = [j for j in jobs if j["id"] == job]
            if mine and mine[0]["status"] == "concluded":
                q.cmd("job-dismiss", {"id": job})
                if mine[0].get("error"):
                    raise RuntimeError(f"{verb} failed: {mine[0]['error']}")
                return
            if time.monotonic() > end:
                # Cancel and dismiss, or the next save/load meets the ID.
                for cmd in ("job-cancel", "job-dismiss"):
                    try:
                        q.cmd(cmd, {"id": job})
                    except Exception:  # noqa: BLE001 -- best effort: the job may have moved on
                        pass
                raise TimeoutError(f"{verb} did not conclude in {timeout} s")
            time.sleep(0.005)

    def _wait_agent(self, timeout: float, what: str) -> None:
        """/ping until it answers; fail at once if QEMU has died."""
        end = time.monotonic() + timeout
        while True:
            if not pid_alive(self.qemu_pid):
                raise RuntimeError(f"{what}: QEMU ({self.qemu_pid}) exited before the agent answered")
            try:
                if self.agent.probe(end - time.monotonic()) == "pong\n":
                    return
            except Exception:  # noqa: BLE001 -- not up yet
                pass
            if time.monotonic() > end:
                raise TimeoutError(f"{what}: the agent did not answer within {timeout} s")
            time.sleep(0.2)

    # -- life -------------------------------------------------------------

    @classmethod
    def boot(cls, spec: GuestSpec, state: Path, share: Path, timeout: float = 300.0) -> Guest:
        """Boot @spec, wait for its agent, take snapshot `clean`, and record
        what a cold start needs in @state/launch.json. Nothing is left
        running if any step fails."""
        share.mkdir(parents=True, exist_ok=True)
        state.mkdir(parents=True, exist_ok=True)
        (state / "launch.json").unlink(missing_ok=True)   # never a stale record
        (state / "qmp.sock").unlink(missing_ok=True)
        args = launch_args(spec, state, share)
        dry = subprocess.run([str(spec.run_qemu), *args], capture_output=True, text=True,
                             env=dict(os.environ, **POOL_ENV, QEMU_DRYRUN="1"), cwd=spec.tree,
                             check=False)
        qemu_bin = next((Path(ln[len("QEMU_DRYRUN: "):]) for ln in dry.stdout.splitlines()
                         if ln.startswith("QEMU_DRYRUN: ")), Path("qemu-system"))
        subprocess.run([_qemu_img(qemu_bin), "create", "-q", "-f", "qcow2",
                        str(state / "vmstate.qcow2"), VMSTATE_VIRTUAL_SIZE], check=True)
        token = os.urandom(8).hex()
        (share / SHARE_PROBE).write_text(f"{token}\r\n")
        t0 = time.monotonic()
        serial = _next_serial_log(state)
        out = subprocess.run([str(spec.run_qemu), *args, "--timeout", str(BOOT_LIFETIME_S),
                              "--serial-log", str(serial)],
                             capture_output=True, text=True, env=dict(os.environ, **POOL_ENV),
                             cwd=spec.tree, check=False)
        text = out.stdout + out.stderr
        facts = dict(ln.split("=", 1) for ln in out.stdout.splitlines()
                     if re.match(r"^[A-Z_0-9]+=", ln))
        vpid = facts.get("VIRTIOFSD_PID")
        mport = facts.get(f"HOSTFWD_{AGENT_GUEST_PORT}")
        try:
            qemu_pid = _pid_by_arg("qemu-system", str(state / "qmp.sock"), timeout=2.0)
        except RuntimeError:
            qemu_pid = 0
        g = cls(state, share, int(mport) if mport else 0, qemu_pid,
                [int(vpid)] if vpid else [], "", 0.0, [], serial)
        try:
            if out.returncode != 0 or not mport or not vpid or not qemu_pid:
                raise RuntimeError(f"run-qemu.sh did not start the guest:\n{text[-1500:]}")
            argv = _cmdline(qemu_pid)
            g._wait_agent(timeout, "boot")
            g.boot_s = time.monotonic() - t0
            g.share_volume = g._find_share(token)
            with g._qmp() as q:
                cls._job(q, "snapshot-save", "save")
            (state / "launch.json").write_text(json.dumps({
                "qemu": argv,
                "virtiofsd": [_cmdline(p) for p in g.virtiofsd_pids],
                "run_tmpdir": facts.get("TMPDIR", ""),
                "share_volume": g.share_volume,
                "port": g.port,
            }, indent=1))
        except BaseException:
            g.kill()
            raise
        # Hand over: the guest run-qemu.sh started lives under ITS `timeout`
        # (a parent per guest, tied to no one), so it would outlive the
        # caller. Stopped, and cold-started from what was just recorded, it
        # is the CALLER's child like every other guest -- killed with the
        # thread that started it -- for a stop and a ~0.4 s cold start on a
        # 10-22 s boot.
        g.stop()
        try:
            g = cls.cold_start(state, share, timeout)
        except Exception as e:  # noqa: BLE001 -- classified for the caller, see HandoverFailed
            raise HandoverFailed(f"booted, but the hand-over failed: {type(e).__name__}: {e}") from e
        g.boot_s = time.monotonic() - t0      # the whole boot, hand-over included
        return g

    def _find_share(self, token: str) -> str:
        self.agent.exec("map -r")
        seen: list[str] = []
        for i in range(8):
            r = self.agent.exec(f"type fs{i}:\\{SHARE_PROBE}")
            # Detection only (the token is random, so a match is the share):
            # `type` adds a line end of its own.
            if (r.stdout or "").strip() == token:
                return f"fs{i}"
            seen.append(f"fs{i}: {r.outcome} {r.status} {(r.stdout or '')[:40]!r}")
        raise RuntimeError("the share is not visible as any fsN: volume: " + "; ".join(seen))

    def drop_forwards(self) -> list[str]:
        """Remove every host forward but the agent's -- what a lease added
        (pooled test_run_background's test_add_port_forward), or anything
        else. QEMU keeps its user-network forwards OUTSIDE the snapshot (a
        restore leaves them, measured), so a forward a lease added would
        otherwise hold its host port into every later lease. Returns the
        rules removed, as hostfwd_remove names them."""
        removed: list[str] = []
        with self._qmp() as q:
            text = str(q.cmd("human-monitor-command", {"command-line": "info usernet"}))
            for ln in text.splitlines():
                f = ln.split()
                if len(f) < 6 or not f[0].endswith("[HOST_FORWARD]"):
                    continue
                proto, addr, port = f[0].split("[", 1)[0].lower(), f[2], f[3]
                if proto == "tcp" and port == str(self.port):
                    continue                     # the agent's own
                rule = f"{proto}:{'' if addr == '*' else addr}:{port}"
                said = q.cmd("human-monitor-command",
                             {"command-line": f"hostfwd_remove {NETDEV_ID} {rule}"})
                # QEMU answers a confirmation, or why not; an unremoved rule
                # keeps its host port, so it is an error, not a quiet skip.
                if "removed" not in str(said):
                    raise RuntimeError(f"hostfwd_remove {rule}: {str(said).strip()}")
                removed.append(rule)
        return removed

    def restore(self) -> None:
        """Back to snapshot `clean`, in this process; returns once the agent
        answers again."""
        self.agent.close()
        with self._qmp() as q:
            self._job(q, "snapshot-load", "load")
        self._wait_agent(30, "restore")
        ev = self._events
        if ev is not None:
            ev.mark()

    def alive(self) -> bool:
        """Whether this guest's QEMU still runs."""
        return bool(self.qemu_pid) and pid_alive(self.qemu_pid)

    def exit_detail(self) -> str:
        """How QEMU ended: its exit status or signal when this process owns
        it (a cold start), else that it is gone."""
        for proc in self._owned:
            if proc.pid == self.qemu_pid and proc.poll() is not None:
                rc = proc.returncode
                return f"killed by signal {-rc}" if rc < 0 else f"exited with status {rc}"
        return "exited (not this process's child: status unknown)"

    def qmp_status(self) -> str:
        """QMP query-status: "running", "paused", "internal-error", ..."""
        with self._qmp() as q:
            return str(cast("dict[str, Any]", q.cmd("query-status"))["status"])

    def system_reset(self, timeout: float = 120.0) -> None:
        """A full system_reset, then the agent again (the start script
        reruns it)."""
        self.agent.close()
        with self._qmp() as q:
            q.cmd("system_reset")
        time.sleep(1.0)
        self._wait_agent(timeout, "system_reset")
        ev = self._events
        if ev is not None:
            ev.mark()                    # the pool's own reset, not the guest's

    def pids(self) -> list[int]:
        """The guest's processes: QEMU and its virtiofsd."""
        return [p for p in (self.qemu_pid, *self.virtiofsd_pids) if p]

    def stop(self, timeout: float = 15.0) -> None:
        """End the guest, KEEPING its state directory for a cold start."""
        self.agent.close()
        try:
            with self._qmp() as q:
                q.cmd("quit")
        except Exception:  # noqa: BLE001 -- a guest whose QMP is gone is ended below
            pass
        end = time.monotonic() + timeout
        while time.monotonic() < end and any(pid_alive(p) for p in self.pids()):
            time.sleep(0.05)
        self.kill()

    def kill(self) -> None:
        """End the guest's processes now (state kept)."""
        self.agent.close()
        if self._events is not None:
            self._events.stop()
            self._events = None
        for p in self.pids():
            if pid_alive(p):
                try:
                    os.kill(p, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        for proc in self._owned:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        end = time.monotonic() + 10
        while time.monotonic() < end and any(pid_alive(p) for p in self.pids()):
            time.sleep(0.05)

    @classmethod
    def cold_start(cls, state: Path, share: Path, timeout: float = 60.0) -> Guest:
        """A NEW QEMU from @state's recorded launch, restored to `clean`.
        Refused while a QEMU still serves @state's QMP socket."""
        rec = json.loads((state / "launch.json").read_text())
        live = False
        try:
            Qmp(state / "qmp.sock", timeout=2.0).close()
            live = True
        except Exception:  # noqa: BLE001 -- nothing listening: the expected case
            pass
        if live:
            raise RuntimeError(f"a QEMU still serves {state / 'qmp.sock'}: stop it first")
        old_port: int = rec["port"]
        old_tmp: str = rec["run_tmpdir"]
        port = _free_port()
        run = state / "run"
        shutil.rmtree(run, ignore_errors=True)
        run.mkdir()

        def fix(a: str) -> str:
            if old_tmp:
                a = a.replace(old_tmp, str(run))
            return re.sub(rf"(hostfwd=tcp:[0-9.]*:){old_port}-:", rf"\g<1>{port}-:", a)

        qemu_argv = [fix(a) for a in rec["qemu"]] + ["-S"]
        for a in qemu_argv:
            for m in re.finditer(r"(?:path=|unix:)(/[^,]+)", a):
                if len(m.group(1)) > SUN_PATH_MAX:
                    raise RuntimeError(f"socket path {m.group(1)} is {len(m.group(1))} bytes; "
                                       f"the limit is {SUN_PATH_MAX}")
        owned: list[subprocess.Popen[bytes]] = []
        serial = _next_serial_log(state)
        g = cls(state, share, port, 0, [], rec["share_volume"], 0.0, owned, serial)
        try:
            for v in rec["virtiofsd"]:
                vargv = [fix(a) for a in v]
                owned.append(subprocess.Popen(vargv, stdout=(run / "virtiofsd.log").open("wb"),
                                              stderr=subprocess.STDOUT,
                                              preexec_fn=_die_with_parent))
                g.virtiofsd_pids.append(owned[-1].pid)
                sock = next((Path(a.split("=", 1)[1]) for a in vargv
                             if a.startswith("--socket-path=")), None)
                end = time.monotonic() + 10
                while sock is not None and not sock.exists():
                    if owned[-1].poll() is not None or time.monotonic() > end:
                        raise RuntimeError("virtiofsd did not start:\n"
                                           + (run / "virtiofsd.log").read_text(errors="replace")[-800:])
                    time.sleep(0.01)
            (state / "qmp.sock").unlink(missing_ok=True)
            t0 = time.monotonic()
            # The serial console is QEMU's stdout (-serial stdio), as under
            # run-qemu.sh --background; its diagnostics go apart.
            qp = subprocess.Popen(qemu_argv, stdout=serial.open("wb"),
                                  stderr=(run / "qemu.err").open("wb"), preexec_fn=_die_with_parent)
            owned.append(qp)
            g.qemu_pid = qp.pid
            q = None
            end = time.monotonic() + timeout
            while q is None:
                try:
                    q = g._qmp()
                except Exception:  # noqa: BLE001 -- QMP not up yet (ConnectError is no OSError)
                    if qp.poll() is not None or time.monotonic() > end:
                        raise RuntimeError("the cold-started QEMU never served QMP:\n"
                                           + (run / "qemu.err").read_text(errors="replace")[-800:])
                    time.sleep(0.02)
            try:
                cls._job(q, "snapshot-load", "load")
                q.cmd("cont")
            finally:
                q.close()
            g._wait_agent(timeout, "cold start")
            g.boot_s = time.monotonic() - t0
            g._events = _EventLog(state / EVENTS_SOCK)
        except BaseException:
            g.kill()
            raise
        return g
