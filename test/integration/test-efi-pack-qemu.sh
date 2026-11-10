#!/bin/bash
# test-meta: arch=both needs= est=40 local-only=0 builds=pack-fixture,pack-fixture-w,pack-fixture-rt guest=none
# guest=none: it asserts ARGV[0] and image paths rooted at the boot volume
#   (fs0:\sub\...), which on a leased guest is not the test's disk.
# test-efi-pack-qemu.sh -- a packed (compressed, self-extracting) .efi must
# behave exactly like its raw original from every angle the Shell, the user
# and the image itself can observe.
#
# The axl-pack stub decompresses the original and has the firmware LoadImage
# it from a buffer. Three channels have to be carried across by hand, and each
# has a case here that fails when one is dropped:
#
#   LoadOptions        argv              raw-vs-wrapped, argv-quoting
#   ShellParameters    stdio redirect    redirect (the `|` pipes; `>` alone
#                                        cannot tell -- see the case) and
#                                        console, its negative
#   real device path   DeviceHandle /    raw-vs-wrapped SELF, subdir,
#                      FilePath          driver-load
#
# plus exit-status (StartImage's status reaches %lasterror%), corrupt (one
# flipped payload byte ON DISK: two re-reads from the volume, then one final
# line, and the script carries on), retry (a payload damaged in MEMORY only,
# by the test stub, is re-read from the volume and the image runs; loaded from
# a buffer, with no file to re-read, it fails cleanly) and driver-load (a
# wrapped DRIVER through the Shell's `load`).
#
# One boot. pack-fixture.efi is the raw fixture, pack-fixture-w.efi the same
# image force-wrapped, pack-driver-w.efi image-path-driver.efi force-wrapped
# (it prints IMGPATH: self=<its own path> from its entry point), and
# pack-fixture-rt.efi the fixture behind the TEST stub
# (AXL_PACK_STUB_TEST_CORRUPT_FIRST: the first decode sees one payload byte
# damaged in memory, as a flaky BMC virtual-media read leaves it).
#
# Usage: ./test/integration/test-efi-pack-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/pack-lib.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
declare -A _PE_MACHINE_MAP=([X64]=34404 [AARCH64]=43620)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
_machine="${_PE_MACHINE_MAP[$TEST_ARCH]:-34404}"

if ! "$PROJECT_DIR/scripts/axl-build.sh" \
        ARCH="$_native_arch" \
        pack-fixture pack-fixture-w pack-fixture-rt > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "efi-pack test: FAIL (build)"
    exit 1
fi
# What was (re)built: the wrap lines say a changed stub really was relinked.
grep -E '^(axl-efi-pack: |  Built: )' "$TEST_TMPDIR/make.log" || true

NATIVE_DIR="$(test_build_dir "$_native_arch")"
RAW="$NATIVE_DIR/pack-fixture.efi"
WRAPPED="$NATIVE_DIR/pack-fixture-w.efi"
DRIVER_W="$NATIVE_DIR/pack-driver-w.efi"
RETRY="$NATIVE_DIR/pack-fixture-rt.efi"

FAIL=0
PASS=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# ---------------------------------------------------------------------------
# Host-side: the wrapped images really are wrapped, and the corrupt copy.
# ---------------------------------------------------------------------------

for img in "$WRAPPED" "$DRIVER_W" "$RETRY"; do
    off=$(pack_header_offset "$img" "$_machine")
    if [[ "$off" =~ ^[0-9]+$ ]]; then
        pass "$(basename "$img") carries one packed payload (header at $off)"
    else
        fail "$(basename "$img") carries one packed payload (got: $off)"
    fi
done

raw_size=$(stat -c%s "$RAW")
w_size=$(stat -c%s "$WRAPPED")
if [[ "$w_size" -ne "$raw_size" ]] && ! cmp -s "$RAW" "$WRAPPED"; then
    pass "pack-fixture-w.efi differs from the raw image ($raw_size -> $w_size bytes)"
else
    fail "pack-fixture-w.efi differs from the raw image ($raw_size -> $w_size bytes)"
fi

# One flipped byte 256 into the LZMA stream: header intact, stream damaged.
CORRUPT="$TEST_TMPDIR/pack-corrupt.efi"
pack_corrupt_copy "$WRAPPED" "$CORRUPT" "$_machine"
if ! cmp -s "$WRAPPED" "$CORRUPT"; then
    pass "corrupt copy differs from pack-fixture-w.efi"
