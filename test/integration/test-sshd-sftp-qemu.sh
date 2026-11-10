#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,sftp,scp est=90 local-only=0 builds=sshd,sshd-full,sftp-server,svc-startfail guest=none
# guest=none: it types into the guest Shell (test_send_stdin) and reads its
#   boot disk back with mtools.
# test-sshd-sftp-qemu.sh -- sshd's `sftp` subsystem as the host's
# own OpenSSH clients see it: sftp(1) in batch mode and scp(1), which speaks
# SFTP since OpenSSH 9.0.
#
# The guest boots with sshd.efi, SftpServer.efi and the shipped sshd.conf
# (its `Subsystem sftp SftpServer.efi` uncommented) at the root of FS0, and
# a tree: sftptest\hello.txt (a known line), sftptest\big.bin (1 MiB
# of random bytes), sftptest\ro.txt (given the read-only attribute by the
# startup script) and sftptest\sub\inner.txt; a RAM disk, fs20, holds an
# empty sftptest directory. The host types `sshd` into the Shell over the
# serial console and then runs the clients against it.
#
# Scenarios, in run order:
#   load         sshd loads the helper as it starts, before its serving
#                line -- never from a session (see the load check)
#   ls-root      `ls -1 /`: the virtual root lists the mapped volumes,
#                exactly
#   ls-dir       `ls -1 /fs0/sftptest`: exactly its four entries
#   get          `get` of hello.txt and of the 1 MiB file: SHA-256 equal to
#                the staged files'
#   scp          `scp host:/fs0/...`, and the Shell form `host:FS0:...` (scp
#                splits at the first colon) with `/` and with `\\`: the
#                same bytes. A single `\` cannot reach the server: scp(1)
#                runs the remote path through glob(3), which takes `\` as
#                an escape (`FS0:\a\b` arrives as `FS0:ab`), so the Shell
#                form needs `/` or a doubled `\\`
#   sftp-shell   sftp(1) with the Shell form, as the guide gives it: from
#                `/`, `FS0:/...` and `FS0:\\...` (each `\` doubled -- sftp
#                drops a single one) fetch the same bytes; `FS0:\...` asks
#                for another name
#   cd-root      `cd /`, `cd ..`, `pwd`: `..` never climbs above /
#   missing      `get` of a file that is not there: sftp(1)'s exact error
#   write        `put` (a line, and 1 MiB), `rename`, `mkdir`, `rm`,
#                `rmdir`, then `get` of the uploads: the same bytes, and
#                `ls` shows exactly what is left
#   put-past-window  a 6 MiB `put`, about three SSH windows: the channel runs
#                in ON_CONSUME, so it finishes only if the session's
#                per-request reports reopen the window; the bytes come back
#   write-refused  `put`, `rm` and `rename` of a file with the read-only
#                attribute, a `rename` across volumes, and `rmdir` of a
#                non-empty directory: sftp(1)'s exact errors
#   extensions   raw requests over `ssh -s` (sftp-raw.py): VERSION
#                advertises posix-rename@openssh.com and limits@openssh.com,
#                and limits reports this server's four numbers, exactly
#   posix-rename sftp(1)'s `rename` onto an existing file -- which it sends
#                as posix-rename once advertised -- replaces it: the bytes
#                come back, and nothing set aside is left; onto the file's
#                own 8.3 alias it changes nothing, and a case-only rename
#                takes the new spelling; `rename -l` (the
#                original request) never replaces; onto a file on
#                another volume it is Failure and both files are kept
#   grow         a size or a WRITE gap growing a file by more than 16 MiB
#                (SFTP_GROW_MAX) is Failure; exactly 16 MiB is done; two
#                pipelined growths are both answered, the second after the
#                server yields to the loop and is resumed
#   cancel-on-close  ssh-peer.py sends INIT + a growing SETSTAT and the
#                channel CLOSE in one write: the session yields, the close
#                cancels its pending resume (debug lines, in order), no
#                resume runs, and the next session works with no fault
#   scp-up       `scp` up to host:/fs0/..., host:'FS0:/...' and
#                host:'FS0:\\...': each comes back byte for byte
#   unload       sshd logs one `SFTP helper loaded` per start, before it
#                serves, and one `SFTP helper unloaded` per stop: no
#                session loads or unloads it
#   sftp-read-only  AxlSftpReadOnly yes, --sftp-read-only and -R: a get
#                works; a put and an rm are refused with sftp(1)'s exact
#                Permission denied lines; VERSION leaves posix-rename out
#                (as sftp-server -R does) and a posix-rename sent anyway is
#                Permission denied; nothing changes
#   no-helper    SftpServer.efi renamed away before sshd starts: sftp(1)
#                prints sshd's exact stderr line and the channel exits 127
#   disabled     `sshd -f nosftp.conf` (no Subsystem line): sshd logs ONE
#                line saying SFTP is off and why, and the subsystem request
#                is refused, in ssh(1)'s exact words
#   bad-helper   a helper that is not an image (garbage.efi) and one whose
#                entry point fails (svc_startfail-dxe.efi): each is
#                reported on stderr and the channel exits; sshd serves on
#   full         sshd-full.efi with no helper file anywhere: `get` works
#                from the embedded copy, loaded as sshd-full started
#   stop-open    `sshd stop` with an sftp(1) session open on sshd-full: the
#                connection drops, the helper is unloaded, sshd stops
#
# Usage: ./test/integration/test-sshd-sftp-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"

sshd_require_tools ssh ssh-keygen sftp scp python3
test_parse_args "$@"
test_setup

sshd_build
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_NATIVE[$TEST_ARCH]}" \
        sftp-server sshd-full tools \
        > "$TEST_TMPDIR/make-sftp.log" 2>&1; then
    echo "FAIL: SftpServer.efi / sshd-full.efi did not build"
    tail -20 "$TEST_TMPDIR/make-sftp.log"
    exit 1
fi
test_add_efi "$TEST_BUILD_DIR/tools/SftpServer.efi"
test_add_efi "$TEST_BUILD_DIR/tools/sshd-full.efi"
test_add_efi "$TEST_BUILD_DIR/tools/mkrd.efi"
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_NATIVE[$TEST_ARCH]}" \
        svc-startfail \
        > "$TEST_TMPDIR/make-startfail.log" 2>&1; then
    echo "FAIL: svc_startfail-dxe.efi did not build"
    tail -20 "$TEST_TMPDIR/make-startfail.log"
    exit 1
fi
# A driver whose entry point fails, as a helper that cannot start; and a
# file that is no image at all, as one that cannot load.
test_add_efi "$TEST_BUILD_DIR/svc_startfail-dxe.efi" badstart.efi
head -c 4096 /dev/urandom > "$TEST_STAGING/garbage.efi"
printf '%s\r\n' 'Subsystem sftp badstart.efi' > "$TEST_STAGING/badstart.conf"
printf '%s\r\n' 'Subsystem sftp garbage.efi' > "$TEST_STAGING/garbage.conf"
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"
cp "$PROJECT_DIR/tools/sshd.conf" "$TEST_STAGING/sshd.conf"
printf '%s\r\n' '# no Subsystem line: SFTP off, as in OpenSSH' 'Port 22' \
    > "$TEST_STAGING/nosftp.conf"
