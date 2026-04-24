#!/bin/bash
# AXL tool tests — boots QEMU and verifies each tool produces correct output.
#
# Usage: ./test/integration/test-tools.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

# Build tools
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tools 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
TOOLS_DIR="$NATIVE_DIR/tools"

# Add tool EFIs to image
for tool in "$TOOLS_DIR"/*.efi; do
    test_add_efi "$tool"
done

# Create test data files
echo "Hello AXL test data for tools" > "$TEST_STAGING/testdata.txt"
mkdir -p "$TEST_STAGING/testdir/subdir"
echo "needle in haystack"  > "$TEST_STAGING/testdir/match.txt"
echo "other content"       > "$TEST_STAGING/testdir/other.log"
echo "sub file"            > "$TEST_STAGING/testdir/subdir/deep.txt"

# Startup script — run each tool and capture its output.
# The host-side script checks the serial log for expected content.
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo ""
    echo "echo === TEST-HEXDUMP ==="
    echo "hexdump.efi -n 32 testdata.txt"
    echo ""
    echo "echo === TEST-GREP-MATCH ==="
    echo "grep.efi Hello testdata.txt"
    echo ""
    echo "echo === TEST-GREP-MISS ==="
    echo "grep.efi ZZZNOMATCH testdata.txt"
    echo ""
    echo "echo === TEST-GREP-RECURSIVE ==="
    echo "grep.efi needle testdir"
    echo ""
    echo "echo === TEST-FIND ==="
    echo "find.efi testdir"
    echo ""
    echo "echo === TEST-SYSINFO ==="
    echo "sysinfo.efi"
    echo ""
    echo "echo === TEST-MKRD-HELP ==="
    echo "mkrd.efi -h"
    echo ""
    echo "echo === TEST-END ==="
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AXL Tool Tests ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

test_clean_log

# ---------------------------------------------------------------------------
# Result checking — validate actual tool output
# ---------------------------------------------------------------------------

PASS=0
FAIL=0

check() {
    local name="$1"
    local pattern="$2"
    if grep -q "$pattern" "$TEST_CLEAN_LOG"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected: $pattern)"
        FAIL=$((FAIL + 1))
    fi
}

# hexdump: should show hex bytes + ASCII decode of "Hello AXL"
check "hexdump-hex-offset"    "00000000:"
check "hexdump-hex-content"   "4865 6c6c 6f20 4158"
check "hexdump-ascii"         "Hello AXL"

# grep: matching line should appear
check "grep-match"            "Hello AXL test data"

# grep: no match should produce no output (just check it didn't crash)
# We verify the TEST-GREP-RECURSIVE marker appears (tool completed)
check "grep-miss-completed"   "=== TEST-GREP-RECURSIVE ==="

# grep: directory argument lists matching files (single-level)
check "grep-recursive-done"   "=== TEST-FIND ==="

# find: should list entries in the test directory
check "find-match-txt"        "match.txt"
check "find-other-log"        "other.log"
check "find-subdir-listed"    "testdir/subdir"

# sysinfo: should show CPU, Memory, Firmware sections
check "sysinfo-cpu"           "=== CPU ==="
check "sysinfo-memory"        "=== Memory ==="
check "sysinfo-firmware"      "=== Firmware ==="
check "sysinfo-uefi-version"  "UEFI:"

# mkrd: help output should show usage
check "mkrd-help-usage"       "Usage: MkRd"
check "mkrd-help-size"        "Size in MB"

# no memory leaks in any tool
check "no-leaks"              "no leaks detected"

echo ""
printf "Tool tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
