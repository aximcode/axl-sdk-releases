#!/bin/bash
# test-meta: arch=x64 needs= est=150 local-only=0
# test-meta: builds=ARCH=x64,BUILD=RELEASE,sdk,tools,release-drivers
# test-release-asset-names.sh -- THE RELEASE ASSETS ARE A CONTRACT.
#
# WHY. A consumer pins a version and the SHA-256 of what it downloads, and
# downloads BY EXACT NAME from
#   https://github.com/aximcode/axl-sdk-releases/releases/download/v<ver>/<asset>
# (axl-utils' install-axl-sdk.sh fetches `install.sh` and
# `axl-sdk-uefi-tools-<ver>-x64.tar.gz` that way; packaging/install.sh
# constructs the tarball names itself). When the archives moved to CPack
# (Build-System-Design §10.16), a CPack default -- `<name>-<version>-<system>`
# for the file, the file name for the top-level directory -- could rename one
# silently, and nothing downstream of a tag would say so until a consumer's
# download 404'd or extracted into the wrong directory.
#
# So this pins, for the version in VERSION:
#   1. the published set: what make-release-assets.sh names, plus the three
#      stable names, is EXACTLY v4.7.5's set with the version substituted;
#   2. each archive's top-level layout: one versioned root, and the entries
#      directly under it, against v4.7.5's (recorded below; the uefi-tools
#      .efi set is SHIPPED_TOOL_NAMES rather than v4.7.5's list, since tools
#      come and go -- and sshd.conf, added since, is named as the one
#      deliberate addition).
# Built for real: the host-tools archive, the x64-only SDK archive and the x64
# uefi-tools archive, through make-release-assets.sh exactly as release.yml
# runs it.
#
# Usage: ./test/integration/test-release-asset-names.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAKER="$PROJECT_DIR/scripts/make-release-assets.sh"

PASS=0; FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

# The trees the archives are made from, built first and declared above, so the
# runner builds them before deciding the cache and a cold tree does not spend
# this test's time limit.
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 BUILD=RELEASE sdk tools release-drivers \
        > /dev/null 2>&1; then
    echo "  FAIL: the x64 RELEASE build"; exit 1
fi

WORK="$(mktemp -d -t axl-assetnames.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT
V="$(cat "$PROJECT_DIR/VERSION")"
HOST="$(uname -m)"

echo "=== 1. the published asset set, for a release and a prerelease ==="
# v4.7.5's assets (gh release view v4.7.5 -R aximcode/axl-sdk-releases), the
# version replaced by @V@: the contract every later release keeps -- plus,
# from 5.0.0, the .deb and .rpm, which a PRERELEASE does not get (an RPM
# Version cannot hold a `-`). SHA256SUMS is written after the set is staged
# and is not part of it.
V475_ASSETS="axl-sdk-host-tools-@V@.tar.gz
axl-sdk-linux-@V@-x86_64.tar.gz
axl-sdk-uefi-tools-@V@-aa64.tar.gz
axl-sdk-uefi-tools-@V@-x64.tar.gz
install.sh
VERSION"
PKG_ASSETS="axl-sdk-@V@-@V@-1.x86_64.rpm
axl-sdk-@V@_@V@-1_amd64.deb"
expect() {   # expect VERSION with|without -- the contract's set for VERSION
    { sed "s/@V@/$1/g; s/x86_64/$HOST/" <<<"$V475_ASSETS"
      [[ "$2" == with ]] && sed "s/@V@/$1/g" <<<"$PKG_ASSETS"; } | LC_ALL=C sort
}
# THE ONE RULE is the driver's `assets`: release.yml and gate.check-asset-names
# ask it, so it is what is pinned, both ways.
check "a release (5.0.0) publishes eight assets: the six and the .deb/.rpm" \
    "$("$MAKER" assets --version 5.0.0)" "$(expect 5.0.0 with)"
check "a prerelease (5.0.0-rc1) publishes the six, no packages" \
    "$("$MAKER" assets --version 5.0.0-rc1)" "$(expect 5.0.0-rc1 without)"
# This tree's own VERSION follows the same rule, whichever it is.
if [[ "$V" == *-* ]]; then want="$(expect "$V" without)"; else want="$(expect "$V" with)"; fi
check "this tree's VERSION ($V) publishes the set its kind of version gets" "$("$MAKER" assets)" "$want"
# ...and each kind's --print-name agrees with it.
got="$( { "$MAKER" sdk --print-name
          "$MAKER" uefi-tools --arch x64 --print-name
          "$MAKER" uefi-tools --arch aa64 --print-name
          "$MAKER" host-tools --print-name
          [[ "$V" == *-* ]] || { "$MAKER" deb --print-name; "$MAKER" rpm --print-name; }
          printf '%s\n' install.sh VERSION; } | LC_ALL=C sort)"
