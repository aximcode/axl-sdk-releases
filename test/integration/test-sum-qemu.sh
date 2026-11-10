#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0 builds=md5sum,sha256sum guest=standard
# test-sum-qemu.sh -- md5sum.efi and sha256sum.efi against GNU coreutils.
#
# Every expected line is what the HOST's md5sum / sha256sum print for the
# same bytes, captured here at run time -- GNU parity, not "what our tool
# happens to emit". The files: an empty one, "abc", and a random file of
# 3 MiB + 12,345 bytes, so the streamed read crosses many buffer boundaries
# and ends mid-buffer.
#
#   hash        both tools over the three files: one `<hex>  <name>` line
#               each, %lasterror% 0
#   stdin       `md5sum < abc.txt` and `md5sum - < abc.txt` name it `-`
#   check       -c over a list with an OK line, a FAILED line, a `*` binary
#               marker, a missing file and an improperly formatted line:
#               stdout (redirected to a file, then `type`d) and stderr
#               (left on the console) each equal GNU's, and the exit is a
#               failure
#   quiet       --quiet -c and --status -c over the same list, GNU's
#               interleaved stdout+stderr (host stdout unbuffered)
#   check-stdin -c reading the list from stdin
#   round-trip  a list the tool wrote through a Shell `>` (UCS-2) checks OK
#   no-lines    a list with no checksum line at all
#   missing     a missing file and a directory among the operands: the error
#               lines, the remaining file still hashed, a failure exit
#   usage       an unknown flag, and --quiet without -c
#   version     --version
#   gnu-cases   one command each, the guest's whole output plus %lasterror%
#               against the host tool's (stdout unbuffered, stderr merged,
#               exit status): --tag, -b, -t output; -c over a list with
#               comments, empty lines, a CR-only line, a CRLF line and a
#               whitespace-only line; --tag lists (and a mismatched
#               algorithm's); --strict, -w, --status/-w/--quiet last-wins,
#               --ignore-missing (some and all missing); every option
#               conflict GNU refuses; a list that is a directory or missing
#               -b/-t/--tag in every order (the last one wins, --tag
#               sets binary), a file named like a flag bundle after `--`
#   escaped     GNU-escaped list lines (`\\`, `\n`, `\r` in names) and a
#               two-CR line: stdout against the host's, with the one
#               documented difference applied (a name holding only `\` is
#               not escaped on output)
#   long        a list whose last line is exactly 8192 bytes with no line
#               end -- the length limit's boundary -- is a checksum line
#               (stdout against the host's; the file does not exist)
#   zero        -z output written raw through a Shell `>`: the guest's hash
#               of that file equals the host's hash of the Shell's BOM
#               followed by `md5sum -z` output
#   console     no operand with stdin the console (not redirected) is
#               refused at once: the UEFI console has no end-of-file, so a
#               read would never finish (GNU would wait for Ctrl-D)
#
# Usage: ./test/integration/test-sum-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

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

if ! "$PROJECT_DIR/scripts/axl-build.sh" \
        ARCH="$_native_arch" \
        md5sum sha256sum > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "sum test: FAIL (build)"
    exit 1
fi

NATIVE_DIR="$(test_build_dir "$_native_arch")"
MD5_EFI="$NATIVE_DIR/tools/md5sum.efi"
SHA_EFI="$NATIVE_DIR/tools/sha256sum.efi"

FAIL=0
PASS=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# ---------------------------------------------------------------------------
# The files, and GNU's answers for them (run in the directory that becomes
# fs0:\, so the names match).
# ---------------------------------------------------------------------------
F="$TEST_TMPDIR/files"
mkdir -p "$F/sub"
: > "$F/empty.txt"
printf 'abc' > "$F/abc.txt"
head -c $((3 * 1024 * 1024 + 12345)) /dev/urandom > "$F/big.bin"
printf 'x' > "$F/sub/x.txt"
printf 'garbage\n' > "$F/bad.txt"
# The escaped-name file: `a\b` on the host (one name holding a backslash) is
# the path a\b -- file b in directory a -- on the guest. Same bytes.
printf 'x' > "$F/a\\b"
mkdir -p "$F/a"
printf 'x' > "$F/a/b"
# A file whose name looks like a bundle of short flags (-t -w ...): after
# `--` it is a name, and must not switch -w on.
printf 'x' > "$F/-twig"

