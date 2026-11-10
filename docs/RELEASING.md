# Releasing AXL

Step-by-step for cutting an AXL release. The flow is opinionated and
the order matters — running it out of order, or skipping the version
helper, has burned us before.

## TL;DR — the fast path

```sh
scripts/cut-release.sh X.Y.Z            # do it (prompts before pushing)
scripts/cut-release.sh X.Y.Z --dry-run  # preview, change nothing
scripts/cut-release.sh X.Y.Z --yes      # skip the prompt (non-interactive)
scripts/cut-release.sh X.Y.Z --watch    # wait for Release+Docs (default: hand back a URL)
```

A **test build** for someone to try on real hardware is not a release: see
[Test releases](#test-releases-no-ci) (`scripts/make-test-release.sh`).

**It stops and asks before it pushes anything.** After printing the commit
list it prompts `Cut and publish vX.Y.Z? [y/N]` and reads stdin. That is right
for a human at a terminal and wrong everywhere else: run from a script, a CI
step, an agent, or anything without a TTY, `read` sees EOF, the answer defaults
to **no**, and the script aborts having done nothing — correctly, and with exit
status 1. Pass `--yes` when there is nobody to answer.

> Check what actually happened before believing either outcome, and check the
> **script's** exit status rather than your shell's. `cut-release.sh X.Y.Z; echo $?`
> reports the script; `cut-release.sh X.Y.Z > log 2>&1; tail log` reports
> `tail`, which succeeds whether the release happened or not. The three facts
> that settle it, and none of them can be faked by a stray zero:
>
> ```sh
> cat VERSION                       # bumped?
> git tag -l vX.Y.Z                 # tag exists?
> git log --oneline @{u}..HEAD | wc -l   # 0 == main was pushed
> ```

`scripts/cut-release.sh` automates the cut: it bumps the version, dates the
CHANGELOG, commits + pushes `main`, tags, and watches Release/Docs to the
published release. By default it does **not** wait on CI (see the gate below).

**But first decide WHERE you are cutting from.** `cut-release.sh` releases the
tip of `main` and everything under `## Unreleased`. If `main` carries work that
does not belong in this release, that is the wrong flow and the script cannot
do the right one — see [Which flow](#which-flow-from-main-or-from-a-tag)
immediately below. Three of the last three releases used the other one.

### The gate: a dispatched CI suite, or the local release suite plus the push CI

**Release tags do NOT trigger CI.** (A tag used to re-run CI on the exact
commit already validated on `main` — pure duplication, ~38 min for an identical
result — so that trigger was removed.) Since 2026-10-21 a push to `main` runs
only the **cheap** jobs (`plan`, `gcc x64`, `gcc aa64`, `clang-tidy`); the
**QEMU suite runs only on `workflow_dispatch`**. The trigger table below is
authoritative; `AXL-CI-Release-Speed-Design.md` §14 records the decision and
its residual risk.

**Either of these certifies a release**, and `cut-release.sh` checks both
before it changes anything — and **refuses** when neither does:

1. **A dispatched CI run** whose `QEMU integration (full suite)` job — and the
   three cheap jobs — went green on the release commit or its release-only
   parent. Dispatch it and wait (`gh workflow run ci.yml --ref main`, or
   `cut-release.sh --ci-gate`, which does it for you). A push run can never
   satisfy this: it lists the suite job as `skipped`, and the gate names the
   job rather than trusting the run's `success`.
2. **A local `scripts/release-suite.sh` run** on a clean tree at the commit
   being released, **plus** a green push run of the three cheap jobs on that
   same tree. `release-suite.sh` is the SAME list of steps CI's integration job
   runs — unit tests in QEMU, the build, `install.sh --arch all --cpp`,
   `run-integration.sh` (uncached, local-only tests included) and the harness
   self-tests — and only its complete success writes the sha-bound
   `.last-run-stamp` the gate reads. A bare `run-integration.sh --no-cache` no
   longer certifies a release: it ran less than the CI job it stood in for.
   The local run still cannot see a clean checkout that does not link — a dev
   box always has the objects that hide it — and the push run's `build` job
   can; that is the 2026-08-18 class (design §11.2), and why the cheap jobs
   stayed on push.

*What counts as green is deliberately narrow:* named jobs, each `success`. A
run-level conclusion is not enough — the `release: vX.Y.Z` commit carries a
`[release-cut]` marker that makes every `ci.yml` job skip, and every push run
skips the suite, so runs exist that report success having tested nothing. The
parent rule below then carries the parent's green across the version bump.
When `gh` is missing or failing, the gate says it **could not check** — it
refuses, but it does not tell you to push something.

**The ancestor walk exists because of the plan job.** A docs-only push skips
every job, so after one there is no green run at the tip — and without this the
gate would refuse a tree prose cannot have changed. So it walks back (bounded,
`AXL_CI_WALK_MAX=25`) and accepts the first green ancestor whose *entire* diff
to the release commit is prose, the version bump, or both — for the dispatched
suite and for the cheap push run alike. "Prose" is `scripts/ci-plan.sh`'s own
allowlist, asked via `--list-unsafe`, not a second copy of it — and a deletion
is never prose.

**The local path** — when you would rather not wait for a dispatch, or the
runner is busy. Commit, push (so the cheap push CI runs on the same commit),
then:

```sh
scripts/release-suite.sh    # every step CI's integration job runs; stamps on success
scripts/lint.sh             # clang-tidy exactly as CI runs it
```

It refuses a tree with tracked changes (a stamp names a commit, not a working
tree), stops at the first failing step, and writes no stamp unless every step
passed and the tree is still that commit at the end (when it is not, it prints
`git status --short` so the message names the file). A run retires any earlier
stamp for the same commit as it starts. The unit tests' ratchet is handed a
temp copy of the committed `test/integration/.last-pass-count`, so the bar is
the committed one and a green run never dirties the tree; when the count rose
it prints a NOTE with old -> new — commit the new baseline.
`scripts/release-suite.sh --step <name>` runs one step for debugging and never
stamps.

**Do not pass `-j"$(nproc)"` here.** That was this command's advice until
2026-09-02, and it contradicts the runner's own default of `nproc - 2`, which
exists because these are timing-sensitive QEMU guests and a saturated host
starves them. Measured on the 4.5.0 gate, on 8 cores:

| workers | wall | result |
|---|---|---|
| `-j8` (`nproc`) | 474s | **FAILED** — `test-task-pool-mp-qemu.sh`, `slot reclaim: workers=2 expected=3` |
| default (`nproc-2`) | 477s | 188 passed, 0 failed |

**Three seconds, and a false red.** The MP test needs three vCPUs actually
scheduled; at `-j8` it does not get them, and it failed twice — the harness's
own retry does not help when the starvation is sustained. A release gate that
cries wolf is worse than a slower one, because the next red gets triaged as
"probably contention" and one day it will not be.

**UNJUDGED: a timing test the box starved certifies nothing.**
`test-net-latency-qemu.sh` times round trips against bounds (p90 under 5 ms),
and on a shared box it missed them at load 18+ with nothing wrong in the code
-- once in a dispatched CI run (5.136 ms against 5). It now carries qloop's
witness (how long the guest's QEMU waited for a CPU over each round-trip
window, `AXL_STARVE_MAX_MS` = 50 ms): a run whose failures are all timing
misses in starved windows exits 77 with an `UNJUDGED:` line. To ctest that is
a skip, which passes a ctest run -- so `release-suite.sh`'s integration step
reads the JUnit report afterwards and re-runs each UNJUDGED test ALONE (the
quiet slot, as verify's qloop runs last and alone --
docs/AXL-Build-System-Design.md §10.14), under the same preset:

| alone, it reads | the step |
|---|---|
| PASS | passes -- the bound was judged, on a quiet box |
| FAIL, or a plain SKIP (it judged nothing) | fails |
| UNJUDGED again | **exit 3, no stamp**: `release-suite: UNJUDGED at step 4/5 ... Re-run on a quiet box.` |

Decided 2026-11-09, and the same in `--ci`: the dispatched suite is the only
place CI runs the integration tests, and a green one IS a release certificate
(`release-gate.sh` accepts it), so a green run over a bound nobody judged would
tag code on it. The observed misses each passed when re-run alone, which is
what the quiet re-run does; a dispatched run goes red for load only when even
a lone run of the test is starved, and then it says UNJUDGED, not FAIL. The
alternative -- CI green, and the gate reading the run's JUnit artifact to
refuse it -- was not built: a second channel into the gate for one rare case.
Outside a release run it is never a FAIL: `run-integration.sh` names it,
counts it apart from passes (`N UNJUDGED`) and exits 3 -- not 0, so a script
chaining on success still stops, and not 1.

`run-integration.sh` discovers every `test-*.sh` (including the patched-QEMU /
real-pointer `local-only` tests, which your dev box CAN run) and runs each in
its own QEMU. A green `release-suite.sh`, with the cheap push CI green on the
same tree, is the release gate. Then:

```sh
scripts/cut-release.sh X.Y.Z               # cut; REFUSES if nothing certifies HEAD
scripts/cut-release.sh X.Y.Z --dry-run     # preview, change nothing (says if it would refuse)
scripts/cut-release.sh X.Y.Z --ci-gate     # nothing covers it yet: dispatch the suite and wait
scripts/cut-release.sh X.Y.Z --force-ci    # --ci-gate, and dispatch even if something covers it
scripts/cut-release.sh X.Y.Z --tag-untested  # tag with NOTHING certifying it -- loud, and in the notes
scripts/cut-release.sh X.Y.Z --watch    # wait for Release+Docs instead of a URL
```

**The gate is enforced, before anything changes.** `cut-release.sh` first
waits for the push run of the commit it is about to release (and refuses if
that run did not succeed), then decides — before the version bump, so a
refusal strands no release commit on the branch. With nothing certifying the
commit it prints both remedies and exits non-zero. `--ci-gate` proceeds and
dispatches the suite after pushing the release commit; `--tag-untested`
proceeds with a WARNING and writes a "Released untested" line into the
release's CHANGELOG section (which `release.yml` publishes as the release
notes) and the tag message; the cut dies if that line did not land, and a test
runs `release.yml`'s own notes builder (`scripts/release-notes.sh`, over
`scripts/changelog-section.sh`) over it. That builder also holds the body under
GitHub's 125,000-character cap: a longer section (5.0.0's was 228 KB) is cut
between entries, keeping the first, with a note linking the full CHANGELOG.md.
`--resume` goes through the same gate, but refuses `--tag-untested`: the
release commit is already pushed, so the note could not reach the notes.
Certify the commit instead (`--resume --ci-gate`, or `scripts/release-suite.sh`
then `--resume`).