printf '%s\r\n' 'Subsystem sftp SftpServer.efi' 'AxlSftpReadOnly yes' \
    > "$TEST_STAGING/sftpro.conf"
mkdir -p "$TEST_STAGING/sftptest/sub"
printf 'hello over sftp\n' > "$TEST_STAGING/sftptest/hello.txt"
head -c 1048576 /dev/urandom > "$TEST_STAGING/sftptest/big.bin"
printf 'inner\n' > "$TEST_STAGING/sftptest/sub/inner.txt"
# Given the read-only attribute in the guest (attrib +r) before sshd starts.
printf 'locked\n' > "$TEST_STAGING/sftptest/ro.txt"
SUM_HELLO=$(sha256sum < "$TEST_STAGING/sftptest/hello.txt" | awk '{print $1}')
SUM_BIG=$(sha256sum < "$TEST_STAGING/sftptest/big.bin" | awk '{print $1}')

sum_of() { sha256sum < "$1" 2>/dev/null | awk '{print $1}'; }

DL="$TEST_TMPDIR/dl"
mkdir -p "$DL"
# What the write scenarios upload: a line, and 1 MiB of random bytes.
printf 'uploaded over sftp\n' > "$DL/up.txt"
head -c 1048576 /dev/urandom > "$DL/big-up.bin"
SUM_UP=$(sum_of "$DL/up.txt")
SUM_BIG_UP=$(sum_of "$DL/big-up.bin")

# The client options every run here shares: the run's key, no host-key
# store, no agent, never a prompt -- and LogLevel ERROR, so stderr holds
# only the errors under test, not the host-key warning or sshd's login
# banner (ssh(1) prints a banner at INFO).
CLIENT_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
             -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes
             -o IdentityAgent=none -o LogLevel=ERROR -i "$TEST_TMPDIR/key")

# sftp_batch NAME -- run sftp -b on the commands read from stdin; its stdout
# in NAME.out, stderr in NAME.err, exit status in SFTP_RC. Run in $DL so a
# `get` lands there.
sftp_batch() {
    local name="$1"
    cat > "$TEST_TMPDIR/$name.batch"
    SFTP_RC=0
    (cd "$DL" && timeout 120 sftp -b "$TEST_TMPDIR/$name.batch" -P "$HOST_PORT" \
        "${CLIENT_OPTS[@]}" anyone@127.0.0.1) \
        > "$TEST_TMPDIR/$name.out" 2> "$TEST_TMPDIR/$name.err" < /dev/null || SFTP_RC=$?
    DUMP_SFTP+=("$TEST_TMPDIR/$name.out" "$TEST_TMPDIR/$name.err")
}

# out_is NAME EXPECTED -- NAME.out is exactly EXPECTED (CRs dropped).
out_is() {
    local got
    got=$(tr -d '\r' < "$TEST_TMPDIR/$1.out")
    [[ "$got" == "$2" ]] && return 0
    diff <(printf '%s\n' "$2") <(printf '%s\n' "$got") | sed 's/^/    | /'
    return 1
}
# err_is NAME EXPECTED -- NAME.err is exactly EXPECTED (CRs dropped).
err_is() {
    local got
    got=$(tr -d '\r' < "$TEST_TMPDIR/$1.err")
    [[ "$got" == "$2" ]] && return 0
    diff <(printf '%s\n' "$2") <(printf '%s\n' "$got") | sed 's/^/    | /'
    return 1
}

DUMP_SFTP=()
# The password the cancel-on-close scenario's ssh-peer.py logs in with
# (`sshd -P`): it speaks only password authentication.
PW=sftp-cancel-pw

# sftp_raw NAME -- the requests read from stdin, sent over one `ssh -s ...
# sftp` channel by sftp-raw.py; its reply lines in NAME.out, its exit
# status in RAW_RC.
SFTP_RAW="$(dirname "$0")/sftp-raw.py"
sftp_raw() {
    local name="$1"
    cat > "$TEST_TMPDIR/$name.raw"
    RAW_RC=0
    timeout 120 python3 -I "$SFTP_RAW" -- ssh -s -p "$HOST_PORT" "${CLIENT_OPTS[@]}" \
        anyone@127.0.0.1 sftp < "$TEST_TMPDIR/$name.raw" \
        > "$TEST_TMPDIR/$name.out" 2> "$TEST_TMPDIR/$name.err" || RAW_RC=$?
    DUMP_SFTP+=("$TEST_TMPDIR/$name.out" "$TEST_TMPDIR/$name.err")
}

# The driver's log lines on the console's own serial port (--log-port 0).
SFTP_PFX='(^|[0-9:.]+ )\[INFO\]  sftp: '
# helper_seq -- the helper's life in the guest log, in order: L for each
# `SFTP helper loaded`, U for each `SFTP helper unloaded`, S for each start's
# serving line.
helper_seq() {
    test_clean_log
    grep -aE -- "${SFTP_PFX}SFTP helper (loaded from .*|unloaded)\$|${LOG_PFX}sshd: serving the UEFI Shell on .*\$" \
            "$TEST_CLEAN_LOG" \
        | sed -E 's/.*SFTP helper loaded from .*/L/; s/.*SFTP helper unloaded$/U/; s/.*sshd: serving the UEFI Shell on .*/S/' \
        | tr -d '\n'
}
# seq_is EXPECTED -- helper_seq is exactly EXPECTED, within 10 s (a stop's
# unload line can trail its `sshd: stopped`).
seq_is() {
    local i got
    for ((i = 0; i < 50; i++)); do
        got=$(helper_seq)
        [[ "$got" == "$1" ]] && return 0
        sleep 0.2
    done
    echo "    | expected $1"
    echo "    | got      $got"
    return 1
}

# A second volume, with an sftptest directory: what a rename across volumes
# targets. A RAM disk made by mkrd, which names it FS20 or above without
# re-mapping the rest: a second disk would be mapped by `connect -r` in an
# order the firmwares differ on (aa64 put it before the boot disk, which
# every path here calls fs0).

# What `ls -1 /` lists: the volumes this guest maps.
SFTP_ROOT_LIST=('/fs0' '/fs20')

# start_sshd COMMAND -- type it and wait for the serving line.
SERVES=0
start_sshd() {
    SERVES=$((SERVES + 1))
    send "$1 $SSHD_LOG_HERE\\r"
    if ! wait_guest_re "${LOG_PFX}sshd: serving the UEFI Shell on ${GUEST_IP//./\\.} port $GUEST_PORT \\(console [0-9]+x[0-9]+\\)( read-only)?\$" "$SERVES" 60; then
        echo "FAIL: '$1' did not start serving"
        test_clean_log; tail -30 "$TEST_CLEAN_LOG"
        exit 1
    fi
}
STOPS=0
stop_sshd() {
    STOPS=$((STOPS + 1))
    send 'sshd stop\r'
    check "sshd stop ($STOPS)" wait_guest "sshd: stopped" "$STOPS" 30
}

echo "=== sshd sftp ($TEST_ARCH) ==="
sshd_boot "$(printf '%s\n' 'attrib +r fs0:\sftptest\ro.txt' 'mkrd XVOL' 'mkdir fs20:\sftptest')"
start_sshd 'sshd'

