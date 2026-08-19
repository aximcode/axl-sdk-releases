#!/bin/bash
# test-meta: arch=both needs=virtiofsd,jq est=5 local-only=1
# test-json-corpus-qemu.sh -- AXL's JSON reader vs. EXTERNAL public corpora.
#
# The corpora are NOT vendored. They are cloned into deps/ (gitignored) by
# scripts/fetch-json-corpora.sh and mounted live into the guest with
# `run-qemu.sh --mount`, which is virtiofs -- so no image is built, nothing is
# copied, and a 2000-file suite costs the same to stage as a 3-file one.
#
# That is the whole design goal: verify against public suites without merging
# their data or their code into this repository.
#
# Complements test/unit/axl-test-json-conformance.c, which bakes 316 cases in
# as the always-runs floor. This is the deep pass: all 318 JSONTestSuite
# parsing cases (the floor holds back 2 as oversize), plus 112 JSON5 cases the
# unit suite has NO equivalent for, plus 18 large realistic documents.
#
# local-only=1 because it needs virtiofsd on the host AND VirtioFsDxe in the
# guest firmware. Stock CI firmware may lack the driver, and the failure mode
# would be an empty volume -- so the guest treats "sentinel not found" as a
# hard failure rather than reporting zero cases, which would read as success.
#
# Usage:
#   ./scripts/fetch-json-corpora.sh                     # one-time
#   ./test/integration/test-json-corpus-qemu.sh         # all suites
#   ./test/integration/test-json-corpus-qemu.sh --suite json5-tests
#
# There is deliberately NO "quick subset" mode. It was built, then measured
# away: the corpus work is 0.6s of a 7.5s run on X64 and 1.6s of 12.7s on
# AARCH64 -- the rest is QEMU boot and mount. Running one suite costs ~6.9s and
# running all four costs ~7.5s, so a subset saves nothing worth a flag. If that
# ever stops being true, the per-suite `ms=` figures below are the evidence to
# revisit it with.
#
# Environment:
#   ARCH        X64 (default) or AARCH64
#   MAX_BYTES   per-document cap (default 33554432, which covers every
#               document in the current corpora); over-cap files are
#               reported, never silently dropped

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CORPORA_DIR="$PROJECT_DIR/deps/json-corpora"

ARCH="${ARCH:-X64}"
SUITE=""
MAX_BYTES="${MAX_BYTES:-33554432}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        # run-integration.sh passes --arch to EVERY test it discovers, so a
        # test that does not accept it cannot pass under the runner at all --
        # this one exited 2 in three seconds and read as a suite failure.
        --arch) ARCH="$2"; shift 2 ;;
        --suite) SUITE="$2"; shift 2 ;;
        --max-bytes) MAX_BYTES="$2"; shift 2 ;;
        -h|--help) sed -n '2,32p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "error: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
done

case "$ARCH" in
    X64|x64)      ARCH_SUFFIX="x64" ;;
    AARCH64|aa64) ARCH_SUFFIX="aa64"; ARCH="AARCH64" ;;
    *) echo "error: unsupported ARCH=$ARCH" >&2; exit 2 ;;
esac

TEST_EFI="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$ARCH_SUFFIX")/AxlTestJsonCorpus.efi"

# --- preflight: fail with guidance, never with a mystery -------------------
#
# exit 77, not 0. The corpora are NOT vendored, so on a machine that never ran
# fetch-json-corpora.sh this test declines every time -- and while it exited 0
# it was counted among the suite's passes, so a run that tested no JSON corpus
# at all reported the same green as one that tested every document in it.
# run-integration.sh scores 77 as SKIP and names it in the totals.
if [[ ! -f "$TEST_EFI" ]]; then
    echo "SKIP: $TEST_EFI not built (run: make tests${ARCH:+ ARCH=$ARCH_SUFFIX})" >&2
    exit 77
fi
if [[ ! -f "$CORPORA_DIR/AXLCORPUS.TAG" ]]; then
    echo "SKIP: no corpora in $CORPORA_DIR" >&2
    echo "      fetch them with: ./scripts/fetch-json-corpora.sh" >&2
    exit 77
fi

# --- differential oracle ----------------------------------------------------
# The bulk suites carry no verdict in their filenames -- and they are NOT all
# valid: MicrosoftEdge/Demos deliberately ships binary-data.json,
# missing-colon.json and unterminated.json as error examples. Hardcoding those
# three names would be importing their data into our tests, which is the one
# thing this whole design avoids.
#
# So a REFERENCE PARSER decides instead. jq is the oracle; the list is
# regenerated from the corpus on every run and written into the mount, so it
# stays correct when upstream adds or removes a case, and nothing is committed.
#
# This is also the strongest check here in its own right: jq compares against
# an independent implementation rather than against a filename convention.
generate_reject_list() {
    local out="$CORPORA_DIR/AXLREJECT.LST" suite f
    : > "$out"
    command -v jq >/dev/null || {
        echo "note: jq absent — bulk suites will assume every document is valid" >&2
        return
    }
    for suite in jsonexamples json-dummy-data; do
        [[ -d "$CORPORA_DIR/$suite" ]] || continue
        while IFS= read -r f; do
            jq -e . "$f" >/dev/null 2>&1 || basename "$f" >> "$out"
        done < <(find "$CORPORA_DIR/$suite" -name '*.json')
    done
    local n; n=$(wc -l < "$out")
    echo "oracle: jq marks $n bulk document(s) as invalid (expected to be rejected)"
}

run_one() {
    local suite="$1" label="$2" log t0 t1 elapsed rc
    log="$(mktemp -t axl-corpus-XXXXXX.log)"

    local args=(--max-bytes "$MAX_BYTES")
    [[ -n "$suite" ]] && args+=(--suite "$suite")

    t0=$(date +%s.%N)
    set +e
    "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$ARCH" \
        --mount "$CORPORA_DIR" \
        --timeout 600 \
        "$TEST_EFI" "${args[@]}" > "$log" 2>&1
    rc=$?
    set -e
    t1=$(date +%s.%N)
    elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')

    # The guest prints one SUITE line per corpus with its own ms timing, which
    # excludes boot and mount and so is the number worth comparing.
    grep -E '^(CORPUS|SUITE):?' "$log" || true
    grep -E '^  FAIL ' "$log" | head -25 || true

    local results
    results=$(grep -E '^=== Results:' "$log" | tail -1 || true)
    if [[ -z "$results" ]]; then
        echo "FAIL: $label produced no Results footer (guest crashed or hung)"
        echo "      log: $log"
        return 1
    fi
    echo "$results  [wall ${elapsed}s]"

    if ! grep -qE '=== Results: [0-9]+ passed, 0 failed ===' "$log"; then
        echo "      log: $log"
        return 1
    fi
    rm -f "$log"
    return 0
}

echo "=== JSON corpus ($ARCH) — external suites, mounted from deps/ ==="
generate_reject_list
fail=0
if [[ -n "$SUITE" ]]; then
    run_one "$SUITE" "$SUITE" || fail=1
else
    run_one "" "all suites" || fail=1
fi

if [[ $fail -ne 0 ]]; then
    echo "JSON corpus ($ARCH): FAILED"
    exit 1
fi
echo "JSON corpus ($ARCH): PASSED"
