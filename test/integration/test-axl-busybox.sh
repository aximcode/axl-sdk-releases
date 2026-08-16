#!/bin/bash
# test-meta: arch=x64 needs= est=8 local-only=0
# axl busybox dispatch test — boots QEMU, runs axl.efi with a few
# representative subcommands, verifies the dispatcher routes argv
# correctly and each tool's body produces the same output it does as
# a standalone .efi.
#
# Usage: ./test/integration/test-axl-busybox.sh [--arch X64|AARCH64]

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
    axl-busybox 2>&1 | tail -5

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/axl.efi"

# Test data — same shape as test-tools.sh's cat / grep coverage. Stage
# files BEFORE test_build_image, otherwise mcopy never sees them.
echo "Hello AXL busybox test data" > "$TEST_STAGING/testdata.txt"
echo "needle in haystack"          > "$TEST_STAGING/match.txt"

echo "=== axl busybox dispatch test ($TEST_ARCH) ==="

# Probe a handful of dispatch shapes: usage/help, two tool subcommands
# that produce well-known stdout, and the unknown-tool error path.
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo BUSYBOX-HELP-START"
    echo "axl.efi --help"
    echo "echo BUSYBOX-HELP-END"
    echo "echo BUSYBOX-CAT-START"
    echo "axl.efi cat testdata.txt"
    echo "echo BUSYBOX-CAT-END"
    echo "echo BUSYBOX-GREP-START"
    echo "axl.efi grep needle match.txt"
    echo "echo BUSYBOX-GREP-END"
    echo "echo BUSYBOX-UNKNOWN-START"
    echo "axl.efi nope"
    echo "echo BUSYBOX-UNKNOWN-END"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 30
test_clean_log

# Lift each probe's output into a variable for assertions. Exit codes
# from EFI shell aren't reliable (it doesn't propagate them well), so
# we grep for content between the BUSYBOX-*-START/END markers.
extract() {
    awk -v start="BUSYBOX-$1-START" -v end="BUSYBOX-$1-END" \
        '$0 ~ start {p=1; next} $0 ~ end {p=0} p' "$TEST_CLEAN_LOG"
}

help_out=$(extract HELP)
cat_out=$(extract CAT)
grep_out=$(extract GREP)
unknown_out=$(extract UNKNOWN)

fails=0

# Help: lists every tool name (use a few representative ones).
for t in cat grep fetch sysinfo netinfo; do
    if ! echo "$help_out" | grep -q "^[[:space:]]*$t[[:space:]]"; then
        echo "FAIL: --help is missing tool '$t'"
        fails=$((fails+1))
    fi
done

# cat: should produce the testdata content via the cat dispatch.
if ! echo "$cat_out" | grep -q "Hello AXL busybox test data"; then
    echo "FAIL: 'axl.efi cat' did not print testdata.txt content"
    fails=$((fails+1))
fi

# grep: should print the line containing 'needle'.
if ! echo "$grep_out" | grep -q "needle in haystack"; then
    echo "FAIL: 'axl.efi grep' did not match the test pattern"
    fails=$((fails+1))
fi

# Unknown tool: dispatcher prints "axl: unknown tool 'nope'" then usage.
if ! echo "$unknown_out" | grep -q "unknown tool 'nope'"; then
    echo "FAIL: 'axl.efi nope' did not surface the unknown-tool error"
    fails=$((fails+1))
fi

echo ""
if [[ $fails -eq 0 ]]; then
    echo "axl busybox test: OK (4 dispatch shapes verified)"
    exit 0
else
    echo "axl busybox test: FAILED ($fails check(s))"
    echo "--- help_out ---"; echo "$help_out"
    echo "--- cat_out ---"; echo "$cat_out"
    echo "--- grep_out ---"; echo "$grep_out"
    echo "--- unknown_out ---"; echo "$unknown_out"
    exit 1
fi
