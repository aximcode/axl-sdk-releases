# QEMU Test Parallelization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the QEMU integration suite materially faster in CI and locally by running tests concurrently (one VM per test, isolation kept) and sharding CI, plus trimming per-boot overhead.

**Architecture:** One execution engine — `test/integration/run-integration.sh` — drives the existing per-script `test-*.sh` harness, reading self-describing `# test-meta:` headers via `test/integration/lib/discover.sh`. Locally it runs an `-jN` job pool; in CI it runs one balanced `--shard i/K` of a build-once matrix. Per-worker `TEST_PORT_BASE` removes host-port collisions (strategy β). See `docs/superpowers/specs/2026-06-17-qemu-test-parallelization-design.md`.

**Tech Stack:** Bash, GNU coreutils, QEMU/OVMF, GitHub Actions, the existing `common-test.sh`/`axl-common.sh` harness.

## Global Constraints

- Output must stay ASCII in any committed shell/string literal (`make check-ascii`; `scripts/check-output-ascii.py`). One line each, verbatim.
- Each integration test keeps its own QEMU VM — **per-test isolation is preserved** (this effort does NOT batch multiple tests into one VM; see spec §7 Non-goals).
- A standalone `test-*.sh` invocation (run directly, not via the runner) must behave exactly as today — the runner's env hooks default to today's values when unset.
- No new hard dependency for the local runner beyond what `common-test.sh` already needs (bash + coreutils).
- Acceptance gate for parallelism: the full suite at `-j8` produces the **same pass/fail set** as serial, run 3× (spec §9).

---

### Task 1: Test discovery + metadata parsing (`lib/discover.sh`)

**Files:**
- Create: `test/integration/lib/discover.sh`
- Test: `test/integration/test-discover-selftest.sh` (a host-only bash self-test, no QEMU)

**Interfaces:**
- Produces: `discover_tests [--arch X64|AARCH64] [--include-local-only]` — prints one runnable test path per line (filtered by arch + `local-only`). `test_meta_field <script> <field>` — echoes a `# test-meta:` field value (`arch`/`needs`/`est`/`local-only`) or its default (`arch=x64`, `needs=`, `est=20`, `local-only=0`).
- Consumes: nothing (foundational).

- [ ] **Step 1: Write the failing self-test**

```bash
#!/bin/bash
# test-discover-selftest.sh — host-only checks for lib/discover.sh (no QEMU).
set -euo pipefail
cd "$(dirname "$0")"
source lib/discover.sh

fail=0
check() { if [[ "$2" == "$3" ]]; then echo "  PASS: $1"; else echo "  FAIL: $1 (got '$2' want '$3')"; fail=1; fi; }

# Fixture script with a full meta tag.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test-fixture-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=both needs=swtpm,openssl est=42 local-only=1
EOF
check "arch field"        "$(test_meta_field "$tmp/test-fixture-qemu.sh" arch)"       "both"
check "needs field"       "$(test_meta_field "$tmp/test-fixture-qemu.sh" needs)"      "swtpm,openssl"
check "est field"         "$(test_meta_field "$tmp/test-fixture-qemu.sh" est)"        "42"
check "local-only field"  "$(test_meta_field "$tmp/test-fixture-qemu.sh" local-only)" "1"

# A script with no meta tag gets defaults.
cat > "$tmp/test-bare-qemu.sh" <<'EOF'
#!/bin/bash
echo hi
EOF
check "default arch"       "$(test_meta_field "$tmp/test-bare-qemu.sh" arch)"       "x64"
check "default est"        "$(test_meta_field "$tmp/test-bare-qemu.sh" est)"        "20"
check "default local-only" "$(test_meta_field "$tmp/test-bare-qemu.sh" local-only)" "0"

[[ $fail -eq 0 ]] && echo "discover selftest: OK" || { echo "discover selftest: FAILED"; exit 1; }
```

