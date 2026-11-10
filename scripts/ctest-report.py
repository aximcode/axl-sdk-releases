#!/usr/bin/env python3
"""ctest-report.py -- render a ctest JUnit file the way the front ends print.

run-integration.sh and verify.sh are thin front ends over CTest (slice 5a
step 6, decision 1; docs/AXL-Build-System-Design.md §10.8). Each runs ctest
with `--output-junit` and prints its summary from that file through here, so
the rendering is one pure function, pinned on JUnit files captured from real
ctest runs (test/integration/test-ctest-report.sh).

  runner   the integration runner's per-test lines and headline:
             `  <name>.sh PASS|FAIL|TIMEOUT|SKIP|UNJUDGED|CACHED <n>s ...`
                                                              (sorted)
             `integration: P passed, F failed[, K SKIPPED][, U UNJUDGED] (<ARCH>)`
             `    skipped: <name>.sh`   (one per skip)
             `    unjudged: <name>.sh`  (one per UNJUDGED test)
           An UNJUDGED test (lib/run-one.sh: a 77 that printed an
           `UNJUDGED: <why>` line -- timing bounds missed while the host
           starved the guest of CPU) is ctest's skip, but neither a pass nor a
           skip here: counted on its own, and with nothing failed the exit is
           3, never 0 -- not red, not green.

  unjudged the script names of the UNJUDGED tests in a JUnit file, one per
           line (scripts/release-suite.sh re-runs each alone)

  verdict  one test's result, as one word: PASS (ctest ran it and its own
           line says PASS), else UNJUDGED, SKIP, FAIL or MISSING -- exit 0
           only for PASS (the quiet re-run certifies only a judged pass)

Each test's line is the one lib/run-one.sh printed into its own output (with
any notice it printed about the test, such as a pool miss, first), so
it carries what ctest's status cannot: TIMEOUT apart from FAIL, a SKIP's
reason, a retry. ctest's status is the cross-check: when the two disagree,
or the line is missing, the test is a FAIL that says so -- a disagreement is
never resolved in the test's favour. A test ctest did not run because a
fixture it needs failed is NOT RUN, counted as failed, under a line that
names the failed fixture and quotes its output.

  verify   verify.sh's table -- `JOB RC TIME DETAIL`, one row per job run --
           the cross-arch line, and the verdict (ALL GREEN, its partial-run
           form, SOMETHING FAILED, or NOT GREEN: UNJUDGED). A job is a set of
           tests in one of the three runs (x64 tree, aa64 tree, qloop alone):
             x64, aa64  unit.one-boot (a full run, booted: every binary in
                        one guest, its ratchet and budget; the detail is its
                        Results line), or the unit.* tests of that tree --
                        a full run with the units POOLED (the detail is the
                        ratchet's line and its budget's, judged or not), or
                        --apps (the named binaries, the ratchet and its
                        fixtures, the inventory)
             lint/docs  gate.lint / gate.docs
             gates      the gate.check-* not labelled artifact, both trees
             imggates   the gate.check-* labelled artifact, both trees
             qloop      gate.qloop, whose SKIP is UNJUDGED -- only with a
                        verdict (scripts/qloop-verdict.sh's) to say why
           TIME is the job's summed test time -- for per-binary unit
           tests, which run concurrently, a sum of their times (their lease
           waits included), not the job's wall time. Cross-arch compares the
           two one-boot runs' passed + skipped assertions, or, per binary
           (pooled, or --apps), the ratchet's `unit-counts:` line, so that
           divergence names its binary.

Exit: 0 nothing failed; 1 something failed or did not run; 2 the report
could not be made (an unreadable file, or no integration test in it -- an
empty answer is not "0 failed"); 3 nothing failed, but something could not
judge (runner: an UNJUDGED test; verify: the quiet slot, qloop).
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

RESULT_LINE = re.compile(r"^  (\S+\.sh) (PASS|FAIL|TIMEOUT|SKIP|UNJUDGED|CACHED)\b.*$")
# What each ctest status allows the test's own line to say. UNJUDGED is a 77
# to ctest, so it agrees only with ctest's skip.
AGREES: dict[str, set[str]] = {
    "run": {"PASS", "CACHED"},
    "fail": {"FAIL", "TIMEOUT"},
    "skip": {"SKIP", "UNJUDGED"},
}
SKIP_MESSAGE = re.compile(r"^SKIP_RETURN_CODE=\d+$")


@dataclass
class Case:
    """One ctest test, as its JUnit testcase records it."""

    name: str
    status: str          # run | fail | notrun
    time: float
    labels: list[str]
    message: str         # <failure>/<skipped> message, or ""
    out: str             # <system-out>


def load(path: Path) -> list[Case]:
    """The JUnit file's testcases, or SystemExit(2) naming why not."""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        print(f"ctest-report: cannot read {path}: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
    cases: list[Case] = []
    for tc in root.iter("testcase"):
        labels: list[str] = []
        for prop in tc.iter("property"):
            if prop.get("name") == "cmake_labels":
                labels = [x for x in (prop.get("value") or "").split(";") if x]
        msg = ""
        for tag in ("failure", "skipped"):
            el = tc.find(tag)
            if el is not None:
                msg = el.get("message") or ""
        out_el = tc.find("system-out")
        try:
            secs = float(tc.get("time") or 0)
        except ValueError:
            secs = 0.0
        cases.append(Case(tc.get("name") or "", tc.get("status") or "", secs,
                          labels, msg, (out_el.text or "") if out_el is not None else ""))
    return cases


