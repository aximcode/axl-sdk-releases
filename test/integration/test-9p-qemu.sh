#!/bin/bash
# test-meta: arch=both needs= est=20 local-only=0
# test-9p-qemu.sh — the AxlTestNet `9p-client` mode speaks 9P2000.L to a host
# server: TCP connect, Tversion negotiate, Tattach the root, then walk + lopen
# + chunked Tread /hello.txt, then walk + lopen + Treaddir /dir. Same boot
# then runs 9p-mount-selftest.efi, which mounts the same server as a UEFI
# fsN: volume (axl_9p_mount) and proves the MOUNT path, not just the raw
# client: it reads /hello.txt THROUGH the published volume and checks it
# byte-matches the raw-client oracle, then writes a new file through the
# mount and confirms the write reached the server via a raw-client re-read.
#
# A minimal host 9P2000.L server (p9-server.py) serves a tiny fixed tree. The
# UEFI guest connects OUT to it via QEMU user-net (10.0.2.2:<port>), then
# version-negotiates and attaches (9P-CONNECT-OK), reads /hello.txt
# (9P-READ:hello from 9p), attempts a missing leaf in an existing dir
# (9P-READ-MISSING-OK on a clean failure), lists /dir with per-entry sizes
# (9P-LIST:a.txt:6,b.txt:6,),
# writes/reads back a new file (WRITE-RB), overwrites it to exercise the
# truncate-existing branch (TRUNC-RB), round-trips a 20000-byte buffer to
# exercise the chunked Twrite loop (MULTICHUNK-OK), creates a directory
# (MKDIR-OK), removes the earlier write and confirms it's gone
# (REMOVE-GONE), renames a freshly-written file and reads back the
# destination (RENAME-RB), moves a file ACROSS directories to exercise the
# client's EXDEV copy-then-unlink fallback -- the fixture refuses a
# cross-directory Trename with Rlerror(EXDEV), exactly as AXL's own server
# does (XDEV-RB + XDEV-SRC-GONE) -- then attempts a cross-directory rename of
# a DIRECTORY and confirms the fallback's directory guard refuses it rather
# than treating the tree as a file (XDEV-DIR-REFUSED), then attempts one onto
# a destination that ALREADY EXISTS and confirms both files survive untouched
# (XDEV-EXIST-REFUSED + the two original payloads read back) -- and finishes
# (9P-CLIENT-OK). Then the mount
# selftest enumerates volumes (MOUNT: volumes=), finds the mount by content
# match (MOUNT: MATCH=1), reads a server-side read-only file through the
# mount to prove mount_open honors the caller's requested mode instead of
# forcing O_RDWR (MOUNT: RO-READ=1), and finishes (MOUNT: DONE). Opts out
# of the ratchet (TEST_SKIP_RATCHET=1) — client-mode + mount integration
# tests, not a unit count.
#
# Usage: ./test/integration/test-9p-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 9p-mount-selftest 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"
test_add_efi "$TEST_BUILD_DIR/9p-mount-selftest.efi"

# Host-side 9P server. The guest reaches it at 10.0.2.2:<port> (QEMU user-net
# maps the guest's gateway to the host loopback), same path the HTTP client
# tests use to reach host-server.py.
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

echo Running 9P client mode...
AxlTestNet.efi 9p-client 10.0.2.2 ${P9_PORT}
stall 1000000

echo Running 9P mount selftest...
9p-mount-selftest.efi 10.0.2.2 ${P9_PORT}
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== 9P2000.L client connect + mount ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID; 9P server PID: $P9_PID on host :$P9_PORT"

if ! test_wait_for "9P-CLIENT-OK\|9P-CLIENT-FAIL" 60; then
    echo "FAIL: 9p-client did not finish within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

if ! test_wait_for "MOUNT: DONE\|MOUNT: FAIL" 30; then
    echo "FAIL: 9p-mount-selftest did not finish within 30 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