else
    fail "corrupt copy differs from pack-fixture-w.efi"
fi

# The wrap's BUILD behaviour (AXL_PACK on wraps, off copies raw, a failed
# step leaves the previous image out of date, re-enabling re-wraps) is
# test-cmake-wrap.sh's: it replays the build's own wrap step. This file's
# copy of it drove the Makefile's WRAP_EFI, retired in 5.0.0.

# The other architecture's filter, with a VALID header CRC: the stub carries
# only its own branch converter and must refuse this before decoding. The
# header CRC is CRC-32/IEEE, which is what a gzip trailer carries.
FILTER="$TEST_TMPDIR/pack-filter.efi"
cp "$WRAPPED" "$FILTER"
hdr=$(pack_header_offset "$FILTER" "$_machine")
if [[ "$hdr" =~ ^[0-9]+$ ]]; then
    if [[ "$_machine" == 34404 ]]; then other_filter=2; else other_filter=1; fi
    printf '%b' "$(printf '\\%03o' "$other_filter")" |
        dd of="$FILTER" bs=1 seek=$((hdr + 12)) conv=notrunc status=none
    dd if="$FILTER" bs=1 skip="$hdr" count=32 status=none | gzip -c | tail -c8 | head -c4 |
        dd of="$FILTER" bs=1 seek=$((hdr + 32)) conv=notrunc status=none
fi

test_add_efi "$RAW"
test_add_efi "$WRAPPED"
test_add_efi "$FILTER"
test_add_efi "$WRAPPED" "sub/pack-fixture-w.efi"
test_add_efi "$CORRUPT"
test_add_efi "$DRIVER_W"
test_add_efi "$RETRY"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo RVW_RAW_BEGIN"
    echo "pack-fixture.efi a \"b c\" d"
    echo "echo RVW_RAW_END"
    echo "echo RVW_W_BEGIN"
    echo "pack-fixture-w.efi a \"b c\" d"
    echo "echo RVW_W_END"
    echo "echo AQ0_BEGIN"
    echo "pack-fixture-w.efi"
    echo "echo AQ0_END"
    echo "echo AQ1_BEGIN"
    echo "pack-fixture-w.efi \"x y\""
    echo "echo AQ1_END"
    echo "echo REDIR_RUN_BEGIN"
    echo "pack-fixture-w.efi --write > out.txt"
    echo "echo REDIR_RUN_END"
    echo "echo REDIR_TYPE_BEGIN"
    echo "type out.txt"
    echo "echo REDIR_TYPE_END"
    echo "echo PIPE_OUT_BEGIN"
    echo "pack-fixture-w.efi --write | pack-fixture.efi --read"
    echo "echo PIPE_OUT_END"
    echo "echo PIPE_IN_BEGIN"
    echo "echo piped | pack-fixture-w.efi --read"
    echo "echo PIPE_IN_END"
    echo "echo CON_BEGIN"
    echo "pack-fixture-w.efi --write"
    echo "echo CON_END"
    echo "pack-fixture.efi --exit 7"
    echo "echo EXIT_RAW=%lasterror%"
    echo "pack-fixture-w.efi --exit 7"
    echo "echo EXIT_W=%lasterror%"
    echo "echo SUB_BEGIN"
    echo "fs0:\\sub\\pack-fixture-w.efi"
    echo "echo SUB_END"
    echo "echo XD_RAW_BEGIN"
    echo "pack-fixture.efi --launch \\pack-fixture.efi"
    echo "echo XD_RAW_END"
    echo "echo XD_W_BEGIN"
    echo "pack-fixture.efi --launch \\pack-fixture-w.efi"
    echo "echo XD_W_END"
    echo "echo FLT_BEGIN"
    echo "pack-filter.efi"
    echo "echo FLT_END"
    echo "echo BAD_BEGIN"
    echo "pack-corrupt.efi"
    echo "echo BAD_NEXT_RAN"
    echo "echo BAD_END"
    echo "echo BADNUL_ERR_BEGIN"
    echo "pack-corrupt.efi 2> NUL"
    echo "echo BADNUL_ERR_END"
    echo "echo BADNUL_OUT_BEGIN"
    echo "pack-corrupt.efi > NUL"
    echo "echo BADNUL_OUT_END"
    echo "echo RT_BEGIN"
    echo "pack-fixture-rt.efi a"
    echo "echo RT_END"
    echo "echo RTBUF_W_BEGIN"
    echo "pack-fixture.efi --launch-buf fs0:\\pack-fixture-w.efi"
    echo "echo RTBUF_W_END"
    echo "echo RTBUF_BEGIN"
    echo "pack-fixture.efi --launch-buf fs0:\\pack-fixture-rt.efi"
    echo "echo RTBUF_NEXT_RAN"
    echo "echo RTBUF_END"
    echo "echo DRV_BEGIN"
    echo "load fs0:\\pack-driver-w.efi"
    echo "echo DRV_END"
    echo "echo PACK_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== efi-pack stub behaviour ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 90

