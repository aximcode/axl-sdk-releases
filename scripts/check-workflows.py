#!/usr/bin/env python3
"""check-workflows.py -- the GitHub workflow files, held to five rules.

  1. EACH FILE PARSES UNAMBIGUOUSLY. A duplicate key is not a YAML syntax
     error: the last one wins and `yaml.safe_load` says nothing. That
     happened here -- gating jobs on `plan` added a second `needs:` to two
     jobs that already had one, and the integration job silently stopped
     depending on the build.

  2. EVERY JOB HAS A `timeout-minutes`, a positive integer. GitHub's default
     is six hours, and the CI runner has ONE slot: on 2026-11-09 the
     clang-tidy job's apt-get sat silent against a dead Ubuntu mirror for 19
     minutes, holding that slot, until it was cancelled by hand. A job added
     later without a bound would reopen that, so the rule is enforced rather
     than remembered.

  3. EVERY `apt-get`/`apt` update, install or upgrade IN A `run:` BLOCK IS
     BOUNDED AS A WHOLE: wrapped in the step's `apt_wall` function, in a step
     that defines `apt_wall` and writes the per-fetch bounds (99-axl-retry)
     before the first call. Every `update` carries `--error-on=any`. And
     every `apt_wall` body in every workflow is the SAME text and contains
     `timeout -k` -- the steps that install before checkout cannot call a
     shared script, so the function is pasted into each, and a copy that
     drifted (or a wrapper that bounds nothing) would otherwise pass on its
     name alone. The per-fetch bounds alone were in place on 2026-11-09 and
     did not stop the hang: apt's Timeout is per read, so a trickling mirror
     never trips it, and Retries multiplies across packages. Without
     `--error-on=any`, an `update` whose every index failed exits 0 and the
     outage reads as "Unable to locate package".

  4. A JOB THAT RUNS `release-suite.sh` KEEPS ITS PER-TEST LOGS WHEN IT DOES
     NOT SUCCEED: a LATER `actions/upload-artifact` step whose `if:` is one of
     the allowed forms (ALLOWED_LOG_IFS -- `failure() || cancelled()`
     included, because a suite that hangs to the job's timeout-minutes ends
     CANCELLED, which `failure()` alone misses) and whose `path` names
     `build/x64-debug/integration-logs` itself (`./`, a trailing `/` and
     `/**` are normalised away). A PARENT of it -- `build/x64-debug` as much
     as `build/` -- is a named finding: it uploads a whole build tree,
     hundreds of MB, for ~2 MB of logs. The JUnit carries only run-one's 25-line
     excerpt per test, and CTest's ATTACHED_FILES_ON_FAIL reaches only a
     dashboard's Test.xml, never --output-junit (measured, CMake 4.2.8 and
     4.3.6) -- so without this a CI-only failure leaves nothing to read but
     `gh run view --log-failed`.
     "Runs the suite" is a real command line (shell_lines drops comments)
     naming `release-suite.sh` behind any path, or none: `./scripts/`,
     `"$GITHUB_WORKSPACE/scripts/`, `$ROOT/scripts/`, inside `bash -c "..."`,
     and `cd scripts && ./release-suite.sh` -- the directory a `cd` left is
     not knowable from the text, and the name is unique in the tree, so the
     file name is what is matched. That over-matches a line that only echoes
     the name, which is the safe direction: a false suite job is a loud
     finding, a missed one is silence. `--step X` is exempt unless X is
     `integration`, which is the step that writes the logs.

  5. A JOB IN A `container:` TRUSTS ITS CHECKOUT BEFORE IT RUNS ANYTHING OVER
     IT. The container runs as root; the mounted checkout belongs to the
     runner's uid, and git refuses a repository another user owns (`fatal:
     detected dubious ownership`, exit 128). actions/checkout scopes its own
     safe.directory to its own git calls, so nothing after it inherits that.
     This bit TWICE: check-nul in the lint job, then make-release-assets.sh
     in the build job (2026-11-10, both legs red) -- each a script deep in a
     step that nobody reads as "runs git". Which steps shell out to git is not
     decidable from the workflow, so the rule does not try: in a container
     job with an `actions/checkout` step, every `run:` step AFTER the checkout
     must already be trusted -- by an earlier step (before the checkout too:
     `--global` config outlives it) whose run holds the trust line anywhere,
     or by that step's own FIRST command. The one exception is a step that
     only installs packages (`is_package_install`, INSTALL_LINE: an apt
     call, and nothing but the apt_wall definition, apt_wall calls of plain
     words naming an apt subcommand, pip installs of pinned `name==version`
     only, `export DEBIAN_FRONTEND=noninteractive` and a printf of
     single-quoted text into 99-axl-retry -- each a shape that can run
     nothing from the checkout): it is where git itself is installed, as
     docs.yml does after its checkout, so it cannot trust first. A step with an `if:` may
     not run, so it trusts nothing. The trust line is exactly
     `git config --global --add safe.directory` with `$GITHUB_WORKSPACE`
     (bare or double-quoted, braced or not -- single quotes would hand git the
     literal text), `${{ github.workspace }}` or `*`; another directory is
     not the checkout. `uses:` steps are not held to it (none of ours runs
     git over the checkout). A checkout into a `path:` other than the
     workspace is a finding: it needs its own directory trusted, which this
     rule does not check. A container job is COUNTED only when it checks out
     and holds a trust line; one with no checkout, or with nothing run after
     its checkout and no trust, is neither flagged nor counted.

Prints one line per finding and exits 1, or prints what it checked and exits
0. The counts are part of the success line so a caller can tell "checked N
jobs and found nothing" from "found no jobs to check" -- the suite-job and
container-job counts included, so rule 4 or 5 checking nothing (the suite
renamed, the containers dropped) cannot read clean.

Usage: check-workflows.py [WORKFLOW.yml ...]   (default .github/workflows/*.yml)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import cast

import yaml  # pyright: ignore[reportMissingTypeStubs]

APT_COMMANDS = ("apt-get", "apt")
# Rule 4: where CI's suite (no AXL_IT_LOGDIR) writes one log per test.
SUITE_LOGS = "build/x64-debug/integration-logs"
# Rule 4: the `if:` forms that upload when the job did not succeed, compared
# after `${{ }}`, quotes and spaces are stripped. An allow-list, not a
# substring test: `!failure()` contains `failure()` and uploads on success only.
ALLOWED_LOG_IFS = ("failure()", "failure()||cancelled()", "cancelled()||failure()",
                   "always()", "!success()")
# Rule 4: an invocation of the suite -- the file name behind any path (or
# none), after a word boundary or a quote, before one or the end.
SUITE_RUN = re.compile(r"""(?:^|[\s;&|()<>`"'])(?:[^\s;&|()<>`"']*/)?release-suite\.sh(?=[\s;&|()<>`"']|$)""")
# Rule 4: the --step values a suite command names; `integration` writes the logs.
SUITE_STEP = re.compile(r"""--step(?:=|\s+)["']?([A-Za-z0-9_-]+)""")
# Rule 4: where the suite's own command ends on its line.
COMMAND_END = re.compile(r"[;&|`]")
# Rule 5: the line that makes git trust the workspace (shell_lines has
# collapsed its whitespace). The shell variable bare or double-quoted only:
# single quotes would hand git the literal text `$GITHUB_WORKSPACE`.
TRUST_LINE = re.compile(
    r"""git config --global --add safe\.directory """
    r"""(?:("?)\$(?:GITHUB_WORKSPACE|\{GITHUB_WORKSPACE\})\1"""
    r"""|(["']?)(?:\$\{\{ ?github\.workspace ?\}\}|\*)\2)""")
# Rule 5: the command lines a package-install step may hold besides the
# apt_wall definition, each in a shape that can run nothing from the checkout:
# an apt_wall call of plain words (and is_package_install also requires an
# apt subcommand on it), a pip install of options and pinned `name==version`
# arguments only (no -r, -e, `.` or path), DEBIAN_FRONTEND, and the printf of
# single-quoted text into 99-axl-retry.
INSTALL_LINE = re.compile(
    r"apt_wall [A-Za-z0-9 _.=+:-]+"
    r"|pip3? install(?: --[a-z-]+| [A-Za-z0-9_.-]+==[A-Za-z0-9_.+-]+)+"
    r"|export DEBIAN_FRONTEND=noninteractive"
    r"|printf(?: '[^']*')+ > ?/etc/apt/apt\.conf\.d/99-axl-retry")
APT_SUBCOMMANDS = ("update", "install", "upgrade", "dist-upgrade", "full-upgrade")
# apt-get options that take a separate argument (`-o X=Y`, `-c FILE`, ...).
APT_OPTS_WITH_ARG = ("-o", "-c", "-t", "--option", "--config-file", "--target-release")


class DuplicateKey(Exception):
    """A mapping in the file repeats a key."""


class NoDupLoader(yaml.SafeLoader):  # pyright: ignore[reportUntypedBaseClass]
    """SafeLoader that refuses a repeated mapping key."""


def _construct_mapping(loader: NoDupLoader, node: yaml.MappingNode, deep: bool = False) -> dict[object, object]:
    seen: dict[object, object] = {}
    pairs = cast("list[tuple[yaml.Node, yaml.Node]]", node.value)
    for key_node, value_node in pairs:
        key: object = loader.construct_object(key_node, deep=deep)  # pyright: ignore[reportUnknownMemberType, reportUnknownVariableType]
        if key in seen:
            raise DuplicateKey(f"duplicate key {key!r}")
        seen[key] = loader.construct_object(value_node, deep=deep)  # pyright: ignore[reportUnknownMemberType]
    return seen


NoDupLoader.add_constructor(  # pyright: ignore[reportUnknownMemberType]
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_mapping
)


def as_map(value: object) -> dict[object, object] | None:
    """`value` as a YAML mapping, or None if it is not one."""
    return cast("dict[object, object]", value) if isinstance(value, dict) else None


def as_list(value: object) -> list[object]:
    """`value` as a YAML sequence; anything else is an empty one."""
    return cast("list[object]", value) if isinstance(value, list) else []


def shell_lines(script: str) -> list[str]:
    """The commands of a run: block: continuation lines joined, whitespace runs
    collapsed (so a finding quotes a readable line), comments dropped."""
    joined = re.sub(r"\\\n", " ", script)
    lines: list[str] = []
    for raw in joined.splitlines():
        line = " ".join(raw.split())
        if line and not line.startswith("#"):
            lines.append(line)
    return lines


def apt_subcommand(line: str) -> str | None:
    """The apt subcommand a command line runs (`update`, `install`, ...), or
    None. The subcommand is the first word after `apt-get`/`apt` that is not
    an option or an option's argument -- so a package named `update-notifier`
    or an `-o Foo=update` is not mistaken for one."""
    # Every apt token on the line is examined, not just the first: in
    # `command -v apt-get >/dev/null && apt-get install x` the first is a probe
    # and the second is the install.
    words = re.sub(r"(&&|\|\||[;|&])", r" \1 ", line).split()
    for i, word in enumerate(words):
        if word not in APT_COMMANDS:
            continue
        skip = False
        for arg in words[i + 1:]:
            if skip:
                skip = False
            elif arg in APT_OPTS_WITH_ARG:
                skip = True
            elif arg.startswith("-"):
                continue
            else:
                if arg in APT_SUBCOMMANDS:
                    return arg
                break
    return None


def apt_wall_body(lines: list[str]) -> str | None:
    """The text of the step's `apt_wall() { ... }` definition, or None."""
    for i, line in enumerate(lines):
        if line.startswith("apt_wall()"):
            body: list[str] = []
            for inner in lines[i:]:
                body.append(inner)
                if inner == "}":
                    return "\n".join(body)
            return "\n".join(body)
    return None


def check_step(where: str, step: dict[object, object], findings: list[str],
               bodies: dict[str, str]) -> int:
    """Rule 3 for one step; returns how many apt calls it holds. Records the
    step's apt_wall body in `bodies` (where -> text)."""
    run = step.get("run")
    if not isinstance(run, str):
        return 0
    lines = shell_lines(run)
    calls = [line for line in lines if apt_subcommand(line) is not None]
    if not calls:
        return 0
    first = lines.index(calls[0])
    body = apt_wall_body(lines[:first])
    if body is None:
        findings.append(f"{where}: calls apt-get without defining apt_wall first")
    else:
        bodies[where] = body
        if "timeout -k " not in body:
            findings.append(f"{where}: apt_wall does not run its command under `timeout -k`")
    if not any("99-axl-retry" in line for line in lines[:first]):
        findings.append(f"{where}: calls apt-get without writing 99-axl-retry first")
    for line in calls:
        if not line.startswith("apt_wall "):
            findings.append(f"{where}: apt-get not wrapped in apt_wall: {line}")
        if apt_subcommand(line) == "update" and "--error-on=any" not in line.split():
            findings.append(f"{where}: apt-get update without --error-on=any: {line}")
    return len(calls)


def is_package_install(lines: list[str]) -> bool:
    """Rule 5: a step that only installs packages -- at least one apt call, and
    nothing but the apt_wall definition and INSTALL_LINE commands. It is where
    git itself gets installed, so it cannot trust before it runs."""
    if not any(apt_subcommand(line) is not None for line in lines):
        return False
    # The apt_wall() body is not read here: rule 3's check_bodies_agree holds
    # every copy to the same text, so a script added to one is a finding there.
    in_body = False
    for line in lines:
        if in_body:
            in_body = line != "}"
        elif line.startswith("apt_wall()"):
            in_body = not line.endswith("}")
        elif not INSTALL_LINE.fullmatch(line):
            return False
        elif line.startswith("apt_wall ") and apt_subcommand(line) is None:
            return False
    return True


def check_trust(where: str, job: dict[object, object], findings: list[str]) -> bool:
    """Rule 5 for one job: True when it is a container job over a checkout that
    trusts the checkout in time; False when it is not one, has nothing to trust,
    or a finding was recorded."""
    if not job.get("container"):
        return False
    checked_out = False
    trusted = False
    for i, step_value in enumerate(as_list(job.get("steps"))):
        step = as_map(step_value)
        if step is None:
            continue
        if str(step.get("uses") or "").startswith("actions/checkout@"):
            target = str((as_map(step.get("with")) or {}).get("path") or "").strip()
            if target not in ("", ".", "./", "$GITHUB_WORKSPACE", "${{ github.workspace }}"):
                findings.append(f"{where}: checks out into path {target!r} in a container; rule 5 trusts "
                                "only the workspace -- check out into the workspace")
                return False
            checked_out = True
            continue
        run = step.get("run")
        lines = shell_lines(run) if isinstance(run, str) else []
        if not lines or trusted:
            continue
        # A step that may not run (`if:`) trusts nothing.
        unconditional = step.get("if") is None
        if not checked_out:
            trusted = unconditional and any(TRUST_LINE.fullmatch(line) for line in lines)
        elif unconditional and TRUST_LINE.fullmatch(lines[0]):
            trusted = True
        elif not is_package_install(lines):
            name = step.get("name") or f"#{i + 1}"
            findings.append(f"{where} step {name!r}: runs after actions/checkout, in a container, before "
                            "git trusts the checkout (git config --global --add safe.directory "
                            "\"$GITHUB_WORKSPACE\")")
            return False
    return checked_out and trusted


def check_file(path: Path, findings: list[str], bodies: dict[str, str],
               counts: dict[str, int]) -> tuple[int, int]:
    """Rules 1-5 for one workflow; returns (jobs, apt calls) seen, and counts
    the suite jobs that keep logs and the container jobs that trust their
    checkout into `counts`."""
    try:
        doc: object = yaml.load(path.read_text(), NoDupLoader)  # pyright: ignore[reportUnknownMemberType]
    except DuplicateKey as exc:
        findings.append(f"{path}: {exc}")
        return 0, 0
    except yaml.YAMLError as exc:
        first = str(exc).splitlines()[0] if str(exc) else type(exc).__name__
        findings.append(f"{path}: does not parse: {first}")
        return 0, 0
    top = as_map(doc)
    jobs = as_map(top.get("jobs")) if top is not None else None
    if not jobs:
        findings.append(f"{path}: no jobs found")
        return 0, 0
    n_apt = 0
    for job_id, job_value in jobs.items():
        where = f"{path}: job {job_id}"
        job = as_map(job_value)
        if job is None:
            findings.append(f"{where}: is not a mapping")
            continue
        limit = job.get("timeout-minutes")
        if limit is None:
            findings.append(f"{where}: has no timeout-minutes (GitHub's default is 6 hours)")
        elif isinstance(limit, bool) or not isinstance(limit, int) or limit <= 0:
            findings.append(f"{where}: timeout-minutes is {limit!r}, not a positive integer")
        runs_suite = False
        keeps_logs = False
        for i, step_value in enumerate(as_list(job.get("steps"))):
            step = as_map(step_value)
            if step is not None:
                name = step.get("name") or f"#{i + 1}"
                n_apt += check_step(f"{where} step {name!r}", step, findings, bodies)
                if runs_release_suite(step):
                    runs_suite = True
                    keeps_logs = False      # only an upload AFTER the suite counts
                elif runs_suite:
                    uploads, too_broad = uploads_logs_on_failure(step)
                    keeps_logs = keeps_logs or uploads
                    for raw in too_broad:
                        findings.append(f"{where} step {name!r}: uploads {raw!r}, a parent of {SUITE_LOGS} "
                                        f"-- a whole build tree (hundreds of MB), not the logs; name {SUITE_LOGS}")
        if runs_suite:
            if keeps_logs:
                counts["suite"] += 1
            else:
                findings.append(f"{where}: runs release-suite.sh but uploads no {SUITE_LOGS} after it "
                                "on failure (if: failure() || cancelled(), or always())")
        if check_trust(where, job, findings):
            counts["container"] += 1
    return len(jobs), n_apt


def runs_release_suite(step: dict[object, object]) -> bool:
    """Rule 4: the step runs the suite -- a real command line, not a comment
    (shell_lines drops those), and not a `--step` of it other than the
    integration step."""
    for line in shell_lines(str(step.get("run") or "")):
        for match in SUITE_RUN.finditer(line):
            # Only the suite's OWN arguments: another command's --step on the
            # same line exempts nothing.
            args = COMMAND_END.split(line[match.end():], maxsplit=1)[0]
            steps = SUITE_STEP.findall(args)
            if not steps or "integration" in steps:
                return True
    return False


def uploads_logs_on_failure(step: dict[object, object]) -> tuple[bool, list[str]]:
    """Rule 4: does this upload-artifact step run on failure and name the
    suite's logs? Also returns the paths it names that are PARENTS of the logs
    (as written): each uploads a whole build tree, not the logs."""
    if not str(step.get("uses") or "").startswith("actions/upload-artifact@"):
        return False, []
    cond = "".join(str(step.get("if") or "").split()).strip("'\"")
    if cond.startswith("${{") and cond.endswith("}}"):
        cond = cond[3:-2]
    if cond not in ALLOWED_LOG_IFS:
        return False, []
    with_map = as_map(step.get("with")) or {}
    uploads = False
    too_broad: list[str] = []
    for raw in str(with_map.get("path") or "").splitlines():
        path = raw.strip()
        path = path.removeprefix("./")
        path = path.removesuffix("/**").rstrip("/")
        if not path:
            continue
        if path == SUITE_LOGS:
            uploads = True
        elif SUITE_LOGS.startswith(path + "/"):
            too_broad.append(raw.strip())
    return uploads, too_broad


def check_bodies_agree(bodies: dict[str, str], findings: list[str]) -> None:
    """Every apt_wall copy must be the same text as the first one seen."""
    if not bodies:
        return
    first_where, reference = next(iter(bodies.items()))
    for where, body in bodies.items():
        if body != reference:
            findings.append(f"{where}: apt_wall differs from the copy in {first_where}")


def main(argv: list[str]) -> int:
    """Check the named workflows, or every one in .github/workflows."""
    if argv:
        paths = [Path(a) for a in argv]
    else:
        root = Path(__file__).resolve().parent.parent
        wf_dir = root / ".github" / "workflows"
        paths = sorted([*wf_dir.glob("*.yml"), *wf_dir.glob("*.yaml")])
    if not paths:
        print("check-workflows: no workflow files found")
        return 1
    findings: list[str] = []
    bodies: dict[str, str] = {}
    n_jobs = 0
    n_apt = 0
    counts = {"suite": 0, "container": 0}
    for path in paths:
        jobs, apt = check_file(path, findings, bodies, counts)
        n_jobs += jobs
        n_apt += apt
    check_bodies_agree(bodies, findings)
    for finding in findings:
        print(finding)
    if findings:
        return 1
    print(f"check-workflows: {len(paths)} workflows, {n_jobs} jobs bounded, {n_apt} apt-get calls bounded, "
          f"{counts['suite']} suite jobs keep logs, {counts['container']} container jobs trust the checkout")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
