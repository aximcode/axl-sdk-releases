#!/bin/bash
# test-meta: arch=x64 needs=ssh est=30 local-only=0 builds=ARCH=x64,sshd guest=none
# guest=none: scripts/ssh-demo.sh, the launcher a human uses, is the
#   subject: its --check boots its own QEMU through run-qemu.sh
#   --background.
# test-ssh-demo-script.sh -- scripts/ssh-demo.sh, the launcher a human uses to
# try the `sshd` tool, and its --check mode.
#
# Scenarios:
#   bad-args      an unknown option, a bad --arch and a bad --port each exit 2
#                 and say what was wrong
#   bad-key       --key with no such file exits 1 and names the key
#   port-busy     a host port something already listens on is refused (exit 1)
#                 with the message that suggests --port, before any boot
#   check         `--check` boots QEMU, holds a real `ssh -tt` session, runs
#                 an echo through a Shell variable and exits 0 printing its
#                 `--check ok` line; run with HOME pointed at an empty dir,
#                 it never creates ~/.ssh (the throwaway key lives in a temp
#                 dir), and it leaves its host port free (QEMU is gone)
#
# Only x64 is exercised here; --arch aa64 is the same script with the other
# image.
#
# Usage: ./test/integration/test-ssh-demo-script.sh

export TEST_SKIP_RATCHET=1

if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "SKIP: no ssh(1)/ssh-keygen(1) on the host"
    exit 77
fi

source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
test_setup

# --check builds nothing, so the build is this test's.
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 all sshd 2>&1 | tail -3 \
   || [[ ! -f "$(test_build_dir X64)/tools/sshd.efi" ]]; then
    echo "FAIL: sshd.efi was not built"
    exit 1
fi

DEMO="$PROJECT_DIR/scripts/ssh-demo.sh"
PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local label="$1"
    shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}

# run_demo OUTFILE args... : run the launcher, stdout+stderr to OUTFILE, set RC.
run_demo() {
    local out="$1"
    shift
    RC=0
    "$DEMO" "$@" > "$out" 2>&1 < /dev/null || RC=$?
}
out_has() { grep -aqF -- "$2" "$1"; }

OUT="$TEST_TMPDIR/out.txt"
ssh-keygen -q -t ed25519 -N '' -C ssh-demo-test -f "$TEST_TMPDIR/key" < /dev/null

echo "=== ssh-demo.sh ==="

# --- bad-args ---------------------------------------------------------------
run_demo "$OUT" --frobnicate
check "bad-args: an unknown option exits 2" test "$RC" = 2
check "bad-args: ... and names it" out_has "$OUT" "unknown option: --frobnicate"
run_demo "$OUT" --arch riscv
check "bad-args: a bad --arch exits 2" test "$RC" = 2
check "bad-args: ... and names it" out_has "$OUT" "--arch must be x64 or aa64, not 'riscv'"
run_demo "$OUT" --port abc
check "bad-args: a bad --port exits 2" test "$RC" = 2
check "bad-args: ... and names it" out_has "$OUT" "--port must be 1-65535, not 'abc'"

# --- bad-key ----------------------------------------------------------------
run_demo "$OUT" --key "$TEST_TMPDIR/no-such-key"
check "bad-key: a missing key exits 1" test "$RC" = 1
check "bad-key: ... and names it" out_has "$OUT" "$TEST_TMPDIR/no-such-key.pub is not a readable ssh-ed25519 public key"

# --- port-busy --------------------------------------------------------------
BUSY=$(test_port 0)
python3 -m http.server "$BUSY" --bind 127.0.0.1 > /dev/null 2>&1 &
LISTENER=$!
# `|| true` is load-bearing: common-test.sh runs under `set -e`, which also
# governs an EXIT trap, so a kill that fails (the listener already gone) ends
# the script with status 1 AFTER its last line and skips test_cleanup.
trap 'kill "$LISTENER" 2>/dev/null || true; test_cleanup' EXIT
for _ in $(seq 1 50); do
    axl_port_in_use "$BUSY" && break
    sleep 0.1
done
run_demo "$OUT" --port "$BUSY" --key "$TEST_TMPDIR/key"
check "port-busy: a busy host port exits 1" test "$RC" = 1
check "port-busy: ... says so and suggests --port" \
    out_has "$OUT" "host port $BUSY is already in use; pick another with --port (e.g. --port $((BUSY + 1)))"
kill "$LISTENER" 2>/dev/null || true
wait "$LISTENER" 2>/dev/null || true
trap 'test_cleanup' EXIT

# --- dry-run ----------------------------------------------------------------
# The lines a human copies. Both ssh lines share one option set: a known_hosts
# write would break the NEXT run (a fresh host key every boot), and an agent
# offering many keys would hit "Too many authentication failures".
DRY_PORT=$(test_port 1)
HOSTOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes"
run_demo "$OUT" --dry-run --port "$DRY_PORT" --key "$TEST_TMPDIR/key" --password pw1
check "dry-run: exits 0" test "$RC" = 0
check "dry-run: the key line, host-key options and -i" \
    grep -qxF -- "  ssh -tt -p $DRY_PORT $HOSTOPTS -i $TEST_TMPDIR/key user@localhost" "$OUT"
check "dry-run: the password line, the same options without -i" \
    grep -qxF -- "  (or log in with the password 'pw1': ssh -tt -p $DRY_PORT $HOSTOPTS user@localhost)" "$OUT"
check "dry-run: says how to disconnect" \
    grep -qxF -- "Disconnect: Enter, then ~.   ('exit' ends the Shell for every session)" "$OUT"
check "dry-run: x64 boot wait" \
    grep -qxF -- "Wait for the guest's 'sshd: serving the UEFI Shell' line first (~10 s)." "$OUT"
check "dry-run: the guest console repeats the disconnect line" \
    grep -qxF -- "echo \"  Disconnect: Enter, then ~.   ('exit' ends the Shell for every session)\"" "$OUT"
run_demo "$OUT" --dry-run --arch aa64 --port "$DRY_PORT" --key "$TEST_TMPDIR/key"
check "dry-run: aa64 boot wait is longer" \
    grep -qxF -- "Wait for the guest's 'sshd: serving the UEFI Shell' line first (~30 s)." "$OUT"
check "dry-run: no password line without --password" \
    bash -c "! grep -qF 'or log in with the password' '$OUT'"

# --- check ------------------------------------------------------------------
mkdir "$TEST_TMPDIR/home"
RC=0
HOME="$TEST_TMPDIR/home" "$DEMO" --check > "$OUT" 2>&1 < /dev/null || RC=$?
check "check: exits 0" test "$RC" = 0
check "check: prints its ok line with a marker" \
    grep -aqE '^ssh-demo: --check ok \(x64, port [0-9]+, marker demo-[0-9a-f]{8}\)$' "$OUT"
check "check: never created ~/.ssh" test ! -e "$TEST_TMPDIR/home/.ssh"
CHECK_PORT=$(sed -nE 's/^ssh-demo: --check ok \(x64, port ([0-9]+),.*/\1/p' "$OUT")
check "check: its host port is free again (QEMU is gone)" \
    bash -c "source '$PROJECT_DIR/scripts/axl-common.sh'; sleep 1; ! axl_port_in_use '${CHECK_PORT:-0}'"

echo ""
printf "ssh-demo: %d passed, %d failed\n" "$PASS" "$FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo "--- last output ---"; tail -40 "$OUT"
fi
[[ $FAIL -eq 0 && $PASS -eq 22 ]] && exit 0 || exit 1
