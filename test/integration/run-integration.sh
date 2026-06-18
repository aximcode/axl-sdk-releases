#!/bin/bash
# run-integration.sh — the integration-suite execution engine, used both
# locally and in CI. A -jN job pool runs tests concurrently; --shard i/K splits
# the set across machines. Reads per-test metadata via lib/discover.sh.
#
# Usage:
#   run-integration.sh [--arch X64|AARCH64] [--timeout SECONDS] [-jN] [--list]
#
# Concurrency: -jN sets the worker count. With no -j, the default leaves two
# cores of headroom (nproc-2, min 1; 1 on hosts with <3 cores) — a saturated
# host can starve a QEMU during its boot/setup window, which surfaces as empty
# serial output and a spurious failure. For the same reason, do NOT stack the
# full -jN suite on top of other heavy jobs (a concurrent `lint.sh`, a build):
# the combined overcommit reintroduces those contention flakes. Run them
# sequentially, or lower -j, if you must overlap. A transient failure is
# retried once (see run_one) so a lone contention blip doesn't fail the suite.
#
# Output: one `  <name> PASS|FAIL|TIMEOUT <dur>s` line per test, then a totals
# line. Exit non-zero if any test failed or timed out.
#
# RUN_INTEGRATION_DIR (env) overrides the test directory and runs every
# test-*.sh in it verbatim — the host-only self-test seam (no QEMU).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/lib/discover.sh"

ARCH="X64"; TIMEOUT=900; LIST_ONLY=0; JOBS=0; SHARD=""; LOGDIR=""  # JOBS=0 => auto
# Local-only tests need a capability the GitHub runners lack (a patched QEMU
# for the SMBus/SPD memdev device, usb-mouse pointer delivery, ...). The dev
# box HAS those, so the local run includes them by default; --ci drops them.
INCLUDE_LOCAL="--include-local-only"
TESTDIR="${RUN_INTEGRATION_DIR:-$SCRIPT_DIR}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)    ARCH="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --list)    LIST_ONLY=1; shift ;;
        --shard)   SHARD="$2"; shift 2 ;;   # i/K (0-based)
        --logdir)  LOGDIR="$2"; shift 2 ;;  # keep per-test output here
        --no-build) RUN_NO_BUILD=1; shift ;; # skip the one-time pre-build (CI: built in a prior job)
        --ci)      INCLUDE_LOCAL=""; shift ;; # exclude local-only tests (CI runners lack the capability)
        -j)        JOBS="$2"; shift 2 ;;
        -j*)       JOBS="${1#-j}"; shift ;;
        *) shift ;;
    esac
done
[[ -z "$LOGDIR" ]] && LOGDIR=$(mktemp -d)
mkdir -p "$LOGDIR"

# Auto job count when -j was not given: leave two cores of headroom so a guest
# isn't starved during boot/setup (starvation there -> empty serial output ->
# spurious failure). Only parallelize with >=3 cores; otherwise stay serial.
if [[ "$JOBS" -le 0 ]]; then
    _ncpu=$(nproc 2>/dev/null || echo 1)
    if [[ "$_ncpu" -ge 3 ]]; then JOBS=$(( _ncpu - 2 )); else JOBS=1; fi
fi

# Standardize the whole suite on AXL_TLS=1. The Makefile WIPES .o/libaxl.a/all
# .efi whenever AXL_TLS toggles between builds (Makefile "AXL_TLS state-change
# detection"); with ~64 plain tests and 5 TLS tests sharing one prefix, that
# wipe fires constantly and — run concurrently — clobbers other tests'
# artifacts mid-run. Forcing AXL_TLS on for every test subprocess keeps the
# flags consistent, so the wipe never triggers and concurrent makes stay
# no-ops. (A TLS-enabled lib is a strict superset; plain-http tests just don't
# exercise TLS. The non-TLS *build* is still covered by CI's build job, and the
# strip contract by test-tls-strippable, which itself builds AXL_TLS=1.)
export AXL_TLS=1
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAKE_ARCH=x64; [[ "$ARCH" == "AARCH64" ]] && MAKE_ARCH=aa64

if [[ -n "${RUN_INTEGRATION_DIR:-}" ]]; then
    mapfile -t TESTS < <(ls "$TESTDIR"/test-*.sh 2>/dev/null)
else
    mapfile -t TESTS < <(discover_tests --arch "$ARCH" $INCLUDE_LOCAL)
fi

# Shard selection: keep only this shard's subset, balanced by est= so each
# shard's total estimated wall-clock is roughly equal (greedy
# longest-processing-time-first into the currently-lightest shard).
if [[ -n "$SHARD" ]]; then
    shard_i="${SHARD%%/*}"; shard_k="${SHARD##*/}"
    declare -a pairs=()
    for t in "${TESTS[@]}"; do
        e=$(test_meta_field "$t" est); [[ "$e" =~ ^[0-9]+$ ]] || e=20
        pairs+=("$e|$t")
    done
    declare -a load=() mine=()
    for ((s=0; s<shard_k; s++)); do load[$s]=0; done
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        e="${line%%|*}"; p="${line#*|}"
        min=0
        for ((s=1; s<shard_k; s++)); do
            [[ ${load[$s]} -lt ${load[$min]} ]] && min=$s
        done
        [[ $min -eq $shard_i ]] && mine+=("$p")
        load[$min]=$(( load[$min] + e ))
    done < <(printf '%s\n' "${pairs[@]}" | sort -t'|' -k1,1nr)
    TESTS=("${mine[@]}")
