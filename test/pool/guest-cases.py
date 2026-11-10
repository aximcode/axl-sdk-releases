#!/usr/bin/env python3
"""guest-cases.py -- axlpool.guest's lifecycle, against real guests.

Usage: guest-cases.py ARCH AGENT_EFI WORKDIR

One guest through the whole life a pooled guest has: boot and snapshot,
dirtied (a UEFI variable, a file on the boot disk) and restored, stopped
with its state kept, cold-started in a NEW QEMU from that state, killed.
Prints `  PASS:`/`  FAIL:` per case, then `guest-cases: <p> passed, <f> failed`.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration" / "lib"))
from axlpool.guest import Guest, pid_alive  # noqa: E402
from axlpool.klass import GuestSpec  # noqa: E402
from checks import Cases, ppid  # noqa: E402

# `type` ends what it prints with a line end of its own (measured): a file
# holding "P1\r\n" reads "P1\r\n\r\n".
GUID = "6a1d3c4e-9b2f-4c7a-8e51-2d930f6ba419"
MISSING_VAR = "0x800000000000000F"   # setvar's status for a variable not there


NO_EVENTS: list[str] = []

# The hung boot's deadline, and what boot may add to it: launching QEMU
# before the wait starts, and killing it after (measured ~2 s together).
HUNG_TIMEOUT_S = 15.0
HUNG_MARGIN_S = 6.0


def census(work: Path) -> list[str]:
    """Every QEMU or virtiofsd naming @work in its command line -- by PATH,
    not by the pids a Guest recorded, so a wrong pid cannot hide a leak."""
    left: list[str] = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        exe = args.split(" ", 1)[0]
        if ("qemu-system" in exe or "virtiofsd" in exe) and str(work) in args:
            left.append(f"{proc.name} {exe}")
    return left


NONE_LEFT: tuple[list[int], list[str]] = ([], [])


def var_status(g: Guest) -> str:
    return g.agent.exec(f"setvar AXLPOOLG -guid {GUID}").status


def main() -> int:
    arch, agent, work = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    c = Cases()
    share = work / "share"
    share.mkdir(parents=True)
    (share / "p.txt").write_text("P1\r\n")
    state = work / "state"
    spec = GuestSpec(arch, agent)
    g: Guest | None = None
    g2: Guest | None = None
    try:
        g = Guest.boot(spec, state, share)
        info = g.agent.info()
        c.check("boot: the agent answers in a -nonest Shell of the right arch",
                (info.get("arch"), info.get("nonest")), (arch, True))
        # A boot hands its guest over to a cold start (so it is the caller's
        # child): the boot's console is serial-0.log, the guest's own now 1.
        ready = [ln for ln in (state / "serial-0.log").read_text(errors="replace").replace("\r", "")
                 .splitlines() if ln.startswith("AXL-AGENT-READY")]
        c.check("boot: its console captured (serial-0.log); the handed-over guest's is serial-1.log",
                (g.serial_log.name, ready), ("serial-1.log", ["AXL-AGENT-READY port=4747 nonest=1"]))
        c.check("boot: the returned guest is the caller's child, not run-qemu.sh's `timeout`'s",
                ppid(g.qemu_pid), os.getpid())
        c.check("boot: the share is found as a volume",
                g.agent.exec(f"type {g.share_volume}:\\p.txt").stdout, "P1\r\n\r\n")

        r = g.agent.exec(f"setvar AXLPOOLG -guid {GUID} -nv -bs =01")
        g.agent.exec("echo dirty > dirty.txt")
        c.check("dirtied: the variable is set and the file written",
                (r.outcome, r.status, var_status(g), g.agent.exec("type dirty.txt").stdout),
                ("ran", "0x0", "0x0", "dirty\r\n\r\n"))
        t = time.monotonic()
        g.restore()
        restore_s = time.monotonic() - t
        c.check("restore: the agent answers within 2 s", restore_s < 2.0, True)
        c.check("restore: the variable is gone", var_status(g), MISSING_VAR)
        gone = g.agent.exec("type dirty.txt")
        c.check("restore: the file written on the boot disk is gone",
                (gone.outcome, gone.status, gone.stdout),
                ("ran", "0x800000000000000E", "type: Cannot open file - 'dirty.txt'\r\n"))

        # `axlpool qmp`: one QMP command from a shell (the deadline-guarded
        # session the pool uses; what bash tests use instead of socat).
        def qmpcli(*args: str) -> tuple[int, str, str]:
            r = subprocess.run([sys.executable, "-m", "axlpool", "qmp", *args],
                               cwd=Path(__file__).resolve().parent, capture_output=True, text=True,
                               timeout=60, check=False)
            return r.returncode, r.stdout, r.stderr
        rc, out, _ = qmpcli(str(state / "qmp.sock"), "query-status")
        c.check("qmp CLI: a command's return, as JSON, exit 0",
                (rc, json.loads(out).get("status") if rc == 0 else out), (0, "running"))
        rc, out, _ = qmpcli(str(state / "qmp.sock"), "human-monitor-command",
                            '{"command-line": "info name"}')
        c.check("qmp CLI: arguments as a JSON object", (rc, out.startswith('"')), (0, True))
        rc, _, err = qmpcli(str(state / "qmp.sock"), "no-such-command")
        c.check("qmp CLI: a command QEMU refuses: exit 1, its error class said",
                (rc, "CommandNotFound" in err), (1, True))
        rc, _, err = qmpcli("--timeout", "2", str(state / "nope.sock"), "query-status")
        c.check("qmp CLI: no QEMU to talk to: exit 3", (rc, "axlpool qmp:" in err), (3, True))

        # QMP EVENTS, on the guest's own events monitor (the test socket stays
        # free): a reset someone else caused is the guest's event; the
        # pool's own restore and system_reset are not.
        c.check("events: none since the restore", g.guest_events(), NO_EVENTS)
        qmpcli(str(state / "qmp.sock"), "system_reset")
        seen = NO_EVENTS
        for _ in range(50):
            seen = g.guest_events()
            if seen:
                break
            time.sleep(0.1)
        c.check("events: a reset from outside the pool is recorded (RESET)", seen, ["RESET"])
        g.restore()
        c.check("events: a restore starts the record over", g.guest_events(), NO_EVENTS)
        g.system_reset()
        time.sleep(1)
        c.check("events: the pool's OWN system_reset is not the guest's", g.guest_events(), NO_EVENTS)
        g.restore()

        # A cold start over a live guest is refused, and harms nothing.
        try:
            Guest.cold_start(state, share)
            refused = "not refused"
        except RuntimeError as e:
            refused = str(e)
        c.check("a cold start while the guest's QEMU is live is refused, the guest unharmed",
                (refused, g.agent.ping()),
                (f"a QEMU still serves {state / 'qmp.sock'}: stop it first", "pong\n"))

        # The share across many restores: virtio-fs state rolls back with
        # the guest while virtiofsd's does not, so open/close it each cycle.
        reads: list[str | None] = []
        for i in range(20):
            (share / "cycle.txt").write_text(f"C{i}\r\n")
            reads.append(g.agent.exec(f"type {g.share_volume}:\\cycle.txt").stdout)
            g.restore()
        c.check("the share reads the host's file across 20 restore cycles",
                reads, [f"C{i}\r\n\r\n" for i in range(20)])

        old_pids = g.pids()
        g.stop()
        c.check("stop: the guest's QEMU and virtiofsd are gone (recorded pids, and by path)",
                ([p for p in old_pids if pid_alive(p)], census(work)), NONE_LEFT)
        c.check("stop: the state is kept",
                sorted(p.name for p in state.glob("*.qcow2")),
                ["disk.qcow2", "vars.qcow2", "vmstate.qcow2"])

        (share / "p.txt").write_text("P2\r\n")
        t = time.monotonic()
        g2 = Guest.cold_start(state, share)
        cold_s = time.monotonic() - t
        print(f"  (boot {g.boot_s:.1f} s, restore {restore_s * 1000:.0f} ms, "
              f"cold start {cold_s:.2f} s)")
        c.check("cold start: a new QEMU answers within 5 s", cold_s < 5.0, True)
        c.check("cold start: its console goes to a new serial log (QEMU truncates the one it opens)",
                (g2.serial_log.name, g2.serial_log.exists(), g.serial_log.exists()),
                ("serial-2.log", True, True))
        c.check("cold start: from the clean snapshot (the variable is gone)",
                var_status(g2), MISSING_VAR)
        c.check("cold start: the share reads the host's current file",
                g2.agent.exec(f"type {g2.share_volume}:\\p.txt").stdout, "P2\r\n\r\n")
        c.check("cold start: a restore still works in the new process",
                (g2.restore() is None, g2.agent.ping()), (True, "pong\n"))

        new_pids = g2.pids()
        g2.kill()
        c.check("kill: nothing of the guest is left running (recorded pids, and by path)",
                ([p for p in new_pids if pid_alive(p)], census(work)), NONE_LEFT)
        g2 = None
    except Exception as e:  # a lifecycle that throws is a failure, reported
        c.check(f"the lifecycle ran without raising ({type(e).__name__}: {e})", False, True)

    finally:
        for x in (g, g2):
            if x is not None:
                x.kill()
    # A boot whose QEMU fails leaves nothing running, and fails at once
    # rather than waiting out the agent timeout.
    bad = work / "bad"
    t = time.monotonic()
    try:
        Guest.boot(GuestSpec(arch, agent, ("--qemu-arg", "-device", "--qemu-arg", "no-such-device")),
                   bad / "state", bad / "share")
        failed = "booted"
    except (RuntimeError, TimeoutError) as e:
        failed = type(e).__name__
    c.check("a boot whose QEMU fails raises within 30 s and leaves nothing running",
            (failed, time.monotonic() - t < 30, census(bad)), ("RuntimeError", True, NONE_LEFT[1]))

    # A boot that fails while QEMU RUNS -- an "agent" the Shell cannot load,
    # so nothing ever answers -- must be ended by boot itself: run-qemu.sh's
    # own cleanup only follows a QEMU that exited. And within ITS deadline:
    # QEMU's port forward accepts the connection, so a /ping that waits on
    # the socket's own timeout (and retries) outlives the caller's deadline.
    hung = work / "hung"
    hung.mkdir()
    corrupt = hung / "corrupt.efi"
    corrupt.write_bytes(b"MZ" + bytes(4094))
    t = time.monotonic()
    try:
        Guest.boot(GuestSpec(arch, corrupt), hung / "state", hung / "share",
                   timeout=HUNG_TIMEOUT_S)
        failed = "booted"
    except (RuntimeError, TimeoutError) as e:
        failed = type(e).__name__
    took = time.monotonic() - t
    c.check(f"a boot whose agent never answers times out within {HUNG_TIMEOUT_S:.0f} s "
            f"+ {HUNG_MARGIN_S:.0f} s and leaves nothing running",
            (failed, took < HUNG_TIMEOUT_S + HUNG_MARGIN_S, census(hung)),
            ("TimeoutError", True, NONE_LEFT[1]))
    if took >= HUNG_TIMEOUT_S + HUNG_MARGIN_S:
        print(f"    (it took {took:.1f} s)")

    print(f"guest-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
