#!/bin/bash
# axl-desc: try the `sshd` tool -- the live UEFI Shell over SSH, in QEMU
# ssh-demo.sh -- boot QEMU to the UEFI Shell with `sshd` running, and tell you
# the exact ssh(1) line that reaches it.
#
# Usage: scripts/ssh-demo.sh [--arch x64|aa64] [--port 2222] [--key PATH]
#                            [--password PW] [--check] [--dry-run]
#
#   --arch ARCH     x64 (default) or aa64
#   --port PORT     host port forwarded to the guest's sshd (default 2222)
#   --key PATH      the key that may log in: a public key (.pub) or its
#                   private half. Default ~/.ssh/id_ed25519.pub; if that is
#                   absent (or not ed25519 -- the only type sshd accepts) a
#                   throwaway key is made in a temp dir. Nothing is ever
#                   written into ~/.ssh.
#   --password PW   also let any user name in with this password (sshd -P);
#                   visible on the guest's command line, a demo convenience
#   --check         no human: boot in the background on a free port with a
#                   throwaway key, hold one `ssh -tt` session, run an `echo`
#                   through a Shell variable, and exit 0 only if its output
#                   came back. Builds nothing (`scripts/axl-build.sh sshd` first).
#   --dry-run       print the ssh line(s) and the guest's startup.nsh, then
#                   exit 0; builds and boots nothing
#
# The serial console of the guest is on your terminal. Quit QEMU with Ctrl-A X.
# Type into the Shell there or over ssh: both reach the same console.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/axl-common.sh"

ARCH=x64
PORT=2222
KEY_ARG=""
PASSWORD=""
CHECK=false
DRY_RUN=false

# The slirp gateway: every host connection reaches the guest from it.
HOST_AS_SEEN=10.0.2.2/32
GUEST_PORT=22

WORK=""
QEMU_PID=""
HOLD_PID=""

usage() {
    awk 'NR > 2 { if (/^#/) { sub(/^# ?/, ""); print } else exit }' "${BASH_SOURCE[0]}"
}

cleanup() {
    if [[ -n "$HOLD_PID" ]]; then kill "$HOLD_PID" 2>/dev/null || true; fi
    # QEMU_PID is the `timeout` wrapper around QEMU; it forwards the signal.
    if [[ -n "$QEMU_PID" ]]; then kill "$QEMU_PID" 2>/dev/null || true; fi
    if [[ -n "$WORK" ]]; then rm -rf "$WORK"; fi
}
trap cleanup EXIT

die() { log_error "$1"; exit "${2:-1}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)     [[ $# -ge 2 ]] || die "--arch needs a value" 2; ARCH="$2"; shift 2 ;;
        --port)     [[ $# -ge 2 ]] || die "--port needs a value" 2; PORT="$2"; shift 2 ;;
        --key)      [[ $# -ge 2 ]] || die "--key needs a value" 2; KEY_ARG="$2"; shift 2 ;;
        --password) [[ $# -ge 2 ]] || die "--password needs a value" 2; PASSWORD="$2"; shift 2 ;;
        --check)    CHECK=true; shift ;;
        --dry-run)  DRY_RUN=true; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          die "unknown option: $1 (try --help)" 2 ;;
    esac
done

$CHECK && $DRY_RUN && die "--check and --dry-run are exclusive" 2
# BOOT_WAIT: how long a human waits for the serving line (aa64 is emulated).
case "$ARCH" in
    x64)  QARCH=X64;     BOOT_WAIT="~10 s" ;;
    aa64) QARCH=AARCH64; BOOT_WAIT="~30 s" ;;
    *)    die "--arch must be x64 or aa64, not '$ARCH'" 2 ;;
esac
[[ "$PORT" =~ ^[0-9]+$ ]] && (( PORT >= 1 && PORT <= 65535 )) \
    || die "--port must be 1-65535, not '$PORT'" 2
# The password goes into a Shell script line: keep it to what survives that.
if [[ -n "$PASSWORD" && ! "$PASSWORD" =~ ^[A-Za-z0-9._@+=:,-]+$ ]]; then
    die "--password may use letters, digits and . _ @ + = : , - only" 2
fi
command -v ssh >/dev/null && command -v ssh-keygen >/dev/null \
    || die "ssh(1) and ssh-keygen(1) are needed on the host"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ssh-demo.XXXXXX")"

