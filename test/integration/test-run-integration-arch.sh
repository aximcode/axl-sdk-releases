#!/bin/bash
# test-meta: arch=none needs= est=15 local-only=0
# test-run-integration-arch.sh -- run-integration.sh's --arch takes either
# spelling of an arch and refuses anything else.
#
# WHY. --arch was stored as typed and compared against "AARCH64" and "X64"
# exactly. `--arch aa64` -- the spelling axl-build.sh, the build trees and
# every make-era habit use -- therefore built and ran the X64 tree, selected
# a test set matching NEITHER arch (213 tests, against 153 for AARCH64 and
# 340 for X64), labelled the result "(aa64)", and failed the pool's agent
# check, so every guest= test booted instead of leasing. It exited 0. A
# reported "AARCH64 suite green" was an x64 run twice (2026-11-09). `--arch
# bogus` exited 0 the same way.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="$SCRIPT_DIR/run-integration.sh"

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
# list ARCH -- the test names --list selects for ARCH, and its exit status
list() {
    local out rc
    out=$("$RUN" --arch "$1" --list 2>&1); rc=$?
    printf '%s\nrc=%s\n' "$out" "$rc"
}

echo "=== run-integration --arch spellings ==="
X64_LIST="$(list X64)"
AA_LIST="$(list AARCH64)"

# CONTROL: the two arches select different sets, so an equality below can
# fail -- and each canonical spelling itself succeeds.
check "X64 and AARCH64 select different tests (control)" \
    "$([[ "$X64_LIST" != "$AA_LIST" ]] && echo differ)" "differ"
check "--arch X64 --list exits 0 (control)" "$(tail -1 <<<"$X64_LIST")" "rc=0"
check "--arch AARCH64 --list exits 0 (control)" "$(tail -1 <<<"$AA_LIST")" "rc=0"

for a in x64 x86_64; do
    check "--arch $a selects exactly what X64 does" \
        "$([[ "$(list "$a")" == "$X64_LIST" ]] && echo same)" "same"
done
for a in aa64 aarch64 Aa64; do
    check "--arch $a selects exactly what AARCH64 does" \
        "$([[ "$(list "$a")" == "$AA_LIST" ]] && echo same)" "same"
done

out=$("$RUN" --arch bogus --list 2>&1); rc=$?
check "--arch bogus is refused with exit 2" "$rc" "2"
check "--arch bogus names the spellings it accepts" "$out" \
    "run-integration: --arch bogus: not an arch (X64 or AARCH64; x64, aa64 and aarch64 are accepted too)"

echo ""
echo "run-integration-arch: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
