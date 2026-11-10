#!/bin/bash
# test-meta: arch=both needs=ssh est=60 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-transport-qemu.sh — AxlSsh P1 transport, graded by a real ssh(1).
#
# WHY ssh(1) AND NOT A CLIENT OF OUR OWN: most AXL protocols never get a second
# implementation to test against, so a codec bug that is self-consistent passes
# both ends of our own tests. SSH has OpenSSH. An exchange hash we compute
# wrongly, a KDF letter swapped, or the two ChaCha20 keys in the wrong order
# all round-trip perfectly against ourselves and fail HERE — which is the whole
# reason this test exists and why the unit tests alone do not close P1.
#
# P1 ENDS AT THE AUTH BOUNDARY. The server completes key exchange, switches on
# the cipher, accepts the service request, and then refuses every
# authentication method. So `ssh` is EXPECTED to end in "Permission denied";
# the PASS condition is "got far enough to be refused", not "logged in".
# Authentication is P2.
#
# The PROTOCOL assertions read ssh's own -vvv transcript, never the guest's
# log: a guest that graded its own wire protocol would prove nothing about
# what reaches a peer. The one guest-log check is conn_drop's re-entry
# tripwire (ssh_check_no_reentry, lib/ssh-guest-lib.sh), which is about the
# server's teardown, not the wire.
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1) — this is an integration
# test, not a unit count.
#
# Usage: ./test/integration/test-ssh-transport-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/ssh-guest-lib.sh"

test_parse_args "$@"

if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "SKIP: no ssh(1)/ssh-keygen on the host — OpenSSH is this test's oracle"
    exit 77
fi

test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=2222
# The guest quits this long after it starts listening. One ssh handshake is
# well under a second; the budget is for a loaded box, and a server that stops
# answering fails on ssh's own ConnectTimeout long before this.
SERVER_DEADLINE_MS=60000

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all ssh-server-selftest 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/ssh-server-selftest.efi"

# listen() refuses an empty key set as of P2, so stage one. The private half
# is discarded immediately: this test still grades "reached userauth and was
# refused", and ssh is never given a key that matches.
ssh-keygen -q -t ed25519 -N '' -C 'axl-transport-unused' \
    -f "$TEST_TMPDIR/unused" </dev/null
{
    echo "# generated per run by test-ssh-transport-qemu.sh"
    cat "$TEST_TMPDIR/unused.pub"
} > "$TEST_STAGING/authorized_keys"
rm -f "$TEST_TMPDIR/unused"

# The startup commands live in test-ssh-transport-qemu.nsh; it names the staged
# file relatively (booted: fs0:\, pooled: the lease's share directory).
test_use_nsh "$GUEST_PORT" "$SERVER_DEADLINE_MS"

test_build_image

echo "=== AxlSsh P1 transport vs $(ssh -V 2>&1) ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
trap 'test_cleanup' EXIT

echo "  QEMU PID: $TEST_QEMU_PID, host :$HOST_PORT -> guest :$GUEST_PORT"

if ! test_wait_boot "ssh: listening" 90; then
    echo "FAIL: SSH server did not start listening within ${TEST_WAITED_S} seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Server is listening"

SSH_LOG="$TEST_TMPDIR/ssh.log"

# BatchMode so it never prompts; StrictHostKeyChecking=no + /dev/null known
# hosts because the host key is regenerated every boot (persistence is P4).
# The command is expected to FAIL — we are grading the transcript, not the
# exit status — so the failure is swallowed here deliberately.
ssh -vvv -p "$HOST_PORT" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o BatchMode=yes \
    -o ConnectTimeout=20 \
    -o PreferredAuthentications=publickey \
    root@127.0.0.1 true > "$SSH_LOG" 2>&1 || true

pass=0; fail=0
# common-test.sh installs `set -euo pipefail`, so a bare `grep -q ...` followed
# by `check "$?"` NEVER REACHES check: the failing grep kills the script and
# the run reports no FAIL line at all, only a non-zero exit. That is a test
# that can only ever print passes -- caught here by a sabotage that was
# "detected" for the wrong reason. So the grep goes INSIDE the condition,
# where a non-zero status is a value rather than a fatal error.
check() {
    local name="$1"; shift
    if "$@"; then
        echo "PASS: $name"; pass=$((pass+1))
    else
        echo "FAIL: $name"; fail=$((fail+1))
    fi
}
in_log() { grep -q "$1" "$SSH_LOG"; }
in_log_re() { grep -qE "$1" "$SSH_LOG"; }

