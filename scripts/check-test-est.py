#!/usr/bin/env python3
"""check-test-est.py -- every test CI runs must fit the limit CI runs it under.

CI runs the integration suite as `ctest --preset x64-ci` (through
`scripts/release-suite.sh --ci`). The per-test limit is the preset's
AXL_IT_TIMEOUT, which the CTest include script hands each test as
AXL_TEST_TIMEOUT -- so this gate does not keep a copy of the limit, or of the
selection. It asks CTest: `ctest --preset x64-ci --show-only=json-v1` lists
exactly the tests that invocation would run, each with the limit it would
run under. A second copy is how this gate's predecessor came to grade 900 s
while CI killed at 240 s (test-ssh-hostile-qemu.sh, est=250).

Two halves, both of which must hold:

  1. EVERY SELECTED TEST'S est= FITS ITS LIMIT. The bar is est itself, not a
     multiple of it: est is set from a POOLED run, x1.3 + 2 for headroom, so
     the pool is already inside the number. An est that is not a number cannot
     be said to fit. It grades what a test DECLARES, not what it takes; an est
     that understates its test is invisible here, and the convention is the
     only defence.
  2. CI ACTUALLY RUNS UNDER THAT LIMIT, GRADED. A workflow step that runs the
     suite must call release-suite.sh (or run-integration.sh) with --ci and
     no --timeout, or run `check-test-est.py && ctest --preset x64-ci`. The
     gate must come first because ctest has no run-time refusal of an est its
     limit cannot hold (the runner's --ci had one). release-suite.sh's own
     x64-ci call must be graded the same way: `--ci` there is only as good as
     what it reaches. (No override check on a ctest line: a preset's
     environment wins over the caller's, and every test has a TIMEOUT, which
     ctest's --timeout does not touch.)

An empty answer is not a pass: a selection CTest could not list, or listed as
empty, graded nothing, and a workflow that runs no suite shows nothing about
the limit. Each is a FAILURE, named.

It never configures CMake (it runs beside builds; see ctest_listing): an
unconfigured tree is a FAILURE that says so.

Environment (for the gate's own tests):
  AXL_CI_WORKFLOW_DIR  the workflows to read (default .github/workflows)
  AXL_RELEASE_SUITE    the release-suite script to read
  AXL_EST_IT_DIR       a stub test directory, handed to CTest as AXL_IT_DIR
                       (an AXL_IT_DIR already exported is ignored)
  AXL_EST_BUILD_DIR    the tree to list (default build/x64-debug)

Usage: scripts/check-test-est.py   (run from anywhere)
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import cast

PRESET = "x64-ci"
CONFIGURE_PRESET = "x64-debug"
TIMEOUT_VAR = "AXL_TEST_TIMEOUT"


def fail(msg: str) -> int:
    """Print a FAIL line and return the gate's failing status."""
    print(f"check-test-est: FAIL -- {msg}")
    return 1


def ctest_of(build_dir: Path) -> str | None:
    """The ctest beside the cmake that configured BUILD_DIR, from its cache."""
    try:
        text = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith("CMAKE_CTEST_COMMAND:INTERNAL="):
            return line.split("=", 1)[1] or None
    return None


def ctest_listing(repo: Path, build_dir: Path) -> tuple[list[dict[str, object]] | None, str]:
    """The tests `ctest --preset x64-ci` would run, or None and why not.

    It never configures: this is a lint gate, run beside builds (verify.sh's
    gates job beside its x64 job), and a second configure of one binary
    directory races the first. The listing writes no JUnit file -- the preset
    names one, and inside a running ctest (CI runs this gate's own test under
    that preset) it would replace the outer run's. It does rewrite
    Testing/Temporary/LastTest.log, as every ctest invocation does; nothing
    here relies on that file.
    """
    if not (build_dir / "CTestTestfile.cmake").is_file():
        return None, (f"{build_dir} is not configured: configure it first "
                      f"(`cmake --preset {CONFIGURE_PRESET}`, or any build through "
                      f"scripts/axl-build.sh), then re-run")
    # The ctest that CONFIGURED the tree, never whichever is first on PATH:
    # run by hand from a shell whose PATH has the box's ctest first, that one
    # would read a registration the pinned CMake wrote
    # (scripts/axl-cmake-pin.sh; docs/AXL-Build-System-Design.md §10.13).
    ctest = ctest_of(build_dir)
    if ctest is None:
        return None, (f"{build_dir / 'CMakeCache.txt'} names no CMAKE_CTEST_COMMAND: "
                      f"reconfigure it (scripts/axl-build.sh), then re-run")
    # Only a stub directory the CALLER names for this gate is registered: an
    # AXL_IT_DIR left exported in a shell would grade stubs and read clean.
    env = {k: v for k, v in os.environ.items() if k != "AXL_IT_DIR"}
    if os.environ.get("AXL_EST_IT_DIR"):
        env["AXL_IT_DIR"] = os.environ["AXL_EST_IT_DIR"]
    res = subprocess.run(
        [ctest, "--preset", PRESET, "--test-dir", str(build_dir),
         "--show-only=json-v1", "--output-junit", os.devnull],
        cwd=repo, env=env, capture_output=True, text=True, check=False)
    if res.returncode != 0:
        return None, (f"`ctest --preset {PRESET} --show-only=json-v1` could "
                      f"not list the tests (exit {res.returncode}): "
                      f"{res.stderr.strip()[-300:]}")
    try:
        doc: object = json.loads(res.stdout)
    except json.JSONDecodeError as exc:
        return None, f"ctest's listing is not JSON ({exc})"
    tests = as_list(as_dict(doc).get("tests"))
    if tests is None:
        return None, "ctest's listing has no tests array"
    return [d for d in (as_dict(t) for t in tests) if d], ""


