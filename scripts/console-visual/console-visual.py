#!/usr/bin/env python3
"""console-visual: a visual/differential test harness for anything that
draws the UEFI console -- see README.md next to this file.

One QEMU boot per run (none with --attach), booted only through
scripts/run-qemu.sh; every scenario of the chosen suite (cases/<suite>.py)
in that one guest; per step the client's grid against the grid oracle, cell
by cell, with pictures when they differ (or always, with --shots); results
under out/console-visual/<suite>/ with an index.html gallery.
"""
from __future__ import annotations

import argparse
import importlib
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cvlib.cases import Suite  # noqa: E402
from cvlib.boot import boot_guest  # noqa: E402
from cvlib.diff import selftest  # noqa: E402
from cvlib.render import selftest as render_selftest  # noqa: E402
from cvlib import explore  # noqa: E402
from cvlib.gallery import write_gallery  # noqa: E402
from cvlib.guest import PROJECT_DIR, Guest, GuestError  # noqa: E402
from cvlib.runner import OK_VERDICTS, Heartbeat, Runner, ScenarioResult  # noqa: E402


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="console visual/differential harness")
    p.add_argument("command", nargs="?", choices=("explore",),
                   help="explore: drive a live session by hand (README, Exploring by hand)")
    p.add_argument("--suite", default="sshd", help="a module in cases/ (default sshd)")
    p.add_argument("--arch", default="x64", choices=("x64", "aa64"))
    p.add_argument("--only", default="", help="comma-separated scenario names")
    p.add_argument("--list", action="store_true", help="list the suite's scenarios and exit")
    p.add_argument("--selftest", action="store_true",
                   help="prove the comparison can fail (no QEMU) and exit")
    p.add_argument("--shots", action="store_true",
                   help="pictures for EVERY step, not only the differing ones")
    p.add_argument("--keep", action="store_true",
                   help="leave the guest running for a later --attach")
    p.add_argument("--attach", action="store_true", help="reuse the --keep guest (no boot)")
    p.add_argument("--stop", action="store_true", help="stop a --keep guest and exit")
    p.add_argument("--no-build", action="store_true", help="do not run the suite's build")
    p.add_argument("--out", type=Path, default=None,
                   help="output directory (default out/console-visual/<suite>)")
    p.add_argument("--heartbeat", type=Path, default=None,
                   help="append a progress line to this file every --heartbeat-secs")
    p.add_argument("--heartbeat-secs", type=float, default=120.0)
    explore.add_arguments(p)
    return p.parse_args()


def load_suite(name: str) -> Suite:
    mod = importlib.import_module(f"cases.{name}")
    suite = getattr(mod, "SUITE", None)
    if not isinstance(suite, Suite):
        raise SystemExit(f"cases/{name}.py defines no SUITE")
    return suite


def main() -> int:
    args = parse_args()
    if args.selftest:
        fails = selftest()
        suite_test = load_suite(args.suite).selftest
        if suite_test is not None:
            fails += suite_test()
        fails += explore.selftest()
        fails += render_selftest()
        for f in fails:
            print(f"selftest FAIL: {f}")
        print("selftest: " + ("FAIL" if fails else "ok"))
        return 1 if fails else 0
    suite = load_suite(args.suite)
    if args.list:
        for sc in suite.cases:
            print(f"{sc.name:12s} {sc.what}{'  [destructive]' if sc.destructive else ''}")
        return 0
    outdir: Path = args.out or PROJECT_DIR / "out" / "console-visual" / suite.name
    outdir.mkdir(parents=True, exist_ok=True)
    state_file = outdir / f"guest-{args.arch}" / "state.json"
    if args.stop:
        try:
            Guest.attach(state_file, suite.name, args.arch).stop()
        except GuestError as e:
            print(e)
        explore.stop_tmux(suite, args.arch, state_file.parent)
        state_file.unlink(missing_ok=True)
        return 0
    if args.command == "explore":
        explore_dir = (args.out or PROJECT_DIR / "out" / "console-visual") / "explore"
        try:
            return explore.explore_main(args, suite, state_file, explore_dir)
        except GuestError as e:
            print(f"explore: {e}", file=sys.stderr)
            return 1
    names = [n for n in args.only.split(",") if n]
    unknown = [n for n in names if n not in {c.name for c in suite.cases}]
    if unknown:
        print(f"unknown scenario(s): {', '.join(unknown)} (see --list)", file=sys.stderr)
        return 2
    keeping = args.keep or args.attach
    # A destructive case runs last, and against a kept guest only when named.
    cases = sorted((c for c in suite.cases
                    if (c.name in names if names else not (c.destructive and keeping))),
                   key=lambda c: c.destructive)
    t_start = time.monotonic()
    timing: dict[str, float] = {}
    if args.attach:
        guest = Guest.attach(state_file, suite.name, args.arch)
        timing["boot (attached: none)"] = 0.0
    else:
        guest, timing = boot_guest(suite, args.arch, state_file, args.no_build, args.keep)
    runner = Runner(suite, guest, outdir, args.shots, 3.0 if args.arch == "aa64" else 1.0)
    hb = Heartbeat(args.heartbeat, args.heartbeat_secs, guest.state.qemu_pid)
    runner.heartbeat = hb
    hb.start()
    results: list[ScenarioResult] = []
    try:
        for sc in cases:
            print(f"{sc.name}: {sc.what}", flush=True)
            r = runner.run_scenario(sc)
            timing[sc.name] = r.secs
            results.append(r)
            if not guest.alive():
                print("the guest died; stopping", flush=True)
                break
    finally:
        hb.end()
        runner.close_sessions()
        runner.tmux.kill_server()
        if not keeping:
            guest.stop()
    timing["total"] = time.monotonic() - t_start
    meta = {"suite": suite.name, "arch": args.arch, "console": f"{runner.cols}x{runner.rows}",
            "date": time.strftime("%Y-%m-%d %H:%M"),
            "guest": "attached" if args.attach else ("kept" if args.keep else "booted+stopped")}
    idx = write_gallery(outdir, suite, results, timing, meta)
    summary = {"meta": meta, "timing": timing,
               "scenarios": [{"name": r.name, "secs": r.secs, "qemu_cores": r.qemu_cores,
                              "error": r.error, "steps": [vars(s) for s in r.steps]}
                             for r in results]}
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2))
    print("timing: " + ", ".join(f"{k} {v:.1f}s" for k, v in timing.items()))
    n_diff = sum(1 for r in results for s in r.steps if s.verdict not in OK_VERDICTS)
    n_fail = sum(1 for r in results for s in r.steps if s.fails)
    # Steps that differ only by classified quirks pass, and are named here so a
    # quirk never goes quiet.
    for r in results:
        for s in r.steps:
            if s.verdict not in OK_VERDICTS and not s.fails:
                print(f"  classified only: {r.name}/{s.step} [{s.session}] {s.verdict} "
                      f"({', '.join(s.known)})")
    n_err = sum(1 for r in results if r.error)
    # A step whose conditions never held did not show what it was written to
    # show, whatever its screens compare as.
    n_unmet = sum(1 for r in results for s in r.steps
                  if s.target and (s.client_secs is None or s.oracle_secs is None))
    print(f"{sum(len(r.steps) for r in results)} captures, {n_diff} differ, "
          f"{n_fail} unclassified or a bug, {n_unmet} never met their conditions, "
          f"{n_err} scenario error(s); gallery {idx}")
    return 1 if (n_fail or n_err or n_unmet) else 0


if __name__ == "__main__":
    sys.exit(main())