grep -q "9P-CONNECT-OK" "$TEST_CLEAN_LOG" \
    && pass "connect + version + attach (9P-CONNECT-OK)" \
    || fail "connect ($(grep '9P-CLIENT-FAIL\|9P-CONNECT' "$TEST_CLEAN_LOG" | head -1))"

grep -q "9P-READ:hello from 9p" "$TEST_CLEAN_LOG" \
    && pass "walk + lopen + read /hello.txt (9P-READ)" \
    || fail "read_file ($(grep '9P-READ' "$TEST_CLEAN_LOG" | head -1))"

grep -q "9P-READ-MISSING-OK" "$TEST_CLEAN_LOG" \
    && pass "missing leaf in existing dir fails cleanly (9P-READ-MISSING-OK)" \
    || fail "missing-leaf read ($(grep '9P-READ-MISSING' "$TEST_CLEAN_LOG" | head -1))"

grep -q "9P-CLIENT-OK" "$TEST_CLEAN_LOG" \
    && pass "client mode completed (9P-CLIENT-OK)" \
    || fail "client mode did not complete ($(grep '9P-CLIENT-FAIL' "$TEST_CLEAN_LOG" | head -1))"

# Whole-line, names AND sizes. /dir is the only non-root path listed anywhere
# in the tree, so this line is the sole guard on join_child_path's non-root
# branch: a join bug yields "/dira.txt", whose getattr fails, whose size is
# then 0 -- invisible to a names-only or substring check.
if grep -Fxq "9P-LIST:a.txt:6,b.txt:6," "$TEST_CLEAN_LOG"; then
    pass "walk + lopen + Treaddir /dir with per-entry sizes (9P-LIST:a.txt:6,b.txt:6,)"
else
    fail "list ($(grep '9P-LIST' "$TEST_CLEAN_LOG" | head -1))"
fi

# write a new file, read it back
grep -q "WRITE-RB: hello-9p-write$" "$TEST_CLEAN_LOG" \
    && pass "write_file round-trips (create)" || fail "write_file create"

grep -q "TRUNC-RB: trunc$" "$TEST_CLEAN_LOG" \
    && pass "write_file truncates an existing file" || fail "write_file truncate"

grep -q "MULTICHUNK-OK" "$TEST_CLEAN_LOG" \
    && pass "write_file chunks a >msize write" || fail "write_file multi-chunk"

# mkdir then list shows the new dir; write+remove then read must fail
grep -q "MKDIR-OK: /newdir" "$TEST_CLEAN_LOG" \
    && pass "mkdir creates a dir" || fail "mkdir ($(grep 'MKDIR-FAIL' "$TEST_CLEAN_LOG" | head -1))"

grep -q "REMOVE-GONE: /wtest.txt" "$TEST_CLEAN_LOG" \
    && pass "remove deletes a file" || fail "remove ($(grep 'REMOVE-FAIL' "$TEST_CLEAN_LOG" | head -1))"

grep -q "RENAME-RB: hello-9p-write$" "$TEST_CLEAN_LOG" \
    && pass "rename moves a file (content preserved)" || fail "rename"

# Cross-directory rename: p9-server.py answers Rlerror(EXDEV) exactly as AXL's
# own server does, so these two prove the CLIENT's copy-then-unlink fallback.
# Whole-line (-Fxq) rather than substring: a truncated payload would satisfy a
# substring match on "XDEV-RB:" and the copy bug it exists to catch would ship.
grep -Fxq "XDEV-RB: xdev-payload" "$TEST_CLEAN_LOG" \
    && pass "cross-directory rename falls back to copy-then-unlink" \
    || fail "EXDEV fallback ($(grep 'XDEV-' "$TEST_CLEAN_LOG" | tr '\n' '|'))"

grep -Fxq "XDEV-SRC-GONE" "$TEST_CLEAN_LOG" \
    && pass "the EXDEV fallback removes the source" \
    || fail "EXDEV source cleanup ($(grep 'XDEV-' "$TEST_CLEAN_LOG" | tr '\n' '|'))"

