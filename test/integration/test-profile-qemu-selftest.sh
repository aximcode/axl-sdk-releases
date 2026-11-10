#!/bin/bash
# test-profile-qemu-selftest.sh -- host-only checks (no QEMU) for the CPU
# sampling helpers behind scripts/profile-qemu.sh: qemu_real_pid and
# proc_cpu_cores in scripts/axl-common.sh.
#
# THE BUG THEY FIX. `run-qemu.sh --background` prints QEMU_PID=<the `timeout`
# wrapper>, not qemu's. profile-qemu.sh sampled that pid's /proc/<pid>/stat, so
# the "is the app spinning?" verdict read ~0 cores for every guest. The stand-in
# for the guest here is `bash -c 'while :; do :; done'` under the same
# `( timeout ... ) &` shape run-qemu.sh uses, so the structure is the real one.
#
# CONTROLS. A sampler that only ever says "idle" passes a test that only feeds
# it idle. So: the busy guest must read > 0.5 through the real pid; the SAME
# guest read through the wrapper must stay < 0.1 (proves the control reproduces
# the old bug, i.e. the wrapper really is a blind spot); an idle child must read
# < 0.1 (the detector can say idle); and the failure paths must fail loudly
# rather than print 0.00.
# Excluded from discovery by name (*-selftest.sh); run by run-selftests.sh.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
PROJECT_DIR="$(cd ../.. && pwd)"
# shellcheck source=../../scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"

fail=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; fail=$((fail + 1)); }
over()  { awk -v v="$1" -v b="$2" 'BEGIN { exit !(v + 0 > b + 0) }'; }
under() { awk -v v="$1" -v b="$2" 'BEGIN { exit !(v + 0 < b + 0) }'; }