**`--ci-gate` does not always dispatch.** `scripts/release-suite.sh` writes
`test/integration/.last-run-stamp` (gitignored — it describes one machine's
run, not a property of the tree). If that stamp, with the cheap push CI,
covers the release commit — or a dispatched run already does — nothing is
dispatched, because it would be the same tests on the same code for an answer
you already hold. A run never dispatched is the only thing that improves both
turnaround and Actions spend — see `AXL-CI-Release-Speed-Design.md` §10.3.

"Covers" is deliberately narrow, since a false positive here tags code no
suite ever ran. Both sources — the local stamp and a CI run — are asked the
same two questions, through the same shared rule
(`release_gate_release_only`), so they cannot come to disagree about what a
release commit is allowed to touch:

| situation | gate |
|---|---|
| stamp's SHA == the release commit, **and** a green cheap push run on it | satisfied |
| stamp is the release commit's PARENT, the only diff is `VERSION`, `include/axl/axl-version.h`, `CHANGELOG.md`, **and** a green cheap push run at the parent | satisfied |
| **a green dispatched (full) CI run at the release commit or that PARENT** | satisfied |
| **a green run at an ANCESTOR, with only prose and/or the version bump since** | satisfied — for either path |
| a good stamp, but no green push run of `gcc x64`, `gcc aa64` and `clang-tidy` on that tree | **refused** — push, and let the push run finish |
| that push run still going | **refused** as *still running* (exit 3) — wait for it; `cut-release.sh` does |
| that push run CANCELLED (a later docs-only push superseded it) | **refused**, naming the cancellation — re-dispatch or re-push |
| a stamp in the old `run-integration.sh` format | **refused** — only `scripts/release-suite.sh` certifies |
| a PUSH run (the suite job listed `skipped`) | **refused** as CI coverage — it never contains the suite |
| a CI run whose jobs were SKIPPED (the `[release-cut]` shape) | **refused** |
| `gh` missing or failing, so CI cannot be consulted at all | **refused** as *could not check* (exit 2), not *not covered* |
| anything else changed since the stamp | **refuses**; dispatches with `--ci-gate` |
| stamp missing or malformed (a failed or partial `release-suite.sh` writes none) | **refuses**; dispatches with `--ci-gate` |

The parent rule is not a convenience: `cut-release.sh` creates the version-bump
commit itself, so the stamp is always one commit behind by construction and a
bare SHA match could never fire at cut time.

`--force-ci` dispatches regardless. Use it when you want the fresh-OS backstop
specifically — a toolchain bump, a workflow change, anything where "it passed
on my box" is the thing in question.

Dispatching CI by hand on `main` (Actions tab → `workflow_dispatch`, or
`gh workflow run ci.yml --ref main`) is how the suite runs in CI at all now —
that commit IS the one you will tag, so a green dispatch is the fresh-OS
backstop and satisfies the gate on its own. The tag does not re-run it.
`--ci-gate` makes the cut dispatch CI on the release branch and wait for it
before tagging, whenever nothing already covers the commit. The `--ci` flag on the runner excludes the `local-only` tests CI
runners can't execute (patched-QEMU SMBus, usb-mouse pointer) — that's what CI runs.

**When the dispatched suite goes red (or is cancelled),** its per-test logs
are on the run as the `integration-logs` artifact (14 days):
`gh run download <run-id> -n integration-logs`. A green run uploads none.

### When a push seems to have vanished

```sh
scripts/check-main-ci.sh          # does origin/main's tip have a green push run?
```

"Green" here is the three cheap jobs passing. A push run never contains the
suite (it runs on dispatch), so the script does not ask for it; whether the
suite covered a commit is the release gate's question. An inert tip — a
docs-only push, or the `[release-cut]` commit, whose own run skips the cheap
jobs — is answered by the commit below it, through the gate's own bounded
walk, and "pending" means the run that decides is still going, not merely
that some run is. Exit 2 means it could not check (no `gh`, a failing one, or
a tip not fetched yet).

On 2026-09-05 a push to `main` produced **no workflow run** — not a skipped
one, which still exists and still lists, but no run object at all. `ci.yml` was
active, its trigger has no path filter, and the commit subject did not match
the skip guard. The event simply did not fire.

Everything downstream fails closed: the release gate refuses a commit it cannot
see a green run for, so a missed event can only *block* a release, never let an
unverified one through. But nothing goes red and nothing is unusual to look at,
so the gap surfaces at cut time — when someone is waiting. This answers it in a
second, and the remedy is one line: `gh workflow run ci.yml --ref main`.

### Before a tag: check docs at CI's doxygen version

```sh
scripts/build-docs.sh --ci-doxygen    # runs CI's doxygen in a container
```

`docs.yml` fires on **every** `v*` tag and a tag cannot be re-cut, so this is
the one gate worth running that the normal `verify.sh` cannot substitute for.
A dev box's doxygen is newer than the `ubuntu-latest` apt package CI installs,
and it ACCEPTS markup the older one rejects — so a locally-clean docs gate is
not evidence. That skew shipped broken docs twice: v3.2.0 and v4.2.0.

### Registering a self-hosted runner (any dev host)

