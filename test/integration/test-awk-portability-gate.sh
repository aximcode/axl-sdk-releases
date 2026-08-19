#!/bin/bash
# test-meta: arch=none needs= est=2 local-only=0
# test-awk-portability-gate.sh — the awk gate must be able to SEE the files it
# claims to cover.
#
# WHY THIS EXISTS. check-awk-portability.py exists because /usr/bin/awk is mawk
# in every container this project builds in, and a gawk extension therefore
# works locally and fails in CI -- sometimes silently, which is how
# `-DAXL_NEWLIB_MALLINFO_INT=1` went un-applied in every CI build while every
# local build applied it.
#
# The gate found its files by glob, and `scripts/*.sh` matches neither
# `scripts/axl-cc` nor its `axl-c++` alias, because the two scripts that SHIP
# to consumers carry no extension. The driver holds the largest awk program in
# the tree and the gate could not see one byte of it: a `strtonum` planted in
# it was reported as "clean -- 203 build files, no gawk-only functions", which
# reads exactly like coverage.
#
# So this asserts the two properties a scanner's silence depends on: that it
# looks at the right files, and that it still fails when one of them is bad. A
# gate that cannot see is worse than no gate, because it is believed.
#
# Host-only: no QEMU, no build. Never mutates a tracked file -- the suite runs
# a parallel pool, and this test's whole subject is a scanner over the worktree.
#
# Usage: ./test/integration/test-awk-portability-gate.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
GATE="$PROJECT_DIR/scripts/check-awk-portability.py"

WORK="$(mktemp -d -t axl-awkgate.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

[[ -x "$GATE" || -f "$GATE" ]] || { echo "  FAIL: $GATE missing"; exit 1; }

echo "=== 1. coverage: the gate looks at the extensionless drivers ==="

LIST="$WORK/list"
if ! python3 "$GATE" --list > "$LIST" 2>"$WORK/list.err"; then
    fail "--list failed"; sed 's/^/      /' "$WORK/list.err" | head -5
else
    pass "--list enumerates what the gate scans"
    # The two that SHIP. Named explicitly rather than counted: a count going up
    # proves nothing about WHICH file was added.
    for want in scripts/axl-cc scripts/axl-c++; do
        grep -qx -- "$want" "$LIST" && pass "$want is covered" \
                                    || fail "$want is NOT covered"
    done
    # ...and the globbed files did not get lost while adding them.
    for want in Makefile scripts/run-qemu.sh test/integration/common-test.sh; do
        grep -qx -- "$want" "$LIST" && pass "$want is still covered" \
                                    || fail "$want was DROPPED"
    done
    # A python script is not shell glue and must not be swept in by the
    # shebang rule -- `awk` inside a python string is not an awk program this
    # gate can reason about, and false positives get a gate allowlisted into
    # uselessness.
    if grep -qE '\.py$' "$LIST"; then
        fail "the shebang sweep pulled in .py files"
        grep -E '\.py$' "$LIST" | head -3 | sed 's/^/      /'
    else
        pass "python scripts are not swept in"
    fi
fi

echo "=== 2. ...and it still FAILS on a gawk-only call in one of them ==="

# Against a COPY of the repo, so the real worktree is never mutated: the gate
# reads git-tracked files, so the fixture has to be a git repo of its own.
CLONE="$WORK/clone"
if git -C "$PROJECT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
    mkdir -p "$CLONE/scripts"
    ( cd "$CLONE" && git init -q . && git config user.email t@t && git config user.name t )
    cp "$GATE" "$CLONE/scripts/"
    # The banned name is ASSEMBLED rather than written literally, because this
    # file is itself git-tracked and the gate scans git-tracked files: spelled
    # out, the fixture makes the gate fail on the very test that proves it
    # works. The file WRITTEN below still contains the real token (printf
    # concatenates at run time), so the fixture tests exactly what it did.
    #
    # Assembled rather than added to EXEMPT on purpose. That dict is empty and
    # the gate's docstring says why -- an allowlist is how this class of check
    # gets "allowlisted into uselessness" -- so the fix that keeps it empty is
    # the right one.
    _gawk_fn='str'; _gawk_fn+='tonum'
    printf '#!/bin/bash\n# a driver with no extension\nawk "BEGIN{print %s(\\"0x10\\")}"\n' \
        "$_gawk_fn" > "$CLONE/scripts/fake-driver"
    chmod +x "$CLONE/scripts/fake-driver"
    ( cd "$CLONE" && git add -A && git commit -qm x )
    if python3 "$CLONE/scripts/check-awk-portability.py" >"$WORK/out" 2>&1; then
        fail "a strtonum in an extensionless driver was reported CLEAN"
        head -5 "$WORK/out" | sed 's/^/      /'
    else
        grep -q "strtonum" "$WORK/out" && pass "a gawk-only call in an extensionless driver FAILS the gate" \
                                       || { fail "gate failed but never named strtonum"
                                            head -5 "$WORK/out" | sed 's/^/      /'; }
    fi
else
    fail "not a git repo, cannot build the fixture"
fi

echo
echo "awk-portability-gate: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
