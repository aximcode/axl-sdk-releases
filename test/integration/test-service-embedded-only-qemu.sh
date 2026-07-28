#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# test-service-embedded-only-qemu.sh — AxlServiceDeploy.embedded_only must load
# the baked-in driver and SKIP the disk search, so a stale loose driver beside
# the launcher cannot shadow it.
#
# svc_embonly.efi (sdk/examples/svc-embonly.c) embeds the REAL driver (setup
# prints SVC-EMBONLY-EMBEDDED-READY) and deploys it with embedded_only = true.
# The test also stages the DECOY driver (same service name, prints
# SVC-EMBONLY-DECOY-READY) renamed to svc-embonly-dxe.efi — the disk-search
# candidate #1 beside the launcher.
#
# GREEN: the EMBEDDED marker appears and the DECOY marker does NOT — the search
# (and its stale-shadow hazard) was skipped. Without embedded_only the search
# would find the decoy first and print the DECOY marker instead (verified by
# flipping the flag off).
#
# Usage: ./test/integration/test-service-embedded-only-qemu.sh [--arch X64|AARCH64]

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
    svc-embonly 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$NATIVE_DIR/svc_embonly.efi"
# Stage the DECOY as the disk-search filename (candidate #1 beside the
# launcher). If embedded_only skips the search this is never consulted.
test_add_efi "$NATIVE_DIR/svc_embonly-decoy-dxe.efi" "svc-embonly-dxe.efi"

MARKER="AXL-EMBONLY-SHELL-SURVIVED"
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "svc_embonly.efi start"
    echo "echo $MARKER"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlService embedded_only test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

test_clean_log

grep -E "SVC-EMBONLY|svc-embonly: start|$MARKER" "$TEST_CLEAN_LOG" \
    | while IFS= read -r line; do echo "  $line"; done

fails=0

# 1. The EMBEDDED driver ran (loaded straight from the blob).
if grep -q "SVC-EMBONLY-EMBEDDED-READY" "$TEST_CLEAN_LOG"; then
    echo "PASS: embedded driver loaded directly from the blob"
else
    echo "FAIL: embedded marker missing (embedded_only did not load the blob)"; fails=$((fails + 1))
fi

# 2. THE feature assertion: the disk decoy was NOT consulted — the search was
#    skipped, so a stale loose driver cannot shadow the embedded one.
if grep -q "SVC-EMBONLY-DECOY-READY" "$TEST_CLEAN_LOG"; then
    echo "FAIL: DECOY ran — the disk search was NOT skipped (stale-shadow hole open)"; fails=$((fails + 1))
else
    echo "PASS: disk decoy was not consulted (search skipped)"
fi

# 3. Clean return to the shell.
if grep -Fxq "$MARKER" "$TEST_CLEAN_LOG"; then
    echo "PASS: shell regained control"
else
    echo "FAIL: shell never ran the post-start command"; fails=$((fails + 1))
fi

echo ""
if [[ $fails -eq 0 ]]; then
    echo "Service embedded_only test: OK"
    exit 0
else
    echo "Service embedded_only test: FAILED ($fails)"
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi
