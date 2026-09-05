# AXL-CI-Release-Speed-Design — cutting release wall time and Actions spend

Status: ACCEPTED 2026-08-13. Revised 2026-08-18 after measuring the mechanism
— see §10, which narrows the scope and DROPS §4.2.
Companion: `docs/AGT-CI-Cost-Handoff.md` in `aximcode/agt` — see §8.

Every number here was measured on the v3.2.0 cut (2026-08-13) or read from the
org's billing API. Nothing is estimated except where it says so.

## 1. What a release costs today

GitHub bills each JOB, rounded up to the minute. For v3.2.0:

| Workflow | Jobs | Billable |
|---|---|---|
| **CI** | clang-tidy 7, **QEMU integration 46**, gcc x64 2, gcc aa64 2 | **57 min** |
| Release | 5 jobs, longest 3m03s | 12 min |
| Docs | one failure (1) + one re-run (5) | 6 min |
| | | **75 min** |

Wall clock from `cut-release.sh` to a published release: **~60 minutes**, of
which 46 is one job.

## 2. The budget is org-wide, and both repos have breached it

Net charges in the billing API pin the included allowance at **~2,000
minutes/month** (GitHub Free, org-wide across private repos):

| Month | Minutes | Repo | Overage |
|---|---|---|---|
| 2026-04 | 2,069 | axl-sdk | **$0.57** |
| 2026-05 | 1,230 | axl-sdk | — |
| 2026-06 | 2,075 | agt | **$0.45** |
| 2026-07 | 704 | axl-sdk | — |

At 2-3 minor + 1 major release a month, axl-sdk's releases alone are ~260
min/month, and they compete with `agt` for the same pool. This is why §8
exists: fixing one repo hands its headroom to the other.

## 3. Root cause: the job is serial, and the fix is already written

The integration step ran **42m25s** (00:51:15 → 01:33:40). The sum of its 131
test durations is **2,541 s = 42.4 min**. Wall equals the serial sum:
parallelism 1.0x. The runner said so itself —
`Under -j1 a test commonly exceeds its est`.

`run-integration.sh` picks `nproc - 2` workers. A standard GitHub runner has 2
vCPUs, `2 >= 3` is false, so `JOBS=1`. The rule that gives a dev box 6 workers
gives CI one. KVM is **not** the problem: `ci.yml` already chmods `/dev/kvm`
and the guests are accelerated.

Meanwhile `run-integration.sh --shard i/K` already exists, balanced by each
test's `est=` with longest-processing-time-first packing, and
`make check-test-meta` already forces every test to declare one. The
parallelism is built, gated, and unused by CI.

Two bounds worth stating before the design:

- **Per-shard fixed cost ~2.5 min**: checkout, apt (19s), the `AXL_TLS=1` build
  + staged SDK (73s), runner start.
- **Floor ~6 min**: `test-cpu-spike-qemu.sh` alone is 179 s, so no shard is
  faster than that plus setup. Sharding past K=8 buys nothing.

## 4. Design

### 4.1 Runner selection — a `plan` job, not a hardcoded `runs-on`

> **STATUS: landed in a simpler form than this section describes.** There is no
> separate `plan` job. `ci.yml` selects with one `runs-on` expression that
> defaults to the self-hosted runner and falls back to `ubuntu-latest` when a
> `workflow_dispatch` passes `runner: hosted` -- which delivers this section's
> actual requirement (never a hardcoded runner; a fallback when the box is
> down) without a job whose only output is a label. The design below is kept
> for its reasoning, not as a description of the code.

The QEMU job targets `[self-hosted, linux, X64, axl-qemu]`. A `plan` job
(~1 billable min) decides where the work goes and how it is split, and emits
`runs-on` + the shard matrix as outputs:

```
workflow_dispatch input: runner = auto | hosted     (default auto)

auto    -> query the org runner API; if a runner labelled `axl-qemu` is ONLINE,
           plan { runs-on: self-hosted, shards: 1, jobs: 6 }
           otherwise fall through to hosted
hosted  -> plan { runs-on: ubuntu-latest, shards: 4, jobs: 1 }
```

The fallback is the point: a release must not queue forever because a
workstation is asleep. `cut-release.sh` reads the same signal and says which
path it is taking before it starts waiting.

Two constraints on the self-hosted path:

- **CI does not `apt-get` on a workstation.** The dependency step gets
  `if: runner.environment == 'github-hosted'`. The box already has every
  dependency — it runs this suite daily.
- `concurrency: group: axl-qemu-${{ github.ref }}` so two dispatches cannot
  fight over one machine.

Self-hosted on a PRIVATE repo carries none of the fork-PR risk that makes
self-hosted runners dangerous on public ones; there are no untrusted
contributors to run code as us.

### 4.2 Sharding, sized to where it lands

One job definition, matrix from `fromJSON(needs.plan.outputs.matrix)`:

| Path | Shards | `-j` | Integration wall | Billable |
|---|---|---|---|---|
| self-hosted (8-core, KVM) | 1 | 6 | ~9 min | 0 |
| hosted fallback | 4 | 1 | ~14 min | ~56 min |

The hosted path also buys the recovery behaviour that prompted this work:
`gh run rerun --failed` re-runs only the red shard, ~14 min instead of 46.
That is GitHub's native job-level re-run — no custom "which tests passed"
state, which is the version of this idea worth avoiding: a resume manifest
makes the green you tag on a union across runs, and it silently describes the
wrong code the moment someone pushes a fix and re-runs one shard.

Same-SHA re-run is sound. Anything else demands a full run, and
`cut-release.sh` will enforce that by gating on a run whose head SHA equals the
commit being tagged.

### 4.3 Gate policy — enforced, not documented

`RELEASING.md` already says the local suite is the authoritative gate and
`--ci-gate` is opt-in. The design makes that a rule the tooling applies:

- **`X.0.0`** → full CI gate (the cross-OS backstop, on the release commit).
- **minor / patch** → local gate.

The failure mode of "the local suite is the gate" is that it decays into "I ran
it recently". So `run-integration.sh` writes a stamp on a clean finish —
`test/integration/.last-run-stamp`, holding the HEAD SHA, the arch, the
pass/fail counts and a timestamp — and a minor cut REFUSES unless the stamp is
green and its SHA matches HEAD. The stamp is gitignored: it describes one
machine's run, not a property of the tree.

### 4.4 Reuse a CI run that is already green (free)

The `release: vX.Y.Z` commit touches exactly three files: `VERSION`,
`include/axl/axl-version.h`, `CHANGELOG.md`. If a successful CI run already
exists for its PARENT, and `git diff --name-only parent..release` is a subset
of those three, the gate is satisfied without dispatching anything.

That is the common case for a major: CI gets dispatched on `main` while the
changelog is being written, and by cut time the answer is already known. It
turns a 46-minute wait into zero without weakening what was tested — the only
delta is a version string.

## 5. Expected outcome

| | Wall | Billable |
|---|---|---|
| Minor (local gate) | **~5 min** | ~18 min |
| Major, self-hosted | **~12 min** | ~23 min |
| Major, hosted fallback | ~20 min | ~69 min |
| Major, reusing a green run (§4.4) | ~5 min | ~18 min |
| **Monthly, 3 minors + 1 major** | | **~90 min** vs ~260 today |

## 6. What this deliberately trades

- **A release depends on a workstation being up.** Mitigated by the auto
  fallback in §4.1, which costs minutes but never blocks.
- **Minor releases lose the fresh-machine dimension.** The same 145 tests run,
  on the same code, before the cut — but not on a clean Ubuntu image. §4.4
  partly buys this back: any CI dispatch on `main` during the month satisfies
  the gate for free.
- **Sharding costs minutes to save wall time** (+12 per hosted major). Accepted
  because the hosted path is the fallback, not the norm.

Explicitly NOT on the table: trimming or sampling the test set. The suite is
the gate; making it cheaper by running less of it is not a cost saving, it is a
coverage cut wearing one.

## 7. Rollout order

Each step is independently useful and independently revertible:

1. **§4.3 gate policy + stamp** — no infrastructure, immediately cuts 3 of 4
   releases a month down to ~5 minutes.
2. **§4.4 green-run reuse** — pure `cut-release.sh` logic, no workflow change.
3. **§4.1/4.2 plan job + sharding, hosted only** — verifiable without a runner
   (dispatch with `runner=hosted`), and it is the fallback path everything else
   leans on, so it should be proven first.
4. **Register the self-hosted runner** and flip the default to `auto`. Last,
   because it is the only step that cannot be validated from the repo alone.

## 8. The same problem in `agt`

`agt` is the other half of the allowance and the more urgent case: it bills on
**every push to `main` and every pull request**, across three workflows —
Tests (3 min), Docs (2 min), Visual (7 min) — so **~12 billable minutes per
push**. June's 2,075 minutes is roughly 170 pushes. axl-sdk fixed exactly this
by making `ci.yml` dispatch-only with the local suite as the gate; `agt` never
got that treatment.

The self-hosted runner in §4.1 serves both repos — the org already has a
`Default` runner group with `visibility=all`, so one registration covers them.

Details, measurements and a step-by-step for that repo:
**`docs/AGT-CI-Cost-Handoff.md` in `aximcode/agt`** (commit `0d43f34`).

## 10. REVISION 2026-08-18 — measured, and the scope narrows

Re-read against the goal as stated: **turnaround AND cost**, both. Three
findings change what is worth building.

### 10.1 The serialisation is not a repo defect — it is `nproc`

`run-integration.sh` already picks its own worker count:

```sh
_ncpu=$(nproc); if [[ "$_ncpu" -ge 3 ]]; then JOBS=$(( _ncpu - 2 )); else JOBS=1; fi
```

`ci.yml` passes no `-j`, so it takes that path. `JOBS=1` is what a 2-vCPU
hosted runner *evaluates to*, not something CI does wrong. On an 8-core box
the same unchanged line gives **`JOBS=6`**. §3's "the parallelism is built,
gated, and unused" resolves itself the moment the job runs somewhere with
cores — no workflow change, no shard matrix, no `plan` job.

