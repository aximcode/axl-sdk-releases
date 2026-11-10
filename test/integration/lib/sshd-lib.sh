# shellcheck shell=bash
# sshd-lib.sh -- the helpers shared by the `sshd` tool's suites
# (test-sshd-qemu.sh, test-sshd-lifecycle-qemu.sh): the boot, the Shell typed
# into over the serial console, real ssh(1) sessions held open on FIFOs, the
# session transcripts read back as lines, and ssh-peer.py for what ssh(1)
# cannot send. Sourced after common-test.sh's test_setup. Never run on its own.
#
# Markers are emitted through a Shell variable (`set -v T p4f`, then
# `echo %T%-<rand>`), so the Shell's own echo of the typed command line --
# which carries the literal `%T%` -- can never match an output assertion.
# Every count is of WHOLE lines.
#
# What a scenario waits for at its end. AxlSsh logs nothing when a connection
# ends (no line follows its close hook unless `ssh` DEBUG is on), and the
# sshd driver keeps no per-connection line of its own. So no assertion here
# counts server lines across connections: a scenario counts lines of its OWN
# session transcripts and Shell lines carrying its OWN random marker, and
# ends by waiting for its ssh(1) processes to exit.

PEER="$(dirname "$0")/ssh-peer.py"

# A host tool this suite cannot run without. In CI (GitHub Actions sets
# CI=true) its absence is a FAILURE -- a suite CI never runs proves nothing;
# the self-hosted runner's PATH carries ~/.local/bin, where this box's tmux
# lives. On a dev box it is a SKIP, which the runner's headline counts.
missing_tool() {
    if [[ "${CI:-}" == true || "${GITHUB_ACTIONS:-}" == true ]]; then
        test_host_fail "$1 (CI must run this suite)"
        exit 1
    fi
    echo "SKIP: $1"
    exit 77
}

# missing_tool for each named host command that is not on PATH.
sshd_require_tools() {
    local t
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 || missing_tool "no $t on the host PATH (needs=$t)"
    done
}

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local label="$1"
    shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}

# A log line is "<time> [LEVEL]  <domain>: <message>" -- the guest's clock in
# front, and on x64 the firmware's ESC[=3h ahead of the first one after the
# take-over -- so it is matched from the level tag to the END of the line.
LOG_PFX='(^|[0-9:.]+ )\[INFO\]  '
# A DEBUG line names its function and line too ("sshd:go_behind:424: "), and
# is NOT anchored: it lands wherever the console's own output left the
# cursor, so it can follow part of a line on the serial log.
DBG_PFX='\[DEBUG\] sshd:[a-z_0-9]+:[0-9]+: '

# The slirp gateway: every host connection reaches the guest from it.
HOST_AS_SEEN=10.0.2.2/32
GUEST_PORT=22
# The address slirp leases the guest: where sshd says it serves.
GUEST_IP=10.0.2.15


send() { test_send_stdin "$1"; }

# The `sshd` option that puts the server's log on the console's own serial
# port. By default sshd writes nothing to a port the console draws on
# (test-sshd-console-qemu.sh proves it), so a suite that reads the server's
# log lines off the serial console -- its serving line with the console
# size, a login, a session falling behind -- asks for them there, as an
# operator can.
SSHD_LOG_HERE='--log-port 0'

# How many WHOLE lines of the clean serial log are exactly $1.
guest_lines() {
    local n
    test_clean_log
    n=$(grep -acxF -- "$1" "$TEST_CLEAN_LOG" || true)
    echo "${n:-0}"
}

# Wait up to $3 s for at least $2 whole serial lines exactly $1.
wait_guest() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        if (( $(guest_lines "$1") >= $2 )); then return 0; fi
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

# How many serial lines match the extended regex $1, and a wait of up to $3 s
# for at least $2 of them.
guest_re() {
    local n
    test_clean_log
    n=$(grep -acE -- "$1" "$TEST_CLEAN_LOG" || true)
    echo "${n:-0}"
}
wait_guest_re() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        if (( $(guest_re "$1") >= $2 )); then return 0; fi
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

