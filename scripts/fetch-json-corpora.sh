#!/bin/bash
# fetch-json-corpora.sh -- download the external JSON/JSON5 test corpora.
#
# Everything lands under deps/, which is gitignored, so NOTHING from these
# projects enters this repository. That is the point: we verify against public
# suites without vendoring their data or their code, and each one keeps its own
# licence and history where it was published.
#
# The corpora are consumed by test/integration/test-json-corpus-qemu.sh, which
# mounts deps/json-corpora into the guest with `run-qemu.sh --mount` (virtiofs).
# No image is built and nothing is copied, so a 300-file suite costs the same
# as a 3-file one at run time.
#
# Why these five, and what each one catches that the others do not:
#
#   jsontestsuite   RFC 8259 accept/reject. The canonical conformance corpus
#                   from "Parsing JSON is a Minefield"; verdict is in the
#                   filename (y_/n_/i_). Already the source of the 316 cases
#                   baked into the unit suite -- this runs all 318 parsing
#                   cases; the floor holds back 2 as oversize.
#   json5-tests     The ONLY JSON5 verdict corpus, and our largest gap: every
#                   defect found during phase A was JSON5 escape handling, and
#                   the granular-dialect design rests on a hand-written matrix
#                   with no external oracle behind it.
#   jsonexamples    30 large realistic documents, from simdjson-DATA rather
#                   than the simdjson repo: upstream moved the benchmark corpus
#                   out, and the main repo now keeps only three. Two of the
#                   thirty are why this suite is here at all --
#                   twitterescaped.json is twitter.json with every non-ASCII
#                   character written as \uXXXX, which is precisely the string
#                   ACCESSOR path where every phase-A defect lived, and
#                   canada.json is ~99% floating-point literals, the one shape
#                   AXL has no accessor for (get_number_str is the fallback).
#   json-dummy-data Bulk records of the shape an ordinary API returns. Round
#                   trip and throughput rather than edge cases.
#   (differential)  Not a download: jq / python3 on the host, used by the
#                   runner to compare VALUES rather than verdicts. The only
#                   oracle here that can see a parser which accepts a document
#                   and then hands back the wrong bytes.
#
# Usage:
#   ./scripts/fetch-json-corpora.sh            # fetch what is missing
#   ./scripts/fetch-json-corpora.sh --force    # re-fetch everything
#   ./scripts/fetch-json-corpora.sh --list     # show status, fetch nothing

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CORPORA_DIR="$PROJECT_DIR/deps/json-corpora"

# name|url|revision-or-empty-for-default-branch
# Pinned where upstream is active, so a corpus refresh is a deliberate commit
# to this file rather than a silent change in what "passing" means.
CORPORA=(
    "jsontestsuite|https://github.com/nst/JSONTestSuite|"
    "json5-tests|https://github.com/json5/json5-tests|"
    "jsonexamples|https://github.com/simdjson/simdjson-data|"
    "json-dummy-data|https://github.com/MicrosoftEdge/Demos|"
)

FORCE=0
LIST_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE=1; shift ;;
        --list)  LIST_ONLY=1; shift ;;
        -h|--help) sed -n '2,40p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "error: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
done

command -v git >/dev/null || { echo "error: git is required" >&2; exit 1; }

mkdir -p "$CORPORA_DIR"

# Every one of these repos carries far more than the documents we read --
# JSONTestSuite alone is 217 MB with its `parsers/` (a build of ~40 JSON
# implementations) and `results/`, against ~2 MB of actual test documents. A
# sparse shallow clone fetches only the subdirectories named, which is the
# difference between a 7-second setup and a multi-minute one.
sparse_clone() {
    local url="$1" dest="$2"
    shift 2

    git clone --depth 1 --filter=blob:none --sparse --quiet "$url" "$dest"
    git -C "$dest" sparse-checkout set "$@" >/dev/null
}

status_of() {
    local dest="$1"
    if [[ -d "$dest/.git" ]]; then
        printf 'present  %s\n' "$(git -C "$dest" rev-parse --short HEAD 2>/dev/null || echo '?')"
    else
        printf 'MISSING\n'
    fi
}

for entry in "${CORPORA[@]}"; do
    IFS='|' read -r name url _rev <<< "$entry"
    dest="$CORPORA_DIR/$name"

    if [[ $LIST_ONLY -eq 1 ]]; then
        printf '  %-16s %-46s %s\n' "$name" "$url" "$(status_of "$dest")"
        continue
    fi

    if [[ -d "$dest/.git" && $FORCE -eq 0 ]]; then
        echo "  $name: already present ($(git -C "$dest" rev-parse --short HEAD))"
        continue
    fi
    [[ $FORCE -eq 1 ]] && rm -rf "$dest"

    echo "  $name: fetching $url"
    case "$name" in
        jsontestsuite)    sparse_clone "$url" "$dest" test_parsing test_transform ;;
        jsonexamples)     sparse_clone "$url" "$dest" jsonexamples ;;
        json-dummy-data)  sparse_clone "$url" "$dest" json-dummy-data ;;
        # json5-tests is ~700 KB whole; sparse would cost more than it saves.
        *)                git clone --depth 1 --quiet "$url" "$dest" ;;
    esac
    echo "  $name: $(git -C "$dest" rev-parse --short HEAD)"
done

[[ $LIST_ONLY -eq 1 ]] && exit 0

# Sentinel the guest looks for. The mount lands on whatever fsN: the firmware
# assigns -- fs1: in practice, because fs0: is the boot volume -- so the driver
# scans volumes for this file rather than hardcoding an index. Without it, a
# guest that cannot see the mount would report "0 cases, no failures", which
# reads exactly like success.
printf 'axl json corpus root\n' > "$CORPORA_DIR/AXLCORPUS.TAG"

echo ""
echo "Corpora in $CORPORA_DIR (gitignored — nothing enters the repo):"
du -sh "$CORPORA_DIR"/* 2>/dev/null | sed 's/^/  /'
