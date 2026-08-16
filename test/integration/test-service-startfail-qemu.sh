#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# test-service-startfail-qemu.sh — a service driver whose setup fails, deployed
# from an EMBEDDED blob, must fail cleanly and hand control back to the shell.
#
# Regression guard for the driver start-failure double-free. svc-startfail.efi
# (sdk/examples/svc-startfail.c via AXL_SERVICE) embeds a driver whose setup
# returns AXL_ERR. Only the launcher is staged — not svc_startfail-dxe.efi — so
# axl_service_start_embedded's disk search misses and it BUFFER-LOADS the
# embedded blob, which gets a synthesized device path. StartImage runs the
# driver, setup fails, DriverEntry returns an EFI error, and the firmware
# auto-unloads the errored image (freeing that device path). Before the
# liveness-aware image_dp_release / axl_driver_unload fix, AXL freed those
# blocks again -> DXE pool corruption: a silent 100%-CPU spin on RELEASE (X64),
# an `ASSERT [DxeCore] ... Pool.c` on DEBUG (AARCH64). Either way the launcher
# never returned and the shell never got its prompt back.
#
# GREEN: setup runs and fails, the framework declines to attach, the launcher
# exits, and the following shell command (the SHELL-SURVIVED marker) runs. The
# DEBUG-ASSERT is caught globally by common-test.sh's test_run_foreground guard;
# the SHELL-SURVIVED marker additionally catches the silent RELEASE wedge.
#
# Run on BOTH arches: AARCH64 (DEBUG firmware) turns a double-free into a loud
# ASSERT, X64 (RELEASE) proves the no-wedge behavior in the shipping config.
#
# Usage: ./test/integration/test-service-startfail-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

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
    svc-startfail 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
# Stage ONLY the launcher. Deliberately NOT the -dxe: its absence makes the
# 4-path disk search miss so the launcher buffer-loads the embedded blob (the
# path that synthesizes a device path and hit the double-free).
test_add_efi "$NATIVE_DIR/svc_startfail.efi"

MARKER="AXL-STARTFAIL-SHELL-SURVIVED"
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    # Run the failing start TWICE: each failed start buffer-loads the embedded
    # driver afresh, so a per-failure leak of the synthesized device path (or an
    # orphaned image handle) would compound here. Both must fail cleanly and
    # RETURN — if either wedges, the marker below never prints.
    echo "svc_startfail.efi start"
    echo "svc_startfail.efi start"
    echo "echo $MARKER"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlService embedded start-failure test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

test_clean_log

grep -E "SVCSTARTFAIL|svc-startfail|StartImage failed|not attaching|$MARKER" \
    "$TEST_CLEAN_LOG" | while IFS= read -r line; do
    echo "  $line"
done

fails=0

# 1. The driver's setup ran on BOTH starts — proves each failed start
#    re-exercised the buffer-loaded path (where a per-failure leak compounds),
#    not just the first before a wedge.
setup_runs=$(grep -c "SVCSTARTFAIL: setup failing on purpose" "$TEST_CLEAN_LOG" || true)
if [[ "$setup_runs" -ge 2 ]]; then
    echo "PASS: embedded driver loaded and its setup ran on both starts ($setup_runs)"
else
    echo "FAIL: setup ran $setup_runs times (expected 2 — a wedge stops the 2nd)"; fails=$((fails + 1))
fi

# 2. The framework declined to attach — the start genuinely failed (not a
#    silently-swallowed error).
if grep -q "svc-startfail'*: setup returned -1 - not attaching" "$TEST_CLEAN_LOG" \
   || grep -q "StartImage failed for '<embedded>'" "$TEST_CLEAN_LOG"; then
    echo "PASS: framework reported the start failure"
else
    echo "FAIL: start-failure not reported"; fails=$((fails + 1))
fi

# 3. THE regression assertion: the shell got control back. If the double-free
#    wedged the box (RELEASE spin) or ASSERTed (DEBUG — also caught globally),
#    this marker never prints.
if grep -Fxq "$MARKER" "$TEST_CLEAN_LOG"; then
    echo "PASS: shell regained control after the failed start (no wedge)"
else
    echo "FAIL: shell never ran the post-start command (wedge/double-free?)"; fails=$((fails + 1))
fi

echo ""
if [[ $fails -eq 0 ]]; then
    echo "Service start-failure test: OK"
    exit 0
else
    echo "Service start-failure test: FAILED ($fails)"
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi
