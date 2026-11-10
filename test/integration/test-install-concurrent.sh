#!/bin/bash
# test-meta: arch=none needs= est=60 local-only=0
# test-install-concurrent.sh — two install.sh runs do not build into one tree
# at the same time.
#
# THE DEFECT. install.sh always builds into build/<arch>-release,
# REGARDLESS of --prefix: --prefix says where to stage, not where to build.
# Three integration tests invoke install.sh (test-jose-cc-qemu,
# test-install-idempotent, test-host-deps-minimal), so under -j8 all three aim
# a parallel build at the SAME tree. There is now exactly ONE tree -- the
# AXL_TLS suffix is gone -- which makes that collision MORE likely, not less.
# Two builds writing one target set is not safe: the observed symptoms are a
# partial archive and "the input file ... is empty" from objcopy.
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
             "install.sh proceeds once the lock is released" \
             "the staged copy happens inside the build-tree lock" \
             "no scratch file is left in the prefix" \
             "a symlinked destination is replaced"; do
        echo "SKIP: $m (flock not available)"
    done
    echo; echo "install-concurrent: 0 passed, 0 failed, 6 skipped"; exit 0
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

# 1b. AXL_TLS must NOT influence the build tree -- INVERTED from what this
#     once asserted, and kept rather than deleted.
#
#     It used to require that a TLS and a non-TLS install resolve DIFFERENT
#     trees: mbedTLS was optional, AXL_TLS was in the build-state signature,
#     and each alternation WIPED the other's objects instead of reusing them.
#     mbedTLS is unconditional now, so there is one tree and one signature.
#
#     The assertion is inverted rather than removed because the failure it
#     guards against is still reachable: a stray AXL_TLS left in someone's
#     environment -- or a prefix rule that grows a suffix again -- would
#     silently re-split the tree and resurrect the wipe-and-rebuild cycle this
#     whole file exists to prevent.
#
#     Asserted through the lock path because that is derived from the same
#     query the build uses, so it cannot pass while the build disagrees. BOTH
#     states are still named explicitly: an earlier version compared the
#     ambient environment against AXL_TLS=1 and passed standalone while FAILING
#     in the suite, because run-integration.sh exported AXL_TLS=1 for every
#     test and the "non-TLS" side inherited it.
notls_lock="$(env -u AXL_TLS "$PROJECT_DIR/scripts/install.sh" \
               --print-build-lock --arch x64 2>/dev/null)"
tls_lock="$(AXL_TLS=1 "$PROJECT_DIR/scripts/install.sh" --print-build-lock \
             --arch x64 2>/dev/null)"
if [[ -n "$tls_lock" && -n "$notls_lock" && "$tls_lock" == "$notls_lock" ]]; then
    pass "a stray AXL_TLS does not split the build tree ($tls_lock)"
else
    fail "AXL_TLS still splits the build tree: '$notls_lock' vs '$tls_lock'"
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

# 4. The COPY is serialised too, not only the build. The staged libraries are
#    copied out of the same tree the lock protects, so releasing it between
#    the build and the copy let one install copy a half-written archive while
#    another rebuilt that tree. AXL_INSTALL_TRACE records the lock's events in
#    order; the copy must land strictly inside the lock.
: > "$WORK/trace"
if AXL_INSTALL_TRACE="$WORK/trace" "$PROJECT_DIR/scripts/install.sh"         --arch x64 --prefix "$WORK/sdk2" >"$WORK/inst2.log" 2>&1; then
    got=$(tr '\n' ' ' < "$WORK/trace" | sed 's/ $//')
    if [[ "$got" == "lock x64 build x64 copied x64 unlock x64" ]]; then
        pass "the staged copy happens inside the build-tree lock ($got)"
    else
        fail "lock events out of order: '$got' (want 'lock x64 build x64 copied x64 unlock x64')"
    fi
else
    fail "traced install failed"
    tail -8 "$WORK/inst2.log" | sed 's/^/      /'
fi

# 5. Every changed file lands by RENAME, so a reader -- a test linking from
#    the stage while another install refreshes it -- sees the old file or the
#    new one, never a partial one. That is by construction, not a race this
#    can force; what is checked is that the scratch files it renames from
#    never survive an install.
left=$(find "$WORK/sdk2" -name '*.axl-tmp.*' 2>/dev/null | head -3)
if [[ -z "$left" && -f "$WORK/sdk2/lib/axl/x64/libaxl.a" ]]; then
    pass "no scratch file is left in the prefix, and the archive landed"
else
    fail "scratch left or archive missing: ${left:-<archive missing>}"
fi

# 6. A SYMLINKED destination is replaced, never written through: the old
#    `install -C` did that (it compares with lstat), and following the link
#    would chmod -- or overwrite -- whatever it points at, which can be a file
#    in someone's source tree.
stagedobj="$WORK/sdk2/lib/axl/x64/axl-reloc.o"
if [[ -f "$stagedobj" ]]; then
    cp "$stagedobj" "$WORK/linktarget"
    chmod 600 "$WORK/linktarget"
    rm -f "$stagedobj"
    ln -s "$WORK/linktarget" "$stagedobj"
    if "$PROJECT_DIR/scripts/install.sh" --arch x64 --prefix "$WORK/sdk2" \
            >"$WORK/inst3.log" 2>&1 \
       && [[ ! -L "$stagedobj" && -f "$stagedobj" ]] \
       && [[ "$(stat -c %a "$WORK/linktarget")" == 600 ]]; then
        pass "a symlinked destination is replaced, its target left alone"
    else
        fail "symlink destination: link=$( [[ -L "$stagedobj" ]] && echo yes || echo no) target mode=$(stat -c %a "$WORK/linktarget")"
    fi
else
    fail "no staged axl-reloc.o to test the symlink case with"
fi

echo
echo "install-concurrent: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