def script_name(test: str) -> str:
    """integration.test-foo -> test-foo.sh"""
    return test.removeprefix("integration.") + ".sh"


def runner(args: argparse.Namespace) -> int:
    """The integration runner's per-test lines and headline."""
    cases = load(Path(args.junit))
    tests = {c.name: c for c in cases if "integration" in c.labels}
    fixtures = {c.name: c for c in cases if "integration" not in c.labels}
    expected: list[str] = []
    if args.expect:
        expected = [x for x in Path(args.expect).read_text().split() if x]
    if not tests and not expected:
        print(f"ctest-report: no integration test in {args.junit}", file=sys.stderr)
        return 2

    failed_fixtures = [f for f in fixtures.values() if f.status == "fail"]
    for f in sorted(failed_fixtures, key=lambda c: c.name):
        # No claim about what followed: a SETUP fixture's dependents did not
        # run (each says so below), a CLEANUP fixture failed after them.
        print(f"integration: {f.name} FAILED:")
        for line in f.out.rstrip("\n").splitlines()[-12:]:
            print(f"    | {line}")

    passed = failed = 0
    skipped: list[str] = []
    unjudged: list[str] = []
    for name in sorted(set(tests) | set(expected)):
        sh = script_name(name)
        c = tests.get(name)
        if c is None:
            print(f"  {sh} NOT RUN (ctest never ran it -- not registered?)")
            failed += 1
            continue
        status = c.status
        if status == "notrun" and SKIP_MESSAGE.match(c.message):
            status = "skip"
        if status == "notrun":
            deps = re.search(r"Failed test dependencies: (.+)", c.out)
            why = c.message + (f": {deps.group(1).strip()}" if deps else "")
            print(f"  {sh} NOT RUN ({why or 'ctest did not run it'})")
            failed += 1
            continue
        # run-one's notices about this test ("  <name>.sh: pool miss (...)"),
        # which the runner used to print live, come first.
        for note in c.out.splitlines():
            if note.startswith(f"  {sh}: "):
                print(note)
        # THIS test's result line: another test's (a nested run's) is not it.
        lines = c.out.splitlines()
        at = next((i for i, x in enumerate(lines)
                   if (m := RESULT_LINE.match(x)) and m.group(1) == sh), None)
        line = RESULT_LINE.match(lines[at]) if at is not None else None
        word = {"run": "passed", "fail": "failed", "skip": "skipped"}.get(status, status)
        if status == "fail" and c.message and c.message != "Failed":
            word += f" ({c.message})"   # ctest's own reason: Timeout, ...
        if line is None:
            print(f"  {sh} FAIL (ctest: {word}; the test printed no result line)")
            failed += 1
        elif line.group(2) not in AGREES.get(status, set()):
            print(f"  {sh} FAIL (ctest: {word}; the test's own line: {line.group(0).strip()})")
            failed += 1
        else:
            print(line.group(0))
            if status == "fail":
                failed += 1
                # run-one's excerpt of the failing test's output follows its
                # line: the reason, inline, as the runner showed it.
                assert at is not None
                for x in lines[at + 1:]:
                    if not x.startswith("    "):
                        break
                    print(x)
            elif line.group(2) == "UNJUDGED":
                unjudged.append(sh)
            else:
                passed += 1
                if status == "skip":
                    skipped.append(sh)

    print("")
    tail = f", {len(skipped)} SKIPPED" if skipped else ""
    tail += f", {len(unjudged)} UNJUDGED" if unjudged else ""
    print(f"integration: {passed} passed, {failed} failed{tail} ({args.arch})")
    for sh in skipped:
        print(f"    skipped: {sh}")
    for sh in unjudged:
        print(f"    unjudged: {sh}")
    if failed:
        return 1
    if unjudged:
        who, its, them = ((unjudged[0], "its", "it") if len(unjudged) == 1
                          else (", ".join(unjudged), "their", "them"))
        print(f"integration: NOT GREEN: UNJUDGED -- {who} could not judge {its} timing "
              f"bounds (the host starved {its} guest of CPU). Not a pass; re-run {them} "
              "on a quiet box.")
        return 3
    return 0


