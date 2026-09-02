#!/bin/bash
# test-meta: arch=none needs= est=5 local-only=0
# test-published-release-check.sh — the post-publish check must SEE.
#
# WHY THIS EXISTS. scripts/check-published-release.sh is the only thing in the
# tree that looks at the bytes a consumer receives (§16.2, job 2). It cannot
# gate -- by the time it can fetch anything the release is public -- so its
# entire value is catching, in minutes, what would otherwise be found by a
# user. A check like that is worth exactly nothing until it has been shown to
# fail, because a silent pass and a broken detector are the same output.
#
# So each failure it exists for is INJECTED here, over `--base-url file://`,
# and the check must name it:
#
#   - a corrupted asset                (the truncated upload)
#   - an asset listed but not present  (the upload that did not finish)
#   - an asset present but unlisted    (the rename that missed SHA256SUMS)
#   - an asset listed but not attached (the rename that missed the upload)
#   - a VERSION that disagrees with the tag
#   - no SHA256SUMS at all
#
# The third and fourth are the pair §16.2 calls "the only check that can see a
# rename that missed one asset", and they fail in OPPOSITE directions, so both
# are asserted rather than one standing in for the other.
#
# Host-only, no network, no QEMU.
#
# Usage: ./test/integration/test-published-release-check.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

CHECK="$PROJECT_DIR/scripts/check-published-release.sh"
V="9.9.9"; TAG="v$V"
WORK="$(mktemp -d -t axl-relchk.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

check() {
    if [[ "$1" -eq 0 ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi
    return 0
}

# mk_release <dir> — a well-formed release: the six assets of §14.1a.
mk_release() {
    local d="$1"; mkdir -p "$d"
    printf 'sdk payload\n'         > "$d/axl-sdk-linux-$V-x86_64.tar.gz"
    printf 'host tools payload\n'  > "$d/axl-sdk-host-tools-$V.tar.gz"
    printf 'uefi x64 payload\n'    > "$d/axl-sdk-uefi-tools-$V-x64.tar.gz"
    printf 'uefi aa64 payload\n'   > "$d/axl-sdk-uefi-tools-$V-aa64.tar.gz"
    printf '#!/bin/sh\nmain() { :; }\nmain "$@"\n' > "$d/install.sh"
    printf '%s\n' "$V"             > "$d/VERSION"
    ( cd "$d" && sha256sum -- * > SHA256SUMS )
    ( cd "$d" && ls -1 | grep -v '^SHA256SUMS$' > "$d/.attached" )
    echo "SHA256SUMS" >> "$d/.attached"
}

# run_check <dir> -> OUT, RC. The attached-asset list is fed from the fixture
# rather than `gh`, so this needs no network and no real release.
run_check() {
    OUT="$(timeout 60 "$CHECK" "$TAG" --base-url "file://$1" \
           --asset-list "$1/.attached" 2>&1)"
    RC=$?
}

echo "=== post-publish release check ==="
echo ""

# ---- 1. a well-formed release passes ------------------------------------
GOOD="$WORK/good"; mk_release "$GOOD"
run_check "$GOOD"
[[ "$RC" -eq 0 ]]
check $? "a well-formed release passes (rc=$RC)"
if [[ "$RC" -ne 0 ]]; then printf '%s\n' "$OUT" | sed 's/^/      /' | tail -8; fi

# ---- 2. a corrupted asset ------------------------------------------------
D="$WORK/corrupt"; mk_release "$D"
printf 'x' >> "$D/axl-sdk-linux-$V-x86_64.tar.gz"
run_check "$D"
[[ "$RC" -ne 0 ]] && grep -q "axl-sdk-linux-$V-x86_64.tar.gz(hash)" <<<"$OUT"
check $? "a corrupted asset is caught and NAMED (rc=$RC)"

# ---- 3. listed but not present (the upload that did not finish) ---------
D="$WORK/missing"; mk_release "$D"
rm -f "$D/axl-sdk-host-tools-$V.tar.gz"
run_check "$D"
[[ "$RC" -ne 0 ]] && grep -q "axl-sdk-host-tools-$V.tar.gz(404)" <<<"$OUT"
check $? "an asset listed but not downloadable is caught and NAMED (rc=$RC)"

# ---- 4 & 5. the rename that missed one place, in both directions --------
# SHA256SUMS covers it, the release does not carry it.
D="$WORK/unattached"; mk_release "$D"
grep -v "axl-sdk-uefi-tools-$V-aa64.tar.gz" "$D/.attached" > "$D/.attached.new"
mv "$D/.attached.new" "$D/.attached"
run_check "$D"
[[ "$RC" -ne 0 ]] && grep -q "attached asset set and SHA256SUMS disagree" <<<"$OUT" \
                  && grep -q "axl-sdk-uefi-tools-$V-aa64.tar.gz" <<<"$OUT"
check $? "an asset checksummed but not attached is caught (rc=$RC)"

# The release carries it, SHA256SUMS does not cover it.
D="$WORK/unsummed"; mk_release "$D"
printf 'stray\n' > "$D/axl-sdk-uefi-tools-$V-riscv.tar.gz"
echo "axl-sdk-uefi-tools-$V-riscv.tar.gz" >> "$D/.attached"
run_check "$D"
[[ "$RC" -ne 0 ]] && grep -q "attached asset set and SHA256SUMS disagree" <<<"$OUT" \
                  && grep -q "axl-sdk-uefi-tools-$V-riscv.tar.gz" <<<"$OUT"
check $? "an asset attached but not checksummed is caught (rc=$RC)"

# ---- 6. VERSION disagreeing with the tag --------------------------------
D="$WORK/badversion"; mk_release "$D"
printf '1.2.3\n' > "$D/VERSION"
( cd "$D" && sha256sum -- * > SHA256SUMS )   # re-seal, so the HASH is right
run_check "$D"
[[ "$RC" -ne 0 ]] && grep -q "VERSION says '1.2.3'" <<<"$OUT"
check $? "a VERSION that disagrees with the tag is caught (rc=$RC)"

# ---- 7. no SHA256SUMS at all --------------------------------------------
D="$WORK/nosums"; mk_release "$D"; rm -f "$D/SHA256SUMS"
run_check "$D"
[[ "$RC" -ne 0 ]] && grep -q "SHA256SUMS is not downloadable" <<<"$OUT"
check $? "a release with no SHA256SUMS is caught (rc=$RC)"

test_host_summary "published-release-check"
