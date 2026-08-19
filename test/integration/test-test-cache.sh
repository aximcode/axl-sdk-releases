#!/bin/bash
# test-meta: arch=x64 needs= est=3 local-only=0
# test-test-cache.sh — the skip cache must miss whenever anything it claims to
# cover changes, and must refuse to answer when it cannot see.
#
# Host-only; boots nothing. It drives lib/test-cache.sh against synthetic
# inputs, because the properties that matter are all about WHEN a key misses,
# and a real QEMU run can only demonstrate one of them per boot.
#
# The dangerous direction is a FALSE HIT -- a skip of a test that would have
# failed. So every assertion here except the two controls is "this change must
# BUST the key". The controls exist because a cache that never hits is
# trivially sound and completely useless, and would pass a file made only of
# miss assertions.
#
# Usage: ./test/integration/test-test-cache.sh [--arch X64]

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
cd "$PROJECT_DIR"
export TEST_SKIP_RATCHET=1

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# A private copy of the harness, so mutating "the harness" in this test cannot
# touch the real tree. lib/test-cache.sh resolves its siblings relative to its
# OWN path, so a copied tree is a complete, isolated fixture.
FIX="$WORK/fix"
mkdir -p "$FIX/lib" "$FIX/../../scripts"
cp "$SCRIPT_DIR/lib/test-cache.sh" "$FIX/lib/"
printf 'x\n' > "$FIX/common-test.sh"
printf 'x\n' > "$FIX/../../scripts/run-qemu.sh"
printf 'x\n' > "$FIX/test-fixture.sh"

export AXL_TEST_CACHE="$WORK/cache"; mkdir -p "$AXL_TEST_CACHE"
export ARCH=X64 AXL_TLS=1
# shellcheck source=lib/test-cache.sh
source "$FIX/lib/test-cache.sh"

ART="$WORK/artifact.efi"; printf 'v1\n' > "$ART"
T=test-fixture.sh

prime() {           # a green run: record inputs, commit the key
    cache_begin_inputs "$T"
    cache_record_input "$T" "$ART"
    cache_commit "$T"
}

echo "=== the skip cache misses on every input it claims to cover ==="

# --- control 1: it can hit at all -------------------------------------------
prime
if cache_is_fresh "$T"; then pass "an unchanged input set is FRESH (the cache can hit)"
else fail "an unchanged input set was not fresh — every miss assertion below is vacuous"; fi

# --- a changed ARTIFACT ------------------------------------------------------
printf 'v2\n' > "$ART"
if cache_is_fresh "$T"; then fail "a changed artifact still read FRESH — this is the false hit that matters"
else pass "a changed artifact busts the key"; fi
printf 'v1\n' > "$ART"; prime

# --- a changed TEST SCRIPT ---------------------------------------------------
printf 'y\n' > "$FIX/test-fixture.sh"
if cache_is_fresh "$T"; then fail "a changed test script still read FRESH"
else pass "a changed test script busts the key"; fi
printf 'x\n' > "$FIX/test-fixture.sh"; prime

# --- a changed HARNESS -------------------------------------------------------
printf 'y\n' > "$FIX/common-test.sh"
if cache_is_fresh "$T"; then fail "a changed common-test.sh still read FRESH"
else pass "a changed harness busts the key"; fi
printf 'x\n' > "$FIX/common-test.sh"; prime

# --- a changed run-qemu.sh ---------------------------------------------------
printf 'y\n' > "$FIX/../../scripts/run-qemu.sh"
if cache_is_fresh "$T"; then fail "a changed run-qemu.sh still read FRESH"
else pass "a changed run-qemu.sh busts the key"; fi
printf 'x\n' > "$FIX/../../scripts/run-qemu.sh"; prime

# --- a changed lib/ ----------------------------------------------------------
printf '# tweak\n' >> "$FIX/lib/test-cache.sh"
if cache_is_fresh "$T"; then fail "a changed lib/*.sh still read FRESH"
else pass "a changed lib/ script busts the key"; fi
cp "$SCRIPT_DIR/lib/test-cache.sh" "$FIX/lib/"; prime

# --- a different ARCH --------------------------------------------------------
ARCH=AARCH64
if cache_is_fresh "$T"; then fail "a different ARCH still read FRESH — the arches would share a key"
else pass "a different arch busts the key"; fi
ARCH=X64

# --- a different AXL_TLS -----------------------------------------------------
AXL_TLS=0
if cache_is_fresh "$T"; then fail "a different AXL_TLS still read FRESH"
else pass "a different AXL_TLS busts the key"; fi
AXL_TLS=1

# --- a VANISHED input --------------------------------------------------------
# "cannot prove unchanged" must read as a miss, never as a hit.
mv "$ART" "$ART.gone"
if cache_is_fresh "$T"; then fail "a vanished input still read FRESH — the cache answered what it cannot see"
else pass "a vanished input busts the key"; fi
mv "$ART.gone" "$ART"; prime

# --- no record at all --------------------------------------------------------
if cache_is_fresh "test-never-run.sh"; then fail "a test with NO record read FRESH"
else pass "a test with no record is never fresh"; fi

# --- a RED run must not leave a usable key -----------------------------------
cache_invalidate "$T"
if cache_is_fresh "$T"; then fail "a failing run left a usable key — a red test could be skipped"
else pass "cache_invalidate drops the key (a red test is never skipped)"; fi
prime

# --- control 2: still hits after all that ------------------------------------
if cache_is_fresh "$T"; then pass "the cache still hits after every restore (miss assertions were real)"
else fail "the cache stopped hitting — the restores above are broken, not the cache"; fi

# --- off by default ----------------------------------------------------------
( unset AXL_TEST_CACHE
  if cache_is_fresh "$T"; then exit 1; else exit 0; fi )
if [[ $? -eq 0 ]]; then pass "with AXL_TEST_CACHE unset the cache never claims freshness"
else fail "the cache answered with AXL_TEST_CACHE unset — it is not opt-in"; fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]]
