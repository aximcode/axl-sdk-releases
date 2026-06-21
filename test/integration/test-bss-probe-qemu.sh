#!/bin/bash
# test-meta: arch=both needs= est=8 local-only=0
# test-bss-probe-qemu.sh — validate the build's uninitialized-.bss handling.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# bss-probe.efi carries an 8 MiB zero-initialized static array. With the
# linker scripts emitting a real .bss (NOBITS) section, that array is NOT
# in the file (the .efi is ~110 KB, not ~8.5 MB); the crt0 zeroes it before C
# runs and the loader maps it RW. This boots the probe under QEMU and asserts
# the array is zero-initialized and writable at runtime, plus checks the PE
# section flags + file size. NOTE: this is boot-integrity on ONE firmware — on
# firmware that hands back zeroed pages it passes even if the crt0 clear were
# missing, so it is NOT the regression guard for the clear. `make
# check-bss-clear` is that guard (disassembles _start, firmware-independent).
#
# Usage: ./test/integration/test-bss-probe-qemu.sh [X64|AARCH64|both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

# The 8 MiB array must not be materialized in the file. Allow generous
# headroom (real .efi is ~110-130 KB) but well under the 8 MiB it would be
# if .bss were folded into .data.
MAX_FILE_BYTES=$((1024 * 1024))   # 1 MiB ceiling

overall_fail=0

run_one() {
    local arch="$1" native_arch out efi log sz objdump_bin bss_flags
    case "$arch" in
        X64)     native_arch="x64";  out="$PROJECT_DIR/out/native-x64"
                 objdump_bin="objdump" ;;
        AARCH64) native_arch="aa64"; out="$PROJECT_DIR/out/native-aa64"
                 objdump_bin="aarch64-linux-gnu-objdump" ;;
    esac
    efi="$out/bss-probe.efi"

    echo "=== bss-probe ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" bss-probe 2>&1 | tail -1

    # Mechanism check: .bss must be a real uninitialized PE section — present,
    # ALLOC, and WITHOUT CONTENTS (no file bytes). This pins the actual fix:
    # a regression to folding .bss into .data would make it CONTENTS-backed,
    # and dropping -j .bss from objcopy would make it absent entirely (which
    # would fault on real HW). The flags line follows the section header.
    bss_flags=$("$objdump_bin" -h "$efi" 2>/dev/null | grep -A1 '[[:space:]]\.bss[[:space:]]' | tail -1)
    if grep -q ALLOC <<<"$bss_flags" && ! grep -q CONTENTS <<<"$bss_flags"; then
        echo "OK ($arch): .bss is ALLOC without CONTENTS (uninitialized, no file bytes)"
    else
        echo "FAIL ($arch): .bss missing or file-backed (flags: '${bss_flags:-none}')"
        overall_fail=$((overall_fail + 1))
    fi

    # Size check: the 8 MiB static array must not be materialized in the file.
    sz=$(stat -c%s "$efi")
    if (( sz < MAX_FILE_BYTES )); then
        echo "OK ($arch): .efi is $sz bytes (8 MiB .bss not in the file)"
    else
        echo "FAIL ($arch): .efi is $sz bytes — .bss appears folded into .data"
        overall_fail=$((overall_fail + 1))
    fi

    log="$(mktemp)"
    timeout 90s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --timeout 30 "$efi" 2>&1 | tee "$log" \
        | grep -iE "BSS-PROBE|PASS:|FAIL:" || true

    local verdict
    verdict="$(grep -E "^BSS-PROBE: [0-9]+ passed, [0-9]+ failed" "$log" | tail -1 || true)"
    rm -f "$log"

    if [[ "$verdict" =~ ,\ 0\ failed$ ]] && [[ ! "$verdict" =~ ^BSS-PROBE:\ 0\ passed ]]; then
        echo "OK ($arch): $verdict (crt0-zeroed + loader-mapped .bss)"
    else
        echo "FAIL ($arch): ${verdict:-no verdict line}"
        overall_fail=$((overall_fail + 1))
    fi
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
    echo
done

if (( overall_fail > 0 )); then
    echo "$overall_fail check(s) failed"
    exit 1
fi
echo "All bss-probe checks passed."
exit 0