def unjudged_names(args: argparse.Namespace) -> int:
    """The UNJUDGED tests' script names, one per line: their own result line
    says UNJUDGED and ctest agrees it skipped. Exit 2 when the file cannot be
    read -- an unread report is not "none unjudged"."""
    for c in load(Path(args.junit)):
        if "integration" not in c.labels:
            continue
        if not (c.status == "notrun" and SKIP_MESSAGE.match(c.message)):
            continue
        sh = script_name(c.name)
        if any((m := RESULT_LINE.match(x)) and m.group(1) == sh and m.group(2) == "UNJUDGED"
               for x in c.out.splitlines()):
            print(sh)
    return 0


ALL_JOBS = ["x64", "aa64", "lint", "gates", "imggates", "qloop", "docs"]
UNIT_FIXTURES = {"unit.counts-reset", "unit.ratchet", "unit.inventory"}


def one_verdict(args: argparse.Namespace) -> int:
    """One test's verdict word; exit 0 only for a judged PASS."""
    want = args.test
    word = "MISSING"
    for c in load(Path(args.junit)):
        if "integration" not in c.labels or script_name(c.name) != want:
            continue
        line = next((m for x in c.out.splitlines()
                     if (m := RESULT_LINE.match(x)) and m.group(1) == want), None)
        status = c.status
        if status == "notrun" and SKIP_MESSAGE.match(c.message):
            status = "skip"
        if line is None or line.group(2) not in AGREES.get(status, set()):
            word = "FAIL"
        else:
            word = "FAIL" if line.group(2) == "TIMEOUT" else line.group(2)
        break
    print(word)
    return 0 if word == "PASS" else 1


def counts_of(case: Case | None) -> dict[str, int]:
    """The ratchet's `unit-counts: App=N ...` line, as a map."""
    if case is None:
        return {}
    for line in case.out.splitlines():
        if line.startswith("unit-counts:"):
            pairs = (x.split("=", 1) for x in line.split()[1:] if "=" in x)
            return {k: int(v) for k, v in pairs if v.isdigit()}
    return {}


ONE_BOOT = "unit.one-boot"


def results_line(case: Case) -> str:
    """The last `Results:` line of a one-boot run, prefix stripped ("" if none)."""
    lines = [x for x in case.out.splitlines() if x.startswith("Results:")]
    return lines[-1].removeprefix("Results:").strip() if lines else ""


def one_boot_total(case: Case | None) -> str:
    """Passed + skipped assertions on a one-boot Results line -- the figure
    the cross-arch check compares ("" when there is no such line)."""
    if case is None:
        return ""
    m = re.search(r"(\d+) passed", results_line(case))
    if not m:
        return ""
    skip = re.search(r"SKIPPED \((\d+) assertions\)", results_line(case))
    return str(int(m.group(1)) + (int(skip.group(1)) if skip else 0))


def failed_names(cases: list[Case], strip: str = "") -> list[str]:
    """The tests that did not pass (a SKIP of the ratchet's 77 is not one)."""
    out: list[str] = []
    for c in cases:
        if c.status == "fail" or (c.status == "notrun" and not SKIP_MESSAGE.match(c.message)):
            out.append(c.name.removeprefix(strip))
    return out