# The output as lines: CSI cursor moves become line breaks (the console
# positions rather than prints a newline), every other CSI and the
# private-mode/charset escapes are dropped, and CR is a line break.
vt_lines() {
    sed -E $'s/\x1b\\[[0-9;]*[Hf]/\\n/g; s/\x1b\\[[0-9;?=]*[A-Za-z]//g; s/\x1b[()][0-9A-Za-z]//g; s/\r/\\n/g' \
        | sed -E 's/[[:space:]]+$//'
}
session_lines() { vt_lines < "$1"; }
session_count() {
    local n
    n=$(session_lines "$1" | grep -acxF -- "$2" || true)
    echo "${n:-0}"
}
wait_session() {
    local i
    for ((i = 0; i < $4 * 5; i++)); do
        if (( $(session_count "$1" "$2") >= $3 )); then return 0; fi
        sleep 0.2
    done
    return 1
}
# The output as ONE string: a console line that wraps is broken by cursor
# moves, so an exact check of a long line joins the lines back up.
session_joined_count() {
    local n
    n=$(session_lines "$1" | tr -d '\n' | grep -aoF -- "$2" | wc -l)
    echo "${n:-0}"
}

rand_tag() { od -An -tx1 -N4 /dev/urandom | tr -d ' \n'; }

# ---------------------------------------------------------------------------
# Build, stage and boot
# ---------------------------------------------------------------------------

# Suites run together build the same targets at the same time;
# scripts/axl-build.sh serialises builds in one tree (it once took a lock
# here, which guarded only the sshd suites against each other).

# Build sshd for this arch and stage it; the session key pair is made here,
# its public half NOT staged (each suite decides where it goes).
sshd_build() {
    # Read here, not when this file is sourced: --arch is parsed after that,
    # and a make for the wrong arch leaves this arch's sshd.efi stale.
    local -A native=([X64]=x64 [AARCH64]=aa64)
    TEST_BUILD_DIR="$(test_build_dir)"
    if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="${native[$TEST_ARCH]:-x64}" \
            all sshd 2>&1 | tail -3 \
       || [[ ! -f "$TEST_BUILD_DIR/tools/sshd.efi" ]]; then
        echo "FAIL: sshd.efi was not built ($TEST_BUILD_DIR/tools/sshd.efi)"
        exit 1
    fi
    test_add_efi "$TEST_BUILD_DIR/tools/sshd.efi"
    ssh-keygen -q -t ed25519 -N '' -C 'axl-sshd-test' -f "$TEST_TMPDIR/key" </dev/null
}

# Boot to the Shell with the network up, the host typing on serial, and the
# guest's port 22 forwarded; then `set -v T p4f`. $1: extra startup.nsh lines
# run after DHCP (may be empty). SSHD_QEMU_EXTRA (an array, may be unset) is
# appended to the QEMU command line: more devices for the guest.
#
# For a suite that boots the network its own way: SSHD_NET (an array) replaces
# the forwarded NIC, each `@HOSTFWD@` in it becoming the forward of the host
# port to $GUEST_PORT; SSHD_NO_DHCP=1 leaves the startup without its
# `ifconfig`, so sshd brings the network up itself.
sshd_boot() {
    HOST_PORT=$(test_port 0)
    {
        echo "@echo -off"
        echo "fs0:"
        echo "cd \\"
        echo "echo Connecting drivers..."
        echo "connect -r"
        echo "stall 1000000"
        if [[ "${SSHD_NO_DHCP:-0}" != 1 ]]; then
            echo "echo Configuring network via DHCP..."
            echo "ifconfig -s eth0 dhcp"
            echo "stall 3000000"
        fi
        [[ -n "${1:-}" ]] && printf '%s\n' "$1"
        echo "echo SSHD-BOOTED"
    } | test_set_startup
    test_build_image
    test_build_qemu_cmd
    if [[ -n "${SSHD_NET+x}" ]]; then
        local a
        for a in ${SSHD_NET[@]+"${SSHD_NET[@]}"}; do
            TEST_QEMU_CMD+=("${a//@HOSTFWD@/,hostfwd=tcp::${HOST_PORT}-:${GUEST_PORT}}")
        done
    else
        test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
    fi
    TEST_QEMU_CMD+=(${SSHD_QEMU_EXTRA[@]+"${SSHD_QEMU_EXTRA[@]}"})
    # SSHD_ALLOW_REBOOT=1: a guest `reset` reboots in place, keeping the
    # variable store (test-sshd-hostkey-qemu.sh), instead of ending QEMU.
    if [[ "${SSHD_ALLOW_REBOOT:-0}" == 1 ]]; then
        local _cmd=() _a
        for _a in "${TEST_QEMU_CMD[@]}"; do [[ "$_a" == -no-reboot ]] || _cmd+=("$_a"); done
        TEST_QEMU_CMD=("${_cmd[@]}")
    fi
    test_enable_stdin
    test_run_background
    trap '_sshd_cleanup' EXIT
    echo "  QEMU PID: $TEST_QEMU_PID, host :$HOST_PORT -> guest :$GUEST_PORT"
    if ! wait_guest "SSHD-BOOTED" 1 120; then
        echo "FAIL: the guest never reached its prompt"
        test_clean_log; tail -30 "$TEST_CLEAN_LOG"
        exit 1
    fi
    sleep 1
    send 'set -v T p4f\r'
}

