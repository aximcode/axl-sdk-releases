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
# AND THE KEY IS ONLY SOUND IF EVERY INPUT IS ALREADY BUILT when run N asks.
# It was not. 81 fixture targets, across 79 tests, are built by the TESTS
# THEMSELVES inside their own run, and the runner decided the cache first --
# so the key hashed a fixture not rebuilt since the library change, matched, and
# skipped the test, which then never rebuilt it either. `libaxl.a` is in no
# key; the fixture was the only thing carrying it. run-integration.sh now
# pre-builds the union of the selected tests' `builds=` targets before the
# first cache decision (§12.16.5, and check-test-meta holds the declaration
# to the test's own make line). A test that stages something NEITHER the
# pre-built set nor a builds= target produces would put this hole back.
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
#                script and every other test script it names, the harness
#                (_tc_harness_files below: all of scripts/ and test/pool/, and
#                every test/integration file that is not a test script), and
#                the arch. A toolchain change is covered TRANSITIVELY: it changes
#                the artifacts. AXL_TLS was here too, when it selected which
#                sources compiled; mbedTLS is unconditional now, so keying on
#                it would only let a stray variable cause pointless misses.
#                WITHOUT BEING RECORDED, too: every tree file the test (or a
#                test it names) spells as a literal $PROJECT_DIR/... path
#                (_tc_literal_inputs), the products of its builds= targets
#                (lib/build-products.sh), and for a POOLED run the code the
#                pool service runs (AXL_POOL_CODE_ID, in cache_key). A test
#                is still cacheable only once it records something: these
#                add to a key, they never make one.
#   held by a gate  what none of that sees -- a tree file copied into the
#                guest by a path that is not literal, not a builds= product
#                and not recorded, or a --mount of an in-repo path --
#                fails gate.check-test-inputs unless the line says
#                `# test-input: not-keyed -- <why>`
#                (scripts/check-test-inputs.py, which asks the readers here
#                rather than copying them).
#   NOT covered  the host environment -- nproc, /dev/shm, network reachability,
#                the version of python/tar/socat a test shells out to. A test
#                whose result depends on those is not a pure function of the
#                tree, and this cache cannot see it change.
#
# That last line is why the cache is OPT-IN and why a cached run refuses to
# report as a full one (§12.8) or to satisfy the release gate. It is an
# inner-loop tool. The pre-push run is `run-integration.sh` with no cache.

# _tc_harness_files -- every file of the harness, relative to the repo (the
# current directory), sorted: what a test can run without recording it as an
# input. By GLOB, not by list: a hand-kept list missed scripts/axl-common.sh
# (sourced by common-test.sh and run-qemu.sh, so by nearly every test), a
# sourced .conf, the prefix scripts common-test.sh EXECUTES, the helpers the
# ssh suites run -- each a false hit, a "green" test that never ran the change.
# So: all of scripts/; all of test/pool/ (the pool's own tests run under a
# BOOTED key, so its code is harness in every mode); and every test/integration
# file except the test scripts (a test is keyed on its own script and the ones
# it names, in cache_key). Dotfiles are not harness (.last-run-stamp changes
# every run), nor is __pycache__.
#
# The cost is deliberate: any edit there misses every key once -- wrong in the
# safe direction only. Returns 1 if a find fails: a list it could not read is
# not a list of nothing.
_tc_harness_files() {
    ( set -o pipefail
      { rc=0
        find scripts test/pool test/integration/lib -type f ! -name '.*' \
            ! -path '*/__pycache__/*' || rc=1
        find test/integration -maxdepth 1 -type f ! -name '.*' \
            ! -name 'test-*.sh' || rc=1
        # data/ and fixtures/ may legitimately not exist; nothing else may.
        for d in test/integration/data test/integration/fixtures; do
            [[ -d "$d" ]] || continue
            find "$d" -type f ! -name '.*' ! -path '*/__pycache__/*' || rc=1
        done
        exit "$rc"
      } | LC_ALL=C sort )
}

