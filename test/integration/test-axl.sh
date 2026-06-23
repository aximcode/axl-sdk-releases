#!/bin/bash
# AXL unit tests — boots QEMU and runs all test .efi applications.
#
# Usage: ./test/integration/test-axl.sh [--arch X64|AARCH64] [--log <path>]
#
# --log <path>   Save the raw QEMU serial log (full firmware boot,
#                Shell session, every test's PASS/FAIL lines) to the
#                given path. Equivalent to setting TEST_KEEP_LOG.

[[ -n "$DEBUG" ]] && set -x

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        --log)  TEST_KEEP_LOG="$2"; export TEST_KEEP_LOG; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64] [--log <path>]"; exit 1 ;;
    esac
done

test_setup

# Map test arch to Makefile arch — matches Makefile default PREFIX=out/native-$(ARCH)
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
TEST_APPS=(AxlTestMem AxlTestString AxlTestIO AxlTestLog AxlTestData AxlTestUtil AxlTestLoop AxlTestSmbus AxlTestTask AxlTestNet AxlTestIpmi AxlTestPlatform AxlTestEvent AxlTestRuntime AxlTestXml AxlTestFsProvider AxlTestGfx AxlTestTruetype AxlTestPixmap AxlTestMath AxlTestInput AxlTestFileView AxlTestPieceTree AxlTestFind AxlTestDriver AxlTestCursor AxlTestCompositor AxlTestGfxRegion AxlTestCrypto AxlTestJose AxlTestNvme AxlTestAta AxlTestScsi AxlTestSmart AxlTestHii AxlTestAuth AxlTestFw)
# Tests deliberately NOT in the default run, each with a reason. The
# guard below treats anything here as accounted-for.
TEST_APPS_SKIP=(
    AxlTestCpuIdle   # ~3s perf workload; run only by test-cpu-idle.sh
)

# Debug override: TEST_APPS_ONLY="AxlTestGfx AxlTestTruetype" runs a subset
# (skips the cross-test ratchet — for fast local iteration only).
if [ -n "${TEST_APPS_ONLY:-}" ]; then
    read -r -a TEST_APPS <<< "$TEST_APPS_ONLY"
else
    # Guard: every built AxlTest*.efi must be either in TEST_APPS (run) or
    # TEST_APPS_SKIP (explicitly excluded). A newly added test that is wired
    # into the Makefile but forgotten here would otherwise build but never
    # run — silently dropping coverage. Make that a hard error instead.
    for _efi in "$NATIVE_DIR"/AxlTest*.efi; do
        [ -e "$_efi" ] || continue
        _name="$(basename "$_efi" .efi)"
        case " ${TEST_APPS[*]} ${TEST_APPS_SKIP[*]} " in
            *" $_name "*) ;;
            *)
                echo "ERROR: $_name.efi is built but is in neither TEST_APPS" \
                     "nor TEST_APPS_SKIP." >&2
                echo "       Add it to one of those lists in" \
                     "test/integration/test-axl.sh." >&2
                exit 1
                ;;
        esac
    done
fi

for app in "${TEST_APPS[@]}"; do
    test_add_efi "$NATIVE_DIR/$app.efi"
done

# Stage the curated PCI vendor/device database next to the EFIs so
# AxlTestPlatform's pci-ids loader tests find it via the standard
# axl_resolve_data_file companion-path lookup. Without this, tests
# would only exercise the "no database loaded" code path.
PCI_IDS_FILE="$PROJECT_DIR/share/pci-ids.json5"
if [[ -f "$PCI_IDS_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$PCI_IDS_FILE" "$TEST_STAGING/pci-ids.json5"
fi
JEDEC_FILE="$PROJECT_DIR/share/jedec.json5"
if [[ -f "$JEDEC_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$JEDEC_FILE" "$TEST_STAGING/jedec.json5"
fi
USB_IDS_FILE="$PROJECT_DIR/share/usb-ids.json5"
if [[ -f "$USB_IDS_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$USB_IDS_FILE" "$TEST_STAGING/usb-ids.json5"
fi

# Test-only class overlay fixture. Carries the "[overlay]" marker
# entry that test_pci_class_db_singleton_overrides loads via the
# explicit-override path — the production sidecar above stays clean
# (zero overrides) so deployed lspci output isn't polluted.
PCI_CLASS_TEST_FILE="$PROJECT_DIR/test/data/pci-class-test.json5"
if [[ -f "$PCI_CLASS_TEST_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$PCI_CLASS_TEST_FILE" "$TEST_STAGING/pci-class-test.json5"
fi

# Stage a deliberately-malformed fixture so the load -1 vs -2 test
# can prove parse failures distinguish from missing-file failures.
mkdir -p "$TEST_STAGING"
cat > "$TEST_STAGING/pci-ids-malformed.json5" <<'EOF'
@@@ deliberately invalid — no token here is valid JSON5 so parse fails
unambiguously even with JSON5's permissive grammar (no unquoted-identifier
key escape, no value-position bareword that could partial-parse).
EOF

# Startup script: init network, then run each test app.
#
# Per-binary timing is extracted host-side from the `=== NAME Tests ===`
# header and the `=== Results: N passed, M failed ===` footer that
# each test emits via axl-test.h. No shell-level markers — an earlier
# revision emitted `echo ____BEGIN/____END` here, but those extra
# UEFI Shell echoes deterministically crashed AxlTestNet on aa64 with
# an ArmCpuDxe data-abort (translation fault) during the socket UDP
# async test's teardown path.
#
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo ""
    echo "connect -r"
    echo "stall 1000000"
    echo "ifconfig -s eth0 dhcp"
    echo "stall 3000000"
    echo ""
    for app in "${TEST_APPS[@]}"; do
        echo "$app.efi"
    done
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AXL Integration Tests ($TEST_ARCH) ==="

test_build_qemu_cmd
# AxlTestNet does several "self-connect" tests against the guest's
# own DHCP IP. Slirp doesn't loopback to the guest, so without help
# every connect waits the full 10 s timeout and SKIPs. Wire the
# stream-echo guestfwd onto each port the suite uses, so the
# client-side connect+send+recv path runs against a real remote.
test_add_network_with_echo 9994 9996 9998 9999
test_run_foreground 120
test_count_results