# --- load ----------------------------------------------------------------------
# The helper is loaded as sshd starts -- from the driver's entry point, at
# TPL_APPLICATION -- and never from a session. A session runs in an event
# callback, which the firmware can dispatch from inside the Shell's
# FreePages of an exiting application: between the memory map giving the
# pages back and the NX policy being applied to them. A helper loaded in that
# window lands in those pages, and the policy then marks its code RW + XN --
# the aarch64 instruction abort seen in loaded runs (2026-10-24).
check "load: sshd loads the helper as it starts, before it serves" seq_is LS

# --- ls-root, ls-dir ------------------------------------------------------------
sftp_batch ls <<'EOF'
ls -1 /
ls -1 /fs0/sftptest
EOF
check "ls: sftp exits 0 ($SFTP_RC)" test "$SFTP_RC" = 0
check "ls-root + ls-dir: / lists the volumes, /fs0/sftptest its four entries, exactly" \
    out_is ls "$(printf '%s\n' 'sftp> ls -1 /' "${SFTP_ROOT_LIST[@]}" \
        'sftp> ls -1 /fs0/sftptest' '/fs0/sftptest/big.bin' '/fs0/sftptest/hello.txt' \
        '/fs0/sftptest/ro.txt' '/fs0/sftptest/sub')"
check "ls: nothing on stderr" err_is ls ""

# --- get --------------------------------------------------------------------------
t0=$(date +%s%N)
sftp_batch get <<'EOF'
get /fs0/sftptest/hello.txt
get /fs0/sftptest/big.bin
EOF
t1=$(date +%s%N)
check "get: sftp exits 0 ($SFTP_RC)" test "$SFTP_RC" = 0
check "get: hello.txt arrives byte for byte" test "$(sum_of "$DL/hello.txt")" = "$SUM_HELLO"
check "get: the 1 MiB file arrives byte for byte" test "$(sum_of "$DL/big.bin")" = "$SUM_BIG"
echo "  get of 1 MiB + 16 B: $(( (t1 - t0) / 1000000 )) ms"
rm -f "$DL/hello.txt" "$DL/big.bin"

# --- scp ----------------------------------------------------------------------------
scp_rc=0
timeout 120 scp -P "$HOST_PORT" "${CLIENT_OPTS[@]}" \
    anyone@127.0.0.1:/fs0/sftptest/hello.txt "$DL/scp-posix.txt" \
    > "$TEST_TMPDIR/scp1.out" 2> "$TEST_TMPDIR/scp1.err" < /dev/null || scp_rc=$?
check "scp host:/fs0/sftptest/hello.txt: exit 0 ($scp_rc), the same bytes" \
    test "$scp_rc" = 0 -a "$(sum_of "$DL/scp-posix.txt")" = "$SUM_HELLO"
scp_rc=0
timeout 120 scp -P "$HOST_PORT" "${CLIENT_OPTS[@]}" \
    'anyone@127.0.0.1:FS0:/sftptest/big.bin' "$DL/scp-shell.bin" \
    > "$TEST_TMPDIR/scp2.out" 2> "$TEST_TMPDIR/scp2.err" < /dev/null || scp_rc=$?
check "scp host:'FS0:/sftptest/big.bin' (the Shell form): exit 0 ($scp_rc), the same 1 MiB" \
    test "$scp_rc" = 0 -a "$(sum_of "$DL/scp-shell.bin")" = "$SUM_BIG"
scp_rc=0
timeout 120 scp -P "$HOST_PORT" "${CLIENT_OPTS[@]}" \
    'anyone@127.0.0.1:FS0:\\sftptest\\hello.txt' "$DL/scp-bs.txt" \
    > "$TEST_TMPDIR/scp3.out" 2> "$TEST_TMPDIR/scp3.err" < /dev/null || scp_rc=$?
check "scp host:'FS0:\\\\sftptest\\\\hello.txt' (backslashes, escaped for glob): exit 0 ($scp_rc), the same bytes" \
    test "$scp_rc" = 0 -a "$(sum_of "$DL/scp-bs.txt")" = "$SUM_HELLO"

# --- sftp-shell ---------------------------------------------------------------------
# The guide's sftp(1) examples. A session starts in /, where a path that does
# not begin with / is joined to it; sftp unescapes the command line and get
# globs the remote path, so a single backslash is lost and must be doubled.
rm -f "$DL/sf-slash.txt" "$DL/sf-bs.txt"
sftp_batch sftp-shell <<'EOF'
get FS0:/sftptest/hello.txt sf-slash.txt
get FS0:\\sftptest\\hello.txt sf-bs.txt
EOF
check "sftp-shell: from /, FS0:/... and FS0:\\\\... both fetch hello.txt ($SFTP_RC)" \
    test "$SFTP_RC" = 0 -a "$(sum_of "$DL/sf-slash.txt")" = "$SUM_HELLO" \
        -a "$(sum_of "$DL/sf-bs.txt")" = "$SUM_HELLO"
sftp_batch sftp-shell-single <<'EOF'
get FS0:\sftptest\hello.txt
EOF
check "sftp-shell: a single backslash never reaches the server, in sftp(1)'s words ($SFTP_RC)" \
    err_is sftp-shell-single 'File "/FS0:sftptesthello.txt" not found.'

# --- cd-root -------------------------------------------------------------------------
sftp_batch cd <<'EOF'
cd /
cd ..
pwd
cd /fs0/sftptest/sub
cd ../../..
pwd
EOF
check "cd-root: .. at / stays at /, and climbing past the volume lands at / (rc $SFTP_RC)" \
    out_is cd "$(printf '%s\n' 'sftp> cd /' 'sftp> cd ..' 'sftp> pwd' \
        'Remote working directory: /' 'sftp> cd /fs0/sftptest/sub' 'sftp> cd ../../..' \
        'sftp> pwd' 'Remote working directory: /')"

# --- missing ------------------------------------------------------------------------
sftp_batch missing <<'EOF'
get /fs0/sftptest/nope.txt
EOF
check "missing: sftp exits 1 ($SFTP_RC)" test "$SFTP_RC" = 1
check "missing: sftp(1)'s exact error for a file that is not there" \
    err_is missing 'File "/fs0/sftptest/nope.txt" not found.'

# --- write ------------------------------------------------------------------------------
sftp_batch write <<'EOF'
put up.txt /fs0/sftptest/up.txt
rename /fs0/sftptest/up.txt /fs0/sftptest/renamed.txt
mkdir /fs0/sftptest/newdir
put up.txt /fs0/sftptest/newdir/inside.txt
rm /fs0/sftptest/newdir/inside.txt
rmdir /fs0/sftptest/newdir
put big-up.bin /fs0/sftptest/big-up.bin
get /fs0/sftptest/big-up.bin big-back.bin
get /fs0/sftptest/renamed.txt renamed-back.txt
rm /fs0/sftptest/renamed.txt
ls -1 /fs0/sftptest
EOF
check "write: sftp exits 0 ($SFTP_RC)" test "$SFTP_RC" = 0
check "write: put, rename, mkdir, rm, rmdir leave exactly the uploaded 1 MiB file new" \
    out_is write "$(printf '%s\n' \
        'sftp> put up.txt /fs0/sftptest/up.txt' \
        'sftp> rename /fs0/sftptest/up.txt /fs0/sftptest/renamed.txt' \
        'sftp> mkdir /fs0/sftptest/newdir' \
        'sftp> put up.txt /fs0/sftptest/newdir/inside.txt' \
        'sftp> rm /fs0/sftptest/newdir/inside.txt' \
        'sftp> rmdir /fs0/sftptest/newdir' \
        'sftp> put big-up.bin /fs0/sftptest/big-up.bin' \
        'sftp> get /fs0/sftptest/big-up.bin big-back.bin' \
        'sftp> get /fs0/sftptest/renamed.txt renamed-back.txt' \
        'sftp> rm /fs0/sftptest/renamed.txt' \
        'sftp> ls -1 /fs0/sftptest' '/fs0/sftptest/big-up.bin' '/fs0/sftptest/big.bin' \
        '/fs0/sftptest/hello.txt' '/fs0/sftptest/ro.txt' '/fs0/sftptest/sub')"