abc_md5=$(cd "$F" && md5sum abc.txt)
{
    echo "$abc_md5"
    echo "00000000000000000000000000000000  big.bin"
    echo "d41d8cd98f00b204e9800998ecf8427e *empty.txt"
    echo "d41d8cd98f00b204e9800998ecf8427e  nosuch.txt"
    echo "not a checksum line"
} > "$F/sums.md5"
(cd "$F" && sha256sum abc.txt empty.txt) > "$F/ok.sha256"

want_md5=$(cd "$F" && md5sum empty.txt abc.txt big.bin)
want_sha=$(cd "$F" && sha256sum empty.txt abc.txt big.bin)
want_in=$(cd "$F" && md5sum < abc.txt && md5sum - < abc.txt)
want_c_out=$(cd "$F" && md5sum -c sums.md5 2>/dev/null || true)
want_c_err=$(cd "$F" && md5sum -c sums.md5 2>&1 >/dev/null || true)
want_cin=$(cd "$F" && sha256sum -c < ok.sha256)
want_quiet=$(cd "$F" && stdbuf -o0 md5sum --quiet -c sums.md5 2>&1 || true)
want_status=$(cd "$F" && stdbuf -o0 md5sum --status -c sums.md5 2>&1 || true)
want_bad=$(cd "$F" && md5sum -c bad.txt 2>&1 || true)
want_miss=$(cd "$F" && md5sum nosuch.txt sub abc.txt 2>&1 || true)

# Lists for the GNU-parity cases.
{
    echo "# a comment"
    echo ""
    printf '\r\n'
    printf '%s\r\n' "$abc_md5"
    echo "   "
    (cd "$F" && md5sum empty.txt)
} > "$F/cm.md5"
(cd "$F" && md5sum --tag abc.txt empty.txt) > "$F/tag.md5"
echo "MD5(abc.txt)= ${abc_md5%% *}" >> "$F/tag.md5"
{ echo "$abc_md5"; echo "junk"; echo "junk two"; } > "$F/st.md5"
{ echo "$abc_md5"; echo "d41d8cd98f00b204e9800998ecf8427e  nosuch.txt"; } > "$F/im.md5"
echo "d41d8cd98f00b204e9800998ecf8427e  nosuch.txt" > "$F/im2.md5"
{
    (cd "$F" && md5sum 'a\b')
    echo "\\d41d8cd98f00b204e9800998ecf8427e  n\\nl"
    echo "\\d41d8cd98f00b204e9800998ecf8427e  c\\rr"
    printf '%s\r\r\n' "$abc_md5"
} > "$F/esc.md5"

# One command per case: the guest runs it with `.efi` on the tool's name,
# the host as written. Expected = the host's stdout+stderr in write order,
# then EXIT=0x<status> (a tool's `return 1` is %lasterror% 0x1).
GNU_CASES=(
    "TAGOUT|md5sum --tag abc.txt empty.txt"
    "TAGSHA|sha256sum --tag abc.txt"
    "BIN|md5sum -b abc.txt"
    "TXT|md5sum -t abc.txt"
    "TAGBIN|md5sum --tag -b abc.txt"
    "COMMENT|md5sum -c cm.md5"
    "TAGCHK|md5sum -c tag.md5"
    "TAGOTHER|sha256sum -c tag.md5"
    "STRICT|md5sum --strict -c st.md5"
    "WARN|md5sum -w -c st.md5"
    "LASTWARN|md5sum --status -w -c st.md5"
    "LASTSTATUS|md5sum -w --status -c sums.md5"
    "IGNMISS|md5sum --ignore-missing -c im.md5"
    "IGNALL|md5sum --ignore-missing -c im2.md5"
    "IGNMISMATCH|md5sum --ignore-missing -c sums.md5"
    "ZEROC|md5sum -z -c sums.md5"
    "TAGC|md5sum --tag -c sums.md5"
    "BINC|md5sum -b -c sums.md5"
    "TAGTXT|md5sum --tag -t abc.txt"
    "IGNNOC|md5sum --ignore-missing abc.txt"
    "STATNOC|md5sum --quiet --status abc.txt"
    "STRICTNOC|md5sum --strict abc.txt"
    "WARNNOC|md5sum -w abc.txt"
    "DIRLIST|md5sum -c sub"
    "NOLIST|md5sum -c nolist.md5"
    "TTHENB|md5sum -t -b abc.txt"
    "BTHENT|md5sum -b -t abc.txt"
    "TTHENTAG|md5sum -t --tag abc.txt"
    "TAGBT|md5sum --tag -b -t abc.txt"
    "TAGTB|md5sum --tag -t -b abc.txt"
    "TAGLATET|md5sum --tag abc.txt -t"
    "BUNDLETB|md5sum -tb abc.txt"
    "STATIGN|md5sum --status --ignore-missing -c im2.md5"
    "TEXTC|md5sum -t -c sums.md5"
    "QUIETNOC|md5sum --quiet abc.txt"
    "ZTAGC|md5sum --zero --tag -c sums.md5"
    "DASHW|md5sum -- -twig"
)
declare -A GNU_WANT=()
for c in "${GNU_CASES[@]}"; do
    tag="${c%%|*}"
    cmd="${c#*|}"
    GNU_WANT[$tag]=$(cd "$F" && { stdbuf -o0 $cmd 2>&1; echo "EXIT=0x$?"; })