### 10.2 §4.2 (hosted sharding) is DROPPED, because it trades cost for wall

§4.2's own table: the hosted 4-way shard is ~14 min wall at **~56 billable**,
against ~42 billable today. That is a wall-time win bought with MORE spend —
correct if wall time is the only goal, a regression once cost is a goal too.
Each shard re-pays checkout, toolchain install and the staged build.

Keep it only as an explicit opt-in for "this release is urgent and the box is
down". Do not make it the default fallback.

### 10.3 §4.3 and §4.4 are COMPLEMENTARY, and neither works alone

This is the pair that serves both goals at once, because the cheapest and
fastest run is the one never dispatched. They are easy to mistake for
alternatives; they are not:

- **§4.3's stamp** records that a SHA went green locally.
- **The release commit is a DIFFERENT SHA.** `cut-release.sh` bumps `VERSION`,
  `include/axl/axl-version.h` and `CHANGELOG.md`, so a naive stamp check fails
  at precisely the moment it is needed.
- **§4.4 is the bridge**: if `git diff --name-only parent..release` is a subset
  of those three files, the parent's green carries to the release commit.

Build them together or neither.

### 10.4 Revised priority

| step | turnaround | cost | verdict |
|---|---|---|---|
| §4.3 stamp + §4.4 parent reuse | skips the run entirely | 0 | **first** — no infrastructure |
| self-hosted runner | 42 min -> ~9 | 42 billable -> 0 | **second** |
| §4.1 plan job / fallback | prevents an indefinite block | ~1 billable/run | optional insurance |
| §4.2 hosted sharding | -28 min wall | **+14 billable** | **dropped** (see 10.2) |

§7's rollout order was right for this goal and this revision restores it. An
earlier reading of this document reordered registration to first; that
optimises wall time alone and is wrong once cost is a goal.

### 10.5 What a self-hosted switch still requires

Not obsolete, and small — but the first item is load-bearing:

1. The QEMU job's `Install dependencies (apt)` and `Enable /dev/kvm access`
   steps must become `if: runner.environment == 'github-hosted'`. Without the
   guard, CI runs `apt-get` against a workstation.
2. `concurrency: group: axl-qemu-${{ github.ref }}` so two dispatches cannot
   contend for one machine.
3. Availability: without §4.1's fallback, a release waits on the box being
   awake. That is the real trade, and it is a choice rather than a defect.

**Not written down anywhere: how to register the runner.** §7 step 4 names the
action; no procedure exists in this repo for installing the Actions runner,
labelling it `axl-qemu`, or running it as a service. That gap is the reason
this step "cannot be validated from the repo alone".

## 11. REVISION 2026-08-18 (second) — the constraint that shaped this is GONE

§1-§7 optimise around Actions minutes being scarce. With a self-hosted runner
they are not scarce, and the conclusions invert rather than merely improve.

### 11.1 What was measured

| | hosted | self-hosted |
|---|--:|--:|
| QEMU integration | ~50 min, 1 worker | **9m20s**, 6 workers |
| billable | ~42 min | **0** |

No repo change produced that. `run-integration.sh` picks `nproc - 2`, which is
1 on a 2-core hosted runner and 6 on an 8-core box.

### 11.2 The policy inverts: CI returns to running on push to `main`

CI was made dispatch-only to save minutes, with the local suite as the
authoritative gate. The cost of that policy came due on 2026-08-18: CI had last
been green on **2026-08-13**, ~40 commits earlier, and was red in three places
that nobody had seen. Two of the three are things a dev box **structurally
cannot** catch:

- a **clean checkout could not link** — `$(PORTING_OBJS)` were on no target's
  prerequisite list, invisible to any tree that already had them built;
- **docs broke under CI's older Doxygen** while the local gate reported clean;
- packaging dependencies had drifted.

Five days of undetected red is a worse trade than the minutes ever were. At
zero cost and nine minutes, CI runs on push to `main` again, with
`cancel-in-progress: true` so a burst collapses to one run.

### 11.3 Jobs run in a CONTAINER, which is what makes the runner portable

Running jobs directly on the host would make every runner a bespoke machine
and re-create the drift above. Each job takes `container: ubuntu:24.04` —
CI's own image — so:

- the environment is identical everywhere, and the Doxygen skew of §10.5
  disappears rather than needing a separate gate;
- a fresh container per run means warm-tree bugs cannot hide;
- `apt-get` inside the container is correct, so the
  `runner.environment == 'github-hosted'` guards are DELETED rather than kept
  as a permanent asterisk;
- a new host needs only Docker and the runner, not the full dev toolchain.

`--device /dev/kvm` passes acceleration through; verified working.

### 11.4 Docker, not podman — measured, not preferred

The runner shells out to the Docker CLI and API. Podman's compat socket got
container jobs running, but three divergences surfaced in the first ten
minutes, and the third is disqualifying:

| symptom | cause |
|---|---|
| `statfs /var/run/docker.sock: permission denied` | the runner bind-mounts the socket and stats it as its own user; podman's rootful socket is `root:root` |
| still denied after `SocketGroup=docker` | `/run/podman` is itself `0700`, so group access on the socket buys nothing without directory traversal |
| `statfs .../_work/_actions: no such file` | **Docker auto-creates a missing bind-mount source; podman does not** — and the runner only populates `_actions` for a job that uses an action, so any job without a `uses:` step cannot start a container |

The first two are fixable with a drop-in and a `tmpfiles.d` entry. The third is
a behavioural difference in the runner's own contract, and working around it
would mean constraining every workflow. **docker-ce is installed instead**
(29.7.2, `cgroup=systemd`, `runc`), which publishes for el10. Proven both
ways: a container job with a `uses:` step, and one WITHOUT — the case podman
refused.

Host requirements are therefore: Docker, the `docker` group, `/dev/kvm`, and
the runner. Nothing about the AXL toolchain.

### 11.5 The jobs are SEQUENCED, and the apt lists say what they mean

Two follow-ups to §11.3, both cheap and both measured.

**`needs: build` on `integration` and on `lint`.** The workflow had no `needs:`
at all, so all four jobs were eligible at once. The runner is one machine with
one job slot, so they were already serial — what was missing was any control
over the ORDER, and it went badly twice in a row:

| run | order | `gcc x64` finished |
|---|---|---|
| 32188096048 | clang-tidy, gcc aa64, **QEMU 9m16s**, gcc x64 | last, at 14m15s |
| 32190974374 | gcc aa64, clang-tidy, **QEMU 9m12s**, gcc x64 | last, at ~14m |

A broken x64 build therefore paid for the whole run and reported the compile
error *after* the QEMU suite had already failed on it. Gating both consumers on
`build` puts the ~2-minute compile of both arches first; a failure there now
ends the run there.

`needs: build` waits for BOTH matrix legs, which is what `fail-fast: false`
(§11.3) is for — x64 and aa64 each report, and only then are the dependents
skipped. `lint` is included on the weaker but sufficient ground that it
compiles the tree too (`bear -- make tests tools AXL_CPP=1`) and four of its
gates link images, so a rejected build fails it for the same cause three
minutes later. The cost is losing clang-tidy findings on a red-build run, which
the re-push returns.

**Not** `integration: needs: [build, lint]`, though it is tempting to order the
9-minute job behind the 3-minute one. That costs nothing on a single-slot
runner and permanently forbids the two running concurrently — which is exactly
what a second `axl-qemu` registration (§9) or the `-f runner=hosted` fallback
would otherwise buy. Sequence on the real dependency, not on duration.

**The apt lists are HOST tooling only.** Target code is 100% bare-metal
toolchain on both arches, so the question for each package is what runs on the
BUILD machine. Measured by dry-running the build job's own targets rather than
by reading the lists: across **463 recipe lines** per arch, exactly one
invokes an unprefixed host tool —

```
gcc -Wall -O2 -o <builddir>/pe-set-debug scripts/pe-set-debug.c
```

— and `aarch64-linux-gnu-` appears **zero** times for either arch.

- **`gcc` stays, in all three jobs.** It is `$(HOSTCC)`, and `pe-set-debug` is
  on every `.efi` link (`LINK_CRT0_CMD`). It is a native binary for the build
  machine, so no cross can produce it. This is the entry most likely to be
  deleted by someone tidying up after the hermetic migration, which is why each
  list now says so at the point of use.
- **`gcc-aarch64-linux-gnu` + `binutils-aarch64-linux-gnu` are gone** — 22
  packages, 43 MB, on every aa64 build. `CC` is ARM's bare-metal gcc and
  `CROSS` is `AXL_AA64_BINUTILS_PREFIX`; nothing has invoked the Linux cross
  since 119c8d76. `release.yml`'s package `Depends` had already made this trim;
  the workflow's own installs had not. Removing it emptied the `cross:` matrix
  key, which went with it.
- **`g++` is gone** from `lint` and `integration`. No host C++ compiler is
  invoked anywhere: `$(CXX)` is a bare-metal path and clang-tidy's C++ pass
  replays that compile database with clang. The one plausible need was
  `check-fuzz-link` — libFuzzer's runtime is C++ — and `clang` pulls
  `libstdc++-15-dev`, verified by linking and running a fuzz target in
  `ubuntu:26.04` with no `g++` installed.
- **`sudo` is gone** from `build`; the container is root and
  `install-toolchain.sh` checks `id -u` first.
- **`binutils` stays in `lint` and `integration`, and only there**, because
  only those invoke it unprefixed: `check-cxx-entry` runs `nm`, `check-no-avx`
  and `check-bss-clear` pick plain `objdump` for x86-64 objects, and four
  integration tests shell out to `nm` / `objdump` / `strings`. Dropping it from
  `build` is truth-in-labelling rather than a size win — `gcc` Depends on it,
  so it arrives regardless — but the comment it replaces claimed the build got
  `ld`, `ar` and `objcopy` from apt, and that stopped being true when x64
  binutils moved to our own toolchain.

### 11.6 Prose does not trigger CI, and the exclusion is deliberately narrow

`push: [main]` landed with no path filter, so a commit touching only design
docs and handoffs paid the full ~14-minute run — three of them in one evening,
every one triggered by a document *about* the CI. Free in dollars, not free in
noise: it keeps the runner busy while nothing real is under test, and it lets a
genuinely red run hide among doc-triggered ones.

The filter is **`paths-ignore: ['docs/**.md']`, and nothing else.** The obvious
spelling — `'**/*.md'` plus `'docs/**'`, which is what the task was originally
written as — silently disables three gates:

| pattern | what it would take with it |
|---|---|
| `'docs/**'` | **docs/sphinx/**: 104 `.rst` files that `check-doc-coverage.py` reads to decide whether every public header is wired into the docs. An `.rst` edit must still be verified. |
| `'**/*.md'` | **the root `README.md`**, whose CONTENT is asserted on by `test-toolchain-variant.sh` — "README documents no `make CROSS=` build command". That is an integration test, not a doc build, and it would go unrun on exactly the commits that can break it. |
| `'**/*.md'` | **the 33 `src/*/README.md`** files that Sphinx `.. include::`s into the module pages. |

The second is the one worth remembering: the trap generalises past the doc
build. "Prose" is not a synonym for "nothing depends on it" — a test can assert
on documentation, and this repo has one that does.

What the filter still costs, stated rather than hidden: `check-nul` scans every
tracked text file, `.md` included — its own docstring names `.md` as a target —
so a NUL byte landing in `docs/*.md` goes unseen until the next non-prose push.
It is a whole-tree scan rather than a diff, so nothing escapes permanently; the
detection is delayed, not lost. `workflow_dispatch` is not path-filtered, so
the manual backstop always runs everything, and `docs.yml` is untouched (tags
and dispatch only), so a docs change still publishes exactly as before.

## 12. REVISION 2026-08-19 — the LOCAL loop is the long pole

Everything above optimises CI. CI is now ~9m20s on our own box at zero cost
(§11.1), and it runs in parallel with whatever the developer does next. The
cost that is actually felt is the LOCAL pre-commit gate, which is paid serially
by a human, once per iteration, and which this document had never measured.

### 12.1 What the local gate costs, measured

Warm tree, each run **alone and serially** (concurrent runs distort both: a
`verify.sh` run during a build hit its own 95 s wall-clock ratchet at 102 s and
the aa64 unit suite went red on a timing test, neither of which reproduced
alone). All three green. 8-core box, `run-integration.sh` picks `nproc - 2` = 6
workers.

| | wall |
|---|--:|
| `scripts/verify.sh` (5 concurrent jobs) | **61 s** |
| `run-integration.sh --arch X64` (164 tests) | **569 s** |
| `run-integration.sh --arch AARCH64` (66 tests) | **274 s** |
| **total** | **904 s ≈ 15m04s** |

**The pool is saturated, so wall time is proportional to work.** Summed
test-time and the perfect-packing floor it implies, from the SAME runs as the
wall times above (an earlier run summed 3,396 s for X64; quoting one run's work
against another run's wall is how these two figures drifted apart in the first
draft of this section):

| | work | / 6 workers = floor | measured | efficiency |
|---|--:|--:|--:|--:|
| X64 | 3,385 s | 564 s | 569 s | **99%** |
| AARCH64 | 1,488 s | 248 s | 274 s | 90% |

Two consequences, and both matter below:

- **No scheduling win exists.** At 99% there is nothing for more workers,
  better packing or reordering to recover. Only running less work helps —
  which is why §12.6 is about skipping and not about scheduling.
- **Skipping converts ~1:1 into wall time.** Drop N% of the work and the wall
  drops about N%, as long as the run stays work-bound. It does not always
  (§12.10).

The X64 critical path is the total, not any one test: the longest single test
is 379 s, comfortably under the 564 s floor.

| slowest X64 tests | s | `local-only` |
|---|--:|:-:|
| `test-console-device-qemu.sh` | 379 | yes |
| `test-cpu-spike-qemu.sh` | 177 | no |
| `test-old-shell-qemu.sh` | 117 | yes |
| `test-9p-server-qemu.sh` | 106 | no |
| `test-kbtune-bounce-qemu.sh` | 87 | yes |
| `test-netload-qemu.sh` | 69 | no |

Checked and NOT true, so it is recorded rather than left to be re-suspected:
the integration suite does **not** re-run the unit suite. `discover_tests`
excludes `test-axl.sh` by name, and the timed X64 log contains no entry for it.

### 12.2 The measurement inverts the obvious fix

The intuitive target is `clang-tidy`: it was named in §9 as a ~7-minute CI job
(re-measured 2026-08-20 at **2 m 38 s**, and third of four — see §9; the old
figure was a hosted-runner number), and it is the long pole *inside*
`verify.sh`. Scoping it to changed files is easy, safe, and file-level.

It is also worth **at most 61 seconds locally**, because that is the whole of
`verify.sh` — 6.7% of the gate. **93% of the local cost is the integration
suite**, which has no change-scoping mechanism of any kind. Any work that does
not touch `run-integration.sh` is optimising the wrong 7%.

(clang-tidy scoping is still worth doing for CI, where the ratio is reversed.
It just is not the answer to "the local gate takes too long".)

### 12.3 What selection already exists

- **`verify.sh --only=JOB`** — job-level, and it already implements the rule
  this document adopts below: a filtered run never prints a bare `ALL GREEN`,
  it names what did not run.
- **`run-integration.sh --shard i/K`, `--arch`, `--ci`, `--list`** — splits
  work across machines; does not reduce it.
- **CI `paths-ignore: docs/**.md`** — narrow on purpose (§11.6).
- **Nothing maps source files to tests.** `# test-meta: needs=` looks like a
  dependency declaration and is not: it names external *tools* (socat, swtpm,
  gpu), so it gates availability, not relevance.

### 12.4 Prior art, judged against this tree rather than in the abstract

| approach | mechanism | verdict here |
|---|---|---|
| **Go's test cache** | hash the test's *observed* inputs; identical hash replays the cached PASS | **The fit.** `common-test.sh` already funnels every staged image through `test_add_efi`, so the harness sees the inputs without anyone declaring them. Safe by construction: it can only skip when the bytes under test are identical. |
| **pytest-testmon / Jest `--onlyChanged`** | per-test coverage map; re-run tests whose covered lines changed | **No.** Coverage does not cross the QEMU boundary, and the couplings that break this tree are link-time, not execution-time (§12.5). |
| **Bazel / Buck2** | declared file->target graph plus a remote cache | Correct for a monorepo, wrong cost for one Makefile. Borrow only the idea of a per-test declared edge — which `# test-meta:` is the natural home for if we ever want one. |
| **Meta predictive test selection** | model trained on historical failures | Needs a failure corpus this repo does not have. |

### 12.5 Why "changed files -> related tests" is specifically unsafe here

The worked example is the commit that prompted this section. `8af4e530` touched
`src/log/` and `Makefile`. A relevance map — by include graph, by directory, by
coverage — would have selected the logging tests.

The actual risk surface was **every image in the tree**, and the failure mode
was at **link** time: an image silently losing its log engine and discarding
every record. Nothing in an include graph or a coverage profile can see that
edge. It took a new artifact-reading gate plus the existing leak gate to cover
it, and a sabotage of one link macro took the *entire unit suite* red.

That is the general shape of this codebase's coupling. A relevance heuristic
would be wrong exactly where being wrong is silent.

### 12.6 The design: cache on what a test actually staged

Not relevance — **inputs**. On a green finish, record for each test the digest
set of everything it staged (`test_add_efi`), plus the test script itself, the
harness (`common-test.sh`, `run-qemu.sh`), the firmware image, and the
toolchain id. Next run, skip a test whose set is byte-identical.

This cannot produce a wrong green from a bad guess: the only thing it asserts
is that the bytes under test have not moved.

### 12.7 What it buys — **superseded by §12.15, which measured it**

> **This section's central claim is WRONG and the measurement is in §12.15.**
> It is kept because the reasoning error is worth seeing: "a `libaxl.a` change
> relinks nearly every image" conflates RELINKING with CHANGING. Every image is
> relinked; `--gc-sections` plus selective archive-member linking means most of
> them come out BYTE-IDENTICAL, because they never linked the code that
> changed. Measured, a core-library edit changes 44 of 118 artifacts and leaves
> **46% of the suite's work skippable**, not 0%.

**Nothing, on the most common commit.** A `libaxl.a` change relinks nearly
every image, so nearly every digest set changes and everything re-runs — which
is correct, and is also the answer for most of the work done here.

Where it pays:

- a `tools/*.c`-only commit, which leaves the library and every non-tool image
  byte-identical;
- a commit touching only one test script;
- a re-run after a flake, where nothing changed at all — today that costs the
  full 569 s to learn what one test does.

**The payoff distribution is the open question, and it is cheap to answer
before building anything**: log the digest sets for one full run, then replay
them against a few real historical commits (`git show --stat`) and count how
many of the 164 would have been skipped. If the answer for a tools-only commit
is not most of them, the feature is not worth its invalidation surface.

### 12.8 Reporting rule — decided

**A scoped or cached run never reports a bare green; it names what it did not
run.** `verify.sh --only` already does this and the reasoning generalises: the
failure this tree keeps meeting is a gate that cannot see a change reporting
the same green as one that can. A cached pass and a real pass must not look
alike in a terminal or in a CI log.

### 12.9 CI stays unrestricted on push

§11.2 records what the opposite policy cost: five days and ~40 commits of
undetected red, in three places, two of which a dev box structurally cannot
catch. A cache is only as good as its input capture — it will not see a
rebuilt OVMF, a harness edit outside the recorded set, or a toolchain bump.
The local gate gets faster; the push gate stays whole.

### 12.10 SHIPPED: `--only-local`, and what it actually measured

Before building the cache, the cheapest lever turned out to be an asymmetry
already encoded in the tree. `# test-meta: local-only=1` marks tests a CI
runner structurally cannot run, and `--ci` already excludes them. **X64: those
are 14 tests and 733 s of the suite's 3,385 s.** The other 2,652 s -- 78% of
what a developer waits for -- is work CI repeats on every push to `main`, on
our own box, for free, minutes later.

