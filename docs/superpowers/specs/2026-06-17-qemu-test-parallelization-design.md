# Faster QEMU integration testing — parallel execution + CI sharding

**Status:** Design (approved sections 1–2; 3–5 pending spec review). **Created:** 2026-06-17.
**Goal:** Make the QEMU integration suite materially faster **both in CI and
locally**, without losing per-test isolation.

## 1. Problem

The ~80 `test/integration/test-*.sh` scripts each boot their **own** QEMU VM and
run **serially**:

- **CI:** one `integration` job (single `ubuntu-latest` runner, x64-only) runs
  every script back-to-back. No sharding.
- **Local:** there is no full integration runner at all — `test-all.sh` only
  runs the already-batched unit suite (`test-axl.sh`); the integration scripts
  are run by hand, one boot each.

Two independent cost multipliers:

1. **No concurrency** — tests run one at a time when the host (or CI matrix)
   could run many at once.
2. **Per-boot overhead** — every test pays a full QEMU boot plus a fixed
   `startup.nsh` preamble: `connect -r`, DHCP bring-up, and multi-second
   `stall` calls (e.g. `stall 3000000` = 3 s).

Unit tests already avoid (1)+(2) by batching all binaries into one boot
(`test-axl.sh`). This effort brings the same kind of win to the integration
suite **without** the batching approach's isolation loss (see §7 Non-goals).

## 2. Approach (two levers, composed)

- **Parallelize** execution: run N tests concurrently, each still its own VM
  (full isolation kept). Local = a `-jN` job pool; CI = a balanced matrix of
  shards. This is the primary wall-clock lever for both targets.
- **Trim per-boot cost**: replace fixed `startup.nsh` sleeps with
  condition-waits and skip unneeded `connect -r`/DHCP, per test, measured.
  Compounds with parallelism.

## 3. Architecture

One execution engine, used by both local and CI:

```
test/integration/
  lib/discover.sh        # enumerate runnable tests + read per-test metadata
  run-integration.sh     # job-pool runner: -jN concurrent workers; --shard i/K
  common-test.sh         # (existing) + per-worker port-base isolation hooks
```

- **`discover.sh`** globs `test-*.sh`, excludes the unit runner and helpers, and
  reads each script's metadata header tag (§4). Emits the runnable list filtered
  by `--arch` and `local-only`.
- **`run-integration.sh`** is the single engine:
  - `-jN` — run N tests concurrently via a bash job pool (default `N=nproc/2`,
    each QEMU uses 1–2 vCPU; capped so N×VM RAM fits).
  - `--shard i/K` — run only the bin-packed subset for shard `i` of `K`
    (§6 balancing). Default (no flag) = all.
  - Per test: assigns a free worker slot → exports `TEST_PORT_BASE` (§5) →
    runs the script with its own `TEST_TMPDIR` (already per-script) → captures
    exit code, duration, and a tail of the log on failure.
  - Aggregates a final summary: `<name> PASS/FAIL <dur>s`, total wall-clock,
    and a non-zero exit if any test failed or timed out.
  - Per-test wall-clock timeout (`--timeout`, default generous) so a hung test
    is reported (not a silent stall that starves the run).

## 4. Test metadata (self-describing, no central manifest)

Each integration script carries a header tag the discovery reads:

```sh
# test-meta: arch=x64 needs=swtpm,openssl est=12 local-only=0
```

- `arch` — `x64` | `aa64` | `both` (default `x64`).
- `needs` — host deps (apt packages / tools) the test requires; the CI shard
  installs the union of its assigned tests' needs.
- `est` — estimated wall-clock seconds, for shard balancing (seeded by a
  one-time measured pass; a missing value defaults to a nominal constant).
- `local-only` — `1` for tests the GitHub runners can't run (e.g.
  `test-input-modifiers-qemu.sh`, which needs QMP pointer injection the runners
  lack). Excluded from CI, run locally.

Metadata lives in the test it describes, so it can't drift from a separate list.
A `lib/discover.sh --lint` check (CI) fails if a `test-*.sh` lacks a
`test-meta:` tag, keeping new tests from silently escaping discovery.

## 5. Concurrency safety — per-worker port base (strategy β)

Per-test temp dirs are **already** isolated (`mktemp -d` per script), so FAT
images and serial logs don't collide. The one hazard is **hardcoded host ports**
(`8443` appears in 9 scripts, `8080` in 6, …): two concurrent tests binding the
same host port collide.

Fix: each worker slot gets a distinct **port base**; tests derive ports from it.

- `run-integration.sh` exports `TEST_PORT_BASE` per worker — worker *i* →
  `20000 + i*200` (200-port stride >> max ports any single test uses).
- `common-test.sh` gains `test_port <slot>` → `echo $((TEST_PORT_BASE + slot))`.
  Outside the runner (a test run directly), `TEST_PORT_BASE` defaults to a
  fixed value so standalone invocation is unchanged.