fi

if [[ $LIST_ONLY -eq 1 ]]; then
    [[ ${#TESTS[@]} -gt 0 ]] && printf '%s\n' "${TESTS[@]}"
    exit 0
fi

# Build ONCE up front (real mode only; the self-test seam uses stubs that need
# no build). This populates the shared prefix before the pool starts, so the
# per-test makes that follow are consistent-flag no-ops instead of a thundering
# herd of concurrent first-builds. --no-build skips it (CI builds in a prior job).
if [[ -z "${RUN_INTEGRATION_DIR:-}" && "${RUN_NO_BUILD:-0}" != "1" ]]; then
    echo "pre-building once (ARCH=$MAKE_ARCH AXL_TLS=1 all tests tools axl-busybox)..."
    if ! make -C "$PROJECT_DIR" ARCH="$MAKE_ARCH" AXL_TLS=1 all tests tools axl-busybox \
            > "$LOGDIR/_prebuild.log" 2>&1; then
        echo "pre-build FAILED — see $LOGDIR/_prebuild.log"; exit 1
    fi
fi

# Run one test under a timeout. Each test gets a UNIQUE TEST_PORT_BASE derived
# from its launch index, so no two tests — concurrent or not — share a host
# port (strategy beta). Result code is written by the caller.
run_one() {  # <idx> <test_path>
    local idx="$1" t="$2" name start dur rc port
    name=$(basename "$t"); start=$SECONDS
    port=$(( 20000 + idx*200 ))
    TEST_PORT_BASE=$port \
        timeout "$TIMEOUT" bash "$t" --arch "$ARCH" > "$LOGDIR/$name.log" 2>&1
    rc=$?
    if [[ $rc -ne 0 ]]; then
        # Retry once. Most failures under the parallel pool are transient
        # resource-contention flakes (a QEMU starved during boot emits empty
        # serial output -> the test's greps miss). A genuine failure fails
        # again. Preserve the first attempt's log for post-mortem and let the
        # host drain briefly before the retry so the contention can ease.
        cp -f "$LOGDIR/$name.log" "$LOGDIR/$name.attempt1.log" 2>/dev/null
        sleep 3
        TEST_PORT_BASE=$port \
            timeout "$TIMEOUT" bash "$t" --arch "$ARCH" > "$LOGDIR/$name.log" 2>&1
        rc=$?
        if [[ $rc -eq 0 ]]; then
            dur=$(( SECONDS - start ))
            echo "  $name PASS ${dur}s (retry; attempt 1 failed — see $name.attempt1.log)"
            return 0
        fi
    fi
    dur=$(( SECONDS - start ))
    if [[ $rc -eq 0 ]]; then
        echo "  $name PASS ${dur}s"; return 0
    fi
    # Failure/timeout (twice): replay the test's captured output (logs live in
    # an ephemeral dir that CI discards, so surface the reason inline). Tail
    # keeps the job log readable when many tests fail at once.
    [[ $rc -eq 124 ]] && echo "  $name TIMEOUT ${dur}s (after retry)" \
                      || echo "  $name FAIL ${dur}s (after retry)"
    echo "    --- $name output (last 25 lines) ---"
    tail -n 25 "$LOGDIR/$name.log" 2>/dev/null | sed 's/^/    | /'
    echo "    --- end $name ---"
    return 1
}

# Give every run-qemu invocation a per-run temp base we own (on tmpfs when
# available) and remove it wholesale at exit. run-qemu honors AXL_QEMU_TMPDIR
# for its disk-image/pflash scratch, so this guarantees no per-run temp dir
# leaks regardless of which run-qemu mode a test uses — the background /
# --mount / --cpu-report paths have their own cleanup lifecycles that get
# skipped when the launcher is killed (8.6 GB of such leaks were found
# accumulating). Respect a caller-provided base (don't delete what we didn't
# create).
own_qtmp=0
if [[ -z "${AXL_QEMU_TMPDIR:-}" ]]; then
    qbase=/tmp; [[ -d /dev/shm && -w /dev/shm ]] && qbase=/dev/shm
    AXL_QEMU_TMPDIR=$(mktemp -d -p "$qbase" axl-itest.XXXXXX)
    export AXL_QEMU_TMPDIR
    own_qtmp=1
fi

# Job pool: at most JOBS tests in flight. Each finished test writes its exit
# status (0 = pass) to results/<idx>; we tally after the barrier.
results=$(mktemp -d)
trap 'rm -rf "$results"; [[ "$own_qtmp" == 1 ]] && rm -rf "$AXL_QEMU_TMPDIR"' EXIT INT TERM
idx=0; running=0
for t in "${TESTS[@]}"; do
    ( run_one "$idx" "$t"; echo $? > "$results/$idx" ) &
    idx=$((idx+1)); running=$((running+1))
    if [[ $running -ge $JOBS ]]; then
        wait -n 2>/dev/null || true
        running=$((running-1))
    fi
done
wait

pass=0; failc=0
for f in "$results"/*; do
    if [[ "$(cat "$f")" == "0" ]]; then pass=$((pass+1)); else failc=$((failc+1)); fi
done

echo ""
echo "integration: $pass passed, $failc failed ($ARCH)"
echo "logs: $LOGDIR"
[[ $failc -eq 0 ]]
