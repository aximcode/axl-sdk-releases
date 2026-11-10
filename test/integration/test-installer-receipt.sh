#!/bin/bash
# test-meta: arch=none needs= est=8 local-only=0
# test-installer-receipt.sh -- install.sh marks the SDK roots it places, and
# keeps its hands off the ones a package placed.
#
# WHY. AXL-Distribution-Design §21 reserved share/axl/ for an SDK-root receipt
# "until something reads it"; §17.5 (the .deb/.rpm returning) made something
# read it. A package installs /opt/axl-sdk-<ver> too, so the name alone no
# longer says who may remove a root: `axl prune` now removes an SDK root only
# on an install.sh receipt (test-axl-prune.sh), and install.sh itself must
# never extract over, select, or uninstall a root dpkg/rpm owns -- the
# package database would go on listing files that are gone.
#
# Runs packaging/install.sh against a file:// release built by
# release-fixture.sh; no network, no container.
#
# Usage: ./test/integration/test-installer-receipt.sh

set -u
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
set +e

INSTALLER="$PROJECT_DIR/packaging/install.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
source "$(dirname "$0")/release-fixture.sh"

run_installer() {  # run_installer <log> <args...>
    local log="$1"; shift
    HOME="$WORK/home" timeout 60 sh "$INSTALLER" --yes "$@" > "$log" 2>&1
}
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        test_host_pass "$1"
    else
        test_host_fail "$1"; echo "      got:  $2"; echo "      want: $3"
    fi
}

V="9.9.9"
REL="$WORK/rel"
publish "$REL" "axl-sdk-linux-$V-x86_64.tar.gz"  "axl-sdk-$V"            "$V"
publish "$REL" "axl-sdk-host-tools-$V.tar.gz"    "axl-sdk-host-tools-$V" "$V" make_manager_tree
seal    "$REL" "$V"
PREFIX="$WORK/home/.local/share"
BIN="$WORK/home/.local/bin"
mkdir -p "$PREFIX" "$BIN"

echo "=== an install.sh root carries its receipt ==="
run_installer "$WORK/sdk.log" --base-url "file://$REL" --prefix "$PREFIX" --bin-dir "$BIN"
check "install.sh installs the fixture SDK" "$?" "0"
R="$PREFIX/axl-sdk-$V/share/axl/.axl-receipt"
SHA="$(awk -v n="axl-sdk-linux-$V-x86_64.tar.gz" '$2 == n {print $1}' "$REL/SHA256SUMS")"
# Every line but the timestamp, exactly; the timestamp by shape.
check "the receipt names its kind, owner, digest and TLS state" \
    "$(grep -v '^AXL_RECEIPT_INSTALLED=' "$R" 2>/dev/null)" \
    "AXL_RECEIPT_KIND=sdk
AXL_RECEIPT_SOURCE=install.sh
AXL_RECEIPT_SHA256=$SHA
AXL_RECEIPT_INSECURE_FETCH=0"
check "...and when it was installed (UTC)" \
    "$(grep -cE '^AXL_RECEIPT_INSTALLED=[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$' "$R" 2>/dev/null)" "1"
check "the digest is a real one (control: the fixture's SHA256SUMS named the asset)" \
    "$([[ "$SHA" =~ ^[0-9a-f]{64}$ ]] && echo sha || echo "'$SHA'")" "sha"

echo "=== a package's root is not install.sh's to touch ==="
PKGV="9.8.0"
mkdir -p "$PREFIX/axl-sdk-$PKGV/bin" "$PREFIX/axl-sdk-$PKGV/share/axl"
echo "$PKGV" > "$PREFIX/axl-sdk-$PKGV/share/axl/version"
printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=deb\n' > "$PREFIX/axl-sdk-$PKGV/share/axl/.axl-receipt"
before="$(readlink "$PREFIX/axl-sdk")"
run_installer "$WORK/use.log" --use "$PKGV" --base-url "file://$REL" --prefix "$PREFIX" --bin-dir "$BIN"
check "--use of a .deb's root is refused" "$?" "1"
check "...naming the package manager's command" \
    "$(grep -F 'install.sh:' "$WORK/use.log" | tail -1)" \
    "install.sh: $PREFIX/axl-sdk-$PKGV belongs to the axl-sdk-$PKGV .deb, which apt manages -- install.sh will not select it (remove it with: sudo apt remove axl-sdk-$PKGV)"
check "...and current did not move" "$(readlink "$PREFIX/axl-sdk")" "$before"
run_installer "$WORK/un.log" --uninstall --version "$PKGV" --prefix "$PREFIX" --bin-dir "$BIN"
check "--uninstall of a .deb's root is refused" "$?" "1"
check "...and the root is still there" "$([[ -d "$PREFIX/axl-sdk-$PKGV/bin" ]] && echo yes)" "yes"
printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=rpm\n' > "$PREFIX/axl-sdk-$PKGV/share/axl/.axl-receipt"
REL2="$WORK/rel2"
publish "$REL2" "axl-sdk-linux-$PKGV-x86_64.tar.gz" "axl-sdk-$PKGV" "$PKGV"
seal    "$REL2" "$PKGV"
run_installer "$WORK/over.log" --force --version "$PKGV" --base-url "file://$REL2" \
    --prefix "$PREFIX" --bin-dir "$BIN"
check "installing over an .rpm's root is refused" "$?" "1"
check "...and the root keeps its rpm receipt" \
    "$(sed -n 's/^AXL_RECEIPT_SOURCE=//p' "$PREFIX/axl-sdk-$PKGV/share/axl/.axl-receipt")" "rpm"

echo "=== the refusal comes BEFORE the download ==="
# Against a mirror with NOTHING in it: refused before the download, the
# package's refusal is the message; refused after it, the run would die on
# the missing SHA256SUMS first and never say why.
mkdir -p "$WORK/emptyrel"
printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=deb\n' > "$PREFIX/axl-sdk-$PKGV/share/axl/.axl-receipt"
run_installer "$WORK/early.log" --force --version "$PKGV" --base-url "file://$WORK/emptyrel" \
    --prefix "$PREFIX" --bin-dir "$BIN"
check "installing over a .deb's root against an empty mirror is refused" "$?" "1"
check "...by the package refusal, not a download failure" \
    "$(grep -F 'install.sh:' "$WORK/early.log" | tail -1)" \
    "install.sh: $PREFIX/axl-sdk-$PKGV belongs to the axl-sdk-$PKGV .deb, which apt manages -- install.sh will not replace it (remove it with: sudo apt remove axl-sdk-$PKGV)"
check "...and nothing was fetched" "$(grep -c 'SHA256SUMS' "$WORK/early.log")" "0"

echo "=== a receipt this user cannot read ==="
if [[ "$(id -u)" -ne 0 ]]; then
    chmod 000 "$PREFIX/axl-sdk-$PKGV/share/axl/.axl-receipt"
    run_installer "$WORK/unr.log" --use "$PKGV" --base-url "file://$REL" --prefix "$PREFIX" --bin-dir "$BIN"
    rc=$?
    chmod 644 "$PREFIX/axl-sdk-$PKGV/share/axl/.axl-receipt"
    check "--use of a root whose receipt cannot be read is refused" "$rc" "1"
    check "...saying so, not treating it as unowned" \
        "$(grep -F 'install.sh:' "$WORK/unr.log" | tail -1)" \
        "install.sh: $PREFIX/axl-sdk-$PKGV has a receipt this user cannot read -- install.sh will not select a root whose owner it cannot see"
else
    echo "  SKIP: an unreadable receipt (running as root, which reads anything)"
fi

test_host_summary "installer-receipt"