# The driver's serving line gives the console size; COLS/ROWS/GEOM from it.
# Returns 1 when the line is not there.
sshd_read_geometry() {
    local re line
    re="${LOG_PFX}sshd: serving the UEFI Shell on ${GUEST_IP//./\\.} port $GUEST_PORT \\(console ([0-9]+)x([0-9]+)\\)( read-only)?\$"
    test_clean_log
    line=$(grep -aE "$re" "$TEST_CLEAN_LOG" | tail -1 || true)
    [[ -n "$line" ]] || return 1
    COLS=$(sed -E 's/.*\(console ([0-9]+)x([0-9]+)\).*/\1/' <<< "$line")
    ROWS=$(sed -E 's/.*\(console ([0-9]+)x([0-9]+)\).*/\2/' <<< "$line")
    GEOM="sshd: the console is ${COLS}x${ROWS}; a smaller terminal waits until it is enlarged"
    echo "  console: ${COLS}x${ROWS}"
}

# ---------------------------------------------------------------------------
# Real ssh(1) sessions, each typed into through its own FIFO
# ---------------------------------------------------------------------------

declare -A SSH_PID=()
declare -A SSH_FD=()
SSH_ARGV=()   # ssh_argv's result

# ssh_argv NAME [ssh options...] -- the one `ssh -tt` command line every
# session here runs, into the array SSH_ARGV: anyone@127.0.0.1 with the run's
# key (or what the options say), its -v log in ssh-NAME.log.
ssh_argv() {
    local name="$1"
    shift
    SSH_ARGV=(ssh -tt -v -E "$TEST_TMPDIR/ssh-$name.log" -p "$HOST_PORT"
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes
        -o IdentityAgent=none -i "$TEST_TMPDIR/key" "$@" anyone@127.0.0.1)
}

# ssh_open NAME [ssh options...] -- ssh_argv's session; NAME.out is what the
# session receives, NAME.err ssh's stderr. The FIFO is held open read-write
# here so ssh never sees EOF between writes.
ssh_open() {
    local name="$1" fd
    shift
    rm -f "$TEST_TMPDIR/$name.in"
    mkfifo "$TEST_TMPDIR/$name.in"
    exec {fd}<>"$TEST_TMPDIR/$name.in"
    SSH_FD[$name]=$fd
    ssh_argv "$name" "$@"
    "${SSH_ARGV[@]}" <&"$fd" > "$TEST_TMPDIR/$name.out" 2> "$TEST_TMPDIR/$name.err" &
    SSH_PID[$name]=$!
}