done

# 32 hex + 2 spaces + an 8158-byte name = 8192 bytes, no newline.
{
    printf 'd41d8cd98f00b204e9800998ecf8427e  '
    head -c 8158 /dev/zero | tr '\0' L
} > "$F/long.md5"
want_long=$(cd "$F" && md5sum -c long.md5 2>/dev/null || true)

# The escaped list's stdout. GNU escapes every name holding `\`; this tool
# only one holding a line break (UEFI paths are full of `\`), so a line
# whose escapes are only `\\` loses its leading `\` and its doubling.
want_esc=$(cd "$F" && md5sum -c esc.md5 2>/dev/null | \
    sed -E '/^\\/{/\\[nr]/!{s/^\\//;s/\\\\/\\/g}}' || true)
# -z: the bytes GNU writes, hashed by GNU under the guest's file name. A
# Shell `>` file starts with the UCS-2 BOM the Shell writes when it creates
# it (FF FE); the tool's raw bytes follow it untouched.
want_zero="$(cd "$F" && { printf '\xff\xfe'; md5sum -z abc.txt empty.txt; } | md5sum | sed 's/-$/z.out/')"
# GNU spells `sub` as "sub"; the guest is given the same operand.

for f in empty.txt abc.txt big.bin bad.txt sums.md5 ok.sha256 sub/x.txt \
         a/b cm.md5 tag.md5 st.md5 im.md5 im2.md5 esc.md5 long.md5 -twig; do
    test_add_efi "$F/$f" "$f"
done
test_add_efi "$MD5_EFI"
test_add_efi "$SHA_EFI"

