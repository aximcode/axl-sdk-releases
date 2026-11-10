#!/usr/bin/env python3
"""client-cases.py -- axlpool.client and `axlpool run`, against a real service.

Usage: client-cases.py ARCH AGENT_EFI WORKDIR

Starts `python3 -m axlpool serve` on a socket in WORKDIR, then drives it the
way tests will: PoolClient directly, and `axlpool run` wrapping a child --
its environment, its exit status passed through, 77 for every pool failure
(a miss, no service, the service gone mid-run), files kept on failure, a
signal forwarded. Prints `  PASS:`/`  FAIL:` per case, then
`client-cases: <p> passed, <f> failed`.
"""
from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, cast

POOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(POOL_DIR))
sys.path.insert(0, str(POOL_DIR.parent / "integration" / "lib"))
from axlpool.client import CHILD_TERM_GRACE_S, PoolClient, PoolError, available  # noqa: E402
from axlpool.service import AXLPOOL_EXIT_MISS, tree_agent  # noqa: E402
from checks import Cases  # noqa: E402

TREE = POOL_DIR.parents[1]
POOL_VARS = sorted(["AXL_POOL_AGENT", "AXL_POOL_QMP", "AXL_POOL_HOST_DIR", "AXL_POOL_GUEST_DIR",
                    "AXL_POOL_SERIAL", "AXL_POOL_SERIAL_OFFSET", "AXL_POOL_LEASE", "AXL_POOL_CLASS",
                    "AXL_POOL_ARCH", "AXL_POOL_TRACE_ID", "AXL_POOL_AGENT_IMAGE",
                    "AXL_POOL_QEMU_PID", "AXL_POOL_START", "AXL_POOL_START_MS",
                    "AXL_POOL_RESTORE_TOKEN"])
# What `axlpool run` adds to Lease.env(): the lease's cost as the wrapper saw
# it, the service's socket (for `axlpool restore`), and the class's
# run-qemu.sh options, one per line (run-qemu.sh's pooled mode compares a
# run's machine with them).
RUN_VARS = sorted([*POOL_VARS, "AXL_POOL_LEASE_MS", "AXL_POOL_SOCKET", "AXL_POOL_CLASS_ARGS"])
# The repo's `standard` class's options, as AXL_POOL_CLASS_ARGS carries them.
STANDARD_ARGS = {"X64": "--bridges\n--nic-model\ne1000", "AARCH64": "--bridges"}


def timing_lines(stderr: str) -> list[str]:
    """`axlpool run`'s leased/released lines, every number as N."""
    return [re.sub(r"[0-9]+", "N", ln) for ln in stderr.splitlines()
            if ln.startswith(("axlpool run: leased ", "axlpool run: released "))]
TRACE = "4bf92f3577b34da6a3ce929d0e0e4736"


class Served:
    """One `axlpool serve` process on its own socket and state."""

    def __init__(self, work: Path, name: str, agent: Path) -> None:
        self.sock = work / f"{name}.sock"
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "axlpool", "serve", "--tree", str(TREE), "--agent", str(agent),
             "--state", str(work / f"{name}-state"), "--socket", str(self.sock), "--exit-idle", "0"],
            cwd=POOL_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(200):
            if available(self.sock):
                return
            time.sleep(0.05)

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(120)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def run(sock: Path, arch: str, *args: str, cls: str = "standard", extra: tuple[str, ...] = (),
        timeout: float = 300, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    """`axlpool run` with @args as the child's command line."""
    return subprocess.run(
        [sys.executable, "-m", "axlpool", "run", "--socket", str(sock), "--tree", str(TREE),
         "--class", cls, "--arch", arch, *extra, "--", *args],
        cwd=POOL_DIR, capture_output=True, text=True, timeout=timeout, check=False, env=env)


class OldService:
    """A service older than the code identity: its hello has no `code`. It
    answers hello and nothing else -- a client that leases from it fails."""

    def __init__(self, sock: Path) -> None:
        self.sock = sock
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(str(sock))
        self.srv.listen(4)
        self.leased = False
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self) -> None:
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            with conn:
                buf = b""
                while True:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        req = cast("dict[str, Any]", json.loads(line))
                        if req.get("op") == "lease":
                            self.leased = True
                        resp: dict[str, object] = (
                            {"ok": True, "id": req.get("id"), "version": 2, "instance": "old0",
                             "agents": {}, "root": "/nonexistent"} if req.get("op") == "hello"
                            else {"ok": False, "id": req.get("id"), "error": "unknown-op",
                                  "detail": "old"})
                        conn.sendall(json.dumps(resp).encode() + b"\n")

    def close(self) -> None:
        self.srv.close()


