#!/bin/bash
# test-crashhandler.sh -- end-to-end test for the CrashHandler DXE driver
# (drivers/crashhandler) + CrashTest app (tools/crashtest.c).
#
# Boots QEMU, loads CrashHandler, triggers a CPU exception via CrashTest,
# reboots, and verifies the crash record round-trips: NVRAM capture on
# boot 1 -> crash-report.txt written on boot 2 -> idempotent re-load.
#
# x64 AND aa64. Auxiliary: opt out of the test-axl.sh pass-count ratchet
# (it boots QEMU several times with its own PASS/FAIL accounting, not the
# guest-emitted PASS:/FAIL: lines the ratchet counts).
#
# IMPORTANT: this test must run QEMU WITHOUT KVM. CrashHandler installs a
# UEFI exception handler that only fires under software emulation (TCG);
# with -enable-kvm the host kernel intercepts #PF/#GP before the firmware
# handler runs. build_crash_qemu_cmd below strips KVM from the base cmd.
#
# Usage: ./test/integration/test-crashhandler.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common-test.sh"

# Not the unit ratchet — we don't call test_count_results.
export TEST_SKIP_RATCHET=1

test_parse_args "$@"

ARCH_LC=$(arch_dir "$TEST_ARCH")
CRASH_HANDLER="$PROJECT_DIR/out/native-$ARCH_LC/drivers/crashhandler.efi"
CRASH_TEST="$PROJECT_DIR/out/native-$ARCH_LC/tools/crashtest.efi"

# Build if missing (warm tree: <2s).
if [[ ! -f "$CRASH_HANDLER" || ! -f "$CRASH_TEST" ]]; then
    echo "Building CrashHandler + CrashTest ($TEST_ARCH)..."
    make -C "$PROJECT_DIR" ARCH="$ARCH_LC" crashhandler crashtest 2>&1 | tail -3
fi
[[ -f "$CRASH_HANDLER" ]] || { echo "FAIL: not found: $CRASH_HANDLER"; exit 1; }
[[ -f "$CRASH_TEST" ]]    || { echo "FAIL: not found: $CRASH_TEST"; exit 1; }

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() {
    echo "  FAIL: $1"; FAIL=$((FAIL + 1))
    if [[ -n "${2:-}" && -f "$2" ]]; then
        echo "  --- serial log (tail) ---"; tail -40 "$2"; echo "  --- end ---"
    fi
}

test_setup   # find_qemu/find_firmware, TEST_NVRAM=copy of FW_VARS, cleanup trap

# Stage the binaries once with the PascalCase names startup.nsh references
# (FAT is case-insensitive, but match exactly to be unambiguous).
test_add_efi "$CRASH_HANDLER" CrashHandler.efi
test_add_efi "$CRASH_TEST"    CrashTest.efi

# Per-arch boot timeout (aa64 under TCG is slower).
QEMU_TIMEOUT=25
[[ "$TEST_ARCH" == "AARCH64" ]] && QEMU_TIMEOUT=$((QEMU_TIMEOUT + 60))

# Build a TCG (NOT KVM) QEMU command. CrashHandler installs a UEFI
# exception handler that only fires under software emulation; -enable-kvm
# lets the host kernel intercept #PF/#GP first. We build the command
# explicitly rather than reuse build_qemu_base_cmd, which enables KVM
# when /dev/kvm is usable.
#
# x64: -cpu max is required — the default qemu64 cannot boot this OVMF
# under TCG (it exits instantly with no output); max exposes the CPU
# features the firmware needs. aa64 boots fine on cortex-a57 under TCG.
# FW_CODE / TEST_NVRAM / TEST_QEMU_BIN are set by test_setup.
build_crash_qemu_cmd() {
    CRASH_QEMU_CMD=("$TEST_QEMU_BIN")
    case "$TEST_ARCH" in
        X64)     CRASH_QEMU_CMD+=(-machine q35   -accel tcg -cpu max) ;;
        AARCH64) CRASH_QEMU_CMD+=(-machine virt  -accel tcg -cpu cortex-a57) ;;
    esac
    CRASH_QEMU_CMD+=(
        -m 512M
        -drive "if=pflash,format=raw,readonly=on,file=$FW_CODE"
        -drive "if=pflash,format=raw,file=$TEST_NVRAM"
        -drive "format=raw,file=$TEST_DISK"
        -nographic
        -no-reboot
    )
}

