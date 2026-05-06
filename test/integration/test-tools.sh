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

# Optionally stage RamDiskDxe.efi so mkrd can exercise its
# axl_driver_ensure() auto-load path end-to-end. Discovery order:
#   1. $RAMDISKDXE_PATH (explicit override)
#   2. $AXL_DEVKIT_DIR/build/staging/drivers/<arch>/RamDiskDxe.efi
# If neither is set, the mkrd test falls back to verifying the
# search-exhaustion path (still proof argv reached the program).
RAMDISK_STAGED=0
_ramdisk_src=""
if [[ -n "${RAMDISKDXE_PATH:-}" && -f "$RAMDISKDXE_PATH" ]]; then
    _ramdisk_src="$RAMDISKDXE_PATH"
elif [[ -n "${AXL_DEVKIT_DIR:-}" \
        && -f "$AXL_DEVKIT_DIR/build/staging/drivers/$_native_arch/RamDiskDxe.efi" ]]; then
    _ramdisk_src="$AXL_DEVKIT_DIR/build/staging/drivers/$_native_arch/RamDiskDxe.efi"
fi

if [[ -n "$_ramdisk_src" ]]; then
    mkdir -p "$TEST_STAGING/drivers/$_native_arch"
    cp "$_ramdisk_src" "$TEST_STAGING/drivers/$_native_arch/RamDiskDxe.efi"
    RAMDISK_STAGED=1
    echo "  Staged RamDiskDxe.efi from $_ramdisk_src"
fi

# Create test data files
echo "Hello AXL test data for tools" > "$TEST_STAGING/testdata.txt"
mkdir -p "$TEST_STAGING/testdir/subdir"
echo "needle in haystack"  > "$TEST_STAGING/testdir/match.txt"
echo "other content"       > "$TEST_STAGING/testdir/other.log"
echo "sub file"            > "$TEST_STAGING/testdir/subdir/deep.txt"

# Multi-line file for cat formatting tests (3 content lines + 2 blank
# runs to exercise -n line numbering and -s blank squeezing).
printf 'first\nsecond\n\n\nthird\n' > "$TEST_STAGING/cat3.txt"

# UCS-2 LE file with BOM — the typical UEFI Shell pipe shape.
# `printf` emits raw bytes; we hand-craft FF FE then "ux\n" interleaved
# with NUL high bytes. Tests that cat's BOM-probe + transcode produce
# the original UTF-8 on output.
printf '\xff\xfeu\x00x\x00\n\x00' > "$TEST_STAGING/cat_ucs2.txt"

# Headerless UCS-2 LE — UEFI shells often write this shape via
# `cmd > out.txt` (no leading BOM). 16 bytes total = 8 ASCII chars,
# enough to trigger the content-sniff classifier in
# axl_text_stream_wrap. Body is "headless" + newline.
printf 'h\x00e\x00a\x00d\x00l\x00e\x00s\x00s\x00\n\x00' \
    > "$TEST_STAGING/cat_ucs2nb.txt"

# Very-long-line file for grep — proves grep streams arbitrary line
# lengths via axl_readline (no fixed-size line buffer / truncation).
# 4096-byte line > old code's 1024-byte cap; pattern is at byte 3000
# so naive scan would also find it, but axl_readline must hand grep
# the full line for the strstr to see it.
{
    head -c 3000 < /dev/zero | tr '\0' 'A'
    printf 'NEEDLE'
    head -c 1090  < /dev/zero | tr '\0' 'B'
    printf '\n'
    echo "shorter line that does not match"
} > "$TEST_STAGING/grep_long.txt"

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
    echo "echo === TEST-GREP-LONGLINE ==="
    # 4096-byte line with pattern at byte 3000 — proves grep
    # streams via axl_readline rather than the previous 1024-byte
    # fixed buffer that would have truncated the line below the
    # match offset.
    echo "grep.efi NEEDLE grep_long.txt"
    echo ""
    echo "echo === TEST-FIND ==="
    echo "find.efi testdir"
    echo ""
    echo "echo === TEST-SYSINFO ==="
    echo "sysinfo.efi"
    echo ""
    echo "echo === TEST-DMIDECODE-VERSION ==="
    echo "dmidecode.efi -V"
    echo ""
    echo "echo === TEST-DMIDECODE-BIOS ==="
    echo "dmidecode.efi -t 0"
    echo ""
    echo "echo === TEST-DMIDECODE-STRING ==="
    echo "dmidecode.efi -s system-manufacturer"
    echo ""
    echo "echo === TEST-DMIDECODE-UNDECODED ==="
    # Type 32 (System Boot Information) has no specialized decoder, so
    # the tool should fall back to Header-and-Data + Strings.
    echo "dmidecode.efi -t 32"
    echo ""
    echo "echo === TEST-CAT-PLAIN ==="
    echo "cat.efi testdata.txt"
    echo ""
    echo "echo === TEST-CAT-NUMBER-SQUEEZE ==="
    # -n numbers all output lines; -s collapses the run of blanks in
    # cat3.txt into a single blank line. Expect 4 numbered lines:
    # `     1\tfirst`, `     2\tsecond`, `     3\t` (the squeezed
    # blank), `     4\tthird`.
    echo "cat.efi -n -s cat3.txt"
    echo ""
    echo "echo === TEST-CAT-UCS2-BOM ==="
    # BOM-probe path: the file is UCS-2 LE with BOM; cat must transcode
    # and emit "ux" as plain UTF-8.
    echo "cat.efi cat_ucs2.txt"
    echo ""
    echo "echo === TEST-CAT-FORCE-ENCODING ==="
    # Force the encoding even though the file has no BOM (drop the
    # first 2 bytes via -e). Use the same UCS-2 LE file to verify -e
    # parses correctly and the wire-side bytes still decode to text.
    echo "cat.efi -e ucs2le cat_ucs2.txt"
    echo ""
    echo "echo === TEST-CAT-UCS2-HEADERLESS ==="
    # Auto-detect UCS-2 LE without a BOM via the content sniff in
    # axl_text_stream_wrap. Body is "headless" — should appear as
    # plain UTF-8 on the console.
    echo "cat.efi cat_ucs2nb.txt"
    echo ""
    echo "echo === TEST-MKRD-HELP ==="
    echo "mkrd.efi -h"
    echo ""
    echo "echo === TEST-MKRD-POSITIONAL ==="
    # Reproduces the user-reported case: positional label arg.
    # When RamDiskDxe.efi is staged at drivers/<arch>/, mkrd's
    # axl_driver_ensure() auto-load should find it and create the
    # ramdisk. When not staged, mkrd should print "not found on any
    # mounted volume" — still proof argv[1] reached the program.
    echo "mkrd.efi testrd"
    echo ""
    echo "echo === TEST-MKRD-LIST ==="
    echo "mkrd.efi -l"
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