`run-integration.sh --only-local` is the inverse of `--ci`: run only what a
push will not tell you.

| | before | after |
|---|--:|--:|
| `verify.sh` | 61 s | 61 s |
| integration X64 | 569 s | **382 s** |
| integration AARCH64 | 274 s | **52 s** |
| **total** | **904 s** | **495 s** |

**-409 s, -45%.**

**The estimate was 122 s and the measurement was 382 s, and the gap is the
useful part.** 733 s over 6 workers predicts 122 s -- but that arithmetic
assumes the run stays WORK-bound, and dropping to 14 tests makes it TAIL-bound:
`test-console-device-qemu.sh` alone is 379 s, so `max(733/6, 379) = 379`.
Measured 382.

So the full suite and the scoped suite have different critical paths and want
different fixes. The full suite is work-bound at 99% packing efficiency (§12.1)
and only responds to doing less. The scoped suite is one test:
**`test-console-device-qemu.sh` is 11% of the whole suite's work and 100% of
the scoped run's critical path.** Splitting or shortening it takes the inner
loop to roughly 230 s; nothing else moves it at all.

**It is not a pre-push gate, and it says so.** Per §12.8 the run prints
`integration: PARTIAL -- only-local: ran the N test(s) CI cannot, SKIPPED
everything CI does run.` It is also excluded from the release-gate stamp
(§10.3) -- and so is `--ci`, which could previously write a "complete run"
stamp having skipped every local-only test. That was the same defect this flag
would have introduced, one flag earlier.

### 12.11 Instrumented: the suite is guest boots, and nothing else

`AXL_TEST_PROFILE=<file>` turns on per-boot instrumentation
(`test/integration/lib/profile.sh`, inert otherwise -- **568.15 s profiled vs
569 s not**, and no file is written when it is off).
`lib/profile-report.py` reads it.

X64, one full run:

| | |
|---|--:|
| guest boots | **226** |
| suite test-time | 3,385 s |
| mean per boot | **15.0 s** |
| directly-timed boots | median **12.1 s**, min **6.8 s**, max 60 s |

**Essentially 100% of the suite's cost is guest boots.** `hello-minimal` boots
once, does almost nothing, and takes **7.05 s** -- so ~7 s is the floor per
boot (QEMU start, OVMF init, shell, `startup.nsh`, reset). **226 x 7 ~= 1,582 s,
47% of the suite, is boot overhead that tests nothing.**

The count read 233 until 2026-08-19: the record was emitted where the QEMU
command is ASSEMBLED, which is one point every launch passes through and is
still not the same as a launch. `QEMU_DRYRUN=1` assembles and exits, and so do
the late validation failures -- so `test-run-qemu-flags.sh`, host-only argument
parsing that boots nothing at all, was credited with SEVEN boots in ONE second.
That impossibility is what exposed it. The record now sits after the dry-run
exit, below which every real launch happens. The correction was confined to
that one test (7 -> 0) and moved no conclusion.

Where the boots are:

| | tests | boots | seconds |
|---|--:|--:|--:|
| boot exactly once | 117 | 117 | 1,786 |
| boot more than once | **29** | **116** | **1,446** |

Collapsing every multi-boot test to a single boot would take 233 -> 146 boots
and reclaim ~600 s of pure overhead. Ten tests hold >=4 serial boots, and one
dominates: **`test-console-device-qemu.sh` holds 15 boots at 25.3 s each** --
2x the median, because it runs DEBUG OVMF deliberately (it asserts on firmware
ASSERTs). It is also `local-only=1`, so **CI never pays for it and the whole
379 s is a local-only cost, on every run.**

The caveat that stops this being a global fix: those 116 boots are mostly not
redundant. They load different drivers or need clean firmware state, which is
*why* they are separate boots. Merging is per-test work, not a sweep.

#### 12.11.1 The first instrumentation was wrong and looked complete

It hooked `test_run_background`/`test_wait_for` and reported "41 boots across
41 tests, 521 s of guest time" -- a confident, well-formatted report covering
**25% of tests and 15% of the work**, with `test-console-device-qemu.sh`, the
single most expensive test, absent entirely.

There are **three** general paths from a test to a guest, and they are
disjoint:

| path | tests | seam |
|---|--:|---|
| `test_run_background` + `test_wait_for` | ~41 | timed exactly |
| `test_run_foreground` | many | timed exactly (starts and ends in one call) |
| `scripts/run-qemu.sh` | 71 | **counted only** |

`run-qemu.sh` is counted rather than timed because it installs and REPLACES an
EXIT trap in four places; a fifth for the stop stamp would clobber a cleanup
that removes the guest's TMPDIR. Seconds-per-boot for that path is derived from
the runner's own per-test time, and the report marks those figures `~` rather
than presenting them as measured.

A fourth path is private to `test-crashhandler.sh`, which assembles its own
QEMU command (it strips KVM, because the crash handler needs a real `#GP`).
Left uninstrumented and recorded in `profile.sh` instead -- hooking a private
command assembly is a worse trade than saying so.

**The report now prints a `recorded NO boot` section**, so the next gap
announces itself instead of reading as a fast test. Host-only tests (a link
probe, an `axl-cc` flag check) legitimately appear there; a `*-qemu.sh` test
appearing there is a bug in the instrumentation. That section is what found the
`test_run_foreground` gap.

#### 12.11.2 `--affected` — association by artifact, working today

    python3 test/integration/lib/profile-report.py prof.txt \
        --affected out/native-x64/hello.efi

Maps changed build artifacts to the tests that stage them, using the `efi|`
records (destination + sha256 captured at `test_add_efi`). This is §12.6's
association question answered from data rather than from a directory map, for
the reason in §12.5 -- and it prints artifacts that **no** profiled test stages,
because "cannot be mapped" must be visibly different from "affects nothing".

### 12.15 Phase 3 answered: the cache is worth building

§12.7 asserted the digest cache buys nothing on a library change, which is most
commits, and that assertion is what kept it below the line in every ranking
here. It is wrong. Measured by perturbing one source, rebuilding, and diffing
the digests of the 118 artifacts the suite actually stages:

| change | artifacts changed | tests that must run | skippable |
|---|--:|--:|--:|
| tool only (`tools/grep.c` help string) | 2 of 118 | 3 | **146 (86%)** |
| library (`src/mem/axl-mem.c` string) | 44 of 118 | 76 | **73 (43%)** |

Weighted by WORK rather than test count, which is what wall clock follows:

    library change:  3,361 s total
                     1,725 s must run
                       101 s cannot be proven unaffected (20 tests stage nothing)
                     1,535 s SKIPPABLE  = 46% of the work
                     => full X64 560 s -> 304 s

**The error was conflating RELINKING with CHANGING.** Every image is relinked
when `libaxl.a` changes. Most come out byte-identical, because
`--gc-sections` plus selective archive-member linking means an image that never
linked the code that changed contains the same bytes it did before. Only 44 of
118 artifacts moved.

That makes the cache the largest remaining win by a distance -- bigger than
everything shipped in §12.10, §12.13 and the rollout combined, and it is the
only one that touches the FULL run, which §12.1 showed is work-bound and
therefore only responds to running less.

#### 12.15.1 What had to be fixed to measure it at all

The artifact capture was recording **82 of 169 tests**. `test_add_efi` is
common-test.sh's staging path, and the 71 tests that reach a guest through
`run-qemu.sh` pass their `.efi` positionally, so `run-qemu.sh` staged it and
nothing recorded it -- the same disjoint-path gap that made the first boot
instrumentation cover a quarter of the suite (§12.11.1). Recording `$EFI_FILE`
alongside the boot record takes it to **149 of 169**. The remaining 20 are
genuinely host-only (link probes, `axl-cc` flag checks, the CMake package) and
stage nothing; they can never be proven unaffected and must always run. That
is 101 s of the 3,361 s, and it is the cache's floor.

#### 12.15.2 Two no-op perturbations, and what they cost

The first two attempts measured nothing and said so convincingly: **0 changed
artifacts** for a `tools/grep.c` edit, then **0** for a `src/data/axl-str.c`
edit. Both inserted a COMMENT, which emits byte-identical code. Read quickly,
the first reading confirmed §12.7's claim -- a library change that appears to
invalidate nothing -- for entirely the wrong reason.

`sabotage.sh --expect-fail` exists to refuse exactly this, and it was not in
play because this is a MEASUREMENT rather than an assertion, so there was no
"the suite must notice" to invert. The lesson generalises past sabotage: **a
perturbation used as a measuring instrument needs its own proof that it
perturbs something.** Changing a string literal does; changing a comment does
not.

#### 12.15.3 What the number is NOT

One library file, one commit shape. `axl-mem.c` is linked by 64 of the members
in a do-nothing image, so it is a WIDELY linked file, but it is still one
sample -- a public-header change that alters a struct layout would invalidate
more, and a leaf module fewer. The distribution across real commits is not
measured, and §12.12 keeps that as the next step rather than assuming this
number generalises.

### 12.16 SHIPPED: the skip cache — full X64 564 s -> 78 s

`run-integration.sh` skips a test whose inputs are byte-identical to its last
GREEN run (`test/integration/lib/test-cache.sh`). **On by default since the
same day it shipped**, and the reason is behavioural rather than technical: it
was built opt-in, and then not used once in the session that built it -- the
full uncached gate was run after every change instead. A flag that the author
does not type is not going to be typed by anyone.

`--no-cache` turns it off, and that is the run a pre-push or release gate must
use: **only an uncached run writes the release-gate stamp**, and `--ci` implies
`--no-cache` because CI is the backstop (§11.2 records five days of undetected
red when it was not run) and a backstop that skips is not one. `cut-release.sh`
names `--no-cache` when it finds no usable stamp, so the fix does not read as
"run it again" -- which would be cached too.

| | no cache | `--cache`, warm |
|---|--:|--:|
| full X64 | 564 s | **71-78 s** (136 cached, 34 ran) |
| `--only-local` X64 | 133 s | **24 s** |

