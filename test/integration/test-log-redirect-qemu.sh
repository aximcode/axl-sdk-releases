#!/bin/bash
# test-meta: arch=both needs= est=20 local-only=0 builds=log-redirect-selftest guest=standard
# test-log-redirect-qemu.sh -- axl_log_set_console_serial(), read back as the
# raw bytes on the serial line.
#
# The redirect moves an image's console log from standard error to a serial
# port, colourless, one write per line; <axl/axl-log.h> also promises that
# axl_log_set_console_enabled(false) silences it, and that NULL sends the log
# back to standard error. log-redirect-selftest.efi writes three WARN lines,
# and WARN is what tells the paths apart: standard error reaches the serial
# terminal through TerminalDxe, which turns the yellow attribute into
# ESC[1m ESC[33m ESC[40m ahead of the line; the redirect writes the line's
# bytes alone.
#
#   on       LOGREDIR-ON is one line with no escape sequence in it
#   silent   LOGREDIR-SILENT is nowhere in the log
#   back     LOGREDIR-BACK is one line with the colour sequence ahead of it
#   done     LOGREDIR-DONE (printf) -- the image ran to its end, so an absent
#            line was not written rather than lost to a fault
#
# The image's own teardown leak verdict is checked as every run's is.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-log-redirect-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
# shellcheck source=common-test.sh
source "$SCRIPT_DIR/common-test.sh"
test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" log-redirect-selftest 2>&1 | tail -2
test_add_efi "$(test_build_dir)/log-redirect-selftest.efi" logredir.efi

echo "=== log redirect ($TEST_ARCH) ==="
test_use_nsh
test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 40
test_clean_log

# Whole raw lines: the harness's "[<seconds>] " stamp, then the record. The
# record's own time of day is [0-9:.]+, so nothing else -- no escape -- can
# stand between the stamp and it.
raw() { grep -caP "^\\[[0-9.]+\\] $1\\r\$" "$TEST_LOG" || true; }
n=$(raw '[0-9:.]+ \[WARN\]  logredir: LOGREDIR-ON')
if [[ "$n" == 1 ]]; then
    test_host_pass "on: the redirected line is written once, with no colour"
else
    test_host_fail "on: the redirected line is written once, with no colour (saw $n; any form: $(grep -ac 'LOGREDIR-ON' "$TEST_LOG" || true))"
fi
n=$(grep -ac 'LOGREDIR-SILENT' "$TEST_LOG" || true)
if [[ "$n" == 0 ]]; then
    test_host_pass "silent: axl_log_set_console_enabled(false) silences the redirect"
else
    test_host_fail "silent: axl_log_set_console_enabled(false) silences the redirect (saw $n)"
fi
n=$(raw '\x1b\[1m\x1b\[33m\x1b\[40m[0-9:.]+ \[WARN\]  logredir: LOGREDIR-BACK')
if [[ "$n" == 1 ]]; then
    test_host_pass "back: after NULL the line is standard error's again, coloured"
else
    test_host_fail "back: after NULL the line is standard error's again, coloured (saw $n; any form: $(grep -ac 'LOGREDIR-BACK' "$TEST_LOG" || true))"
fi
if grep -aqx 'LOGREDIR-DONE' "$TEST_CLEAN_LOG"; then
    test_host_pass "done: the image ran to its end"
else
    test_host_fail "done: the image ran to its end (no LOGREDIR-DONE line)"
fi

rc=0
test_host_summary "log redirect ($TEST_ARCH)" || rc=1
test_check_leaks || rc=1
exit "$rc"