def as_dict(value: object) -> dict[str, object]:
    """A JSON object, or {} for anything else."""
    return cast("dict[str, object]", value) if isinstance(value, dict) else {}


def as_list(value: object) -> list[object] | None:
    """A JSON array, or None for anything else."""
    return cast("list[object]", value) if isinstance(value, list) else None


def prop(test: dict[str, object], name: str) -> object:
    """A CTest property's value, or None."""
    for p in as_list(test.get("properties")) or []:
        d = as_dict(p)
        if d.get("name") == name:
            return d.get("value")
    return None


def inner_limit(test: dict[str, object]) -> int | None:
    """The limit the test itself runs under (its AXL_TEST_TIMEOUT)."""
    for e in as_list(prop(test, "ENVIRONMENT")) or []:
        if isinstance(e, str) and e.startswith(f"{TIMEOUT_VAR}="):
            val = e.split("=", 1)[1]
            return int(val) if val.isdigit() else None
    return None


def declared_ests(repo: Path, scripts: list[Path]) -> dict[Path, str]:
    """Each script's est= as its test-meta header declares it.

    Read by lib/discover.sh's own test_meta_field -- the parser the
    registration used -- in one shell, so this gate cannot disagree with
    discovery about what a header says.
    """
    lib = repo / "test" / "integration" / "lib" / "discover.sh"
    res = subprocess.run(
        ["bash", "-c",
         'source "$1" || exit 2; shift; '
         'for f in "$@"; do printf "%s\\t%s\\n" "$f" "$(test_meta_field "$f" est)"; done',
         "check-test-est", str(lib), *[str(s) for s in scripts]],
        capture_output=True, text=True, check=False)
    if res.returncode != 0:
        raise RuntimeError(f"could not read the test-meta headers "
                           f"(exit {res.returncode}): {res.stderr.strip()[-300:]}")
    out: dict[Path, str] = {}
    for line in res.stdout.splitlines():
        path, _, est = line.partition("\t")
        out[Path(path)] = est
    return out


def joined_lines(path: Path) -> list[tuple[int, str]]:
    """(first line number, text) per logical line: `\\` continuations joined,
    so an override on the next line is still seen; comments dropped."""
    out: list[tuple[int, str]] = []
    buf = ""
    start = 0
    for n, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if not buf:
            start = n
        stripped = raw.rstrip()
        if stripped.endswith("\\"):
            buf += stripped[:-1] + " "
            continue
        line = buf + raw
        buf = ""
        if re.match(r"^\s*#", line):
            continue
        out.append((start, line))
    return out


RUNNER = re.compile(r"(^|\s)\S*(run-integration|release-suite)\.sh(\s|$)")
# ctest has no run-time refusal of an est its limit cannot hold (the runner's
# --ci had one), so a direct call must run this gate first, on the same line,
# joined by `&&` -- `;` or `|| true` would run the suite over a failing gate.
GRADED_CTEST = re.compile(
    rf"(^|[\s;&|(])\S*check-test-est\.py\s*&&\s*ctest\s(.*\s)?--preset[\s=]{re.escape(PRESET)}(\s|$)")
CTEST_PRESET = re.compile(r"(^|[\s;&|(])ctest\s.*--preset[\s=]")
OVERRIDE = re.compile(r"(^|\s)(--timeout([\s=]|$)|AXL_IT_TIMEOUT=)")
CI_FLAG = re.compile(r"(^|\s)--ci(\s|$)")
CI_PRESET = re.compile(rf"--preset[\s=]{re.escape(PRESET)}(\s|$)")


