#!/bin/bash
# build-packages.sh — build .deb and .rpm packages locally.
#
# Mirrors what .github/workflows/release.yml does. Intended for
# smoke-testing the packaging pipeline before cutting a release tag,
# or for producing packages on machines that can't reach GitHub
# Actions.
#
# The library is built with TLS (mbedtls) compiled in — matches CI.
# Users who want a smaller libaxl.a can run scripts/install.sh
# directly with AXL_TLS=0.
#
# Usage:
#   ./scripts/build-packages.sh                 # default
#   ./scripts/build-packages.sh --version 0.1.2 # override version
#   ./scripts/build-packages.sh --outdir /tmp   # override output dir
#
# Requires:
#   fpm            (gem install --no-document fpm)
#   rpmbuild       (apt install rpm  /  dnf install rpm-build)
#   gcc, binutils
#   gcc-aarch64-linux-gnu, binutils-aarch64-linux-gnu (cross-compile)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

source "$SCRIPT_DIR/axl-common.sh"

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------

PKG_NAME="axl-sdk"
PKG_VERSION=$(cat "$PROJECT_ROOT/VERSION")
OUTDIR="$PROJECT_ROOT/out/packages"
TLS_ENV="1"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            PKG_VERSION="$2"; shift 2 ;;
        --outdir)
            OUTDIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '3,20p' "$0"; exit 0 ;;
        *)
            log_error "unknown option: $1"
            exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Prereq checks
# ---------------------------------------------------------------------------

for tool in fpm rpmbuild gcc; do
    if ! command -v "$tool" &>/dev/null; then
        log_error "'$tool' not found in PATH"
        case "$tool" in
            fpm)      echo "  install with: gem install --no-document fpm" >&2 ;;
            rpmbuild) echo "  install with: apt install rpm  /  dnf install rpm-build" >&2 ;;
        esac
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------

STAGE=$(mktemp -d -t axl-pkg-stage-XXXXXX)
VERIFY=$(mktemp -d -t axl-pkg-verify-XXXXXX)
trap 'rm -rf "$STAGE" "$VERIFY"' EXIT

log_info "Building $PKG_NAME $PKG_VERSION (with TLS / mbedtls)"
log_info "Staging SDK tree under $STAGE/usr ..."

AXL_TLS="$TLS_ENV" "$SCRIPT_DIR/install.sh" \
    --arch all --prefix "$STAGE/usr" > /dev/null

mkdir -p "$STAGE/usr/share/doc/axl-sdk"
cp -r "$PROJECT_ROOT/sdk/examples" "$STAGE/usr/share/doc/axl-sdk/"

# Strip build artifacts that may have leaked in from a dirty tree —
# match the same filter the CI workflow uses.
find "$STAGE/usr/share/doc/axl-sdk/examples" -type f \
    ! -name '*.c' ! -name '*.h' \
    ! -name '*.md' ! -name 'CMakeLists.txt' \
    -delete

cp "$PROJECT_ROOT/CHANGELOG.md" "$PROJECT_ROOT/README.md" \
   "$PROJECT_ROOT/LICENSE" "$PROJECT_ROOT/THIRD_PARTY.md" \
   "$STAGE/usr/share/doc/axl-sdk/"
# mbedtls is statically linked into libaxl.a; Apache-2.0 §4(a)
# requires the LICENSE be carried with any binary redistribution.
mkdir -p "$STAGE/usr/share/doc/axl-sdk/third_party/mbedtls"
cp "$PROJECT_ROOT/deps/mbedtls/LICENSE" \
   "$STAGE/usr/share/doc/axl-sdk/third_party/mbedtls/"

# ---------------------------------------------------------------------------
# Build .deb and .rpm
# ---------------------------------------------------------------------------

mkdir -p "$OUTDIR"

# Fedora convention is to name developer packages "<foo>-devel". We
# ship a single "axl-sdk" package (no runtime counterpart to split
# out of), but advertise Provides: axl-sdk-devel so that
# `dnf install axl-sdk-devel` resolves correctly and matches user
# muscle memory (same trick Debian folks would use with Provides
# in debian/control).
PROVIDES_DEVEL="${PKG_NAME}-devel"

COMMON_FPM_FLAGS=(
    --name "$PKG_NAME"
    --version "$PKG_VERSION"
    --iteration 1
    --description "AximCode Library SDK for UEFI application development (includes TLS / mbedtls)"
    --url "https://axl.aximcode.com"
    --license "see /usr/share/doc/axl-sdk/LICENSE"
    --maintainer "AximCode <noreply@aximcode.github.io>"
    --provides "$PROVIDES_DEVEL"
    --depends gcc
    --depends binutils
    --depends gcc-aarch64-linux-gnu
    --depends binutils-aarch64-linux-gnu
    --after-install "$PROJECT_ROOT/packaging/postinst.sh"
    -C "$STAGE"
)

log_info "Building .deb ..."
(cd "$OUTDIR" && fpm -s dir -t deb --architecture amd64  "${COMMON_FPM_FLAGS[@]}" . > /dev/null)

log_info "Building .rpm ..."
(cd "$OUTDIR" && fpm -s dir -t rpm --architecture x86_64 "${COMMON_FPM_FLAGS[@]}" . > /dev/null)

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------