Three independent warm runs: 77.6 s, 72.7 s, 70.9 s. The spread is ambient load
on a shared box, not variance in the cache. The no-cache baseline reproduces to
**564.06 / 564.10 s** across runs half an hour apart, which is what makes the
pair trustworthy -- a contaminated measurement does not land 0.04 s from an
independent one.

**The order of operations is the design**, because what a test stages is only
knowable by running it -- there is nothing to hash before the first run. Run 1
records each input as the test stages it and commits a key on green; run N
re-hashes the RECORDED list. The list stays valid only while the test would
stage the same things, which is why the test script and the harness are in the
key: change either, the key misses, the test runs, and the list is rewritten.
Go's test cache in miniature -- observed inputs, never declared ones.

#### 12.16.1 The soundness demonstration

Not "it got faster", which any broken cache achieves. Perturb `tools/grep.c`,
rebuild, re-run the full suite against a warm cache, and see WHICH tests wake
up:

    test-axl-busybox.sh   (embeds grep.efi)
    test-shell-pipe.sh    (pipes through it)
    test-tools.sh         (tests it)

Three, and exactly the three that stage `grep.efi`. Everything else stayed
cached, and the run was 170/0.

`test-test-cache.sh` holds the property tests -- 13 assertions, and every one
except the two controls asserts a MISS, because the dangerous direction is a
false hit: a changed artifact, test script, `common-test.sh`, `run-qemu.sh`,
any `lib/*.sh`, a different arch, a different `AXL_TLS`, a vanished input, no
record at all, and a key dropped after a red run. The two controls exist
because a cache that never hits is trivially sound and useless, and would pass
a file made only of miss assertions.

#### 12.16.2 What it does NOT cover, and the 37 that always run

The key covers every staged artifact by SOURCE path and digest, the firmware
(`FW_CODE`/`FW_VARS` are recorded like any other input), the test script, the
harness, the arch and `AXL_TLS`. A toolchain change is covered transitively --
it changes the artifacts.

It does NOT cover the host environment: `nproc`, `/dev/shm`, network
reachability, the version of python/tar/socat a test shells out to. A test
whose result is not a pure function of the tree cannot be seen to change. That
is why the cache is opt-in, why a cached run prints
`integration: PARTIAL -- --cache: N test(s) SKIPPED`, and why it is excluded
from the release-gate stamp alongside `--shard`, `--ci` and `--only-local`.

The 37 that ran on a warm no-change suite are the honest floor, and each is a
"cannot prove", never a "probably fine":

- **20 host-only tests** stage nothing (link probes, `axl-cc` flag checks, the
  CMake package). Nothing to hash, so nothing to compare.
- **3 stage a temp file that is gone by key time**, so `cache_key` refuses to
  answer -- a missing input reads as a miss, never a hit.
- **`test-json-corpus-qemu.sh`** SKIPs when the external corpora are not cloned,
  so it boots nothing and records nothing.
- the rest are tests whose artifacts genuinely moved.

#### 12.16.3 A measurement-hygiene note that cost real time

Two of these numbers were re-taken because an orphaned `sphinx-build` was found
running at 48% CPU. It came from a command of mine being interrupted mid-flight:
the shell died and `sphinx-build -j auto` reparented to init, where it competes
for all 8 cores. `scripts/build-docs.sh` runs TWO of them concurrently (html and
man), so a stray one is a heavy contaminator, and `verify.sh` does not leak one
when it completes normally -- verified.

The sweep then made it worse: `pkill -f 'sphinx-build'` matches its OWN command
line and killed the shell running it. Kill by PID, from a `ps | grep '[s]phinx'`
bracket pattern.

**Before quoting a wall-clock number here, check `ps` for strays and quote more
than one run.** The reproducibility above is the evidence, not the environment.

#### 12.16.4 Both numbers are real, and they answer different questions

78 s is a **no-change re-run** -- the flake retry, the "did I break anything
with that doc edit" loop. After a real library change the figure is the §12.15
one, ~304 s, because 46% of the work is skippable and the rest is not. Quoting
78 s as the cost of a library change would be the same category error §12.13.1
already paid for twice.

### 12.17 NOT SCHEDULED — caching `verify.sh`'s jobs, if the clock is felt again

Recorded as a level to reach for later, deliberately not built. **Trigger: pick
this up only if the local gate starts feeling slow again.** Measured
2026-08-19, each job alone against all five concurrently:

| job | alone |
|---|--:|
| x64 unit suite | 49.1 s |
| aa64 unit suite | 47.7 s |
| docs | 43.5 s |
| lint | 30.3 s |
| make gates | 26.6 s |
| sum | 197.2 s |
| **all five concurrently** | **61.2 s** |

**`verify.sh` is CONCURRENCY-bound, and that is the whole analysis.** Wall is
61 s against a 197 s sum, and only 12 s above the longest single job. So
skipping a job buys nothing unless it is the one on the critical path -- the
opposite of the integration suite, which is WORK-bound at 99% packing (§12.1)
and where skipping converts ~1:1. Same distinction, opposite conclusion; it is
worth re-deriving rather than assuming, because the two look alike.

What a path-based `--changed` would buy, by commit shape:

| shape | jobs that can see it | wall |
|---|---|--:|
| docs-only | docs | ~43 s (-18) |
| test-only | make | ~27 s (-34) |
| `src/*.c` | x64, aa64, lint, make | ~55 s (**-6**) |

6-34 s, and the COMMON case is the worst one: the two unit suites are the
longest jobs and every source change touches them.

**A correction this section exists to record.** §12.2 ranked this work last
because it was "worth at most 61 s, 6.7% of the gate". True then. The inner
loop is now ~85 s and `verify.sh` is ~85% of it, so the reason for the ranking
has expired even though the 61 s has not moved. That is the third time in this
arc that removing the largest cost reshuffled what mattered -- the pattern is
worth more than any of the individual numbers.

**If picked up, extend the §12.16 cache rather than building a `--changed`.**
The two unit suites are 49 s and 48 s of the critical path and are structurally
identical to an integration test: one QEMU boot whose inputs are the test
`.efi`s, the firmware and the harness. If `libaxl` is unchanged those binaries
are byte-identical and the run is skippable on the same proof -- measured
artifacts, never guessed relevance (§12.5). Expected: both arch jobs cached
takes wall to `max(docs 43, lint 30, make 27)` ~= 43 s; docs is cacheable too
(its inputs are the public headers plus `src/*/README.md`, both hashable), which
would give ~30 s, and an inner loop of ~40 s.

**Why it is not built now.** The risk is materially larger than for
integration: skipping a unit job means not executing **10,497 assertions**, so
a key that is subtly wrong hides a real regression instead of merely costing
time. Integration tests fail loudly and individually; a skipped unit suite is
silent. It would need the §12.8 treatment (PARTIAL, never a bare green,
excluded from the release-gate stamp) plus no-cache-by-default in CI, and the
payoff -- 20-45 s on a gate that is already ~85 s -- does not currently justify
that care.

### 12.18 The x64-only census, audited — and two silent-coverage fixes

§12.14 said the X64/AARCH64 asymmetry is COVERAGE and declined to say how much
of it is legitimate, because the obvious grep was caught missing a reason
spelled `DEBUG-OVMF` with a hyphen. Audited properly:

| | |
|---|--:|
| tests declaring `arch=x64` | 102 |
| header states an architecture reason | 74 |
| no stated reason, but HOST-ONLY (0 boots) -- `arch=x64` just means "run once" | 7 |
| **guest tests, x64-only, no stated reason** | **21** |

21, not the 65 the first grep claimed. The 7 host-only ones are not an
asymmetry at all: a link probe or an `axl-cc` flag check has no architecture,
and `arch=x64` is how the runner is told to run it once.

**Three of the 21 were tested on aa64 and all three passed unchanged** --
`test-time-qemu.sh` (21/0), `test-yield-ctrlc.sh`, and
`test-console-readline-qemu.sh`, which already printed `PASS (AARCH64)`. So the
arch support was there and only the `test-meta` line said otherwise. All three
are now `arch=both`; aa64 selects 71 tests where it selected 68.

That is a sample of 3, and it is evidence about the sample and not a claim
about the remaining 18. It does establish the category though: **some of this
residual is "never ported", not "unportable"**, and a per-test check is cheap
(run it with `--arch AARCH64` and read the exit code). The remaining 18:

    ata  axbench-ctrlc  cpu-topology  https-driver  input-modifiers  net-config
    nic  nvme  scsi  sendkey-render  shell-coexist  shell-fv  shell-launcher
    smart  tcp-close-pendtx-driver  tcp-multi-transmit  ws-broadcast-tls
    ws-teardown-driver

Several plainly depend on emulated devices (`nvme`, `ata`, `scsi`, `nic`) whose
aa64 availability is a real question rather than an oversight. Nobody should
flip one without running it.

#### 12.18.1 A skipped test was being counted as a pass

`exit 0` was the convention for "this test declined to run", so the suite
counted it among its passes. **39 of 176 tests can take that path.** On this
machine exactly one does: `test-json-corpus-qemu.sh`, whose corpora are not
vendored -- so a run that tested no JSON corpus at all reported the same green
as one that tested every document in it.

`run-integration.sh` now scores **exit 77** (the automake convention) as SKIP:
its own verdict, neither PASS nor FAIL, named in the totals
(`N passed, M failed, K SKIPPED`) and listed underneath. It is handled BEFORE
the retry (a test that cannot run will not run better the second time) and
before the cache (the reason it skipped is not in the key, so committing one
would skip it forever afterwards).

Converted: `test-json-corpus-qemu.sh`. The other 38 are latent -- they only
matter on a machine missing the dependency -- and adopt 77 as they are touched
rather than in a sweep. `test-runner-selftest.sh` asserts the mechanism with a
stub that exits 77, including that it is scored neither PASS nor FAIL.

### 12.12 Rollout — revised by what §12.10 and §12.11 measured