test_clean_log

echo "--- serial log (RVW_RAW_BEGIN .. PACK_DONE) ---"
sed -n '/^RVW_RAW_BEGIN/,/^PACK_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'

# Strictly between two marker lines (test_slice_log fails the run when a
# marker is absent, so an empty window never passes by accident). The
# teardown leak verdict every DEBUG image logs is dropped here -- it is a log
# line, not output under test, and test_check_leaks grades it below -- as are
# blank lines (`type` ends with one).
LEAK_VERDICT_RE='^[0-9:.]+ \[INFO\]  mem: no leaks detected$'
win_all() {
    if ! test_slice_log "$1" "$2" "$TEST_TMPDIR/slice"; then
        echo "<marker $1 or $2 missing>"
        return 0
    fi
    grep -Ev "$LEAK_VERDICT_RE" "$TEST_TMPDIR/slice" | grep -v '^$' || true
}
# The window without its LIDP= line: that one carries the firmware's PCI path
# text, which differs per arch and machine, so it is asserted separately
# against the raw image's own line.
win() {
    win_all "$1" "$2" | grep -v '^LIDP=' || true
}
lidp() {
    win_all "$1" "$2" | grep '^LIDP=' || true
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

# --- raw-vs-wrapped: every line identical but the image's own file name.
expect_win "raw-vs-wrapped: raw fixture lines" RVW_RAW_BEGIN RVW_RAW_END \
'ARGC=4
ARGV[0]=pack-fixture.efi
ARGV[1]=a
ARGV[2]=b c
ARGV[3]=d
SELF=FS0:\pack-fixture.efi
BOOT=FS0:\x'
expect_win "raw-vs-wrapped: wrapped fixture lines" RVW_W_BEGIN RVW_W_END \
'ARGC=4
ARGV[0]=pack-fixture-w.efi
ARGV[1]=a
ARGV[2]=b c
ARGV[3]=d
SELF=FS0:\pack-fixture-w.efi
BOOT=FS0:\x'

# LoadImage was given the stub's real device path: the firmware recorded it
# as the inner image's LoadedImageDevicePath, identical to the raw image's
# but for the file name.
raw_lidp=$(lidp RVW_RAW_BEGIN RVW_RAW_END)
w_lidp=$(lidp RVW_W_BEGIN RVW_W_END)
if [[ "$raw_lidp" =~ ^LIDP=.+/\\pack-fixture\.efi$ ]]; then
    pass "raw-vs-wrapped: raw LIDP names the file ($raw_lidp)"
else
    fail "raw-vs-wrapped: raw LIDP names the file ($raw_lidp)"
fi
if [[ "$w_lidp" == "${raw_lidp%\\pack-fixture.efi}\\pack-fixture-w.efi" ]]; then
    pass "raw-vs-wrapped: wrapped LIDP is the raw one but for the file name"
else
    fail "raw-vs-wrapped: wrapped LIDP is the raw one but for the file name ($w_lidp)"
fi

# --- argv-quoting
if [[ "$(win AQ0_BEGIN AQ0_END | head -1)" == "ARGC=1" ]]; then
    pass "argv-quoting: no args -> ARGC=1"
else
    fail "argv-quoting: no args -> ARGC=1"
fi
aq1=$(win AQ1_BEGIN AQ1_END)
if [[ "$(sed -n 1p <<<"$aq1")" == "ARGC=2" \
      && "$(sed -n 3p <<<"$aq1")" == "ARGV[1]=x y" ]]; then
    pass "argv-quoting: \"x y\" -> ARGV[1]=x y"
else
    fail "argv-quoting: \"x y\" -> ARGV[1]=x y"
fi

# --- redirect: nothing on the console while redirected, hello in the file.
expect_win "redirect: nothing reaches the console under >" \
    REDIR_RUN_BEGIN REDIR_RUN_END ''
expect_win "redirect: type out.txt shows hello" \
    REDIR_TYPE_BEGIN REDIR_TYPE_END 'hello'

# A `|` pipe, unlike `>`, reaches an image ONLY through the shell-parameters
# protocol: the Shell swaps gST->ConOut for a `>` file redirect, so `>` keeps
# working for text output even with no protocol on the inner handle. These
# two are the cases that fail when the stub does not pass the protocol on.
expect_win "redirect: wrapped writer | raw reader" PIPE_OUT_BEGIN PIPE_OUT_END \
    'READ=hello'
expect_win "redirect: echo | wrapped reader" PIPE_IN_BEGIN PIPE_IN_END \
    'READ=piped'

# --- console: the same command unredirected prints on the console.
expect_win "console: --write without > prints hello" CON_BEGIN CON_END 'hello'

# --- exit-status
exit_raw=$(grep -E '^EXIT_RAW=' "$TEST_CLEAN_LOG" | head -1 || true)
exit_w=$(grep -E '^EXIT_W=' "$TEST_CLEAN_LOG" | head -1 || true)
if [[ "$exit_raw" == "EXIT_RAW=0x7" ]]; then
    pass "exit-status: raw fixture --exit 7 -> %lasterror% 0x7"
else
    fail "exit-status: raw fixture --exit 7 -> %lasterror% 0x7 (got '$exit_raw')"
fi
if [[ "$exit_w" == "EXIT_W=0x7" ]]; then
    pass "exit-status: wrapped fixture --exit 7 -> %lasterror% 0x7"
else
    fail "exit-status: wrapped fixture --exit 7 -> %lasterror% 0x7 (got '$exit_w')"
fi

# --- subdir: the image path names the subdirectory; the boot volume does not.
expect_win "subdir: fs0:\\sub\\pack-fixture-w.efi" SUB_BEGIN SUB_END \
'ARGC=1
ARGV[0]=fs0:\sub\pack-fixture-w.efi
SELF=FS0:\sub\pack-fixture-w.efi
BOOT=FS0:\x'

sub_lidp=$(lidp SUB_BEGIN SUB_END)
if [[ "$sub_lidp" == "${raw_lidp%\\pack-fixture.efi}\\sub\\pack-fixture-w.efi" ]]; then
    pass "subdir: LIDP is the raw one but for sub\\pack-fixture-w.efi"
else
    fail "subdir: LIDP is the raw one but for sub\\pack-fixture-w.efi ($sub_lidp)"
fi

# --- corrupt: the payload is damaged ON DISK, so both re-reads from the
# volume fail too: one line per re-read, the final line, then the script
# carries on. DxeCore's own report of the failed StartImage is DEBUG output,
# on neither arch's console (x64: the debugcon; aa64: its debug UART), so the
# window is the stub's lines alone.
bad=$(win BAD_BEGIN BAD_END)
if [[ "$bad" == 'axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)
axl-pack: packed image is corrupt -- re-reading from the volume (attempt 2 of 2)
axl-pack: packed image is corrupt
BAD_NEXT_RAN' ]]; then
    pass "corrupt: two re-reads, one final line, next command runs"
else
    fail "corrupt: two re-reads, one final line, next command runs"
    echo "    got: $(printf '%q' "$bad")"
fi

# --- the stub's lines are standard error: `2> NUL` takes them, `> NUL` does
# not. Each sink is told from the console by IDENTITY (the Shell's console
# file), not by "has no file size", which NUL shares with the console.
nul_err=$(win BADNUL_ERR_BEGIN BADNUL_ERR_END)
nul_out=$(win BADNUL_OUT_BEGIN BADNUL_OUT_END)
if [[ -z "$nul_err" ]]; then
    pass "corrupt 2> NUL: the stub's lines go to NUL"
else
    fail "corrupt 2> NUL: the stub's lines go to NUL"
    echo "    got: $(printf '%q' "$nul_err")"
fi
if [[ "$nul_out" == 'axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)
axl-pack: packed image is corrupt -- re-reading from the volume (attempt 2 of 2)
axl-pack: packed image is corrupt' ]]; then
    pass "corrupt > NUL: the stub's lines stay on the console"
else
    fail "corrupt > NUL: the stub's lines stay on the console"
    echo "    got: $(printf '%q' "$nul_out")"
fi

# --- exit data: the Shell discards ExitData, so a launcher mode of the raw
# fixture starts the target itself and prints what StartImage returned.
xd_raw=$(win XD_RAW_BEGIN XD_RAW_END)
xd_w=$(win XD_W_BEGIN XD_W_END)
if [[ "$xd_raw" == 'EXIT=0x8000000000000015 DATA=XDATA' ]]; then
    pass "exit-data: raw image -> EXIT=0x8000000000000015 DATA=XDATA"
else
    fail "exit-data: raw image -> EXIT=0x8000000000000015 DATA=XDATA (got $(printf '%q' "$xd_raw"))"
fi
if [[ "$xd_w" == 'EXIT=0x8000000000000015 DATA=XDATA' ]]; then
    pass "exit-data: wrapped image forwards status and ExitData"
else
    fail "exit-data: wrapped image forwards status and ExitData (got $(printf '%q' "$xd_w"))"
fi

# --- filter: the other arch's filter is refused cleanly, before decoding.
flt=$(win FLT_BEGIN FLT_END)
if [[ "$flt" == 'axl-pack: packed image header is invalid -- re-reading from the volume (attempt 1 of 2)
axl-pack: packed image header is invalid -- re-reading from the volume (attempt 2 of 2)
axl-pack: packed image header is invalid' ]]; then
    pass "filter: a payload naming the absent converter is refused as invalid"
else
    fail "filter: a payload naming the absent converter is refused as invalid (got $(printf '%q' "$flt"))"
fi

# --- retry: the first decode saw a damaged payload; the re-read from the
# volume is good, so ONE retry line and then the image runs exactly as the
# wrapped fixture does.
expect_win "retry: one re-read from the volume, then the image runs" RT_BEGIN RT_END \
'axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)
ARGC=2
ARGV[0]=pack-fixture-rt.efi
ARGV[1]=a
SELF=FS0:\pack-fixture-rt.efi
BOOT=FS0:\x'

# --- retry from a buffer: started with no device path there is no file to
# re-read, so the retry ends at once with the original error -- no crash, and
# the launcher carries on. The control proves --launch-buf itself works.
rtbuf_w=$(win RTBUF_W_BEGIN RTBUF_W_END)
if [[ "$rtbuf_w" == 'EXIT=0x8000000000000015 DATA=XDATA' ]]; then
    pass "retry: control -- a good wrapped image runs from a buffer"
else
    fail "retry: control -- a good wrapped image runs from a buffer (got $(printf '%q' "$rtbuf_w"))"
fi
rtbuf=$(win RTBUF_BEGIN RTBUF_END)
if [[ "$rtbuf" == 'axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)
axl-pack: packed image is corrupt
EXIT=0x8000000000000001 DATA=NULL
RTBUF_NEXT_RAN' ]]; then
    pass "retry: from a buffer, no file to re-read -> the original error"
else
    fail "retry: from a buffer, no file to re-read -> the original error (got $(printf '%q' "$rtbuf"))"
fi

# --- driver-load: the Shell reports success and the driver's marker names it.
drv=$(win DRV_BEGIN DRV_END)
if grep -qE "^IMGPATH: self=FS0:\\\\pack-driver-w\\.efi\$" <<<"$drv"; then
    pass "driver-load: IMGPATH: self=FS0:\\pack-driver-w.efi"
else
    fail "driver-load: IMGPATH: self=FS0:\\pack-driver-w.efi"
fi
if grep -qE "^Image 'FS0:\\\\pack-driver-w\\.efi' loaded at [0-9A-Fa-f]+ - Success\$" <<<"$drv"; then
    pass "driver-load: Shell load reports Success"
else
    fail "driver-load: Shell load reports Success"
fi

if grep -Fxq PACK_DONE "$TEST_CLEAN_LOG"; then
    pass "script ran to PACK_DONE"
else
    fail "script ran to PACK_DONE"
fi

# Every DEBUG image logs a teardown leak verdict; a missing or failing one
# (the wrapped inner image never reaching teardown, say) fails the run.
if test_check_leaks; then
    pass "leak gate"
else
    fail "leak gate"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then
    echo "efi-pack test: OK"
    exit 0
fi
echo "efi-pack test: FAIL"
exit 1
