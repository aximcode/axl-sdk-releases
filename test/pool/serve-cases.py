#!/usr/bin/env python3
"""serve-cases.py -- `axlpool serve`'s memory cap, on the host (no guest).

Usage: serve-cases.py WORKDIR

The pool's cap (pool.py mem_cap_mb) bounds the guest RAM the service keeps;
these pin how `serve` sets it: half the host's MemTotal by default, a stated
MiB count, or 0 for none -- and that the Service hands it to its Pool.
Prints `  PASS:`/`  FAIL:` per case and `serve-cases: <p> passed, <f> failed`.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration" / "lib"))
from axlpool.pool import LeaseMiss  # noqa: E402
from axlpool.service import Service, mem_cap_mb  # noqa: E402
from checks import Cases  # noqa: E402

TREE = Path(__file__).resolve().parents[2]
MEMINFO = "MemTotal:       39845676 kB\nMemFree:        12000000 kB\n"


def main() -> int:
    work = Path(sys.argv[1])
    c = Cases()

    c.check("no --mem-cap-mb: half of MemTotal, in MiB",
            mem_cap_mb(None, MEMINFO), 39845676 // 1024 // 2)
    c.check("--mem-cap-mb 0: no cap", mem_cap_mb(0, MEMINFO), None)
    c.check("--mem-cap-mb N: N", mem_cap_mb(6144, MEMINFO), 6144)
    try:
        got: object = mem_cap_mb(None, "MemFree: 1 kB\n")
    except ValueError as e:
        got = str(e)
    c.check("no MemTotal to halve: refused, not an uncapped pool",
            got, "no MemTotal line in /proc/meminfo: give --mem-cap-mb (0 for no cap)")

    agent = work / "agent.efi"
    agent.write_bytes(b"MZ")
    for name, cap in (("capped", 1234), ("uncapped", None)):
        logs: list[dict[str, object]] = []
        svc = Service(TREE, agent, work / f"{name}-state", work / f"{name}.sock", exit_idle=0,
                      mem_cap_mb=cap, log=logs.append)
        c.check(f"the Service hands its cap to the Pool ({name})", svc.pool.mem_cap_mb, cap)
        svc.stop()     # stop() before run(): run() releases the socket and returns at once
        svc.run()
        c.check(f"its `started` record states the cap ({name})",
                [r.get("axl.mem_cap_mb", "absent") for r in logs if r["body"] == "started"], [cap])

    # A cap below a class's guest RAM can never fit that class: its lease must
    # MISS at once, saying so -- not wait out the deadline (120 s per test,
    # then a booted rerun) with no reason given. Before any guest exists. The
    # class is ANOTHER tree's: the service serves every checkout, and only its
    # own tree's classes are checked at start (below).
    other = work / "other-tree"
    (other / "test" / "pool").mkdir(parents=True)
    (other / "test" / "pool" / "classes.toml").write_text('[class.big]\nargs = ["--mem", "2G"]\n')
    svc = Service(TREE, agent, work / "low-state", work / "low.sock", exit_idle=0,
                  mem_cap_mb=1024, log=lambda _rec: None)
    t0 = time.monotonic()
    try:
        svc.pool.lease("big", "X64", timeout=30, tree=other)
        got = "leased"
    except LeaseMiss as e:
        got = str(e)
    c.check("a class over the cap misses at once, naming both sizes",
            (got, time.monotonic() - t0 < 5),
            ("over-cap: class big needs 2048 MiB, over the pool's memory cap of 1024 MiB", True))
    svc.stop()
    svc.run()

    # The service's OWN tree: refused at start, which is where it is fixable.
    try:
        Service(TREE, agent, work / "tiny-state", work / "tiny.sock", exit_idle=0,
                mem_cap_mb=300, log=lambda _rec: None)
        got = "started"
    except ValueError as e:
        got = str(e)
    c.check("a cap below its own tree's largest class: the service refuses to start",
            got, "--mem-cap-mb 300 is below class unit's 768 MiB guest RAM: no lease of it could fit")

    print(f"serve-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
