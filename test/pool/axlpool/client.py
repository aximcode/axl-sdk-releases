"""client.py -- the service's client side (service.py's PROTOCOL), and the
`axlpool run` wrapper a test runs under.

PoolClient is synchronous and one request at a time: what a test or a
script needs. Every error the service answers is raised as PoolError(code,
detail, reason); a service that closes the connection is PoolError("eof").
Socket errors (no service, a timeout) are OSError.

`axlpool run` is service.py's THE TEST-SIDE WRAPPER, built on it.
"""
from __future__ import annotations

import itertools
import json
import os
import select
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from types import FrameType, TracebackType
from typing import Any, cast

from axlpool import classes
from axlpool.classes import CLASSES_FILE
from axlpool.service import AXLPOOL_EXIT_MISS, POOL_MISS_MARKER, PROTOCOL_VERSION, default_socket

CLIENT_NAME = "axlpool-client/1"
LEASE_REPLY_SLACK_S = 30.0     # a lease's answer may come this long after its own timeout
CHILD_TERM_GRACE_S = 10.0      # after a SIGTERM (service gone, or forwarded): then KILL
WATCH_STEP_S = 0.5             # how often run() looks at the child and the connection
DEFAULT_LEASE_TIMEOUT_S = 120.0  # must fit inside the test's own limit (CI's is 240 s)
EXIT_NOT_RUNNABLE = 127        # the test command could not be started (the shell's code)
FORWARDED_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)


class PoolError(Exception):
    """An error the service answered (its `error` code), or "eof"."""

    def __init__(self, code: str, detail: str, reason: str = "") -> None:
        super().__init__(f"{code}{f' ({reason})' if reason else ''}: {detail}")
        self.code = code
        self.detail = detail
        self.reason = reason


