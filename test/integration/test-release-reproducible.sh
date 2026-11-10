#!/bin/bash
# test-meta: arch=x64 needs=dpkg-deb est=180 local-only=0
# test-meta: builds=ARCH=x64,BUILD=RELEASE,sdk,tools,release-drivers
# test-release-reproducible.sh -- two builders, two filesystems, one archive.
#
# WHY. A consumer pins a release asset by SHA-256, and "the tarball was
# rebuilt" must not look like "the SDK changed". The archives come from
# `cpack -G External` (scripts/make-release-assets.sh, cmake/AxlPackTar.cmake;
# Build-System-Design §10.18) because CPack's own archive writer could not be
# made reproducible, in two ways this test can see:
#   - it records the builder's user and group NAMES (`mgosha` here, `runner`
#     on CI), and `tar x` as root restores ownership by name;
#   - it lists entries in the staging directory's READDIR order, which on ext4
#     is a hash with a per-filesystem seed: two runners, two archives.
#
# For each archive (host-tools, the x64 SDK, x64 uefi-tools), three builds:
#   A  this user, umask 022, staging on a freshly made ext4 filesystem
#   B  this user, umask 022, staging on ANOTHER freshly made ext4 filesystem
#   C  root in a user namespace (`unshare -r`; another HOME/USER where the
#      kernel refuses one), umask 077, staging on a TMPFS (/dev/shm) -- a
#      third filesystem TYPE, whose directory sizes differ from ext4's (the
#      class CPack's DEB writer put into Installed-Size)
# A, B and C must be byte-identical; every entry 0/0 with no owner names;
# every mtime the commit's. A against B is the readdir half: it needs root
# or passwordless sudo to make the two filesystems, and says SKIP for that
# half (only) where it has neither.
#
# The control: with CPACK_GENERATOR TGZ in place of External, A and B differ
# (scripts/sabotage.sh, recorded in §10.18).
#
# Usage: ./test/integration/test-release-reproducible.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAKER="$PROJECT_DIR/scripts/make-release-assets.sh"

PASS=0; FAIL=0; SKIP=0
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

WORK="$(mktemp -d -t axl-repro.XXXXXXXX)"
# Builder C stages on tmpfs where there is one.
TMPC="$(mktemp -d -p /dev/shm axl-repro.XXXXXXXX 2>/dev/null || mktemp -d -t axl-repro-c.XXXXXXXX)"
# Say what C's filesystem really is, rather than claim tmpfs.
C_FS="$(stat -f -c %T "$TMPC" 2>/dev/null || echo unknown)"
echo "builder C stages on $C_FS ($TMPC)"
MOUNTS=()
cleanup() {
    local m
    for m in "${MOUNTS[@]}"; do $SUDO umount "$m" 2>/dev/null; done
    rm -rf "$WORK" "$TMPC"
}
trap cleanup EXIT
V="$(cat "$PROJECT_DIR/VERSION")"
EPOCH="$(git -C "$PROJECT_DIR" log -1 --format=%ct)"

# TWO FRESH ext4 FILESYSTEMS, each with its own hash seed. Root, or
# passwordless sudo, mounts them; the build runs as this user inside.
SUDO=""
[[ "$(id -u)" -eq 0 ]] || SUDO="sudo -n"
EXT4=0
# NOT in a $(...): the mount must be recorded in THIS shell for the EXIT
# trap to unmount it (a first draft lost both mounts that way).
mk_ext4() {   # mk_ext4 NAME -- mount a fresh ext4 at $WORK/NAME, or fail
    local img="$WORK/$1.img" mnt="$WORK/$1"
    mkdir -p "$mnt"
    truncate -s 512M "$img" && mkfs.ext4 -q -F "$img" >/dev/null 2>&1 \
        && $SUDO mount -o loop "$img" "$mnt" 2>/dev/null || return 1
    MOUNTS+=("$mnt")
    $SUDO chown "$(id -u):$(id -g)" "$mnt" || return 1
}
if command -v mkfs.ext4 >/dev/null 2>&1 && { [[ -z "$SUDO" ]] || sudo -n true 2>/dev/null; }; then
    if mk_ext4 fsa && mk_ext4 fsb; then
        FS_A="$WORK/fsa" FS_B="$WORK/fsb"
        EXT4=1
        echo "staging A and B on two fresh ext4 filesystems"
    fi
fi
(( EXT4 )) || echo "no root/sudo or mkfs.ext4 here: the two-filesystem half is SKIPPED"

if unshare -r true 2>/dev/null; then
    OTHER=(unshare -r)
    echo "builder C: root in a user namespace (unshare -r), umask 077"
else
    OTHER=(env HOME="$WORK/otherhome" USER=nobody LOGNAME=nobody)
    mkdir -p "$WORK/otherhome"
    echo "builder C: this uid with another HOME/USER/LOGNAME (no user namespace here), umask 077"
fi

build() {   # build OUTDIR TMPDIR WHO KIND ARGS...
    local out="$1" tmp="$2" who="$3"; shift 3
    mkdir -p "$tmp"
    if [[ "$who" == me ]]; then
        (umask 022; TMPDIR="$tmp" "$MAKER" "$@" --out "$out") > "$WORK/build.log" 2>&1
    else
        (umask 077; TMPDIR="$tmp" "${OTHER[@]}" "$MAKER" "$@" --out "$out") > "$WORK/build.log" 2>&1
    fi || { sed 's/^/      /' "$WORK/build.log" | tail -12; return 1; }
}
sha() { sha256sum < "$1" | cut -d' ' -f1; }

for spec in "host-tools|axl-sdk-host-tools-$V.tar.gz" \
            "sdk --arch x64|axl-sdk-linux-$V-$(uname -m).tar.gz" \
            "uefi-tools --arch x64|axl-sdk-uefi-tools-$V-x64.tar.gz"; do
    args="${spec%%|*}" file="${spec##*|}"
    echo "=== $file ==="
    # shellcheck disable=SC2086  # $args is a deliberate word list
    build "$WORK/c" "$TMPC/tmp" other $args || { check "built as another uid" "failed" "ok"; continue; }
    c="$WORK/c/$file"
    if (( EXT4 )); then
        # shellcheck disable=SC2086
        if build "$WORK/a" "$FS_A/tmp" me $args && build "$WORK/b" "$FS_B/tmp" me $args; then
            check "byte-identical across two ext4 filesystems (readdir order)" \
                "$(sha "$WORK/a/$file")" "$(sha "$WORK/b/$file")"
            check "byte-identical across builders (uid, umask)" \
                "$(sha "$WORK/a/$file")" "$(sha "$c")"
        else
            check "built on both filesystems" "failed" "ok"
        fi
    else
        echo "  SKIP: byte-identical across two ext4 filesystems (no root/sudo)"; SKIP=$((SKIP + 1))
        build "$WORK/a" "$WORK/tmp-a" me $args \
            && check "byte-identical across builders (uid, umask)" "$(sha "$WORK/a/$file")" "$(sha "$c")"
    fi
    # Every entry's owner as tar prints it WITHOUT --numeric-owner: a name, if
    # the archive carries one, else the number. 0/0 means no names at all.
    owners="$(tar -tvzf "$c" | awk '{print $2}' | sort -u | tr '\n' ' ')"
    check "every entry is 0/0, with no owner names" "$owners" "0/0 "
    want="$(TZ=UTC0 date -d "@$EPOCH" '+%Y-%m-%d %H:%M:%S')"
    got_mt="$(TZ=UTC0 tar --full-time -tvzf "$c" | awk '{print $4" "$5}' | sort -u | tr '\n' ' ')"
    check "every mtime is the commit's (SOURCE_DATE_EPOCH)" "$got_mt" "$want "
    rm -rf "$WORK/a" "$WORK/b" "$WORK/c"
done

# THE PACKAGES (§17.5): the .deb through dpkg-deb (packaging/system/
# AxlPackDeb.cmake -- CPack's DEB writer put the staging filesystem's
# directory sizes into Installed-Size), the .rpm through CPack's RPM generator
# with rpmbuild's SOURCE_DATE_EPOCH macros and a fixed build host
# (packaging/system/CPackConfig-package.cmake.in). Held here the same way:
# A and B on two ext4 filesystems, C on tmpfs as another uid. Scope: one
# rpmbuild -- another rpm version may record its own version and defaults.
pkgs=()
if ! grep -q '\.deb$' <<<"$("$MAKER" assets)"; then
    # The driver's `assets` decides; a prerelease publishes no packages.
    echo "  SKIP: the .deb and .rpm ($V publishes none: a prerelease)"; SKIP=$((SKIP + 2))
else
    pkgs=(deb)
    command -v rpmbuild >/dev/null 2>&1 && pkgs+=(rpm) \
        || { echo "  SKIP: the .rpm (no rpmbuild here)"; SKIP=$((SKIP + 1)); }
fi
for kind in ${pkgs[@]+"${pkgs[@]}"}; do
    file="$("$MAKER" "$kind" --print-name)"
    echo "=== $file ==="
    build "$WORK/c" "$TMPC/tmp" other "$kind" --arch x64 || { check "built as another uid" "failed" "ok"; continue; }
    if (( EXT4 )); then
        if build "$WORK/a" "$FS_A/tmp" me "$kind" --arch x64 \
           && build "$WORK/b" "$FS_B/tmp" me "$kind" --arch x64; then
            check "byte-identical across two ext4 filesystems (readdir order)" \
                "$(sha "$WORK/a/$file")" "$(sha "$WORK/b/$file")"
            check "byte-identical across builders and filesystem types (uid, umask, ext4/$C_FS)" \
                "$(sha "$WORK/a/$file")" "$(sha "$WORK/c/$file")"
        else
            check "built on both filesystems" "failed" "ok"
        fi
    else
        echo "  SKIP: byte-identical across two ext4 filesystems (no root/sudo)"; SKIP=$((SKIP + 1))
        if build "$WORK/a" "$WORK/tmp-a" me "$kind" --arch x64; then
            check "byte-identical across builders (uid, umask)" \
                "$(sha "$WORK/a/$file")" "$(sha "$WORK/c/$file")"
        else
            check "built as this user" "failed" "ok"
        fi
    fi
    # Every payload mtime the commit's: dpkg-deb and rpmbuild only CLAMP to
    # SOURCE_DATE_EPOCH, so a file OLDER than the commit (a checkout's) would
    # otherwise keep its own -- and two clones give two packages.
    if [[ "$kind" == deb ]]; then
        if command -v dpkg-deb >/dev/null 2>&1; then
            got_mt="$(dpkg-deb --fsys-tarfile "$WORK/c/$file" | TZ=UTC0 tar --full-time -tvf - \
                      | awk '{print $4" "$5}' | sort -u | tr '\n' ' ')"
            check "every .deb payload mtime is the commit's" "$got_mt" \
                "$(TZ=UTC0 date -d "@$EPOCH" '+%Y-%m-%d %H:%M:%S') "
        fi
    else
        got_mt="$(rpm -qp --dump "$WORK/c/$file" 2>/dev/null | awk '{print $3}' | sort -u | tr '\n' ' ')"
        check "every .rpm payload mtime is the commit's" "$got_mt" "$EPOCH "
    fi
    rm -rf "$WORK/a" "$WORK/b" "$WORK/c"
done

echo ""
echo "release-reproducible: $PASS passed, $FAIL failed, $SKIP skipped"
[[ $FAIL -eq 0 ]]
