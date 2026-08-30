#!/bin/bash
# test-meta: arch=none needs= est=5 local-only=0
# test-build-id.sh -- every .efi we produce carries a build identity.
#
# WHY THIS EXISTS. Surveyed 2026-08-28, our own images had NO build identity of
# any kind:
#
#   artifact              link stamp   CodeView GUID
#   linker .map           yes          none
#   real MSVC PE          yes          no CodeView record at all
#   AXL's own .efi        0            ALL-ZERO GUID, age 1
#   RSOD dump             none         none
#
# `src/crt0/axl-debug-info.S` emits the RSDS record with sixteen literal zero
# bytes and `pe-set-debug` patched the RVA and the module name but never the
# GUID. So if one of our own .efi files ever landed in RSOD triage, nothing
# could say WHICH BUILD it was -- not a stamp (0 on every image this SDK
# produces) and not a GUID (zero on every one). Two builds of the same tool
# were indistinguishable, which is precisely the failure the 4.3.4 wrong-artifact
# gate exists to catch in the MSVC corpus and could not catch in ours.
#
# WHAT IS PINNED. That the identity is present, DETERMINISTIC (the same bytes
# always give the same id, so a rebuild that changed nothing is recognisable as
# the same build) and DISCRIMINATING (one changed byte changes it). An id that
# is merely non-zero would satisfy a naive check while being useless -- a
# constant is non-zero too.
#
# Host-only: no QEMU. Uses already-built .efi files.
#
# Usage: ./test/integration/test-build-id.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

WORK="$(mktemp -d -t axl-buildid.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "=== .efi build identity ==="
echo ""

PSD="$(test_build_dir x64)/build/pe-set-debug"
TOOLS="$(test_build_dir x64)/tools"
if [[ ! -x "$PSD" ]]; then
    echo "SKIP: pe-set-debug not built (run: make ARCH=x64 tools)"
    exit 0
fi
test_host_pass "pe-set-debug is built"

A="$TOOLS/hexdump.efi"
B="$TOOLS/lspci.efi"
for f in "$A" "$B"; do
    [[ -f "$f" ]] || { echo "SKIP: $f not built"; exit 0; }
done

# ── the identity exists and is not the old all-zero placeholder ──
ID_A="$("$PSD" --print-build-id "$A" 2>&1)"
if [[ "$ID_A" =~ ^[0-9a-f]{32}$ ]]; then
    test_host_pass "--print-build-id emits a 128-bit hex id"
else
    test_host_fail "--print-build-id emits a 128-bit hex id"
    echo "      got: '$ID_A'"
    test_host_summary "build-id"
    exit 1
fi
if [[ "$ID_A" != "00000000000000000000000000000000" ]]; then
    test_host_pass "the id is not the all-zero placeholder"
else
    test_host_fail "the id is not the all-zero placeholder"
fi

# ── DISCRIMINATING: two different images, two different ids ──────
#
# The assertion that a constant would pass. "Non-zero" alone is satisfied by
# hardcoding any value, which would be exactly as useless as the zeros it
# replaced.
ID_B="$("$PSD" --print-build-id "$B" 2>&1)"
if [[ "$ID_A" != "$ID_B" ]]; then
    test_host_pass "two different images have different ids"
else
    test_host_fail "two different images have different ids"
    echo "      both: $ID_A"
fi

# ── DETERMINISTIC: the same bytes always give the same id ────────
#
# Without this the id could be a timestamp or a random value, which would make
# every rebuild look like a different build and destroy the property that
# makes it useful: recognising that two artifacts ARE the same build.
# Copy under the SAME BASENAME. pe-set-debug derives the module name from the
# filename and writes it into the image, and the id covers that -- which is
# correct and worth stating: two images differing only in their recorded module
# name ARE different images. A first draft copied to `copy.efi` and read a
# different id, which is the tool behaving properly.
mkdir -p "$WORK/same"; cp "$A" "$WORK/same/$(basename "$A")"
COPY="$WORK/same/$(basename "$A")"
ID_COPY="$("$PSD" --print-build-id "$COPY" 2>&1)"
if [[ "$ID_COPY" == "$ID_A" ]]; then
    test_host_pass "the same bytes give the same id"
else
    test_host_fail "the same bytes give the same id"
    echo "      original: $ID_A"
    echo "      copy:     $ID_COPY"
fi

