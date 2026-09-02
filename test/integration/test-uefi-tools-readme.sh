#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-uefi-tools-readme.sh -- the README inside the UEFI-tools tarball.
#
# WHY THIS EXISTS. That file said "Tools included:" and then listed **13 of
# 38**. AXL-Distribution-Design.md §14.3 counted it: the shipped tool set is
# stated in four places, two are derived and correct for free, and the two
# hand-maintained ones had both drifted. README.md's copy was fixed last cycle
# by adding the missing rows behind `make check-tool-docs`; this one is fixed
# by deriving it, because unlike README.md's table its entries are flat
# ~40-character labels -- exactly what devkit.conf's gated `desc:` lines are.
#
# It was also assembled INLINE in release.yml, so nothing could check it
# without cutting a release. Same defect, and the same fix, as the host-tools
# tarball: the text now comes from scripts/make-uefi-tools-readme.py, which
# release.yml and this test both call.
#
# WHAT IS PINNED, and the second one matters more than the tool list:
#
#   - EVERY shipped tool appears, with a description. A generator that emits
#     an empty or partial list is the defect that started this.
#   - THE LICENCE TEXT SURVIVES REGENERATION. The file carries mbedTLS's
#     Apache-2.0 election, EDK2's BSD-2-Clause-Patent notice, iPXE's
#     GPL-2.0-or-later notice and a **GPL-2.0 §3(b) written offer**. Moving
#     ~90 lines of heredoc into a generator is precisely the kind of refactor
#     that silently drops a paragraph, and dropping that one is a licence
#     violation rather than a typo.
#
# Host-only: no QEMU, no compiler, no build. It reads devkit.conf and a make
# variable and prints text.
#
# Usage: ./test/integration/test-uefi-tools-readme.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

GEN="$PROJECT_DIR/scripts/make-uefi-tools-readme.py"

check() {
    if [[ "$1" -eq 0 ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi
    return 0
}

WORK="$(mktemp -d -t axl-utreadme.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "=== UEFI-tools README ==="
echo ""

if [[ ! -x "$GEN" ]]; then
    test_host_fail "scripts/make-uefi-tools-readme.py is executable"
    test_host_summary "uefi-tools-readme"
    exit 1
fi
test_host_pass "scripts/make-uefi-tools-readme.py is executable"

OUT="$WORK/README.txt"
if ! "$GEN" --arch x64 --version 9.9.9 > "$OUT" 2> "$WORK/err.log"; then
    test_host_fail "generates a README for x64"
    sed 's/^/      /' "$WORK/err.log" | tail -8
    test_host_summary "uefi-tools-readme"
    exit 1
fi
test_host_pass "generates a README for x64"

# ---------------------------------------------------------------------------
# Every shipped tool, with a description.
# ---------------------------------------------------------------------------
mapfile -t TOOLS < <(make -s -C "$PROJECT_DIR" print-TOOL_NAMES | tr ' ' '\n' | grep -v '^$')

# A guard that checks nothing passes forever, so refuse a suspiciously short
# answer the way release.yml's own tool-set check does.
[[ "${#TOOLS[@]}" -ge 25 ]]
check $? "print-TOOL_NAMES named ${#TOOLS[@]} tools (>= 25 expected)"

missing=""
undescribed=""
for t in "${TOOLS[@]}"; do
    line="$(grep -E "^  +${t}\.efi +[^ ]" "$OUT" | head -1)"
    if [[ -z "$line" ]]; then
        missing="$missing $t"
    elif [[ -z "$(sed -E "s/^ +${t}\.efi +//" <<< "$line" | tr -d '[:space:]')" ]]; then
        undescribed="$undescribed $t"
    fi
done
[[ -z "$missing" ]]
check $? "all ${#TOOLS[@]} shipped tools are listed${missing:+ -- missing:$missing}"
[[ -z "$undescribed" ]]
check $? "every listed tool has a description${undescribed:+ -- blank:$undescribed}"

# ...and nothing listed that is not shipped. A stale hand-written entry for a
# deleted tool is the same class of defect pointing the other way.
listed_extra=""
while read -r name; do
    [[ -n "$name" ]] || continue
    found=0
    for t in "${TOOLS[@]}"; do [[ "$t" == "$name" ]] && { found=1; break; }; done
    (( found )) || listed_extra="$listed_extra $name"
done < <(sed -nE 's/^  +([a-z0-9-]+)\.efi .*/\1/p' "$OUT")
[[ -z "$listed_extra" ]]
check $? "nothing listed that is not shipped${listed_extra:+ -- extra:$listed_extra}"

# ---------------------------------------------------------------------------
# The licence payload. These are obligations, not prose.
# ---------------------------------------------------------------------------
declare -A OBLIGATION=(
    ["GPL-2\\.0 .?3\\(b\\) written offer"]="iPXE's GPL-2.0 section 3(b) written offer"
    ["dual-licensed Apache-2.0 OR GPL-2.0-or-later"]="mbedTLS's dual licence"
    ["this[[:space:]]*distribution elects Apache-2.0"]="mbedTLS Apache-2.0 election"
    ["BSD-2-Clause-Patent"]="EDK2's BSD-2-Clause-Patent notice"
    ["GPL-2.0-or-later"]="iPXE's licence"
    ["third_party/ipxe/COPYING.GPLv2"]="the iPXE licence-text pointer"
    ["support@aximcode.com"]="the address the written offer names"
)
for pat in "${!OBLIGATION[@]}"; do
    tr '\n' ' ' < "$OUT" | grep -Eq "$pat"
    check $? "carries ${OBLIGATION[$pat]}"
done

# ---------------------------------------------------------------------------
# Arch and version reach the text they parameterise.
# ---------------------------------------------------------------------------
grep -q 'drivers/x64/' "$OUT"
check $? "names drivers/x64/ for --arch x64"
grep -q '9.9.9' "$OUT"
check $? "carries the version it was given"

"$GEN" --arch aa64 --version 9.9.9 > "$WORK/aa64.txt" 2>/dev/null
grep -q 'drivers/aa64/' "$WORK/aa64.txt" && ! grep -q 'drivers/x64/' "$WORK/aa64.txt"
check $? "--arch aa64 names drivers/aa64/ and not drivers/x64/"

test_host_summary "uefi-tools-readme"