check "write: nothing on stderr" err_is write ""
check "write: the 1 MiB upload comes back byte for byte" \
    test "$(sum_of "$DL/big-back.bin")" = "$SUM_BIG_UP"
check "write: the renamed upload comes back byte for byte" \
    test "$(sum_of "$DL/renamed-back.txt")" = "$SUM_UP"

# --- put-past-window ----------------------------------------------------------------------
# The sftp channel runs in AXL_SSH_CHANNEL_WINDOW_ON_CONSUME: the client's
# window reopens only as the session reports each request it dispatched. A
# 6 MiB put is about three windows (2 MB each) of WRITE requests with sftp's
# 64 in flight, so it completes only if those reports reach the wire -- a
# session that stopped reporting would hold its client after the first 2 MB
# and the put would hang, not finish. It comes back byte for byte.
head -c 6291456 /dev/urandom > "$DL/win-up.bin"
SUM_WIN_UP=$(sum_of "$DL/win-up.bin")
sftp_batch put-past-window <<'EOF'
put win-up.bin /fs0/sftptest/win-up.bin
get /fs0/sftptest/win-up.bin win-back.bin
rm /fs0/sftptest/win-up.bin
EOF
check "put-past-window: sftp exits 0 ($SFTP_RC)" test "$SFTP_RC" = 0
check "put-past-window: a 6 MiB put, three windows past the first, comes back byte for byte" \
    test "$(sum_of "$DL/win-back.bin")" = "$SUM_WIN_UP"
check "put-past-window: nothing on stderr" err_is put-past-window ""
rm -f "$DL/win-up.bin" "$DL/win-back.bin"

# --- move ---------------------------------------------------------------------------------
# rename between directories of one volume, there and back.
sftp_batch move <<'EOF'
rename /fs0/sftptest/big-up.bin /fs0/sftptest/sub/big-up.bin
ls -1 /fs0/sftptest/sub
rename /fs0/sftptest/sub/big-up.bin /fs0/sftptest/big-up.bin
ls -1 /fs0/sftptest/sub
get /fs0/sftptest/big-up.bin moved-back.bin
EOF
check "move: sftp exits 0 ($SFTP_RC), nothing on stderr" \
    test "$SFTP_RC" = 0 -a ! -s "$TEST_TMPDIR/move.err"
check "move: rename into sub/ and back, exactly" \
    out_is move "$(printf '%s\n' \
        'sftp> rename /fs0/sftptest/big-up.bin /fs0/sftptest/sub/big-up.bin' \
        'sftp> ls -1 /fs0/sftptest/sub' '/fs0/sftptest/sub/big-up.bin' \
        '/fs0/sftptest/sub/inner.txt' \
        'sftp> rename /fs0/sftptest/sub/big-up.bin /fs0/sftptest/big-up.bin' \
        'sftp> ls -1 /fs0/sftptest/sub' '/fs0/sftptest/sub/inner.txt' \
        'sftp> get /fs0/sftptest/big-up.bin moved-back.bin')"
check "move: the 1 MiB file is the same after two moves" \
    test "$(sum_of "$DL/moved-back.bin")" = "$SUM_BIG_UP"

# --- times --------------------------------------------------------------------------------
# put -p (write, fsetstat, close) and scp -p keep the file's time; get -p
# brings it back, so the host sees what the guest stored. 1000000000 is
# even: FAT keeps it exactly.
printf 'stamped\n' > "$DL/stamped.txt"
touch -d @1000000000 "$DL/stamped.txt"
# Stamped with the epoch (SOURCE_DATE_EPOCH=0): before FAT's range, so the
# server clamps it to the first time the FAT driver can keep, 1981-01-01.
printf 'epoch\n' > "$DL/epoch.txt"
touch -d @0 "$DL/epoch.txt"
sftp_batch times <<'EOF'
put -p stamped.txt /fs0/sftptest/stamped.txt
get -p /fs0/sftptest/stamped.txt stamped-back.txt
rm /fs0/sftptest/stamped.txt
put -p epoch.txt /fs0/sftptest/epoch.txt
get -p /fs0/sftptest/epoch.txt epoch-back.txt
rm /fs0/sftptest/epoch.txt
EOF
check "times: put -p and get -p, sftp exits 0 ($SFTP_RC), nothing on stderr" \
    test "$SFTP_RC" = 0 -a ! -s "$TEST_TMPDIR/times.err"
check "times: put -p keeps the mtime ($(stat -c %Y "$DL/stamped-back.txt" 2>/dev/null))" \
    test "$(stat -c %Y "$DL/stamped-back.txt" 2>/dev/null)" = 1000000000
check "times: put -p of an epoch-stamped file stores 1981-01-01 ($(stat -c %Y "$DL/epoch-back.txt" 2>/dev/null))" \
    test "$(stat -c %Y "$DL/epoch-back.txt" 2>/dev/null)" = 347155200
scp_rc=0
timeout 120 scp -p -P "$HOST_PORT" "${CLIENT_OPTS[@]}" "$DL/stamped.txt" \
    anyone@127.0.0.1:/fs0/sftptest/stamped-scp.txt \
    > "$TEST_TMPDIR/scp-p.out" 2> "$TEST_TMPDIR/scp-p.err" < /dev/null || scp_rc=$?
DUMP_SFTP+=("$TEST_TMPDIR/scp-p.err")
sftp_batch times-scp <<'EOF'
get -p /fs0/sftptest/stamped-scp.txt stamped-scp-back.txt
rm /fs0/sftptest/stamped-scp.txt
EOF
check "times: scp -p exits 0 ($scp_rc) and keeps the mtime ($(stat -c %Y "$DL/stamped-scp-back.txt" 2>/dev/null))" \
    test "$scp_rc" = 0 -a "$SFTP_RC" = 0 \
        -a "$(stat -c %Y "$DL/stamped-scp-back.txt" 2>/dev/null)" = 1000000000

# --- write-refused ------------------------------------------------------------------------
# A file with the read-only attribute is mode 0444 to a client: a put over
# it is refused as OpenSSH refuses one over a file it may not write.
sftp_batch roattr <<'EOF'
put up.txt /fs0/sftptest/ro.txt
EOF
check "write-refused: sftp exits 1 ($SFTP_RC)" test "$SFTP_RC" = 1
check "write-refused: a put over a read-only file is Permission denied, exactly" \
    err_is roattr 'dest open "/fs0/sftptest/ro.txt": Permission denied'