# ── IDEMPOTENT: re-running the patcher does not churn the id ─────
#
# pe-set-debug writes the id INTO the image, so a naive implementation hashes
# a file that now contains its own hash and produces a different answer every
# run. That would make `make` non-reproducible and the id meaningless.
"$PSD" "$COPY" >/dev/null 2>&1
ID_AGAIN="$("$PSD" --print-build-id "$COPY" 2>&1)"
if [[ "$ID_AGAIN" == "$ID_A" ]]; then
    test_host_pass "re-running pe-set-debug leaves the id unchanged"
else
    test_host_fail "re-running pe-set-debug leaves the id unchanged"
    echo "      before: $ID_A"
    echo "      after:  $ID_AGAIN"
fi

# ── SENSITIVE: one changed byte changes the id ───────────────────
#
# Patch a byte deep in .text rather than in a header, so this cannot pass by
# accident through some header-only digest.
# Same basename again, then tweak, then RE-RUN the patcher: --print-build-id
# reports the id an image CARRIES, so a byte changed without re-running would
# leave the stored id untouched and the assertion would be testing nothing.
mkdir -p "$WORK/tweak"; cp "$A" "$WORK/tweak/$(basename "$A")"
TWEAK="$WORK/tweak/$(basename "$A")"
SIZE=$(stat -c%s "$TWEAK")
OFF=$(( SIZE / 2 ))
printf '\xa5' | dd of="$TWEAK" bs=1 seek="$OFF" conv=notrunc status=none
"$PSD" "$TWEAK" >/dev/null 2>&1
ID_TWEAK="$("$PSD" --print-build-id "$TWEAK" 2>&1)"
if [[ "$ID_TWEAK" != "$ID_A" ]]; then
    test_host_pass "one changed byte changes the id"
else
    test_host_fail "one changed byte changes the id"
fi

# ── every shipped tool carries one ────────────────────────────────
#
# A per-image property is only worth having if it holds for all of them; one
# link path that skipped pe-set-debug would leave a hole exactly where triage
# needs it.
MISSING=""
for f in "$TOOLS"/*.efi; do
    id="$("$PSD" --print-build-id "$f" 2>/dev/null)"
    [[ "$id" =~ ^[0-9a-f]{32}$ ]] && [[ "$id" != "00000000000000000000000000000000" ]] \
        || MISSING="$MISSING $(basename "$f")"
done
if [[ -z "$MISSING" ]]; then
    test_host_pass "every built .efi in tools/ carries a non-zero id"
else
    test_host_fail "every built .efi in tools/ carries a non-zero id"
    echo "      without one:$MISSING"
fi

# ── rsod-decode reports the SAME string ───────────────────────
#
# The point of the id is triage, and triage means a human comparing an image
# against a build log. Two renderings of the same sixteen bytes would defeat
# that: rsod-decode also keeps the braced GUID form for PDB matching, which
# byte-swaps the first three fields and would NOT match what pe-set-debug
# prints.
DEC="$PROJECT_DIR/scripts/rsod-decode.py"
LINE="$(timeout 60 python3 "$DEC" --syms "$A" --base 0x140000000 \
        --addr 0x140001000 --detail 2>/dev/null \
        | sed -e 's/\x1b\[[0-9;]*m//g' | grep -m1 '^Image 0')"
if grep -q "id=$ID_A" <<<"$LINE"; then
    test_host_pass "rsod-decode prints the same id pe-set-debug does"
else
    test_host_fail "rsod-decode prints the same id pe-set-debug does"
    echo "      pe-set-debug: $ID_A"
    echo "      rsod-decode:  $LINE"
fi

# ── and it is NOT required ────────────────────────────────────
#
# Images from other toolchains carry no CodeView record at all -- the real
# MSVC corpus PE has none -- so a build id must be reported when present and
# never demanded. A tool that refused an image for lacking one would refuse
# exactly the artifacts it exists to decode.
NOCV="$WORK/nocv"
if python3 "$PROJECT_DIR/test/integration/lib/make-rsod-fixture.py" \
        "$NOCV" --no-codeview >/dev/null 2>&1; then
    OUT="$WORK/nocv.txt"
    if timeout 60 python3 "$DEC" --syms "$NOCV/app.efi" --rsod "$NOCV/console.log" \
            --detail > "$OUT" 2>&1; then
        test_host_pass "an image with NO CodeView record still decodes"
    else
        test_host_fail "an image with NO CodeView record still decodes"
        tail -4 "$OUT" | sed 's/^/      /'
    fi
    if grep -qE '^Image 0.* id=' <<<"$(sed -e 's/\x1b\[[0-9;]*m//g' "$OUT")"; then
        test_host_fail "no id is invented for an image that records none"
    else
        test_host_pass "no id is invented for an image that records none"
    fi
else
    test_host_fail "the --no-codeview fixture builds"
fi

echo ""
test_host_summary "build-id"