# _tc_named_tests <test> -- the OTHER test scripts <test> names (it may run
# one: test-net-latency-starved-qemu.sh runs test-net-latency-qemu.sh, whose
# boots record under the parent's name), as existing paths relative to the
# repo, sorted. Followed to the end of the chain, not one level: a test that
# runs a test that runs a third is keyed on all three. A name that matches
# inside a longer one or a comment only adds a file: an extra miss, never a
# false hit.
_tc_named_tests() {
    local -A seen=(["$1"]=1)
    local -a todo=("$1")
    local cur n
    while (( ${#todo[@]} )); do
        cur="${todo[0]}"; todo=("${todo[@]:1}")
        [[ -f "test/integration/$cur" ]] || continue
        while read -r n; do
            [[ -n "$n" && -z "${seen[$n]:-}" && -f "test/integration/$n" ]] || continue
            seen[$n]=1; todo+=("$n")
        done < <(grep -oE 'test-[A-Za-z0-9_-]+\.(sh|nsh)' "test/integration/$cur")
    done
    unset 'seen[$1]'
    if (( ${#seen[@]} )); then
        printf 'test/integration/%s\n' "${!seen[@]}" | LC_ALL=C sort
    fi
    return 0
}

# _tc_literal_inputs <test> -- every tree file <test>, or a test it names,
# spells as a literal `$PROJECT_DIR/...` (or `${PROJECT_DIR}/...`) path,
# relative to the repo, sorted: what it may hand the guest without recording
# it (`cp "$PROJECT_DIR/tools/sshd.conf" "$TEST_STAGING/"`, or a variable set
# from one). Only a path naming an existing regular FILE counts:
#   - a DIRECTORY is never hashed -- deps/json-corpora is thousands of files,
#     and a corpus that changes only on a deliberate fetch costs more keyed
#     than it catches; scripts/check-test-inputs.py makes a test that copies
#     or mounts one record what it means or say why not (`# test-input:`);
#   - a literal that continues with an expansion (`$PROJECT_DIR/share/$n`)
#     names a prefix, which is a directory or nothing: not keyed, and the
#     same gate holds whatever it is staged as;
#   - nothing under build/, out/ or stage/: what the build makes is keyed
#     from builds= (lib/build-products.sh) or recorded, and a run's own logs
#     there would miss every key.
# A path in a comment or a string that is never staged only adds a file: an
# extra miss, never a false hit.
_tc_literal_inputs() {   # <test> [the tests it names, as _tc_named_tests prints them]
    local -a files=()
    local p found
    # A test that is not in this tree (a runner self-test's stub directory)
    # has no literals here, as _tc_named_tests reads none from it: the
    # script's own digest in the key stands for it.
    for p in "test/integration/$1" "${@:2}"; do
        [[ -f "$p" ]] && files+=("$p")
    done
    (( ${#files[@]} )) || return 0
    # grep's 1 is "no literal"; 2 is "could not read", which is no answer.
    found=$(grep -ohE '\$\{?PROJECT_DIR\}?/[A-Za-z0-9._/+@-]+' "${files[@]}")
    (( $? <= 1 )) || return 1
    while IFS= read -r p; do
        p="${p#*PROJECT_DIR}"; p="${p#\}}"; p="${p#/}"
        p="${p%/}"
        case "$p" in
            ""|build|build/*|out|out/*|stage|stage/*|..|../*|*/../*|*/..) continue ;;
        esac
        if [[ -f "$p" ]]; then printf '%s\n' "$p"; fi
    done <<< "$found" | LC_ALL=C sort -u
}

_tc_active() { [[ -n "${AXL_TEST_CACHE:-}" && -d "${AXL_TEST_CACHE:-/nonexistent}" ]]; }

# The builds= products reader (bp_products), loaded once here rather than in
# each key's subshell.
# shellcheck source=build-products.sh
source "$(dirname "${BASH_SOURCE[0]}")/build-products.sh"

_tc_sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# AXL_TEST_CACHE_MODE=pool (run-integration.sh --pool, for a guest= test it
# runs pooled): a pooled run and a booted one are DIFFERENT runs of the test
# -- other inputs (the agent, the pool's code), another path through the
# harness -- so each has its own inputs and key. One shared key let a green
# pooled run skip the booted path until an input changed, and back.
_tc_mode() { [[ "${AXL_TEST_CACHE_MODE:-}" == pool ]] && printf '@pool'; return 0; }
# The arch, in the FILE names as well as inside the key. Named by test only,
# an X64 run and an AARCH64 run -- the two CTest trees, run back to back --
# overwrote each other's key and every run of either missed. The runner side
# (lib/run-one.sh) exports AXL_TEST_CACHE_ARCH so every recorder -- the test
# process, and run-qemu.sh beside it, whose own ARCH is its own business --
# names the same file. ARCH is the fallback for a caller that sets it (the
# runner's shell does); a test run by hand with AXL_TEST_CACHE exported
# records under "@", which nothing reads: only the runner decides the cache.
_tc_arch() { printf '@%s' "${AXL_TEST_CACHE_ARCH:-${ARCH:-}}"; }
# The inputs a test recorded last time, one absolute path per line.
_tc_inputs_file() { printf '%s/%s%s%s.inputs\n' "$AXL_TEST_CACHE" "$1" "$(_tc_arch)" "$(_tc_mode)"; }
# The key its last GREEN run produced.
_tc_key_file()    { printf '%s/%s%s%s.key\n'    "$AXL_TEST_CACHE" "$1" "$(_tc_arch)" "$(_tc_mode)"; }

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
    # A POOLED run ran on the service's guests, so the service's code is an
    # input -- and it may not be this tree's test/pool/ (an older start, or
    # the main checkout's copy while this key hashes a worktree's). The
    # runner asks the service once per run (cmake/AxlIntegrationTests.cmake.in
    # puts its answer in AXL_POOL_CODE_ID), and a lease from a service whose
    # code differs is a pool miss (`axlpool run --expect-code`), so a key
    # with this identity is only ever committed by a test that ran on it. A
    # service that could not be asked, or reports none (older than the
    # identity), leaves NO pooled key: the test runs.
    local pool_code=""
    if [[ -n "$(_tc_mode)" ]]; then
        pool_code="${AXL_POOL_CODE_ID:-}"
        [[ -n "$pool_code" ]] || return 0
    fi
    local repo products named
    repo="$(cd "$dir/../.." && pwd)" || return 0
    named="$(cd "$repo" && _tc_named_tests "$test")" || return 0
    # The builds= products, from the tree's build graph (lib/build-products
    # .sh): an error there is no answer, so no key. This test's own builds=
    # only, NOT the tests it names: a name is mostly a mention in a comment
    # (test-sshd-conf-qemu.sh names eleven, test-efi-pack-release-qemu.sh's
    # BUILD=RELEASE `all` among them), and keying their products would miss
    # on every release image and leave no key whenever the run's prep did not
    # build them. A test that RUNS another declares the child's builds= in
    # its own, as test-net-latency-starved-qemu.sh does.
    products="$(bp_products "${ARCH:-X64}" "$dir/$test")" || return 0
    # The harness digest first, OUTSIDE the key's pipeline, so an error is
    # seen: a find or a read that failed would key over less, and a key over
    # less is stable -- a false hit. No digest, no key: the test runs. One
    # sha256sum over the harness, the tests this one names, the tree files
    # they name literally, and the builds= products.
    local harness
    # shellcheck disable=SC2086 # the named tests: paths without spaces, one per word
    harness="$(set -o pipefail; cd "$repo" \
        && { _tc_harness_files && { [[ -z "$named" ]] || printf '%s\n' "$named"; } \
             && _tc_literal_inputs "$test" $named \
             && { [[ -z "$products" ]] || printf '%s\n' "$products"; }; } \
        | tr '\n' '\0' | xargs -0 -r sha256sum --)" || return 0
    {
        # The fixed part. ARCH selects which artifacts get built at all, so
        # two arches never share a key.
        printf 'arch=%s\n' "${ARCH:-}"
        printf 'mode=%s\n' "$(_tc_mode)"
        printf 'pool_code=%s\n' "$pool_code"
        printf 'script %s\n' "$(_tc_sha "$dir/$test")"
        # The harness, named tests and literals (paths relative) and the
        # builds= products (absolute, as recorded inputs are: a key is a
        # checkout's own). A file that vanishes drops its line, which
        # changes the key.
        printf '%s\n' "$harness" | sed 's/^/harness /'
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

# Script mode, for scripts/check-test-inputs.py -- which must judge "keyed by
# its literal path" by THIS reader, not a second copy of it:
#   test-cache.sh --literals TEST...   `TEST<TAB>PATH` for each tree file the
#                                      key reads from TEST's literal paths
#                                      (relative to the repo). Exit 1 on any
#                                      error: no answer is not "none".
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -uo pipefail
    [[ "${1:-}" == --literals && $# -ge 2 ]] \
        || { echo "usage: test-cache.sh --literals TEST..." >&2; exit 2; }
    shift
    cd "$(dirname "${BASH_SOURCE[0]}")/../../.." || exit 1
    _tc_rc=0
    for _tc_t in "$@"; do
        _tc_t="${_tc_t##*/}"
        # shellcheck disable=SC2086 # the named tests: paths without spaces
        if ! _tc_named="$(_tc_named_tests "$_tc_t")" \
           || ! _tc_lit="$(_tc_literal_inputs "$_tc_t" $_tc_named)"; then
            echo "test-cache.sh: cannot read the literal paths of $_tc_t" >&2
            _tc_rc=1; continue
        fi
        while IFS= read -r _tc_p; do
            [[ -n "$_tc_p" ]] && printf '%s\t%s\n' "$_tc_t" "$_tc_p"
        done <<< "$_tc_lit"
    done
    exit "$_tc_rc"
fi