def verify(args: argparse.Namespace) -> int:
    """verify.sh's table, cross-arch line and verdict."""
    jobs = [j for j in args.jobs.split(",") if j]
    unknown = [j for j in jobs if j not in ALL_JOBS]
    if unknown:
        # A job this does not know would get no row and a green "partial run".
        print(f"ctest-report: unknown job(s) {', '.join(unknown)} (known: {' '.join(ALL_JOBS)})",
              file=sys.stderr)
        return 2
    runs: dict[str, dict[str, Case]] = {}
    for tree in ("x64", "aa64", "qloop"):
        path = getattr(args, tree)
        runs[tree] = {c.name: c for c in load(Path(path))} if path else {}

    rows: list[tuple[str, str, float, str]] = []
    unjudged = ""
    unjudged_budget = ""   # x64's pooled budget, when its ratchet did not judge it
    for job in jobs:
        if job in ("x64", "aa64") and ONE_BOOT in runs[job]:
            c = runs[job][ONE_BOOT]
            line = results_line(c)
            if c.status == "fail" or (c.status == "notrun" and not SKIP_MESSAGE.match(c.message)):
                rows.append((job, "1", c.time, "FAILED: " + (line or c.message or ONE_BOOT)))
            elif not line:
                # ctest keeps the HEAD of a test's output; a cut-short tail
                # loses the line that says what ran. Not seen is not green.
                rows.append((job, "1", c.time,
                             f"FAILED: {ONE_BOOT} printed no Results line"
                             " (its output cut short by --test-output-size?)"))
            else:
                rows.append((job, "0", c.time, line))
        elif job in ("x64", "aa64"):
            run = runs[job]
            cases = [c for n, c in run.items() if n.startswith("unit.")]
            binaries = [c for c in cases if c.name not in UNIT_FIXTURES]
            bad = failed_names(cases)
            ratchet = run.get("unit.ratchet")
            if not cases:
                rows.append((job, "1", 0.0, "FAILED: no unit test ran"))
            elif bad:
                # A failed ratchet says WHY -- a count regression and a budget
                # breach are different failures behind the same test name.
                why = ""
                if ratchet is not None and "unit.ratchet" in bad:
                    why = next((x for x in ratchet.out.splitlines() if x.startswith("FAIL: ")), "")
                rows.append((job, "1", sum(c.time for c in cases),
                             "FAILED: " + " ".join(bad) + (f" -- {why}" if why else "")))
            elif ratchet is not None and ratchet.status == "run":
                # The budget's verdict too, judged or NOT: a run that no
                # longer proves its budget (a binary booted after a pool
                # miss, an unmeasured one, the aa64 tree) says so in its row.
                lines = ratchet.out.splitlines()
                line = next((x for x in lines if x.startswith("unit ratchet:")), "")
                budget = next((x for x in lines if x.startswith("unit budget:")), "")
                detail = "; ".join(x for x in (line or "the ratchet passed", budget) if x)
                rows.append((job, "0", sum(c.time for c in cases), detail))
                # x64's budget left unjudged is named in the verdict -- not
                # under --apps, whose own note already says the budget did not
                # judge it. No budget line at all is unjudged too.
                if job == "x64" and not args.apps:
                    if not budget:
                        unjudged_budget = "the ratchet printed no budget line"
                    elif budget.startswith("unit budget: not judged"):
                        unjudged_budget = budget.removeprefix("unit budget: ")
            elif not args.apps:
                # A FULL per-binary (pooled) run whose ratchet judged no whole
                # run: the suite's count was never checked. Not a pass.
                why = next((x for x in (ratchet.out.splitlines() if ratchet else [])
                            if x.startswith("PARTIAL:")), "")
                rows.append((job, "1", sum(c.time for c in cases),
                             "FAILED: the ratchet did not judge a whole run" + (f" -- {why}" if why else "")))
            else:
                # verify.sh refuses an --apps name the tree does not register;
                # this is the report's own check of the same thing: a name
                # with no test ran nothing, and "0 passed" is not a pass.
                named = [a for a in args.apps.split(",") if a]
                ran = {c.name.removeprefix("unit.") for c in binaries}
                missing = [a for a in named if a not in ran]
                if missing:
                    rows.append((job, "1", sum(c.time for c in cases),
                                 f"FAILED: {len(named) - len(missing)} of {len(named)} --apps unit binaries ran"
                                 f" (not registered in the tree: {' '.join(missing)})"))
                else:
                    rows.append((job, "0", sum(c.time for c in cases),
                                 f"{len(binaries)} of {len(named)} --apps unit binaries passed"
                                 " (the ratchet and the wall-clock budget do not judge a subset)"))
        elif job in ("lint", "docs"):
            c = runs["x64"].get(f"gate.{job}")
            if c is None or c.status != "run":
                why = "did not run" if c is None else (c.message or c.status)
                rows.append((job, "1", c.time if c else 0.0, f"FAILED: gate.{job} ({why})"))
            elif job == "lint":
                line = next((x for x in c.out.splitlines() if x.startswith("lint: clean")), "lint: clean")
                rows.append((job, "0", c.time, line))
            else:
                rows.append((job, "0", c.time, "0 warnings/errors"))
        elif job in ("gates", "imggates"):
            want_art = job == "imggates"
            # Both trees: a gate that reads an arch's compiler or images is
            # registered in each, and an aa64 failure is named as one.
            cases: list[Case] = []
            bad: list[str] = []
            for tree in ("x64", "aa64"):
                mine = [c for n, c in runs[tree].items()
                        if n.startswith("gate.check-") and ("artifact" in c.labels) == want_art]
                cases += mine
                bad += [(f"{tree}:" if tree != "x64" else "") + b
                        for b in failed_names(mine, "gate.")]
            t = sum(c.time for c in cases)
            if not cases:
                rows.append((job, "1", 0.0, "FAILED: no gate ran"))
            elif bad:
                rows.append((job, "1", t, "FAILED: " + " ".join(bad)))
            else:
                what = "image gate(s)" if want_art else "gate(s)"
                rows.append((job, "0", t, f"{len(cases)} {what} clean"))
        elif job == "qloop":
            c = runs["qloop"].get("gate.qloop")
            verdict = ""
            if args.qloop_verdict and Path(args.qloop_verdict).is_file():
                verdict = Path(args.qloop_verdict).read_text().strip().removeprefix("qloop-verdict: ")
            if c is None:
                rows.append((job, "1", 0.0, "FAILED: gate.qloop did not run"))
            elif c.status == "notrun" and c.message == "SKIP_RETURN_CODE=3":
                if verdict:
                    rows.append((job, "UNJ", c.time, verdict))
                    unjudged = f"qloop: {verdict}"
                else:
                    rows.append((job, "1", c.time, "FAILED: gate.qloop was skipped and left no verdict"))
            elif c.status == "run":
                rows.append((job, "0", c.time, verdict or "judged"))
            else:
                rows.append((job, "1", c.time, verdict or f"FAILED: gate.qloop ({c.message or c.status})"))

    # As wide as the longest job name, so every row's RC lines up.
    w = max(len(j) for j in ALL_JOBS)
    print(f"{'JOB':<{w}} {'RC':<4} {'TIME':>6} DETAIL")
    for job, rc, t, detail in rows:
        print(f"{job:<{w}} {rc:<4} {round(t):>5}s {detail}")
    failed = any(rc not in ("0", "UNJ") for _, rc, _, _ in rows)

    if "x64" in jobs and "aa64" in jobs and (ONE_BOOT in runs["x64"] or ONE_BOOT in runs["aa64"]):
        xt = one_boot_total(runs["x64"].get(ONE_BOOT))
        at = one_boot_total(runs["aa64"].get(ONE_BOOT))
        if xt and xt == at:
            print(f"cross-arch: both {xt}  OK")
        else:
            print(f"cross-arch: MISMATCH x64={xt or '?'} aa64={at or '?'}")
            failed = True
    elif "x64" in jobs and "aa64" in jobs:
        xc = counts_of(runs["x64"].get("unit.ratchet"))
        ac = counts_of(runs["aa64"].get("unit.ratchet"))
        xs, as_ = sum(xc.values()), sum(ac.values())
        if xc and xc == ac:
            sub = f" (--apps subset only: {args.apps} -- NOT the full suite)" if args.apps else ""
            print(f"cross-arch: both {xs}  OK{sub}")
        else:
            diff = [f"{k}: x64 {xc.get(k, '-')}, aa64 {ac.get(k, '-')}"
                    for k in sorted(set(xc) | set(ac)) if xc.get(k) != ac.get(k)]
            print(f"cross-arch: MISMATCH x64={xs or '?'} aa64={as_ or '?'}"
                  + (f" ({'; '.join(diff)})" if diff else ""))
            failed = True
    else:
        print("cross-arch: not compared (both arches were not run)")

    notrun = [" ".join(j for j in ALL_JOBS if j not in jobs)] if len(jobs) < len(ALL_JOBS) else []
    if args.apps:
        notrun.append(f"the unit binaries other than {args.apps};"
                      " the ratchet and the wall-clock budget (.last-pass-count NOT written)")
        print(f"apps: PARTIAL -- x64/aa64 ran ONLY {args.apps}, not all {args.known_apps} unit binaries."
              " The ratchet and the wall-clock budget were NOT applied, and .last-pass-count was NOT written.")
    if unjudged_budget:
        notrun.append(f"the x64 unit budget ({unjudged_budget})")
    green = "ALL GREEN" + (f" (partial run -- NOT RUN: {'; '.join(notrun)})" if notrun else "")
    # ctest's OWN status, cross-checked: a test or fixture the table never
    # shows (the temp base's cleanup) can fail with every row green -- the
    # runner fails on it too. A status never collected is not a pass.
    for item in args.ctest_rc:
        run, _, rc = item.partition("=")
        if rc == "?" or not rc.isdigit():
            print(f"ctest: the {run} run left no exit status -- it was not waited for")
            failed = True
        elif rc != "0" and not failed:
            print(f"ctest: the {run} run exited {rc} although every row above passed"
                  f" -- a test or fixture outside the table failed (see {run}-ctest.txt in the logs)")
            failed = True
    if failed:
        print(f"logs: {args.logs}")
        print("SOMETHING FAILED")
        return 1
    if unjudged:
        print(f"logs: {args.logs}")
        print(f"NOT GREEN: UNJUDGED -- {unjudged}. Re-run --only=qloop on a quiet box.")
        return 3
    print(green)
    return 0