# FAT under UEFI also refuses to delete or rename it: Permission denied.
sftp_batch roattr-rm <<'EOF'
rm /fs0/sftptest/ro.txt
EOF
check "write-refused: rm of a read-only file is Permission denied, exactly ($SFTP_RC)" \
    err_is roattr-rm 'remote delete /fs0/sftptest/ro.txt: Permission denied'
sftp_batch roattr-mv <<'EOF'
rename /fs0/sftptest/ro.txt /fs0/sftptest/ro2.txt
EOF
check "write-refused: rename of a read-only file is Permission denied, exactly ($SFTP_RC)" \
    err_is roattr-mv 'remote rename "/fs0/sftptest/ro.txt" to "/fs0/sftptest/ro2.txt": Permission denied'
# Across volumes: EXDEV, which sftp-server reports as Failure. The target's
# directory exists on fs20, so only the volume check refuses it.
sftp_batch xvol <<'EOF'
rename /fs0/sftptest/hello.txt /fs20/sftptest/hello.txt
EOF
check "write-refused: a rename across volumes fails ($SFTP_RC), in sftp(1)'s words" \
    err_is xvol 'remote rename "/fs0/sftptest/hello.txt" to "/fs20/sftptest/hello.txt": Failure'
sftp_batch xvol-ls <<'EOF'
ls -1 /fs0/sftptest/hello.txt
ls -1 /fs20/sftptest
EOF
check "write-refused: ...and nothing moved" \
    out_is xvol-ls "$(printf '%s\n' 'sftp> ls -1 /fs0/sftptest/hello.txt' \
        '/fs0/sftptest/hello.txt' 'sftp> ls -1 /fs20/sftptest')"
sftp_batch rmdirfull <<'EOF'
rmdir /fs0/sftptest/sub
EOF
check "write-refused: rmdir of a non-empty directory fails ($SFTP_RC), in sftp(1)'s words" \
    err_is rmdirfull 'remote rmdir "/fs0/sftptest/sub": Failure'

# --- extensions -----------------------------------------------------------------------------
sftp_raw ext <<'EOF'
init
limits
EOF
check "extensions: VERSION names posix-rename and limits; limits is 256 KB packet, 32 KB read, 256 KB - 1024 write, 16 handles, exactly ($RAW_RC)" \
    out_is ext "$(printf '%s\n' \
        'VERSION 3 posix-rename@openssh.com=1 limits@openssh.com=1' \
        'LIMITS 262144 32768 261120 16')"

# --- posix-rename -------------------------------------------------------------------------------
# sftp(1)'s `rename` sends posix-rename@openssh.com to a server that
# advertises it, and that replaces an existing target, as rename(2) does.
sftp_batch prename <<'EOF'
put up.txt /fs0/sftptest/pr-a.txt
put big-up.bin /fs0/sftptest/pr-b.bin
rename /fs0/sftptest/pr-a.txt /fs0/sftptest/pr-b.bin
get /fs0/sftptest/pr-b.bin pr-back.txt
ls -1 /fs0/sftptest
rm /fs0/sftptest/pr-b.bin
EOF
check "posix-rename: sftp exits 0 ($SFTP_RC), nothing on stderr" \
    test "$SFTP_RC" = 0 -a ! -s "$TEST_TMPDIR/prename.err"
check "posix-rename: rename onto an existing file replaces it, nothing set aside is left" \
    out_is prename "$(printf '%s\n' \
        'sftp> put up.txt /fs0/sftptest/pr-a.txt' \
        'sftp> put big-up.bin /fs0/sftptest/pr-b.bin' \
        'sftp> rename /fs0/sftptest/pr-a.txt /fs0/sftptest/pr-b.bin' \
        'sftp> get /fs0/sftptest/pr-b.bin pr-back.txt' \
        'sftp> ls -1 /fs0/sftptest' '/fs0/sftptest/big-up.bin' '/fs0/sftptest/big.bin' \
        '/fs0/sftptest/hello.txt' '/fs0/sftptest/pr-b.bin' '/fs0/sftptest/ro.txt' \
        '/fs0/sftptest/sub' \
        'sftp> rm /fs0/sftptest/pr-b.bin')"
check "posix-rename: the replaced file holds the renamed one's bytes" \
    test "$(sum_of "$DL/pr-back.txt")" = "$SUM_UP"
# One entry under two spellings: onto its own 8.3 alias is OK and changes
# nothing -- the long name stays -- and a change of letter case alone is
# made. sftp(1) lists names sorted bytewise, so upper case comes first.
sftp_batch prename-alias <<'EOF'
put up.txt /fs0/sftptest/longfilename.txt
rename /fs0/sftptest/longfilename.txt /fs0/sftptest/LONGFI~1.TXT
ls -1 /fs0/sftptest
rename /fs0/sftptest/longfilename.txt /fs0/sftptest/LongFileName.TXT
ls -1 /fs0/sftptest
get /fs0/sftptest/LongFileName.TXT alias-back.txt
rm /fs0/sftptest/LongFileName.TXT
EOF
check "posix-rename: sftp exits 0 for the alias and the case-only rename ($SFTP_RC), nothing on stderr" \
    test "$SFTP_RC" = 0 -a ! -s "$TEST_TMPDIR/prename-alias.err"
check "posix-rename: onto its 8.3 alias changes nothing; a case-only rename takes the new spelling, exactly" \
    out_is prename-alias "$(printf '%s\n' \
        'sftp> put up.txt /fs0/sftptest/longfilename.txt' \
        'sftp> rename /fs0/sftptest/longfilename.txt /fs0/sftptest/LONGFI~1.TXT' \
        'sftp> ls -1 /fs0/sftptest' '/fs0/sftptest/big-up.bin' '/fs0/sftptest/big.bin' \
        '/fs0/sftptest/hello.txt' '/fs0/sftptest/longfilename.txt' '/fs0/sftptest/ro.txt' \
        '/fs0/sftptest/sub' \
        'sftp> rename /fs0/sftptest/longfilename.txt /fs0/sftptest/LongFileName.TXT' \
        'sftp> ls -1 /fs0/sftptest' '/fs0/sftptest/LongFileName.TXT' \
        '/fs0/sftptest/big-up.bin' '/fs0/sftptest/big.bin' '/fs0/sftptest/hello.txt' \
        '/fs0/sftptest/ro.txt' '/fs0/sftptest/sub' \
        'sftp> get /fs0/sftptest/LongFileName.TXT alias-back.txt' \
        'sftp> rm /fs0/sftptest/LongFileName.TXT')"
check "posix-rename: ...and the file kept its bytes through both" \
    test "$(sum_of "$DL/alias-back.txt")" = "$SUM_UP"
# `rename -l` sends the original SSH_FXP_RENAME, which never replaces.
sftp_batch prename-legacy <<'EOF'
rename -l /fs0/sftptest/hello.txt /fs0/sftptest/big.bin
EOF
check "posix-rename: rename -l onto an existing file fails ($SFTP_RC), in sftp(1)'s words" \
    err_is prename-legacy 'remote rename "/fs0/sftptest/hello.txt" to "/fs0/sftptest/big.bin": Failure'
