#!/bin/bash
# check-tautology.sh -- no `test_check(true, ...)` in the unit suite.
#
# An assertion that evaluates nothing pretends coverage (CLAUDE.md: "No
# test_check(true, ...)"). Matched anywhere on the line, then comment lines
# dropped: anchoring to line start instead let `foo(); test_check(true, ...)`
# through -- verified by sabotage. The exclusion is what spares the
# primitives' own docstrings, which quote the banned form to explain it.
#
# Usage: check-tautology.sh [DIR]   (default test/unit/)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${1:-$REPO/test/unit}"
[[ -d "$DIR" ]] || { echo "check-tautology: CANNOT RUN — no directory $DIR"; exit 1; }
hits=$(grep -rn 'test_check(true' "$DIR" | grep -vE ':[[:space:]]*(\*|//|/\*)')
rc=$?
# grep: 0 a hit, 1 none, 2 could not read -- the last must not read as clean.
if (( rc > 1 )); then
    echo "check-tautology: CANNOT RUN — grep failed over $DIR"; exit 1
fi
if [[ -n "$hits" ]]; then
    echo "check-tautology: FAIL — assertions that evaluate nothing:"
    sed 's/^/    /' <<< "$hits"
    echo "  Use test_skip_n(n, why) / test_survived(name) / test_skip(name),"
    echo "  or assert the value. See test/unit/axl-test.h."
    exit 1
fi
echo "check-tautology: clean — no test_check(true, ...) in the unit suite."