def main() -> int:
    """Parse arguments and render."""
    ap = argparse.ArgumentParser(description="render a ctest JUnit file the way the front ends print")
    sub = ap.add_subparsers(dest="mode", required=True)
    r = sub.add_parser("runner", help="the integration runner's lines and headline")
    r.add_argument("--junit", required=True, help="ctest's --output-junit file")
    r.add_argument("--arch", required=True, help="X64 or AARCH64, for the headline")
    r.add_argument("--expect", help="a file of test names the run was asked for")
    o = sub.add_parser("verdict", help="one test's verdict word")
    o.add_argument("--junit", required=True, help="ctest's --output-junit file")
    o.add_argument("--test", required=True, help="the script name, test-foo.sh")
    u = sub.add_parser("unjudged", help="the UNJUDGED tests' script names, one per line")
    u.add_argument("--junit", required=True, help="ctest's --output-junit file")
    v = sub.add_parser("verify", help="verify.sh's table and verdict")
    v.add_argument("--jobs", required=True, help="the jobs run, comma-separated")
    v.add_argument("--x64", help="the x64 tree's JUnit file")
    v.add_argument("--aa64", help="the aa64 tree's JUnit file")
    v.add_argument("--qloop", help="the qloop run's JUnit file")
    v.add_argument("--qloop-verdict", help="this run's qloop-verdict.txt")
    v.add_argument("--apps", default="", help="--apps, as given")
    v.add_argument("--known-apps", default="?", help="how many unit binaries the suite has")
    v.add_argument("--logs", default="", help="the kept log directory")
    v.add_argument("--ctest-rc", action="append", default=[], metavar="RUN=RC",
                   help="a run's ctest exit status (? if never collected); repeatable")
    args = ap.parse_args()
    if args.mode == "runner":
        return runner(args)
    if args.mode == "verify":
        return verify(args)
    if args.mode == "unjudged":
        return unjudged_names(args)
    if args.mode == "verdict":
        return one_verdict(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