- **Migration:** the networked tests (~30) move from `PORT=8443` /
  `hostfwd=tcp::8443-...` literals to `PORT=$(test_port 0)` etc. Mechanical
  (most tests use 1–3 ports), and folded into the same pass as the §8 stall
  trim since we're editing those scripts anyway.

This is collision-free by construction, deterministic, and debuggable (a
worker's ports are a known range). Rejected alternatives: dynamic `:0`
allocation (α — more robust but ~30 invasive rewrites and a TOCTOU window);
per-worker loopback/netns bind (γ — no test edits but fiddly SLIRP host-alias
routing to validate).

## 6. CI sharding

Replace the single serial `integration` job with build-once + a balanced matrix:

- **Build once:** the x64 `build` job (or a thin `prep` step) runs
  `make all tests tools` + `install.sh`, tars the outputs (`.efi`s, tools,
  staged drivers, SDK), and uploads one artifact. Shards do **not** rebuild —
  the fixed `make` cost (incl. the mbedtls submodule) is paid once, which is
  what otherwise caps shard speedup.
- **Matrix shards:** `integration` becomes `strategy: matrix: shard: [0,1,2,3]`,
  `needs: build`. Each shard: apt-install the **runtime** deps for its assigned
  tests (or the suite union, simpler), enable KVM (as today), download the
  artifact, then `run-integration.sh --shard ${{ matrix.shard }}/4`.
- **Balancing:** shards are bin-packed by `est=` (longest-processing-time-first)
  so the slowest shard ≈ total/K. `K` starts at **4** (tunable). Wall-clock
  floor = per-shard fixed setup (apt + artifact download) + the single slowest
  test.
- **Aggregation:** the job fails if any shard fails; GH shows per-shard logs. An
  optional `integration-summary` job `needs: [the shards]` gives one rollup
  status for branch protection.

Note: CI today builds **DEBUG only**, so RELEASE-mode optimizer warnings aren't
caught by the CI gate (they surface in the Release workflow). Out of scope here,
but worth a follow-up: add a RELEASE build step to the `build` job.

## 7. Non-goals

- **Grouping multiple tests into one shared VM** (the original idea). For the
  already-homogeneous unit binaries this works (`test-axl.sh`); for integration
  it's a bigger refactor (heterogeneous host-side servers, TLS certs, swtpm, and
  device profiles can't just be concatenated) **and** it trades away per-test
  isolation — one hang starves the whole group. Parallelism gives most of the
  win with none of that risk. Revisit selectively, after §3–§6 land, only for
  clusters proven safe.
- **Network-namespace / loopback-alias isolation** (strategy γ). Deferred unless
  β's port migration proves too costly.
- **Dynamic free-port allocation** (strategy α). Deferred; β is sufficient.

## 8. Per-boot trim (conservative, measured)

Audit `startup.nsh` across the suite for **fixed** waits and replace with
condition-waits where safe:

- `connect -r` + `stall 1000000`: keep `connect -r`; replace the blind 1 s
  settle with a readiness probe where a test has one, else leave it.
- `ifconfig -s eth0 dhcp` + `stall 3000000`: poll until an IP is assigned
  instead of sleeping 3 s flat; skip DHCP entirely in tests that don't use the
  network.

This is the **riskiest** piece — several stalls are load-bearing (driver
settle), so it is done **test-by-test with a re-run to confirm no new
flakiness**, and a stall stays wherever removing it regresses. Also confirm KVM
is active locally (the `axl-common.sh` gate already falls back to TCG).

## 9. Validation & rollout

- **Runner correctness:** a failing test → non-zero exit + named in the summary;
  a hung test → per-test timeout + reported. Verified against a known-pass
  subset plus a deliberately-failing and a deliberately-hanging stub.
- **Parity gate (acceptance):** the full suite at `-j8` locally must produce the
  **same pass/fail set** as a serial run — zero port collisions, zero state
  bleed. Run **3×** to catch concurrency-induced flakiness.
- **CI parity:** run the sharded job alongside the existing serial job on a
  branch until they agree on the pass/fail set, then delete the serial job.
- **Rollout order** (each step lands independently and is reversible):
  1. `discover.sh` + metadata tags + `run-integration.sh` core (serial first).
  2. β port migration + `test_port` helper → validate `-jN` local parity.
  3. Per-boot stall/DHCP trim (measured, per test).
  4. CI sharding (build-once artifact + matrix).

## 10. Open questions

- `K` (shard count) and `-jN` default: tune against measured `est=` once seeded.
- Whether to install per-shard `needs=` deps or the suite union (start with the
  union for simplicity; optimize later if apt time dominates).
- Local RAM ceiling for N concurrent VMs (each boots at 512M) — cap `N`
  accordingly on smaller dev boxes.