# Onto an existing file on another volume: EXDEV, Failure, and neither file
# is touched -- the target is not even set aside.
sftp_batch prename-xvol <<'EOF'
put up.txt /fs20/sftptest/pr-x.txt
rename /fs0/sftptest/hello.txt /fs20/sftptest/pr-x.txt
EOF
check "posix-rename: onto a file on another volume fails ($SFTP_RC), in sftp(1)'s words" \
    err_is prename-xvol 'remote rename "/fs0/sftptest/hello.txt" to "/fs20/sftptest/pr-x.txt": Failure'
# A directory onto that file: rename(2) answers EXDEV before it looks at
# the types (ENOTDIR would be NO_SUCH_FILE), so it is Failure too.
sftp_raw prename-xvol-dir <<'EOF'
init
posix-rename /fs0/sftptest/sub /fs20/sftptest/pr-x.txt
EOF
check "posix-rename: a directory onto a file on another volume is Failure (EXDEV first), exactly ($RAW_RC)" \
    out_is prename-xvol-dir "$(printf '%s\n' \
        'VERSION 3 posix-rename@openssh.com=1 limits@openssh.com=1' 'STATUS 4 Failure')"
rm -f "$DL/prx-back.txt" "$DL/prx-hello.txt"
sftp_batch prename-xvol-back <<'EOF'
get /fs20/sftptest/pr-x.txt prx-back.txt
get /fs0/sftptest/hello.txt prx-hello.txt
rm /fs20/sftptest/pr-x.txt
EOF
check "posix-rename: ...and both files are as they were ($SFTP_RC)" \
    test "$SFTP_RC" = 0 -a "$(sum_of "$DL/prx-back.txt")" = "$SUM_UP" \
        -a "$(sum_of "$DL/prx-hello.txt")" = "$SUM_HELLO"

# --- grow ---------------------------------------------------------------------------------------
# 16777216 is SFTP_GROW_MAX: the FAT driver zero-fills a growth inside one
# call, with every session waiting, so one request may add at most that.
# The file starts at 5 bytes, so the limit is measured from its end: from
# an empty file a 16 MiB offset could never exceed it, whatever the check.
# Then two growths pipelined: the server answers the first and yields, and
# only sshd's loop resuming it answers the second -- without that resume
# the reply never comes and the script ends short.
# Served by `sshd -v` (DEBUG), so the helper's yield line reaches the log:
# the replies alone cannot tell "yielded and was resumed" from "never
# yielded".
stop_sshd
start_sshd "sshd -v -P $PW"
YIELD_RE='(^|[0-9:.]+ )\[DEBUG\] sftp: yielding after a zero-fill until the loop resumes the session$'
sftp_raw grow <<'EOF'
init
open /fs0/sftptest/grow.bin 1a
write 0 5
fsetstat-size 16777222
fsetstat-size 16777221
fsetstat-size 5
write 16777222 1
write 16777221 1
fsetstat-size 0
close
open /fs0/sftptest/grow2.bin 1a
close
pipe-setstat-size 1048576 /fs0/sftptest/grow.bin /fs0/sftptest/grow2.bin
EOF
check "grow: from 5 bytes, a size or a WRITE gap of 16 MiB + 1 is Failure, of 16 MiB done, exactly ($RAW_RC)" \
    out_is grow "$(printf '%s\n' 'VERSION 3 posix-rename@openssh.com=1 limits@openssh.com=1' \
        'HANDLE 0000000000000001' 'STATUS 0 Success' 'STATUS 4 Failure' 'STATUS 0 Success' \
        'STATUS 0 Success' 'STATUS 4 Failure' 'STATUS 0 Success' 'STATUS 0 Success' \
        'STATUS 0 Success' 'HANDLE 0000000000000002' 'STATUS 0 Success' \
        'STATUS 0 Success' 'STATUS 0 Success')"
sftp_batch grow-rm <<'EOF'
rm /fs0/sftptest/grow.bin
rm /fs0/sftptest/grow2.bin
EOF
check "grow: the file is removed ($SFTP_RC)" test "$SFTP_RC" = 0
# Four growths ran -- 16 MiB by size, a 16 MiB WRITE gap, two pipelined
# sizes -- and each yielded once; the pipelined second was answered only
# after its resume.
check "grow: each of the four growths yielded the loop once, by the exact debug line ($(guest_re "$YIELD_RE"))" \
    test "$(guest_re "$YIELD_RE")" = 4

# --- cancel-on-close ----------------------------------------------------------------------
# A session that yields and whose channel closes before the resume runs:
# the relay must cancel the resume, or it runs on a freed session. ssh(1)
# cannot close inside that window, so ssh-peer.py sends, in ONE write, the
# CHANNEL_DATA carrying INIT and a growing SETSTAT AND the CHANNEL_CLOSE:
# the server parses both in one pass, so the close comes between the yield
# and the next drain. The debug lines give the order: the yield, then the
# cancel, then `chan N: closed` -- and no resume. If the batch arrived split
# (the resume ran first) the window was missed and the test says so.
CC_PATH=/fs0/sftptest/cc.bin
sftp_raw cc-make <<'EOF'
init
open /fs0/sftptest/cc.bin 1a
close
EOF
check "cancel: an empty file to grow is made, exactly ($RAW_RC)" \
    out_is cc-make "$(printf '%s\n' 'VERSION 3 posix-rename@openssh.com=1 limits@openssh.com=1' \
        'HANDLE 0000000000000001' 'STATUS 0 Success')"