Jobs run in a **container** (`ubuntu:24.04`, CI's own image), so a runner host
needs Docker and KVM — **not** the AXL toolchain. That is what makes this
portable: any machine you develop on can become the runner, and a second one
can join without displacing the first.

**Prerequisites**

```sh
sudo dnf config-manager --add-repo https://download.docker.com/linux/centos/docker-ce.repo
sudo dnf remove podman-docker            # it owns /usr/bin/docker; docker-ce-cli must
sudo dnf install docker-ce docker-ce-cli containerd.io docker-buildx-plugin
sudo systemctl enable --now docker
sudo usermod -aG docker "$(id -un)"      # log out/in, or use `sg docker`
```

**Docker, not podman**, and the reason is not preference. The Actions runner
drives the Docker CLI and API; podman's compat socket runs container jobs but
**will not create a missing bind-mount source**, and the runner only populates
`_work/_actions` for a job that uses an action — so any job without a `uses:`
step fails to start a container. Details in
`AXL-CI-Release-Speed-Design.md` §11.4.

**Register** (once per machine; the org's `Default` runner group is
`visibility=all`, so one registration serves every repo):

```sh
mkdir -p ~/actions-runner && cd ~/actions-runner
curl -fsSLO https://github.com/actions/runner/releases/latest/download/actions-runner-linux-x64.tar.gz
tar xzf actions-runner-linux-x64.tar.gz

./config.sh --url https://github.com/aximcode \
            --token "$(gh api -X POST /orgs/aximcode/actions/runners/registration-token --jq .token)" \
            --name "axl-qemu-$(hostname -s)" \
            --labels axl-qemu \
            --unattended --replace

sudo ./svc.sh install "$(id -un)" && sudo ./svc.sh start
```

**A DEBUG OVMF is a runner prerequisite**, provisioned like the toolchains.
The suite job runs on the host, and `test-stderr-stale-qemu.sh` needs a
DEBUG build of OVMF (its pool is cleared on free, so a write through a freed
handle faults). The job's "Pin the runner's DEBUG OVMF" step resolves it with
`find_debug_ovmf` (`scripts/axl-common.sh`) as the runner account sees it --
`AXL_OVMF_DEBUG_DIR`, else `$EDK2_DIR/Build/OvmfX64/DEBUG_GCC5/FV`, else
`$HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV` -- and fails the job, naming each
place and why, when there is none. Build `OvmfPkg` DEBUG for X64 and, if it
is not in one of those trees, add `AXL_OVMF_DEBUG_DIR=<its FV directory>` to
the runner's `~/actions-runner/.env` (then restart the service).

**The hosted fallback cannot certify a release.** `runner: hosted` exists for
when the box is down, but a hosted runner has no DEBUG OVMF and building EDK2
there costs more than a fallback is worth. Its suite job therefore REFUSES
(`release-suite.sh --ci`, exit 2) before any step, saying why and what to do
-- run on the self-hosted runner, or set `AXL_OVMF_DEBUG_DIR` -- rather than
skip the tests that need one. The cheap jobs (build, lint) still run there.

The **label** `axl-qemu` is what the workflow selects on; the **name** only has
to be unique, which is why it carries the hostname. Register several hosts with
the same label and GitHub sends each job to whichever is idle.

**Confirm it is ONLINE, not merely registered** — a job targeting a label whose
runners are all offline queues rather than failing:

```sh
gh api /orgs/aximcode/actions/runners \
  --jq '.runners[] | {name, status, busy, labels: [.labels[].name]}'
```

**Decommissioning matters.** A retired machine left registered is a runner that
is permanently offline; if it is the only one, jobs queue forever. From the
host: `sudo ./svc.sh stop && sudo ./svc.sh uninstall && ./config.sh remove
--token <removal-token>`. Or delete it in the org's runner settings.

**Sanity check:** `.github/workflows/runner-probe.yml` dispatches a container
job that prints the distro and whether `/dev/kvm` is usable. Run it after
registering a new host.

### GitHub Actions trigger policy

**A push to `main` runs only the cheap `ci.yml` jobs; nothing runs on any other
branch.** The QEMU suite is dispatched, never pushed. The triggers are baked
into the workflow files:

| Workflow | What | Triggers on |
|---|---|---|
| **ci.yml** | plan + build (gcc x64/aa64) + lint (clang-tidy); QEMU integration on dispatch | **every push to `main`** runs `plan`, `gcc x64`, `gcc aa64` and `clang-tidy` — a `plan` job classifies the push first (`scripts/ci-plan.sh`), and a **docs-only** one runs `plan` alone in ~4 s. **`QEMU integration (full suite)` runs only on `workflow_dispatch`** (since 2026-10-21, design §14) — by hand or from `cut-release.sh --ci-gate`. NOT on release tags, and no schedule. A push cancels an older push run; it no longer cancels a running dispatch |
| **docs.yml** | Doxygen/Sphinx → Cloudflare Pages | major tag `vX.0.0`, or manual |
| **release.yml** | build + publish `install.sh`, `VERSION`, `SHA256SUMS`, the four tarballs and (not for a prerelease) the `.deb`/`.rpm` | **every** release tag `v*` (it's the publish step), or manual |

So: a push to a non-`main` branch runs nothing, a push to `main` runs the cheap
jobs; **no** release tag runs CI (it was validated on
`main` before the cut); a **patch/minor** release tag runs only `release.yml`
(publish); a **major** (`vX.0.0`) tag also runs `docs.yml`. `cut-release.sh` tells
its watcher which to expect (major → `Release Docs`, minor/patch → `Release`), so
a cut never hangs waiting for a workflow that won't run.

**Trigger a chain on demand** ("I specifically want it") — no push required:

```sh
scripts/run-ci.sh                 # dispatch + watch CI on origin/main
scripts/run-ci.sh <branch|tag>    # on a specific ref
scripts/run-ci.sh --docs          # dispatch the Docs deploy instead
# or directly:  gh workflow run ci.yml --ref main   (Actions tab → Run workflow)
```

To change the policy, edit the `on:` block at the top of each workflow (each has
a comment explaining its trigger).

## Prerequisites

- You've picked the flow — see
  [Which flow](#which-flow-from-main-or-from-a-tag). On the `main` flow:
  you're on `main`, working tree clean, and `git log origin/main..HEAD`
  shows the commits to ship. On the tag flow: you're on the release
  branch and `git log vPREV..HEAD` shows them — **read that range**, it
  is what the release will contain.
- The integration suite passes locally. The quick smoke set:

  ```sh
  ./test/integration/test-axl.sh
  ./test/integration/test-tools.sh
  ./test/integration/test-tcp-echo.sh
  ./test/integration/test-http.sh
  ./test/integration/test-cpu-idle.sh
  ```

  **But this list is a SUBSET of what CI runs** — `.github/workflows/ci.yml`
  is the source of truth, and it runs more (e.g. `test-input-keys-qemu.sh`,
  `test-yield-ctrlc.sh`, `test-axl-cc-service.sh`, the `clang-tidy` job, both
  arches). Running only the five above can leave a CI-only failure undetected
  until the tag. Either run every `*.sh` step listed in `ci.yml`, or — better
  — rely on the §4b "watch CI on `main` before tagging" gate, which validates
  the *exact* CI matrix. Some tests are deliberately local-only (they need a
  QMP-pointer-injection-capable host the GitHub runners don't provide, e.g.
  `test-input-modifiers-qemu.sh`); run those by hand before a release.

- Both archs build clean against `BUILD=RELEASE` -- the CMake RELEASE
  presets, through the wrapper `release.yml`, `install.sh` and every test
  build with. Each configuration has its own tree (`build/<arch>-release`),
  so the two coexist with the DEBUG trees the suite uses:

  ```sh
  ./scripts/axl-build.sh ARCH=x64  BUILD=RELEASE tools
  ./scripts/axl-build.sh ARCH=aa64 BUILD=RELEASE tools
  ```

  Ask the wrapper where a configuration's artefacts land rather than
  hardcoding the path:

  ```sh
  ./scripts/axl-build.sh --print-prefix ARCH=x64 BUILD=RELEASE   # -> build/x64-release
  ```

- **The release builds from a clean checkout.** CI's build job builds the
  RELEASE tree of each arch and makes the release's tarballs from it with the
  scripts `release.yml` runs, on every push. (Until 5.0.0 it also held them to
  the retired Makefile's -- `release-parity.sh`, gone with it.) Both CI and
  `release.yml` build with the CMake `scripts/ci-cmake.sh` pins, and so does
  every local entry point -- `release-suite.sh` included -- through
  `scripts/axl-cmake-pin.sh` (AXL-Build-System-Design.md §10.13), so a local
  certification runs the CMake that ships. Bumping the pin is a change to
  `ci-cmake.sh` alone (the version and its two SHA-256s).

- **clang-tidy is clean locally.** The CI workflow's `lint` job
  runs `clang-tidy` with `WarningsAsErrors: '*'`; the unit /
  integration suites above don't exercise it. This step has been
  the post-tag failure mode TWICE in a row (v0.18.0 → v0.18.1,
  v0.19.0 → v0.19.1): Release published artifacts cleanly, then
  CI flagged a finding and required a follow-up patch release.
  Run it the same way CI does, before tagging, so any findings
  surface BEFORE the artifacts are public — just use the wrapper:

  ```sh
  scripts/lint.sh      # the x64-lint preset, exactly as ci.yml's lint job
  ```

  Fix any errors and re-run. If the only findings are warnings
  (not errors), CI is fine — `WarningsAsErrors: '*'` in
  `.clang-tidy` only escalates the checks the config enables; the
  `2 warnings generated` lines per file are noise.

  **Version skew — local-clean does NOT imply CI-clean (unless versions
  match).** CI pins **`clang-tidy-21`** by running the `lint` job inside an
  **`ubuntu:26.04` container** (26.04 is the first Ubuntu whose apt ships
  clang-tidy-21 natively; 24.04 tops out at 20). `cmake/AxlLint.cmake` pins the
  same `AXL_CLANG_TIDY_VERSION 21` — so a clean `scripts/lint.sh` *does* mean a clean CI
  lint when your local clang-tidy is 21. Three ways to get a matching 21:
  - **Ubuntu 26.04+:** `sudo apt-get install clang-tidy-21`.
  - **EL/Fedora/macOS dev boxes:** the distro's current `clang-tidy` is often
    already 21 — the lint preset detects that and uses it without warning
    (it only warns on a *real* mismatch).
  - **Any host:** run CI's exact lint in the same image —
    `podman run --rm -v "$PWD":/src:ro ubuntu:26.04 …` (the container `ci.yml`'s
    lint job uses), so local == CI byte-for-byte regardless of distro.

  This pinning exists because a *newer* local clang-tidy silently passed code an
  older CI clang-tidy flagged — v1.0.0 hit exactly that
  (`bugprone-sizeof-expression` on a correct array-of-pointers `sizeof`). When
  you intentionally move clang-tidy versions, bump it in both `ci.yml` (the
  `clang-tidy-NN` install + invocation, and the `ubuntu:NN.NN` container if the
  new version needs a newer base) and `AXL_CLANG_TIDY_VERSION` in
  `cmake/AxlLint.cmake` together.
  Either way, the §4b CI-on-`main` run remains the authoritative gate.

- **Doxygen has the same version-skew trap, with no pin available.**
  `docs.yml` installs whatever doxygen `ubuntu-latest`'s apt ships (1.9.8 at
  writing); a dev box is usually far newer, and reference resolution differs —
  1.13 resolved two link targets 1.9.8 could not, so `scripts/build-docs.sh`
  called the zero-warning gate clean while the v3.2.0 Docs run failed on both.
  Docs is best-effort for the release itself (artifacts come from Release), but
  the published site does not update until it is fixed. Before a release that
  touches public headers, reproduce CI's version:

  ```sh
  podman run --rm -v "$PWD":/src:z -w /src ubuntu:24.04 bash -c \
    'apt-get update -qq && apt-get install -y -qq doxygen && \
     cd docs/sphinx && doxygen Doxyfile'
  ```

  **Run clang-tidy one file per process (`-n1`).** Passing many
  TUs to a single `clang-tidy` invocation makes the path-sensitive
  `clang-analyzer-*` checks (notably `security.ArrayBound`)
  non-deterministic: the same clean tree intermittently reports
  spurious out-of-bounds findings that vanish when each file is
  analyzed alone with full budget. `-n1 -P"$(nproc)"` is
  deterministic, parallel, and still catches real bugs (they
  reproduce per-file). A bare `xargs -0 clang-tidy` (batched)
  is the flaky form — don't use it.

  **What `scripts/lint.sh` covers, and what it deliberately does not.**
  It is a wrapper over the **x64-lint CMake preset** (`cmake/AxlLint.cmake`):
  clang-tidy runs beside each compile, per target, and Ninja re-checks only
  the TUs whose source, headers, `.clang-tidy` or clang-tidy changed. The
  preset will not even configure if a source is in no linted target. Four
  passes:

  1. `clang -fsyntax-only -Wall -Wextra` over every project TU
     (`scripts/check-clang-warnings.py`, the preset's `lint.clang-warnings`
     test, over the preset's compile database). The tree builds with **gcc**, and
     clang-*tidy* reports `.clang-tidy`'s checks, not the clang frontend's
     own `-W` diagnostics — so before this pass a clang-only compiler
     warning had nowhere to surface, and a `-Wformat` "zero field width"
     reached a commit that way. One category is suppressed:
     `-Wmissing-field-initializers` (60 of the 75 `-Wextra` findings; in C a
     partial initializer zero-fills the rest by language guarantee, so it is
     a style opinion, not a defect signal). Everything else `-Wextra` brings
     is enforced.
  2. clang-tidy over `src/` — the full `.clang-tidy` config.
  3. clang-tidy over `test/unit/`, `tools/` **and `test/integration/`** —
     **`bugprone-*` only** (`--checks='-clang-analyzer-*'`, which subtracts
     the analyzer from the shared config so the disabled-check list stays
     single-sourced).
  4. clang-tidy over the C++ TUs — the full config, against the cross
     toolchain's libstdc++.

  Pass 3 is narrowed on purpose, and the numbers are why. All three
  directories are **0** findings at `bugprone-*` today and all three are far
  from clean at the full config, for the same reason with different
  specifics:

  | Scope | `bugprone-*` | full config | what the difference is |
  |---|---|---|---|
  | `test/unit/` | 0 | ~40 | the analyzer objecting to what unit tests do deliberately — casting `99` to an enum to pin the bad-enum error path, indexing a buffer the test has already asserted non-NULL |
  | `tools/` | 0 | 10 | 4 × `security.PointerSub` on ordinary buffer arithmetic, 2 × garbage-value in `mkfixture.c`/`sysinfo.c`, and a `core.DivideZero` in `crashtest.c` — a tool whose entire purpose is to divide by zero |
  | `test/integration/` (`*.c`) | 0 (was 5) | 10 (was 15) | 4 × `security.PointerSub`, every one of them on `AXL_EMBED_SIZE`, which subtracts two linker symbols *by construction*; 2 × `core.CallAndMessage` on a vtable the fixture has already asserted non-NULL; 3 × `unix.StdCLibraryFunctions` and 1 × `insecureAPI.strcpy` on what the two libc fixtures *exist to do* — one of them is the deliberate `open(NULL, O_RDONLY)` assertion |

  Enabling the analyzer over any of them would mean a red gate or ~50
  suppressions that bury the real findings.

  `test/integration/` was measured at **5** `bugprone-*` findings the first
  time anything linted it — all `bugprone-assignment-in-if-condition`, all in
  `ssh-server-selftest.c`, all `(v = f()) != NULL` inside an `if`. They were
  **fixed, not waived**: `src/`, `tools/` and `test/unit/` contain not one
  instance of that idiom, because this pass has forbidden it everywhere it
  could see for as long as it has run, and a fixture is not where the house
  style gets an exemption.

  `tools/` was measured at **3** `bugprone-*` findings before it was turned
  on, and the three were resolved individually rather than waived as a
  group:

  - `tools/sysinfo.c` — `bugprone-switch-missing-default-case` on the SMBIOS
    memory-type decode. A real, if trivial, omission: **fixed** with an
    explicit `default: break;`, which is also the clearer code, since the
    switch is over a raw firmware byte and unlisted values are the expected
    case rather than an oversight.
  - `tools/sed.c` ×2 — `bugprone-branch-clone`, both *semantically distinct*
    cases that happen to share an implementation (`:` label-definition vs the
    `b`/`t`/`T` jumps; `A_ZERO` vs `default`). Merging them would delete
    intent, so each carries a **targeted** `NOLINTBEGIN/END(bugprone-branch-clone)`
    with the reason written out above it — the same shape `src/format/axl-format.c`
    already uses for its `long` / `long long` `va_arg` branches. The check
    stays enabled everywhere else.

  **How `test/integration/*.c` got into the lint**, since "absent from it"
  is what kept the directory out for as long as it was out: under the old
  `bear`-driven lint, the `integration-fixtures` Makefile target compiled all
  60 guest fixtures -- objects only, no links. Under the preset they are the
  `axl_fixture_objs` target (every `test/integration/*.c`, as the retired
  Makefile's wildcard took them), and the preset's coverage check fails configure if
  one falls out.

  **It widens pass 1 as well as pass 3**, which is worth stating because
  nothing asked for it: `check-clang-warnings.py` selects from the
  *database*, so those 59 TUs also joined `clang -Wall -Wextra` — 366 → 426
  TUs, a far wider check set than the `bugprone-*` they were added for.
  Measured at 2.5 s for the whole pass, **zero findings**.

  Cost, measured 2026-11-02 on the preset (x64, idle box): cold -- an empty
  `build/x64-lint`, which is what CI's fresh container pays -- **59.6 s**;
  warm with nothing changed **0.03 s**, after a one-file change **1.9 s**, the
  full `scripts/lint.sh` including its tests **3.1 s**. The old `bear`-driven
  lint cost ~55 s on every run.

  **Not covered (measured, deferred — not an oversight):**
  - `test/fuzz/` — a HOST build under libFuzzer, which the lint build does
    not run, so it is absent from the compile database. clang-tidy does not
    skip such a file: it interpolates a compile command from the nearest
    sibling by path, so the result is either analysis under flags nobody
    chose for it or a nonsense `'foo.h' file not found` — measured both
    ways, and neither is real analysis of this code.
  - `test/integration/*.cpp` — the 16 C++ fixtures. Pass 4 takes its list
    **from the database**, so putting them in it also puts them in that pass
    at the *full* config, which is a separate measurement and a separate
    decision from the `bugprone-*` one made for the C fixtures.
  - ~~The **non-default configurations** of three C fixtures~~ -- closed:
    the preset lints EVERY variant of a source (`axl_object_variant`;
    `docs/AXL-Build-System-Design.md` §10.3), so `pin-svc-driver.c`'s,
    `sd-sibling-driver.c`'s and `console-device-smoke.c`'s `#ifdef`-gated
    halves are read too.

## Which flow: from `main`, or from a tag?

Two flows, and picking the wrong one is the most expensive mistake in this
document. Ask one question:

> **Does `main` contain anything that should NOT go out in this release?**

**No — release the tip of `main`.** Use `scripts/cut-release.sh X.Y.Z`. This is
the flow the script was written for and the rest of this document describes.

**Yes — cut from the previous TAG on a release branch.** Use
`scripts/cut-release.sh X.Y.Z --branch release-X.Y.Z` from a worktree on that
branch. The script used to hard-require `main` and hardcode
`git push origin main`; `--branch` parameterises both, and refuses any branch
whose name does not encode the version being cut (`release-X.Y.Z`, or
`release/X.Y.x` for a maintenance line carrying successive patches of one
minor). The differences from a `main` cut are in the next section.

This is not an exotic case. **v3.2.1, v3.2.2 and v3.2.3 were all cut from tags**
— every release since v3.2.0 — because `main` was accumulating a toolchain and
C++ rework that had no business in a patch. The script models
"release = tip of `main`", and this project frequently does not work that way.

### Why this matters more than it looks

`## Unreleased` is **branch-wide state**; a release is a **commit range**. They
agree only when the release is "everything on this branch since the last tag".
`cut-release.sh` dates whatever sits under `## Unreleased` — so on a branch
where those two have diverged, it will happily stamp unrelated work into the
release notes and the tag.

That is not hypothetical. v3.2.3 was first cut from `main` and dated **43
commits** of in-progress work into a patch release, two of them under a
`### Breaking` heading, publishing eight assets before anyone read the version
number against the content. It was deleted (zero downloads) and re-cut from the
`v3.2.2` tag.

`scripts/check-release-semver.sh` now refuses that specific shape — a
`### Breaking` section under a non-major bump — from inside `cut-release.sh`.
It is a backstop, not a substitute for picking the right flow: it catches a
mislabelled *version*, not unrelated work that happens to be non-breaking.

### The release commit skips CI, on purpose

`cut-release.sh` pushes a `release: vX.Y.Z` commit to `main` before it tags,
and `ci.yml` triggers on every push to `main`. That commit's entire content is
a version string and a date: the gate certifies it from its parent's runs, and
`release.yml` builds and tests everything again on the tag. (When the push
trigger also ran the suite, this ran it **twice on this box** per release.)

The release commit therefore carries a `[release-cut]` marker, and every
`ci.yml` job is conditioned on **not** seeing it.

**It is deliberately not `[skip ci]`.** GitHub honours that string natively —
by inspecting the pushed HEAD commit — and the **tag** push carries the same
commit. Using it would very likely skip `release.yml` and `docs.yml` too, and
publish nothing at all while reporting success. Our marker means nothing to
GitHub; only `ci.yml` looks for it.

`test-release-gate.sh` holds the two spellings equal (they live in files that
cannot import each other), asserts every `ci.yml` job is guarded, and asserts
that `release.yml` and `docs.yml` carry no such guard.

### Two kinds of breaking change

Semver mandates MAJOR for incompatible **API** changes. Not every break is an
API break, and treating them as one makes the version number stop describing
the thing consumers read it for. So the CHANGELOG has two headings, and
picking between them is a one-line test:

> **Does a consumer's CODE stop working, or their SCRIPT?**

| heading | means | version |
|---|---|---|
| `### Breaking` | their **code** breaks — a removed or changed function, a struct field, an ABI change. It no longer compiles, or no longer runs correctly | **MAJOR**, and the guard refuses anything less |
| `### Breaking (packaging)` | their **scripts** break — a renamed release asset, a retired install channel, a changed extraction layout. Their code compiles and links unchanged | **any level.** The guard allows it and prints a NOTE listing the entries |

**Both are real breaks and both must be announced.** The difference is only
which number has to move. A packaging break is never silent: the guard lists
it on every cut, and the release notes carry the section verbatim, because a
consumer pinning an asset name has to act on it before their next bump.

**The heading is your claim, and nothing can check it.** No script can tell
whether a removed symbol was public. If you are unsure, use `### Breaking` —
the cost of an unnecessary major is a number; the cost of a missed one is a
consumer whose build breaks on what their tooling told them was a safe bump.

**Worked example — 4.4.0 → 4.5.0 (D2).** Every release asset was renamed, the
`.deb`/`.rpm` were retired, and two tarballs stopped being tarbombs. Every
consumer that fetched or installed the SDK had to change something. And
`axl.h` was untouched: source and ABI compatibility were total, so a consumer
who vendored the headers and libs noticed nothing. That is
`### Breaking (packaging)` on a minor — and it is why the heading exists,
because the first draft of that CHANGELOG forced a 5.0.0 for a release in
which no API changed.

**`--allow-breaking` is still the escape hatch for a real API break** below a
major, and it should stay rare. If you find yourself reaching for it, check
first whether the entries belong under `### Breaking (packaging)` instead —
that is a re-classification, not a bypass.

### Cutting from a tag — what differs

Steps 1, 2, 3, 5, 6 and 7 below are **identical**. Step 4 pushes this branch
rather than `main`, and step **4b** — the gate — is the one that differs in
kind: see "CI is dispatched, not awaited" below.

```sh
# instead of working on main:
git worktree add ~/axl-sdk-X.Y.Z -b release-X.Y.Z vPREV   # e.g. v3.2.2
cd ~/axl-sdk-X.Y.Z

# cherry-pick or apply ONLY the change this release is for, write the
# `## Unreleased` section (see below), commit it, then:
scripts/cut-release.sh X.Y.Z --branch release-X.Y.Z

# steps 1-3 and 5-7 are what the script always does; step 4 pushes THIS
# branch rather than main. By hand, the only difference is:
#     git push -u origin release-X.Y.Z
```

**CI is dispatched, not awaited.** `ci.yml` triggers on `push: branches:
[main]`, so pushing a release branch starts **no run at all** — the gate would
wait 75 minutes for something that can never appear. On a `--branch` cut the
script therefore turns `--ci-gate` on for you and dispatches `ci.yml` on that
ref — *unless* the local stamp or an existing green run already covers the
release commit, in which case it says so and skips the dispatch. In a worktree
where you have just run the local suite, skipping is the normal outcome, not a
bug. (A dispatched run also ignores the `[release-cut]` marker, because that
guard reads `github.event.head_commit`, which is null on `workflow_dispatch`.)

Two things to get right:

- **The `## Unreleased` section on the release branch is yours to write.** The
  tag's CHANGELOG will not have one (it was dated at that release), so add a
  heading carrying *only* this release's entries. Do not copy `main`'s
  accumulator across; that is the mistake this whole section exists to prevent.
- **The source mirror follows the tag.** `release.yml`'s "Publish source
  snapshot" step force-replaces `aximcode/axl-sdk-releases`' default branch
  with the tagged tree. Cutting a patch from an OLDER line therefore rewinds
  that mirror to the older source until the next cut from `main`. Harmless when
  the patch is the newest thing released; worth knowing before cutting, say,
  4.6.x after 4.7.0 shipped.
- **Land it on `main` afterwards.** The release branch is not a long-lived
  fork. Cherry-pick the change onto `main` as one squashed commit with the
  version appended to the subject — the pattern `e98a4f6a` (v3.2.1) and
  `d64e15d1` (v3.2.2) set. Expect `CHANGELOG.md` to be the only conflict: keep
  `main`'s `## Unreleased` intact and place the newly-dated section beneath it.

## Cut the release

### 1. Bump the version

```sh
scripts/bump-version.sh X.Y.Z
```

This updates **both** `VERSION` and `include/axl/axl-version.h` in
one shot. Don't edit either file by hand — the CMake configure
compares them and refuses to build on mismatch (and `cut-release.sh`
runs `scripts/check-version.sh`). If you've ever seen
`ERROR: VERSION (X.Y.Z) != axl-version.h (P.Q.R)` in CI, this is
why.

### 2. Date the CHANGELOG

`CHANGELOG.md` accumulates under a `## Unreleased` heading during
development. At release time, rename it:

```diff
- ## Unreleased
+ ## X.Y.Z — YYYY-MM-DD
```

Sweep `git log <prev-tag>..HEAD --oneline` and confirm every
user-visible change has an entry. Headlines worth catching:

- New public API (Added)
- Behavior changes that callers might trip on (Changed, Migration)
- Bug fixes — especially anything that was a UAF, leak, or
  flake (Fixed)
- Build/CI/Docs/Examples changes that affect contributors

### 3. One commit for the release metadata

```sh
git add VERSION include/axl/axl-version.h CHANGELOG.md
git commit -m "release: vX.Y.Z"
```

Keep this commit small. The release-cut commit is the canonical
"what changed in this release" reference; if it's noisy with code
changes, the diff-against-previous-tag becomes harder to read.

### 4. Push the release branch first

```sh
git push origin main                 # main flow
git push -u origin release-X.Y.Z     # tag flow
```

The branch must contain the release-metadata commit *before* the tag
points at it; if you tag first and then push the branch, the
release.yml workflow can race and check out the wrong commit.
`release.yml` triggers on any `v*` tag regardless of which branch
carries it, so the tag flow publishes exactly the same way.

### 4b. The gate: a dispatched suite, or the local suite plus the push CI

See ["The gate"](#the-gate-a-dispatched-ci-suite-or-the-local-release-suite-plus-the-push-ci)
above — it is the authority, and this step used to contradict it. In short:
`scripts/cut-release.sh X.Y.Z` refuses unless a dispatched suite run, or the
local release suite plus the cheap push CI, already covers the commit; it
names both remedies when nothing does. `--ci-gate` dispatches CI on the release
branch and waits for the suite to go green before creating the tag;
`--tag-untested` tags anyway, loudly, and records it in the release notes.
CI is **not** re-run by the tag; a **major** (`vX.0.0`) tag additionally
triggers `docs.yml` as a post-publish step.

The local path, run before you cut:

```sh
scripts/release-suite.sh   # CI's integration job, locally; stamps on success
scripts/lint.sh            # clang-tidy as CI runs it
```

The two CI-only failures that burned v1.0.0 (a headless-runner pointer test, a
clang-tidy version skew) are handled structurally: the pointer test is
`local-only` (excluded from CI), and `scripts/lint.sh` pins the exact CI
clang-tidy version — run it locally and it matches CI byte-for-byte.

### 5. Create and push the tag

```sh
git tag -a vX.Y.Z -m "vX.Y.Z

<headline paragraph from CHANGELOG>

  - bullet 1
  - bullet 2
  - bullet 3

See CHANGELOG.md for the full list."

git push origin vX.Y.Z
```

The annotated tag's body shows up in the GitHub Release page and
in `gh release view vX.Y.Z`. Worth a few minutes of polish.

### 6. Watch the workflows — now OPT-IN

**`cut-release.sh` no longer waits by default.** It pushes the tag, prints the
Actions URL and the verification command, and hands the terminal back. Watching
to completion cost 4-7 minutes of somebody's attention per release and what it
returned was a workflow's exit status; `scripts/check-published-release.sh`
verifies the published **bytes** — which is the stronger claim, catches a
truncated upload or a missed rename that a green workflow would not, and can be
run whenever the notification arrives:

```sh
scripts/check-published-release.sh vX.Y.Z   # when Release + Docs finish
scripts/cut-release.sh X.Y.Z --watch        # or wait, as before
```

Nothing here can un-publish a tag, so not watching costs no safety — only the
latency between a failed publish and your noticing it, which is what the
verification command above closes.

The tag push triggers **two** workflows (and on a minor/patch, one):

> **CI is NOT one of them**, despite what this section said until 2026-08-18.
> A tag re-running CI on a commit already validated on `main` was pure
> duplication, and that trigger was removed. (This note itself then went stale
> the other way — it said `ci.yml` is `workflow_dispatch` only, long after the
> push-to-`main` trigger came back. The trigger table above is authoritative.)

- **Release** (`.github/workflows/release.yml`) — builds the SDK,
  host-tools and UEFI-tools tarballs (both arches), and attaches
  everything plus `install.sh`, `VERSION` and `SHA256SUMS` to a
  GitHub Release on `aximcode/axl-sdk-releases`. Since 5.0.0 it also
  builds the versioned **`.deb` and `.rpm`** (`axl-sdk-<ver>`, under
  `/opt/axl-sdk-<ver>`) for every release but a prerelease. They were
  retired with D2 in 4.5.0 and came back built from the same install
  rules as the tarballs — the `.rpm` by CPack's RPM generator, the `.deb`
  by `dpkg-deb` under `cpack -G External` (CPack's DEB generator's
  Installed-Size counts directory sizes, so its `.deb` differed between
  ext4 and XFS)
  ([AXL-Distribution-Design.md](AXL-Distribution-Design.md) §17.5).
- **Docs** (`.github/workflows/docs.yml`) — Doxygen + Sphinx
  build + Cloudflare Pages deploy.

**Realistic timing.** The TAG's own workflows are fast —
**~4–5 minutes wall-clock** for Release + Docs, which run in
parallel. Verified on v0.9.0: Release 2m57s, Docs 1m44s.
Release's Build-tools-aa64 (~3 min) is the longest of them,
slower than x64 because cross-tool execution goes through QEMU
user-mode emulation.

**The CI backstop is the slow one, and it is a separate wait.**
`--ci-gate` dispatches ci.yml on the release commit before
tagging **unless something already covers it** (see above),
and its QEMU job runs the WHOLE integration suite, each test in
its own QEMU. Measured **~50 minutes** on the v3.2.0 cut; the
other three jobs finished in under 6.

That wall time is not a repo defect: `run-integration.sh` picks
`nproc - 2` workers, and a hosted runner has 2 cores, so it
evaluates to one worker. The same unchanged line gives six on an
8-core box. `AXL-CI-Release-Speed-Design.md` §10 has the
measurement and what to do about it; the stamp above is the part
that is already built. `wait_for_ci` allows 75 minutes. If it ever times out on
a run that is still going, the tag was NOT created — wait for CI
yourself and finish with `--resume`, rather than re-cutting.

**Pathological case — bad-DNS days.** When GitHub Actions runner
DNS is flaky (azure.archive.ubuntu.com mirrors), `apt-get
install` retries can stall jobs at **30–40 minutes** before
giving up. The v0.9.0 first attempt hit this — five jobs across
CI/Release/Docs all failed at exit-code 100 (apt's "couldn't
fetch packages") after 18–40 minute stalls. The workflows now
write `Acquire::Retries=3` and `Acquire::http(s)::Timeout=15`
into `/etc/apt/apt.conf.d/99-axl-retry` as the first action of
every install-deps step — that bounds the worst case to ~5
minutes on bad days instead of 30+. Even with the bound, plan
for **up to ~15 minutes total** if a re-run is needed during a
runner network blip.

**Workflow notes:**

- AArch64 builds run on x86 hosted runners. The library + tests
  + tools cross-compile fine in 1-3 min each; QEMU emulation
  only kicks in when something needs to *execute* an aa64 binary
  (e.g. CI's QEMU integration tests).
- The `Docs` workflow occasionally hits transient apt-mirror
  failures even on otherwise-healthy days. Re-running just that
  workflow is the fix; it's not a release blocker since
  artifacts come from `Release`, not `Docs`.

**The default — and what to use unless you have a specific
reason to stream live output:**

```sh
scripts/watch-release-runs.sh
```

That's it. The script polls all three workflows via GraphQL
(separate quota pool from REST) at 60-second intervals, prints
status per workflow per tick, exits rc=0 only when all three
reach SUCCESS, rc=1 if any finish non-SUCCESS. Total cost:
~60 GraphQL calls/hour. **Use this. Don't reach for `gh run
watch`** unless you have a specific reason to stream
per-step output.

CI and Release **must** finish green for the release to be
considered shipped. Docs is best-effort — re-run later if it
flakes.

#### Why not `gh run watch`?

GitHub's REST API has a 5,000 req/hour cap per authenticated
user. `gh run watch --exit-status` is a polling loop disguised
as a stream — it makes ~1 request every 3 seconds. Three
parallel watchers (CI + Release + Docs) for a 30-minute
multi-arch workflow generates **~1,800 calls**; add ad-hoc
`gh run view` / `gh run list` during diagnosis and you'll
exhaust the quota mid-flight. When that happens the watcher
exits **rc=1** on the rate-limit error, which is
indistinguishable from "the run failed" by exit code alone.

GitHub does NOT sell a per-account rate-limit boost; the only
escape is Enterprise Cloud (15,000/hr) or a GitHub App on a
multi-installation org (up to 12,500/hr).

`scripts/watch-release-runs.sh` sidesteps the entire issue —
GraphQL has its own 5,000-points/hr quota that REST polling
doesn't touch, and a status query costs ~1 point. ~60 calls
per hour is well below the limit even if you forget to stop
the loop.

#### The `gh run watch` fallback

When you need streaming per-step output (e.g., debugging a
specific job's failure), use `gh run watch` for **one
workflow at a time** — never three in parallel. Pick the
slowest (Release):

```sh
gh run list --commit "$(git rev-parse HEAD)" \
    --json databaseId,workflowName,status --limit 5
gh run watch <release-id>
```

Critical caveat: **`gh run watch` follows ALL jobs in the
workflow**, not just the first one. Don't be misled by an
early `✓ Complete job` line in the output — multi-arch
workflows have several jobs, and the watcher keeps running
until the slowest one finishes.

Two corollaries:

1. **The streamed `✓ Complete job` lines are per-individual-job,
   NOT workflow completion.** A multi-arch workflow has several
   jobs (Build tools x64 → Build tools aa64 → Build axl-sdk →
   Build host-tools → publish); seeing `✓ Complete job` only
   means one of those finished. Don't conflate with workflow
   conclusion.
2. **Cross-check via GraphQL after rate-limit hits.** GraphQL
   has a separate 5000 req/h quota that REST polling doesn't
   touch:

   ```sh
   gh api graphql -f query='
   { repository(owner: "aximcode", name: "axl-sdk") {
       object(expression: "<sha>") {
         ... on Commit { checkSuites(first: 10) {
           nodes { workflowRun { workflow { name } } status conclusion }
   } } } } }'
   ```

   Returns `status=COMPLETED, conclusion=SUCCESS|FAILURE` for
   each workflow without consuming REST quota. Use this whenever
   `gh run watch` exits non-zero or `gh run view` 403s — it's
   the authoritative source while the REST limit recovers
   (typically 1 hour from the first rate-limit error).

### 7. Confirm

The Release is published on the public sibling repo
`aximcode/axl-sdk-releases`, not on the private upstream — pass
`--repo` to `gh release view`:

```sh
gh release view vX.Y.Z --repo aximcode/axl-sdk-releases
```

Should show exactly **nine** assets for a release (**seven** for a
prerelease, which publishes no packages):

| asset | name |
|---|---|
| the installer | `install.sh` |
| the version it resolves | `VERSION` |
| checksums over every other asset | `SHA256SUMS` |
| host SDK prefix | `axl-sdk-linux-<ver>-x86_64.tar.gz` |
| host tooling, no compiler | `axl-sdk-host-tools-<ver>.tar.gz` |
| TARGET firmware binaries (two files) | `axl-sdk-uefi-tools-<ver>-{x64,aa64}.tar.gz` |
| Debian/Ubuntu package (release only) | `axl-sdk-<ver>_<ver>-1_amd64.deb` |
| RHEL/Fedora package (release only) | `axl-sdk-<ver>-<ver>-1.x86_64.rpm` |

Don't count from this table: the authority is the rule `release.yml`
publishes by — `scripts/make-release-assets.sh assets --version X.Y.Z`
lists every asset but `SHA256SUMS`. `scripts/check-published-release.sh
vX.Y.Z` is the check to run on the published release: it downloads every
asset against `SHA256SUMS` and checks the package names. (This section said "six" through 4.7.5, which actually
published seven; the uefi-tools row is two files.)

Three names are stable and the rest carry the version, and only the first
two need to be stable — they are the only ones fetched by a reader who
does not yet know the version (§14.1a). `<arch>` means the **host** on
the SDK tarball and the **target firmware** on the uefi-tools ones. The
packages' names carry the version TWICE, deliberately: the package NAME is
versioned (`axl-sdk-<ver>`, installed side by side under
`/opt/axl-sdk-<ver>`), and so is its version field
([AXL-Distribution-Design.md](AXL-Distribution-Design.md) §17.5). An RPM
`Version` cannot hold a prerelease's `-`, which is why a prerelease
publishes no packages.

The release job itself asserts the count, and `gate.check-asset-names`
holds these spellings equal to the ones `packaging/install.sh`
constructs, so a rename that misses one place fails before the tag.

The SDK tarball includes the full C and C++ surface when CI builds
with BOTH bare-metal toolchains cached
(`scripts/install-toolchain.sh all` — C and C++ compile
bare-metal on both arches, so neither is optional any more).
Specifically, it contains:

- C bits (always): `axl-cc` driver, `libaxl.a` per arch,
  `axl.h` + `axl/*.h` headers, CRT0 objects, linker scripts,
  CMake config, pkg-config, JSON5 sidecars.  The `axl-c++` driver
  is here too — it is a dependency-free `exec axl-cc -x c++`
  wrapper, so it ships unconditionally; without the C++ glue it
  simply reports which install step is missing. Since D2 the
  archive also carries `share/doc/axl-sdk/` — LICENSE, NOTICE,
  THIRD_PARTY.md, the examples, and the five third-party licence
  sets the pre-D2 packages carried (the 5.0.0 packages install this
  same tree, so they carry them too).
- C++ bits (when toolchain present at build time): the four
  `axl-cxxrt-*.o` glue objects per arch, plus the C++ headers.
  The archive conveys no libstdc++ — `axl-cc` names the
  consumer's installed copy — so there is no runtime-dependency
  escalation; pure-C consumers can ignore the extra files.

CI cache invalidation: both toolchain tarballs are keyed on
`hashFiles('scripts/axl-toolchains.conf')`, so ANY edit to that
manifest forces a re-fetch of both — which is also why a
toolchain version bump must be PUBLISHED before the commit that
bumps it lands, or every CI job downloads a URL that 404s.
Verify the tarball contains the C++ glue after a release build.
`axl-cxxrt-terminate.o` is the one to grep for: it is the only
member that is C++-only, so its absence means exactly "no C++
half". The release job already asserts both arches, so this is
for checking a published artifact by hand:

```sh
tar tzf axl-sdk-linux-*-x86_64.tar.gz | grep axl-cxxrt
```

If `axl-cxxrt-terminate.o` is missing, the build host either didn't have
the bare-metal toolchains available or `install.sh`'s
auto-detect failed.  See
[`AXL-SDK-Design.md` §"C++ support"](AXL-SDK-Design.md) for the
toolchain story.

### 8. Get back on `main`

```sh
git checkout main      # if `git status` shows detached HEAD
git status             # confirm: "On branch main, working tree clean"
```

`git tag -a` and `git push origin <tag>` themselves don't detach
HEAD, but a typical release session involves enough sideband
operations (rebases, fix-ups during step 5's tag-message edit,
`git checkout vX.Y.Z` to spot-check the tagged tree before
pushing) that it's worth verifying. Subsequent commits made
while detached will be invisible to `git push origin main`,
which is the silent-data-loss case to avoid.

## Test releases (no CI)

To hand a build of `sshd` and the tools to a remote user without cutting a
release, publish a **test release** -- from a clean, committed checkout by default:

```sh
scripts/make-test-release.sh --dry-run   # build + gate, print the gh commands
scripts/make-test-release.sh             # build + gate + publish + prune
scripts/make-test-release.sh --keep 5    # keep 5 test releases instead of 3
scripts/make-test-release.sh --dirty     # build uncommitted changes too
scripts/make-test-release.sh --update    # rebuild the newest test release in place
scripts/make-test-release.sh --update TAG [--dirty]
scripts/make-test-release.sh --list      # test releases, newest first
scripts/make-test-release.sh --delete TAG
scripts/make-test-release.sh --delete-all
```

Every mode touches **only test pre-releases**: a tag shaped
`test-<8 digits>-<8 hex>[-dirty-<8 hex>]` whose release is a pre-release.
`--delete`, `--delete-all` and `--update` refuse anything else by name -- a
`v*` tag, a test-shaped full release, a tag that does not exist -- and
`--dry-run` prints every gh write of every mode instead of running it.

- **`--dirty`** builds the working tree as it is. Without it a tree with
  uncommitted tracked changes is refused, and the message names `--dirty`;
  untracked files under `share/` or `third_party/` are refused either way.
  The patch is every tracked change **plus every untracked, non-ignored file
  under the build inputs** (the top-level directories the repository tracks,
  plus any untracked top-level directory, less every hidden one (`.github`,
  and an untracked `.claude/` or the like: private state never goes in a
  public patch) and `docs/` -- a test release builds the tools and their
  tarball, never the Sphinx docs). Before building, the run prints the
  untracked directories it will include (`--dirty: including untracked:
  newlib/`) and refuses one over 50 MB, naming it -- `git diff --binary
  HEAD` over a throwaway index with those files added intent-to-add, so a new
  header a tracked edit includes is in it and the patch, applied to the
  commit, reproduces exactly what was built. An untracked file at the root
  (a scratch note) is not an input and is left out. The tag ends
  `-dirty-<first 8 hex of the patch's sha256>`, the notes say the build is
  from uncommitted changes and list them (the patch's `--stat`), and the
  patch ships as `<tag>.patch`, listed in `SHA256SUMS`.
- **`--update [TAG]`** (default: the newest test pre-release) rebuilds and
  re-gates from the current tree (respecting `--dirty`), then replaces that
  release's assets (`--clobber`, and any asset the new build lacks is
  deleted) and its notes in place. The tag name -- and so the link -- stays.
  When the source differs from the one the tag's commit names, the tag is
  moved to a fresh commit made as a new tag's is (below); `main` is still
  never written. The notes' first line states what THIS build is (the tag's
  name is from the first build published under it), every notes file
  records its build as `Source: axl-sdk <sha>[-dirty-<hash8>]`, and the
  notes say the tag moved whenever the source differs from the one the
  published notes describe -- so a rerun after an update that failed past
  moving the tag still says so.
- **`--list`** prints each test pre-release's tag, its tag commit's date and
  its asset count, newest first.

What it does, all on this machine:

1. Refuses a tree with tracked changes unless `--dirty` (the tag must name
   what was built), an untracked file under `share/` or `third_party/` (the
   build globs those, so it would ship), and a tag that already exists on
   `aximcode/axl-sdk-releases`. If HEAD is not on `origin/main` (the local
   ref; nothing is fetched) it warns and goes on -- it never pushes -- and the
   notes label the commit "(local commit, not yet pushed)".
2. `scripts/axl-build.sh ARCH=<arch> BUILD=RELEASE tools` for x64 and aa64
   (CMake, the RELEASE preset).
3. Builds both UEFI-tools tarballs with `scripts/make-release-assets.sh uefi-tools`
   -- the command `release.yml` runs, so the layout is a release's -- into
   `out/test-release/<tag>/`, versioned `<VERSION>-<tag>` (for example
   `axl-sdk-uefi-tools-4.7.3-test-20261020-0b4094d0-x64.tar.gz`).
4. **Gates** each arch on `test-efi-pack-release-qemu.sh`, booting a
   read-only stick extracted from **the tarball it is about to publish**
   (`UEFI_TOOLS_TARBALL=<tarball>`). Any failure publishes nothing, and no
   flag skips the gate.
5. Publishes the two tarballs and `SHA256SUMS` as a **Pre-release** on
   `aximcode/axl-sdk-releases`, tag `test-<YYYYMMDD>-<8-hex sha>`, with
   `--latest=false`. The notes name the commit and say it is a test build,
   not a release. The tag names a **commit of its own** on the releases
   repo, made through the git data API: `main`'s tree, `main` as its parent,
   a fresh date, and the axl-sdk commit in its message. GitHub orders
   releases by their tag's commit date, so a tag at `main`'s old head sorted
   a new test build below every release since; this one sorts at the top.
   `main` is read, never written; the release is created on that tag
   (`--verify-tag`), and a failed create deletes the tag it made (or, if
   that delete fails, prints the command that finishes it). A rerun that
   finds a leftover DRAFT on its tag -- a create that died while uploading --
   deletes it and goes on; a published release on the tag is refused, with
   `--update TAG` or `--delete TAG` named as the way forward.
6. Prunes: keeps the 3 newest test pre-releases (`--keep N`) -- newest by the
   tag commit's date, which is what GitHub orders by and what `--update`
   moves, so a release just refreshed in place is kept -- and deletes the
   older ones with their tags (each tag's own commit is then unreferenced).
   Only pre-releases tagged `test-<8 digits>-<8 hex>` are ever candidates.

What it does **not** do: push or tag axl-sdk, or touch CI. The tag exists only
on the releases repo and never matches `v*`, so no workflow fires; nothing in
`release.yml`, `docs.yml` or `ci.yml` runs. It is never "Latest", which matters:
`packaging/install.sh` is fetched from `releases/latest/download/`, and a test
release carries no `install.sh` or `VERSION`. The page's "Source code" links
are GitHub's archive of the releases repo at that tag -- the last official
source snapshot, not this build; the notes say so.

Test releases are published without asking; an official release (above)
stays ask-first. A run is two release builds (incremental when the tree
is already built) plus two QEMU boots of the release-image test, the aa64
one emulated.

## Recovery: a failed release tag

If a workflow fails on a freshly-pushed tag and **no GitHub Release
has been created yet**, it's safe to re-cut:

1. Verify no release exists: `gh release view vX.Y.Z` should print
   `release not found`.
2. Land the fix where the tag was cut from: `main` for a normal cut, **the
   release branch** for a `--branch` cut — it is what the tag will be re-cut
   from.
3. Move the tag:

   ```sh
   git tag -d vX.Y.Z
   git push origin :refs/tags/vX.Y.Z
   git tag -a vX.Y.Z -m "<original tag message>"
   git push origin vX.Y.Z
   ```

   On a release branch, `scripts/cut-release.sh X.Y.Z --resume --branch <branch>`
   does that re-cut for you **once the old tag is deleted** — it refuses while
   the tag still exists — and it dispatches the CI run, because `ci.yml` has no
   trigger for that ref and nothing else will start one. (`--branch` turns the
   gate on by itself; `--ci-gate` adds nothing there. `--resume --ci-gate` on
   `main` dispatches too: a push to `main` runs no suite.)

4. Watch the new workflow runs (step 6) and confirm (step 7).

If a GitHub Release **does** exist for the tag (i.e. release.yml
ran past the `gh release create` step before failing), re-cutting
the same version is no longer clean: consumers may have already
downloaded the partial artifacts. Bump to the next patch version
instead.

## Why these steps

The order is a recovery from each pitfall we've actually hit:

- `bump-version.sh` exists because a stale `axl-version.h` against
  `VERSION` fails the build (the CMake configure refuses it, as the
  Makefile's `check-version` did). Cut a release without it and the first
  build in CI fails; v0.2.6 was re-cut for exactly this reason.
- "Push main before the tag" exists because release.yml clones the
  *tag*, but the tag points to a commit that has to be reachable
  via the branch ref the runners pull from.
- "One commit for release metadata" exists because the tag-to-tag
  diff is the canonical changelog input on the next release.

## See also

- [`scripts/bump-version.sh`](../scripts/bump-version.sh) — the
  atomic version helper.
- [`.github/workflows/release.yml`](../.github/workflows/release.yml)
  — the tarball build and publish workflow.
- [`scripts/make-release-assets.sh`](../scripts/make-release-assets.sh)
  — every release archive and `SHA256SUMS`, by CPack over the install rules
  (`cmake/AxlInstall.cmake`, `cmake/AxlPackage.cmake`, `packaging/*/`), that
  `release.yml`, the release-image test and `make-test-release.sh` share.
- [`scripts/make-test-release.sh`](../scripts/make-test-release.sh) — test
  releases (above).
- [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) — the
  per-push cheap jobs, and the dispatched integration suite that gates a
  release.
- [`CHANGELOG.md`](../CHANGELOG.md) — what's shipped, by tag.
