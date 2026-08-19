#!/bin/bash
# test-meta: arch=both needs= est=43 local-only=0
# test-9p-tool-qemu.sh -- the `9p` TOOL against a host 9P server. Same
# guest-connects-out topology as test-9p-qemu.sh (p9-server.py on the host,
# reached at 10.0.2.2:<port> through QEMU user-net), but the guest runs the
# shipped tool from the Shell instead of a purpose-built selftest app -- so
# what is under test is the launcher: argv parsing, host[:port] splitting,
# network bring-up, and the printed contract a human reads.
#
# Covers the one-shot verbs: `ls` (entry lines, exact), `get` to stdout, `get`
# to a file with a Shell `type` read-back that proves the bytes landed (not
# just that the tool said so), and `put` from a staged file with a `get`
# read-back that proves the bytes reached the server. The two `get` cases use
# DIFFERENT source files so neither can be satisfied by the other's output.
#
# Also covers the RESIDENT `mount` / `umount` pair: `mount` deploys the
# embedded 9p-mount-dxe.efi, which publishes the remote tree as a UEFI fsN:
# volume; the Shell then `type`s a file THROUGH that volume, which is the
# whole claim -- the mount outlives the command that created it. The read
# targets /dir/a.txt because its content ("alpha") appears nowhere else in
# the run: reading /hello.txt instead would be satisfied by the earlier
# `get` to stdout, so the assertion could not fail.
#
# `umount` is proved symmetrically: unloading the driver image is not the
# same claim as the volume actually going away, so after `9p.efi umount`
# the Shell repeats the SAME read. The guest surviving it at all is the
# check on the dangling-shell-map hazard axl-fs-provider.c's unpublish path
# guards against; "alpha" being ABSENT from that slice is the check that
# the provider actually stopped answering rather than the unload merely
# having looked clean.
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1) -- integration, not a
# unit count.
#
# Usage: ./test/integration/test-9p-tool-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all 9p 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/tools/9p.efi"

# The source file for `9p put`, staged with EXACT bytes -- written from the
# host rather than with the Shell's `echo >a`, which appends CRLF and would
# make the read-back assertion depend on line-ending trivia. Trailing '\n'
# is deliberate (matches p9-server.py's hello.txt): `get` with no outfile
# streams file bytes verbatim (cat.c's exact-streaming convention, no
# forced terminator), so a payload without a trailing newline would run
# the read-back's last line into the next AXL_MEM_DEBUG "no leaks
# detected" shutdown log on the same serial line -- not a tool bug, just
# unrelated diagnostic output sharing a cursor position with un-terminated
# stdout.
printf 'put-ok-9p\n' > "$TEST_STAGING/putsrc.txt"

P9_PORT=$(test_port 0)
P9_PID=0
python3 "$(dirname "$0")/p9-server.py" "$P9_PORT" &
P9_PID=$!
trap 'test_cleanup; [[ $P9_PID -gt 0 ]] && kill $P9_PID 2>/dev/null || true' EXIT

# Interpolated heredoc (unquoted) so $P9_PORT lands in the startup script.
cat << NSHEOF | test_set_startup
@echo -off
fs0:
cd \\

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo TOOL-LS
9p.efi ls 10.0.2.2:${P9_PORT} /
echo TOOL-GET-STDOUT
9p.efi get 10.0.2.2:${P9_PORT} /hello.txt
echo TOOL-GET-FILE
9p.efi get 10.0.2.2:${P9_PORT} /readonly.txt fs0:\\got.txt
type fs0:\\got.txt
echo TOOL-PUT
9p.efi put fs0:\\putsrc.txt 10.0.2.2:${P9_PORT} /puttest.txt
echo TOOL-PUT-RB
9p.efi get 10.0.2.2:${P9_PORT} /puttest.txt
echo TOOL-MOUNT
9p.efi mount 10.0.2.2 --port ${P9_PORT}
echo TOOL-MOUNT-STATUS
9p.efi status
echo TOOL-MOUNT-READ
type fs1:\\dir\\a.txt
echo TOOL-UMOUNT
9p.efi umount
9p.efi status
echo TOOL-UMOUNT-DONE
type fs1:\\dir\\a.txt
echo TOOL-DONE
reset -s
NSHEOF

test_build_image

echo "=== 9p tool one-shot verbs ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID; 9P server PID: $P9_PID on host :$P9_PORT"

if ! test_wait_for "TOOL-DONE" 90; then
    echo "FAIL: the tool run did not finish within 90 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

# Whole-line match against the guest's serial output. A substring match would
# let a wrong size or a wrong name hide inside a right-looking line. $2 narrows
# the search to a slice of the log (see test_slice_log, common-test.sh) when
# the same string could have come from more than one command in the run.
assert_line() {
    local log="${2:-$TEST_CLEAN_LOG}"
    if grep -Fxq "$1" "$log"; then
        pass "$1"
    else
        fail "$1"
    fi
}

