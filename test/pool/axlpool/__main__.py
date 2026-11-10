"""python3 -m axlpool -- the pool's command line (design doc §6.3 step 4).

  serve   run the service (the systemd unit's ExecStart)
  run     run a test on a leased guest (service.py's THE TEST-SIDE WRAPPER)
  units   write the systemd user units that run the service (units.py)
  resource-spec   print the CTest resource spec of a tree's classes
  exec    run one Shell line through a leased guest's agent (inside `run`)
  status  whether a service answers, and which arches it has agents for
  code    the code identity the service runs (a run asks it once; pooled keys hold it)
  settle  wait until no guest is booting or resetting (the quiet slot's ask)
  qmp     one QMP command to a QEMU's QMP socket (what bash tests use)
  restore the held lease's guest back to its clean snapshot (inside `run`)

Exit status, serve: 0 on a clean stop (SIGTERM, SIGINT, the idle exit); 2
on a usage error; 3 (EXIT_CANNOT_START, which the unit does not restart on)
when the service could not start (another owns its socket or state
directory, a live socket at its path, an unsafe socket path, the classes do
not load).
run: the test's status; 77 (AXLPOOL_EXIT_MISS) for any pool failure; 2 on a
usage error.
units: 0 written; 1 when they cannot be (not a tree, an unquotable path). resource-spec: 0 printed; 1 when it cannot be.
status: 0 when a service answers (its instance and agents printed), 1 not.
code: 0 with the code identity the service runs printed alone (a digest of
the pool code it loaded at start); 1 when no service answers; 3 when it
answers but reports none (it predates the identity).
run --expect-code ID: a service whose identity is not ID is a pool failure
(77, marked), checked before anything is leased.
settle: 0 when no guest of the pool is booting or resetting (how long it
waited printed); 1 when some still were after --timeout (they are named);
2 when no service answers.
qmp: 0 with the command's return printed as JSON; 1 when QEMU answered with
an error (its class and description said); 3 when there was no QEMU to talk
to (no socket, no answer within --timeout).
restore: 0 when the guest is back at its clean snapshot (`axlpool restore:
restored in <ms> ms`); 1 when the service refused it (its error code said --
a service older than the op answers unknown-op); 3 when no service answered;
2 on a usage error (no AXL_POOL_LEASE or AXL_POOL_RESTORE_TOKEN: not inside
`run`).
exec: 0 when the line ran with status 0; 1 when it ran and failed; 124 when
it did not finish within --timeout; 77 when the agent could not run or judge
it; 2 on a usage error.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import cast

# The agent client lives with the integration tests (test/integration/lib).
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "integration" / "lib"))
from axl_agent import AgentClient  # noqa: E402
from qemu.qmp import ExecuteError  # noqa: E402

from axlpool.classes import ARCHES, CLASSES_FILE, ClassError, load  # noqa: E402
from axlpool.guest import Qmp  # noqa: E402
from axlpool.client import DEFAULT_LEASE_TIMEOUT_S, PoolClient, PoolError, find_tree, run  # noqa: E402
from axlpool.resource_spec import SpecError, spec  # noqa: E402
from axlpool.service import (AXLPOOL_EXIT_MISS, EXIT_CANNOT_START, Service,  # noqa: E402
                             default_socket, default_state, mem_cap_mb, refuse_activated,
                             tree_agent)
from axlpool.units import UnitError, render, write  # noqa: E402

# AxlTestNet's AXL_TEST_UDP_ECHO_PORT (test/unit/axl-test-net.c): the guests
# send to 10.0.2.2:<it>, the host's loopback.
UNIT_UDP_ECHO_PORT = 35555


def _nonneg_int(text: str) -> int:
    """A count of at least 0: a negative cap would refuse every guest."""
    try:
        n = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a number") from None
    if n < 0:
        raise argparse.ArgumentTypeError(f"{n} is negative")
    return n


def _port(text: str) -> int:
    """A UDP port, or 0 (none): an out-of-range one would make bind raise
    OverflowError -- a traceback, not the refused start (exit 3) it must be."""
    try:
        n = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a port number") from None
    if not 0 <= n <= 65535:
        raise argparse.ArgumentTypeError(f"{n} is not a port (0..65535)")
    return n

EXEC_TIMEOUT_S = 120.0
EXIT_TIMED_OUT = 124            # as timeout(1): the line's own hang, not a pool failure
EXIT_NO_QMP = 3                 # qmp: no QEMU to talk to
EXIT_NO_SERVICE = 2             # settle: nothing answered
EXIT_RESTORE_NO_SERVICE = 3     # restore: nothing answered (refused is 1)
EXIT_NO_CODE = 3                # code: the service answered, but reports no code identity
# What a guest does while it is neither serving a lease nor idle: the work a
# timing-sensitive test must not share the box with (settle).
UNSETTLED = ("booting", "resetting")
SETTLE_POLL_S = 0.2


def _pinned_agents(tree: Path, given: list[str]) -> dict[str, Path]:
    """Each arch's pinned agent: from --agent ([ARCH=]PATH; an ARCH= one
    wins over a bare PATH whatever their order), else @tree's own build.
    RuntimeError when no arch has one."""
    agents: dict[str, Path] = {}
    explicit: dict[str, Path] = {}
    for g in given:
        arch, sep, path = g.partition("=")
        if sep and arch.upper() in ARCHES:
            explicit[arch.upper()] = Path(path).resolve()
        else:
            agents.update({a: Path(g).resolve() for a in ARCHES})
    agents.update(explicit)
    if not given:
        for a in ARCHES:
            try:
                agents[a] = tree_agent(tree, a)
            except ValueError:
                pass                  # that arch is not built: its leases miss no-agent
    if not agents:
        raise RuntimeError(f"no agent built in {tree} for any arch "
                           f"(scripts/axl-build.sh ARCH=x64 agent, and ARCH=aa64)")
    return agents


def _serve(ns: argparse.Namespace) -> int:
    try:
        tree = Path(ns.tree).resolve()
        svc = Service(tree, _pinned_agents(tree, ns.agent), Path(ns.state).resolve(),
                      Path(ns.socket).resolve(), exit_idle=ns.exit_idle,
                      udp_echo_port=ns.udp_echo_port or None,
                      mem_cap_mb=mem_cap_mb(ns.mem_cap_mb, Path("/proc/meminfo").read_text()))
    except Exception as e:  # noqa: BLE001 -- ANY refused start is 3: the unit must not loop on it
        print(f"axlpool serve: {e}", file=sys.stderr)
        refuse_activated(f"the pool service cannot start: {e}")
        return EXIT_CANNOT_START
    svc.run()
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="axlpool")
    sub = ap.add_subparsers(dest="command", required=True)
    sv = sub.add_parser("serve", help="run the service")
    sv.add_argument("--tree", required=True,
                    help="the service's own tree: its classes are the default, its agent pinned")
    sv.add_argument("--agent", action="append", default=[], metavar="[ARCH=]PATH",
                    help="pin an agent image (for every arch, or ARCH's; repeatable). "
                         "Default: each arch's build in --tree")
    sv.add_argument("--state", default=str(default_state()), help="state directory")
    sv.add_argument("--socket", default=str(default_socket()), help="the socket to bind")
    sv.add_argument("--udp-echo-port", type=_port, default=UNIT_UDP_ECHO_PORT,
                    help="serve the unit suite's UDP echo on 127.0.0.1:PORT for the "
                         f"service's life (default {UNIT_UDP_ECHO_PORT}, AxlTestNet's "
                         "AXL_TEST_UDP_ECHO_PORT; 0: none)")
    sv.add_argument("--mem-cap-mb", type=_nonneg_int, default=None, metavar="MIB",
                    help="the guest RAM the pool may keep: idle guests are stopped, least "
                         "recently used first, to start one that would pass it (default: half "
                         "of MemTotal; 0: no cap)")
    sv.add_argument("--exit-idle", type=float, default=900.0,
                    help="exit after this many seconds with no client and no running guest; 0 never")
    rn = sub.add_parser("run", help="run a test on a leased guest")
    rn.add_argument("--socket", default=str(default_socket()), help="the service's socket")
    rn.add_argument("--tree", help="the tree whose class to lease (default: the one around the cwd)")
    rn.add_argument("--class", dest="class_name", required=True)
    rn.add_argument("--arch", required=True, choices=("X64", "AARCH64"))
    rn.add_argument("--timeout", type=float, default=DEFAULT_LEASE_TIMEOUT_S,
                    help="seconds to wait for a guest; must fit inside the test's own limit")
    rn.add_argument("--agent", choices=("service", "tree"), default="service",
                    help="the agent image the guest runs: the service's (default) or the "
                         "tree's own build -- what a CI run of that tree must exercise")
    rn.add_argument("--expect-code", default=None, metavar="ID",
                    help="the service code identity this run found (`axlpool code`): a service "
                         "running other code is a pool miss (77), nothing leased")
    rn.add_argument("--keep-files-on-fail", action="store_true",
                    help="keep a failing test's share files (their path is printed)")
    rn.add_argument("cmd", nargs=argparse.REMAINDER, help="-- the test and its arguments")
    un = sub.add_parser("units", help="write the systemd user units")
    un.add_argument("--tree", required=True, help="the service's own tree (a stable checkout)")
    un.add_argument("--out", required=True, help="the directory to write them into")
    rs = sub.add_parser("resource-spec", help="print the CTest resource spec of a tree's classes")
    rs.add_argument("--tree", help="the tree (default: the one around the current directory)")
    ex = sub.add_parser("exec", help="run one Shell line on the leased guest (AXL_POOL_AGENT)")
    ex.add_argument("--timeout", type=float, default=EXEC_TIMEOUT_S, help="seconds for the line")
    ex.add_argument("--console", action="store_true",
                    help="the line writes to the guest's console (its serial log), not captured: "
                         "kept even if it crashes or hangs")
    ex.add_argument("line", nargs=argparse.REMAINDER, help="-- the Shell line")
    cd = sub.add_parser("code", help="print the code identity the service runs")
    cd.add_argument("--socket", default=str(default_socket()), help="the service's socket")
    st = sub.add_parser("status", help="does a service answer on the socket?")
    st.add_argument("--socket", default=str(default_socket()), help="the service's socket")
    se = sub.add_parser("settle", help="wait until no guest is booting or resetting")
    se.add_argument("--socket", default=str(default_socket()), help="the service's socket")
    se.add_argument("--timeout", type=float, default=60.0, help="seconds to wait at most")
    ro = sub.add_parser("restore", help="the held lease's guest back to its clean snapshot "
                                        "(AXL_POOL_LEASE, AXL_POOL_RESTORE_TOKEN)")
    ro.add_argument("--socket", default=None,
                    help="the service's socket (default: AXL_POOL_SOCKET, else the user's)")
    ro.add_argument("--timeout", type=float, default=60.0, help="seconds for the restore")
    qm = sub.add_parser("qmp", help="one QMP command to a QEMU's QMP socket")
    qm.add_argument("--timeout", type=float, default=10.0, help="seconds for connect and command")
    qm.add_argument("socket", help="the QMP socket (-qmp unix:PATH,server=on,wait=off)")
    qm.add_argument("qmp_command", metavar="COMMAND", help="e.g. query-status, set_link")
    qm.add_argument("arguments", nargs="?", default=None, help="its arguments, a JSON object")
    ns = ap.parse_args(argv)
    if ns.command == "qmp":
        return _qmp(ap, ns)
    if ns.command == "code":
        return _code(Path(ns.socket))
    if ns.command == "status":
        return _status(Path(ns.socket))
    if ns.command == "settle":
        return _settle(Path(ns.socket), ns.timeout)
    if ns.command == "exec":
        return _exec(ap, ns)
    if ns.command == "restore":
        return _restore(ap, ns)
    if ns.command == "serve":
        return _serve(ns)
    if ns.command == "units":
        return _units(ns)
    if ns.command == "resource-spec":
        return _resource_spec(ap, ns)
    return _run(ap, ns)


def _qmp(ap: argparse.ArgumentParser, ns: argparse.Namespace) -> int:
    try:
        args = json.loads(ns.arguments) if ns.arguments is not None else None
    except ValueError as e:
        ap.error(f"qmp: the arguments are not JSON: {e}")
    if args is not None and not isinstance(args, dict):
        ap.error("qmp: the arguments must be a JSON object")
    try:
        q = Qmp(Path(ns.socket), timeout=ns.timeout)
    except Exception as e:  # noqa: BLE001 -- no QEMU to talk to, whatever the reason
        print(f"axlpool qmp: cannot talk to {ns.socket}: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_NO_QMP
    with q:
        try:
            ret = q.cmd(ns.qmp_command, cast("dict[str, object] | None", args))
        except ExecuteError as e:
            print(f"axlpool qmp: {ns.qmp_command}: {e.error_class}: {e}", file=sys.stderr)
            return 1
        except Exception as e:  # noqa: BLE001 -- the session failed: not QEMU's answer
            print(f"axlpool qmp: {ns.qmp_command}: no answer: {type(e).__name__}: {e}", file=sys.stderr)
            return EXIT_NO_QMP
    print(json.dumps(ret))
    return 0


def _status(sock: Path) -> int:
    try:
        with PoolClient(sock, timeout=5.0) as pc:
            print(f"axlpool: service {pc.instance} on {sock}, agents: {' '.join(sorted(pc.agents)) or 'none'}")
            return 0
    except (OSError, PoolError, ValueError) as e:
        print(f"axlpool: no pool service on {sock}: {e}", file=sys.stderr)
        return 1


def _code(sock: Path) -> int:
    """The service's code identity alone on stdout -- what a run asks ONCE
    (cmake/AxlIntegrationTests.cmake.in) and keys its pooled tests on."""
    try:
        with PoolClient(sock, timeout=5.0) as pc:
            if not pc.code:
                print(f"axlpool code: the service on {sock} reports no code identity "
                      "(it predates it: restart it)", file=sys.stderr)
                return EXIT_NO_CODE
            print(pc.code)
            return 0
    except (OSError, PoolError, ValueError) as e:
        print(f"axlpool code: no pool service on {sock}: {e}", file=sys.stderr)
        return 1


def _settle(sock: Path, timeout: float) -> int:
    """Until no guest of the pool, of any tree, is booting or resetting -- a
    released guest's reset runs in the background, and a verify run's last
    releases are still restoring when its quiet slot starts. Bounded: past
    @timeout the caller runs anyway, told what was still busy."""
    t0 = time.monotonic()
    try:
        with PoolClient(sock, timeout=5.0) as pc:
            while True:
                busy = sorted(f"{g.get('guest')} {g.get('status')}" for g in pc.guests()
                              if g.get("status") in UNSETTLED)
                waited = time.monotonic() - t0
                if not busy:
                    print(f"axlpool settle: settled after {int(waited * 1000)} ms")
                    return 0
                if waited >= timeout:
                    print(f"axlpool settle: not settled after {timeout:g} s: {', '.join(busy)}")
                    return 1
                time.sleep(min(SETTLE_POLL_S, max(0.0, timeout - waited)))
    except (OSError, PoolError, ValueError) as e:
        print(f"axlpool settle: no pool service on {sock}: {e}", file=sys.stderr)
        return EXIT_NO_SERVICE


def _exec(ap: argparse.ArgumentParser, ns: argparse.Namespace) -> int:
    words = list(ns.line)
    if words and words[0] == "--":
        words = words[1:]
    if not words:
        ap.error("exec: no line (axlpool exec -- <shell line>)")
    addr = os.environ.get("AXL_POOL_AGENT", "")
    host, _, port = addr.rpartition(":")
    if not host or not port.isdigit():
        ap.error("exec: no AXL_POOL_AGENT (host:port) -- run it inside `axlpool run`")
    try:
        r = AgentClient(host, int(port), timeout=ns.timeout).exec(" ".join(words), console=ns.console)
    except TimeoutError:
        # The line's own hang -- or an agent that died mid-line, which reads
        # the same from here; the lease's release then finds the guest
        # unhealthy and replaces it, and says so in the pool's log.
        print(f"axlpool exec: the line did not finish within {ns.timeout:g} s", file=sys.stderr)
        return EXIT_TIMED_OUT
    except Exception as e:  # noqa: BLE001 -- any other failure to get an answer is the pool's
        print(f"axlpool exec: no answer from the agent: {type(e).__name__}: {e}", file=sys.stderr)
        return AXLPOOL_EXIT_MISS
    if r.http_status != 200:
        print(f"axlpool exec: {r.error.strip()} (HTTP {r.http_status})", file=sys.stderr)
        return AXLPOOL_EXIT_MISS
    if ns.console and r.stdout_state != "console":
        # An agent older than protocol 2 ignores the request and captures:
        # the output would go nowhere a test reads. Never a quiet fallback.
        print(f"axlpool exec: the agent captured the line (stdout {r.stdout_state}) instead of "
              f"writing it to the console: it predates protocol 2 -- rebuild the agent",
              file=sys.stderr)
        return AXLPOOL_EXIT_MISS
    sys.stdout.write(r.stdout or "")
    sys.stderr.write(r.stderr or "")
    if r.outcome != "ran" or r.shell_exiting:
        print(f"axlpool exec: outcome {r.outcome}{', the Shell is exiting' if r.shell_exiting else ''}",
              file=sys.stderr)
        return AXLPOOL_EXIT_MISS
    return 0 if int(r.status, 16) == 0 else 1


def _restore(ap: argparse.ArgumentParser, ns: argparse.Namespace) -> int:
    lease = os.environ.get("AXL_POOL_LEASE", "")
    token = os.environ.get("AXL_POOL_RESTORE_TOKEN", "")
    if not lease or not token:
        ap.error("restore: no AXL_POOL_LEASE / AXL_POOL_RESTORE_TOKEN -- run it inside `axlpool run`")
    sock = Path(ns.socket or os.environ.get("AXL_POOL_SOCKET") or default_socket())
    try:
        pc = PoolClient(sock, timeout=ns.timeout)
    except (OSError, PoolError, ValueError) as e:
        print(f"axlpool restore: no pool service on {sock}: {e}", file=sys.stderr)
        return EXIT_RESTORE_NO_SERVICE
    with pc:
        try:
            took = pc.restore(lease, token, wait=ns.timeout)
        except PoolError as e:
            print(f"axlpool restore: {e}", file=sys.stderr)
            return EXIT_RESTORE_NO_SERVICE if e.code == "eof" else 1
        except (OSError, ValueError) as e:
            print(f"axlpool restore: no answer from {sock}: {type(e).__name__}: {e}", file=sys.stderr)
            return EXIT_RESTORE_NO_SERVICE
    print(f"axlpool restore: restored in {took} ms")
    return 0


def _resource_spec(ap: argparse.ArgumentParser, ns: argparse.Namespace) -> int:
    tree = Path(ns.tree).resolve() if ns.tree else find_tree(Path.cwd())
    if tree is None:
        ap.error("resource-spec: no --tree, and no test/pool/classes.toml above the current directory")
    path = tree / CLASSES_FILE
    if not path.is_file():
        print(f"axlpool resource-spec: {tree} has no test/pool/classes.toml", file=sys.stderr)
        return 1
    try:
        print(json.dumps(spec(load(path)), indent=2, sort_keys=True))
    except (ClassError, SpecError, OSError) as e:
        print(f"axlpool resource-spec: {e}", file=sys.stderr)
        return 1
    return 0


def _units(ns: argparse.Namespace) -> int:
    try:
        units = render(Path(ns.tree).resolve(), Path(sys.executable))
    except UnitError as e:
        print(f"axlpool units: {e}", file=sys.stderr)
        return 1
    written = write(Path(ns.out).absolute(), units)
    for p in written:
        print(f"wrote {p}")
    print(f"install: cp {' '.join(str(p) for p in written)} ~/.config/systemd/user/ && "
          f"systemctl --user daemon-reload && systemctl --user enable --now axlpool.socket")
    return 0


def _run(ap: argparse.ArgumentParser, ns: argparse.Namespace) -> int:
    cmd = list(ns.cmd)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        ap.error("run: no test command (axlpool run ... -- test.sh [args])")
    tree = Path(ns.tree).resolve() if ns.tree else find_tree(Path.cwd())
    if tree is None:
        ap.error("run: no --tree, and no test/pool/classes.toml above the current directory")
    return run(Path(ns.socket), tree, ns.class_name, ns.arch, cmd, timeout=ns.timeout,
               keep_files_on_fail=ns.keep_files_on_fail, agent=ns.agent,
               expect_code=ns.expect_code)


if __name__ == "__main__":
    sys.exit(main())