# Generated (its GNU cases are a list above), so test_use_nsh_text: booted, a
# startup.nsh runs it from fs0:\ and then resets; pooled, the agent runs it
# from the lease's share directory. A function, not a `{ } |` pipe: the pipe's
# right side is a subshell, and the script name it sets would die with it.
sum_nsh() {
    echo "@echo -off"
    echo "echo HMD5_BEGIN"
    echo "md5sum.efi empty.txt abc.txt big.bin"
    echo "echo EXIT=%lasterror%"
    echo "echo HMD5_END"
    echo "echo HSHA_BEGIN"
    echo "sha256sum.efi empty.txt abc.txt big.bin"
    echo "echo EXIT=%lasterror%"
    echo "echo HSHA_END"
    echo "echo IN_BEGIN"
    echo "md5sum.efi < abc.txt"
    echo "md5sum.efi - < abc.txt"
    echo "echo IN_END"
    echo "echo C_BEGIN"
    echo "md5sum.efi -c sums.md5 > cout.txt"
    echo "echo EXIT=%lasterror%"
    echo "echo C_END"
    echo "echo CO_BEGIN"
    echo "type cout.txt"
    echo "echo CO_END"
    echo "echo Q_BEGIN"
    echo "md5sum.efi --quiet -c sums.md5"
    echo "echo EXIT=%lasterror%"
    echo "md5sum.efi --status -c sums.md5"
    echo "echo EXIT=%lasterror%"
    echo "echo Q_END"
    echo "echo CIN_BEGIN"
    echo "sha256sum.efi -c < ok.sha256"
    echo "echo EXIT=%lasterror%"
    echo "echo CIN_END"
    echo "echo RT_BEGIN"
    echo "sha256sum.efi abc.txt big.bin > rt.sha256"
    echo "sha256sum.efi -c rt.sha256"
    echo "echo EXIT=%lasterror%"
    echo "echo RT_END"
    echo "echo BAD_BEGIN"
    echo "md5sum.efi -c bad.txt"
    echo "echo EXIT=%lasterror%"
    echo "echo BAD_END"
    echo "echo MISS_BEGIN"
    echo "md5sum.efi nosuch.txt sub abc.txt"
    echo "echo EXIT=%lasterror%"
    echo "echo MISS_END"
    echo "echo USE_BEGIN"
    echo "md5sum.efi --bogus"
    echo "echo EXIT=%lasterror%"
    echo "sha256sum.efi --quiet abc.txt"
    echo "echo EXIT=%lasterror%"
    echo "echo USE_END"
    echo "echo VER_BEGIN"
    echo "md5sum.efi --version"
    echo "sha256sum.efi --version"
    echo "echo VER_END"
    for c in "${GNU_CASES[@]}"; do
        tag="${c%%|*}"
        cmd="${c#*|}"
        echo "echo G_${tag}_BEGIN"
        echo "${cmd/sum /sum.efi }"
        echo "echo EXIT=%lasterror%"
        echo "echo G_${tag}_END"
    done
    echo "echo ESC_BEGIN"
    echo "md5sum.efi -c esc.md5 > esc.out 2> esc.err"
    echo "echo EXIT=%lasterror%"
    echo "echo ESC_END"
    echo "echo ESCO_BEGIN"
    echo "type esc.out"
    echo "echo ESCO_END"
    echo "echo LONG_BEGIN"
    echo "md5sum.efi -c long.md5 > long.out 2> long.err"
    echo "echo EXIT=%lasterror%"
    echo "type long.out"
    echo "echo LONG_END"
    echo "echo Z_BEGIN"
    echo "md5sum.efi -z abc.txt empty.txt > z.out"
    echo "md5sum.efi z.out"
    echo "echo Z_END"
    echo "echo TTY_BEGIN"
    echo "md5sum.efi"
    echo "echo EXIT=%lasterror%"
    echo "echo TTY_END"
    echo "echo SUM_DONE"
}
test_use_nsh_text test-sum-qemu < <(sum_nsh)

test_build_image

echo "=== md5sum / sha256sum ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 90

test_clean_log

echo "--- serial log (HMD5_BEGIN .. SUM_DONE) ---"
sed -n '/^HMD5_BEGIN/,/^SUM_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'

# Strictly between two marker lines; the DEBUG teardown leak verdict (graded
# by test_check_leaks below) and blank lines (`type` ends with one) dropped.
LEAK_VERDICT_RE='^[0-9:.]+ \[INFO\]  mem: no leaks detected$'
win() {
    if ! test_slice_log "$1" "$2" "$TEST_TMPDIR/slice"; then
        echo "<marker $1 or $2 missing>"
        return 0
    fi
    grep -Ev "$LEAK_VERDICT_RE" "$TEST_TMPDIR/slice" | grep -v '^$' || true
}

# expect_win NAME BEGIN END EXPECTED -- the whole window, exactly.
expect_win() {
    local got
    got=$(win "$2" "$3")
    if [[ "$got" == "$4" ]]; then
        pass "$1"
    else
        fail "$1"
        echo "    expected: $(printf '%q' "$4")"
        echo "    got:      $(printf '%q' "$got")"
    fi
}

OK_EXIT='EXIT=0x0'
# A tool's `return 1` reaches %lasterror% as 0x1.
BAD_EXIT='EXIT=0x1'
VER="$(cat "$PROJECT_DIR/VERSION")"

expect_win "hash: md5sum over empty, abc, 3 MiB+ random = GNU" \
    HMD5_BEGIN HMD5_END "$want_md5