check "each kind's --print-name names its member of that set" "$got" "$want"
# The gate holds release.yml's staging to the same set, for both kinds.
for gv in 5.0.0 5.0.0-rc1; do
    out="$(python3 "$PROJECT_DIR/scripts/check-asset-names.py" --version "$gv" 2>&1)"; rc=$?
    case "$gv" in *-*) np=0 ;; *) np=2 ;; esac
    check "gate.check-asset-names is clean for $gv ($np packages)" "$rc $out" \
        "0 check-asset-names: clean -- 4 archives and $np packages for $gv, 4 installer candidates"
done
# A PACKAGE FOR A PRERELEASE IS REFUSED, by name -- the driver is the rule's
# one owner, so its kinds hold to it too.
for kind in deb rpm; do
    out="$("$MAKER" "$kind" --version 5.0.0-rc1 --print-name 2>&1)"; rc=$?
    check "$kind for a prerelease is refused (rc 2, named)" "$rc $out" \
        "2 make-release-assets: no .deb/.rpm for a prerelease version (5.0.0-rc1)
usage: see 'make-release-assets.sh --help'"
done
# release.yml's release job stages exactly those files (its count guard and
# its cp list); gate.check-asset-names holds the cp list to the names above.
# The exact lines, as fixed strings (one holds a quote and a backslash).
ask='ASSETS="$(./scripts/make-release-assets.sh assets --version "$V")"'
guard="diff <(ls -1 stage | LC_ALL=C sort) <(printf '%s\\n' \"\$ASSETS\")"
n="$(grep -cF -- "$ask" "$PROJECT_DIR/.github/workflows/release.yml") $(grep -cF -- "$guard" "$PROJECT_DIR/.github/workflows/release.yml")"
check "release.yml asks the driver for the asset set (twice) and stages exactly it" "$n" "2 1"

# layout ARCHIVE -- the root, then each entry directly under it, sorted.
layout() {
    tar tzf "$1" > "$WORK/list" || { echo "UNREADABLE"; return; }
    awk -F/ '{print $1}' "$WORK/list" | LC_ALL=C sort -u | tr '\n' ' '
    echo "|"
    awk -F/ 'NF >= 2 && $2 != "" {print $2}' "$WORK/list" | LC_ALL=C sort -u | tr '\n' ' '
}
build() {   # build KIND ARGS... -- into $WORK/out, log on failure
    if ! "$MAKER" "$@" --out "$WORK/out" > "$WORK/build.log" 2>&1; then
        sed 's/^/      /' "$WORK/build.log" | tail -15
        return 1
    fi
}

echo "=== 2. host-tools: root and top level ==="
if build host-tools; then
    got="$(layout "$WORK/out/axl-sdk-host-tools-$V.tar.gz")"
    check "host-tools layout is v4.7.5's" "$got" \
        "axl-sdk-host-tools-$V |
CHANGELOG.md LICENSE NOTICE README.md VERSION bin libexec share "
else
    check "make-release-assets.sh host-tools succeeds" "failed" "0"
fi

echo "=== 3. SDK (x64 only, as CI's per-arch leg): root and top level ==="
if build sdk --arch x64; then
    got="$(layout "$WORK/out/axl-sdk-linux-$V-$HOST.tar.gz")"
    check "SDK layout is v4.7.5's" "$got" "axl-sdk-$V |
VERSION bin include lib libexec share "
else
    check "make-release-assets.sh sdk --arch x64 succeeds" "failed" "0"
fi

echo "=== 4. uefi-tools x64: root and top level ==="
if build uefi-tools --arch x64; then
    shipped="$("$PROJECT_DIR/scripts/axl-list.sh" SHIPPED_TOOL_NAMES | tr ' ' '\n' \
               | grep -v '^$' | sed 's/$/.efi/')"
    # v4.7.5's non-tool entries, plus sshd.conf (shipped since e6419f49).
    want_top="$(printf '%s\n' $shipped README.txt VERSION drivers jedec.json5 \
                pci-ids.json5 sshd.conf third_party usb-ids.json5 \
                | LC_ALL=C sort | tr '\n' ' ')"
    got="$(layout "$WORK/out/axl-sdk-uefi-tools-$V-x64.tar.gz")"
    check "uefi-tools layout is v4.7.5's (tools per SHIPPED_TOOL_NAMES)" "$got" \
        "axl-sdk-uefi-tools-$V-x64 |
$want_top"
else
    check "make-release-assets.sh uefi-tools --arch x64 succeeds" "failed" "0"
fi

echo ""
echo "release-asset-names: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
