#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0 builds=exit-status-selftest guest=standard
# test-fw-debug-uart-qemu.sh -- a real guest's firmware DEBUG output stays off
# its console, booted and pooled alike, and lands where the harness reads it.
#
# exit-status-selftest.efi returns EFI_COMPROMISED_DATA, and a DEBUG firmware's
# DxeCore reports any image that returns an error on its DEBUG channel:
# "Error: Image at <addr> start failed: Compromised Data" -- a status nothing
# at boot returns, so the line is this run's. On AARCH64 that channel is the
# guest's debug UART (qemu_fw_debug_uart in axl-common.sh), which used to be
# the console; on X64 it is OVMF's 0x402 debugcon, which every pooled X64
# guest has and a booted one has only when asked (--debugcon,
# TEST_QEMU_DEBUGCON).
#
# Asserted, through both of the harness's paths to a guest:
#   common-test.sh   the console (TEST_LOG) carries the app's line and not the
#                    firmware's; TEST_FW_DEBUG_LOG carries it, once, on
#                    AARCH64 and on a pooled X64 guest; a booted X64 guest
#                    keeps no such log (its default has no debugcon).
#   run-qemu.sh      the same, from its --serial-log and the debug log it keeps
#                    beside it (<name>-firmware-debug.log) -- run with the
#                    standard class's machine, so inside a lease it pools too.
#   X64 --debugcon   a booted run with the port asked for: its file carries
#                    the line, once -- the same line, booted and pooled.
#
# Usage: ./test/integration/test-fw-debug-uart-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup
_native="$( [[ "$TEST_ARCH" == AARCH64 ]] && echo aa64 || echo x64 )"
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native" exit-status-selftest 2>&1 | tail -1
APP="$(test_build_dir)/exit-status-selftest.efi"
test_add_efi "$APP"
test_use_nsh
test_build_image

MODE="$(_test_pooled && echo pooled || echo booted)"
echo "=== The firmware's DEBUG output is off the console ($TEST_ARCH, $MODE) ==="

PASS=0 FAIL=0
chk() {
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1 (got [$2], want [$3])"; FAIL=$((FAIL + 1))
    fi
}
APP_LINE='exit-status-selftest: arming 0x8000000000000021, returning rc=1'
FW_RE='^Error: Image at [0-9A-Fa-f]+ start failed: Compromised Data$'
# count_fw FILE: lines of FILE that are the firmware's report (CR dropped).
count_fw() { tr -d '\r' < "$1" 2>/dev/null | grep -acE "$FW_RE" || true; }

test_build_qemu_cmd
test_run_foreground 60
test_clean_log
chk "common-test.sh: the app ran (its line on the console)" \
    "$(grep -acxF "$APP_LINE" "$TEST_CLEAN_LOG")" 1
chk "common-test.sh: the console carries no firmware DEBUG line" "$(count_fw "$TEST_CLEAN_LOG")" 0
if [[ "$TEST_ARCH" == AARCH64 || "$MODE" == pooled ]]; then
    chk "common-test.sh: the firmware's DEBUG log carries it, once" "$(count_fw "$TEST_FW_DEBUG_LOG")" 1
else
    chk "common-test.sh: a booted X64 guest keeps no debug log (no debugcon unasked)" "$TEST_FW_DEBUG_LOG" ""
fi

# run-qemu.sh with the standard class's machine: inside a lease it pools.
RQ_MACHINE=(--bridges --net)
[[ "$TEST_ARCH" == X64 ]] && RQ_MACHINE+=(--nic-model e1000)
RQ_SER="$TEST_TMPDIR/rq.log"
RQ_DBG="$TEST_TMPDIR/rq-firmware-debug.log"
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$TEST_ARCH" "${RQ_MACHINE[@]}" --timeout 60 --no-cpu-warn \
    --serial-log "$RQ_SER" "$APP" 8000000000000021 > "$TEST_TMPDIR/rq.out" 2>&1 || true
chk "run-qemu.sh: the app ran" "$(grep -acxF "$APP_LINE" "$TEST_TMPDIR/rq.out")" 1
chk "run-qemu.sh: its console carries no firmware DEBUG line" \
    "$(count_fw "$RQ_SER")|$(count_fw "$TEST_TMPDIR/rq.out")" "0|0"
if [[ "$TEST_ARCH" == AARCH64 || "$MODE" == pooled ]]; then
    chk "run-qemu.sh: the debug log beside --serial-log carries it, once" "$(count_fw "$RQ_DBG")" 1
else
    chk "run-qemu.sh: a booted X64 run keeps no debug log" "$( [[ -e "$RQ_DBG" ]] && echo present || echo absent)" absent
fi

if [[ "$TEST_ARCH" == X64 ]]; then
    # Booted always (a pooled run cannot take --debugcon): the same line in
    # the port's file, so booted and pooled agree on what the debugcon holds.
    RQ_DCON="$TEST_TMPDIR/rq-debugcon.log"
    "$PROJECT_DIR/scripts/run-qemu.sh" --arch X64 "${RQ_MACHINE[@]}" --timeout 60 --no-cpu-warn \
        --debugcon "$RQ_DCON" "$APP" 8000000000000021 > "$TEST_TMPDIR/rq-dcon.out" 2>&1 || true
    chk "run-qemu.sh --debugcon (booted): the app ran" "$(grep -acxF "$APP_LINE" "$TEST_TMPDIR/rq-dcon.out")" 1
    chk "run-qemu.sh --debugcon (booted): the port's file carries it, once" "$(count_fw "$RQ_DCON")" 1
fi

echo "Results: $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
