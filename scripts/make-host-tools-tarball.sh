#!/bin/bash
# make-host-tools-tarball.sh — build the host-tools tarball.
#
# WHY THIS EXISTS. This archive was assembled INLINE in .github/workflows/
# release.yml, and make-sdk-tarball.sh's own header named that as the defect:
# "the host-tools tarball IS assembled inline below and is exactly the kind of
# thing that then has no local reproduction." Its name, its layout and whether
# the commands inside it actually run could only be checked by cutting a
# release. Building it here means release.yml and
# test/integration/test-host-tools-tarball.sh run the SAME code.
#
# WHO IT IS FOR. 0.36 MB against the SDK tarball's 13.5 MB -- a CI job that
# only needs run-qemu should not pull 182 headers to get it
# (AXL-Distribution-Design.md §14.2 keeps the asset for exactly this reason).
#
# THE TOP-LEVEL DIRECTORY IS THE VERSIONED ROOT. `axl-sdk-host-tools-<ver>/`.
# The predecessor unpacked SIX entries into the caller's working directory
# (§14.1c), which is why the consumer README had to say
# `mkdir -p ~/axl-sdk-host-tools && tar xf ... -C` -- the caller created the
# directory because the archive would not.
#
# NO ARCH IN THE NAME. The payload is shell and Python; an arch field would
# either be a lie or force two byte-identical uploads (§14.1a).
#
# ONE OWNER FOR THE FILE LIST: the Makefile's HOST_TOOL_FILES, the same list
# scripts/install.sh stages into <prefix>/libexec/axl. These were two
# hand-written lists until adding axl_version.py to one and not the other
# shipped a tarball whose four Python tools all died with ModuleNotFoundError.
#
# Usage: scripts/make-host-tools-tarball.sh [--out DIR]
set -euo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

OUT_DIR="$SDK_DIR/dist"
PRINT_NAME=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)   OUT_DIR="${2:?--out needs a directory}"; shift 2 ;;
        --out=*) OUT_DIR="${1#--out=}"; shift ;;
        # Report the archive's name and build nothing -- see the same flag on
        # make-sdk-tarball.sh, and scripts/check-asset-names.py.
        --print-name) PRINT_NAME=1; shift ;;
        -h|--help)
            sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "ERROR: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

VERSION="$(cat "$SDK_DIR/VERSION")"
NAME="axl-sdk-host-tools-${VERSION}"
TARBALL="${NAME}.tar.gz"

if [[ "$PRINT_NAME" == "1" ]]; then
    echo "$TARBALL"
    exit 0
fi

# The libexec staging rule, shared with scripts/install.sh so the two staging
# paths cannot disagree about which of these files are commands.
# shellcheck source=./stage-host-tools.sh
source "$SDK_DIR/scripts/stage-host-tools.sh"

# Read the list back from the Makefile rather than restating it here.
mapfile -t HOST_TOOLS < <(make -s -C "$SDK_DIR" print-HOST_TOOL_FILES | tr ' ' '\n' | grep -v '^$')
[[ "${#HOST_TOOLS[@]}" -gt 0 ]] || { echo "ERROR: HOST_TOOL_FILES is empty" >&2; exit 1; }

# Stage into a temp dir, never into $OUT_DIR: the staging path must not survive
# into the archive in any form, and the cleanest way to be sure is for it never
# to be a path the consumer could inherit.
STAGE_ROOT="$(mktemp -d -t axl-httar.XXXXXXXX)"
trap 'rm -rf "$STAGE_ROOT"' EXIT
STAGE="$STAGE_ROOT/$NAME"

echo "[make-host-tools-tarball] staging $NAME (${#HOST_TOOLS[@]} tools) ..."

# bin/axl + libexec/axl/, the SAME shape the SDK tarball and the prefix use.
# It was a flat scripts/ directory with no dispatcher, which made this the one
# channel whose users could not run `axl <cmd>` and were therefore forced to
# hardcode <root>/scripts/... -- exactly what the dispatcher exists to stop.
# `axl` resolves its prefix from its own location, so it finds libexec/axl
# wherever the archive is extracted.
mkdir -p "$STAGE/bin" "$STAGE/libexec/axl" "$STAGE/share/axl"
# THE MODE IS THE DECLARATION "this is a command", so it comes from the shared
# rule rather than from a blanket 0755 plus a hand-written exception. That
# exception list held exactly one name (axl_version.py) and missed the other
# two files that are not commands: axl-common.sh, which run-qemu.sh SOURCES,
# and gdb-sample.py, which is loaded INSIDE gdb and has no shebang -- both
# shipped 0755 in v4.7.0 and were offered by `axl --help` as commands that
# cannot run. scripts/install.sh had the right rule for the same file list all
# along; this path is why it had to be shared instead of copied.
for _f in "${HOST_TOOLS[@]}"; do
    install -m "$(axl_host_tool_mode "$SDK_DIR/scripts/$_f")" \
            "$SDK_DIR/scripts/$_f" "$STAGE/libexec/axl/$_f"
