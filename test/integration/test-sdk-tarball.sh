#!/bin/bash
# test-meta: arch=none needs= est=25 local-only=0
# test-sdk-tarball.sh -- the SDK ships as a relocatable tarball.
#
# WHY THIS EXISTS. AXL-SDK-Design.md documented this workflow --
#
#     tar xf axl-sdk-x64-linux.tar.gz
#     ./bin/axl-cc hello.c -o hello.efi
#
# -- in the present tense, and it had NEVER been built. The release publishes
# axl-sdk-tools-<arch>.tar.gz (target .efi utilities) and
# axl-sdk-host-tools.tar.gz (helper scripts), but nothing containing the SDK
# prefix itself. AXL-Distribution-Design.md §5.1 called it "the gap": Arch,
# Alpine, NixOS, SUSE, CI containers and locked-down corporate hosts have no
# supported install path, and a version pin can only be held by keeping a whole
# source checkout -- which is why ~/axl-sdk-2.2.0, ~/axl-sdk-2.2.1 and
# ~/axl-sdk-2.9.0 on this machine are full source trees, not prefixes.
#
# WHAT IS PINNED. That the archive is RELOCATABLE and COMPLETE, proved the only
# way that means anything: extract it somewhere it has never been and compile a
# real .efi with the axl-cc inside it. A file listing would pass against an
# archive whose contents cannot actually build -- and everything in here is
# $0-relative precisely so that this works, so the assertion should exercise it
# rather than trust it.
#
# The single top-level directory is `axl-sdk-<version>`, which is also the
# versioned root of §12.2: extract into /opt and you have /opt/axl-sdk-<ver>
# with no rename step.
#
# Host-only: no QEMU. Runs a real install.sh for both target arches, so it is
# the slowest host test here; that is the price of proving the archive builds.
#
# Usage: ./test/integration/test-sdk-tarball.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

MAKER="$PROJECT_DIR/scripts/make-sdk-tarball.sh"
VERSION="$(cat "$PROJECT_DIR/VERSION")"

WORK="$(mktemp -d -t axl-sdktar.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "=== SDK tarball ==="
echo ""

if [[ ! -x "$MAKER" ]]; then
    test_host_fail "scripts/make-sdk-tarball.sh is executable"
    test_host_summary "sdk-tarball"
    exit 1
fi
test_host_pass "scripts/make-sdk-tarball.sh is executable"

OUT="$WORK/dist"
if ! "$MAKER" --out "$OUT" > "$WORK/build.log" 2>&1; then
    test_host_fail "make-sdk-tarball.sh succeeds"
    tail -12 "$WORK/build.log" | sed 's/^/      /'
    test_host_summary "sdk-tarball"
    exit 1
fi
test_host_pass "make-sdk-tarball.sh succeeds"

# The name carries the HOST platform, not the target: one archive holds both
# target arches, per §5.3's lean that a per-target split is not worth its
# packaging cost on its own.
TARBALL="$OUT/axl-sdk-linux-${VERSION}-x86_64.tar.gz"
if [[ -f "$TARBALL" ]]; then
    test_host_pass "produces axl-sdk-linux-${VERSION}-x86_64.tar.gz"
else
    test_host_fail "produces axl-sdk-linux-${VERSION}-x86_64.tar.gz"
    ls -la "$OUT" 2>/dev/null | sed 's/^/      /'
    test_host_summary "sdk-tarball"
    exit 1
fi

# ── exactly one top-level directory, named for the version ────
#
# A tarball that unpacks loose files into the caller's cwd is the classic
# tarbomb, and here it would also defeat §12.2: the whole point of the name is
# that `tar xf ... -C /opt` yields /opt/axl-sdk-<ver> with nothing to rename.
#
# List ONCE into a file and grep the file. `tar tzf ... | grep -q` looks
# obvious and is a trap under `set -o pipefail`: grep -q exits at the first
# match, tar takes SIGPIPE, and the pipeline reports FAILURE for a match that
# succeeded. It misfires only for entries early in the listing, so `bin/axl`
# was reported missing from an archive it was plainly in while
# `share/axl/version` -- the last entry -- passed.
LISTING="$WORK/listing.txt"
tar tzf "$TARBALL" > "$LISTING"