check "version exchange: ssh(1) read our identification line" \
    in_log "remote software version AxlSsh_1.0"

check "KEXINIT: curve25519-sha256 negotiated" \
    in_log "kex: algorithm: curve25519-sha256"

check "KEXINIT: ssh-ed25519 host key negotiated" \
    in_log "kex: host key algorithm: ssh-ed25519"

# STRICT KEY EXCHANGE (OpenSSH PROTOCOL section 1.10; Terrapin,
# CVE-2023-48795). ssh(1) enables it only when the SERVER's kex_algorithms
# carries kex-strict-s-v00@openssh.com, and says so in exactly this line --
# captured verbatim from an OpenSSH 9.9p1 -vvv transcript, first against this
# box's own sshd and then against this guest. It is a debug3 line, which is
# why this file runs ssh with -vvv.
#
# This is the ANNOUNCEMENT half. The ENFORCEMENT half -- both sequence
# numbers reset to zero at NEWKEYS -- is graded by every assertion below it:
# ssh resets its own counters when it sees the server offer the name, so a
# server that announces strict KEX and does not reset desynchronises the AEAD
# nonce on the very first encrypted packet and never reaches SERVICE_ACCEPT.
check "strict KEX: ssh(1) enabled it against our KEXINIT" \
    in_log "kex_choose_conf: will use strict KEX ordering"

check "KEXINIT: chacha20-poly1305@openssh.com negotiated" \
    in_log_re "kex: (server->client|client->server) cipher: chacha20-poly1305@openssh.com"

# "Server host key:" is printed when ssh PARSES K_S, before it verifies
# anything -- an earlier version of this file claimed it graded the exchange
# hash, and a sabotage that swapped Q_C and Q_S in H left it passing. What
# actually grades H is the line below it: ssh only reaches NEWKEYS *received*
# after verifying our Ed25519 signature over the H it computed itself from the
# same eight fields, so a one-byte difference stops the handshake there.
check "host key offered: ssh(1) parsed our ssh-ed25519 K_S" \
    in_log "Server host key: ssh-ed25519"

check "key exchange completed: NEWKEYS sent" \
    in_log "SSH2_MSG_NEWKEYS sent"

check "exchange hash verified: ssh(1) checked our signature over H, then NEWKEYS" \
    in_log "SSH2_MSG_NEWKEYS received"

# Everything past here is ENCRYPTED under chacha20-poly1305@openssh.com, so
# reaching it proves the cipher works in BOTH directions: ssh sealed the
# service request, we opened it, we sealed the accept, ssh opened it.
check "encrypted transport works: SERVICE_ACCEPT received" \
    in_log "SSH2_MSG_SERVICE_ACCEPT received"

# P1's boundary: refused, cleanly, rather than hung or reset.
check "reached the userauth boundary and was refused cleanly" \
    in_log_re "Authentications that can continue|Permission denied"

# Graded on the log as it stands after the last handshake: this suite does
# not wait for SERVER: DONE (its fixture deadline is a fixed guard, and
# waiting it out would roughly double the run), so a drop at the deadline
# or in axl_ssh_server_free is NOT covered here. The hostile-lib suites and
# policy wait for DONE and cover it.
ssh_check_no_reentry "through the last handshake (shutdown not graded)"

if [[ "$fail" -ne 0 ]]; then
    echo "--- ssh -vvv transcript (tail) ---"
    tail -40 "$SSH_LOG" | sed 's/^/      /'
    test_clean_log
    echo "--- guest serial log (tail) ---"
    tail -20 "$TEST_CLEAN_LOG" | sed 's/^/      /'
fi

echo
echo "ssh transport: $pass passed, $fail failed ($TEST_ARCH)"
[[ "$fail" -eq 0 ]]
