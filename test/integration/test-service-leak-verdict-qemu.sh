#!/bin/bash
# test-meta: arch=both needs= est=30 local-only=0 guest=standard
# test-meta: builds=svc-verdict
# test-service-leak-verdict-qemu.sh — a stopped service driver prints its OWN
# teardown leak verdict, so the leak gate sees the driver and not only its
# launcher.
#
# svc_verdict.efi and svc_verdict_leak.efi are AXL_SERVICE launchers with their
# driver embedded (test/integration/svc-verdict.c). Each is started detached and
# then stopped, which unloads the driver image. The clean driver frees what its
# setup allocated; the leak variant's teardown skips that free, so its unload
# must report exactly that block, attributed to svc-verdict.c.
#
# The launchers print a verdict of their own at every exit (four runs), so the
# clean driver's is the fifth clean verdict, and the leaking driver's is the
# only leak report. Each driver also checks that its LoadOptions were there
# during setup and gone once the launcher's start returned.
# Both drivers ask for axl_loop_default() in setup; that loop is the runtime's
# to free at unload, so it must never read as a leak.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-service-leak-verdict-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
# shellcheck source=common-test.sh
source "$SCRIPT_DIR/common-test.sh"
test_parse_args "$@"
test_setup

declare -A _MK=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_MK[$TEST_ARCH]}" svc-verdict 2>&1 | tail -2

PREFIX_DIR="$(test_build_dir)"
test_add_efi "$PREFIX_DIR/svc_verdict.efi"
test_add_efi "$PREFIX_DIR/svc_verdict_leak.efi"

test_use_nsh

test_build_image

echo "=== service leak verdict ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 40
test_clean_log

count() { grep -acE -- "$1" "$TEST_CLEAN_LOG" || true; }

# Both drivers really ran and were really stopped (otherwise a missing verdict
# would prove nothing about unload). Log domains are cut at 15 characters, so
# the leak variant logs as svc-vleak.
for dom in svc-verdict svc-vleak; do
    n_setup=$(count "${dom}: setup$")
    n_down=$(count "${dom}: teardown$")
    if [[ "$n_setup" == 1 && "$n_down" == 1 && "$(count "${dom}: default loop: in use$")" == 1 ]]; then
        test_host_pass "$dom: the driver was set up once (on the default loop) and torn down once"
    else
        test_host_fail "$dom: the driver was set up once (on the default loop) and torn down once (setup=$n_setup teardown=$n_down)"
    fi
done

# The launcher hands the driver its options for its entry point, then takes
# them back: present during setup, and no longer reachable from the driver's
# LoadedImage afterwards -- a pointer left behind would reach the freed copy.
for dom in svc-verdict svc-vleak; do
    if [[ "$(count "${dom}: options at entry: present$")" == 1 \
          && "$(count "${dom}: options after entry: none$")" == 1 ]]; then
        test_host_pass "$dom: LoadOptions present at entry, cleared once the launcher's start returned"
    else
        test_host_fail "$dom: LoadOptions present at entry, cleared once the launcher's start returned ($(grep -aE "${dom}: options" "$TEST_CLEAN_LOG" | sed 's/.*options/options/' | tr -d '\r' | tr '\n' ';' || true))"
    fi
done

# Every launcher exit is clean (four), and so is the clean driver's unload:
# five clean verdicts. start --detach hands the driver its options and exits
# while the driver stays resident, so the launcher must not keep (and so
# leak) its copy of them.
n_clean=$(count 'mem: no leaks detected$')
if [[ "$n_clean" == 5 ]]; then
    test_host_pass "verdicts: five clean (four launcher exits + the clean driver's unload)"
else
    test_host_fail "verdicts: five clean (four launcher exits + the clean driver's unload) (saw $n_clean)"
fi

# The leaking driver's unload: one report, one block, from the fixture.
n_report=$(count '=== AxlMem leak report: 1 allocations, 48 bytes ===$')
n_any=$(count '=== AxlMem leak report: ')
n_block=$(count '\[0\] 48 bytes at .* - test/integration/svc-verdict\.c:[0-9]+$')
if [[ "$n_report" == 1 && "$n_any" == 1 && "$n_block" == 1 ]]; then
    test_host_pass "verdicts: the leaking driver's unload reports its one 48-byte block in svc-verdict.c"
else
    test_host_fail "verdicts: the leaking driver's unload reports its one 48-byte block in svc-verdict.c (report=$n_report any=$n_any block=$n_block)"
fi

# And the gate itself sees it: test_check_leaks fails, naming the fixture.
gate_rc=0
gate_out=$(test_check_leaks 2>&1) || gate_rc=$?
if [[ "$gate_rc" != 0 ]] && grep -qE 'svc-verdict\.c:[0-9]+' <<< "$gate_out"; then
    test_host_pass "leak gate: fails on the driver's leak and names svc-verdict.c"
else
    test_host_fail "leak gate: fails on the driver's leak and names svc-verdict.c (rc=$gate_rc)"
    sed 's/^/    /' <<< "$gate_out"
fi

test_host_summary "service leak verdict ($TEST_ARCH)"
