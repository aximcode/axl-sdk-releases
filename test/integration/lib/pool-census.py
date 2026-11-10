#!/usr/bin/env python3
"""pool-census.py -- group a profiled run's guest launches into pool classes.

Reads the `launch|` records an AXL_TEST_PROFILE file carries (lib/profile.sh,
_prof_launch, and scripts/run-qemu.sh) and answers the question the guest
pool's design turns on (docs/AXL-Test-Guest-Pool-Design.md §8, phase 0): how
many distinct guest SHAPES does the suite boot, and how much of its work does
each one carry?

A class is a launch's command line with everything that varies per run taken
out -- temp paths, image files, ports, socket names, ids -- so that two tests
booting the same machine, firmware and devices land in one class. What is
KEPT is what a pooled guest could not change after boot: arch, machine, CPU,
accelerator, memory, firmware build, each device's driver, network back end,
platform tables (-smbios / -acpitable), and how the run talks to the guest.

Usage:
  AXL_TEST_PROFILE=/tmp/prof.txt ./test/integration/run-integration.sh --arch X64 --no-cache
  python3 test/integration/lib/pool-census.py /tmp/prof.txt [--tests]
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

# Options whose VALUE is part of the guest's shape, kept (temp paths scrubbed).
KEEP_VALUE = {"-machine", "-M", "-cpu", "-m", "-smp", "-vga", "-boot", "-accel", "-bios",
              "-net", "-smbios", "-acpitable", "-fw_cfg", "-global", "-rtc", "-icount"}
# Options whose value is a driver plus properties: the driver and every
# property except the per-run ones (PER_RUN_PROPS).
DRIVER_VALUE = {"-device", "-netdev", "-object", "-tpmdev", "-numa", "-audiodev"}
# Flags that change the guest.
FLAGS = {"-enable-kvm", "-snapshot", "-S", "-nodefaults", "-no-user-config"}
# Harness plumbing: how the run reaches the guest, not what the guest is. The
# same guest is launched with different plumbing by common-test.sh and by each
# run-qemu.sh mode; the launch mode column reports it instead. The value of a
# valued one is dropped with it.
PLUMBING_FLAGS = {"-nographic", "-no-reboot", "-no-shutdown", "-daemonize"}
PLUMBING_VALUED = {"-serial", "-monitor", "-qmp", "-chardev", "-action", "-display",
                   "-name", "-pidfile", "-D", "-d", "-trace", "-k", "-gdb",
                   "-debugcon"}
# Device/back-end properties that name per-run plumbing rather than the guest.
PER_RUN_PROPS = {"id", "bus", "addr", "netdev", "chardev", "drive", "path", "mem-path",
                 "fd", "socket", "sock_fd", "hostfwd", "guestfwd", "port"}
TEMP_PATH = re.compile(r"/(?:dev/shm|tmp)/[^,\s]*")


@dataclass
class Launch:
    """One guest launch, as recorded."""

    test: str
    via: str
    vars: str
    argv: list[str]


@dataclass
class TestUse:
    """What one test booted."""

    launches: int = 0
    classes: set[str] = field(default_factory=lambda: set[str]())
    seconds: float = 0.0


def scrub(value: str) -> str:
    """A value with every per-run temp path replaced by <tmp>."""
    return TEMP_PATH.sub("<tmp>", value)


def props(value: str, keep: set[str] | None = None) -> str:
    """A driver,prop=v,... value without the per-run properties.

    KEEP limits the properties kept to those named (for -drive, where only the
    interface says anything about the guest)."""
    items = value.split(",")
    # A driver name leads (-device nvme,...); -drive leads with a property.
    kept: list[str] = [] if "=" in items[0] else [items[0]]
    for kv in items[len(kept):]:
        k = kv.split("=", 1)[0]
        if (keep is not None and k not in keep) or (keep is None and k in PER_RUN_PROPS):
            continue
        v = kv.split("=", 1)[-1] if "=" in kv else ""
        # A path names a per-run file; an EMPTY value (`romfile=`: no option
        # ROM) and a bare name QEMU resolves in its own data directory are
        # part of the guest, and stay.
        if k in ("file", "romfile") and "/" in v and not v.startswith("sha256:"):
            kv = f"{k}=<path>"
        kept.append(scrub(kv))
    return ",".join(kept)


def shape(launch: Launch) -> str:
    """The class key: what a pooled guest could not change after boot."""
    argv = launch.argv
    parts: list[str] = []
    if launch.vars:
        parts.append(f"vars={launch.vars}")
    i = 1
    while i < len(argv):
        opt = argv[i]
        val = argv[i + 1] if i + 1 < len(argv) and not argv[i + 1].startswith("-") else None
        if opt in FLAGS:
            parts.append(opt)
        elif opt in PLUMBING_FLAGS:
            pass
        elif opt in PLUMBING_VALUED:
            pass
        elif opt == "-drive" and val is not None:
            if "if=pflash" in val and "readonly=on" in val:
                m = re.search(r"file=([^,]+)", val)
                parts.append(f"fw={m.group(1) if m else '?'}")
            elif "if=pflash" not in val:
                # The per-run vars copy is skipped: its TEMPLATE is `vars=`.
                parts.append(f"disk={props(val, {'if', 'media', 'format'})}")
        elif opt == "-blockdev" and val is not None:
            parts.append(f"blockdev={props(val, {'driver'})}")
        elif opt in KEEP_VALUE and val is not None:
            parts.append(f"{opt}={scrub(val)}")
        elif opt in DRIVER_VALUE and val is not None:
            parts.append(f"{opt}={props(val)}")
        else:
            parts.append(opt if val is None else f"{opt}={scrub(val)}")
        if opt in FLAGS or opt in PLUMBING_FLAGS:
            i += 1
        else:
            i += 2 if val is not None else 1
    return " ".join([Path(argv[0]).name if argv else "?"] + sorted(parts))


def mode(launch: Launch) -> str:
    """How the run reached the guest -- harness mechanics, not the guest's shape.

    run-qemu.sh records its mode flags (only the ones set are kept here);
    common-test.sh's two launchers record foreground/background."""
    if not launch.via.startswith("run-qemu:"):
        return launch.via
    on = [kv.split("=")[0] for kv in launch.via[len("run-qemu:"):].split(",")
          if kv.split("=")[-1] not in ("", "false")]
    return "run-qemu" + (f"[{','.join(on)}]" if on else "")