work=$(mktemp -d)
# The stand-in guest is a COPY of bash named qemu-system-stub: qemu_real_pid
# accepts only a child whose comm is qemu-system*, and a copy is the cheapest
# thing that has one.
cp "$(command -v bash)" "$work/qemu-system-stub"
PIDS=()
GUESTS=()   # the timeout/guest/leaf pids, which must be dead after cleanup
# Every pid this test starts is killed AND reaped: the subshell wrapper, the
# `timeout` and the guest. Killing only the subshell (as this once did) leaves
# `timeout` and a 100%-busy loop alive until the 30 s timeout fires.
cleanup() {
    local p
    for p in ${PIDS[@]+"${PIDS[@]}"}; do kill "$p" 2>/dev/null; done
    for p in ${PIDS[@]+"${PIDS[@]}"}; do wait "$p" 2>/dev/null; done
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# launch <cmd...>: the run-qemu.sh shape. Sets WRAP = the subshell pid (what
# run-qemu falls back to when its pgrep misses), TPID = the `timeout` pid (what
# --background normally calls QEMU_PID), and, once resolvable, the guest.
launch() {
    ( timeout 30 "$@" >/dev/null 2>&1 < /dev/null || : ) &
    WRAP=$!
    PIDS+=("$WRAP")
    TPID=$(qemu_child_pid "$WRAP")
    PIDS+=("$TPID"); GUESTS+=("$TPID")
    local g; g=$(qemu_child_pid "$TPID")
    if [[ -n "$g" ]]; then PIDS+=("$g"); GUESTS+=("$g"); fi
}

echo "=== profile-qemu CPU sampling self-test ==="

launch "$work/qemu-system-stub" -c 'while :; do :; done; :'
[[ -n "$TPID" ]] || bad "busy control: no timeout pid found"
REAL=$(qemu_real_pid "$TPID"); rc=$?
if [[ $rc -eq 0 && -n "$REAL" && "$REAL" != "$TPID" ]]; then
    pass "qemu_real_pid resolves the child, not the timeout wrapper"
else
    bad "qemu_real_pid returned '$REAL' rc=$rc for wrapper $TPID"
fi
# run-qemu's own fallback hands over the SUBSHELL pid when its pgrep misses; the
# helper must descend through `timeout` to the same guest, not stop on it.
REAL2=$(qemu_real_pid "$WRAP"); rc=$?
if [[ $rc -eq 0 && "$REAL2" == "$REAL" ]]; then
    pass "the wrapper-of-wrapper shape (run-qemu's fallback pid) resolves to the same guest"
else
    bad "qemu_real_pid on the subshell pid gave '$REAL2' rc=$rc, expected $REAL"
fi
cores=$(proc_cpu_cores "$REAL" 1)
if over "$cores" 0.5; then
    pass "busy guest through the real pid reads $cores cores (> 0.5)"
else
    bad "busy guest through the real pid read '$cores', expected > 0.5"
fi
wcores=$(proc_cpu_cores "$TPID" 1)
if under "$wcores" 0.1; then
    pass "CONTROL: same guest through the wrapper reads $wcores (the old bug)"
else
    bad "CONTROL: the wrapper read '$wcores' -- the control no longer reproduces the bug"
fi

launch "$work/qemu-system-stub" -c 'sleep 20; :'
IREAL=$(qemu_real_pid "$TPID")
icores=$(proc_cpu_cores "$IREAL" 1)
if under "$icores" 0.1; then
    pass "CONTROL: idle guest reads $icores cores (< 0.1)"
else
    bad "CONTROL: idle guest read '$icores'"
fi

# The pdeathsig prefix (setpriv) execs into qemu in place: for a moment the
# child's comm is `setpriv`. The helper must wait it out, not refuse. The stand-in
# is a copy of bash named setpriv that sleeps 0.6 s and then execs the guest.
cp "$(command -v bash)" "$work/setpriv"
launch "$work/setpriv" -c "sleep 0.6; exec \"$work/qemu-system-stub\" -c 'sleep 20; :'"
out=$(qemu_real_pid "$TPID" 2>&1); rc=$?
if [[ $rc -eq 0 && "$(cat "/proc/$out/comm" 2>/dev/null)" == qemu-system* ]]; then
    pass "a child still named setpriv is waited for until it execs into qemu"
else
    bad "a child mid-exec (setpriv) was refused or mis-resolved: rc=$rc out='$out'"
fi

# A child that is not qemu is REFUSED. Without the comm check the helper
# returned whatever hung off the wrapper, which for run-qemu's fallback path is
# `timeout` -- the original blind spot again, now with a plausible-looking pid.
launch bash -c 'sleep 20; :'
out=$(qemu_real_pid "$TPID" 2>&1); rc=$?
if [[ $rc -ne 0 && "$out" == *"not qemu-system*"* ]]; then
    pass "CONTROL: a non-qemu child is refused, not returned"
else
    bad "a non-qemu child was accepted or silently mis-reported: rc=$rc out='$out'"
fi

# Failure paths: loud, and no number on stdout. The busy loop is a leaf.
sleep 20 &
LEAF=$!
PIDS+=("$LEAF"); GUESTS+=("$LEAF")
out=$(qemu_real_pid "$LEAF" 2>&1); rc=$?
if [[ $rc -ne 0 && "$out" == *"no child process"* ]]; then
    pass "qemu_real_pid fails loudly when the pid has no child"
else
    bad "qemu_real_pid on a childless pid: rc=$rc out='$out'"
fi
dead=$(bash -c 'echo $$')
out=$(proc_cpu_cores "$dead" 1 2>/dev/null); rc=$?
if [[ $rc -ne 0 && -z "$out" ]]; then
    pass "proc_cpu_cores on a dead pid fails and prints nothing"
else
    bad "proc_cpu_cores on a dead pid: rc=$rc out='$out'"
fi

# Cleanup really stops everything it started. The busy loop under `timeout` was
# orphaned for the rest of its 30 s when only the subshell was killed. Polled,
# since the guests are grandchildren this shell cannot wait on.
cleanup
alive=""
for ((i = 0; i < 30; i++)); do
    alive=""
    for p in "${GUESTS[@]}"; do kill -0 "$p" 2>/dev/null && alive="$alive $p"; done
    [[ -z "$alive" ]] && break
    sleep 0.1
done
if [[ -z "$alive" ]]; then
    pass "cleanup leaves no timeout/guest process behind (${#GUESTS[@]} checked)"
else
    bad "cleanup leaked running pid(s):$alive"
fi

# Wiring: the script must use the helpers, not sample QEMU_PID directly.
if grep -q 'qemu_real_pid "\$QEMU_PID"' "$PROJECT_DIR/scripts/profile-qemu.sh"; then
    pass "profile-qemu.sh samples via qemu_real_pid"
else
    bad "profile-qemu.sh does not call qemu_real_pid"
fi
if grep -q 'proc_cpu_cores "\$QEMU_PID"' "$PROJECT_DIR/scripts/profile-qemu.sh"; then
    bad "profile-qemu.sh samples the timeout wrapper again"
fi

if [[ $fail -eq 0 ]]; then echo "PASS"; exit 0; fi
echo "FAIL: $fail"
exit 1