# --- Build or locate sshd.efi -------------------------------------------------
PREFIX="$("$SCRIPT_DIR/build-prefix.sh" "$ARCH" --abs)"
SSHD_EFI="$PREFIX/tools/sshd.efi"
if ! $CHECK && ! $DRY_RUN; then
    log_info "building sshd ($ARCH)"
    "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$ARCH" all sshd >"$WORK/make.log" 2>&1 \
        || { tail -20 "$WORK/make.log" >&2; die "the build failed"; }
fi
$DRY_RUN || [[ -f "$SSHD_EFI" ]] \
    || die "$SSHD_EFI is missing (run: scripts/axl-build.sh ARCH=$ARCH all sshd)"

# --- The key ------------------------------------------------------------------
is_ed25519_pub() { [[ -f "$1" ]] && grep -q '^ssh-ed25519 ' "$1"; }

KEY_PRIV=""
KEY_PUB=""
THROWAWAY=false
if ! $CHECK && [[ -n "$KEY_ARG" ]]; then
    if [[ "$KEY_ARG" == *.pub ]]; then
        KEY_PUB="$KEY_ARG"; KEY_PRIV="${KEY_ARG%.pub}"
    else
        KEY_PRIV="$KEY_ARG"; KEY_PUB="$KEY_ARG.pub"
    fi
    is_ed25519_pub "$KEY_PUB" \
        || die "$KEY_PUB is not a readable ssh-ed25519 public key (sshd accepts only ed25519)"
    [[ -f "$KEY_PRIV" ]] || die "the private key $KEY_PRIV does not exist"
elif ! $CHECK && is_ed25519_pub "$HOME/.ssh/id_ed25519.pub" \
        && [[ -f "$HOME/.ssh/id_ed25519" ]]; then
    KEY_PUB="$HOME/.ssh/id_ed25519.pub"; KEY_PRIV="$HOME/.ssh/id_ed25519"
else
    KEY_PRIV="$WORK/demo_key"; KEY_PUB="$KEY_PRIV.pub"; THROWAWAY=true
    ssh-keygen -q -t ed25519 -N '' -C 'ssh-demo' -f "$KEY_PRIV" </dev/null
fi

# --- The host port ------------------------------------------------------------
if $CHECK; then
    axl_alloc_host_port PORT 1 || exit 1
elif axl_port_in_use "$PORT"; then
    die "host port $PORT is already in use; pick another with --port (e.g. --port $((PORT + 1)))"
fi

# ONE option set for every line printed and for --check. The guest makes a
# fresh host key each boot, so a known_hosts entry would make the NEXT run
# fail with REMOTE HOST IDENTIFICATION HAS CHANGED; IdentitiesOnly keeps an
# agent holding many keys from hitting "Too many authentication failures".
# No LogLevel=ERROR: it would hide the pre-auth banner.
SSH_BASE=(-tt -p "$PORT" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes)
SSH_OPTS=("${SSH_BASE[@]}" -i "$KEY_PRIV")
SSH_LINE="ssh ${SSH_OPTS[*]} user@localhost"
SSH_PW_LINE="ssh ${SSH_BASE[*]} user@localhost"
DISCONNECT="Disconnect: Enter, then ~.   ('exit' ends the Shell for every session)"

# --- Stage and boot -----------------------------------------------------------
cp "$KEY_PUB" "$WORK/authorized_keys"
SSHD_CMD="sshd -p $GUEST_PORT -x $HOST_AS_SEEN"
[[ -n "$PASSWORD" ]] && SSHD_CMD+=" -P $PASSWORD"
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo Connecting drivers..."
    echo "connect -r"
    echo "stall 1000000"
    echo "echo Configuring network via DHCP..."
    echo "ifconfig -s eth0 dhcp"
    echo "stall 3000000"
    echo "$SSHD_CMD"
    if ! $CHECK; then
        # run-qemu's interactive screen is an alternate one, which hides what
        # this script printed before boot: say it again on the console itself.
        echo "echo \"==========================================================\""
        echo "echo \"In another terminal:\""
        echo "echo \"  $SSH_LINE\""
        [[ -n "$PASSWORD" ]] && \
            echo "echo \"  or with the password $PASSWORD: $SSH_PW_LINE\""
        echo "echo \"  $DISCONNECT\""
        echo "echo \"Quit QEMU: Ctrl-A then X\""
        echo "echo \"==========================================================\""
    fi
} > "$WORK/startup.nsh"

QEMU_ARGS=(--arch "$QARCH" --net --hostfwd "$PORT:$GUEST_PORT"
           --extra "$WORK/authorized_keys" --nsh "$WORK/startup.nsh")

if ! $CHECK; then
    echo
    echo "Connect from another terminal with:"
    echo "  $SSH_LINE"
    [[ -n "$PASSWORD" ]] && \
        echo "  (or log in with the password '$PASSWORD': $SSH_PW_LINE)"
    $THROWAWAY && echo "  (key: a throwaway one in $WORK, removed when QEMU exits)"
    echo "$DISCONNECT"
    echo "Wait for the guest's 'sshd: serving the UEFI Shell' line first ($BOOT_WAIT)."
    echo "Quit QEMU with Ctrl-A then X."
    echo
    if $DRY_RUN; then
        echo "--- the guest's startup.nsh ---"
        cat "$WORK/startup.nsh"
        exit 0
    fi
    # Not exec: the throwaway key dir is removed when QEMU is gone.
    rc=0
    "$SCRIPT_DIR/run-qemu.sh" "${QEMU_ARGS[@]}" -i "$SSHD_EFI" || rc=$?
    exit "$rc"
fi

# --- --check: one ssh -tt round trip -------------------------------------------
SERIAL="$WORK/serial.log"
OUT="$WORK/session.out"
ERR="$WORK/session.err"
"$SCRIPT_DIR/run-qemu.sh" "${QEMU_ARGS[@]}" --background --timeout 180 \
    --serial-log "$SERIAL" "$SSHD_EFI" >"$WORK/qemu.out" 2>&1 \
    || { cat "$WORK/qemu.out" >&2; die "QEMU did not start"; }
QEMU_PID="$(sed -n 's/^QEMU_PID=//p' "$WORK/qemu.out")"
[[ -n "$QEMU_PID" ]] || { cat "$WORK/qemu.out" >&2; die "run-qemu printed no QEMU_PID"; }

# wait_for SECS DESCRIPTION CMD...: poll, and stop early if QEMU is gone.
wait_for() {
    local secs="$1" what="$2" i
    shift 2
    for ((i = 0; i < secs * 5; i++)); do
        if "$@"; then return 0; fi
        kill -0 "$QEMU_PID" 2>/dev/null \
            || { log_error "QEMU exited while waiting for $what"; return 1; }
        sleep 0.2
    done
    log_error "timed out (${secs}s) waiting for $what"
    return 1
}
fail_with_logs() {
    log_error "$1"
    echo "--- serial log (tail) ---" >&2
    tail -30 "$SERIAL" 2>/dev/null | tr -d '\033' >&2 || true
    echo "--- ssh stderr ---" >&2
    tail -10 "$ERR" 2>/dev/null >&2 || true
    exit 1
}

served() { grep -aqE 'sshd: serving the UEFI Shell on .+ port' "$SERIAL" 2>/dev/null; }
wait_for 120 "the guest's sshd serving line" served \
    || fail_with_logs "sshd never came up"

# The session's output as lines, as the integration suites read it: cursor
# moves are line breaks, other escapes are dropped.
vt_lines() {
    sed -E $'s/\x1b\\[[0-9;]*[Hf]/\\n/g; s/\x1b\\[[0-9;?=]*[A-Za-z]//g; s/\x1b[()][0-9A-Za-z]//g; s/\r/\\n/g' "$1" \
        | sed -E 's/[[:space:]]+$//'
}

MARK="demo-$(od -An -tx1 -N4 /dev/urandom | tr -d ' \n')"
mkfifo "$WORK/in"
# Held read-write so ssh never sees EOF between writes.
exec 9<>"$WORK/in"
ssh "${SSH_OPTS[@]}" -o BatchMode=yes -o ConnectTimeout=20 \
    user@localhost <"$WORK/in" >"$OUT" 2>"$ERR" &
HOLD_PID=$!

has_bytes() { [[ -s "$OUT" ]]; }
wait_for 60 "the session's first bytes" has_bytes \
    || fail_with_logs "the ssh session never opened"
sleep 1
printf 'set -v T demo\r' >&9
sleep 1
printf 'echo %%T%%-%s\r' "${MARK#demo-}" >&9
got_mark() { vt_lines "$OUT" | grep -qxF -- "$MARK"; }
wait_for 30 "the echo's output line" got_mark \
    || fail_with_logs "the marker line '$MARK' never came back through ssh"
grep -q 'sshd: the console is' "$ERR" \
    || fail_with_logs "ssh printed no console-size banner"

echo "ssh-demo: --check ok ($ARCH, port $PORT, marker $MARK)"