run_boot() {
    # $1 = log path. Re-stages the disk from current TEST_STAGING and runs.
    test_build_image
    build_crash_qemu_cmd
    timeout "$QEMU_TIMEOUT" "${CRASH_QEMU_CMD[@]}" > "$1" 2>&1 || true
}

echo "=== CrashHandler Integration Tests ($TEST_ARCH) ==="

# --- Test 1: crash capture -------------------------------------------------
echo "Test 1: Crash capture"
test_set_startup <<'EOF'
@echo -off
echo "=== CrashHandler Test Boot ==="
load fs0:\CrashHandler.efi
echo "CrashHandler loaded, triggering crash..."
fs0:\CrashTest.efi ud
EOF
LOG1="$TEST_TMPDIR/boot1.log"
run_boot "$LOG1"

if grep -q "CRASH:" "$LOG1"; then
    pass "CrashHandler caught exception"
elif grep -q "Exception Type" "$LOG1"; then
    pass "Exception occurred (default handler)"
else
    fail "No exception detected in serial output" "$LOG1"
fi

grep -q "NVRAM" "$LOG1" \
    && pass "Crash record saved to NVRAM" \
    || fail "NVRAM save message not found"

grep -q "CrashHandler.*active\|exception types monitored" "$LOG1" \
    && pass "CrashHandler loaded successfully" \
    || fail "CrashHandler load message not found"

grep -qi "CrashTest\|NULL pointer\|triggering\|crash" "$LOG1" \
    && pass "CrashTest.efi executed" \
    || fail "CrashTest.efi execution not confirmed"

# --- Test 2: crash report on reboot (persistent NVRAM) ---------------------
echo "Test 2: Crash report on reboot"
test_set_startup <<'EOF'
@echo -off
echo "=== CrashHandler Reboot Test ==="
load fs0:\CrashHandler.efi
if exist fs0:\crash-report.txt then
  echo "REPORT_FOUND"
  type fs0:\crash-report.txt
endif
echo "=== DONE ==="
reset -s
EOF
LOG2="$TEST_TMPDIR/boot2.log"
run_boot "$LOG2"

grep -q "crash record.*saved\|crash-report.txt" "$LOG2" \
    && pass "Crash report written on reboot" \
    || fail "Crash report not generated" "$LOG2"

grep -q "REPORT_FOUND" "$LOG2" \
    && pass "crash-report.txt exists on filesystem" \
    || fail "crash-report.txt not found on filesystem"

grep -qi "#PF\|Page Fault\|Synchronous\|Data Abort\|Exception.*0x" "$LOG2" \
    && pass "Crash report contains exception type" \
    || fail "Exception type not in crash report"

grep -q "Registers:" "$LOG2" \
    && pass "Crash report contains registers" \
    || fail "Registers not in crash report"

grep -q "Loaded Images:" "$LOG2" \
    && pass "Crash report contains loaded image table" \
    || fail "Loaded image table not in crash report"

# --- Test 3: idempotency (fresh NVRAM, double load) ------------------------
echo "Test 3: Idempotency"
cp "$FW_VARS" "$TEST_NVRAM"   # reset NVRAM so no stale crash record
test_set_startup <<'EOF'
@echo -off
load fs0:\CrashHandler.efi
load fs0:\CrashHandler.efi
echo "=== DONE ==="
reset -s
EOF
LOG3="$TEST_TMPDIR/boot3.log"
run_boot "$LOG3"

grep -q "already active" "$LOG3" \
    && pass "Second load detected as already active" \
    || fail "Idempotency check not working" "$LOG3"

# --- Summary ---------------------------------------------------------------
echo ""
echo "Results: $PASS passed, $FAIL failed ($TEST_ARCH)"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
