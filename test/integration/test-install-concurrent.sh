#!/bin/bash
# test-meta: arch=none needs= est=48 local-only=0
# test-install-concurrent.sh — two install.sh runs do not build into one tree
# at the same time.
#
# THE DEFECT. install.sh always builds into out/native-<arch>-release (plus
# -tls when AXL_TLS is set), REGARDLESS of --prefix: --prefix says where to
# stage, not where to build. Three integration tests invoke install.sh
# (test-jose-cc-qemu, test-install-idempotent, test-pkg-deps-minimal) and
# run-integration.sh exports AXL_TLS=1 suite-wide, so under -j8 all three
# aim `make -j$(nproc)` at the SAME tree. Two makes writing one target set is
# not safe: the observed symptoms are a partial archive and
# "the input file ... is empty" from objcopy.
#
# It is not theoretical and it is not rare enough to ignore: test-jose-cc
# passed in two suite runs and failed in the third on the same night, on a
# stale prefix, because its install.sh lost that race. Diagnosing it cost
# more than the fix -- the failure surfaced as `undefined reference to
# axl_crypto_rng`, which reads as a library defect.
#
# d8ab47ee split the prefix per AXL_TLS so a TLS run and a non-TLS run stop
# wiping each other. That is a different problem: it made the two
# CONFIGURATIONS independent, and did nothing about two builds of the SAME
# configuration.
#
# WHY A LOCK RATHER THAN A TREE PER CALLER. Giving each install.sh its own
# build tree would be correct and costs a full rebuild per invocation --
# minutes each, several times per suite. The builds are identical work; they
# only must not interleave. Serialising is the cheap correct answer.
#
# WHY THIS TEST IS DETERMINISTIC. Racing two real builds and asserting they
# both survive would pass most of the time whether or not the lock exists --
# a test that only fails sometimes is not a gate. So it asserts the
# MECHANISM: hold the lock, prove install.sh waits; release it, prove
# install.sh proceeds.
#
# Usage: ./test/integration/test-install-concurrent.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

WORK="$(mktemp -d)"
cleanup() {
    [[ -n "${HOLDER_PID:-}" ]] && kill "$HOLDER_PID" 2>/dev/null
    [[ -n "${INST_PID:-}" ]]   && kill "$INST_PID"   2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

command -v flock >/dev/null || {
    # Balanced SKIP count -- see feedback_balancer_count.
    for m in "install.sh names its build-tree lock" \
             "install.sh WAITS while another holds the build-tree lock" \
             "install.sh proceeds once the lock is released"; do
        echo "SKIP: $m (flock not available)"
    done
    echo; echo "install-concurrent: 0 passed, 0 failed, 3 skipped"; exit 0
}

# 1. install.sh must expose the lock path it will take, so a caller (and this
#    test) can reason about it without duplicating the derivation. A lock
#    whose location is implied by reading the source is one this test would
#    have to re-derive, and the two spellings would drift.
#    stdout only: the script's log_* helpers write to stderr, and a
#    machine-readable query is read from stdout. Capturing 2>&1 here would
#    fold the "[INFO] Backend: native" banner into the path.
LOCKPATH="$("$PROJECT_DIR/scripts/install.sh" --print-build-lock --arch x64 2>/dev/null)" && rc=0 || rc=$?
if [[ "$rc" -eq 0 && -n "$LOCKPATH" ]]; then
    pass "install.sh names its build-tree lock"
else
    fail "install.sh --print-build-lock failed (rc=$rc): $LOCKPATH"
    echo; echo "install-concurrent: $PASS passed, $((FAIL+2)) failed"; exit 1
fi

mkdir -p "$(dirname "$LOCKPATH")"

# 1b. A TLS install and a non-TLS install must resolve DIFFERENT trees.
#     install.sh passes PREFIX on make's command line, which OVERRIDES the
#     Makefile's own rule -- so hardcoding it here (as this script did) silently
#     defeated d8ab47ee, the split that gives an AXL_TLS build its own tree.
#     Both configurations landed in out/native-<arch>-release, and because
#     AXL_TLS is in the build-state SIGNATURE, each alternation WIPED the
#     other's objects rather than reusing them.
#
#     Asserted through the lock path because that is derived from the same
#     query the build uses, so this cannot pass while the build disagrees.
#     BOTH states are named explicitly. An earlier version compared the
#     ambient environment against AXL_TLS=1 and passed standalone while
#     FAILING in the suite -- run-integration.sh exports AXL_TLS=1 for every
#     test, so the "non-TLS" side inherited it and both resolved -tls. The
#     property under test is a property of the two configurations, not of
#     whatever the caller happened to export.
notls_lock="$(env -u AXL_TLS "$PROJECT_DIR/scripts/install.sh" \
               --print-build-lock --arch x64 2>/dev/null)"
tls_lock="$(AXL_TLS=1 "$PROJECT_DIR/scripts/install.sh" --print-build-lock \
             --arch x64 2>/dev/null)"
if [[ -n "$tls_lock" && -n "$notls_lock" && "$tls_lock" != "$notls_lock" ]]; then
    pass "TLS and non-TLS installs resolve separate build trees"
else
    fail "TLS and non-TLS installs share a tree ($notls_lock)"
fi

# 2. Hold the lock, then start an install and prove it does NOT get past the
#    build step. `--print-build-lock` returns before locking, so the probe
#    here is a real install invocation.
exec 9>"$LOCKPATH"
flock 9                       # this shell now holds it

: > "$WORK/inst.log"
( "$PROJECT_DIR/scripts/install.sh" --arch x64 --prefix "$WORK/sdk" \
      >"$WORK/inst.log" 2>&1; echo "rc=$?" >>"$WORK/inst.log" ) &
INST_PID=$!

# Give it long enough to reach the build. It cannot finish a build in this
# window even warm, so "still running" here means it is waiting, not racing.
sleep 20
if kill -0 "$INST_PID" 2>/dev/null && ! grep -q '^rc=' "$WORK/inst.log"; then
    pass "install.sh WAITS while another holds the build-tree lock"
else
    fail "install.sh did not wait for the lock"
    tail -5 "$WORK/inst.log" | sed 's/^/      /'
fi

# 3. Release, and it must complete. Proves the wait is a lock and not a hang.
flock -u 9
exec 9>&-

waited=0
while kill -0 "$INST_PID" 2>/dev/null && [[ "$waited" -lt 600 ]]; do
    sleep 5; waited=$((waited + 5))
done
if grep -q '^rc=0' "$WORK/inst.log"; then
    pass "install.sh proceeds once the lock is released"
else
    fail "install.sh did not complete after release (waited ${waited}s)"
    tail -8 "$WORK/inst.log" | sed 's/^/      /'
fi
INST_PID=""

echo
echo "install-concurrent: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