def code_cases(c: Cases, arch: str, work: Path, served: Served) -> None:
    """The service's code identity, as a run asks it once (`axlpool code`)
    and as each lease is held to it (`axlpool run --expect-code`): a lease
    from a service running other code is a pool MISS, so a pooled skip-cache
    key is only ever committed by a test that ran on the code it names."""
    code = [sys.executable, "-m", "axlpool", "code"]
    with PoolClient(served.sock) as pc:
        ident = pc.code
    r = subprocess.run([*code, "--socket", str(served.sock)], cwd=POOL_DIR, capture_output=True,
                       text=True, check=False)
    c.check("code: prints the service's code identity (its hello's), exit 0",
            (r.returncode, r.stdout, bool(re.fullmatch(r"[0-9a-f]{64}", ident))),
            (0, f"{ident}\n", True))
    r = subprocess.run([*code, "--socket", str(work / "none.sock")], cwd=POOL_DIR,
                       capture_output=True, text=True, check=False)
    c.check("code: no service answering exits 1, nothing on stdout", (r.returncode, r.stdout), (1, ""))
    old = OldService(work / "old.sock")
    try:
        r = subprocess.run([*code, "--socket", str(old.sock)], cwd=POOL_DIR, capture_output=True,
                           text=True, check=False)
        c.check("code: a service that reports no identity (older) exits 3, nothing on stdout",
                (r.returncode, r.stdout), (3, ""))
        r = run(old.sock, arch, "true", extra=("--expect-code", ident))
        c.check("run --expect-code against a service with no identity: a MISS (77, marked), "
                "nothing leased",
                (r.returncode, [ln for ln in r.stderr.splitlines() if ln.startswith("AXLPOOL-MISS:")],
                 old.leased),
                (AXLPOOL_EXIT_MISS, ["AXLPOOL-MISS: the pool service runs code (none reported) "
                                     f"other than this run's ({ident[:12]}): it restarted, or is older"],
                 False))
    finally:
        old.close()
    marker = work / "ran-under-code"
    r = run(served.sock, arch, "sh", "-c", f'touch "{marker}"', extra=("--expect-code", "0" * 64))
    c.check("run --expect-code naming OTHER code: a MISS (77, marked), the test never run",
            (r.returncode, [ln for ln in r.stderr.splitlines() if ln.startswith("AXLPOOL-MISS:")],
             marker.exists()),
            (AXLPOOL_EXIT_MISS, [f"AXLPOOL-MISS: the pool service runs code ({ident[:12]}) other "
                                 f"than this run's ({'0' * 12}): it restarted, or is older"], False))
    r = run(served.sock, arch, "sh", "-c", f'touch "{marker}"; exit 5', extra=("--expect-code", ident))
    c.check("run --expect-code naming the service's own code: the test runs, its status stands",
            (r.returncode, marker.exists()), (5, True))