**Tracked as phases in [ROADMAP.md](ROADMAP.md) → "Local gate wall time".**
That file owns what is done and pending; this section owns the design and the
measurements. Each phase updates this section with what it MEASURED rather than
what it intended — the two have already diverged twice here (§12.10's 122 s
estimate against a 382 s measurement, and §12.11's first instrumentation
covering a quarter of the suite while looking complete), and both times the
divergence was the useful part.


**Done:** `--only-local` (§12.10), 904 s -> 495 s with no new machinery; the
boot instrumentation (§12.11), which turned "where does the time go" from a
guess into a table; and Phase 1 below. §12.11 also subsumed rollout step 3 of
the previous revision -- digest recording already exists, as the `efi|` records.

Ranked by what the measurements say, not by what looked obvious first:

1. [DONE] **`test-console-device-qemu.sh` split four ways.** It held 15 serial
   DEBUG-OVMF boots at 25.3 s each. The scenarios cannot be MERGED -- each loads
   a different driver at the shell prompt and screenshots it -- so they are
   split, and the split is sized rather than guessed: the local-only set is
   ~733 s of work over 6 workers, a 122 s floor, so the longest piece only has
   to get under that. **Four is the first split that reaches it** (largest
   piece ~107 s); a fifth would buy nothing.

   | | before | after |
   |---|--:|--:|
   | `--only-local` X64 | 382 s | **194 s** |
   | `--only-local` aa64 | 52 s | 51 s |
   | full X64 | 569 s | 573 s |
   | full aa64 | 274 s | 257 s |
   | **inner loop** (`verify.sh` + both `--only-local`) | **495 s** | **306 s** |

   All 42 assertions preserved exactly (12+12+9+9, against 11 `run_scenario` x3
   plus 2 serial scenarios x2 and two inline at 3+2). The full run is unchanged
   because it is work-bound (§12.1) and splitting moves work rather than
   removing it -- predicted, and confirmed at 573 s against 569 s.

   **The exit criterion was <= 300 s and it landed at 306 s.** Recorded as a
   miss rather than rounded: 1.9% short. The cause is that the binding
   constraint moved again. With 17 local-only tests over 6 workers the pool
   packs lumpily -- six tests are now >= 87 s (`old-shell` 117, `fbcon-life`
   107, `takeover` 101, `fbcon` 97, `input` 92, `kbtune-bounce` 87) and fill
   every worker at once, so the run measures 194 s against a 125 s work floor:
   65% packing, where the full run gets 99%. It is no longer one test, and
   Phase 2 clears the remainder as a side effect.
2. [DONE, and not what was planned] **Longest-first scheduling.** Phase 2 was
   going to be "split `test-old-shell-qemu.sh` next". Reading the code first
   found something cheaper: the pool consumed tests in DISCOVERY order, i.e.
   the glob's alphabetical one, which is arbitrary with respect to cost. Feeding
   a fixed job set to N workers is makespan scheduling, where
   longest-processing-time-first is the classic greedy and is within 4/3 of
   optimal; arbitrary order has no bound at all, because a long test that starts
   last runs alone while five workers idle. A `sort` on the `est=` every test
   already declares:

   | | before | after |
   |---|--:|--:|
   | `--only-local` X64 | 194 s | **134 s** |
   | `--only-local` aa64 | 51 s | 51 s |
   | full X64 | 573 s | 571 s |
   | full aa64 | 257 s | **244 s** |
   | **inner loop** | 306 s | **246 s** |

   **The scoped run went from 65% to 93% packing** (134 s against its 125 s
   floor) with no test changed, which is the best ratio in this whole exercise.
   The full run does not move, and that was predicted rather than discovered:
   §12.1 already measured it at 99%, because with 164 tests there is always
   small work left to fill a gap. Order only decides the makespan when the job
   set is small and lumpy -- which is exactly what `--only-local` is.

   It also clears Phase 1's 6 s miss: the inner loop is now **246 s** against
   that phase's <= 300 s exit.

   Checked and NOT done: refreshing `est=` from the profiler's measurements.
   Only 2 of 169 are off by >= 25 s and both are OVER-estimates, which LPT
   handles safely (it just starts them early). The churn would have bought
   nothing.
3. **The other multi-boot tests** -- `test-old-shell-qemu.sh` (117 s, 6 boots)
   is the longest that remains. 29 tests hold 116 of the 233 boots.
   **The exit number for this one needs restating**, because Phase 1 proved
   splitting cannot deliver it: the full run is work-bound at 99%, so splitting
   MOVES work between workers and removes none. Only MERGING boots reduces the
   full run, and those boots mostly exist for state isolation -- each loads a
   different driver or needs clean firmware -- so merging trades wall clock for
   cross-contamination and worse diagnosis. Splitting still helps the SCOPED
   run, but after LPT that is already at 93% packing, so the remaining headroom
   there is ~9 s. **Recommendation: stop here unless a specific test can shed
   boots without losing isolation.**
3. [DONE -- §12.15] **The digest cache's payoff, measured.** The answer
   reverses the ranking: a library change leaves **46% of the work skippable**
   (full X64 560 s -> 304 s), and a tool-only change 86% of tests. §12.7's
   "buys nothing on a libaxl change" conflated relinking with changing.
4. **BUILD THE SKIPPING HALF**, with §12.8's reporting. Now the largest
   remaining win by a distance, and the only one that touches the FULL run.
   Needs, beyond what §12.15 already records: the test script, the harness
   (`common-test.sh`, `run-qemu.sh`), the firmware image and the toolchain id in
   the key -- an artifact digest alone does not see those. The 20 tests that
   stage nothing must always run; that is the floor and it is 101 s.
   Before building, sample more commit shapes (§12.15.3): one widely-linked
   library file is one data point, not a distribution.
5. **clang-tidy scoping** for CI, separately. Deliberately last: it is the
   intuitive target and §12.2 shows it is worth at most 61 s locally.

**Not on this list: making boots cheaper.** At a ~7 s floor and a 12.1 s
median, halving per-boot cost would beat everything above -- but `run-qemu.sh`
already skips the Boot Manager countdown, and ~7 s is close to what an OVMF
boot costs. Recorded so it is not re-proposed as an obvious win without a
measurement behind it.

### 12.13 Phase 3 — merging boots, and why the payoff kept shrinking

Splitting helps the scoped run; only MERGING boots helps the full one (§12.1 is
work-bound). `test-tar-qemu.sh` was the pilot: **7 boots -> 1, 50 s -> 7.3 s**,
all 11 assertions preserved plus a new one asserting the guest reached the last
step.

It merged cleanly because nothing there needed isolation. Every boot mounted the
SAME host directory and differed only in `tar.efi`'s arguments, and the
scenarios were already coupled THROUGH that mount -- step 2 lists what step 1
wrote, step 7 extracts what step 5 wrote. The dependency was never the boot; it
was the filesystem. One shell session running them in order preserves it
exactly.

#### 12.13.1 The estimate fell twice, and the reason is the useful part

| estimate | basis | full-X64 saving |
|---|---|--:|
| first | all 32 multi-boot tests merge; 84 boots at the 15 s mean | ~200 s (**-35%**) |
| second | keyword split + hand audit; ~50 boots at the 15 s mean | ~125 s (**-22%**) |
| **measured** | tar's 6 boots removed = **7 s of wall**; extrapolated | ~50 s (**-9%**) |

**The boots you can remove are the cheap ones.** That is the whole correction.
Tar's boots were ~7 s each -- a trivial payload paying full firmware boot --
while the 15 s suite mean is dragged up by boots that are NOT candidates:
`console-device` at 25.3 s (DEBUG OVMF, one driver each), `cpu-spike` at 44 s
(the boot is doing real work). Expensive boots are expensive *because* they do
something, and the something is usually what forbids merging. Applying a suite
mean to a self-selected cheap subset is what produced both earlier figures.

Measured end to end: full X64 **571 s -> 564 s**, boots **226 -> 220**.

#### 12.13.2 What it costs, and the recommendation

A merged test loses per-scenario isolation: a guest-side failure in an early
step leaves later steps running against missing inputs, so one fault can print
several failures. Tar mitigates it with per-step markers, a per-step transcript
slice, and an explicit "reached the last step" assertion that fires FIRST -- so
a dead boot reads as one dead boot rather than six tar bugs. That mitigation is
per-test work; it is not a sweep.

**Recommendation: stop here, or cherry-pick.** The remaining ~19 candidates buy
about **43 s** of wall, on a run performed once before a push, at the cost of
19 test rewrites each carrying that cascade risk. The pilot was worth doing --
`test-tar-qemu.sh` is now 7.3 s instead of 50 s and reads better -- but the
marginal ones are not, and the inner loop (§12.10, §12.12) is where a developer
actually feels the clock.

A trap worth recording from the pilot: the shell echoes a command BEFORE its
output, so the transcript carries `FS0:\> echo TARSTEP:2` and then `TARSTEP:2`.
A `sed` range ending at the next `/TARSTEP:/` closes on the echoed marker
itself and returns one line -- which read as "tar -t listed nothing", a guest
bug that did not exist. Slice from the bare marker line to the next command
echo.

### 12.14 Why X64 takes 2.3x AARCH64, which is not a speed difference

The natural reading of `X64 564 s / AARCH64 244 s` is that x64 is slower, and
the natural objection is that it should be the other way round -- aa64 runs
under TCG with no KVM. Both readings are wrong, in opposite directions.

| | X64 | AARCH64 |
|---|--:|--:|
| tests selected | 169 | **68** |
| guest boots | 220 | **80** |
| test-time (work) | 3,385 s | 1,488 s |
| wall | 564 s | 244 s |
| per boot, mean | 12.6 s | **14.4 s** |
| per boot, median | 12.2 s | 12.1 s |
| per boot, min | **6.8 s** | **11.4 s** |

**Per boot, aa64 IS slower** -- 14% on the mean and **68% at the floor**, which
is the TCG penalty showing up exactly where it should. x64 simply runs 2.75x
the boots. Work is 2.3x and wall is 2.3x, matching to the digit because both
arches are work-bound at 6 workers (§12.1).