# ssh_open_sized NAME COLS ROWS [ssh options...] -- ssh_open on a terminal of
# that size: script(1) gives ssh a pty, sized before ssh reads it, so the
# pty-req carries COLSxROWS (ssh_open's FIFO stdin sends 0x0). NAME.out also
# holds what the pty echoes.
ssh_open_sized() {
    local name="$1" size="stty cols $2 rows $3" fd
    shift 3
    rm -f "$TEST_TMPDIR/$name.in"
    mkfifo "$TEST_TMPDIR/$name.in"
    exec {fd}<>"$TEST_TMPDIR/$name.in"
    SSH_FD[$name]=$fd
    ssh_argv "$name" "$@"
    script -qfec "$size; exec ${SSH_ARGV[*]@Q}" /dev/null \
        <&"$fd" > "$TEST_TMPDIR/$name.out" 2> "$TEST_TMPDIR/$name.err" &
    SSH_PID[$name]=$!
}

# The count of $2 (a grep -P pattern) in session $1's raw bytes, across lines.
session_raw_count() {
    local n
    n=$(grep -aozP -- "$2" "$(ssh_out "$1")" | tr -cd '\0' | wc -c)
    echo "${n:-0}"
}

# Wait up to $2 s for session $1's first bytes (the repaint).
ssh_wait_repaint() {
    local i
    for ((i = 0; i < $2 * 5; i++)); do
        [[ -s "$TEST_TMPDIR/$1.out" ]] && return 0
        sleep 0.2
    done
    return 1
}

ssh_type() { printf '%b' "$2" >&"${SSH_FD[$1]}"; }
ssh_out() { echo "$TEST_TMPDIR/$1.out"; }

# Is session $1's ssh(1) still running?
ssh_alive() { kill -0 "${SSH_PID[$1]}" 2>/dev/null; }

