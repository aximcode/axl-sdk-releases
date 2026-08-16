#!/bin/bash
# test-meta: arch=x64 needs= est=11 local-only=0
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

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tools 2>&1 | tail -3

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
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo ""
    # s/// at end-of-line — zero-width match (the heap-underflow regression).
    echo "echo === TEST-DOLLAR-START ==="
    echo "echo a | sed.efi \"s/\$/X/\""
    echo "echo === TEST-DOLLAR-END ==="
    # global substitution.
    echo "echo === TEST-SUBG-START ==="
    echo "echo hello | sed.efi \"s/l/L/g\""
    echo "echo === TEST-SUBG-END ==="
    # BRE default + the \xHH (space) escape, greedy head strip.
    echo "echo === TEST-BREHEAD-START ==="
    echo "echo Tag: HELLO | sed.efi \"s/.*:\\x20//\""
    echo "echo === TEST-BREHEAD-END ==="
    # BRE capture group with the GNU \( \) \+ forms.
    echo "echo === TEST-BREGRP-START ==="
    echo "echo foobar | sed.efi \"s/\\(o\\+\\)/[\\1]/\""
    echo "echo === TEST-BREGRP-END ==="
    # ERE capture group (-E) — the bare ( ) + forms.
    echo "echo === TEST-EREGRP-START ==="
    echo "echo foobar | sed.efi -E \"s/(o+)/[\\1]/\""
    echo "echo === TEST-EREGRP-END ==="
    # y/// transliteration.
    echo "echo === TEST-Y-START ==="
    echo "echo abcabc | sed.efi \"y/abc/xyz/\""
    echo "echo === TEST-Y-END ==="
    # Nth-occurrence substitution.
    echo "echo === TEST-NTH-START ==="
    echo "echo abcabc | sed.efi \"s/a/X/2\""
    echo "echo === TEST-NTH-END ==="
    # case-insensitive flag.
    echo "echo === TEST-ICASE-START ==="
    echo "echo HELLO | sed.efi \"s/hello/Q/I\""
    echo "echo === TEST-ICASE-END ==="
    # -n with a print address.
    echo "echo === TEST-PRINT2-START ==="
    echo "sed.efi -n \"2p\" nums.txt"
    echo "echo === TEST-PRINT2-END ==="
    # numeric range.
    echo "echo === TEST-RANGE-START ==="
    echo "sed.efi -n \"2,3p\" nums.txt"
    echo "echo === TEST-RANGE-END ==="
    # N join + substitute the embedded newline.
    echo "echo === TEST-NJOIN-START ==="
    echo "sed.efi \"N;s/\\n/-/\" pairs.txt"
    echo "echo === TEST-NJOIN-END ==="
    # hold space: stash line 1, append it after line 2.
    echo "echo === TEST-HOLD-START ==="
    echo "sed.efi -n \"1h;2{G;p}\" pairs.txt"
    echo "echo === TEST-HOLD-END ==="
    # -s: per-file last line.
    echo "echo === TEST-SEP-START ==="
    echo "sed.efi -s -n \"\$p\" f1.txt f2.txt"
    echo "echo === TEST-SEP-END ==="
    # append a line after.
    echo "echo === TEST-APPEND-START ==="
    echo "echo x | sed.efi \"a APP_SENTINEL\""
    echo "echo === TEST-APPEND-END ==="
    # error path: bare line address 0 is rejected.
    echo "echo === TEST-ZEROERR-START ==="
    echo "echo x | sed.efi \"0d\""
    echo "echo === TEST-ZEROERR-END ==="
    # -z NUL-delimited INPUT: three NUL-separated records a/b/c; -zn 'l'
    # prints each record unambiguously with a trailing '$', one per line.
    # If -z input is broken (reads \n instead), the whole file is one record
    # and 'l' would print the NUL bytes as \000 on a single line.
    echo "echo === TEST-ZNULINPUT-START ==="
    echo "sed.efi -zn l nulrecs.bin"
    echo "echo === TEST-ZNULINPUT-END ==="
    echo ""
    echo "reset -s"
} | test_set_startup

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
