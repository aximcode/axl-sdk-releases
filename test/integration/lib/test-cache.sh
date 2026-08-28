# shellcheck shell=bash
# test-cache.sh — skip a test whose inputs are byte-identical to its last green run.
#
# **Inert unless AXL_TEST_CACHE names a directory.** Off, every function here is
# a string comparison and a return.
#
# WHY THIS AND NOT A RELEVANCE MAP. Measured 2026-08-19 (§12.15): a change to a
# widely-linked library file leaves **46% of the suite's work** untouched, and a
# tool-only change leaves 86% of tests untouched -- because `--gc-sections` plus
# selective archive-member linking means an image that never linked the changed
# code comes out BYTE-IDENTICAL. The full run is work-bound (§12.1), so skipping
# converts ~1:1 into wall clock. A directory-name map cannot see any of this and
# is unsafe here besides (§12.5).
#
# THE ORDER OF OPERATIONS IS THE WHOLE DESIGN, because what a test stages is
# only knowable by RUNNING it -- there is nothing to hash before the first run:
#
#   run 1   the test records each input as it stages it, and on GREEN the
#           runner commits a key over that list plus the fixed parts.
#   run N   the runner re-hashes the RECORDED list and recomputes the key. Match
#           => skip. No record, a missing file, or any mismatch => run.
#
# The recorded list stays valid only while the test would stage the same things,
# which is why the test script and the harness are in the key: change either and
# the key misses, the test runs, and the list is rewritten. Same shape as Go's
# test cache -- observed inputs, not declared ones.
#
# WHAT THE KEY COVERS, and what it does not:
#
#   covered      every staged artifact (by SOURCE path + sha256), the firmware
#                (FW_CODE/FW_VARS are recorded like any other input), the test
#                script, common-test.sh, run-qemu.sh, every lib/*.sh, and the
#                arch. A toolchain change is covered TRANSITIVELY: it changes
#                the artifacts. AXL_TLS was here too, when it selected which
#                sources compiled; mbedTLS is unconditional now, so keying on
#                it would only let a stray variable cause pointless misses.
#   NOT covered  the host environment -- nproc, /dev/shm, network reachability,
#                the version of python/tar/socat a test shells out to. A test
#                whose result depends on those is not a pure function of the
#                tree, and this cache cannot see it change.
#
# That last line is why the cache is OPT-IN and why a cached run refuses to
# report as a full one (§12.8) or to satisfy the release gate. It is an
# inner-loop tool. The pre-push run is `run-integration.sh` with no cache.

_tc_active() { [[ -n "${AXL_TEST_CACHE:-}" && -d "${AXL_TEST_CACHE:-/nonexistent}" ]]; }

_tc_sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# The inputs a test recorded last time, one absolute path per line.
_tc_inputs_file() { printf '%s/%s.inputs\n' "$AXL_TEST_CACHE" "$1"; }
# The key its last GREEN run produced.
_tc_key_file()    { printf '%s/%s.key\n'    "$AXL_TEST_CACHE" "$1"; }

# --- the test side ----------------------------------------------------------

# cache_record_input <test> <path> — called as the test stages something.
# Appends; duplicates are harmless (the key sorts and uniques).
cache_record_input() {
    _tc_active || return 0
    [[ -f "$2" ]] || return 0
    printf '%s\n' "$(cd "$(dirname "$2")" && pwd)/$(basename "$2")" \
        >> "$(_tc_inputs_file "$1")"
}

# cache_begin_inputs <test> — truncate the list at the start of a real run, so
# a test that stages FEWER things than last time does not inherit stale entries
# (which would make the key miss forever, the safe direction, but confusingly).
cache_begin_inputs() {
    _tc_active || return 0
    : > "$(_tc_inputs_file "$1")"
}

# --- the runner side --------------------------------------------------------

# cache_key <test> — print the key, or nothing if it cannot be computed.
#
# Prints NOTHING when the input list is missing or names a file that no longer
# exists. Both mean "cannot prove this unchanged", and the caller runs the test.
# A cache that guesses in this situation is worse than no cache.
cache_key() {
    _tc_active || return 0
    local test="$1" inputs; inputs="$(_tc_inputs_file "$test")"
    [[ -s "$inputs" ]] || return 0

    local dir; dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    {
        # The fixed part. ARCH selects which artifacts get built at all, so
        # two arches never share a key.
        printf 'arch=%s\n' "${ARCH:-}"
        printf 'script %s\n' "$(_tc_sha "$dir/$test")"
        printf 'harness %s\n' "$(_tc_sha "$dir/common-test.sh")"
        printf 'harness %s\n' "$(_tc_sha "$dir/../../scripts/run-qemu.sh")"
        local l
        for l in "$dir"/lib/*.sh; do
            printf 'harness %s %s\n' "$(basename "$l")" "$(_tc_sha "$l")"
        done
        # The recorded inputs. Sorted so record order cannot change the key.
        local p
        while read -r p; do
            [[ -n "$p" ]] || continue
            [[ -f "$p" ]] || { printf 'MISSING %s\n' "$p"; return 1; }
            printf 'input %s %s\n' "$p" "$(_tc_sha "$p")"
        done < <(sort -u "$inputs")
    } | sha256sum | cut -d' ' -f1
}

# cache_is_fresh <test> — 0 if this test's inputs match its last GREEN run.
cache_is_fresh() {
    _tc_active || return 1
    local keyf; keyf="$(_tc_key_file "$1")"
    [[ -s "$keyf" ]] || return 1
    local now; now="$(cache_key "$1")" || return 1
    [[ -n "$now" && "$now" == "$(cat "$keyf")" ]]
}

# cache_commit <test> — record the key after a GREEN run. Only green: a failing
# test must never be skipped next time on the grounds that nothing changed.
cache_commit() {
    _tc_active || return 0
    local now; now="$(cache_key "$1")" || return 0
    [[ -n "$now" ]] && printf '%s\n' "$now" > "$(_tc_key_file "$1")"
}

# cache_invalidate <test> — drop the key on a FAILING run, so a red test is
# never skipped even if a later run somehow reproduces the same inputs.
cache_invalidate() {
    _tc_active || return 0
    rm -f "$(_tc_key_file "$1")"
}
