#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0 guest=standard
# test-pool-script-status-qemu.sh -- a test script whose last status is 0xE
# (not found) still RUNS on a leased guest.
#
# The agent answers a line by its outcome, and reads a SCRIPT whose last
# status is 0xE as "not run" -- it cannot tell that from a script that was
# never found (include/axl/axl-shell.h, the 0xE row). A `type` of a missing
# file, followed by an `echo` (which sets no status), leaves exactly that.
# common-test.sh used to take the agent's word, call it a pool failure and
# rerun the test booted -- so every pooled test with such a script quietly
# booted. Its .nsh is this test's commands.
#
# Booted and pooled alike: the run must return (a pool miss exits 77 from
# inside it, which the subshell below turns into a failed check), and the
# console must hold the script's lines around the failed `type`, exactly.
#
# Usage: ./test/integration/test-pool-script-status-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

PASS=0 FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1 (got [$2], want [$3])"; FAIL=$((FAIL + 1))
    fi
}

test_use_nsh
test_build_image
echo "=== a script whose last status is 0xE ($TEST_ARCH) ==="
test_build_qemu_cmd
# A subshell, so a pool miss's exit 77 is a status here, not this test's end.
# (The run's own TIMING line is written; the test's summary line, which only
# the main shell writes, counts no run for it.)
rc=0
( test_run_foreground 60 ) || rc=$?
check "the run returned, not a pool miss (exit 77)" "$rc" "0"
# Leased, the run must really have been the pooled one (its exec's output
# file), or this test proves nothing about the pooled path.
if [[ -n "${AXL_POOL_AGENT:-}" ]]; then want=pooled; else want=booted; fi
if [[ -f "$TEST_TMPDIR/exec.out" ]]; then got=pooled; else got=booted; fi
check "it ran $want" "$got" "$want"
test_clean_log
check "the script ran to its last line, around the failed type" \
    "$(grep -axE "before-not-found|type: Cannot open file - 'nosuch.txt'|after-not-found" "$TEST_CLEAN_LOG" | tr '\n' '|')" \
    "before-not-found|type: Cannot open file - 'nosuch.txt'|after-not-found|"

echo "pool-script-status ($TEST_ARCH): $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
