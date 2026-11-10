#!/bin/bash
# test-meta: arch=x64 needs= est=11 local-only=0 guest=standard
# AXL sed tool test — boots QEMU and checks sed.efi against known GNU-sed
# output for the command/address/regex surface, including BRE-vs-ERE,
# the \xHH escape, ranges, hold space, N-join, -s per-file $, -z
# NUL-delimited input, and the error paths. Each case's expected output is
# exactly what GNU sed prints.
#
# Usage: ./test/integration/test-sed.sh [--arch X64|AARCH64]

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

"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all tools 2>&1 | tail -3

test_add_efi "$(test_build_dir "$_native_arch")/tools/sed.efi"

# Fixtures (LF-only; the harness FAT image preserves them verbatim).
printf '1\n2\n3\n4\n'                             > "$TEST_STAGING/nums.txt"
printf 'a\nb\nc\nd\n'                             > "$TEST_STAGING/pairs.txt"
printf 'Tag: HELLO\nfoo bar baz\nhello world\n'   > "$TEST_STAGING/words.txt"
printf 'a\nb\n'                                   > "$TEST_STAGING/f1.txt"
printf 'c\nd\n'                                   > "$TEST_STAGING/f2.txt"
# NUL-delimited fixture: three records with no trailing NUL.
printf 'a\0b\0c'                                  > "$TEST_STAGING/nulrecs.bin"

# Each case is wrapped in START/END markers so section-scoped checks can
# assert on exactly that command's output.
test_use_nsh

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 40
test_clean_log

PASS=0
FAIL=0

section() {
    awk -v s="=== TEST-$1-START ===" -v e="=== TEST-$1-END ===" \
        'index($0,s){f=1;next} index($0,e){f=0} f' "$TEST_CLEAN_LOG"
}
check_in() {       # name, section-base, pattern
    if section "$2" | grep -q "$3"; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1 (expected '$3' in TEST-$2)"
        echo "    section was:"; section "$2" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

check_in "s/\$/X/ zero-width end (no crash)" DOLLAR  "^aX$"
check_in "s/l/L/g global"                    SUBG    "^heLLo$"
check_in "BRE .*:\\x20 head strip"           BREHEAD "^HELLO$"
check_in "BRE \\(o\\+\\) capture"            BREGRP  "f\[oo\]bar"
check_in "ERE (o+) capture"                  EREGRP  "f\[oo\]bar"
check_in "y/abc/xyz/"                        Y       "^xyzxyz$"
check_in "s/a/X/2 nth"                       NTH     "^abcXbc$"
check_in "s/hello/Q/I case-insensitive"      ICASE   "^Q$"
check_in "-n 2p"                             PRINT2  "^2$"
check_in "-n 2,3p range (2)"                 RANGE   "^2$"
check_in "-n 2,3p range (3)"                 RANGE   "^3$"
check_in "N join a-b"                        NJOIN   "^a-b$"
check_in "N join c-d"                        NJOIN   "^c-d$"
check_in "hold G prints stashed line"        HOLD    "^a$"
check_in "-s per-file \$ (f1 -> b)"          SEP     "^b$"
check_in "-s per-file \$ (f2 -> d)"          SEP     "^d$"
check_in "a APPENDED text"                   APPEND  "APP_SENTINEL"
check_in "bare 0 address rejected"           ZEROERR "invalid use of line address 0"
check_in "-z NUL input: record 'a' alone"    ZNULINPUT "^a\\\$"
check_in "-z NUL input: record 'b' alone"    ZNULINPUT "^b\\\$"

echo ""
echo "sed.efi: $PASS passed, $FAIL failed"
test_cleanup
[[ $FAIL -eq 0 ]]
