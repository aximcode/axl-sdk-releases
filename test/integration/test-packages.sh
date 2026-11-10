#!/bin/bash
# test-meta: arch=x64 needs=podman,dpkg-deb,rpmbuild est=120 cpu=2 local-only=0 guest=none
# guest=none: it boots no firmware -- the qemu-* names in it are the package
# dependencies it checks, and its containers only install packages.
# test-meta: builds=ARCH=x64,BUILD=RELEASE,sdk
# test-packages.sh -- the .deb and .rpm, installed by the real package tools
# on the distributions they are for (AXL-Distribution-Design.md §17.5).
#
# WHY. The packages returned in 5.0.0 with a design whose whole point is how
# they coexist with everything else: versioned names (axl-sdk-<v>) side by
# side, the tarball's tree at /opt/axl-sdk-<v>, nothing in /usr/bin, their own
# alternatives link (/opt/axl-sdk-package) and a profile.d script that
# APPENDS to PATH so a per-user install.sh wins, a receipt that makes `axl`'s
# manager verbs refuse, and Conflicts/Replaces/Obsoletes for the 4.4.0
# `axl-sdk` whose /usr/bin/axl shadowed install.sh's. None of that is visible
# from the package file; all of it is in what dpkg/rpm and a login shell do
# with it. So each package is installed in debian:stable-slim / fedora:latest
# and walked through: replacing a 4.4.0-shaped axl-sdk, where its files land,
# PATH both ways, `axl` from the package root, a newer package beside it,
# removal of each, and nothing left behind.
#
# The real package for the version in VERSION (x64 SDK, to keep it inside the
# time limit); two fabricated ones built here with the SAME maintainer scripts:
# a newer axl-sdk-9.9.9 and the old `axl-sdk` 4.4.0 owning /usr/bin/axl.
# The weak dependencies are asserted in the metadata (exact Recommends);
# letting apt/dnf resolve them would download the whole QEMU stack.
#
# Usage: ./test/integration/test-packages.sh

set -u
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
set +e

WORK="$(mktemp -d)"
# shellcheck source=/dev/null
source "$(dirname "$0")/lib/podman-run.sh"
podman_owned_init
trap 'podman_owned_cleanup; rm -rf "$WORK"' EXIT

command -v podman >/dev/null 2>&1 \
    || { echo "SKIP: podman not available -- the packages are installed in containers"; exit 0; }

V="$(cat "$PROJECT_DIR/VERSION")"
FAKE_V="9.9.9"
MAKER="$PROJECT_DIR/scripts/make-release-assets.sh"
# A version that publishes no packages (a prerelease) has none to test: the
# driver's `assets` is the one place that is decided. 77 is SKIP.
ASSETS="$("$MAKER" assets)" || { echo "FAIL: make-release-assets.sh assets"; exit 1; }
if ! grep -q '\.deb$' <<<"$ASSETS"; then
    echo "SKIP: $V publishes no .deb/.rpm (a prerelease)"
    exit 77
fi
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then test_host_pass "$1"
    else test_host_fail "$1"; echo "      got:  $2"; echo "      want: $3"; fi
}

# The tree the packages are made from, declared in builds= above.
"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 BUILD=RELEASE sdk > /dev/null 2>&1 \
    || { test_host_fail "the x64 RELEASE build"; test_host_summary "packages"; exit 1; }

echo "=== building the packages ==="
for kind in deb rpm; do
    if "$MAKER" "$kind" --arch x64 --out "$WORK/real" > "$WORK/$kind.log" 2>&1; then
        test_host_pass "make-release-assets.sh $kind builds"
    else
        test_host_fail "make-release-assets.sh $kind builds"; tail -12 "$WORK/$kind.log"
        test_host_summary "packages"; exit 1
    fi
done
DEB="$WORK/real/$("$MAKER" deb --print-name)"
RPM="$WORK/real/$("$MAKER" rpm --print-name)"

# The weak dependencies and the replacement of 4.4.0, from the metadata.
check "the .deb Recommends the QEMU stack" "$(dpkg-deb -f "$DEB" Recommends)" \
    "qemu-system-x86, qemu-system-arm, ovmf, qemu-efi-aarch64, virtiofsd, mtools, dosfstools"
check "the .deb Conflicts with and Replaces the 4.4.0 axl-sdk" \
    "$(dpkg-deb -f "$DEB" Conflicts) / $(dpkg-deb -f "$DEB" Replaces)" "axl-sdk / axl-sdk"
check "the .rpm Recommends the QEMU stack" "$(rpm -qp --recommends "$RPM" 2>/dev/null | tr '\n' ' ')" \
    "dosfstools edk2-aarch64 edk2-ovmf mtools qemu-system-aarch64 qemu-system-x86 virtiofsd "
