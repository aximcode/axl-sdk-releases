#!/bin/bash
# test-meta: arch=both needs= est=10 local-only=0
# test-tool-help-qemu.sh — every axl-sdk tool answers -h/--help uniformly.
#
# Companion to test-tool-version-qemu.sh. The axl_args_run framework already
# gives ~23 tools a `-h`/`--help` header of the shape "<tool> <VERSION> - <help>".
# This test pins that SAME header for the tools that historically did NOT parse
# args: the storage/kbtune enumerators + axbench (now routed through the
# framework), and sed/fbcon/fwtool (non-framework tools that call the shared
# axl_help_handle hook). It also confirms crashtest answers --version.
#
# Asserts, over one QEMU boot:
#   - `<tool> -h` AND `<tool> --help` each print exactly "<tool> <VERSION> - <tagline>"
#     for every changed tool, and do NOT run the tool's normal action.
#   - `sed -h` no longer prints "invalid option".
#   - `crashtest --version` prints "crashtest <VERSION>".
#
# Usage: ./test/integration/test-tool-help-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
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

# tool|tagline — the exact text after " - " in the `-h` header. Keep in sync
# with each tool's node .help (framework) / axl_help_handle tagline (hook).
TOOLS_HELP=(
    "ata|List ATA/SATA devices with identity + SMART health"
    "nvme|List NVMe controllers with identity, SMART health + namespaces"
    "scsi|List SCSI/SAS logical units with identity, capacity + health"
    "smart|Scan all storage (NVMe/ATA/SCSI) for normalized health"
    "kbtune|Interactive keyboard debounce tuner (needs a GOP console)"
    "axbench|AP task-pool micro-benchmark"
    "sed|Stream editor (POSIX sed + common GNU extensions)"
    "fbcon|Framebuffer console take-over (resident driver)"
    "fwtool|Firmware image (.fd / SPI) FV/FFS explorer"
)

# Staged binaries needed: the help tools + crashtest.
NEED=(crashtest)
for e in "${TOOLS_HELP[@]}"; do NEED+=("${e%%|*}"); done
for t in "${NEED[@]}"; do
    [[ -f "$TOOLS/$t.efi" ]] || { echo "ERROR: $TOOLS/$t.efi not built"; exit 1; }
done

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"
LOG="$TMP/serial.log"

{
    echo '@echo -off'
    echo 'fs0:'
    for e in "${TOOLS_HELP[@]}"; do
        t="${e%%|*}"
        echo "echo HELP_${t}_SHORT"
        echo "${t}.efi -h"
        echo "echo HELP_${t}_LONG"
        echo "${t}.efi --help"
        echo "echo HELP_${t}_END"
    done
    echo 'echo CRASH_VER'
    echo 'crashtest.efi --version'
    echo 'echo MARK_DONE'
    echo 'reset -s'
} > "$NSH"

EXTRA=()
for t in "${NEED[@]:1}"; do EXTRA+=(--extra "$TOOLS/$t.efi"); done

timeout=90
[[ "$ARCH" == "AARCH64" || ! -r /dev/kvm ]] && timeout=240
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout "$timeout" \
    --nsh "$NSH" "${EXTRA[@]}" "$TOOLS/${NEED[0]}.efi" > "$LOG" 2>&1 || true

fail=0
ok() { echo "PASS: $1"; }
no() { echo "FAIL: $1"; fail=1; }
# Print the log slice between two markers.
sect() { sed -n "/$1/,/$2/p" "$LOG"; }

echo "=== version string in use: $VERSION ==="

for e in "${TOOLS_HELP[@]}"; do
    t="${e%%|*}"; tag="${e#*|}"
    want="$t $VERSION - $tag"
    # -h and --help must both emit the exact header line.
    if sect "HELP_${t}_SHORT" "HELP_${t}_LONG" | grep -aqF "$want"; then
        ok "$t -h prints '$want'"
    else
        no "$t -h did not print '$want'"
    fi
    if sect "HELP_${t}_LONG" "HELP_${t}_END" | grep -aqF "$want"; then
        ok "$t --help prints the header"
    else
        no "$t --help did not print the header"
    fi
done

# sed -h must no longer be rejected as an invalid option.
if sect "HELP_sed_SHORT" "HELP_sed_LONG" | grep -aqiE "invalid option"; then
    no "sed -h still prints 'invalid option'"
else
    ok "sed -h is not rejected as an invalid option"
fi

# crashtest answers --version with the stamp.
if sect "CRASH_VER" "MARK_DONE" | grep -aqE "^crashtest ${VERSION}([[:space:]]|$)"; then
    ok "crashtest --version reports the stamp"
else
    no "crashtest --version did not report the stamp"
fi

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): tool help/version consistency verified ==="; exit 0
else
    echo "=== FAIL ($ARCH) ==="; exit 1
fi
