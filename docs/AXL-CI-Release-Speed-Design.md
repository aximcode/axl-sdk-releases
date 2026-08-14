# AXL-CI-Release-Speed-Design — cutting release wall time and Actions spend

Status: ACCEPTED 2026-08-13, not yet implemented.
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

## 9. Open questions

- **Does the hosted shard count want to be 4 or 6?** 4 gives ~14 min at +12
  billable; 6 gives ~10 min at +15. Pick after the first real hosted run, since
  the per-shard fixed cost is the estimate with the least evidence behind it.
- **Should `clang-tidy` (7 min, container `apt` every run) cache its image?**
  Out of scope here; it is the second-largest CI job now that the first is
  fixed.
- **Does the runner want a second registration** so the hosted-style 4-way
  shard can also run on the box? Only worth it if the 9-minute self-hosted run
  becomes the critical path, which it is not today.