# An SFTP string, as hex.
sstr_hex() { local h; h=$(hexof "$1"); printf '%08x%s' $(( ${#h} / 2 )) "$h"; }
CC_SET="0900000001$(sstr_hex "$CC_PATH")000000010000000000100000"
CC_SFTP="000000050100000003$(printf '%08x' $(( ${#CC_SET} / 2 )))$CC_SET"
CC_DATA="@00000000$(printf '%08x' $(( ${#CC_SFTP} / 2 )))$CC_SFTP"
CC_SUBSYS="@00000000$(sstr_hex subsystem)01$(sstr_hex sftp)"
RESUME_RE='\[DEBUG\] sftp:[a-z_0-9]+:[0-9]+: resuming a yielded session$'
CANCEL_RE='\[DEBUG\] sftp:[a-z_0-9]+:[0-9]+: cancelling a pending resume: its channel closed$'
CLOSED_RE='\[DEBUG\] ssh:[a-z_0-9]+:[0-9]+: chan [0-9]+: closed$'
cc_y0=$(guest_re "$YIELD_RE"); cc_r0=$(guest_re "$RESUME_RE"); cc_c0=$(guest_re "$CANCEL_RE")
peer_start cc "$PW" "send:90:$(peer_open 1048576),expect:91,send:98:$CC_SUBSYS,expect:99,batch:94:$CC_DATA+97:@00000000,idle:2"
cc_rc=0
wait "${PEER_PID[cc]}" || cc_rc=$?
PEER_PID[cc]=""   # reaped: the cleanup trap must not kill a pid that is gone
DUMP_SFTP+=("$(peer_log cc)")
check "cancel: ssh-peer.py sent the data and the close in one batch (rc $cc_rc)" \
    grep -qE '^peer: sent batch 94\+97 seqs=' "$(peer_log cc)"
sleep 1
cc_y=$(( $(guest_re "$YIELD_RE") - cc_y0 ))
cc_r=$(( $(guest_re "$RESUME_RE") - cc_r0 ))
cc_c=$(( $(guest_re "$CANCEL_RE") - cc_c0 ))
if (( cc_y == 1 && cc_r == 1 && cc_c == 0 )); then
    echo "  NOTE: the batch arrived split -- the resume ran before the close, so the"
    echo "        cancel was not exercised (a host-side split, not a server defect)"
fi
check "cancel: the growth yielded once, the close cancelled its resume, and no resume ran (yield $cc_y, cancel $cc_c, resume $cc_r)" \
    test "$cc_y" = 1 -a "$cc_c" = 1 -a "$cc_r" = 0
# The order in the log: the last yield, then the last cancel, then a
# `chan N: closed` after it.
test_clean_log
cc_yl=$(grep -naE -- "$YIELD_RE" "$TEST_CLEAN_LOG" | tail -1 | cut -d: -f1 || true)
cc_cl=$(grep -naE -- "$CANCEL_RE" "$TEST_CLEAN_LOG" | tail -1 | cut -d: -f1 || true)
cc_xl=$(grep -naE -- "$CLOSED_RE" "$TEST_CLEAN_LOG" | awk -F: -v after="${cc_cl:-0}" '$1 > after {print $1; exit}' || true)
check "cancel: in the log the yield ($cc_yl) comes before the cancel ($cc_cl), and a channel close ($cc_xl) after it" \
    test -n "$cc_yl" -a -n "$cc_cl" -a -n "$cc_xl" -a "${cc_yl:-0}" -lt "${cc_cl:-0}" -a "${cc_cl:-0}" -lt "${cc_xl:-0}"
echo "  cancel: the scenario's lines, in log order:"
grep -naE -- "$YIELD_RE|$CANCEL_RE|$RESUME_RE|$CLOSED_RE" "$TEST_CLEAN_LOG" | tail -6 | sed 's/^/    | /' || true
# What follows still works, and nothing faulted.
sftp_batch cc-after <<'EOF'
ls -1 /fs0/sftptest/cc.bin
rm /fs0/sftptest/cc.bin
EOF
check "cancel: a following session works ($SFTP_RC)" \
    out_is cc-after "$(printf '%s\n' 'sftp> ls -1 /fs0/sftptest/cc.bin' '/fs0/sftptest/cc.bin' \
        'sftp> rm /fs0/sftptest/cc.bin')"
check "cancel: no CPU exception and no heap corruption ($(cpu_exceptions), $(heap_corruptions))" \
    test "$(cpu_exceptions)" = 0 -a "$(heap_corruptions)" = 0
stop_sshd
start_sshd 'sshd'

# --- scp-up ---------------------------------------------------------------------------------
# Up to the canonical form and the two Shell forms (a doubled `\`, as for a
# download), then each back down over sftp.
SCP_UP=('/fs0/sftptest/scp-posix.txt' 'FS0:/sftptest/scp-shell.txt'
        'FS0:\\sftptest\\scp-bs.txt')
for i in "${!SCP_UP[@]}"; do
    scp_rc=0
    timeout 120 scp -P "$HOST_PORT" "${CLIENT_OPTS[@]}" "$DL/up.txt" \
        "anyone@127.0.0.1:${SCP_UP[$i]}" \
        > "$TEST_TMPDIR/scpup$i.out" 2> "$TEST_TMPDIR/scpup$i.err" < /dev/null || scp_rc=$?
    DUMP_SFTP+=("$TEST_TMPDIR/scpup$i.err")
    check "scp-up host:'${SCP_UP[$i]}': exit 0 ($scp_rc), nothing on stderr" \
        test "$scp_rc" = 0 -a ! -s "$TEST_TMPDIR/scpup$i.err"
done
sftp_batch scpback <<'EOF'
get /fs0/sftptest/scp-posix.txt scp-posix-back.txt
get /fs0/sftptest/scp-shell.txt scp-shell-back.txt
get /fs0/sftptest/scp-bs.txt scp-bs-back.txt
EOF
check "scp-up: all three uploads come back byte for byte ($SFTP_RC)" \
    test "$SFTP_RC" = 0 -a "$(sum_of "$DL/scp-posix-back.txt")" = "$SUM_UP" \
        -a "$(sum_of "$DL/scp-shell-back.txt")" = "$SUM_UP" \
        -a "$(sum_of "$DL/scp-bs-back.txt")" = "$SUM_UP"

# --- unload -----------------------------------------------------------------------------
# Three starts so far (sshd, sshd -P, sshd), 37 sessions among them: one load
# per start, before it serves, one unload per stop, and none for a session.
check "unload: one load per start and one unload per stop, none per session" seq_is LSULSULS

# --- sftp-read-only --------------------------------------------------------------------------
# AxlSftpReadOnly yes in the file, --sftp-read-only, and -R (a session that
# cannot type cannot write files either): each still serves a get, and
# refuses a put with the line sftp(1) prints against sftp-server -R, and an
# rm likewise. Nothing is created or removed.
ro_case() {
    local name="$1" cmd="$2"
    stop_sshd
    start_sshd "$cmd"
    rm -f "$DL/ro-hello.txt"
    sftp_batch "ro-$name" <<'EOF'
get /fs0/sftptest/hello.txt ro-hello.txt
put up.txt /fs0/sftptest/ro-up.txt
EOF
    check "sftp-read-only ($cmd): the put fails, sftp exits 1 ($SFTP_RC)" \
        test "$SFTP_RC" = 1
    check "sftp-read-only ($cmd): ...refused Permission denied, in sftp(1)'s exact words" \
        err_is "ro-$name" 'dest open "/fs0/sftptest/ro-up.txt": Permission denied'
    check "sftp-read-only ($cmd): ...while a get still reads" \
        test "$(sum_of "$DL/ro-hello.txt")" = "$SUM_HELLO"
    sftp_batch "ro-rm-$name" <<'EOF'
rm /fs0/sftptest/hello.txt
EOF
    check "sftp-read-only ($cmd): an rm is refused ($SFTP_RC), in sftp(1)'s exact words" \
        err_is "ro-rm-$name" 'remote delete /fs0/sftptest/hello.txt: Permission denied'
    sftp_raw "ro-raw-$name" <<'EOF'
init
posix-rename /fs0/sftptest/hello.txt /fs0/sftptest/hello2.txt
limits
EOF
    check "sftp-read-only ($cmd): VERSION leaves posix-rename out, one sent anyway is Permission denied, limits still answers ($RAW_RC)" \
        out_is "ro-raw-$name" "$(printf '%s\n' 'VERSION 3 limits@openssh.com=1' \
            'STATUS 3 Permission denied' 'LIMITS 262144 32768 261120 16')"
    sftp_batch "ro-ls-$name" <<'EOF'
ls -1 /fs0/sftptest
EOF
    check "sftp-read-only ($cmd): nothing was created or removed" \
        out_is "ro-ls-$name" "$(printf '%s\n' 'sftp> ls -1 /fs0/sftptest' \
            '/fs0/sftptest/big-up.bin' '/fs0/sftptest/big.bin' '/fs0/sftptest/hello.txt' \
            '/fs0/sftptest/ro.txt' '/fs0/sftptest/scp-bs.txt' \
            '/fs0/sftptest/scp-posix.txt' '/fs0/sftptest/scp-shell.txt' \
            '/fs0/sftptest/sub')"
}
ro_case conf 'sshd -f sftpro.conf'
ro_case flag 'sshd --sftp-read-only'
# Only -R's implied read-only is announced: the other two were asked for.
FORCED_RE="${SFTP_PFX}read-only \\(AxlReadOnly\\)\$"
check "sftp-read-only: AxlSftpReadOnly and the flag log no AxlReadOnly line ($(guest_re "$FORCED_RE"))" \
    test "$(guest_re "$FORCED_RE")" = 0
ro_case R 'sshd -R'
check "sftp-read-only: -R logs once why SFTP is read-only ($(guest_re "$FORCED_RE"))" \
    test "$(guest_re "$FORCED_RE")" = 1
stop_sshd

# --- no-helper --------------------------------------------------------------------------
send 'mv fs0:\SftpServer.efi fs0:\SftpServer.off\r'
send 'echo %T%-moved\r'
wait_guest "p4f-moved" 1 20 || true
start_sshd 'sshd'
sftp_batch nohelper <<'EOF'
ls /
EOF
check "no-helper: sftp exits 255 ($SFTP_RC)" test "$SFTP_RC" = 255
check "no-helper: sshd's stderr line, then sftp(1)'s Connection closed" \
    err_is nohelper "$(printf '%s\n' 'sshd: subsystem sftp: FS0:\SftpServer.efi not found' 'Connection closed')"
check "no-helper: the start loaded nothing, the session loaded nothing" \
    seq_is LSULSULSULSULSULSUS
# Known at start, so said at start, at the default level: an operator learns
# of a missing helper before the first client does.
NOHELPER_RE="${SFTP_PFX}SFTP helper FS0:\\\\SftpServer\\.efi not found; sftp sessions are refused\$"
check "no-helper: the start logs once that the helper is missing ($(guest_re "$NOHELPER_RE"))" \
    test "$(guest_re "$NOHELPER_RE")" = 1

# --- disabled -----------------------------------------------------------------------------
stop_sshd
start_sshd 'sshd -f nosftp.conf'
sftp_batch disabled <<'EOF'
ls /
EOF
OFF_RE="${SFTP_PFX}off \\(nosftp\\.conf has no Subsystem line\\)\$"
check "disabled: sshd logs that SFTP is off, and why, once ($(guest_re "$OFF_RE"))" \
    test "$(guest_re "$OFF_RE")" = 1
check "disabled: ...and no such line for the shipped sshd.conf, which has one ($(guest_re "${SFTP_PFX}off .*\$"))" \
    test "$(guest_re "${SFTP_PFX}off .*\$")" = 1
NOTFOUND_RE="${SFTP_PFX}subsystem request for sftp by user anyone failed, subsystem not found\$"
check "disabled: the refusal is logged in sshd's words, naming the user ($(guest_re "$NOTFOUND_RE"))" \
    wait_guest_re "$NOTFOUND_RE" 1 10
check "disabled: sftp exits 255 ($SFTP_RC)" test "$SFTP_RC" = 255
check "disabled: the subsystem request is refused, in ssh(1)'s and sftp(1)'s words" \
    err_is disabled "$(printf '%s\n' 'subsystem request failed on channel 0' 'Connection closed')"

# --- bad-helper ---------------------------------------------------------------------------
stop_sshd
start_sshd 'sshd -f garbage.conf'
sftp_batch garbage <<'EOF'
ls /
EOF
check "bad-helper: a helper that is no image -- sftp exits 255 ($SFTP_RC)" test "$SFTP_RC" = 255
check "bad-helper: ...told on stderr that it cannot load" \
    err_is garbage "$(printf '%s\n' 'sshd: subsystem sftp: FS0:\garbage.efi cannot load' 'Connection closed')"
stop_sshd
start_sshd 'sshd -f badstart.conf'
sftp_batch badstart <<'EOF'
ls /
EOF
check "bad-helper: a helper whose entry fails -- sftp exits 255 ($SFTP_RC)" test "$SFTP_RC" = 255
check "bad-helper: ...told on stderr that it failed to start" \
    err_is badstart "$(printf '%s\n' 'sshd: subsystem sftp: FS0:\badstart.efi failed to start' 'Connection closed')"

# --- full ----------------------------------------------------------------------------------
stop_sshd
start_sshd 'sshd-full'
sftp_batch full <<'EOF'
get /fs0/sftptest/big.bin
EOF
check "full: sftp exits 0 ($SFTP_RC) with no SftpServer.efi on the volume" test "$SFTP_RC" = 0
check "full: the 1 MiB file arrives byte for byte" test "$(sum_of "$DL/big.bin")" = "$SUM_BIG"
check "full: the helper came from the embedded copy" \
    wait_guest_re "${SFTP_PFX}SFTP helper loaded from \\(embedded\\)\$" 1 10
check "full: ...loaded as sshd-full started, before it served; the session left it loaded" \
    seq_is LSULSULSULSULSULSUSSSSLS

# --- stop-open -------------------------------------------------------------------------------
# An interactive sftp(1) held open on a FIFO: the helper has its session
# open when `sshd stop` comes.
SUBSYS_RE="${SFTP_PFX}subsystem request for sftp by user anyone\$"
S1=$(guest_re "$SUBSYS_RE")
mkfifo "$TEST_TMPDIR/hold.in"
exec {HOLD_FD}<>"$TEST_TMPDIR/hold.in"
(cd "$DL" && timeout 120 sftp -P "$HOST_PORT" "${CLIENT_OPTS[@]}" anyone@127.0.0.1) \
    <&"$HOLD_FD" > "$TEST_TMPDIR/hold.out" 2> "$TEST_TMPDIR/hold.err" &
HOLD_PID=$!
check "stop-open: the held session opened the subsystem" \
    wait_guest_re "$SUBSYS_RE" $((S1 + 1)) 30
stop_sshd
check "stop-open: the helper was unloaded as sshd stopped" \
    seq_is LSULSULSULSULSULSUSSSSLSU
# The connection drops under the session, as OpenSSH's sshd ending takes its
# sftp-server with it; ssh(1) says so, exactly. (sftp(1) itself then waits
# at its prompt for the next command, so its exit is not the signal.)
hold_dropped() {
    local i
    for ((i = 0; i < 50; i++)); do
        tr -d '\r' < "$TEST_TMPDIR/hold.err" \
            | grep -qxF 'Connection to 127.0.0.1 closed by remote host.' && return 0
        sleep 0.2
    done
    return 1
}
check "stop-open: ssh(1) reports the connection closed by the remote host" hold_dropped
kill "$HOLD_PID" 2>/dev/null || true
wait "$HOLD_PID" 2>/dev/null || true
exec {HOLD_FD}>&-

sshd_poweroff
test_clean_log
if test_check_leaks; then
    pass "boot: every leak verdict is clean"
else
    fail "boot: a leak or a missing verdict"
fi
n=$(heap_corruptions)
check "boot: no heap corruption reported ($n)" test "$n" = 0

if (( FAIL > 0 )); then
    for f in "${DUMP_SFTP[@]}"; do
        echo "--- $f"; tr -d '\r' < "$f" | tail -20
    done
fi
echo ""
echo "sshd-sftp ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 ))
