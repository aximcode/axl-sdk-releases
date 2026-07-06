#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# test-ramdisk-map-qemu.sh — mkrd assigns a usable shell map name via
# EFI_SHELL_PROTOCOL.SetMap, so a non-interactive .nsh can use the RAM disk in
# ONE call with NO `map -r`. Verifies:
#   1. `mkrd RAMDISK`        -> auto-picks the next free fsN, sets %RAMDISK%;
#                              `%RAMDISK%:` then works immediately (write+read).
#   2. `mkrd SCRATCH -m RD`  -> maps as RD:; `RD:` works immediately.
#   3. `mkrd OTHER -m RD`    -> fails (RD: already in use) — no clobber.
# This is the read-only-boot ePSA scratch-volume workflow (delldiags).
#
# Usage: ./test/integration/test-ramdisk-map-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
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
MKRD="$PROJECT_DIR/out/native-$NATIVE/tools/mkrd.efi"

make -C "$PROJECT_DIR" ARCH="$NATIVE" tools >/dev/null 2>&1 || true
[[ -f "$MKRD" ]] || { echo "ERROR: $MKRD not built"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"
LOG="$TMP/serial.log"
# NB: avoid "-r" / "()" in echo markers — the shell's echo treats a bare
# "-r" token as a flag. Use plain ASCII markers.
cat > "$NSH" <<'EOF'
@echo -off
fs0:
echo MARK_AUTO
mkrd.efi RAMDISK
%RAMDISK%:
echo auto-fsn-ok > last.nsh
type last.nsh
fs0:
echo MARK_NAMED
mkrd.efi SCRATCH -m RD -s 8
RD:
echo named-rd-ok > sf.txt
type sf.txt
fs0:
echo MARK_TAKEN
mkrd.efi OTHER -m RD
echo MARK_TOOLONG
mkrd.efi OTHER2 -m THISNAMEISWAYTOOLONG16
echo MARK_DONE
EOF

timeout=60
[[ "$ARCH" == "AARCH64" || ! -r /dev/kvm ]] && timeout=200
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout "$timeout" \
    --nsh "$NSH" "$MKRD" > "$LOG" 2>&1 || true

echo "=== markers ==="
sed -n '/MARK_AUTO/,/MARK_DONE/p' "$LOG" | grep -aE "RAM disk created|label|size|mapping|var|auto-fsn-ok|named-rd-ok|already in use|MARK_" | sed 's/^/  /'

fail=0
assert() { if grep -aqF -- "$1" "$LOG"; then echo "PASS: $1"; else echo "FAIL: missing '$1'"; fail=1; fi; }

echo ""
assert "%RAMDISK% = fs"                     # auto-picked an fsN, %RAMDISK% set
assert "auto-fsn-ok"                        # %RAMDISK%: usable with no map -r
assert "%SCRATCH% = RD"                     # named mapping published
assert "named-rd-ok"                        # RD: usable with no map -r
# taken-name failure (case 3)
if grep -aq 'shell map name "RD" is already in use' "$LOG"; then
    echo "PASS: -m RD refused when RD: already mapped"
else
    echo "FAIL: taken map name was not refused"; fail=1
fi
# over-long name rejected (case 4)
if grep -aq 'is too long' "$LOG"; then
    echo "PASS: over-long -m name rejected"
else
    echo "FAIL: over-long map name not rejected"; fail=1
fi

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): mkrd SetMap one-call mapping verified ==="; exit 0
else
    echo "=== FAIL ($ARCH) ==="; exit 1
fi