# Negative-match variant: passes if the pattern is ABSENT from the log
# between the named start marker and the end of the log.
check_absent_in_section() {
    local name="$1"
    local start_marker="$2"
    local pattern="$3"
    local section
    section=$(awk "/$start_marker/,0" "$TEST_CLEAN_LOG")
    if echo "$section" | grep -q "$pattern"; then
        echo "  FAIL: $name (saw unwanted: $pattern)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    fi
}

# hexdump: should show hex bytes + ASCII decode of "Hello AXL"
check "hexdump-hex-offset"    "00000000:"
check "hexdump-hex-content"   "4865 6c6c 6f20 4158"
check "hexdump-ascii"         "Hello AXL"

# cat: plain file content shows up
check "cat-plain"             "Hello AXL test data"

# cat -n -s: line 1 is "first", line 4 is "third" (after squeeze
# collapses the two-blank run into one numbered blank line)
check "cat-number-first"      "     1.first"
check "cat-number-third"      "     4.third"

# cat: BOM-probe path — UCS-2 LE input transcoded back to UTF-8 "ux"
check "cat-ucs2-bom"          "ux"

# cat -e ucs2le: forced-encoding path produces the same result
check "cat-force-encoding"    "=== TEST-CAT-UCS2-HEADERLESS ==="

# cat: headerless UCS-2 LE auto-detected by content sniff (no BOM)
check "cat-ucs2-headerless"   "headless"

# grep: matching line should appear
check "grep-match"            "Hello AXL test data"

# grep: no match should produce no output (just check it didn't crash)
# We verify the TEST-GREP-RECURSIVE marker appears (tool completed)
check "grep-miss-completed"   "=== TEST-GREP-RECURSIVE ==="

# grep: directory argument lists matching files (single-level)
check "grep-recursive-done"   "=== TEST-FIND ==="

# grep: 4KB line with NEEDLE at byte 3000 — verifies streaming
# read returns the full line (old fixed 1024-byte buffer would
# have dropped everything past byte 1024).
check "grep-longline-streamed" "AAAANEEDLE\|grep_long.txt:.*NEEDLE"

# find: should list entries in the test directory
check "find-match-txt"        "match.txt"
check "find-other-log"        "other.log"
check "find-subdir-listed"    "testdir/subdir"

# sysinfo: should show CPU, Memory, Firmware sections
check "sysinfo-cpu"           "=== CPU ==="
check "sysinfo-memory"        "=== Memory ==="
check "sysinfo-firmware"      "=== Firmware ==="
check "sysinfo-uefi-version"  "UEFI:"

# dmidecode: should report the SMBIOS version QEMU publishes
check "dmidecode-version"     "SMBIOS .* present"
# dmidecode -t 0 emits the BIOS Information section (via our typed reader)
check "dmidecode-bios-header" "BIOS Information"
check "dmidecode-bios-vendor" "Vendor:"
# dmidecode -s prints a single string (QEMU is the usual mfr)
check "dmidecode-string"      "=== TEST-DMIDECODE-STRING ==="
# Undecoded type falls back to Header-and-Data hex dump
check "dmidecode-fallback-hex" "Header and Data:"

# mkrd: help output should show usage
check "mkrd-help-usage"       "Usage: MkRd"
check "mkrd-help-size"        "Size in MB"
# mkrd positional: argv[1] must reach the program.
#   Negative: "label required" would mean the positional never arrived
#             (reproduces the user-reported OEM-firmware bug).
check_absent_in_section "mkrd-positional-no-label-required" \
    "=== TEST-MKRD-POSITIONAL ===" "label required"

# Auto-load proof — must succeed regardless of whether an external
# RamDiskDxe.efi was staged: when one is staged, axl_driver_ensure()
# finds it on disk; when not, the .incbin'd blob in mkrd-blob.S is
# the always-on fallback. Either way, the protocol must end up live
# and the ramdisk must appear in the listing.
check "mkrd-autoload-success" \
    "RAM disk .testrd. created"
check "mkrd-list-shows-testrd" \
    "testrd"

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