done
install -m 0755 "$SDK_DIR/scripts/axl" "$STAGE/bin/axl"

# THE INSTALLER SHIPS INSIDE IT (§20's M2). This component is the MANAGER:
# `axl prune` never removes the CURRENT one or the one it is running out of
# (it does prune superseded generations of this family, which would otherwise
# accumulate forever), and `axl use` never repoints it, so it is the one tree
# that can still self-update after any rollback. That only works if it carries the installer
# its verbs exec.
#
# 0644 DELIBERATELY, the same as the SDK prefix: `axl` lists the EXECUTABLES in
# libexec/axl as commands, so a mode bit here would offer `axl install` as a
# third way to reach the same thing.
install -m 0644 "$SDK_DIR/packaging/install.sh" "$STAGE/libexec/axl/install.sh"

# `axl --print-version` and axl_version.py both read this; install.sh's
# already_installed check reads it too.
echo "$VERSION" > "$STAGE/share/axl/version"
echo "$VERSION" > "$STAGE/VERSION"
install -m 0644 "$SDK_DIR/LICENSE" "$SDK_DIR/NOTICE" "$SDK_DIR/CHANGELOG.md" "$STAGE/"

INSTALL_HINT_DEB="sudo apt install qemu-system-x86 qemu-system-arm ovmf qemu-efi-aarch64 virtiofsd mtools dosfstools"
INSTALL_HINT_DNF="sudo dnf install qemu-system-x86 qemu-system-aarch64 edk2-ovmf edk2-aarch64 virtiofsd mtools dosfstools"
INSTALL_HINT_PAC="sudo pacman -S qemu-system-x86 qemu-system-aarch64 edk2-ovmf edk2-armvirt virtiofsd mtools dosfstools"

cat > "$STAGE/README.md" <<EOF
# AXL SDK Host Tools v${VERSION}

Host-side runtime tooling for testing axl-sdk apps in QEMU.
See https://github.com/aximcode/axl-sdk for the full SDK.

## Quick start

\`\`\`bash
# 1. Install system QEMU + OVMF (one-time):
#    Debian/Ubuntu: ${INSTALL_HINT_DEB}
#    Fedora/RHEL:   ${INSTALL_HINT_DNF}
#    Arch:          ${INSTALL_HINT_PAC}
#
# 2. Run a UEFI app under QEMU:
./bin/axl run-qemu path/to/your-app.efi

# 3. Drop into the UEFI shell with the current dir mounted:
./bin/axl run-qemu -i --mount .

# Put bin/ on PATH and every command is 'axl <verb>':
#   axl --help          list them
#   axl rsod-decode     decode a crash dump
#   axl gdb-syms        recover module load addresses
\`\`\`

QEMU/firmware discovery is automatic via standard system paths.
To override, set \`QEMU_DIR\`, \`OVMF_CODE\`, \`AAVMF_CODE\`, or
\`VIRTIOFSD\`.

## License

Apache-2.0. See LICENSE and NOTICE.
EOF

mkdir -p "$OUT_DIR"
echo "[make-host-tools-tarball] archiving ..."
# Sorted, with a fixed owner and mtime, so two builds of the same tree give
# byte-identical archives -- a consumer pinning by SHA256 can then tell "the
# tools changed" from "the tarball was rebuilt".
tar --sort=name \
    --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git -C "$SDK_DIR" log -1 --format=%ct 2>/dev/null || echo 0)" \
    -czf "$OUT_DIR/$TARBALL" -C "$STAGE_ROOT" "$NAME"

echo "[make-host-tools-tarball] $OUT_DIR/$TARBALL"
ls -lh "$OUT_DIR/$TARBALL" | awk '{print "                   " $5}'
