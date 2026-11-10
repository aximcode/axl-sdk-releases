#!/bin/bash
# test-meta: arch=none needs=setpriv est=8 local-only=0 guest=none
# guest=none: harness test: a `-M none` QEMU child of test_run_background,
#   killed with its shell; no UEFI guest.
# test-run-background-pdeathsig.sh -- a QEMU guest started by
# test_run_background dies with the test shell that started it, even when
# that shell is SIGKILLed.
#
# test_run_background runs QEMU as the test shell's direct child, with no
# `timeout` watchdog. A test that is SIGKILLed (the runner's own per-test
# limit escalating, an OOM kill, a user's kill -9) runs no EXIT trap, so
# test_cleanup never kills the guest: it survives at ppid 1, holding its
# host ports and /dev/shm memory, and every later test competes with it.
# run-qemu.sh closed the same leak with PR_SET_PDEATHSIG (setpriv
# --pdeathsig KILL), which survives the exec into QEMU; test_run_background
# now takes the same prefix from the same helper (axl_pdeath_init).
#
#   pdeath    a child shell runs test_run_background over an idle QEMU
#             (-M none -S: a real QEMU process, no firmware) and is SIGKILLed;
#             the QEMU must be gone within 2 s
#   control   the same QEMU started by a plain `cmd &` in a child shell that
#             is SIGKILLed SURVIVES the 2 s -- so "gone" above is the death
#             signal's doing, and the check can see a survivor at all
#
# Host-only: no guest boots, nothing is built. Every QEMU this starts is
# killed by PID on exit.
#
# Usage: ./test/integration/test-run-background-pdeathsig.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

WORK="$(mktemp -d -t axl-pdeath.XXXXXXXX)"
QPIDS=()
cleanup() {
    local p
    for p in "${QPIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# shellcheck source=scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"
QEMU=$(find_qemu X64) || { echo "  FAIL: no qemu-system-x86_64"; exit 1; }
if ! setpriv --pdeathsig KILL true >/dev/null 2>&1; then
    # CI's runner has util-linux >= 2.33; a dev box without it gets the
    # fallback (no prefix), which this test cannot exercise.
    if [[ "${CI:-}" == true || "${GITHUB_ACTIONS:-}" == true ]]; then
        echo "  FAIL: setpriv --pdeathsig is unavailable (CI must run this test)"
        exit 1
    fi
    echo "SKIP: setpriv --pdeathsig is unavailable on this host"
    exit 77
fi
IDLE_QEMU=("$QEMU" -M none -display none -S -monitor none -serial none -nodefaults)

# run_child <name> <body> -- run a child bash that writes the QEMU's PID to
# $WORK/<name>.qpid and then sleeps; SIGKILL it once the PID is written, and
# print the QEMU PID. The body starts QEMU and sets QPID.
run_child() {
    local name="$1" body="$2" child i qpid=""
    cat > "$WORK/$name.sh" << EOF
source "$PROJECT_DIR/test/integration/common-test.sh"
set +e
CPU_WARN=false
CPU_REPORT=false
TEST_TMPDIR="$WORK/$name.d"
TEST_LOG="$WORK/$name.log"
mkdir -p "\$TEST_TMPDIR"
TEST_QEMU_CMD=($(printf '%q ' "${IDLE_QEMU[@]}"))
$body
echo "\$QPID" > "$WORK/$name.qpid"
sleep 600
EOF
    bash "$WORK/$name.sh" > "$WORK/$name.out" 2>&1 &
    child=$!
    for i in $(seq 1 100); do
        [[ -s "$WORK/$name.qpid" ]] && break
        sleep 0.05
    done
    qpid=$(cat "$WORK/$name.qpid" 2>/dev/null)
    kill -9 "$child" 2>/dev/null
    wait "$child" 2>/dev/null
    echo "$qpid"
}

# gone_within <pid> <tenths> -- true if pid is gone within that many 0.1 s.
gone_within() {
    local i
    for i in $(seq 1 "$2"); do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
    done
    ! kill -0 "$1" 2>/dev/null
}

is_qemu() { [[ "$(cat "/proc/$1/comm" 2>/dev/null)" == qemu-system-* ]]; }

echo "=== pdeath: test_run_background, its shell SIGKILLed ==="
QP=$(run_child pdeath 'test_run_background; QPID=$TEST_QEMU_PID; sleep 0.3; [[ "$(cat /proc/$QPID/comm 2>/dev/null)" == qemu-system-* ]] || QPID=dead')
if [[ "$QP" == dead ]]; then
    # A QEMU that never got going is gone after the kill for the wrong reason.
    fail "test_run_background's QEMU was not running before the kill -- the check below would prove nothing"
    sed 's/^/      /' "$WORK/pdeath.log" "$WORK/pdeath.out" 2>/dev/null | head -8
elif [[ -n "$QP" ]]; then
    QPIDS+=("$QP")
    if gone_within "$QP" 20; then
        pass "the guest ($QP) is gone within 2 s of its test shell's SIGKILL"
    else
        fail "the guest ($QP, $(cat "/proc/$QP/comm" 2>/dev/null)) outlived its SIGKILLed test shell"
    fi
else
    fail "test_run_background reported no QEMU PID"
    sed 's/^/      /' "$WORK/pdeath.out" | head -8
fi

echo "=== control: a plain background QEMU, its shell SIGKILLed ==="
QC=$(run_child control '"${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 & QPID=$!')
if [[ -n "$QC" ]]; then
    QPIDS+=("$QC")
    sleep 0.3
    if is_qemu "$QC" && ! gone_within "$QC" 20; then
        pass "control: a plain background QEMU ($QC) survives its shell's SIGKILL"
    else
        fail "control: the plain QEMU ($QC) died too -- the check above cannot see a survivor"
    fi
else
    fail "control: no QEMU PID"
fi

echo
echo "run-background-pdeathsig: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 && $PASS -gt 0 ]]
