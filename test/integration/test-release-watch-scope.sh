#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-release-watch-scope.sh — the release watcher must block on the workflows
# the TAG started, and only those.
#
# WHY THIS EXISTS. Measured on the v4.3.1 cut: the tag's own workflows were both
# finished at 5m02s (Release 4m02s, Docs 5m02s) and the script did not return
# for another 11m05s -- 69% of a 16m07s cut spent watching CI.
#
# CI is not started by the tag. It is started by the `git push origin main` that
# cut-release.sh performs two steps earlier, and it re-runs the same integration
# suite the LOCAL uncached gate already certified on that exact commit
# (RELEASING.md: "the authoritative pre-release gate is the full suite run
# locally"). By the time it finishes the tag is pushed and the release is
# published, so waiting on it gates nothing -- it only delays the human.
#
# It was also failing the cut: the terminal condition and the final verdict both
# scanned EVERY workflow on the SHA rather than the expected set, which is how
# v4.3.0 reported `RELEASE_VERDICT: FAIL` for a CI container defect after having
# successfully published all 8 assets.
#
# So: EXPECTED gates, everything else is reported and ignored. This asserts both
# halves, because dropping the report would be the opposite failure -- a red CI
# nobody mentions.
#
# Driven through a STUB `gh` on PATH, so it needs no network and no real run.
#
# Usage: ./test/integration/test-release-watch-scope.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
WATCH="$PROJECT_DIR/scripts/watch-release-runs.sh"

WORK="$(mktemp -d -t axl-watch.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# stub_gh <payload-file> — a `gh` that answers the GraphQL poll from a fixture
# and nothing else. `jq` is real; only the network is replaced.
mk_stub() {
    mkdir -p "$WORK/bin"
    cat > "$WORK/bin/gh" <<STUB
#!/bin/bash
# only the graphql poll is used by the watcher
cat "$WORK/payload.json"
STUB
    chmod +x "$WORK/bin/gh"
}
mk_stub

# payload <name:status:conclusion> ... — build the GraphQL shape the watcher parses
payload() {
    local nodes=""
    for spec in "$@"; do
        IFS=: read -r n s c <<< "$spec"
        [[ -n "$nodes" ]] && nodes+=","
        nodes+="{\"workflowRun\":{\"workflow\":{\"name\":\"$n\"}},\"status\":\"$s\",\"conclusion\":$([[ "$c" == "-" ]] && echo null || echo "\"$c\"")}"
    done
    printf '{"data":{"repository":{"object":{"checkSuites":{"nodes":[%s]}}}}}' "$nodes" > "$WORK/payload.json"
}

run_watch() {  # run_watch <tag> -> stdout+stderr, sets RC
    # POLL_INTERVAL_S, not INTERVAL -- the script reads the former and defaults
    # the latter from it. Passing the wrong one silently polls at 60 s.
    OUT="$(cd "$PROJECT_DIR" && PATH="$WORK/bin:$PATH" \
        EXPECT_WORKFLOWS="Release Docs" MAX_POLLS=2 POLL_INTERVAL_S=1 \
        timeout 45 "$WATCH" "$1" 2>&1)"
    RC=$?
}

echo "=== 1. the tag's workflows are done; CI is still running ==="
# The v4.3.1 shape exactly. The watcher must RETURN, not sit on CI.
payload "Release:COMPLETED:SUCCESS" "Docs:COMPLETED:SUCCESS" "CI:IN_PROGRESS:-"
run_watch v4.3.1
if [[ "$RC" -eq 0 ]]; then
    pass "returns while a non-gating workflow is still IN_PROGRESS (rc=0)"
else
    fail "blocked or failed on a non-gating workflow (rc=$RC)"
    printf '%s\n' "$OUT" | tail -6 | sed 's/^/      /'
fi
grep -q "RELEASE_VERDICT: PASS" <<<"$OUT" && pass "verdict is PASS" \
                                          || fail "verdict was not PASS"
# ...and it must still SAY that CI is running. Silence here would be the
# opposite defect: a red CI nobody mentions.
grep -q "CI" <<<"$OUT" && pass "...and still reports the non-gating CI run" \
                       || fail "the non-gating run was hidden entirely"

echo "=== 2. a FAILING non-gating workflow does not fail the release ==="
# v4.3.0's exact shape: all 8 assets published, CI red on a container defect.
payload "Release:COMPLETED:SUCCESS" "Docs:COMPLETED:SUCCESS" "CI:COMPLETED:FAILURE"
run_watch v4.3.1
[[ "$RC" -eq 0 ]] && pass "a red CI does not fail a published release (rc=0)" \
                  || { fail "a red non-gating workflow failed the cut (rc=$RC)"
                       printf '%s\n' "$OUT" | tail -5 | sed 's/^/      /'; }
grep -qiE "not gating|informational|does not gate" <<<"$OUT" \
    && pass "...and the report says it is not gating" \
    || fail "the report does not distinguish gating from non-gating"

echo "=== 3. the CONTROL — a gating workflow still blocks and still fails ==="
# Without this, 'return immediately' would pass sections 1-2 and mean nothing.
payload "Release:COMPLETED:SUCCESS" "Docs:COMPLETED:FAILURE"
run_watch v4.3.1
[[ "$RC" -ne 0 ]] && pass "a FAILED expected workflow still fails the cut" \
                  || fail "a failed expected workflow was ignored (rc=$RC)"
grep -q "RELEASE_VERDICT: FAIL" <<<"$OUT" && pass "...with verdict FAIL" \
                                          || fail "no FAIL verdict"

# ...and an expected workflow still IN_PROGRESS must keep it waiting.
payload "Release:COMPLETED:SUCCESS" "Docs:IN_PROGRESS:-"
run_watch v4.3.1
[[ "$RC" -ne 0 ]] && pass "an expected workflow still running keeps it waiting (times out)" \
                  || fail "returned while an EXPECTED workflow was in progress"

echo
echo "release-watch-scope: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