# A cross-directory rename of a DIRECTORY must be refused, not silently
# treated as a file by the fallback -- this is the only guard between "move a
# directory across a slow client fallback" and quietly deleting a tree.
grep -Fxq "XDEV-DIR-REFUSED" "$TEST_CLEAN_LOG" \
    && pass "cross-directory rename of a directory is refused, not copied" \
    || fail "EXDEV directory guard ($(grep 'XDEV-' "$TEST_CLEAN_LOG" | tr '\n' '|'))"

# The EXDEV fallback must also refuse a destination that ALREADY EXISTS -- a
# decision taken explicitly during review, documented as a contract in both
# axl-9p.h and src/9p/README.md. Copy-then-unlink is not atomic, so inheriting
# rename(2)'s permission to clobber would let a mid-copy session drop leave a
# real file truncated. The refusal alone is only half the claim: the other
# half, and the one that actually matters, is that BOTH files still hold
# their original bytes. p9-server.py overwrites at the destination by design,
# so deleting the client-side guard makes the rename SUCCEED and destroy
# /xdst2.txt -- these three lines are what notices.
grep -Fxq "XDEV-EXIST-REFUSED" "$TEST_CLEAN_LOG" \
    && pass "cross-directory rename onto a taken destination is refused" \
    || fail "EXDEV existing-destination guard ($(grep 'XDEV-' "$TEST_CLEAN_LOG" | tr '\n' '|'))"

grep -Fxq "XDEV-EXIST-DST: dst-original" "$TEST_CLEAN_LOG" \
    && pass "the refused destination still holds its original bytes" \
    || fail "EXDEV destination intact ($(grep 'XDEV-EXIST-DST' "$TEST_CLEAN_LOG" | head -1))"

grep -Fxq "XDEV-EXIST-SRC: src-original" "$TEST_CLEAN_LOG" \
    && pass "the refused source still holds its original bytes" \
    || fail "EXDEV source intact ($(grep 'XDEV-EXIST-SRC' "$TEST_CLEAN_LOG" | head -1))"

# --- axl_9p_mount: read/write THROUGH the published fsN: volume ---------

_mount_vols=$(grep -oP "MOUNT: volumes=\K[0-9]+" "$TEST_CLEAN_LOG" | head -1)
[[ -n "$_mount_vols" && "$_mount_vols" -gt 0 ]] \
    && pass "mount publishes an enumerable volume (MOUNT: volumes=$_mount_vols)" \
    || fail "mount volume enumeration ($(grep 'MOUNT: volumes=' "$TEST_CLEAN_LOG" | head -1))"

grep -q "MOUNT: MATCH=1" "$TEST_CLEAN_LOG" \
    && pass "hello.txt read THROUGH the mounted volume byte-matches the raw-client oracle (MOUNT: MATCH=1)" \
    || fail "mount read mismatch ($(grep 'MOUNT: CONTENT=\|MOUNT: MATCH=\|MOUNT: FAIL' "$TEST_CLEAN_LOG" | head -3))"

grep -q "MOUNT: WRITE-RB=mount-write-ok" "$TEST_CLEAN_LOG" \
    && pass "write THROUGH the mount reaches the server (MOUNT: WRITE-RB)" \
    || fail "mount write round-trip ($(grep 'MOUNT: WRITE-RB\|MOUNT: FAIL write' "$TEST_CLEAN_LOG" | head -1))"

grep -q "MOUNT: RO-READ=1" "$TEST_CLEAN_LOG" \
    && pass "read-only file opens O_RDONLY through the mount (MOUNT: RO-READ=1)" \
    || fail "read-only mode fix ($(grep 'MOUNT: RO-READ' "$TEST_CLEAN_LOG" | head -1))"

grep -q "MOUNT: DONE" "$TEST_CLEAN_LOG" \
    && pass "9p-mount-selftest completed (MOUNT: DONE)" \
    || fail "9p-mount-selftest did not finish"

echo ""
printf "9P client connect + mount: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 22 ]] && exit 0 || exit 1