class PoolClient:
    """One connection to the service, `hello` done. A context manager."""

    def __init__(self, sock: Path | None = None, *, client: str = CLIENT_NAME,
                 timeout: float = 10.0) -> None:
        self.sock = sock if sock is not None else default_socket()
        self._timeout = timeout
        self._s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._s.settimeout(timeout)
        self._buf = b""
        self._ids = itertools.count(1)
        try:
            self._s.connect(str(self.sock))
            hello = self._call({"op": "hello", "version": PROTOCOL_VERSION, "client": client})
        except BaseException:
            self._s.close()
            raise
        self.instance = str(hello.get("instance", ""))
        # The code the service runs (service.py `code_identity`); "" from a
        # service older than it.
        self.code = str(hello.get("code", ""))
        self.agents = cast("dict[str, dict[str, Any]]", hello.get("agents", {}))

    def __enter__(self) -> PoolClient:
        return self

    def __exit__(self, exc_type: type[BaseException] | None, exc: BaseException | None,
                 tb: TracebackType | None) -> None:
        self.close()

    def close(self) -> None:
        self._s.close()

    def fileno(self) -> int:
        return self._s.fileno()

    def lease(self, tree: Path, class_name: str, arch: str, *, timeout: float = 300.0,
              booted_after: float | None = None, agent: str = "service",
              traceparent: str | None = None) -> dict[str, Any]:
        """A lease's wire fields (id, token, env, ...). PoolError on a miss."""
        req: dict[str, object] = {"op": "lease", "tree": str(tree.resolve()), "class": class_name,
                                  "arch": arch, "timeout": timeout, "agent": agent}
        if booted_after is not None:
            req["booted_after"] = booted_after
        if traceparent is not None:
            req["traceparent"] = traceparent
        resp = self._call(req, wait=timeout + LEASE_REPLY_SLACK_S)
        lease = cast("dict[str, Any]", resp.get("lease", {}))
        if not isinstance(lease.get("env"), dict):
            raise PoolError("bad-reply", "a lease without its env (a service older than this client?)")
        lease["trace_id"] = resp.get("trace_id", lease.get("trace_id", ""))
        return lease

    def release(self, lease: dict[str, Any], *, keep_files: bool = False) -> dict[str, Any]:
        """The release result: outcome, cause, loss, detail, kept."""
        resp = self._call({"op": "release", "lease": lease["id"], "token": lease["token"],
                           "keep_files": keep_files}, wait=LEASE_REPLY_SLACK_S * 4)
        return cast("dict[str, Any]", resp.get("result", {}))

    def restore(self, lease_id: str, restore_token: str, *, wait: float = LEASE_REPLY_SLACK_S * 2) -> int:
        """A held lease's guest back to clean (service.py `restore`); the
        milliseconds it took, waiting at most @wait seconds for the answer.
        PoolError when refused or failed."""
        resp = self._call({"op": "restore", "lease": lease_id, "restore_token": restore_token},
                          wait=wait)
        return int(cast("dict[str, Any]", resp.get("result", {})).get("duration_ms", 0))

    def guests(self, tree: Path | None = None) -> list[dict[str, Any]]:
        req: dict[str, object] = {"op": "guests"}
        if tree is not None:
            req["tree"] = str(tree.resolve())
        return cast("list[dict[str, Any]]", self._call(req).get("guests", []))

    def revalidate(self, tree: Path) -> list[str]:
        resp = self._call({"op": "revalidate", "tree": str(tree.resolve())}, wait=LEASE_REPLY_SLACK_S)
        return cast("list[str]", resp.get("retired", []))

    def ping(self) -> None:
        self._call({"op": "ping"})

    def _call(self, req: dict[str, object], wait: float | None = None) -> dict[str, Any]:
        rid = str(next(self._ids))
        self._s.settimeout(wait if wait is not None else self._timeout)
        self._s.sendall(json.dumps({**req, "id": rid}).encode() + b"\n")
        while True:
            resp = self._recv()
            if resp.get("id") == rid:
                break
            if resp.get("id") is None and not resp.get("ok"):
                break                    # an error about the connection itself (a refused peer)
            # Else: a late answer to a call that timed out -- not ours.
        if not resp.get("ok"):
            raise PoolError(str(resp.get("error", "")), str(resp.get("detail", "")),
                            str(resp.get("reason", "")))
        return resp

    def _recv(self) -> dict[str, Any]:
        while b"\n" not in self._buf:
            chunk = self._s.recv(65536)
            if not chunk:
                raise PoolError("eof", "the service closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        try:
            resp: object = json.loads(line)
        except ValueError:
            resp = None
        if not isinstance(resp, dict):
            raise PoolError("bad-reply", f"not a JSON object: {line[:200]!r}")
        return cast("dict[str, Any]", resp)

    def closed_by_peer(self) -> bool:
        """Without blocking: has the service closed the connection? (A
        held lease has nothing in flight, so anything readable is EOF.)"""
        ready, _, _ = select.select([self._s], [], [], 0)
        if not ready:
            return False
        try:
            return self._s.recv(1, socket.MSG_PEEK) == b""
        except OSError:
            return True


def available(sock: Path | None = None, timeout: float = 2.0) -> bool:
    """Whether a service answers on @sock (default: default_socket())."""
    try:
        with PoolClient(sock, timeout=timeout):
            return True
    except (OSError, PoolError, ValueError):
        return False


def find_tree(start: Path) -> Path | None:
    """The nearest directory at or above @start with test/pool/classes.toml."""
    for d in (start.resolve(), *start.resolve().parents):
        if (d / CLASSES_FILE).is_file():
            return d
    return None


def _miss(msg: str) -> int:
    """A POOL failure: said, then marked for the runner (POOL_MISS_MARKER --
    the one line run-integration.sh reads a 77 as the pool's, not the
    test's own SKIP), and AXLPOOL_EXIT_MISS to return."""
    _say(msg)
    print(f"{POOL_MISS_MARKER} {msg}", file=sys.stderr, flush=True)
    return AXLPOOL_EXIT_MISS


def _say(msg: str) -> None:
    print(f"axlpool run: {msg}", file=sys.stderr, flush=True)


class _Interrupted(Exception):
    """A signal while no child runs: the wrapper ends, 128+N."""

    def __init__(self, signum: int) -> None:
        super().__init__(signum)
        self.signum = signum


class _Child:
    """The test's process group, as the signal handler sees it."""

    def __init__(self) -> None:
        self.proc: subprocess.Popen[bytes] | None = None
        self.spawning = False
        self.releasing = False      # the test is over: a signal waits for the release
        self.pending = 0            # a signal held while spawning or releasing
        self.ended = False          # its group was killed after it exited: the pgid may be reused
        self.term_at = 0.0          # when a forwarded signal asked it to end (time.monotonic())

    def signal_group(self, signum: int) -> None:
        if self.proc is not None and not self.ended:
            try:
                os.killpg(self.proc.pid, signum)
            except ProcessLookupError:
                pass                # the group is gone already

    def end_group(self) -> None:
        """The child has exited: kill what it left, then never signal again."""
        self.signal_group(signal.SIGKILL)
        self.ended = True

    def on_signal(self, signum: int, _frame: FrameType | None) -> None:
        if self.spawning or self.releasing:
            self.pending = signum   # delivered after Popen returns / reported after release
        elif self.proc is not None and self.proc.poll() is None:
            self.signal_group(signum)
            self.term_at = self.term_at or time.monotonic()
        else:
            raise _Interrupted(signum)


def run(sock: Path, tree: Path, class_name: str, arch: str, cmd: list[str], *,
        timeout: float = DEFAULT_LEASE_TIMEOUT_S, keep_files_on_fail: bool = False,
        agent: str = "service", expect_code: str | None = None) -> int:
    """service.py's THE TEST-SIDE WRAPPER: @cmd on a leased guest, in a
    session (process group) of its own. Returns the child's status (128+N
    for signal N), or AXLPOOL_EXIT_MISS for any pool failure -- a miss, no
    service, the service gone, a failed or LOST release, or (@expect_code
    given) a service whose code identity is not @expect_code, checked before
    anything is leased.

    SIGTERM, SIGINT and SIGHUP go to the child's whole group while it runs;
    a group still running CHILD_TERM_GRACE_S later is KILLed (137), released
    as any ended test -- the wrapper's own killer cannot do it (`timeout -k`
    reaches the wrapper alone: the test is in a group of its own).
    Before it runs (waiting for a guest) one ends the wrapper with 128+N; its
    closed connection hands the lease back. After it ran, the release
    finishes first and the test's own status stands. The child gets its own
    process group, not a new session; when stdin is a terminal its stdin is
    /dev/null, so a test that reads gets EOF instead of being stopped by
    SIGTTIN outside the terminal's foreground group.

    The child's environment is the lease's (Lease.env()) plus
    AXL_POOL_LEASE_MS, the lease's cost as this wrapper measured it, and
    AXL_POOL_SOCKET, the service's socket (for `axlpool restore`); and the
    wrapper says, on stderr, `leased ...` once granted and `released ...`
    once handed back -- the TIMING lines of scripts/axl-common.sh."""
    child = _Child()
    old = {s: signal.signal(s, child.on_signal) for s in FORWARDED_SIGNALS}
    try:
        return _run(child, sock, tree, class_name, arch, cmd, timeout, keep_files_on_fail, agent,
                    expect_code)
    except _Interrupted as e:
        _say(f"interrupted by {signal.Signals(e.signum).name}")
        return 128 + e.signum
    finally:
        for s, h in old.items():
            signal.signal(s, h)
        child.end_group()            # stragglers must not use a released guest


def _ms_since(t0: float) -> int:
    return int((time.monotonic() - t0) * 1000)


def _run(child: _Child, sock: Path, tree: Path, class_name: str, arch: str, cmd: list[str],
         timeout: float, keep_files_on_fail: bool, agent: str, expect_code: str | None) -> int:
    t_lease = time.monotonic()
    try:
        pc = PoolClient(sock)
    except (OSError, PoolError, ValueError) as e:
        return _miss(f"no pool service on {sock}: {e}")
    with pc:
        # Held to the code the RUN found the service running (asked once,
        # `axlpool code`): one restarted with other code since -- or older
        # than the identity -- would run this test on code the run's
        # skip-cache key does not name. A miss boots it instead.
        if expect_code is not None and pc.code != expect_code:
            return _miss(f"the pool service runs code ({pc.code[:12] or 'none reported'}) other "
                         f"than this run's ({expect_code[:12]}): it restarted, or is older")
        try:
            lease = pc.lease(tree, class_name, arch, timeout=timeout, agent=agent,
                             traceparent=os.environ.get("TRACEPARENT"))
        except (OSError, PoolError, ValueError) as e:
            return _miss(f"no guest: {e}")
        # The TIMING lines (scripts/axl-common.sh, "Wall-time accounting"):
        # what the lease cost as THIS side saw it -- connect, queue, the
        # pool's start (its own start_ms) -- said, and handed to the test.
        lease_ms = _ms_since(t_lease)
        _say(f"leased {lease.get('guest', '?')} {lease.get('class_name', class_name)}/"
             f"{lease.get('arch', arch)} ({lease.get('start', '?')}, pool start "
             f"{lease.get('start_ms', '?')} ms) in {lease_ms} ms")
        env = {**os.environ, **cast("dict[str, str]", lease["env"]),
               "AXL_POOL_LEASE_MS": str(lease_ms), "AXL_POOL_SOCKET": str(sock)}
        # The class's run-qemu.sh options, one per line: run-qemu.sh's pooled
        # mode runs a call on the lease only when its machine is this one.
        # From the tree's classes file, as the service read it for the lease
        # (it reloads it per lease); unreadable, the variable is left unset
        # and every run-qemu.sh call boots, saying so.
        try:
            env["AXL_POOL_CLASS_ARGS"] = "\n".join(
                classes.load(tree / CLASSES_FILE).machine_args(class_name, arch))
        except (OSError, classes.ClassError) as e:
            _say(f"no class options for run-qemu.sh: {e}")
            env.pop("AXL_POOL_CLASS_ARGS", None)
        stdin = subprocess.DEVNULL if sys.stdin is not None and sys.stdin.isatty() else None
        child.spawning = True
        try:
            child.proc = subprocess.Popen(cmd, env=env, process_group=0, stdin=stdin)
        except OSError as e:
            _say(f"cannot run {cmd[0]}: {e}")
            child.spawning, child.releasing = False, True
            return _release(pc, lease, EXIT_NOT_RUNNABLE, keep_files=False, child=child)
        child.spawning = False
        if child.pending:
            child.signal_group(child.pending)
        proc = child.proc
        while proc.poll() is None:
            if pc.closed_by_peer():
                _miss("the pool service went away mid-run; ending the test")
                child.signal_group(signal.SIGTERM)
                try:
                    proc.wait(CHILD_TERM_GRACE_S)
                except subprocess.TimeoutExpired:
                    child.signal_group(signal.SIGKILL)
                    proc.wait()
                return AXLPOOL_EXIT_MISS
            if child.term_at and time.monotonic() - child.term_at >= CHILD_TERM_GRACE_S:
                _say(f"the test ignored the signal for {CHILD_TERM_GRACE_S:g} s; killing its group")
                child.signal_group(signal.SIGKILL)
                proc.wait()
                break
            try:
                proc.wait(WATCH_STEP_S)
            except subprocess.TimeoutExpired:
                pass
        rc = proc.returncode
        status = rc if rc >= 0 else 128 - rc
        child.releasing = True
        child.end_group()                     # before the guest goes back
        return _release(pc, lease, status, keep_files=keep_files_on_fail and status != 0,
                        child=child)


def _release(pc: PoolClient, lease: dict[str, Any], status: int, *, keep_files: bool,
             child: _Child) -> int:
    """Hand the guest back; @status unless the pool failed (77). A signal
    held during it is reported, not acted on: the test is over."""
    t_release = time.monotonic()
    try:
        res = pc.release(lease, keep_files=keep_files)
    except (OSError, PoolError, ValueError) as e:
        return _miss(f"the release failed: {e}")
    _say(f"released {lease.get('guest', '?')} ({res.get('outcome', '?')}) in {_ms_since(t_release)} ms")
    if child.pending:
        _say(f"{signal.Signals(child.pending).name} arrived after the test ended; released first")
    if res.get("kept"):
        _say(f"kept: {res['kept']}")
    if res.get("outcome") == "lost":
        return _miss(f"the guest was lost: {res.get('loss')} {res.get('detail', '')}".rstrip())
    return status