$OK_EXIT"
expect_win "hash: sha256sum over empty, abc, 3 MiB+ random = GNU" \
    HSHA_BEGIN HSHA_END "$want_sha
$OK_EXIT"
expect_win "stdin: < and - name it '-'" IN_BEGIN IN_END "$want_in"
expect_win "check: stderr (missing file, WARNING lines) = GNU, failure exit" \
    C_BEGIN C_END "$want_c_err
$BAD_EXIT"
expect_win "check: stdout (OK / FAILED / FAILED open or read) = GNU" \
    CO_BEGIN CO_END "$want_c_out"
expect_win "quiet: --quiet and --status = GNU, failure exits" Q_BEGIN Q_END \
    "$want_quiet
$BAD_EXIT
$want_status
$BAD_EXIT"
expect_win "check-stdin: -c reads the list from stdin" CIN_BEGIN CIN_END \
    "$want_cin
$OK_EXIT"
expect_win "round-trip: a list written through > checks OK" RT_BEGIN RT_END \
'abc.txt: OK
big.bin: OK
'"$OK_EXIT"
expect_win "no-lines: a list with no checksum line = GNU, failure exit" \
    BAD_BEGIN BAD_END "$want_bad
$BAD_EXIT"
expect_win "missing: missing file and directory = GNU, rest hashed, failure exit" \
    MISS_BEGIN MISS_END "$want_miss
$BAD_EXIT"
expect_win "usage: unknown flag, --quiet without -c" USE_BEGIN USE_END \
"md5sum: unknown flag --bogus
md5sum $VER - Print or check MD5 (128-bit) checksums
Usage: md5sum [flags] [<file>...]
  <file>...             Files to hash (with -c: checksum lists); \`-\` or none = stdin
  -b, --binary          Mark each name with \`*\` (binary mode; UEFI has no other)
  -c, --check           Read checksums from the FILEs and check them
      --tag             Print BSD-style lines: ALGO (name) = hex
  -t, --text            Text mode (accepted; the same bytes as binary on UEFI)
  -z, --zero            End each output line with NUL, not newline (raw bytes)
      --ignore-missing  With -c: don't fail or report for missing files
      --quiet           With -c: don't print OK for each verified file
      --status          With -c: print nothing; the exit status says it all
      --strict          With -c: fail on improperly formatted checksum lines
  -w, --warn            With -c: warn about each improperly formatted line
  -h, --help            Show this help
$BAD_EXIT
sha256sum: the --quiet option is meaningful only when verifying checksums
Try 'sha256sum --help' for more information.
$BAD_EXIT"
expect_win "version: both answer --version" VER_BEGIN VER_END \
"md5sum $VER
sha256sum $VER"

for c in "${GNU_CASES[@]}"; do
    tag="${c%%|*}"
    expect_win "gnu: ${c#*|}" "G_${tag}_BEGIN" "G_${tag}_END" "${GNU_WANT[$tag]}"
done
# stderr goes to a file: GNU shell-quotes a name holding a line break in
# its error lines, which this tool does not, so only stdout is compared.
expect_win "escaped: -c over escaped and two-CR lines, failure exit" \
    ESC_BEGIN ESC_END "$BAD_EXIT"
expect_win "escaped: stdout = GNU's (but a \\-only name unescaped)" \
    ESCO_BEGIN ESCO_END "$want_esc"
expect_win "long: an unterminated 8192-byte last line is a checksum line" \
    LONG_BEGIN LONG_END "$BAD_EXIT
$want_long"
expect_win "zero: -z bytes through > hash as GNU's -z output" Z_BEGIN Z_END \
    "$want_zero"

expect_win "console: stdin is the console -> refused, failure exit" \
    TTY_BEGIN TTY_END \
"md5sum: standard input is the console, which has no end of file; name a FILE or redirect stdin
$BAD_EXIT"

if grep -Fxq SUM_DONE "$TEST_CLEAN_LOG"; then
    pass "script ran to SUM_DONE"
else
    fail "script ran to SUM_DONE"
fi

if test_check_leaks; then
    pass "leak gate"
else
    fail "leak gate"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then
    echo "sum test: OK"
    exit 0
fi
echo "sum test: FAIL"
exit 1
