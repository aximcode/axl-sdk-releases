#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# sd-ergo — end-to-end fixture built ENTIRELY from the turnkey
# AXL_SHARED_DRIVER / AXL_SHARED_DRIVER_LAUNCHER macros (I/O-model Phase 3).
#
# sd-ergo-driver.c is nothing but three plain functions (ergo_init/ergo_run/
# ergo_unload) + a single AXL_SHARED_DRIVER(...) invocation — no vtable, no
# publish/unpublish, no AXL_DRIVER. sd-ergo-launcher.c is nothing but one
# AXL_SHARED_DRIVER_LAUNCHER(...) invocation — the entire launcher `int main`.
#
# This is the real proof that a shared driver collapses to three functions
# + one launcher macro, with the SDK owning stdin + exit-status:
#   echotext -> driver reads one line via axl_stdin_text() (UCS-2->UTF-8
#               pipe decode), prints ERGO:<line>
#   status   -> driver arms axl_set_exit_status(0x12345678), prints ERGOSTAT;
#               the launcher (via axl_shared_driver_run/_dispatch) must exit
#               with that exact status so the shell's %lasterror% reports it
#
# Usage: ./test/integration/test-sd-ergo-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

# Opt out of the ratchet — this is an end-to-end scenario, not a unit
# binary whose assertion count feeds the ratchet baseline.
export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    sd-ergo 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
LAUNCHER="$NATIVE_DIR/sd-ergo-launcher.efi"

# Skip-and-warn if the fixture could not be built/staged on this box
# (matches the skip convention of the sibling driver tests).
if [[ ! -f "$LAUNCHER" ]]; then
    echo "WARN: sd-ergo fixtures not built on this box; skipping."
    echo "sd-ergo test: SKIP"
    exit 0
fi

test_add_efi "$LAUNCHER"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo ERGO_BEGIN"
    echo "echo hello | sd-ergo-launcher.efi echotext"
    echo "sd-ergo-launcher.efi status"
    echo "echo ESTAT=%lasterror%"
    echo "echo ERGO_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== sd-ergo Macro-Built Fixture Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log

echo "--- serial log (ERGO_BEGIN .. ERGO_DONE) ---"
sed -n '/ERGO_BEGIN/,/ERGO_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'

# Anchored exact-string assertions, scoped to the ERGO_BEGIN..ERGO_DONE
# window so these can't be satisfied by unrelated output.
ergo_section() { sed -n '/ERGO_BEGIN/,/ERGO_DONE/p' "$TEST_CLEAN_LOG"; }
echo_ok=$(ergo_section | grep -c '^ERGO:hello$' || true)
estat_ok=$(ergo_section | grep -c 'ESTAT=0x12345678' || true)
done_marker=$(ergo_section | grep -c '^ERGO_DONE' || true)

echo ""
printf "Results: echo=%d estat=%d done=%d\n" "$echo_ok" "$estat_ok" "$done_marker"

# GREEN requires both the stdin round trip AND the cross-image exit-status
# reflection, and that the shell reached ERGO_DONE (script ran to completion).
if [[ "$echo_ok" -ge 1 && "$estat_ok" -ge 1 && "$done_marker" -ge 1 ]]; then
    echo "sd-ergo test: OK"
    exit 0
else
    echo "sd-ergo test: FAIL"
    exit 1
fi