def read(path: Path) -> tuple[list[Launch], dict[str, float], int]:
    """The launch records, the runner's per-test seconds, and how many launch
    records were MALFORMED -- counted, never dropped silently: a record in an
    older shape (no vars field) would otherwise vanish and shorten the count."""
    launches: list[Launch] = []
    seconds: dict[str, float] = {}
    bad = 0
    for raw in path.read_text(errors="replace").splitlines():
        kind, _, rest = raw.partition("|")
        if kind == "launch":
            fields = rest.split("|", 3)
            if len(fields) != 4 or not fields[3]:
                bad += 1
                continue
            test, via, vars_template, cmd = fields
            launches.append(Launch(test, via, vars_template, cmd.split("\x1f")))
        elif kind == "test":
            name, _, dur = rest.partition("|")
            try:
                seconds[name] = float(dur)
            except ValueError:
                pass
    return launches, seconds, bad


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("profile", type=Path)
    ap.add_argument("--tests", action="store_true", help="also list every test")
    a = ap.parse_args()
    launches, seconds, bad = read(a.profile)
    if bad:
        print(f"pool-census: {bad} launch record(s) in {a.profile} are malformed (an "
              "older record shape?) and were NOT counted")
    if not launches:
        print(f"pool-census: {a.profile} has no launch| records -- was it written "
              "by a run with AXL_TEST_PROFILE set, after lib/profile.sh grew "
              "_prof_launch?")
        return 2

    ids: dict[str, str] = {}
    by_class: dict[str, list[Launch]] = defaultdict(list)
    uses: dict[str, TestUse] = defaultdict(TestUse)
    for la in launches:
        key = shape(la)
        by_class[key].append(la)
        uses[la.test].launches += 1
        uses[la.test].classes.add(key)
    for name, use in uses.items():
        use.seconds = seconds.get(name, 0.0)
    ranked = sorted(by_class, key=lambda k: -len(by_class[k]))
    for n, key in enumerate(ranked, 1):
        ids[key] = f"C{n}"

    single = {t for t, u in uses.items() if len(u.classes) == 1}
    total_s = sum(u.seconds for u in uses.values())
    print(f"=== {len(launches)} launches, {len(uses)} tests, {len(by_class)} classes, "
          f"{total_s:.0f}s of test work in tests that boot ===\n")
    print("class  launches  tests  test-secs  shape\n"
          "                                     launched as")
    for key in ranked:
        tests = {la.test for la in by_class[key]}
        secs = sum(uses[t].seconds for t in tests if t in single)
        modes: dict[str, int] = defaultdict(int)
        for la in by_class[key]:
            modes[mode(la)] += 1
        print(f"{ids[key]:>5}  {len(by_class[key]):>8}  {len(tests):>5}  {secs:>9.0f}  {key}")
        print(" " * 37 + ", ".join(f"{m} x{n}" for m, n in sorted(modes.items(),
                                                                 key=lambda kv: -kv[1])))
    multi = sorted((t for t in uses if t not in single), key=lambda t: -uses[t].seconds)
    print(f"\n{len(multi)} test(s) boot more than one class (test-secs above count "
          "single-class tests only):")
    for t in multi:
        u = uses[t]
        print(f"  {t}: {u.launches} launches, classes {sorted(ids[k] for k in u.classes)}, "
              f"{u.seconds:.0f}s")
    if a.tests:
        print("\nper test: launches  secs  classes")
        for t in sorted(uses, key=lambda t: -uses[t].seconds):
            u = uses[t]
            print(f"  {t:50s} {u.launches:>3} {u.seconds:>6.0f}  "
                  f"{' '.join(sorted(ids[k] for k in u.classes))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
