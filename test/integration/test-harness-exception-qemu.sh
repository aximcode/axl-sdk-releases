#!/bin/bash
# test-meta: arch=both needs= est=80 local-only=0 builds=crashtest guest=standard
# test-harness-exception-qemu.sh -- the harness SEES a firmware exception,
# booted and pooled alike. CrashTest.efi raises a #UD; OVMF's own handler
# prints its dump on the serial port and the guest hangs. Asserted: the run
# is marked timed out (the test's hang, not a pool failure), the dump is in
# the log the test reads -- pooled, that is the serial slice the harness
# appends after the agent's capture, which is otherwise empty -- and the
# script never got past the fault. Without this, a pooled test's
# "no exception" check could be passing over a stream it never read.
#
# Usage: ./test/integration/test-harness-exception-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup
_native="$( [[ "$TEST_ARCH" == AARCH64 ]] && echo aa64 || echo x64 )"
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native" crashtest 2>&1 | tail -1
test_add_efi "$(test_build_dir)/tools/crashtest.efi" CrashTest.efi
test_use_nsh
test_build_image

echo "=== Harness: a firmware exception reaches the log ($TEST_ARCH, $(_test_pooled && echo pooled || echo booted)) ==="
test_build_qemu_cmd
test_add_no_network
test_run_foreground 15
test_clean_log

case "$TEST_ARCH" in
    X64)     want='Exception Type - 06' ;;
    AARCH64) want='Synchronous Exception' ;;
esac
pass=0; fail=0
chk() { if [[ "$2" == "$3" ]]; then echo "  PASS: $1"; pass=$((pass+1)); else echo "  FAIL: $1 (got [$2], want [$3])"; fail=$((fail+1)); fi; }
chk "the run is marked timed out (the guest hung in the fault)" "$TEST_RUN_TIMED_OUT" 1
chk "the script reached the fault" "$(grep -c '^EXC_BEGIN' "$TEST_CLEAN_LOG")" 1
chk "the firmware's exception dump is in the log ($want)" \
    "$(grep -c "$want" "$TEST_CLEAN_LOG" | awk '{print ($1 > 0)}')" 1
chk "nothing ran past the fault" "$(grep -c '^EXC_NOT_REACHED' "$TEST_CLEAN_LOG")" 0
# AARCH64: only the dump's header is the console's; its registers are DEBUG
# output, on the debug UART -- and the harness appends that to the log on an
# exception, so the dump a decoder needs is still in the log the test reads.
if [[ "$TEST_ARCH" == AARCH64 ]]; then
    chk "the dump's registers are in the log too (ELR, from the debug UART)" \
        "$(grep -cE '  SP 0x[0-9A-F]{16}  ELR 0x[0-9A-F]{16}  SPSR 0x' "$TEST_CLEAN_LOG" | awk '{print ($1 > 0)}')" 1
fi
echo "Results: $pass passed, $fail failed"
(( fail == 0 && pass > 0 ))