The medians are identical (12.1 vs 12.2), and that is the informative part.
Emulation costs CPU, so it is visible in the FLOOR -- firmware init, shell
startup, the parts that are pure computation. A median boot is dominated by
waiting on its payload, where the two arches converge. **Emulation is expensive
for cheap boots and nearly free for expensive ones.**

So the asymmetry is COVERAGE: 101 tests declare `arch=x64`, 60 declare `both`,
and **none declares aa64**. aa64 exercises 40% of the suite.

How many of those 101 are legitimately x86-bound (KVM, virtiofs, DEBUG OVMF,
Shell106, the MS x64 ABI) is NOT recorded here, because the obvious way to
count it does not work: a keyword grep over the headers reported "65 state no
reason" and was then caught missing the stated reason in the four
console-device files, which spell it `DEBUG-OVMF` with a hyphen. A number that
wrong in its first spot-check is not worth quoting; establishing it needs a
per-test read.

One consequence worth knowing before anyone "balances" the arches: parity would
make aa64 the SLOWER run, not a matching one -- 220 boots at a 11.4 s floor
against x64's 6.8 s.

## 9. Open questions

- ~~**Does the hosted shard count want to be 4 or 6?**~~ **MOOT.** §10.2 dropped
  hosted sharding outright (it trades billable minutes for wall time, and the
  self-hosted runner removed the problem it solved). The question outlived the
  approach it belonged to; kept struck through rather than deleted so nobody
  re-derives it.
- ~~**OPEN — should `clang-tidy` (7 min, container `apt` every run) cache its
  image?** Now the largest CI job.~~ **ANSWERED 2026-08-20: NO.** Both halves of
  the premise were stale, which is why measuring came first.

  Runs `32440922073` and `32437410282` (self-hosted, both green), whole run
  **813 s**, jobs strictly sequential — the box has one job slot, so a job's
  seconds are real wall clock, not hidden behind a peer:

  | job | duration | share |
  |---|---|---|
  | `gcc x64` | 60 s | 7% |
  | `gcc aa64` | 53 s | 7% |
  | `clang-tidy` | **158 s** | 19% |
  | QEMU integration (full suite) | **536 s** | **66%** |

  It is **2 m 38 s, not 7 min**, and it is **not the largest job** — QEMU
  integration is **3.4x** larger. The doc was written when CI ran on
  GitHub-hosted runners; moving to our own hardware and caching the toolchain
  shrank it, and the question outlived its own numbers.

  Inside the job: `apt` **25 s / 28 s** across the two runs, the twelve gates
  ~48 s, `scripts/lint.sh` **77 s** (identical both runs). So caching the image
  attacks **25 s = 3.1% of the run**, and *at best* — an image still has to be
  pulled.

  **What it would cost is the reason to decline.** A prebuilt image puts the
  apt package list in a SECOND place. That is exactly the class that failed the
  v4.3.0 tag run: `check-cxx-entry` moved to the cross `objdump` and `ci.yml`'s
  apt list did not move with it, so the cross tool could not load and the gate
  blamed codegen. Institutionalising a second copy of that list to save 25 s is
  the wrong trade — and the runner is ours, so there are no billable minutes
  being saved either, only wall clock that §12 has already established is not
  the long pole.

  The scoping half (§12.12 step 5) is unaffected by this answer: it attacks the
  77 s, not the 25 s, and remains open on its own terms.
- ~~**OPEN, low priority — does the runner want a second registration** so the
  hosted-style 4-way shard can also run on the box?~~ **ANSWERED 2026-08-20 by
  the same measurement: not yet, and the ceiling is now known.** With one job
  slot everything is serial, so a second slot could overlap `clang-tidy` (158 s)
  with QEMU integration (536 s) — worth **at most 19%** of the run, which is
  more than image caching buys and still leaves QEMU integration as the floor.
  The condition stated here has not changed: §12 established the LOCAL gate as
  the long pole, and a 13-minute CI run on free hardware is not what anyone is
  waiting for.

---

## 13. REVISION 2026-09-05 — §4.4 shipped, and it was pointed at the wrong source

The backlog asked to **stop running CI on every push to `main`** and go
nightly, to "cut a release without spending 15 minutes of a human's attention."
Two things were wrong with that, and both are visible in the entry's own table.

**It removes none of the 15 minutes.** The measured stages were: the local
uncached suite (477 s / 509 s, blocking), watching `Release` to completion
(7m05s / 4m11s), and CI on the release push — which is already **28 s
(skipped)**, fixed by the `[release-cut]` marker. Push-CI costs machine time on
a dev box, not attention, and the standing advice after a push is not to wait
for it.

**And it re-opens what §11.2 measured.** Dispatch-only CI cost five days and
~40 commits of undetected red, in three places, two of which a dev box
structurally cannot catch. Nightly bounds that at a day instead of five; it
does not change the class. §12.9 is titled "CI stays unrestricted on push" for
this reason.

### 13.1 The duplication was one level over

§4.4 said it in 2026-08: *"If a successful CI run already exists for its
PARENT, and `git diff --name-only parent..release` is a subset of those three
[files], the gate is satisfied without dispatching anything."* That shipped —
but only for a **local stamp**. `release_gate_covers` read
`test/integration/.last-run-stamp`, which only an uncached LOCAL run writes.

So the actual shape was: CI runs the uncached suite on push, on this box, and
goes green on the release commit's parent — and the gate could not see it.
A human then ran the same suite on the same hardware for the same tree
(~8.5 min, blocking), and `--ci-gate` would dispatch a **third** run.

`release_gate_ci_covers` closes it. Same two-step, same rule: the commit
itself, then its parent through the now-shared `release_gate_release_only`.

### 13.2 What counts as a green CI run, and why the run conclusion is not it

**Every job in the run must have succeeded.** A run-level `conclusion` is not
enough, and the counterexample is not hypothetical — it is the most ordinary
path there is. The `release: vX.Y.Z` commit carries `[release-cut]`, every
`ci.yml` job is conditioned on not seeing it, so a CI run *does* exist at that
commit and it tested nothing. Verified against the live v4.7.0 cut: the run at
`492dd3b2` reports `conclusion: skipped`, the gate refuses it, and then accepts
the parent `636f596a` where the suite genuinely ran. A gate that read the run
conclusion alone would have tagged on the strength of a run that skipped
everything.

A run reporting **no jobs** is refused for the same reason a missing `gh` is:
"the query could not run" and "the run had none" are the same empty answer and
opposite facts.

### 13.3 Watching `Release` became opt-in

`cut-release.sh` pushes the tag, prints the Actions URL and
`check-published-release.sh`, and returns. The watch is `--watch`.

Nothing about this weakens verification — it strengthens it. Watching returned
a *workflow exit status*; `check-published-release.sh` (§16.2 job 2) verifies
the published **bytes** against `SHA256SUMS` and the release's own asset list,
which is what catches a truncated upload or a missed rename. A tag cannot be
un-published either way, so the only thing the wait bought was latency between
a failed publish and someone noticing, and that is exactly what the handed-back
command closes.

### 13.4 Still open: what runs on a push

Measured on `b1c6fde6`: `gcc x64` 74 s, `gcc aa64` 67 s, `clang-tidy` 174 s,
**`QEMU integration` 481 s**. Splitting by cost — cheap jobs on every push, the
integration job nightly — would return ~10 minutes of a dev box per push while
keeping the clean-checkout link failure §11.2 named, which the `build` job is
what catches.

It is not free, and the entry above is why it is separate: a push would stop
running the 1450+ unit assertions, `install.sh --arch all --cpp`, and ~180
integration tests, up to a day late. Decided against the numbers from a release
cut through the new gate, not against the v4.5.0/v4.6.0 table.

### 13.5 The marker guard could not survive being documented

Found the same day, by the commit that shipped §13.1-§13.4: **CI skipped it.**
Its message explained the `[release-cut]` mechanism, the guard was
`!contains(github.event.head_commit.message, '[release-cut]')` — a substring
test over the WHOLE message, body included — so any commit that *mentions* the
marker disables the thing it is describing. Silently: the run exists and
reports `skipped`, which reads like a deliberate skip rather than a lost gate.

`test-release-gate.sh` already carried this exact shape one layer over, and
said so: its `[skip ci]` check strips comments first, because "the comment in
cut-release.sh explaining why we do NOT use `[skip ci]` contains the string —
a check that cannot survive its own subject being discussed." The guard had
the same defect and nothing was looking.

The guard now requires **both** halves:

```yaml
if: "${{ !(startsWith(github.event.head_commit.message, 'release: v')
       && contains(github.event.head_commit.message, '[release-cut]')) }}"
```

A body is never the start of a message, so prose cannot trigger it; the marker
stays so a commit hand-titled `release: v...` does not skip on its title
alone. It must be **double-quoted at the YAML level** — the expression holds
`release: v`, and a colon-space cannot appear in a plain scalar; unquoted, the
workflow stops parsing.

Held together by `test-release-gate.sh`: every job carries both halves, no job
carries the bare form, and `cut-release.sh` writes a subject the guard
matches — the two live in files that cannot import each other, which is the
drift that produced this.

**One consequence worth stating.** `d9dc7117` has no CI coverage of its own.
It is covered by the next push's run over the same tree, which is the ordinary
recovery and needs no re-tag, because nothing was published.

### 13.6 SHIPPED: a push is classified before it is run

`paths-ignore: ['docs/**.md']` is gone, replaced by a `plan` job that calls
**`scripts/ci-plan.sh`**. Two reasons it is a script and not a glob:

- **A glob is logic no gate can read.** `which-gates.sh` says it for every
  workflow edit — a change here runs only on push or tag. This particular
  decision is *whether the suite runs at all*, and being wrong is silent in the
  direction that matters. `test-ci-plan.sh` drives every class through a
  fixture repo, and the allowlist is sabotage-verified: putting the root
  `README.md` in the safe set fails the run.
- **The old filter was wrong in both directions.** `docs/**.md` skipped design
  docs entirely — no gate at all — while a `src/*/README.md` change paid the
  full ~13 minutes for build, QEMU and lint, *none of which builds the docs*.
  There is no Sphinx anywhere in `ci.yml`. A module README ran everything
  except the one thing that could see it.