def main() -> int:
    arch, agent, work = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    c = Cases()
    served = Served(work, "a", agent)
    try:
        cases(c, arch, agent, work, served)
    except Exception as e:  # a contract that throws is a failure, reported
        c.check(f"the cases ran without raising ({type(e).__name__}: {e})", False, True)
    finally:
        served.stop()
    print(f"client-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


def cases(c: Cases, arch: str, agent: Path, work: Path, served: Served) -> None:
    c.check("available(): a live service answers; a missing socket does not",
            (available(served.sock), available(work / "none.sock")), (True, False))

    # PoolClient: the protocol behind one call each.
    with PoolClient(served.sock) as pc:
        lease = pc.lease(TREE, "standard", arch, timeout=300)
        try:
            pc.lease(TREE, "nope", arch, timeout=5)
            missed: tuple[str, str] = ("no error", "")
        except PoolError as e:
            missed = (e.code, e.reason)
        result = pc.release(lease)
        guests = pc.guests(TREE)
    c.check("PoolClient: lease, a miss raised as PoolError(miss, its reason), release",
            (isinstance(lease.get("id"), str), missed, result.get("outcome")),
            (True, ("miss", "unknown-class"), "returned"))
    c.check("PoolClient: guests of a tree", len(guests) > 0 and all(g.get("tree") == str(TREE) for g in guests),
            True)

    # `axlpool run`: the child sees the lease as environment, never the token,
    # and its guest's agent answers; its exit status is the wrapper's.
    child = ("import json, os, sys; sys.path.insert(0, " + repr(str(POOL_DIR.parent / "integration" / "lib"))
             + "); from axl_agent import AgentClient; h, p = os.environ['AXL_POOL_AGENT'].rsplit(':', 1); "
             "r = AgentClient(h, int(p)).exec('echo hello'); "
             "print(json.dumps({'lease': os.environ.get('AXL_POOL_LEASE'), "
             "'pool_vars': sorted(k for k in os.environ if k.startswith('AXL_POOL_')), 'out': r.stdout, "
             "'class_args': os.environ.get('AXL_POOL_CLASS_ARGS')})); "
             "sys.exit(3)")
    r = run(served.sock, arch, sys.executable, "-c", child)
    seen = cast("dict[str, Any]", json.loads(r.stdout.strip().splitlines()[-1])) if r.stdout.strip() else {}
    c.check("run: the child's status passes through; it runs on its guest; its pool environment "
            "is exactly Lease.env() plus AXL_POOL_LEASE_MS, AXL_POOL_SOCKET and the class's "
            "options (no token)",
            (r.returncode, str(seen.get("lease", "")).count("-L") == 1, seen.get("pool_vars"),
             str(seen.get("out", "")).strip(), seen.get("class_args")),
            (3, True, RUN_VARS, "hello", STANDARD_ARGS[arch]))
    # The wall-time lines a test's log keeps: the guest, its class/arch, how it
    # started, the waits. The guest id and start kind vary; the rest is exact.
    tl = timing_lines(r.stderr)
    starts = [m.group(1) for ln in tl if (m := re.search(r"\((booted|restored|cold), ", ln))]
    norm = [re.sub(r"\((booted|restored|cold), ", "(START, ", re.sub(r" g[^ ]+ ", " GUEST ", ln)) for ln in tl]
    c.check("run: says leased (guest, class/arch, start kind, pool start, wait) and released "
            "(outcome, wait), one line each",
            (norm, len(starts)),
            (["axlpool run: leased GUEST standard/XN (START, pool start N ms) in N ms",
              "axlpool run: released GUEST (returned) in N ms"], 1))

    code_cases(c, arch, work, served)

    # `axlpool exec`: a bash test's way to run one line through its agent.
    # The line's output is its stdout; its status the exit (77 when the
    # agent could not run or judge it, as for any pool failure).
    ex = [sys.executable, "-m", "axlpool", "exec", "--"]
    r = run(served.sock, arch, *ex, "echo hello")
    # (text mode folds the Shell's CRLF)
    c.check("exec: a line's stdout, exit 0", (r.returncode, r.stdout), (0, "hello\n"))
    r = run(served.sock, arch, sys.executable, "-m", "axlpool", "exec", "--console", "--", "echo hello")
    c.check("exec --console: exit 0, nothing captured (it went to the guest's console)",
            (r.returncode, r.stdout), (0, ""))
    r = run(served.sock, arch, *ex, "ls fs99:")
    c.check("exec: a line that fails exits 1", r.returncode, 1)
    # A line that outlives --timeout is the TEST's hang, not the pool's: 124,
    # as timeout(1) says it, so the harness marks the run timed out.
    r = run(served.sock, arch, sys.executable, "-m", "axlpool", "exec", "--timeout", "2", "--",
            "stall 8000000")
    c.check("exec: a line past --timeout exits 124, saying so",
            (r.returncode, "did not finish within 2" in r.stderr), (124, True))
    r = run(served.sock, arch, *ex, "exit")
    c.check("exec: a line the agent refuses (exit) exits 77, the reason said",
            (r.returncode, "refused" in r.stderr), (AXLPOOL_EXIT_MISS, True))
    r = subprocess.run([*ex, "echo x"], cwd=POOL_DIR, capture_output=True, text=True, check=False,
                       env={k: v for k, v in os.environ.items() if k != "AXL_POOL_AGENT"})
    c.check("exec: no AXL_POOL_AGENT is a usage error (2)",
            (r.returncode, "AXL_POOL_AGENT" in r.stderr), (2, True))

    # `axlpool restore`: the held lease's guest back to clean, from inside the
    # test (run-qemu.sh's pooled mode between runs). A volatile variable set
    # before it is gone after it; the lease is still the test's.
    rs = [sys.executable, "-m", "axlpool", "restore"]
    r = run(served.sock, arch, "sh", "-c",
            f'{" ".join(ex)} "set -v AXLRS 1" && {" ".join(rs)} && {" ".join(ex)} "echo %AXLRS%"')
    said = [re.sub(r"\d+", "N", ln) for ln in r.stdout.splitlines() if ln.startswith("axlpool restore:")]
    c.check("restore: inside run, exit 0, said, the guest back to clean (the variable gone)",
            (r.returncode, said, r.stdout.splitlines()[-1] if r.stdout else ""),
            (0, ["axlpool restore: restored in N ms"], "Echo is on."))
    r = subprocess.run(rs, cwd=POOL_DIR, capture_output=True, text=True, check=False,
                       env={k: v for k, v in os.environ.items() if not k.startswith("AXL_POOL_")})
    c.check("restore: outside run (no AXL_POOL_LEASE) is a usage error (2)",
            (r.returncode, "AXL_POOL_LEASE" in r.stderr), (2, True))
    r = run(served.sock, arch, "sh", "-c", f'AXL_POOL_RESTORE_TOKEN=forged {" ".join(rs)}')
    c.check("restore: a token the service refuses exits 1, its error said",
            (r.returncode, "permission" in r.stderr), (1, True))
    r = run(served.sock, arch, "sh", "-c", f'AXL_POOL_SOCKET={work / "none.sock"} {" ".join(rs)}')
    c.check("restore: no service answering exits 3", r.returncode, 3)

    # A failing child's files are kept (--keep-files-on-fail), and said where.
    r = run(served.sock, arch, "sh", "-c", 'echo evidence > "$AXL_POOL_HOST_DIR/out.log"; exit 1',
            extra=("--keep-files-on-fail",))
    kept = [ln.split("kept: ", 1)[1] for ln in r.stderr.splitlines() if "kept: " in ln]
    c.check("run --keep-files-on-fail: a failing child's files kept, the path printed",
            (r.returncode, len(kept), (Path(kept[0]) / "out.log").read_text() if kept else ""),
            (1, 1, "evidence\n"))
    r = run(served.sock, arch, "sh", "-c", 'echo x > "$AXL_POOL_HOST_DIR/out.log"; exit 0',
            extra=("--keep-files-on-fail",))
    c.check("run --keep-files-on-fail: a passing child's files are not kept",
            (r.returncode, "kept: " in r.stderr), (0, False))

    # Every pool failure is 77 (CTest's SKIP_RETURN_CODE), never the test's FAIL.
    never = work / "never-started"
    r = run(served.sock, arch, "touch", str(never), cls="nope")
    c.check("run: a miss exits 77, the child never started, the reason said",
            (r.returncode, never.exists(), "unknown-class" in r.stderr), (AXLPOOL_EXIT_MISS, False, True))
    r = run(served.sock, arch, str(work / "no-such-test"))
    c.check("run: a test that cannot start exits 127, its guest handed back",
            (r.returncode, "cannot run" in r.stderr, wait_no_leased(served.sock)), (127, True, True))
    r = run(work / "none.sock", arch, "true")
    c.check("run: no service exits 77", r.returncode, AXLPOOL_EXIT_MISS)

    # --agent tree: the lease runs the TREE's own agent build (what a CI run of
    # a commit must exercise), not the service's pinned image.
    r = run(served.sock, arch, "sh", "-c", 'echo "$AXL_POOL_AGENT_IMAGE"', extra=("--agent", "tree"))
    want_img = str(tree_agent(TREE, arch))
    c.check("run --agent tree: the lease's agent image is the tree's own build",
            (r.returncode, r.stdout.strip()), (0, want_img))
    r = run(served.sock, arch, "true", extra=("--agent", "nope"))
    c.check("run --agent: anything but service|tree is a usage error (2)", r.returncode, 2)
    # THE TEST'S TIME IS THE FIRMWARE'S: the agent runs at a 1 ms tick
    # (agent_latency "always", for its own ~1 ms round trips), but a line it
    # runs -- the test's program -- must see the machine a booted guest gives
    # it, the firmware's 10 ms tick: a 1 ms tick costs ~0.15 cores idle and
    # changes network timing (measured: pooled tests' CPU and back-off checks).
    probe = agent.parents[1] / "timer-res-selftest.efi"
    r = run(served.sock, arch, "sh", "-c",
            f'cp {probe} "$AXL_POOL_HOST_DIR/" && '
            f'{sys.executable} -m axlpool exec -- "$AXL_POOL_GUEST_DIR\\timer-res-selftest.efi period"')
    c.check("a line the agent runs sees the firmware's 10 ms tick, not the agent's 1 ms",
            (r.returncode, r.stdout.strip().splitlines()[-1:] if r.stdout.strip() else []),
            (0, ["PERIOD=10000"]))
    r = run(served.sock, arch, sys.executable, "-c",
            "import sys; sys.path.insert(0, " + repr(str(POOL_DIR.parent / "integration" / "lib"))
            + "); import os, time; from axl_agent import AgentClient; h, p = os.environ['AXL_POOL_AGENT'].rsplit(':', 1); "
            "a = AgentClient(h, int(p)); t = time.monotonic(); [a.ping() for _ in range(20)]; "
            "print(round((time.monotonic() - t) / 20 * 1000, 1))")
    c.check("...and between lines the agent is back at its own tick: a /ping round trip under 5 ms",
            (r.returncode, float(r.stdout.strip() or "99") < 5.0), (0, True))
    settle_cases(c, arch, work, served)

    # A signal to the wrapper reaches the child's whole process group (the
    # child sets its trap, then says it is ready); the lease is still released.
    marker, ready = work / "got-term", work / "term-ready"
    p = wrapper(served.sock, arch, "sh", "-c",
                f"trap 'echo term > {marker}; exit 5' TERM; touch {ready}; "
                "while :; do sleep 0.1; done")
    started = wait_file(ready)
    p.send_signal(signal.SIGTERM)
    rc, err = finish(p)
    c.check("run: SIGTERM reaches the child, its status is the wrapper's, the lease released",
            (started, marker.exists(), rc, wait_no_leased(served.sock), "Traceback" in err),
            (True, True, 5, True, False))
    # A child that IGNORES the forwarded TERM is KILLed, its whole group, after
    # the grace (CHILD_TERM_GRACE_S): the wrapper must not wait on it forever
    # -- `timeout N axlpool run` cannot do it, its KILL reaches only the
    # wrapper, whose group the test is not in -- and the lease is released.
    ready = work / "deaf-ready"
    p = wrapper(served.sock, arch, "sh", "-c",
                f"trap '' TERM; touch {ready}; while :; do sleep 0.1; done")
    started = wait_file(ready)
    t0 = time.monotonic()
    p.send_signal(signal.SIGTERM)
    rc, err = finish(p)
    took = time.monotonic() - t0
    c.check("run: a child deaf to TERM is KILLed after the grace (137), the lease released",
            (started, rc, CHILD_TERM_GRACE_S <= took < CHILD_TERM_GRACE_S + 10,
             wait_no_leased(served.sock), "Traceback" in err),
            (True, 128 + signal.SIGKILL, True, True, False))

    # TRACEPARENT: its trace id is the lease's, and the test sees it.
    r = run(served.sock, arch, "sh", "-c", 'echo "$AXL_POOL_TRACE_ID"',
            env={**os.environ, "TRACEPARENT": f"00-{TRACE}-b7ad6b7169203331-01"})
    c.check("run: TRACEPARENT's trace id reaches the test as AXL_POOL_TRACE_ID",
            (r.returncode, r.stdout.strip()), (0, TRACE))

    other = Served(work, "b", agent)          # fresh: its first lease BOOTS (10-22 s)
    try:
        # A signal while the wrapper still waits for its guest: no child, so
        # it ends at once, 128+N, no traceback; the pending lease is dropped.
        for sig, want in ((signal.SIGTERM, 128 + signal.SIGTERM), (signal.SIGINT, 128 + signal.SIGINT)):
            ran = work / f"ran-{sig.name}"
            p = wrapper(other.sock, arch, "touch", str(ran))
            # Its handlers are in (so it is past startup, in or at its wait),
            # and a guest boots for it (it cannot be served before).
            booting = handlers_installed(p.pid) and wait_status(other.sock, "booting")
            time.sleep(0.5)
            t_sig = time.monotonic()
            p.send_signal(sig)
            rc, err = finish(p)
            c.check(f"run: {sig.name} while waiting for a guest ends the wrapper at once, "
                    f"{want}, no traceback, the test never ran",
                    (booting, rc, time.monotonic() - t_sig < 5, "Traceback" in err, ran.exists()),
                    (True, want, True, False, False))

        # The SERVICE dying mid-run is a pool failure: 77, not the child's
        # status -- and the test's whole process group is ended at once (its
        # own child here sleeps 30 s; a release failing after it would ALSO
        # read 77, hence the clock).
        pidf, ready = work / "grandchild.pid", work / "gone-ready"
        p = wrapper(other.sock, arch, "sh", "-c",
                    f"sleep 30 & echo $! > {pidf}; touch {ready}; wait")
        started = wait_file(ready, 120)
        other.proc.kill()
        other.proc.wait()
        t_gone = time.monotonic()
        rc, _ = finish(p)
        took = time.monotonic() - t_gone
        grandchild = int(pidf.read_text()) if pidf.exists() else -1
        time.sleep(0.5)
        c.check("run: the service gone mid-run ends the test's whole process group at once, exits 77",
                (started, rc, took < 15, grandchild > 0 and not is_sleep(grandchild)),
                (True, AXLPOOL_EXIT_MISS, True, True))
    finally:
        other.stop()
        sweeper = Served(work, "b", agent)    # its next start sweeps the guests it left
        sweeper.stop()


def is_sleep(pid: int) -> bool:
    """@pid is alive AND still a `sleep` (a reused pid is not it)."""
    try:
        return Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")[0].endswith(b"sleep")
    except OSError:
        return False


def wrapper(sock: Path, arch: str, *args: str) -> subprocess.Popen[str]:
    """`axlpool run` in the background, @args its child."""
    return subprocess.Popen(
        [sys.executable, "-m", "axlpool", "run", "--socket", str(sock), "--tree", str(TREE),
         "--class", "standard", "--arch", arch, "--", *args],
        cwd=POOL_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def finish(p: subprocess.Popen[str]) -> tuple[int, str]:
    """@p's exit status and stderr; killed after 60 s."""
    try:
        _, err = p.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        p.kill()
        _, err = p.communicate()
    return p.returncode, err


def wait_file(path: Path, limit_s: float = 60) -> bool:
    end = time.monotonic() + limit_s
    while time.monotonic() < end:
        if path.exists():
            return True
        time.sleep(0.05)
    return False


def handlers_installed(pid: int, limit_s: float = 30) -> bool:
    """Whether @pid catches SIGTERM and SIGHUP -- run()'s handlers (Python
    catches neither by default), read from /proc/<pid>/status SigCgt."""
    want = (1 << (signal.SIGTERM - 1)) | (1 << (signal.SIGHUP - 1))
    end = time.monotonic() + limit_s
    while time.monotonic() < end:
        try:
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("SigCgt:") and int(line.split()[1], 16) & want == want:
                    return True
        except OSError:
            return False
        time.sleep(0.02)
    return False


def wait_status(sock: Path, status: str, limit_s: float = 60) -> bool:
    end = time.monotonic() + limit_s
    while time.monotonic() < end:
        try:
            with PoolClient(sock) as pc:
                if any(g.get("status") == status for g in pc.guests()):
                    return True
        except (OSError, PoolError):
            pass
        time.sleep(0.1)
    return False


def leased_count(sock: Path) -> int:
    try:
        with PoolClient(sock) as pc:
            return sum(1 for g in pc.guests() if g.get("status") == "leased")
    except (OSError, PoolError):
        return 0


def wait_no_leased(sock: Path) -> bool:
    for _ in range(100):
        if leased_count(sock) == 0:
            return True
        time.sleep(0.2)
    return False


SETTLE_RE = re.compile(r"^axlpool settle: settled after \d+ ms$")


def settle(sock: Path, timeout: float) -> subprocess.CompletedProcess[str]:
    """`axlpool settle` on @sock."""
    return subprocess.run(
        [sys.executable, "-m", "axlpool", "settle", "--socket", str(sock), "--timeout", str(timeout)],
        cwd=POOL_DIR, capture_output=True, text=True, timeout=timeout + 30, check=False)


def unsettled(sock: Path) -> list[str]:
    with PoolClient(sock) as pc:
        return sorted(str(g.get("status")) for g in pc.guests() if g.get("status") in ("booting", "resetting"))


def settle_cases(c: Cases, arch: str, work: Path, served: Served) -> None:
    """`axlpool settle`: waits until no guest of the pool is booting or
    resetting (the quiet slot asks before it leases), bounded."""
    # The control: a guest that is BOOTING keeps the pool unsettled -- a
    # first `unit` lease boots one, for seconds.
    got: list[dict[str, Any]] = []

    def lease_unit() -> None:
        with PoolClient(served.sock) as pc:
            lease = pc.lease(TREE, "unit", arch, timeout=300)
            got.append(lease)
            while not stop.is_set():
                time.sleep(0.1)
            pc.release(lease)

    stop = threading.Event()
    t = threading.Thread(target=lease_unit)
    t.start()
    try:
        booting = wait_status(served.sock, "booting")
        r = settle(served.sock, 0.5)
        c.check("settle: a booting guest is waited for, bounded -- exit 1, what is left said",
                (booting, r.returncode, r.stdout.strip().startswith("axlpool settle: not settled after 0.5 s:"),
                 "booting" in r.stdout),
                (True, 1, True, True))
    finally:
        stop.set()
        t.join(300)
    # Released just now: its reset runs in the background, and settle waits it out.
    r = settle(served.sock, 120)
    c.check("settle: waits out the released guest's reset -- exit 0, settled said, nothing left unsettled",
            (len(got), r.returncode, bool(SETTLE_RE.match(r.stdout.strip())), unsettled(served.sock)),
            (1, 0, True, []))
    r = settle(work / "none.sock", 5)
    c.check("settle: no service exits 2, said", (r.returncode, "no pool service" in r.stderr), (2, True))


if __name__ == "__main__":
    sys.exit(main())
