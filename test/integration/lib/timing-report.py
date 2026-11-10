#!/usr/bin/env python3
"""timing-report.py -- where an integration run's wall time went, per test,
read from the run's log directory alone.

Usage:
    timing-report.py [--tests-dir DIR] [--top N] LOGDIR

LOGDIR is what run-integration.sh prints as `logs: <dir>` (a CTest tree keeps
build/<arch>-debug/integration-logs). Each <test>.log there carries the TIMING
lines its test's processes wrote -- the formats are defined once, in the
"Wall-time accounting" block of scripts/axl-common.sh:

    TIMING: guest ...     common-test.sh, one per guest run
    TIMING: test=...      common-test.sh, the test's summary
    TIMING: run-qemu ...  scripts/run-qemu.sh, one per guest
    TIMING: runner ...    lib/run-one.sh, the whole run (after it ends)
    axlpool run: leased / released ...   the pooled run's wrapper

Columns, in seconds:
    TOTAL    the runner's wall (else the test summary's total)
    CPU      the runner's cpu: user + system of every process the test
             reaped -- its QEMUs, builds and tools ("-": a log from before
             the field). On a CPU-bound run this, not TOTAL, is what a test
             costs the others. A pooled guest's QEMU is the pool's: not in it
    SETUP    before the test's first guest run; POST after its last
    BOOT     guest launch to the Shell banner, summed over booted runs (a
             trailing ~ marks a floor: some run printed no boot reading)
    GUEST-B  booted guest runs, boot included (common-test.sh and run-qemu.sh)
    GUEST-P  pooled guest runs (the exec on a leased guest)
    POOL     the lease's wait plus the release, as the wrapper measured them
    HOST     TOTAL less GUEST-B, GUEST-P and POOL: everything host-side
             (builds, image staging, checks, harness), floored at 0 when
             concurrent guests overlap

A census, not a sample: every log is accounted for. A test that recorded no
guest timing is sorted by what its script (or a lib/*.sh it sources, comment
lines ignored) can launch: QEMU directly (`qemu-system-*`, an exec of
TEST_QEMU_CMD) -- UNMEASURED, a guest nothing instruments may have run; an
instrumented launcher only (run-qemu.sh, test_run_foreground/background,
test_timing_begin) -- STARTED NO GUEST, since those report every guest they
start (a dry run, a stub); neither -- HOST-ONLY. A log with no TIMING line
at all is unmeasured. Earlier attempts (a retry's first,
a pool miss's pooled run): their wall is counted below the table, their CPU
in the test's CPU cell -- both were paid.

Exit: 0, or 2 when LOGDIR is not a directory.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# Launchers that print a TIMING line for every guest they start.
LAUNCHER_RE = re.compile(r"test_run_foreground|test_run_background|test_timing_begin|"
                         r"run-qemu\.sh|RUN_QEMU")
# QEMU started by the script itself, which nothing instruments.
DIRECT_RE = re.compile(r"qemu-system-|\"\$\{TEST_QEMU_CMD\[@\]\}\"|\"\$TEST_QEMU_BIN\"|\"\$QEMU_BIN\"")
COMMENT_RE = re.compile(r"^\s*#.*$", re.M)
SOURCE_RE = re.compile(r"""^\s*(?:source|\.)\s+["']?[^"'\s]*?/(lib/[A-Za-z0-9_.-]+\.sh)""", re.M)
KV_RE = re.compile(r"(\S+?)=(\S*)")
LEASED_RE = re.compile(r"^axlpool run: leased .* in ([0-9]+) ms$")
RELEASED_RE = re.compile(r"^axlpool run: released .* in ([0-9]+) ms$")
STATUS = {0: "PASS", 77: "SKIP", 124: "TIMEOUT"}


def _secs(v: str) -> float | None:
    try:
        return float(v)
    except ValueError:
        return None


def _kv(line: str) -> dict[str, str]:
    return dict(KV_RE.findall(line))


@dataclass
class TestTiming:
    """One test's TIMING lines, folded."""

    name: str
    runner_wall: float | None = None
    runner_cpu: float | None = None
    cpu_floor: bool = False      # an earlier attempt had no cpu reading
    runner_rc: int | None = None
    test_total: float | None = None
    setup: float | None = None
    post: float | None = None
    boot: float = 0.0
    boot_unknown: int = 0
    guest_b: float = 0.0
    guest_p: float = 0.0
    booted: int = 0
    pooled: int = 0
    lease_ms: int | None = None
    release_ms: int | None = None
    any_line: bool = False

    @property
    def total(self) -> float | None:
        return self.runner_wall if self.runner_wall is not None else self.test_total

    @property
    def guests(self) -> int:
        return self.booted + self.pooled

    @property
    def pool(self) -> float:
        return ((self.lease_ms or 0) + (self.release_ms or 0)) / 1000.0

    @property
    def has_pool(self) -> bool:
        return self.lease_ms is not None or self.release_ms is not None

    @property
    def host(self) -> float:
        return max(0.0, (self.total or 0.0) - self.guest_b - self.guest_p - self.pool)

    @property
    def status(self) -> str:
        if self.runner_rc is None:
            return "-"
        return STATUS.get(self.runner_rc, "FAIL")


def parse_log(name: str, path: Path) -> TestTiming:
    """Fold one log's TIMING lines. The test's own summary is the one named
    for it; a nested test's summary contributes nothing (its guest lines,
    which land in this log too, already count)."""
    t = TestTiming(name)
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("TIMING: "):
            t.any_line = True
            kv = _kv(line[len("TIMING: "):])
            if line.startswith(("TIMING: guest ", "TIMING: run-qemu ")):
                g = _secs(kv.get("guest", "")) or 0.0
                # run-qemu.sh's pooled mode says mode=pooled (design §6.9).
                if kv.get("kind") == "pooled" or kv.get("mode") == "pooled":
                    t.guest_p += g
                    t.pooled += 1
                else:
                    t.guest_b += g
                    t.booted += 1
                    b = _secs(kv.get("boot", "-"))
                    if b is None:
                        t.boot_unknown += 1
                    else:
                        t.boot += b
            elif line.startswith("TIMING: test=") and kv.get("test") == name:
                t.test_total = _secs(kv.get("total", ""))
                t.setup = _secs(kv.get("setup", "-"))
                t.post = _secs(kv.get("post", "-"))
            elif line.startswith("TIMING: runner ") and kv.get("test") == name:
                t.runner_wall = _secs(kv.get("wall", ""))
                t.runner_cpu = _secs(kv.get("cpu", "-"))
                rc = kv.get("rc", "")
                t.runner_rc = int(rc) if rc.isdigit() else None
        elif m := LEASED_RE.match(line):
            t.lease_ms = int(m.group(1))
        elif m := RELEASED_RE.match(line):
            t.release_ms = int(m.group(1))
    return t


def runner_field(path: Path, key: str) -> float | None:
    """An earlier attempt's runner @key, wall or cpu (None when it has none)."""
    for line in reversed(path.read_text(errors="replace").splitlines()):
        if line.startswith("TIMING: runner "):
            return _secs(_kv(line).get(key, ""))
    return None


EARLIER_SUFFIXES = (".attempt1.log", ".pool-miss.log")


def reaches(script: Path, pattern: re.Pattern[str], seen: set[Path] | None = None) -> bool:
    """Whether @script, or a lib/*.sh it sources (transitively), matches
    @pattern outside its comment lines."""
    seen = seen if seen is not None else set()
    if script in seen or not script.is_file():
        return False
    seen.add(script)
    text = COMMENT_RE.sub("", script.read_text(errors="replace"))
    if pattern.search(text):
        return True
    root = script.parent if script.parent.name != "lib" else script.parent.parent
    return any(reaches(root / rel, pattern, seen) for rel in SOURCE_RE.findall(text))


def _cell(v: float | None, *, unknown_suffix: bool = False) -> str:
    if v is None:
        return "-"
    return f"{v:.3f}{'~' if unknown_suffix else ''}"


def _row(cells: list[str], runs: str, status: str, name: str) -> str:
    return "".join(f"{c:>9}" for c in cells) + f"  {runs:<5} {status:<7} {name}".rstrip()


def _runs(t: TestTiming) -> str:
    s = (f"{t.booted}b" if t.booted else "") + (f"{t.pooled}p" if t.pooled else "")
    return s or "-"


def report(logdir: Path, tests_dir: Path | None, top: int | None, sort: str = "total") -> str:
    out: list[str] = [f"timing report: {logdir}"]
    logs = sorted(p for p in logdir.glob("*.log")
                  if not p.name.startswith("_")
                  and not p.name.endswith(EARLIER_SUFFIXES))
    cached_file = logdir / "_cached.txt"
    cached = sorted({ln.strip() for ln in cached_file.read_text().splitlines() if ln.strip()}
                    if cached_file.is_file() else set[str]())
    tests = [parse_log(p.name[:-len(".log")], p) for p in logs]
    # CPU is what a test cost the others, so an earlier attempt's counts too
    # (a retry's first run, a pool miss's pooled run): both were paid.
    for t in tests:
        if t.runner_cpu is None:
            continue
        for suf in EARLIER_SUFFIXES:
            if (logdir / f"{t.name}{suf}").is_file():
                c = runner_field(logdir / f"{t.name}{suf}", "cpu")
                if c is None:
                    t.cpu_floor = True
                else:
                    t.runner_cpu += c
    timed = [t for t in tests if t.total is not None]
    blank = [t for t in tests if t.total is None]
    out.append(f"tests: {len(tests) + len(cached)} -- {len(tests)} logs ({len(timed)} with a wall "
               f"time, {len(blank)} with no TIMING line at all), {len(cached)} cached (not run)")
    out.append("")

    def script_of(t: TestTiming) -> Path | None:
        return tests_dir / t.name if tests_dir is not None else None

    def unsplit(t: TestTiming) -> bool:
        if t.guests or t.status == "SKIP":
            return False
        s = script_of(t)
        return s is not None and reaches(s, DIRECT_RE)

    def no_guest_started(t: TestTiming) -> bool:
        if t.guests or t.status == "SKIP" or unsplit(t):
            return False
        s = script_of(t)
        return s is not None and reaches(s, LAUNCHER_RE)

    if sort == "cpu":
        timed.sort(key=lambda t: (-(t.runner_cpu or 0.0), -(t.total or 0.0), t.name))
    else:
        timed.sort(key=lambda t: (-(t.total or 0.0), t.name))
    out.append(_row(["TOTAL", "CPU", "SETUP", "BOOT", "GUEST-B", "GUEST-P", "POOL", "POST", "HOST"],
                    "RUNS", "STATUS", "TEST"))
    for t in timed[:top] if top is not None else timed:
        if unsplit(t):
            out.append(_row([_cell(t.total), _cell(t.runner_cpu, unknown_suffix=t.cpu_floor)]
                            + ["?"] * 7, "?", t.status,
                            t.name))
            continue
        out.append(_row([
            _cell(t.total), _cell(t.runner_cpu, unknown_suffix=t.cpu_floor), _cell(t.setup),
            _cell(t.boot if t.booted else None, unknown_suffix=t.boot_unknown > 0),
            _cell(t.guest_b if t.booted else None), _cell(t.guest_p if t.pooled else None),
            _cell(t.pool if t.has_pool else None), _cell(t.post), _cell(t.host)],
            _runs(t), t.status, t.name))
    split = [t for t in timed if not unsplit(t)]
    tot = sum(t.total or 0.0 for t in split)
    boot = sum(t.boot for t in split)
    gb = sum(t.guest_b for t in split)
    gp = sum(t.guest_p for t in split)
    pool = sum(t.pool for t in split)
    host = sum(t.host for t in split)
    unknown = sum(t.boot_unknown for t in split)
    nb = sum(t.booted for t in split)
    np_ = sum(t.pooled for t in split)
    runs = ((f"{nb}b" if nb else "") + (f"{np_}p" if np_ else "")) or "-"
    cpu = sum(t.runner_cpu or 0.0 for t in split)
    floor = any(t.cpu_floor for t in split)
    out.append(_row([_cell(tot), _cell(cpu, unknown_suffix=floor),
                     _cell(sum(t.setup or 0.0 for t in split)),
                     _cell(boot, unknown_suffix=unknown > 0), _cell(gb), _cell(gp), _cell(pool),
                     _cell(sum(t.post or 0.0 for t in split)), _cell(host)],
                    runs, "", f"SUM over {len(split)} split tests"))
    out.append("")
    if tot > 0:
        def pct(v: float) -> str:
            return f"{100.0 * v / tot:.1f}%"
        out.append(f"share of the split wall ({tot:.3f} s): boot {pct(boot)}, booted run "
                   f"{pct(gb - boot)}, pooled run {pct(gp)}, pool lease+release {pct(pool)}, "
                   f"host {pct(host)}")
    if unknown:
        out.append(f"~ BOOT is a floor: {unknown} booted runs printed no boot-ready reading")
    uns = [t for t in timed if unsplit(t)]
    if uns:
        out.append(f"unsplit: {sum(t.total or 0.0 for t in uns):.3f} s of wall in {len(uns)} "
                   "tests (marked ?)")
    all_cpu = sum(t.runner_cpu or 0.0 for t in timed)
    all_none = sum(1 for t in timed if t.runner_cpu is None)
    out.append("CPU (user + system of each test's reaped processes; a pooled guest's QEMU is the "
               f"pool's, not in it): {all_cpu:.3f} s over all {len(timed)} tests, earlier attempts "
               "(a retry's first, a pool miss's pooled run) included"
               + (f"; {all_none} of them with no reading (-)" if all_none else "")
               + ("; ~ a floor: an earlier attempt had none" if any(t.cpu_floor for t in timed)
                  else ""))
    out.append("")

    unmeasured: list[tuple[str, str]] = [
        (t.name, "no guest timing, and its script launches QEMU itself (not through "
                 "run-qemu.sh, common-test.sh or test_timing_begin)") for t in uns]
    unmeasured += [(t.name, "no TIMING line at all (not even the runner's)") for t in blank]
    unmeasured.sort()
    out.append(f"unmeasured ({len(unmeasured)}):")
    out.extend(f"  {n}: {why}" for n, why in unmeasured)
    started_none = sorted(t.name for t in timed if no_guest_started(t))
    if started_none:
        out.append(f"started no guest ({len(started_none)}): {', '.join(started_none)} -- "
                   f"{'it names' if len(started_none) == 1 else 'they name'} an instrumented "
                   "launcher, which reports every guest it starts")
    host_only = sorted(t.name for t in timed if not t.guests and t.status != "SKIP"
                       and not unsplit(t) and not no_guest_started(t))
    skipped = sorted(t.name for t in timed if t.status == "SKIP")
    out.append(f"host-only ({len(host_only)}): {', '.join(host_only)}".rstrip())
    out.append(f"skipped ({len(skipped)}): {', '.join(skipped)}".rstrip())
    out.append(f"cached, not run ({len(cached)}): {', '.join(cached)}".rstrip())
    earlier = sorted(p for p in logdir.glob("*.log")
                     if p.name.endswith(EARLIER_SUFFIXES))
    out.append(f"earlier attempts (retry, pool miss): {len(earlier)}, "
               f"{sum(runner_field(p, 'wall') or 0.0 for p in earlier):.3f} s of wall not in the table "
               "(their CPU is in it)")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Where an integration run's wall time went.")
    ap.add_argument("logdir", type=Path)
    ap.add_argument("--tests-dir", type=Path, default=Path(__file__).resolve().parent.parent,
                    help="where the test scripts live (default: test/integration)")
    ap.add_argument("--top", type=int, default=None, help="table only the N dearest tests")
    ap.add_argument("--sort", choices=("total", "cpu"), default="total",
                    help="order the table by wall (default) or by CPU")
    ns = ap.parse_args(argv)
    logdir: Path = ns.logdir
    if not logdir.is_dir():
        print(f"timing-report: {logdir}: not a directory", file=sys.stderr)
        return 2
    print(report(logdir, ns.tests_dir, ns.top, ns.sort))
    return 0


if __name__ == "__main__":
    sys.exit(main())