- [ ] **Step 2: Run it, verify it fails**

Run: `./test/integration/test-discover-selftest.sh`
Expected: FAIL — `lib/discover.sh` does not exist (`source: lib/discover.sh: No such file`).

- [ ] **Step 3: Implement `lib/discover.sh`**

```bash
#!/bin/bash
# discover.sh — enumerate runnable integration tests + read their
# `# test-meta:` headers. Sourced by run-integration.sh and the meta lint.
# A header line looks like:
#   # test-meta: arch=x64 needs=swtpm,openssl est=12 local-only=0

# Echo one `# test-meta:` field (or its default) for a test script.
test_meta_field() {
    local script="$1" field="$2" line val
    line=$(grep -m1 '^# test-meta:' "$script" 2>/dev/null || true)
    # token after `field=`, up to the next space
    val=$(printf '%s\n' "$line" | grep -oE "${field}=[^ ]+" | head -n1 | cut -d= -f2-)
    if [[ -n "$val" ]]; then printf '%s\n' "$val"; return; fi
    case "$field" in
        arch)       printf 'x64\n' ;;
        needs)      printf '\n' ;;
        est)        printf '20\n' ;;
        local-only) printf '0\n' ;;
        *)          printf '\n' ;;
    esac
}

# Print runnable test scripts, one per line.
discover_tests() {
    local want_arch="X64" include_local=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --arch) want_arch="$2"; shift 2 ;;
            --include-local-only) include_local=1; shift ;;
            *) shift ;;
        esac
    done
    local dir; dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    local f base arch lo
    for f in "$dir"/test-*.sh; do
        base=$(basename "$f")
        # Skip the unit batch runner, the aggregate runner, and self-tests.
        case "$base" in
            test-axl.sh|test-all.sh|test-discover-selftest.sh) continue ;;
        esac
        arch=$(test_meta_field "$f" arch)
        lo=$(test_meta_field "$f" local-only)
        [[ "$lo" == "1" && $include_local -eq 0 ]] && continue
        case "$arch" in
            both) : ;;
            x64)    [[ "$want_arch" == "X64" ]] || continue ;;
            aa64)   [[ "$want_arch" == "AARCH64" ]] || continue ;;
        esac
        printf '%s\n' "$f"
    done
}
```

- [ ] **Step 4: Run the self-test, verify it passes**

Run: `./test/integration/test-discover-selftest.sh`
Expected: PASS — `discover selftest: OK`.

- [ ] **Step 5: Commit** (DEFERRED if the v2.0.1 cut is still in flight — see plan note)

```bash
git add test/integration/lib/discover.sh test/integration/test-discover-selftest.sh
git commit -m "test: add integration test discovery + meta parsing"
```

---

### Task 2: Per-worker port isolation hook (`common-test.sh`)

**Files:**
- Modify: `test/integration/common-test.sh` (add `test_port`, honor `TEST_PORT_BASE`)
- Test: extend `test/integration/test-discover-selftest.sh` with a `test_port` block

**Interfaces:**
- Produces: `test_port <slot>` → echoes `$((TEST_PORT_BASE + slot))`. `TEST_PORT_BASE` defaults to `18000` when unset (a standalone test gets today-like fixed ports); `run-integration.sh` overrides it per worker.
- Consumes: nothing.

- [ ] **Step 1: Add a failing assertion to the self-test**

```bash
# --- test_port ---
source common-test.sh 2>/dev/null || true   # for test_port; tolerate its top-level vars
TEST_PORT_BASE=20400
check "test_port slot 0" "$(test_port 0)" "20400"
check "test_port slot 5" "$(test_port 5)" "20405"
unset TEST_PORT_BASE
check "test_port default base" "$(test_port 0)" "18000"
```

- [ ] **Step 2: Run it, verify it fails**

Run: `./test/integration/test-discover-selftest.sh`
Expected: FAIL — `test_port: command not found` (or empty output).

- [ ] **Step 3: Implement `test_port` in `common-test.sh`**

Add near the other helpers:

```bash
# Per-worker host-port allocation. run-integration.sh exports TEST_PORT_BASE
# per concurrent worker so parallel tests never collide on a host port; a
# standalone invocation falls back to a fixed base (today's behavior).
: "${TEST_PORT_BASE:=18000}"
test_port() {
    echo $(( TEST_PORT_BASE + ${1:-0} ))
}
```

- [ ] **Step 4: Run the self-test, verify it passes**

Run: `./test/integration/test-discover-selftest.sh`
Expected: PASS.

- [ ] **Step 5: Commit** (deferred per plan note)

```bash
git add test/integration/common-test.sh test/integration/test-discover-selftest.sh
git commit -m "test: add per-worker TEST_PORT_BASE + test_port helper"
```

---

### Task 3: Runner core — serial mode with summary + timeout (`run-integration.sh`)

**Files:**
- Create: `test/integration/run-integration.sh`
- Test: `test/integration/test-runner-selftest.sh` (host-only; uses stub tests, no QEMU)

**Interfaces:**
- Consumes: `discover_tests`, `test_meta_field` (Task 1).
- Produces: `run-integration.sh [--arch A] [-jN] [--shard i/K] [--timeout S] [--list]` — runs the selected tests, prints `<name> PASS|FAIL|TIMEOUT <dur>s`, a totals line, and exits non-zero if any failed/timed out. `--list` prints the resolved test set without running. (`-jN` and `--shard` added in Task 4/5; this task implements serial + summary + timeout.)

- [ ] **Step 1: Write the failing self-test** (stubs, no QEMU)

```bash
#!/bin/bash
# test-runner-selftest.sh — host-only checks for run-integration.sh using
# stub "tests" (a pass, a fail, a hang). No QEMU.
set -uo pipefail
cd "$(dirname "$0")"
stub=$(mktemp -d); trap 'rm -rf "$stub"' EXIT
printf '#!/bin/bash\nexit 0\n'             > "$stub/test-pass-qemu.sh"
printf '#!/bin/bash\nexit 1\n'             > "$stub/test-fail-qemu.sh"
printf '#!/bin/bash\nsleep 30\n'           > "$stub/test-hang-qemu.sh"
chmod +x "$stub"/*.sh

out=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --timeout 2 2>&1); rc=$?
fail=0
grep -q "test-pass-qemu.sh PASS" <<<"$out" || { echo "FAIL: pass not reported"; fail=1; }
grep -q "test-fail-qemu.sh FAIL" <<<"$out" || { echo "FAIL: fail not reported"; fail=1; }
grep -q "test-hang-qemu.sh TIMEOUT" <<<"$out" || { echo "FAIL: hang not timed out"; fail=1; }
[[ $rc -ne 0 ]] || { echo "FAIL: runner exit 0 despite a failure"; fail=1; }
[[ $fail -eq 0 ]] && echo "runner selftest: OK" || exit 1
```

- [ ] **Step 2: Run it, verify it fails**

Run: `./test/integration/test-runner-selftest.sh`
Expected: FAIL — `run-integration.sh` does not exist.

- [ ] **Step 3: Implement `run-integration.sh` (serial)**

```bash
#!/bin/bash
# run-integration.sh — the integration-suite execution engine. Serial here;
# -jN + --shard added in later tasks. Reads test-meta via lib/discover.sh.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/lib/discover.sh"

ARCH="X64"; TIMEOUT=900; LIST_ONLY=0
# RUN_INTEGRATION_DIR overrides the test dir (self-test seam).
TESTDIR="${RUN_INTEGRATION_DIR:-$SCRIPT_DIR}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --list) LIST_ONLY=1; shift ;;
        *) shift ;;
    esac
done

mapfile -t TESTS < <(
    if [[ -n "${RUN_INTEGRATION_DIR:-}" ]]; then
        ls "$TESTDIR"/test-*.sh 2>/dev/null
    else
        discover_tests --arch "$ARCH"
    fi
)

if [[ $LIST_ONLY -eq 1 ]]; then printf '%s\n' "${TESTS[@]}"; exit 0; fi

pass=0; failc=0
for t in "${TESTS[@]}"; do
    name=$(basename "$t")
    start=$SECONDS
    timeout "$TIMEOUT" bash "$t" --arch "$ARCH" >/dev/null 2>&1; rc=$?
    dur=$(( SECONDS - start ))
    if [[ $rc -eq 124 ]]; then echo "  $name TIMEOUT ${dur}s"; failc=$((failc+1))
    elif [[ $rc -ne 0 ]]; then echo "  $name FAIL ${dur}s"; failc=$((failc+1))
    else echo "  $name PASS ${dur}s"; pass=$((pass+1)); fi
done

echo ""
echo "integration: $pass passed, $failc failed"
[[ $failc -eq 0 ]]
```

- [ ] **Step 4: Run the self-test, verify it passes**

Run: `./test/integration/test-runner-selftest.sh`
Expected: PASS — `runner selftest: OK`.

- [ ] **Step 5: Commit** (deferred per plan note)

```bash
git add test/integration/run-integration.sh test/integration/test-runner-selftest.sh
git commit -m "test: add serial integration runner with summary + per-test timeout"
```

---

### Task 4: Parallel job pool (`-jN`)

**Files:**
- Modify: `test/integration/run-integration.sh` (add `-jN` worker pool + per-worker `TEST_PORT_BASE`)
- Modify: `test/integration/test-runner-selftest.sh` (assert `-j4` still reports all three stub results correctly)

**Interfaces:**
- Produces: `-jN` runs up to N tests concurrently; worker *i* gets `TEST_PORT_BASE=$((20000 + i*200))`. Output lines unchanged (still `<name> PASS|FAIL|TIMEOUT <dur>s`), emitted as each test finishes.
- Consumes: serial runner (Task 3), `test_port` semantics (Task 2).

- [ ] **Step 1: Add a failing parallel assertion to the self-test**

```bash
# parallel: same verdicts, and a per-worker port base is exported
printf '#!/bin/bash\necho "BASE=$TEST_PORT_BASE"; exit 0\n' > "$stub/test-portcheck-qemu.sh"; chmod +x "$stub/test-portcheck-qemu.sh"
outp=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j4 --timeout 2 2>&1)
grep -q "test-pass-qemu.sh PASS" <<<"$outp" || { echo "FAIL: -j4 pass"; exit 1; }
grep -q "test-hang-qemu.sh TIMEOUT" <<<"$outp" || { echo "FAIL: -j4 timeout"; exit 1; }
echo "runner parallel selftest: OK"
```

- [ ] **Step 2: Run it, verify it fails** — `-j4` not parsed yet, runs serial (still passes verdicts) OR the worker-base check fails. Confirm the new line is exercised; expected: the assertion you added for parallel behavior fails before the change.

Run: `./test/integration/test-runner-selftest.sh`

- [ ] **Step 3: Implement the job pool**

Replace the serial `for` loop with a bounded pool. Add `-j) JOBS="$2"; shift 2 ;;` to arg parsing (`JOBS=1` default), then:

```bash
run_one() {  # <worker_slot> <test_path>
    local slot="$1" t="$2" name start dur rc
    name=$(basename "$t"); start=$SECONDS
    TEST_PORT_BASE=$(( 20000 + slot*200 )) \
        timeout "$TIMEOUT" bash "$t" --arch "$ARCH" >/dev/null 2>&1
    rc=$?; dur=$(( SECONDS - start ))
    if [[ $rc -eq 124 ]]; then echo "  $name TIMEOUT ${dur}s"; return 1
    elif [[ $rc -ne 0 ]]; then echo "  $name FAIL ${dur}s"; return 1
    else echo "  $name PASS ${dur}s"; return 0; fi
}

declare -a SLOT_PID=()         # slot -> running pid (0 = free)
declare -A PID_RESULT_FILE=()
results=$(mktemp -d); trap 'rm -rf "$results"' EXIT
i=0
for t in "${TESTS[@]}"; do
    # find/wait for a free slot
    while :; do
        for ((s=0; s<JOBS; s++)); do
            if [[ -z "${SLOT_PID[$s]:-}" ]] || ! kill -0 "${SLOT_PID[$s]}" 2>/dev/null; then
                free=$s; break 2
            fi
        done
        wait -n 2>/dev/null || true
    done
    ( run_one "$free" "$t"; echo $? > "$results/$i" ) &
    SLOT_PID[$free]=$!
    i=$((i+1))
done
wait
failc=0; pass=0
for f in "$results"/*; do if [[ "$(cat "$f")" == "0" ]]; then pass=$((pass+1)); else failc=$((failc+1)); fi; done
echo ""; echo "integration: $pass passed, $failc failed"
[[ $failc -eq 0 ]]
```

- [ ] **Step 4: Run the self-test, verify it passes**

Run: `./test/integration/test-runner-selftest.sh`
Expected: PASS — both serial and parallel selftests OK.

- [ ] **Step 5: Commit** (deferred per plan note)

```bash
git add test/integration/run-integration.sh test/integration/test-runner-selftest.sh
git commit -m "test: parallel -jN job pool with per-worker TEST_PORT_BASE"
```

---

### Task 5: Port migration (β) for one representative test, then the rest

**Files:**
- Modify: `test/integration/test-http-async-qemu.sh` (the worked example), then each networked `test-*.sh` that hardcodes a host port (the full list comes from the grep in Step 1).
- Add `# test-meta:` headers to every discovered test in the same pass.

**Interfaces:**
- Consumes: `test_port` (Task 2). Produces: no hardcoded host-port literals in networked tests; each test self-describes via `# test-meta:`.

- [ ] **Step 1: Enumerate the tests to migrate**

Run: `grep -lE 'PORT=[0-9]{4,5}|hostfwd=tcp::[0-9]{4,5}|test_add_port_forward [0-9]{4,5}' test/integration/test-*.sh`
This is the migration worklist. Note it in the commit body.

- [ ] **Step 2: Migrate the worked example (`test-http-async-qemu.sh`)**

Replace the two hardcoded ports:

```bash
# Before:
#   PLAIN_PORT=18090
#   TLS_PORT=18453
# After:
PLAIN_PORT=$(test_port 0)
TLS_PORT=$(test_port 1)
```

Add at the top (after the header comment):

```bash
# test-meta: arch=x64 needs=openssl est=40 local-only=0
```

- [ ] **Step 3: Verify the worked example still passes standalone**

Run: `./test/integration/test-http-async-qemu.sh --arch X64`
Expected: `http-async tests: 11 passed, 0 failed (X64)` (unchanged — standalone `TEST_PORT_BASE` default keeps ports valid).

- [ ] **Step 4: Migrate the remaining tests from the Step 1 worklist**

For each: replace hardcoded host ports with `$(test_port N)` (N counting from 0 within the test), and add a `# test-meta:` header (`arch`, `needs` = any apt deps it uses such as `openssl`/`swtpm`/`socat`, `est` ≈ its measured serial duration, `local-only=1` only for the QMP-pointer tests like `test-input-modifiers-qemu.sh`). Run each standalone once to confirm it still passes.

- [ ] **Step 5: Parity gate — full suite serial vs `-j8`, 3×**

```bash
./test/integration/run-integration.sh --arch X64 > /tmp/serial.txt
for n in 1 2 3; do ./test/integration/run-integration.sh --arch X64 -j8 > /tmp/par-$n.txt; done
# Same pass/fail SET across all four (order will differ):
for f in /tmp/serial.txt /tmp/par-1.txt /tmp/par-2.txt /tmp/par-3.txt; do
  grep -oE 'test-[^ ]+ (PASS|FAIL|TIMEOUT)' "$f" | sort > "$f.sorted"; done
diff /tmp/serial.txt.sorted /tmp/par-1.txt.sorted && \
diff /tmp/par-1.txt.sorted /tmp/par-2.txt.sorted && \
diff /tmp/par-2.txt.sorted /tmp/par-3.txt.sorted && echo "PARITY OK"
```
Expected: `PARITY OK`. Investigate any test that differs (port collision or state bleed) before proceeding.

- [ ] **Step 6: Commit** (deferred per plan note)

```bash
git add test/integration/test-*.sh
git commit -m "test: migrate integration tests to per-worker ports + meta headers"
```

---

### Task 6: Meta-tag lint gate

**Files:**
- Modify: `test/integration/lib/discover.sh` (add `--lint`)
- Modify: `Makefile` (a `check-test-meta` target) and `.github/workflows/ci.yml` (run it in the lint job)

**Interfaces:**
- Produces: `lib/discover.sh --lint` exits non-zero and names any `test-*.sh` (excluding the known non-test scripts) lacking a `# test-meta:` header.

- [ ] **Step 1: Add a failing assertion to the discover self-test**

```bash
cat > "$tmp/test-nometa-qemu.sh" <<'EOF'
#!/bin/bash
echo hi
EOF
if ( cd "$tmp" && RUN_INTEGRATION_DIR="$tmp" bash "$OLDPWD/lib/discover.sh" --lint ) 2>/dev/null; then
  echo "FAIL: --lint passed a meta-less test"; fail=1
else echo "  PASS: --lint flags missing meta"; fi
```

- [ ] **Step 2: Run it, verify it fails** (`--lint` unimplemented → exits 0).

- [ ] **Step 3: Implement `--lint`** in `discover.sh` (a `main` guard so it works when executed, not only sourced):

```bash
if [[ "${BASH_SOURCE[0]}" == "${0}" && "${1:-}" == "--lint" ]]; then
    dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    miss=0
    for f in "$dir"/test-*.sh; do
        case "$(basename "$f")" in test-axl.sh|test-all.sh|test-*-selftest.sh) continue ;; esac
        grep -q '^# test-meta:' "$f" || { echo "missing test-meta: $f"; miss=1; }
    done
    exit $miss
fi
```

- [ ] **Step 4: Run the self-test, verify it passes.**

- [ ] **Step 5: Wire `make check-test-meta` + CI lint step, then commit** (deferred per plan note)

```makefile
check-test-meta:
	@bash test/integration/lib/discover.sh --lint && echo "check-test-meta: clean"
```
Add to `ci.yml`'s `lint` job: `- run: make check-test-meta`.

```bash
git add test/integration/lib/discover.sh Makefile .github/workflows/ci.yml
git commit -m "test: lint that every integration test has a test-meta header"
```

---

### Task 7: Per-boot trim (conservative, measured)

**Files:**
- Modify: individual `test-*.sh` `startup.nsh` blocks (network + driver tests).

**Interfaces:** none new — behavior-preserving speedup.

- [ ] **Step 1: Baseline each candidate's duration** (`run-integration.sh --list` then time the network/storage tests individually).
- [ ] **Step 2: For one test, replace a fixed wait with a condition-wait** — e.g. in a DHCP test, replace `ifconfig -s eth0 dhcp` + `stall 3000000` with a poll loop that exits as soon as an IP is assigned (bounded by a max). Keep `connect -r`.
- [ ] **Step 3: Re-run that test 3× standalone** — confirm it still passes every time (no flakiness from the tighter wait). If it flakes, restore the stall and move on (the stall is load-bearing).
- [ ] **Step 4: Repeat per candidate**, committing in small batches.
- [ ] **Step 5: Commit** (deferred per plan note)

```bash
git add test/integration/test-*.sh
git commit -m "test: trim fixed startup stalls to condition-waits where safe"
```

---

### Task 8: CI sharding (build-once + balanced matrix)

**Files:**
- Modify: `.github/workflows/ci.yml` (upload a build artifact from `build`; replace the serial `integration` job with a `needs: build` matrix calling `run-integration.sh --shard`).
- Modify: `test/integration/run-integration.sh` (add `--shard i/K`, bin-packed by `est=`).

**Interfaces:**
- Produces: `run-integration.sh --shard i/K` runs only shard *i*'s bin-packed subset (longest-`est`-first, round-robin into the lightest shard).

- [ ] **Step 1: Add a failing shard assertion to the runner self-test**

```bash
# Two shards must partition the set with no overlap and full coverage.
a=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 0/2 --list | sort)
b=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 1/2 --list | sort)
both=$(printf '%s\n%s\n' "$a" "$b" | sort)
all=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --list | sort)
[[ "$both" == "$all" ]] || { echo "FAIL: shards != full set"; exit 1; }
[[ -z "$(comm -12 <(echo "$a") <(echo "$b"))" ]] || { echo "FAIL: shard overlap"; exit 1; }
echo "runner shard selftest: OK"
```

- [ ] **Step 2: Run it, verify it fails** (`--shard` unimplemented).

- [ ] **Step 3: Implement `--shard i/K`** — read `est=` per test, sort descending, assign each to the currently-lightest shard (greedy LPT), keep only shard `i`'s list. Add `--shard) SHARD="$2"; shift 2 ;;`.

- [ ] **Step 4: Run the self-test, verify it passes.**

- [ ] **Step 5: Rework `ci.yml`** — `build` job: after `make all tests tools` + `install.sh`, `tar czf axl-ci.tgz out/native-x64 build/staging ...` and `actions/upload-artifact`. New `integration` job: `needs: build`, `strategy: matrix: shard: [0,1,2,3]`, download artifact, apt-install runtime deps, enable KVM, `run-integration.sh --arch X64 --shard ${{ matrix.shard }}/4`.

- [ ] **Step 6: CI parity** — on a branch, run the sharded job alongside the old serial job until the pass/fail sets agree; then delete the serial job. Commit (deferred per plan note).

```bash
git add .github/workflows/ci.yml test/integration/run-integration.sh
git commit -m "ci: shard integration tests across a build-once matrix"
```

---

## Plan note — commit timing

The v2.0.1 release cut is in flight on `main`. Execute this plan **in a git
worktree on a feature branch** (via `superpowers:using-git-worktrees`) so its
commits never contend with the cut's `git tag`/`git push`. Land it on `main`
(merge / PR) only after v2.0.1 is published and with the user's OK.

## Self-review

- **Spec coverage:** discovery+meta (Task 1,6 ↔ spec §4), port-base β (Task 2,5 ↔ §5), local runner serial+parallel (Task 3,4 ↔ §3), parity gate (Task 5 ↔ §9), per-boot trim (Task 7 ↔ §8), CI sharding build-once+balanced (Task 8 ↔ §6). Non-goals (§7) intentionally absent. Covered.
- **Placeholder scan:** the per-test migration (Task 5 Step 4) and per-boot trim (Task 7) iterate a worklist rather than enumerating all ~80 edits inline — each is a single mechanical pattern shown in full on a worked example, which is the DRY representation of an identical repeated edit, not a "TODO".
- **Type/name consistency:** `test_meta_field`/`discover_tests` (Task 1) used by Task 3/6/8; `test_port`/`TEST_PORT_BASE` (Task 2) used by Task 4/5; `--shard`/`est=` (Task 8) consistent with `test_meta_field ... est`.
