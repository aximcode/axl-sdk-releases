#!/bin/bash
# test-meta: arch=x64 needs= est=8 local-only=0 guest=standard
# test-axl-cc-service.sh — end-to-end proof that `axl-cc --service`
# produces a working launcher + driver pair from a single .c file.
#
# Builds sdk/examples/service-demo.c via:
#
#     axl-cc --service service_demo service-demo.c
#
# which compiles the source twice (once with -DAXL_SERVICE_BUILD_DRIVER
# for service_demo-dxe.efi, once embedding the driver via --embed for
# service_demo.efi) and exercises the full pipeline end-to-end:
# AXL_SERVICE macro -> axl_service_main -> launch_embedded -> driver
# AXL_SERVICE_DRIVER -> setup. Asserts on the round-trip "setup: ..."
# log line under the "service-demo" domain. The links must print no ld
# warning.
#
# Requires scripts/install.sh --arch <arch> to have run first
# (axl-cc + per-arch libs are the artifact under test). Exits 2 if
# the staged SDK isn't present.
#
# Usage: ./test/integration/test-axl-cc-service.sh [--arch X64|AARCH64]

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

AXL_CC="$(test_sdk_dir)/bin/axl-cc"
LIB_DIR="$(test_sdk_dir)/lib/axl/$_native_arch"

if [[ ! -x "$AXL_CC" || ! -d "$LIB_DIR" ]]; then
    echo "ERROR: staged SDK missing for $_native_arch — run 'scripts/install.sh --arch $_native_arch' first" >&2
    exit 2
fi

SRC="$PROJECT_DIR/sdk/examples/service-demo.c"
if [[ ! -f "$SRC" ]]; then
    echo "FAIL: source missing: $SRC"
    exit 1
fi

OUT_DIR=$(mktemp -d)
# test_setup's own EXIT trap (test_cleanup) reaps the guest and TEST_TMPDIR;
# replacing it rather than chaining left a /dev/shm/axl-ctest.* per run.
trap "test_cleanup; rm -rf '$OUT_DIR'" EXIT

# axl-cc --service writes outputs to CWD with fixed names
# (NAME.efi + NAME-dxe.efi); cd into the temp dir so they land there.
echo "+ axl-cc --arch $_native_arch --service service_demo $SRC"
( cd "$OUT_DIR" && "$AXL_CC" --arch "$_native_arch" \
    --service service_demo "$SRC" ) > "$OUT_DIR/build.log" 2>&1
cat "$OUT_DIR/build.log"

# The link is warning-free. The embedded driver blob carries a
# .note.GNU-stack and the toolchain's objects (newlib's memset) none, and ld
# warned about the mix on every --embed link until axl-cc passed
# -z noexecstack.
ld_warnings=$(grep -cE -- '-ld: warning: ' "$OUT_DIR/build.log" || true)
if [[ "$ld_warnings" != 0 ]]; then
    echo "FAIL: the axl-cc link printed $ld_warnings ld warning(s)"
    exit 1
fi

DEMO_EFI="$OUT_DIR/service_demo.efi"
DXE_EFI="$OUT_DIR/service_demo-dxe.efi"

if [[ ! -f "$DEMO_EFI" || ! -f "$DXE_EFI" ]]; then
    echo "FAIL: --service did not produce both outputs"
    echo "  expected: $DEMO_EFI + $DXE_EFI"
    ls -la "$OUT_DIR"
    exit 1
fi

test_add_efi "$DEMO_EFI"

test_use_nsh

test_build_image

echo "=== axl-cc --service Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

test_clean_log

grep -E "service-demo:.*(setup:|teardown|start failed)|^FAIL:" \
    "$TEST_CLEAN_LOG" | while IFS= read -r line; do
    echo "  $line"
done

setup_count=$(grep -c "service-demo:.*setup: port=" "$TEST_CLEAN_LOG" || true)
fails=$(grep -c "service-demo:.*start failed\|^FAIL:" "$TEST_CLEAN_LOG" || true)

opts_ok=0
if grep -qE "service-demo:.*setup: port=8080 verbose=0 name=demo" \
        "$TEST_CLEAN_LOG"; then
    opts_ok=1
fi

# The synthesized CLI must reject the out-of-range --port at parse time
# (axl-args prints "... for --port exceeds max 65535") AND that run must
# never reach setup. This proves the descriptor's min/max propagated into
# the CLI — the positive behavior the unit test can't cover.
range_ok=0
if grep -qE "for --port exceeds max 65535" "$TEST_CLEAN_LOG" \
        && ! grep -q "setup: port=99999" "$TEST_CLEAN_LOG"; then
    range_ok=1
fi

echo ""
echo "Counts: setup=$setup_count FAIL=$fails round_trip_ok=$opts_ok range_ok=$range_ok"

if [[ $fails -eq 0 && $setup_count -ge 1 && $opts_ok -eq 1 && $range_ok -eq 1 ]]; then
    echo "axl-cc --service test: OK"
    exit 0
else
    echo "axl-cc --service test: FAILED"
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi
