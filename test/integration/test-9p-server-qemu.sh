#!/bin/bash
# test-meta: arch=both needs= est=110 local-only=0
# test-9p-server-qemu.sh — the FIRST thing that ever executes Axl9pServer's
# handler code. 9p-server-selftest.efi seeds a small tree inside the guest,
# exports it with axl_9p_server_new + axl_9p_server_listen, and pumps the loop;
# QEMU forwards a host port to it, and p9-client.py — a real 9P2000.L client on
# the HOST — drives the whole functional surface over the wire and asserts on
# every reply.
#
# Topology is the guest-listens / host-connects one from test-http.sh
# (test_add_port_forward), NOT the guest-connects-out one the 9P *client* tests
# (test-9p-qemu.sh) use. The assertions are the client's stdout, matched
# EXACTLY (grep -Fxq) — never a substring of the guest's own log, which would
# grade the server on its own testimony.
#
# Two connections are live at once: a `--linger` client that holds a read view,
# a write stream and a directory iterator open across the server's deadline, so
# axl_9p_server_free reaps a LIVE connection (SERVER: REAPING -> SERVER: DONE)
# rather than an already-drained one.
#
# Coverage, FUNCTIONAL: version/attach, getattr on a file and a directory
# (including the exact 153-byte Rgetattr body), read whole / at an offset / at
# EOF / past EOF, nested walk + a missing name (ENOENT), readdir with the
# synthetic "." and ".." plus a RESUMED readdir across several calls, write at
# 0 and at a non-zero offset with byte-exact read-back, mkdir,
# clunk-then-reuse (EBADF), remove a file, and remove of a non-empty directory
# (ENOTEMPTY).
#
# Coverage, ADVERSARIAL (the CASEnn lines, numbered after the accumulated case
# list in .superpowers/sdd/task-3-report.md): several requests in ONE TCP
# segment -- including [Tclunk][Tversion(131072)][Tattach][Tgetattr], the exact
# shape of the two heap use-after-frees this server shipped and fixed -- a
# frame over the negotiated msize and one under the header length (both must
# REAP, proven by the socket closing), msize refusal below the 512 floor,
# counts that lie about the frame, 64-bit read offsets and dirent cursors,
# far-offset writes that must answer EFBIG rather than zero-fill toward the FAT
# ceiling, a full fid table, the s9p_fid_restore_open rollback in all four
# shapes it can be asked to reconstruct, and the read-only export's EROFS gate
# driven for EVERY mutating message type.
#
# The read-only cases need a genuinely read-only server, so the guest publishes
# TWO exports over the same root: writable on GUEST_PORT, read_only=true on
# GUEST_PORT_RO. Both are forwarded on ONE netdev.
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1) — this is an integration
# test, not a unit count.
#
# Usage: ./test/integration/test-9p-server-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
HOST_PORT_RO=$(test_port 1)
GUEST_PORT=5640
GUEST_PORT_RO=5641
# The guest quits its loop this long after it starts listening; the harness
# must finish the client run well inside it, then wait for the teardown.
# MEASURED: the functional suite is ~2 s and the adversarial suite ~35 s
# (it writes and reads back ~530 KB, opens 60-odd connections, and holds two
# 1.5 s waits for a peer reap), so 90 s leaves better than 2x headroom on a
# loaded host. It is a BUDGET, not a hang mask: a case that stops getting
# answers fails on p9-client.py's own 8-20 s socket timeouts long before this.
SERVER_DEADLINE_MS=90000
LINGER_SECONDS=200

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all 9p-server-selftest 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/9p-server-selftest.efi"

# Interpolated heredoc (unquoted) so the port and deadline land in the script.
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

echo Starting 9P server...
9p-server-selftest.efi ${GUEST_PORT} ${SERVER_DEADLINE_MS} ${GUEST_PORT_RO}
NSHEOF

test_build_image

echo "=== Axl9pServer live-socket round-trip ($TEST_ARCH) ==="

test_build_qemu_cmd
# Two guest ports (writable + read-only export) on ONE netdev --
# test_add_port_forward emits a whole -device/-netdev pair, so calling it
# twice would collide on the netdev id. Same idiom as
# test-http-rebind-multi-qemu.sh.
TEST_QEMU_CMD+=(
    -device "$(_test_nic_device),netdev=net0"
    -netdev "user,id=net0,hostfwd=tcp::${HOST_PORT}-:${GUEST_PORT},hostfwd=tcp::${HOST_PORT_RO}-:${GUEST_PORT_RO}"
)
test_run_background

LINGER_PID=0
trap 'test_cleanup; [[ $LINGER_PID -gt 0 ]] && kill $LINGER_PID 2>/dev/null || true' EXIT INT TERM

echo "  QEMU PID: $TEST_QEMU_PID, host :$HOST_PORT -> guest :$GUEST_PORT" \
     "(rw), :$HOST_PORT_RO -> :$GUEST_PORT_RO (ro)"

if ! test_wait_for "SERVER: LISTENING port=$GUEST_PORT ro=$GUEST_PORT_RO" 90; then
    echo "FAIL: 9P server did not start listening within 90 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Server is listening ($(grep -o 'SERVER: ROOT=[^ ]* backing=[a-z0-9]*' "$TEST_LOG" | head -1))"

CLIENT_OUT="$TEST_TMPDIR/p9-client.out"
LINGER_OUT="$TEST_TMPDIR/p9-linger.out"

# Connection #1: holds open fids across the whole run and past the server's
# deadline, so the free path has a live connection to reap.
python3 "$(dirname "$0")/p9-client.py" --linger "$LINGER_SECONDS" \
    127.0.0.1 "$HOST_PORT" > "$LINGER_OUT" 2>&1 &
LINGER_PID=$!

# Budget note: this poll runs INSIDE the guest's SERVER_DEADLINE_MS window,
# which starts ticking at the listen. Capped at 15 s (a linger client that
# needs longer than that is broken, and times out red here) so the worst case
# is 15 s of poll + a ~25 s assertion suite against a 90 s deadline, leaving
# better than 45 s of headroom on a loaded host.
_linger_ready=0
for _ in $(seq 1 15); do
    if grep -Fxq "LINGER READY" "$LINGER_OUT" 2>/dev/null; then
        _linger_ready=1
        break
    fi
    sleep 1
done
if [[ $_linger_ready -eq 0 ]]; then
    echo "FAIL: the lingering 9P connection never came up"
    echo "--- linger output ---"; cat "$LINGER_OUT"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# Connection #2 onwards: the assertion suite -- functional, then adversarial.
# The adversarial half opens (and deliberately gets reaped on) many more
# connections of its own, and drives the read-only export on its own port.
CLIENT_RC=0
_client_start=$SECONDS
timeout 120 python3 "$(dirname "$0")/p9-client.py" 127.0.0.1 "$HOST_PORT" \
    --ro-port "$HOST_PORT_RO" > "$CLIENT_OUT" 2>&1 || CLIENT_RC=$?
echo "  client suite took $((SECONDS - _client_start))s of the" \
     "$((SERVER_DEADLINE_MS / 1000))s server deadline"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Exact whole-line match against the client's stdout. A substring match here
# would let a wrong value hide inside a right-looking line, which is the exact
# failure mode this project has been bitten by before.
assert_line() {
    if grep -Fxq "$1" "$CLIENT_OUT"; then
        pass "$1"
    else
        fail "$1  [$(grep -F "DIAG [$1]" "$CLIENT_OUT" | head -1)]"
    fi
}

# A line the client SHOULD print but currently cannot, because of a defect
# this test found in code outside its own scope. The assertion text is not
# weakened -- it stays exactly what correct behavior would print -- and this
# gate is symmetric: an XFAIL that starts PASSING fails the run too, so the
# defect cannot be fixed without someone coming back here and promoting the
# line into ASSERTIONS.
assert_xfail() {
    if grep -Fxq "$1" "$CLIENT_OUT"; then
        fail "XPASS (the known defect looks FIXED -- move this line into ADVERSARIAL and drop it from KNOWN_DEFECTS in p9-client.py): $1"
    elif grep -Fxq "XFAIL $1" "$CLIENT_OUT"; then
        pass "XFAIL (known defect, see the Task 6b report): $1  [$(grep -F "DIAG [$1]" "$CLIENT_OUT" | head -1)]"
    else
        fail "neither the line nor its XFAIL marker was printed (the group aborted before reaching it): $1"
    fi
}

echo ""
echo "  --- 9P wire round-trip (host client -> guest server) ---"

ASSERTIONS=(
    "VERSION msize=8192 version=9P2000.L"
    "ATTACH root isdir=1"
    "GETATTR body=153"
    "GETATTR hello.txt size=21"
    "GETATTR hello.txt isdir=0"
    "GETATTR sub isdir=1"
    "READ hello.txt exact = ok"
    "READ hello.txt = hello from 9p server"
    "READ offset = ok"
    "READ at-eof = 0"
    "READ past-eof = 0"
    "WALK sub/inner.txt = ok"
    "WALK sub/inner.txt read = ok"
    "WALK missing = ENOENT"
    "READDIR contains ."
    "READDIR contains .."
    "READDIR contains hello.txt"
    "READDIR contains sub"
    "READDIR no duplicates"
    "READDIR types = ok"
    "READDIR . offset=1"
    "READDIR .. offset=2"
    "READDIR resumed = ok"
    "READDIR chunked first reply = . .."
    "READDIR chunked round-trips >= 3"
    "WRITE count=11"
    "WRITE+READBACK ok"
    "WRITE offset count = ok"
    "WRITE offset = ok"
    "WRITE offset size=23"
    "MKDIR qid isdir=1"
    "MKDIR+READDIR ok"
    "CLUNK reuse = EBADF"
    "REMOVE file = ok"
    "REMOVE nonempty-dir = ENOTEMPTY"
)

# The adversarial half. Every line here is the client's verdict on bytes the
# server sent back; CASEnn refers to the accumulated case list, EXTRA to a
# case that list does not number.
ADVERSARIAL=(
    "CASE03 Tversion msize=0 = Rversion 0/unknown"
    "CASE03 Tversion msize=511 = Rversion 511/unknown"
    "CASE03 refused msize leaves the session at 8192 = ok"
    "CASE04 frame one byte over msize = connection closed"
    "EXTRA frame size below the 7-byte header = connection closed"
    "CASE04 server still serving after two reaped connections"
    "EXTRA one frame split across two segments = reassembled"
    "EXTRA unimplemented message type = EPROTO"
    "EXTRA truncated Tclunk body = EINVAL"
    "EXTRA string length past end of frame = EINVAL"
    "EXTRA server alive after three malformed messages"
    "CASE01 pipelined 1/4 = Rclunk tag=10"
    "CASE01 pipelined 2/4 = Rversion msize=131072 tag=11"
    "CASE01 pipelined 3/4 = Rattach isdir=1 tag=12"
    "CASE01 pipelined 4/4 = Rgetattr body=153 tag=13"
    "CASE01 max-msize Twrite accepted count=131049"
    "CASE01 max-msize write read back byte-exact"
    "CASE01 grown-msize Rread is exactly 131060 bytes on the wire"
    "CASE01 connection usable after the burst"
    "CASE02 431 requests in one 8189-byte segment, all answered in order"
    "CASE02 connection usable after the burst"
    "CASE05 partial walk = Rwalk with 1 of 2 qids"
    "CASE05 partial walk left newfid unbound = EBADF"
    "CASE05 clunk of the unbound newfid = EBADF"
    "EXTRA walk .. at the export root = ENOENT"
    "EXTRA walk a backslash-escaping name = ENOENT"
    "EXTRA walk a slash-bearing name = ENOENT"
    "EXTRA walk a NUL-bearing name = ENOENT"
    "EXTRA walk sub/../.. stops at the root = 2 qids"
    "EXTRA that escaping walk bound nothing = EBADF"
    "EXTRA Twalk onto an in-use newfid = EINVAL"
    "EXTRA Twalk newfid==fid with nwname>0 = EINVAL"
    "EXTRA Twalk from an unbound fid = EBADF"
    "CASE06 nwname over the msize cap (630) = EINVAL"
    "CASE06 connection alive after the over-cap walk"
    "CASE06 walk of 17 components in one Twalk = ok"
    "CASE06 the deep newfid is bound = ok"
    "EXTRA all 128 fids in a full table are live at once = ok"
    "EXTRA the 129th fid = ENOMEM"
    "EXTRA connection alive with a full fid table"
    "EXTRA a clunked table slot is reusable = ok"
    "CASE07 Tlopen file = qid type 0x00, iounit 0"
    "CASE07 Tlopen directory = qid type 0x80, iounit 0"
    "CASE08 Tlopen of an unbound fid = EBADF"
    "CASE08 Tlopen of a path deleted since the walk = ENOENT"
    "EXTRA Tlopen directory O_WRONLY = EISDIR"
    "CASE12 Tread at offset==size = 0 bytes"
    "CASE12 Tread past EOF = 0 bytes"
    "CASE12 Tread at offset 2^64-1 = 0 bytes"
    "CASE13 Tread count=2^32-1 clamped to 8181"
    "CASE13 the clamped Rread is exactly msize on the wire"
    "CASE11 multi-chunk read to EOF byte-exact = ok"
    "CASE14 Tread on a directory fid = EISDIR"
    "CASE14 Tread on a never-opened fid = EBADF"
    "CASE17 Treaddir on a file fid = ENOTDIR"
    "CASE17 Treaddir on a never-opened fid = EBADF"
    "CASE15 subdirectory listing has exactly one . and one .."
    "CASE15 synthetic . and .. carry cursors 1 and 2"
    "CASE15 real entries follow with strictly later cursors"
    "CASE18 Treaddir cursor past the end = empty"
    "CASE18 Treaddir cursor 2^64-1 = empty"
    "CASE18 Treaddir rewound to cursor 0 = the same listing"
    "CASE18 connection alive after made-up cursors"
    "CASE16 the paged listing took at least 4 round trips"
    "CASE16 every entry appears exactly once across the pages"
    "CASE16 no entry repeated across pages"
    "CASE19 one page taken before the renegotiation"
    "CASE19 Tversion mid-readdir = Rversion 8192/9P2000.L"
    "CASE19 the open directory fid is gone = EBADF"
    "CASE19 session usable after the reset"
    "CASE25 Twrite count overstated by 1 = EINVAL"
    "CASE25 Twrite count 0xFFFFFFFF = EINVAL"
    "CASE25 the lying writes changed nothing (size=64)"
    "CASE25 connection usable after a lying count"
    "CASE40 Twrite at offset 0xFFFFFFFE = EFBIG"
    "CASE40 the EFBIG refusal came back promptly (< 2s)"
    "CASE40 the refused far write grew nothing"
    "CASE40 Twrite ending one byte past MAX_GROW = EFBIG"
    "CASE40 a bounded grow past EOF still writes = 4"
    "CASE40 the bounded grow left size=4164"
    "CASE26 out-of-order positional writes reassemble = ok"
    "CASE28 write past EOF read back through the same fid = ok"
    "CASE27 Twrite on an O_RDONLY fid = EBADF"
    "CASE27 Twrite on a directory fid = EISDIR"
    "CASE27 Twrite on a never-opened fid = EBADF"
    "CASE27 Tread on an O_WRONLY fid = EBADF"
    "CASE30 O_TRUNC left no tail of the old content = ok"
    "CASE30 the truncated file is 5 bytes"
    "CASE37 Tfsync on a write fid = Rfsync"
    "CASE37 Tfsync on a read fid = Rfsync"
    "CASE37 Tfsync on a directory fid = Rfsync"
    "CASE37 Tfsync on an unbound fid = EBADF"
    "CASE37 Tfsync on a bound but never-opened fid = EBADF"
    "CASE22 Tlcreate = qid type 0x00, iounit 0"
    "CASE22 Twrite on the rebound dfid lands in the new file = ok"
    "CASE22 the rebound fid no longer names the directory = ENOTDIR"
    "CASE23 Tlcreate onto an existing name = EEXIST"
    "CASE23 Tlcreate with a slash in the name = EINVAL"
    "CASE23 Tlcreate with a backslash in the name = EINVAL"
    "CASE23 Tlcreate named .. = EINVAL"
    "CASE23 Tlcreate named . = EINVAL"
    "CASE23 Tlcreate with dfid naming a file = ENOTDIR"
    "CASE31 Tmkdir = qid type 0x80"
    "CASE31 Tmkdir onto an existing name = EEXIST"
    "CASE31 the new directory is in the listing"
    "CASE32 Tremove clunked the fid = EBADF"
    "CASE32 the removed file is gone = ok"
    "CASE32 Tremove of an empty directory = ok"
    "CASE32 Tremove of a non-empty directory = ENOTEMPTY"
    "CASE32 the refused Tremove still clunked the fid = EBADF"
    "CASE32 Tremove of the export root = EPERM"
    "CASE32 the refused root Tremove still clunked = EBADF"
    "CASE33 Tremove of a file this fid had open for writing = ok"
    "CASE33 connection alive after the removes"
    "CASE34 same-directory Trename = the fid names the new path"
    "CASE34 the rename moved the name = ok"
    "CASE34 the renamed file kept its content = ok"
    "CASE34 Trename onto itself = Rrename, no-op"
    "CASE34 Trename onto an existing name = EEXIST"
    "CASE34 Trename of the export root = EPERM"
    "CASE42 cross-directory Trename = EXDEV"
    "CASE42 the EXDEV rename moved nothing"
    "CASE42 the fid still names the source after EXDEV"
    "CASE42 the client's copy+unlink fallback then works = ok"
    "CASE42 connection alive after the renames"
    "CASE35 an already-open fid sees the shrunk file = ok"
    "CASE35 the grown region reads back as 2048 zeros"
    "CASE41 Tsetattr one byte past MAX_GROW = EFBIG"
    "CASE41 Tsetattr size=2^64-1 = EFBIG"
    "CASE41 the refused grows left size=2148"
    "CASE35 Tsetattr(SIZE) on a directory = EISDIR"
    "CASE36 Tsetattr with valid=0 and with no-op bits changed nothing"
    "CASE36 Tsetattr on an unbound fid = EBADF"
    "CASE36 connection alive after the setattrs"
    "CASE29 reader fid sees the original content"
    "CASE29 the other fid on the same connection sees the write"
    "CASE29 a reader on connection A sees connection B's write"
    "CASE20 12 abrupt disconnects, every one served = ok"
    "CASE20 connection slots were all recycled"
    "CASE21 20 open file fids over 16 frames, 3 passes = ok"
    "CASE21 connection alive after the round-robin"
    "CASE11 negotiated msize=131072 = ok"
    "CASE24 multi-chunk write at msize=131072 = 200000 bytes"
    "CASE11 read at msize=131072 byte-exact = ok"
    "CASE11 the same file read at msize=8192 byte-exact = ok"
    "CASE38 Tlcreate on a read-only export = EROFS"
    "CASE38 Tmkdir on a read-only export = EROFS"
    "CASE38 Trename on a read-only export = EROFS"
    "CASE38 Tsetattr on a read-only export = EROFS"
    "CASE38 Twrite on a read-only export = EROFS"
    "CASE38 Tremove on a read-only export = EROFS"
    "CASE38 the refused Tremove did NOT clunk the fid = ok"
    "CASE39 Tmkdir under an UNBOUND dfid = EROFS"
    "CASE39 Tlcreate with an invalid name = EROFS"
    "CASE39 Twrite on an unbound fid = EROFS"
    "CASE38 Tlopen(O_RDONLY) + Tread still work read-only = ok"
    "CASE38 Tfsync still works read-only = Rfsync"
    "CASE38 Tlopen(O_WRONLY) on a read-only export = EROFS"
    "CASE38 Twalk + Treaddir still work read-only = ok"
    "CASE38 Tversion still works read-only = ok"
    "CASE38 Tattach still works read-only = ok"
    "CASE38 read-only export still serving"
    "CASE43 [O_RDONLY] connection A holds the file open = ok"
    "CASE43 [O_RDONLY] Trename with another connection's handle live = Rrename"
    "CASE43 [O_RDONLY] the rename moved the file"
    "CASE43 [O_RDONLY] the renamed fid is bound but closed = EBADF"
    "CASE43 [O_RDONLY] the renamed fid works again after Tlopen"
    "CASE43 [O_RDONLY] connection A is told EIO once its file moved"
    "CASE43 [O_RDONLY] both connections alive after the rename"
    "CASE43 [O_WRONLY] connection A holds the file open = ok"
    "CASE43 [O_WRONLY] Trename with another connection's handle live = Rrename"
    "CASE43 [O_WRONLY] the rename moved the file"
    "CASE43 [O_WRONLY] the renamed fid is bound but closed = EBADF"
    "CASE43 [O_WRONLY] the renamed fid works again after Tlopen"
    "CASE43 [O_WRONLY] connection A is told EIO once its file moved"
    "CASE43 [O_WRONLY] both connections alive after the rename"
    "CASE43 [rd] Trename the backing store refuses = EIO"
    "CASE43 [rd] the failed rename left the tree alone"
    "CASE43 [rd] the rolled-back fid still reads"
    "CASE43 [rd] connection alive after the rollback"
    "CASE43 [wr] Trename the backing store refuses = EIO"
    "CASE43 [wr] the failed rename left the tree alone"
    "CASE43 [wr] the rolled-back fid still writes"
    "CASE43 [wr] connection alive after the rollback"
    "CASE43 [rdwr] Trename the backing store refuses = EIO"
    "CASE43 [rdwr] the failed rename left the tree alone"
    "CASE43 [rdwr] the rolled-back fid still reads"
    "CASE43 [rdwr] the rolled-back fid still writes"
    "CASE43 [rdwr] connection alive after the rollback"
    "CASE43 [dir] Trename the backing store refuses = EIO"
    "CASE43 [dir] the failed rename left the tree alone"
    "CASE43 [dir] the rolled-back directory fid still lists"
    "CASE43 [dir] connection alive after the rollback"
    "CASE44 the ro export reads the file the rw export wrote"
    "CASE44 the ALREADY-OPEN ro fid sees the rw export's write"
    "CASE44 the ro fid tracks a SECOND write, and the shrink with it"
    "CASE44 a read through the ro fid after the remove = EIO"
    "CASE44 the NEXT read gives the same EIO, not EBADF"
    "CASE44 the ro export is still serving"
    "CASE44 the rw export is still serving"
)

# Assertions the server cannot currently satisfy because of a defect this test
# found and did not fix. Root cause and rationale live next to KNOWN_DEFECTS
# in p9-client.py -- ONE description, and the client is what has to recognize
# the line. Empty: the one entry this suite carried (Tfsync on a write fid,
# blocked on axl_fflush always failing for file streams) is fixed, and the
# line is now an ordinary ADVERSARIAL assertion. The machinery stays for the
# next find.
KNOWN_DEFECTS=()

for _line in "${ASSERTIONS[@]}" "${ADVERSARIAL[@]}"; do
    assert_line "$_line"
done
for _line in "${KNOWN_DEFECTS[@]}"; do
    assert_xfail "$_line"
done

# The client derives this count from its own assertion tally, and the harness
# derives it from the array lengths -- so an assertion added on one side and
# not the other fails the run rather than passing quietly.
assert_line "CLIENT OK $(( ${#ASSERTIONS[@]} + ${#ADVERSARIAL[@]} )) (xfail ${#KNOWN_DEFECTS[@]})"

[[ $CLIENT_RC -eq 0 ]] && pass "p9-client.py exited 0" \
                       || fail "p9-client.py exit status $CLIENT_RC"

# --- teardown: axl_9p_server_free with a LIVE connection still attached ----

echo ""
echo "  --- teardown (free with a live connection) ---"

if test_wait_for "SERVER: DONE" 90; then
    pass "axl_9p_server_free returned with a live connection attached (SERVER: DONE)"
else
    fail "server never reached SERVER: DONE (free hung or faulted)"
fi

test_clean_log
grep -q "SERVER: REAPING" "$TEST_CLEAN_LOG" \
    && pass "deadline fired and the loop exited (SERVER: REAPING)" \
    || fail "server loop never exited"

echo ""
printf "9P server round-trip: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- what the client could not assert ---"
    grep -E "^(MISSING|DIAG|GROUP-ABORT|CLIENT) " "$CLIENT_OUT" || true
    echo ""; echo "--- client output ---"; cat "$CLIENT_OUT"
    echo ""; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
fi

# Derived, not a literal: one check per ASSERTIONS/ADVERSARIAL/KNOWN_DEFECTS
# entry plus the four the harness makes itself (the client's own pass count,
# its exit status, and the two teardown markers). Adding an assertion must not
# fail the run for the wrong reason.
EXPECTED_PASS=$(( ${#ASSERTIONS[@]} + ${#ADVERSARIAL[@]} + ${#KNOWN_DEFECTS[@]} + 4 ))
[[ $FAIL -eq 0 && $PASS -eq $EXPECTED_PASS ]] && exit 0 || exit 1
