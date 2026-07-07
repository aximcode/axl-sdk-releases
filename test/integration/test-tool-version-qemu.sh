#!/bin/bash
# test-meta: arch=both needs= est=10 local-only=0
# test-tool-version-qemu.sh — every axl-sdk tool reports the SDK release
# version uniformly. The version is stamped ONCE at the AXL_TOOL_MAIN layer
# (so it covers framework tools AND custom-parser tools) from AXL_VERSION_STRING,
# so a field report always says which build it is.
#
# Asserts, over one QEMU boot:
#   - `<tool> --version` and `<tool> -V` print "<tool> <VERSION>" for a
#     representative set spanning every entry shape (framework leaf/verbs,
#     custom-parser storage tools, sed, the busybox-style ones).
#   - EVERY staged tool (except the dev-only axbench/crashtest) prints the
#     version — a count guard against a tool slipping the stamp.
#   - `<tool> -h` help shows the version (framework tools).
#   - dmidecode's old `-V` (SMBIOS spec version) moved to `--smbios-version`;
#     `-V`/`--version` now report the tool version like every other tool.
#
# Usage: ./test/integration/test-tool-version-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "ERROR: unknown arg '$1'"; exit 2 ;;
    esac
done
case "$ARCH" in
    X64)     NATIVE=x64 ;;
    AARCH64) NATIVE=aa64 ;;
    *) echo "ERROR: --arch X64|AARCH64"; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLS="$PROJECT_DIR/out/native-$NATIVE/tools"
VERSION="$(cat "$PROJECT_DIR/VERSION")"

make -C "$PROJECT_DIR" ARCH="$NATIVE" tools >/dev/null 2>&1 || true
[[ -d "$TOOLS" ]] || { echo "ERROR: $TOOLS not built"; exit 1; }

# Tools that intentionally do NOT carry the version stamp (dev/bench only,
# they don't route through AXL_TOOL_MAIN). Everything else must.
EXCLUDE="axbench crashtest"

# Collect the staged tool basenames (stem, no .efi).
mapfile -t ALL < <(cd "$TOOLS" && ls *.efi 2>/dev/null | sed 's/\.efi$//' | sort)
VERSIONED=()
for t in "${ALL[@]}"; do
    skip=0
    for x in $EXCLUDE; do [[ "$t" == "$x" ]] && skip=1; done
    [[ "$skip" == 0 ]] && VERSIONED+=("$t")
done
[[ "${#VERSIONED[@]}" -ge 20 ]] || { echo "ERROR: only ${#VERSIONED[@]} tools found"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"
LOG="$TMP/serial.log"

{
    echo '@echo -off'
    echo 'fs0:'
    # Every tool's --version, each fenced by a marker so we can attribute it.
    for t in "${VERSIONED[@]}"; do
        echo "echo VER_${t}"
        echo "${t}.efi --version"
    done
    # Short-flag + help-header + dmidecode migration spot-checks.
    echo 'echo MARK_SHORTV'
    echo 'mkrd.efi -V'
    echo 'echo MARK_HELP'
    echo 'mkrd.efi -h'
    echo 'echo MARK_DMI_VER'
    echo 'dmidecode.efi --version'
    echo 'echo MARK_DMI_SMBIOS'
    echo 'dmidecode.efi --smbios-version'
    echo 'echo MARK_DONE'
    echo 'reset -s'
} > "$NSH"

# Stage the first tool as the app + every other tool via --extra.
EXTRA=()
for t in "${VERSIONED[@]:1}"; do EXTRA+=(--extra "$TOOLS/$t.efi"); done

timeout=90
[[ "$ARCH" == "AARCH64" || ! -r /dev/kvm ]] && timeout=240
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout "$timeout" \
    --nsh "$NSH" "${EXTRA[@]}" "$TOOLS/${VERSIONED[0]}.efi" > "$LOG" 2>&1 || true

fail=0
sect() { sed -n "/$1/,/$2/p" "$LOG"; }
ok()   { echo "PASS: $1"; }
no()   { echo "FAIL: $1"; fail=1; }

echo "=== version string in use: $VERSION ==="

# --- 1. EVERY versioned tool prints "<tool> <VERSION>" ---
missing=()
for t in "${VERSIONED[@]}"; do
    # The tool's --version line must read exactly "<stem> <VERSION>".
    if ! grep -aqE "^${t} ${VERSION}([[:space:]]|\$|\r)" "$LOG"; then
        missing+=("$t")
    fi
done
if [[ "${#missing[@]}" -eq 0 ]]; then
    ok "all ${#VERSIONED[@]} tools print '<tool> $VERSION' on --version"
else
    no "tools missing the version stamp: ${missing[*]}"
fi

# --- 2. Short flag -V works (mkrd) ---
sect MARK_SHORTV MARK_HELP | grep -aqE "^mkrd ${VERSION}([[:space:]]|\$|\r)" \
    && ok "-V prints the version (mkrd $VERSION)" || no "-V did not print the version"

# --- 3. Help output carries the version (framework tools) ---
sect MARK_HELP MARK_DMI_VER | grep -aqF "$VERSION" \
    && ok "-h help shows the version" || no "-h help missing the version"

# --- 4. dmidecode: --version = tool version; --smbios-version = the old -V behavior ---
sect MARK_DMI_VER MARK_DMI_SMBIOS | grep -aqE "^dmidecode ${VERSION}" \
    && ok "dmidecode --version reports the tool version" \
    || no "dmidecode --version did not report the tool version"
# The migrated SMBIOS-version flag still works (prints an SMBIOS spec version,
# e.g. "SMBIOS x.y" — QEMU/OVMF exposes a Type-0 table); at minimum it must NOT
# print the tool version and must be accepted (no "unknown flag").
if sect MARK_DMI_SMBIOS MARK_DONE | grep -aqiE "unknown (flag|option)|dmidecode ${VERSION}"; then
    no "dmidecode --smbios-version not migrated (unknown flag or prints tool version)"
else
    ok "dmidecode --smbios-version accepted (SMBIOS-version behavior migrated off -V)"
fi

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): tool version stamp verified ==="; exit 0
else
    echo "=== FAIL ($ARCH) ==="; exit 1
fi