# fpm rewrites `-` to `_` in RPM version strings (RPM spec disallows
# dashes in versions), so prerelease versions like 0.1.3-rc1 land as
# `0.1.3_rc1` in the rpm filename. The deb keeps the original string.
PKG_VERSION_RPM="${PKG_VERSION//-/_}"
DEB="$OUTDIR/${PKG_NAME}_${PKG_VERSION}-1_amd64.deb"
RPM="$OUTDIR/${PKG_NAME}-${PKG_VERSION_RPM}-1.x86_64.rpm"

if [[ ! -f "$DEB" || ! -f "$RPM" ]]; then
    log_error "package build failed — expected $DEB and $RPM"
    exit 1
fi

log_info "Verifying .rpm contents + axl-cc sanity ..."

(cd "$VERIFY" && rpm2cpio "$RPM" | cpio -idm --quiet)

VER_CHECK=$("$VERIFY/usr/bin/axl-cc" --version)
log_info "axl-cc reports: $VER_CHECK"

HELLO="$PROJECT_ROOT/sdk/examples/hello.c"
if [[ -f "$HELLO" ]]; then
    "$VERIFY/usr/bin/axl-cc" "$HELLO" -o "$VERIFY/hello.efi" > /dev/null
    if file "$VERIFY/hello.efi" | grep -q "PE32+"; then
        log_info "hello.c -> PE32+ EFI binary: OK"
    else
        log_error "hello.c did not produce a valid PE32+ binary"
        exit 1
    fi
fi

# Confirm the Provides: axl-sdk-devel metadata actually landed.
if rpm -qp --provides "$RPM" | grep -q "${PROVIDES_DEVEL}"; then
    log_info "RPM provides: ${PROVIDES_DEVEL}: OK"
else
    log_warning "RPM does not advertise Provides: ${PROVIDES_DEVEL}"
fi

# ---------------------------------------------------------------------------
# Tools tarballs — pre-built .efi utilities for USB-stick boot use.
# Mirrors the build-tools job in release.yml. Built with TLS so
# fetch handles HTTPS and rfbrowse (Redfish) works.
# ---------------------------------------------------------------------------

TOOLS_X64="$OUTDIR/axl-sdk-tools-x64.tar.gz"
TOOLS_AA64="$OUTDIR/axl-sdk-tools-aa64.tar.gz"

for arch in x64 aa64; do
    log_info "Building tools ($arch) ..."
    # `make tools` alone doesn't bring in the CRT0 / reloc /
    # debug-info objects the tool .efi links against; build `all`
    # first so those exist.
    make -C "$PROJECT_ROOT" ARCH="$arch" BUILD=RELEASE AXL_TLS=1 all tools > /dev/null

    TOOLS_STAGE=$(mktemp -d -t axl-tools-XXXXXX)
    cp "$PROJECT_ROOT/out/native-$arch/tools/"*.efi "$TOOLS_STAGE/"
    echo "$PKG_VERSION" > "$TOOLS_STAGE/VERSION"
    # mbedtls is statically linked into tools that use networking;
    # carry its LICENSE for Apache-2.0 compliance.
    mkdir -p "$TOOLS_STAGE/third_party/mbedtls"
    cp "$PROJECT_ROOT/deps/mbedtls/LICENSE" \
       "$TOOLS_STAGE/third_party/mbedtls/"
    cat > "$TOOLS_STAGE/README.txt" <<EOF
AXL SDK pre-built UEFI tools for $arch
version: $PKG_VERSION

Tools included:
  mkrd.efi      RAM disk management
  hexdump.efi   Hex/ASCII file viewer
  fetch.efi     HTTP client (GET/POST/PUT)
  find.efi      Recursive file finder
  grep.efi      Pattern search
  sysinfo.efi   System inventory (firmware, SMBIOS, memory)
  netinfo.efi   Network diagnostics and ping
  ipmi.efi      IPMI (BMC operations)
  rfbrowse.efi  Redfish browser

Usage:
  1. Format a USB stick as FAT32.
  2. Copy all .efi files to the stick.
  3. Boot to the UEFI Shell and cd to the stick.
  4. Run each tool with --help for options.

TLS / HTTPS:
  Built with mbedtls; fetch handles both http:// and https://
  URLs, and rfbrowse (Redfish) is fully functional. Tools that
  don't reference networking (mkrd, hexdump, find, grep,
  sysinfo) link without pulling mbedtls in.

Third-party licenses:
  mbedtls is Copyright (c) The Mbed TLS Contributors,
  dual-licensed Apache-2.0 OR GPL-2.0-or-later; this
  distribution elects Apache-2.0. Full license text at
  third_party/mbedtls/LICENSE.

Docs: https://axl.aximcode.com/
EOF
    tar -C "$TOOLS_STAGE" -czf "$OUTDIR/axl-sdk-tools-$arch.tar.gz" .
    rm -rf "$TOOLS_STAGE"
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

log_info "Artifacts written to $OUTDIR:"
ls -lh "$DEB" "$RPM" "$TOOLS_X64" "$TOOLS_AA64" 2>/dev/null \
    | awk '{ printf "  %-10s %s\n", $5, $NF }'

echo ""
echo "To install locally:"
echo "  sudo apt install $DEB         # Debian/Ubuntu"
echo "  sudo dnf install $RPM         # Fedora/RHEL"
echo ""
echo "To stage the tool .efi binaries on a FAT USB stick:"
echo "  tar xf $TOOLS_X64  -C /media/\$USER/USB/"
echo "  tar xf $TOOLS_AA64 -C /media/\$USER/USB/   # aa64 target"