def workflow_problems(wf_dir: Path) -> tuple[list[str], int]:
    """Problems with how the workflows run the suite, and how many runs."""
    problems: list[str] = []
    runs = 0
    files = sorted([*wf_dir.glob("*.yml"), *wf_dir.glob("*.yaml")])
    for f in files:
        for n, line in joined_lines(f):
            cmd = line.strip()
            loc = f"{f.name}:{n}"
            if RUNNER.search(line):
                runs += 1
                if OVERRIDE.search(line):
                    problems.append(f"{loc} overrides the limit: {cmd}")
                elif not CI_FLAG.search(line):
                    problems.append(f"{loc} runs the suite without --ci, so not under the x64-ci limit: {cmd}")
            elif CTEST_PRESET.search(line):
                # No override check here: a preset's environment wins over the
                # caller's, and every test has a TIMEOUT, which --timeout does
                # not touch.
                runs += 1
                if not CI_PRESET.search(line):
                    problems.append(f"{loc} runs ctest with a preset other than {PRESET}: {cmd}")
                elif not GRADED_CTEST.search(line):
                    problems.append(f"{loc} runs ctest --preset {PRESET} without "
                                    f"scripts/check-test-est.py before it: {cmd}")
    if runs == 0:
        problems.append(f"no workflow in {wf_dir} runs the suite (release-suite.sh --ci, "
                        f"or check-test-est.py && ctest --preset {PRESET}), so nothing shows "
                        f"CI uses its limit")
    return problems, runs


def release_suite_problems(path: Path) -> list[str]:
    """release-suite.sh --ci must reach the x64-ci preset, graded first."""
    if not path.is_file():
        return [f"{path} does not exist, so nothing shows what --ci runs"]
    problems: list[str] = []
    ci_runs = 0
    for n, line in joined_lines(path):
        if not re.search(r"(^|[\s;&|(])ctest(\s|$)", line):
            continue
        if not CI_PRESET.search(line):
            continue
        ci_runs += 1
        if not GRADED_CTEST.search(line):
            problems.append(f"{path.name}:{n} runs ctest --preset {PRESET} without "
                            f"scripts/check-test-est.py before it: {line.strip()}")
    if ci_runs == 0:
        problems.append(f"{path.name} never runs ctest --preset {PRESET}, so --ci does not reach CI's limit")
    return problems


def main() -> int:
    """Run the gate."""
    repo = Path(__file__).resolve().parent.parent
    wf_dir = Path(os.environ.get("AXL_CI_WORKFLOW_DIR") or repo / ".github" / "workflows")
    rs_path = Path(os.environ.get("AXL_RELEASE_SUITE") or repo / "scripts" / "release-suite.sh")

    build_dir = Path(os.environ.get("AXL_EST_BUILD_DIR") or repo / "build" / CONFIGURE_PRESET)
    tests, why = ctest_listing(repo, build_dir)
    if tests is None:
        return fail(why)
    broken = [str(t.get("name")) for t in tests
              if t.get("name") in ("integration.bad-timeout", "integration.discovery-failed")]
    if broken:
        return fail(f"the registration itself is broken: {', '.join(broken)} -- run "
                    f"`ctest --preset {PRESET} -R '{broken[0]}' -V` for why")

    selected: list[tuple[str, Path, int | None]] = []
    for t in tests:
        if "integration" not in (as_list(prop(t, "LABELS")) or []):
            continue
        cmd = as_list(t.get("command")) or []
        script = cmd[-1] if cmd else None
        if not isinstance(script, str):
            return fail(f"{t.get('name')} has no command to read a script from")
        selected.append((str(t.get("name")), Path(script), inner_limit(t)))
    if not selected:
        return fail(f"the {PRESET} selection is EMPTY, so nothing was graded")

    try:
        ests = declared_ests(repo, [s for _, s, _ in selected])
    except RuntimeError as exc:
        return fail(str(exc))

    offenders: list[str] = []
    limits: set[int] = set()
    largest = (-1, "")
    for name, script, limit in selected:
        base = script.name
        if limit is None:
            offenders.append(f"{base}: CTest gives it no {TIMEOUT_VAR} ({name})")
            continue
        limits.add(limit)
        est = ests.get(script, "")
        if not est.isdigit():
            offenders.append(f"{base}: est={est} is not a number")
        elif int(est) > limit:
            offenders.append(f"{base}: est={est}s > {limit}s")
        elif int(est) > largest[0]:
            largest = (int(est), base)

    wf_probs, runs = workflow_problems(wf_dir)
    rs_probs = release_suite_problems(rs_path)
    lim = "/".join(f"{x}s" for x in sorted(limits)) or "?"
    status = 0
    if offenders:
        status = fail(f"{len(offenders)} test(s) declare an est= the {lim} per-test limit cannot hold:")
        for o in offenders:
            print(f"    {o}")
        print("  Split the test, or measure it and correct its est (pooled time x1.3 + 2).")
        print(f"  Raising the limit is a CI decision: AXL_IT_TIMEOUT in the {PRESET} test preset (CMakePresets.json).")
    if wf_probs or rs_probs:
        status = fail("CI does not run the suite under the graded limit:")
        for p in [*wf_probs, *rs_probs]:
            print(f"    {p}")
    if status == 0:
        print(f"check-test-est: clean -- {len(selected)} test(s) ({PRESET}) fit the {lim} per-test "
              f"limit; largest est={largest[0]}s ({largest[1]}); {runs} workflow invocation(s) take it")
    return status


if __name__ == "__main__":
    sys.exit(main())
