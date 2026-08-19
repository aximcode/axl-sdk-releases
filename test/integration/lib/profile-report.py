#!/usr/bin/env python3
"""profile-report.py — read an AXL_TEST_PROFILE file and say where the time went.

Two questions, one data file (see lib/profile.sh for the format):

  1. **Where is the wall clock?** Per-test boot count and boot seconds, sorted.
     A QEMU suite's cost is guest boots; the -j6 pool parallelises ACROSS tests
     and never WITHIN one, so a test holding many serial boots is that many
     units of work pinned to a single worker. That is invisible in per-test
     timing, which is all run-integration.sh reports.

  2. **Which tests can a given change affect?** `--affected FILE...` maps
     changed build artifacts to the tests that stage them. The association is
     DERIVED from what each test actually staged, never from a path or module
     name -- see AXL-CI-Release-Speed-Design.md §12.5 for why a name-based map
     is unsafe here (a change under src/log/ altered every image in the tree
     through a link-time edge no directory map could see).

Usage:
  AXL_TEST_PROFILE=/tmp/prof.txt ./test/integration/run-integration.sh --arch X64
  python3 test/integration/lib/profile-report.py /tmp/prof.txt
  python3 test/integration/lib/profile-report.py /tmp/prof.txt --affected out/native-x64/hello.efi
"""

from __future__ import annotations

import sys
from collections import defaultdict
from pathlib import Path


class Profile:
    """Records from one profiled run.

    Two record kinds count a boot, because there are two disjoint paths to a
    guest and each needs its own seam:

      boot|  common-test.sh's launcher — carries a real duration
      qemu|  scripts/run-qemu.sh       — a COUNT only, no duration (that
             script replaces its EXIT trap in four places, so a stop stamp
             would clobber a cleanup)

    Durations for the count-only path are derived from `test|`, which is the
    runner's own per-test measurement rather than a second one.
    """

    def __init__(self) -> None:
        self.timed: dict[str, list[tuple[float, str]]] = defaultdict(list)
        self.counted: dict[str, int] = defaultdict(int)
        self.total: dict[str, float] = {}
        self.staged: dict[str, set[str]] = defaultdict(set)

    def boots(self, test: str) -> int:
        return len(self.timed.get(test, ())) + self.counted.get(test, 0)

    def boot_seconds(self, test: str) -> tuple[float, bool]:
        """(seconds attributable to boots, exact?)."""
        timed = self.timed.get(test, ())
        if timed and not self.counted.get(test):
            return sum(d for d, _ in timed), True
        # Count-only (or mixed): the runner's total is the best available
        # figure, and it includes the test's own host-side work. Flagged as
        # approximate rather than presented as if it were measured.
        return self.total.get(test, 0.0), False


def load(path: Path) -> Profile:
    p = Profile()
    for raw in path.read_text(errors="replace").splitlines():
        parts = raw.split("|")
        try:
            if parts[0] == "boot" and len(parts) >= 4:
                p.timed[parts[1]].append((float(parts[2]), parts[3]))
            elif parts[0] == "qemu" and len(parts) >= 2:
                p.counted[parts[1]] += 1
            elif parts[0] == "test" and len(parts) >= 3:
                p.total[parts[1]] = float(parts[2])
            elif parts[0] == "efi" and len(parts) >= 4:
                p.staged[parts[1]].add(parts[2])
        except ValueError:
            continue
    return p


def report_time(p: Profile) -> None:
    tests = set(p.total) | set(p.timed) | set(p.counted)
    rows = []
    for t in tests:
        n = p.boots(t)
        secs, exact = p.boot_seconds(t)
        rows.append((secs, n, secs / n if n else 0.0, exact, t))
    rows.sort(reverse=True)

    nboots = sum(r[1] for r in rows)
    noboot = [r for r in rows if r[1] == 0]
    print(f"=== {len(tests)} tests, {nboots} guest boots, "
          f"{sum(r[0] for r in rows):.0f}s total ===")
    print("    (~ = derived from the test's total, not measured per boot)\n")
    print(f"{'secs':>8} {'boots':>6} {'s/boot':>8}  test")
    for secs, n, avg, exact, t in rows[:25]:
        mark = " " if exact else "~"
        print(f"{secs:8.1f} {n:6d} {mark}{avg:7.1f}  {t}")

    # The shape that matters. The -j6 pool spreads work ACROSS tests and never
    # WITHIN one, so a test holding many serial boots is that many units of
    # work pinned to a single worker. It is invisible in per-test timing, and
    # it sets the floor for any run small enough that the total stops
    # dominating -- which is exactly what --only-local produces.
    serial = sorted((r for r in rows if r[1] >= 4), reverse=True)
    if serial:
        print(f"\n=== {len(serial)} test(s) hold >=4 serial boots — that many "
              f"units of work pinned to ONE worker ===")
        for secs, n, avg, _exact, t in serial:
            print(f"  {t}: {n} boots, {secs:.0f}s "
                  f"— splitting frees ~{secs - avg:.0f}s from the critical path")

    if noboot:
        print(f"\n=== {len(noboot)} test(s) recorded NO boot ===")
        print("    Host-only tests (a link probe, an axl-cc flag check) legitimately")
        print("    boot nothing. A QEMU test in this list is an instrumentation gap,")
        print("    not a fast test — check it reaches QEMU by a third path.")
        for secs, _n, _avg, _e, t in noboot[:10]:
            print(f"  {secs:6.1f}s  {t}")


def report_affected(staged: dict[str, set[str]], changed: list[str]) -> None:
    # Match on basename: the profile records the DESTINATION inside the guest
    # image (`app/hello.efi`), while a caller naturally passes a build path
    # (`out/native-x64/hello.efi`). Matching whole paths would silently select
    # nothing, which is the failure mode that reads as "no test covers this".
    want = {Path(c).name for c in changed}
    hit = sorted(t for t, arts in staged.items()
                 if any(Path(a).name in want for a in arts))
    known = {Path(a).name for arts in staged.values() for a in arts}

    unknown = sorted(want - known)
    if unknown:
        print("!! not staged by ANY profiled test — cannot be mapped, so every")
        print("!! test must be assumed affected:")
        for u in unknown:
            print(f"     {u}")
        print()

    print(f"=== {len(hit)} of {len(staged)} profiled tests stage a changed "
          f"artifact ===")
    for t in hit:
        print(f"  {t}")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = Path(argv[1])
    if not path.exists():
        print(f"no profile at {path} — run with AXL_TEST_PROFILE set",
              file=sys.stderr)
        return 2

    prof = load(path)
    if not prof.total and not prof.staged and not prof.timed:
        print(f"{path} has no usable records", file=sys.stderr)
        return 2

    if "--affected" in argv:
        changed = argv[argv.index("--affected") + 1:]
        if not changed:
            print("--affected needs at least one file", file=sys.stderr)
            return 2
        report_affected(prof.staged, changed)
    else:
        report_time(prof)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