# The negative counterpart: PASS when $1 does NOT appear as a whole line in
# the log/slice. Proves the absence-half of a teardown claim (a driver
# unloading is not the same fact as the state it was serving having gone
# away) -- see the umount/-READ assertion below.
#
# An EMPTY slice fails rather than passes. Absence over zero lines is not
# evidence of anything: if the commands that were supposed to produce those
# lines never ran, this assertion would report the teardown proved while
# nothing had been observed at all. (test_slice_log already refuses a
# missing marker outright; this covers the case where both markers are
# present but nothing ran between them.)
assert_absent() {
    local log="${2:-$TEST_CLEAN_LOG}"
    if [[ ! -s "$log" ]]; then
        fail "$1 (the slice is EMPTY -- an absence check over no lines proves nothing)"
    elif grep -Fxq "$1" "$log"; then
        fail "$1 (present; must be ABSENT here)"
    else
        pass "$1 absent"
    fi
}

echo ""
echo "  --- one-shot verbs ---"

# p9-server.py's root: hello.txt (14 B), dir/ (a dir), readonly.txt (18 B).
assert_line "f 14 hello.txt"
assert_line "d 0 dir"
assert_line "f 18 readonly.txt"

# `get` with no outfile streams the bytes to stdout.
assert_line "hello from 9p"

# `get` with an outfile reports what it wrote -- and the Shell's `type` reads
# the file back, so a `get` that reported 18 bytes and wrote garbage fails
# here. A DIFFERENT source file from the stdout case (/readonly.txt, 18 B)
# keeps the two paths independent: neither assertion can be satisfied by the
# other path's output.
assert_line "9p: wrote 18 bytes to fs0:\\got.txt"
assert_line "read-only content"

# `put` reports what it sent; the read-back proves the bytes reached the
# server rather than only the tool's own opinion of success.
assert_line "9p: put 10 bytes to /puttest.txt"
assert_line "put-ok-9p"

echo ""
echo "  --- mount / umount (resident) ---"

MOUNTED_LOG="$TEST_TMPDIR/clean-mounted.log"
MOUNT_STATUS_LOG="$TEST_TMPDIR/clean-status-mounted.log"
MOUNT_READ_LOG="$TEST_TMPDIR/clean-mount-read.log"
UMOUNT_LOG="$TEST_TMPDIR/clean-umounted.log"
UMOUNT_READ_LOG="$TEST_TMPDIR/clean-umount-read.log"
test_slice_log TOOL-MOUNT        TOOL-MOUNT-STATUS "$MOUNTED_LOG"
test_slice_log TOOL-MOUNT-STATUS TOOL-MOUNT-READ   "$MOUNT_STATUS_LOG"
test_slice_log TOOL-MOUNT-READ   TOOL-UMOUNT       "$MOUNT_READ_LOG"
test_slice_log TOOL-UMOUNT       TOOL-UMOUNT-DONE  "$UMOUNT_LOG"
test_slice_log TOOL-UMOUNT-DONE  TOOL-DONE         "$UMOUNT_READ_LOG"

# The mount verb names the volume it published, so a human can type it -- and
# so this test can. `fs1:` is what the guest actually reports here (the boot
# volume is fs0:); the `type` below uses the same name, so a firmware that
# assigned a different one fails BOTH lines rather than silently reading some
# other volume.
assert_line "9p: mounted 10.0.2.2 as fs1:" "$MOUNTED_LOG"
assert_line "9p-mount: running" "$MOUNT_STATUS_LOG"

# The other half of `status`, and the reason it exists at all: the two
# resident services must be INDEPENDENT. They are distinguished only by the
# GUID axl_service_* derives from AxlService::name, so a collision between
# "9p-mount" and "9p-serve" would make `serve-stop` unload the mount (and
# vice versa). Asserting only "9p-mount: running" cannot see that -- a
# collision prints BOTH as running and passes. This line is what turns the
# probe one-sided assertion into a proof: nothing deployed the server in
# this run, so it must read stopped while the mount reads running.
assert_line "9p-serve: stopped" "$MOUNT_STATUS_LOG"

# The payload claim: the Shell reads a file through the published volume,
# after the `9p mount` command has already exited. "alpha" is /dir/a.txt's
# content and appears nowhere else in the run, so nothing but a working mount
# can produce this line.
assert_line "alpha" "$MOUNT_READ_LOG"

assert_line "9p: unmounted" "$UMOUNT_LOG"
assert_line "9p-mount: stopped" "$UMOUNT_LOG"

# The other half of the umount claim: the driver image unloading is not the
# same fact as the volume it published having gone away. Repeat the exact
# read the mounted case used, AFTER umount -- if teardown left the shell map
# entry dangling (the freed-device-path hazard axl-fs-provider.c's unpublish
# path documents) or left the provider still answering despite reporting
# success, "alpha" reappears here and this fails. The guest reaching
# TOOL-DONE at all is the check that the read didn't wedge on freed memory.
assert_absent "alpha" "$UMOUNT_READ_LOG"

echo ""
printf "9p tool one-shot + resident verbs: %d passed, %d failed (%s)\n" \
    "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 15 ]] && exit 0 || exit 1