TOPS="$(awk -F/ '{print $1}' "$LISTING" | sort -u)"
if [[ "$TOPS" == "axl-sdk-${VERSION}" ]]; then
    test_host_pass "one top-level directory, axl-sdk-${VERSION}"
else
    test_host_fail "one top-level directory, axl-sdk-${VERSION}"
    echo "      got: $(echo "$TOPS" | tr '\n' ' ')"
fi

# ── completeness, by path ─────────────────────────────────────
#
# libaxl-standin.a is listed for a reason the rest of this loop does not need:
# the extract-and-compile below proves the archive can build, and that covers
# everything a normal link reaches -- libaxl.a, the crt0 objects, the specs
# file. It does NOT reach the stand-in, because it builds with the bare-metal
# toolchain and the stand-in only ever appears on an AXL_TOOLCHAIN=host link
# (AXL-Host-Toolchain-Design.md §5.3). So the one artifact the working proof
# cannot see is the one that needs its own assertion, per arch. That is the
# shape of the defect this release fixes: install.sh staged all three uefi-tools
# sidecars, release.yml staged one, and nothing compared them.
for want in \
    "axl-sdk-${VERSION}/bin/axl" \
    "axl-sdk-${VERSION}/bin/axl-cc" \
    "axl-sdk-${VERSION}/bin/axl-c++" \
    "axl-sdk-${VERSION}/lib/axl/x64/libaxl.a" \
    "axl-sdk-${VERSION}/lib/axl/aa64/libaxl.a" \
    "axl-sdk-${VERSION}/lib/axl/x64/libaxl-standin.a" \
    "axl-sdk-${VERSION}/lib/axl/aa64/libaxl-standin.a" \
    "axl-sdk-${VERSION}/lib/cmake/axl/axl-config.cmake" \
    "axl-sdk-${VERSION}/lib/pkgconfig/axl.pc" \
    "axl-sdk-${VERSION}/libexec/axl/rsod-decode.py" \
    "axl-sdk-${VERSION}/libexec/axl/install.sh" \
    "axl-sdk-${VERSION}/share/axl/version" \
    "axl-sdk-${VERSION}/VERSION" ; do
    if grep -qxF -- "$want" "$LISTING"; then
        test_host_pass "contains ${want#axl-sdk-${VERSION}/}"
    else
        test_host_fail "contains ${want#axl-sdk-${VERSION}/}"
    fi
done

# ── the licence payload the .deb/.rpm used to carry ───────────
#
# The packages staged 75 doc files, including five third-party licence sets,
# and D2 retires the packages. These are OBLIGATIONS, not documentation:
# mbedTLS, DejaVu and edk2 are Apache-2.0 / Bitstream / BSD-2-Clause-Patent
# §4(a)-style "carry the licence with any binary redistribution"; libvterm is
# MIT and offers no public-domain election; FreeType's FTL carries a CREDIT
# clause for products shipping the path-filling APIs. All five are compiled
# INTO libaxl.a, so the archive that ships libaxl.a is the one that owes them.
#
# NOTICE is Apache-2.0 4(d) and was itself once omitted from the package.
for want in \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/LICENSE" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/NOTICE" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/THIRD_PARTY.md" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/CHANGELOG.md" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/README.md" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/examples/hello.c" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/examples/hello.cpp" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/examples/containers.cpp" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/examples/embed-asset.txt" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/third_party/mbedtls/LICENSE" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/third_party/dejavu/LICENSE" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/third_party/libvterm/LICENSE" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/third_party/freetype/FTL.TXT" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/third_party/freetype/LICENSE.TXT" \
    "axl-sdk-${VERSION}/share/doc/axl-sdk/third_party/edk2/LICENSE" ; do
    if grep -qxF -- "$want" "$LISTING"; then
        test_host_pass "carries ${want#axl-sdk-${VERSION}/share/doc/axl-sdk/}"
    else
        test_host_fail "carries ${want#axl-sdk-${VERSION}/share/doc/axl-sdk/}"
    fi