check "the .rpm Obsoletes the 4.4.0 axl-sdk" "$(rpm -qp --obsoletes "$RPM" 2>/dev/null)" "axl-sdk < 4.5"
# The payload IS the SDK archive's tree (one set of install rules), plus the
# package's receipt and profile.d script.
"$MAKER" sdk --arch x64 --out "$WORK/real" > "$WORK/sdk.log" 2>&1
TGZ="$WORK/real/$("$MAKER" sdk --print-name)"
tar tzf "$TGZ" | sed -e "s|^axl-sdk-$V/||" -e 's|/$||' | grep -v '^$' | LC_ALL=C sort > "$WORK/tgz.list"
dpkg-deb -c "$DEB" | awk '{print $6}' | sed -e "s|^\./opt/axl-sdk-$V/\{0,1\}||" -e 's|/$||' \
    | grep -v '^\./opt\|^\.$\|^$' | LC_ALL=C sort > "$WORK/deb.list"
check "the .deb holds the SDK archive's files, plus its receipt and profile.sh" \
    "$(comm -3 "$WORK/tgz.list" "$WORK/deb.list" | tr -d '\t' | tr '\n' ' ')" "share/axl/.axl-receipt share/axl/profile.sh "
rpm -qlp "$RPM" 2>/dev/null | sed -e "s|^/opt/axl-sdk-$V/\{0,1\}||" | grep -v '^$' \
    | LC_ALL=C sort > "$WORK/rpm.list"
check "the .rpm holds the SDK archive's files, plus its receipt and profile.sh" \
    "$(comm -3 "$WORK/tgz.list" "$WORK/rpm.list" | tr -d '\t' | tr '\n' ' ')" "share/axl/.axl-receipt share/axl/profile.sh "

# fake_tree DIR VERSION -- an SDK root with the real dispatcher, as a package
# would place it at /opt/axl-sdk-VERSION.
fake_tree() {
    local d="$1/opt/axl-sdk-$2"
    mkdir -p "$d/bin" "$d/share/axl" "$d/libexec/axl"
    install -m 0755 "$PROJECT_DIR/scripts/axl" "$d/bin/axl"
    echo "$2" > "$d/share/axl/version"
    install -m 0644 "$PROJECT_DIR/packaging/system/profile.sh" "$d/share/axl/profile.sh"
}
# scripts VERSION DIR -- the real maintainer scripts, for VERSION.
scripts() {
    local prio
    prio=$(( ${1%%.*} * 10000 + $(cut -d. -f2 <<<"$1") * 100 + ${1##*.} ))
    for s in postinst prerm; do
        sed -e "s/@AXL_VERSION@/$1/g" -e "s/@AXL_ALT_PRIORITY@/$prio/g" \
            "$PROJECT_DIR/packaging/system/$s.in" > "$2/$s"
        chmod 0755 "$2/$s"
    done
}
# The fabricated .debs.
mkdir -p "$WORK/fdeb/newer/DEBIAN" "$WORK/fdeb/old/DEBIAN" "$WORK/fdeb/old/usr/bin"
fake_tree "$WORK/fdeb/newer" "$FAKE_V"
printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=deb\n' > "$WORK/fdeb/newer/opt/axl-sdk-$FAKE_V/share/axl/.axl-receipt"
scripts "$FAKE_V" "$WORK/fdeb/newer/DEBIAN"
printf 'Package: axl-sdk-%s\nVersion: %s-1\nArchitecture: amd64\nMaintainer: test\nConflicts: axl-sdk\nReplaces: axl-sdk\nDescription: fixture\n' \
    "$FAKE_V" "$FAKE_V" > "$WORK/fdeb/newer/DEBIAN/control"
printf '#!/bin/sh\necho old\n' > "$WORK/fdeb/old/usr/bin/axl"; chmod 0755 "$WORK/fdeb/old/usr/bin/axl"
printf 'Package: axl-sdk\nVersion: 4.4.0-1\nArchitecture: amd64\nMaintainer: test\nDescription: fixture\n' \
    > "$WORK/fdeb/old/DEBIAN/control"
mkdir -p "$WORK/pkgs-deb" "$WORK/pkgs-rpm"
dpkg-deb --root-owner-group --build "$WORK/fdeb/newer" "$WORK/pkgs-deb/newer.deb" > /dev/null \
    && dpkg-deb --root-owner-group --build "$WORK/fdeb/old" "$WORK/pkgs-deb/old.deb" > /dev/null \
    || { test_host_fail "fixture .debs"; test_host_summary "packages"; exit 1; }
cp "$DEB" "$WORK/pkgs-deb/new.deb"

# The fabricated .rpms.
rpm_fake() {   # rpm_fake NAME VERSION OBSOLETES ROOTDIR SCRIPTSDIR|-
    local top="$WORK/rpmtop-$1" spec
    mkdir -p "$top/SPECS"
    spec="$top/SPECS/$1.spec"
    {
        echo "Name: $1"; echo "Version: $2"; echo "Release: 1"; echo "Summary: fixture"
        echo "License: none"; echo "BuildArch: x86_64"; echo "AutoReqProv: no"
        [[ -n "$3" ]] && echo "Obsoletes: $3"
        echo "%description"; echo "fixture"
        echo "%install"; echo "cp -a $4/. %{buildroot}/"
        if [[ "$5" != - ]]; then
            echo "%post"; cat "$5/postinst"; echo "%preun"; cat "$5/prerm"
        fi
        echo "%files"
        (cd "$4" && find . -mindepth 1 -type f | sed 's|^\.||')
        # Its directories too, which an rpm otherwise leaves behind on erase.
        (cd "$4" && find ./opt -mindepth 1 -type d 2>/dev/null | sed 's|^\.|%dir |')
    } > "$spec"
    rpmbuild --define "_topdir $top" --define "__os_install_post %{nil}" \
        --define "debug_package %{nil}" -bb "$spec" > "$top/log" 2>&1 || { tail -5 "$top/log"; return 1; }
    find "$top/RPMS" -name '*.rpm' | head -1
}
mkdir -p "$WORK/frpm/newer" "$WORK/frpm/old/usr/bin" "$WORK/frpm/scripts"
fake_tree "$WORK/frpm/newer" "$FAKE_V"
printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=rpm\n' > "$WORK/frpm/newer/opt/axl-sdk-$FAKE_V/share/axl/.axl-receipt"
scripts "$FAKE_V" "$WORK/frpm/scripts"
printf '#!/bin/sh\necho old\n' > "$WORK/frpm/old/usr/bin/axl"; chmod 0755 "$WORK/frpm/old/usr/bin/axl"
r_newer="$(rpm_fake "axl-sdk-$FAKE_V" "$FAKE_V" "axl-sdk < 4.5" "$WORK/frpm/newer" "$WORK/frpm/scripts")" \
    && r_old="$(rpm_fake axl-sdk 4.4.0 "" "$WORK/frpm/old" -)" \
    || { test_host_fail "fixture .rpms"; test_host_summary "packages"; exit 1; }
cp "$r_newer" "$WORK/pkgs-rpm/newer.rpm"; cp "$r_old" "$WORK/pkgs-rpm/old.rpm"
cp "$RPM" "$WORK/pkgs-rpm/new.rpm"
test_host_pass "fixture packages built (axl-sdk-$FAKE_V, axl-sdk 4.4.0)"

PKG_SH="$(cat "$PROJECT_DIR/scripts/container-pkg.sh")"
INNER="$(cat "$(dirname "$0")/lib/packages-in-container.sh")" \
    && [[ -n "$INNER" ]] || { test_host_fail "read lib/packages-in-container.sh"; test_host_summary "packages"; exit 1; }
for fam in deb rpm; do
    case "$fam" in
        deb) image=docker.io/library/debian:stable-slim ;;
        rpm) image=docker.io/library/fedora:latest ;;
    esac
    echo "=== $fam: $image ==="
    log="$WORK/$fam-container.log"
    podman_owned_run "$log" \
        -v "$WORK/pkgs-$fam:/pkgs:ro" \
        -v "$PROJECT_DIR/sdk/examples:/ex:ro" \
        -e "FAM=$fam" -e "V=$V" -e "FAKE_V=$FAKE_V" \
        "$image" sh -c "$PKG_SH
$INNER"
    crc=$?
    sed 's/^/    /' "$log"
    if grep -q 'AXL-PKG-FAIL' "$log"; then
        test_host_fail "$fam: the container could not install its tools (mirror or network -- see AXL-PKG-FAIL above)"
        continue
    fi
    n_pass="$(grep -c '^  PASS ' "$log" || true)"
    n_fail="$(grep -c '^  FAIL ' "$log" || true)"
    # EXACT: a step that never ran produces no FAIL.
    # The .rpm has two more: alternatives removed, and brought back.
    want=22; [[ "$fam" == rpm ]] && want=24
    check "$fam: every in-container assertion ran and passed (rc=$crc)" "$n_pass/$n_fail" "$want/0"
done

test_host_summary "packages"
