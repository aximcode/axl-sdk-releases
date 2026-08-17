#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-sdk-prefix.sh — the STAGED SDK has its own accessor, separate from the
# build directory's.
#
# WHY THIS EXISTS. Two different things live under out/ and both were called
# "the prefix":
#
#   out/native-<arch>[-release][-tls]   the BUILD directory -- intermediate
#                                       objects, a function of ARCH x BUILD x
#                                       AXL_TLS. Answered by build-prefix.sh.
#   out/{bin,lib,include,share}         the STAGED SDK -- what
#                                       `install.sh --prefix` produces and what
#                                       a consumer actually consumes.
#
# AXL-Distribution-Design.md §4 is titled "a build directory is not an install
# prefix" and its P2 is the work of separating them. The two were conflated
# because they share a parent and a name, and that conflation is exactly why
# P2 and the CMake port's slice 3 looked like one 149-file sweep: measured,
# slice 3's surface is 155 make callers of which 139 already ask, P2's is the
# ~10 files below, and the genuine overlap is SEVEN files.
#
# So this is not a new indirection for its own sake -- it is the missing half
# of one that already exists, and it makes P2 a change to one script instead of
# a sweep. Same rationale as build-prefix.sh being a script rather than a
# sourced function: most callers here source nothing at all.
#
# Host-only: no QEMU, no build.
#
# Usage: ./test/integration/test-sdk-prefix.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
SDKP="$PROJECT_DIR/scripts/sdk-prefix.sh"

WORK="$(mktemp -d)"; cleanup() { rm -rf "$WORK"; }; trap cleanup EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

[[ -x "$SDKP" ]] || { echo "  FAIL: scripts/sdk-prefix.sh is not executable"; echo; echo "sdk-prefix: 0 passed, 1 failed"; exit 1; }

# 1. The default is today's staged location, so every existing caller and every
#    consumer instruction keeps working unchanged. A separation that moved the
#    default would be a breaking change wearing a refactor's clothes.
got="$("$SDKP")"
[[ "$got" == "out" ]] && pass "default is 'out' (unchanged from today)" \
                      || fail "default was '$got', expected 'out'"

# 2. --abs resolves against the repo, matching build-prefix.sh's flag exactly.
#    Same spelling on purpose: two accessors that disagree about their own flags
#    are worse than one accessor and a hardcoded path.
got="$("$SDKP" --abs)"
[[ "$got" == "$PROJECT_DIR/out" ]] && pass "--abs is repo-rooted" \
                                   || fail "--abs was '$got'"

# 3. The point of the whole exercise: P2 becomes ONE variable, not a sweep.
got="$(AXL_SDK_PREFIX=/opt/axl-sdk "$SDKP")"
[[ "$got" == "/opt/axl-sdk" ]] && pass "AXL_SDK_PREFIX relocates the staged SDK" \
                               || fail "override gave '$got'"

# 4. An override that is already absolute must not be re-rooted under the repo.
got="$(AXL_SDK_PREFIX=/opt/axl-sdk "$SDKP" --abs)"
[[ "$got" == "/opt/axl-sdk" ]] && pass "--abs leaves an absolute override alone" \
                               || fail "--abs of an absolute override gave '$got'"

# 5. A relative override IS re-rooted, so `AXL_SDK_PREFIX=stage` behaves like
#    the default rather than depending on the caller's cwd -- these callers run
#    from wherever the test harness put them.
got="$(AXL_SDK_PREFIX=stage "$SDKP" --abs)"
[[ "$got" == "$PROJECT_DIR/stage" ]] && pass "--abs re-roots a relative override" \
                                     || fail "relative override --abs gave '$got'"

# 6. It answers about the STAGED SDK, never the build directory. Asserted
#    because the two are one character apart in practice and the whole defect
#    being fixed is that they were treated as the same thing.
bp="$("$PROJECT_DIR/scripts/build-prefix.sh" x64)"
sp="$("$SDKP")"
[[ "$bp" != "$sp" ]] && pass "build dir ($bp) and staged SDK ($sp) are distinct" \
                     || fail "both accessors returned '$sp'"

# 7. Unknown flags are refused rather than silently treated as a prefix -- the
#    failure mode otherwise is a staged SDK at ./--abs.
if "$SDKP" --nonsense >/dev/null 2>&1; then
    fail "an unknown flag was accepted"
else
    pass "an unknown flag is refused"
fi

# 8. The callers actually moved. A new accessor nothing uses is not a
#    separation, it is a second way to spell the same hardcoded path -- and
#    this suite has been bitten by exactly that shape before.
#    `|| true` is load-bearing: grep -rl exits 1 when it matches NOTHING, and
#    under `set -euo pipefail` that killed this script at exactly the moment
#    the sweep succeeded -- a test that can only survive its own failure.
hard=$(grep -rlE '\$(\{)?PROJECT_DIR(\})?/out/(bin|lib|include)' \
         "$SCRIPT_DIR" --include='test-*.sh' 2>/dev/null | wc -l || true)
[[ "$hard" -eq 0 ]] && pass "no integration test hand-composes \$PROJECT_DIR/out/{bin,lib,include}" \
                    || { fail "$hard integration test(s) still hand-compose the staged path"
                         grep -rlE '\$(\{)?PROJECT_DIR(\})?/out/(bin|lib|include)' \
                           "$SCRIPT_DIR" --include='test-*.sh' 2>/dev/null | head -5 | sed 's/^/      /'; }

echo
echo "sdk-prefix: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