done

# embed-asset.txt above is not source but is an INPUT: embed-asset.c documents
# it as the file its `--embed` flag bundles, so the example does not build
# without it. The keep-list is deny-by-default, and it dropped this one.
#
# The examples ship as SOURCE. A dirty checkout leaves .efi and .o files in
# sdk/examples, and shipping build output from the maintainer's machine in a
# licence directory is how a staging path escapes into a release.
if grep -E "^axl-sdk-${VERSION}/share/doc/axl-sdk/examples/.*\.(efi|o|a|so)$" "$LISTING" > /dev/null; then
    test_host_fail "the examples carry no build artifacts"
else
    test_host_pass "the examples carry no build artifacts"
fi

# ── the assertion that actually means something ───────────────
#
# Extract somewhere the archive has never been and BUILD with it. Everything
# in the prefix resolves relative to $0 so that this works; a file listing
# would pass against an archive that cannot compile.
EXTRACT="$WORK/elsewhere"
mkdir -p "$EXTRACT"
if tar xzf "$TARBALL" -C "$EXTRACT"; then
    test_host_pass "extracts cleanly"
else
    test_host_fail "extracts cleanly"
fi
SDK="$EXTRACT/axl-sdk-${VERSION}"

GOT_VER="$("$SDK/bin/axl" --print-version 2>&1)"
if [[ "$GOT_VER" == "$VERSION" ]]; then
    test_host_pass "the extracted axl reports $VERSION"
else
    test_host_fail "the extracted axl reports $VERSION (got '$GOT_VER')"
fi
GOT_PFX="$("$SDK/bin/axl" --print-prefix 2>&1)"
if [[ "$GOT_PFX" == "$SDK" ]]; then
    test_host_pass "--print-prefix resolves to where it was extracted"
else
    test_host_fail "--print-prefix resolves to where it was extracted"
    echo "      wanted '$SDK', got '$GOT_PFX'"
fi

cat > "$WORK/hello.c" <<'EOF'
#include <axl.h>
int main(void) { axl_printf("hello\n"); return 0; }
EOF
if "$SDK/bin/axl-cc" "$WORK/hello.c" -o "$WORK/hello.efi" > "$WORK/cc.log" 2>&1; then
    test_host_pass "the extracted axl-cc compiles a program"
else
    test_host_fail "the extracted axl-cc compiles a program"
    tail -8 "$WORK/cc.log" | sed 's/^/      /'
fi
if [[ -f "$WORK/hello.efi" ]] && \
   file "$WORK/hello.efi" 2>/dev/null | grep -q 'PE32+.*EFI application'; then
    test_host_pass "and the result is a PE32+ EFI application"
else
    test_host_fail "and the result is a PE32+ EFI application"
    file "$WORK/hello.efi" 2>/dev/null | sed 's/^/      /'
fi

# The archive must not carry an absolute path from the build machine into a
# consumer's prefix -- the failure that makes a "relocatable" archive not be.
#
# Search for the STAGING prefix, not $OUT: make-sdk-tarball.sh stages into its
# own mktemp dir, so $OUT (where the .tar.gz lands) is not a path that could
# ever appear inside the archive. Grepping for it would have been a second
# assertion that cannot fail.
STAGE_HINT="axl-sdktar."
# NO PIPELINE HERE, deliberately. The first draft was
#   grep -rIl ... | head -3 | grep -q .
# which has the same pipefail defect as the listing above and inverts THIS
# verdict: on the failing path grep -q matches, exits, SIGPIPEs head, and
# pipefail makes the `if` false -- so a baked-in path would have been reported
# as clean. A detector that cannot report the thing it detects is worse than
# none, so the result is captured instead.
BAKED="$(grep -rIl -- "$STAGE_HINT" "$SDK" 2>/dev/null || true)"
if [[ -n "$BAKED" ]]; then
    test_host_fail "no build-machine staging path is baked into the archive"
    echo "$BAKED" | head -3 | sed 's/^/      /'
else
    test_host_pass "no build-machine staging path is baked into the archive"
fi

echo ""
test_host_summary "sdk-tarball"
