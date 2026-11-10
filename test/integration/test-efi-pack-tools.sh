#!/bin/bash
# test-meta: arch=none needs= est=20 local-only=0 builds=ARCH=x64,tool-sizes,release-drivers
# test-efi-pack-tools.sh -- the shipped set is built WRAPPED under its original
# names, and the raw images stay available beside it (P4g Task 6).
#
#   tools/<n>.efi            what ships: the axl-pack stub when the wrap rule
#                            pays, else a byte copy of the raw image
#   tools/raw/<n>.efi        the linked image, always raw
#   tools/<n>.so             the RAW image's symbols, beside the shipped name
#                            (debug tooling and check-log-linkage look there)
#   release-drivers/<arch>/  the release's drivers/<arch>/ set, same rule
#
# Which image is the stub is read from the PE module name pe-set-debug writes
# into .dbgdir: `axl-pack-stub` for a wrapped image, the file name for a raw
# one. The raw image's name is the control -- the reader must SEE a name before
# a name it reports for the shipped image means anything.
#
# Host-only: no QEMU. x64 DEBUG, the tree the integration suite boots.
#
# Usage: ./test/integration/test-efi-pack-tools.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

LOG="$(mktemp -t axl-pack-tools.XXXXXXXX)"; trap 'rm -f "$LOG"' EXIT

echo "=== shipped tools are wrapped (x64) ==="
echo ""

if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 tool-sizes > "$LOG" 2>&1; then
    echo "FAIL: make tool-sizes failed"; tail -20 "$LOG"; exit 1
fi
SIZES="$(cat "$LOG")"
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 release-drivers > "$LOG" 2>&1; then
    echo "FAIL: make release-drivers failed"; tail -20 "$LOG"; exit 1
fi

PREFIX_DIR="$(test_build_dir x64)"
TOOLS="$PREFIX_DIR/tools"
RDRV="$PREFIX_DIR/release-drivers/x64"

check_eq() {
    local label="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then
        test_host_pass "$label"
    else
        test_host_fail "$label"
        echo "      want: '$want'"
        echo "      got:  '$got'"
    fi
}

# -- sshd: the image the format exists for --------------------------------
check_eq "control: raw sshd's module name is its file name" \
    "$(test_pe_module "$TOOLS/raw/sshd.efi")" "sshd.efi"
check_eq "shipped sshd is the stub (module name axl-pack-stub)" \
    "$(test_pe_module "$TOOLS/sshd.efi")" "axl-pack-stub"

if [[ -f "$TOOLS/sshd.so" && ! -e "$TOOLS/raw/sshd.so" ]]; then
    test_host_pass "sshd.so sits beside the shipped name, not in raw/"
else
    test_host_fail "sshd.so sits beside the shipped name, not in raw/"
    ls -l "$TOOLS/sshd.so" "$TOOLS/raw/sshd.so" 2>&1 | sed 's/^/      /'
fi

# -- tool-sizes: one exact line per shipped image ----------------------------
fsize() { stat -c%s "$1" 2>/dev/null || echo "ERR"; }
check_eq "tool-sizes prints sshd's raw and shipped sizes" \
    "$(grep -E '^sshd raw ' <<< "$SIZES")" \
    "sshd raw $(fsize "$TOOLS/raw/sshd.efi") shipped $(fsize "$TOOLS/sshd.efi")"

read -r -a SHIPPED <<< "$("$PROJECT_DIR/scripts/axl-list.sh" SHIPPED_TOOL_NAMES)"
if (( ${#SHIPPED[@]} < 30 )); then
    test_host_fail "SHIPPED_TOOL_NAMES names the shipped set (got ${#SHIPPED[@]})"
else
    want_lines="" raw_sum=0 ship_sum=0
    for t in "${SHIPPED[@]}"; do
        r=$(fsize "$TOOLS/raw/$t.efi"); s=$(fsize "$TOOLS/$t.efi")
        want_lines+="$t raw $r shipped $s"$'\n'
        [[ "$r" != ERR && "$s" != ERR ]] && raw_sum=$((raw_sum + r)) ship_sum=$((ship_sum + s))
    done
    check_eq "tool-sizes has exactly one line per shipped image (${#SHIPPED[@]})" \
        "$(grep -E '^[A-Za-z0-9-]+ raw [0-9]+ shipped [0-9]+$' <<< "$SIZES" \
            | grep -v '^total ' | sort)" \
        "$(printf '%s' "$want_lines" | sort)"
    check_eq "tool-sizes totals both columns" \
        "$(grep -E '^total raw ' <<< "$SIZES")" \
        "total raw $raw_sum shipped $ship_sum"
fi

# -- release-drivers: the release's drivers/<arch>/ names ---------------------
# The EDK2 drivers save less than the stub costs, so each ships as a byte copy.
for d in NetworkCommon UsbCdcEcm UsbCdcNcm UsbRndis RamDiskDxe; do
    if cmp -s "$PROJECT_DIR/third_party/edk2/$d-x64.efi" "$RDRV/$d.efi"; then
        test_host_pass "release-drivers/x64/$d.efi is the vendored image, kept raw"
    else
        test_host_fail "release-drivers/x64/$d.efi is the vendored image, kept raw"
    fi
done

test_host_summary "efi-pack-tools"