The allowlist is §11.6's trap list inverted, and it was re-grepped rather than
inherited. `src/*/README.md` is safe — its only consumer is Sphinx's
`.. include::`. The root `README.md` is **not**: three things read it, and two
are tests (`check-tool-docs.py` asserts every shipped tool has a row in its
table; `test-toolchain-variant.sh` asserts it documents no `make CROSS=`
build). `docs/sphinx/**` is not, because `check-doc-coverage.py` reads all 104
`.rst` files in the lint job. And **a deletion is never safe**:
`test-source-snapshot.sh` asserts the snapshot *contains* `docs/AXL-Design.md`
— editing cannot break that, removing can, and `--name-only` shows both the
same way.

### 13.7 The gate had to learn job NAMES, or the split would have broken it

This is the interaction, and it was not visible from either change alone.
`release_gate_ci_covers` (§13.2) accepted a run in which **every job
succeeded**. Once a push can produce a *partial* run — a docs-only push runs
the `plan` job and nothing else — every job in it succeeds, and the gate would
have read that as "CI is green" and tagged code the suite never touched.
Reachable by cutting a release straight after a README fix.

So the gate now also requires a job named `AXL_CI_SUITE_JOB` (`QEMU
integration (full suite)`) to be present and green. That is a second spelling
of `ci.yml`'s job name, so `test-release-gate.sh` holds the two equal — the
same treatment the `[release-cut]` marker gets, for the same reason: they live
in files that cannot import each other.

### 13.8 NOT done: moving the integration job to nightly

§13.4 proposed it. Building §13.6 showed why it does not compose with §13.1:
**the release gate depends on the integration job having run on the commit
being released.** Move it to a schedule and a push run can never satisfy the
gate, so every cut falls back to either a local uncached suite or a dispatch —
which is the ~8.5 minutes §13.1 just removed, reintroduced.

The measurement stands (`gcc x64` 74 s, `gcc aa64` 67 s, `clang-tidy` 174 s,
`QEMU integration` 481 s) and so does the appeal of ~10 minutes of a dev box
per push. What is missing is a way for a release to know the suite covered its
tree without the suite having run on its push. That is a real design question,
not a flag flip, and it is left open rather than half-answered.

### 13.9 NOT done: a docs job on push

The plan job classifies a docs-only push, and today nothing then runs — the
same net effect the old `paths-ignore` had, now auditable and one line from
being routed somewhere. Routing it at a real Sphinx build was deliberately not
bundled in: `docs.yml`'s dependency step pins four pip packages and a
`doxygen`/`nodejs` apt set, and copying that into `ci.yml` is a second copy of
a pinned toolchain — the drift shape this file keeps paying for. It wants a
shared step, which is its own change.

So a module README still has no push-time doc gate. It has no *worse* one than
before, and it no longer costs 13 minutes to get nothing.

### 13.10 OPEN: a class below "docs-only", and what the log actually says

`ci-plan.sh` has two classes: docs-only, or everything. The obvious next one is
"no C/C++ and no build driver changed" — but it was worth reading a real run
before designing it, because the guess and the measurement disagree.

Run `33985299331`, a push of shell + tests + workflow YAML and one design doc,
every step accounted for:

| job | wall | what this push could actually affect |
|---|---:|---|
| `plan` | 8 s | the classifier itself |
| `gcc x64` / `gcc aa64` | 79 s / 77 s | **nothing** — no C changed |
| `clang-tidy` (20 gates) | 178 s | **2 steps**: the `test-meta` header on the added test, and `check-nul` |
| `QEMU integration` | 483 s | **the point of the push** — the suite runs the two tests it changed |

**The intuition that the suite is the waste is exactly backwards here.** The
483-second job is the one with real work; the ~260 s of container build and
static analysis is what ran for nothing. And because `integration` declares
`needs: build`, removing the build from the critical path saves roughly **80 s
of wall clock**, not 260 — the rest is machine load, which matters on a dev box
but is not a human waiting.

So the class is worth having and is smaller than it looks. Three things it must
get right, and the third is why this is not written yet:

1. **Shell is the WORST case for relevance-guessing, not the best.**
   `scripts/axl-cc` is the compiler driver, the `Makefile` drives every gate,
   `scripts/lint.sh` defines the clang-tidy invocation, and `install.sh` stages
   the SDK the suite tests. A change to any of those can alter every produced
   image. §12.5's worked example is precisely this shape: `src/log/` plus the
   `Makefile` looked like "the logging tests" and the blast radius was every
   image in the tree, failing at link.
2. **The unit of skipping is a STEP, not a job.** The lint job earned two of
   its twenty steps on this push. Skipping the job loses those two; keeping it
   pays for eighteen. That argues for splitting the image-producing gates away
   from the file-reading ones, which is a bigger change than a path filter.
3. **The saving is ~80 s of wall clock.** Against §12's finding that the local
   loop is the long pole, that is small, and the cost of being wrong is silent.
   It should be built when someone is feeling the 80 s, not on principle.

### 13.11 The two halves fought, and the newer one had shipped first

§13.6's plan job and §13.1's gate reuse landed an hour apart, and the
interaction was only visible once both existed: a docs-only push skips every
job, so **there is no green run at the tip**, and the gate refused the release
— sending the cut back to the ~8.5-minute local suite §13.1 had just removed.
Reproduced on the real tree, not reasoned about: after `cd57acf1` the gate
answered *"tree changed since 3b920e9c"* and listed eight `.md` files.

`release_gate_ci_covers` now walks back through ancestors, bounded
(`AXL_CI_WALK_MAX=25`), and accepts the first green one whose entire diff to
the release commit is prose, the version bump, or both. Accepting that range is
consistent rather than lenient: *"this diff cannot affect the suite"* is
precisely what `ci-plan.sh` asserts, and `test-ci-plan.sh` sabotage-verifies
the claim.

**Two lists, composed, neither copied.** `ci-plan.sh --list-unsafe` prints the
changed paths that are *not* prose (and every removed path, safe or not);
`release-gate.sh` checks the remainder is a subset of
`AXL_RELEASE_ONLY_FILES`. Re-stating either list in the other file would put a
second copy of it in the code that decides whether a tag is verified.

**Three bugs while building it, every one caught by a test rather than by
review**, and all three are the same shape — a guard that failed for a reason
unrelated to the question:

- the helper located `ci-plan.sh` with `git rev-parse --show-toplevel`, which
  during a test is a fixture repo under `/tmp` with no `scripts/` in it. It
  returned "cannot answer" and the walk silently accepted nothing;
- the parent step **returned** when the parent's diff was not release-only —
  so a docs-only push, the entire case the walk exists for, never reached it.
  The walk starts at the commit itself, so the parent is just its second
  iteration and the special case is gone rather than patched;
- unbounded, "walk back until something is green" is a way to tag anything.
  The bound is a backstop, not the guard: every step still has to pass the
  inertness check, and sabotaging *that* makes a C file anywhere in the range
  accepted.

### 13.12 SHIPPED: the ci-only class, and why it is allowed where "shell" is not

Mike said it twice, which is the signal to stop explaining and count. Nine
pushes on 2026-09-05: **none touched a C file, and eight ran the full QEMU
suite.** Only one — pure docs, no deletions — took the 4-second path. The
two-gear classifier almost never engaged.

**`.github/`-only is a different kind of claim from "shell changed", not a
weaker version of the same one.** §12.5's warning is about *guessing* which
tests a change can reach; this is not a guess. **Nothing builds from
`.github`** — grepped, not assumed: no `Makefile`, `build.sh` or `install.sh`
rule reads it. There is no path from a `.yml` file to a produced artifact, so
the only things such a change can reach are CI itself and the tests that READ
those files. Contrast the shell case in §13.10, where `axl-cc` is the compiler
driver and the `Makefile` drives every gate — there the blast radius genuinely
is every image.

That reader set is **derived by grep, never written down**: a hand-kept list
would go stale the first time a test starts reading a workflow, which is the
defect this script exists to prevent one level up. Over-inclusion is the safe
direction. Today it derives three, all host-only:

| | |
|---|---|
| full run | ~480 s, hundreds of QEMU boots |
| `--only=test-ci-plan.sh,test-release-gate.sh,test-tools-sidecars-gate.sh` | **11 s, zero QEMU processes** |

An empty derivation falls back to the full run, because "the grep found
nothing" and "no test reads a workflow" are the same blank line.

**What is NOT skipped, and why.** `lint` still runs: five of its gates read
workflow files (`check-tool-version`, `check-awk-portability`,
`check-tool-docs`, `check-tools-sidecars`, `check-toolchain-conf`). `build`
still runs too, and that one is a DAG constraint rather than a judgement — a
skipped `needs:` job skips its dependents in GitHub Actions, so skipping
`build` would take `integration` with it. 74 seconds is not worth the
`if: always()` contortion that would avoid it.

### 13.13 The self-test wrapper that made the suite go 121/59

Recorded because the failure is more interesting than the feature. Four
`*-selftest.sh` files check the harness itself and **run nowhere** —
`lib/discover.sh` excludes them by name, twice, and nothing else invokes them.
~100 s of correct, dead assertions.

The obvious fix — a discovered wrapper that execs them — took CI to **121
passed / 59 failed**. Not contention: `test-runner-selftest.sh`'s SIGTERM case
deliberately drives a hang and reaps it, and inside the suite's own six workers
the reap did not complete. It left **two `qemu-system` processes running** and
leaked 108 MB of `/dev/shm`, and those orphans then competed with every
remaining test.

`lib/discover.sh`'s exclusion comment already said a self-test of the runner
must not be scheduled BY the runner as a peer of the tests it checks. A wrapper
is that, with an extra file. **Reverted.**

The finding stands and is now better understood: those four need a **serial**
home — a dedicated CI step or a Makefile target — and whoever builds it has to
reckon with a SIGTERM case that has just demonstrated it can fail under load.
Until then `run-integration.sh --only` is exercised by a self-test that nothing
runs, which is the honest state rather than a fixed one.