# Wait up to $2 s for session $1's ssh(1) to exit on its own; its status in
# SSH_RC. Returns 1 if it is still running.
ssh_wait_exit() {
    local i pid="${SSH_PID[$1]}"
    for ((i = 0; i < $2 * 5; i++)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            SSH_RC=0
            wait "$pid" 2>/dev/null || SSH_RC=$?
            SSH_PID[$1]=""
            _ssh_release_fd "$1"
            return 0
        fi
        sleep 0.2
    done
    return 1
}

_ssh_release_fd() {
    local fd="${SSH_FD[$1]:-}"
    if [[ -n "$fd" ]]; then
        exec {fd}>&-
        SSH_FD[$1]=""
    fi
}

# End session $1 from the client side and wait for its process to go.
ssh_close() {
    local pid="${SSH_PID[$1]:-}"
    if [[ -n "$pid" ]]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        SSH_PID[$1]=""
    fi
    _ssh_release_fd "$1"
}

# The geometry banner ssh(1) printed on session $1's stderr: how many times.
ssh_banner_count() {
    local n
    n=$(tr -d '\r' < "$TEST_TMPDIR/$1.err" | grep -acxF -- "$GEOM" || true)
    echo "${n:-0}"
}

# ---------------------------------------------------------------------------
# ssh-peer.py: a password session whose every packet the test chooses
# ---------------------------------------------------------------------------

# The hex of a string's bytes, as printf %b reads it.
hexof() { printf '%b' "$1" | od -An -tx1 -v | tr -d ' \n'; }

# A CHANNEL_DATA body for the confirmed channel (`@`): the recipient
# placeholder, then the data as an SSH string.
peer_data() {
    local hex len
    hex=$(hexof "$1")
    len=$(( ${#hex} / 2 ))
    printf '@00000000%08x%s' "$len" "$hex"
}

# A CHANNEL_REQUEST body: recipient placeholder, name, want_reply, no data.
peer_req() {
    local hex
    hex=$(hexof "$1")
    printf '@00000000%08x%s%02x' "$(( ${#hex} / 2 ))" "$hex" "$2"
}

# The session-open body (sender 0) with window $1, maximum packet 32768.
peer_open() {
    python3 "$PEER" chan-open-hex --sender 0 --window "$1" --maxpkt 32768
}

# A WINDOW_ADJUST body for the confirmed channel adding $1 bytes.
peer_adjust() {
    python3 "$PEER" chan-adjust-hex --recipient @ --bytes "$1"
}

declare -A PEER_PID=()

# peer_start NAME PASSWORD SCRIPT -- ssh-peer.py in the background; its lines
# in peer-NAME.log.
peer_start() {
    python3 "$PEER" login 127.0.0.1 "$HOST_PORT" --user anyone --password "$2" \
        --script "$3" > "$TEST_TMPDIR/peer-$1.log" 2>&1 &
    PEER_PID[$1]=$!
}
peer_log() { echo "$TEST_TMPDIR/peer-$1.log"; }
peer_has() { grep -qxF -- "$2" "$TEST_TMPDIR/peer-$1.log" 2>/dev/null; }
wait_peer_line() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        peer_has "$1" "$2" && return 0
        sleep 0.2
    done
    return 1
}
peer_stop() {
    local pid="${PEER_PID[$1]:-}"
    if [[ -n "$pid" ]]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        PEER_PID[$1]=""
    fi
}

# What a peer's `expectdata` received, as ONE string in ssh-peer.py's escaped
# form (`\xHH`, `\\`, `\"`, `\n`, `\r`, `\t`, printable ASCII as itself).
# Escaping is per byte, so the messages' strings joined are the escape of
# the joined bytes: a sequence split across messages is whole here.
peer_stream() {
    sed -nE 's/^peer: data out "(.*)"$/\1/p' "$TEST_TMPDIR/peer-$1.log" | tr -d '\n'
}

# What a peer's `expectdata` received, as the raw bytes, into file $2. In the
# escaped form a double quote is always `\"` and a backslash `\\`, so
# turning `\"` back into `"` first leaves only escapes printf %b reads.
peer_save() {
    peer_stream "$1" | sed 's/\\"/"/g' | { IFS= read -r -d '' x || true; printf '%b' "$x"; } > "$2"
}

# The byte length of an escaped string (each escape is one byte). Left to
# right, so an escaped backslash is consumed before what follows it.
esc_len() {
    local s
    s=$(sed -E 's/\\(x[0-9a-f]{2}|.)/./g' <<< "$1")
    echo "${#s}"
}

# ---------------------------------------------------------------------------
# Teardown
# ---------------------------------------------------------------------------

_sshd_cleanup() {
    local n
    for n in "${!SSH_PID[@]}"; do
        [[ -n "${SSH_PID[$n]}" ]] && kill "${SSH_PID[$n]}" 2>/dev/null
    done
    for n in "${!PEER_PID[@]}"; do
        [[ -n "${PEER_PID[$n]}" ]] && kill "${PEER_PID[$n]}" 2>/dev/null
    done
    # SSHD_KEEP=<dir> keeps the transcripts and the serial log: an assertion
    # on what a session receives is pinned from a real one. Named files only
    # -- the scratch dir holds FIFOs, which a copy would block on.
    if [[ -n "${SSHD_KEEP:-}" && -n "${TEST_TMPDIR:-}" ]]; then
        mkdir -p "$SSHD_KEEP"
        find "$TEST_TMPDIR" -maxdepth 1 -type f \
            \( -name '*.out' -o -name '*.err' -o -name '*.log' -o -name '*.mark' \) \
            -exec cp -f {} "$SSHD_KEEP"/ \; 2>/dev/null
        cp -f "$TEST_LOG" "$SSHD_KEEP"/ 2>/dev/null
    fi
    test_cleanup
}

# Power the guest off from its Shell and wait for QEMU to go.
sshd_poweroff() {
    local i
    send 'reset -s\r'
    for ((i = 0; i < 100; i++)); do
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || break
        sleep 0.2
    done
}

# How many lines of the whole serial log report a heap corruption.
heap_corruptions() {
    local n
    n=$(grep -acE "fence corrupt|use-after-free write|double free of" "$TEST_LOG" || true)
    echo "${n:-0}"
}

# How many CPU exceptions the firmware reported (x64 and AARCH64 word their
# banners differently).
cpu_exceptions() {
    local n
    n=$(grep -acE "Exception Type|Synchronous Exception" "$TEST_LOG" || true)
    echo "${n:-0}"
}
