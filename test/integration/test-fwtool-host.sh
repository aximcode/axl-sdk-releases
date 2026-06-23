#!/bin/bash
# test-meta: arch=both needs= est=8 local-only=0
# test-fwtool-host.sh — golden byte-match: the HOST fwtool (HOSTCC build of
# the SAME backend-free parser + LZMA sources as fwtool.efi) must extract the
# UEFI Shell from real OVMF/AAVMF firmware BYTE-FOR-BYTE identically to the
# reference scripts/extract-fv-shell.py. This is the integration milestone
# that proves the C firmware-image parser decodes real-world GUIDED-LZMA
# firmware exactly, not just the synthetic unit-test fixtures.
#
# No QEMU: a plain host script. It builds out/.../fwtool-host, then for each
# arch's firmware it can locate (X64 OVMF + AARCH64 AAVMF, incl. the
# QEMU-bundled edk2-*-code.fd) runs both extractors and `cmp`s the outputs.
# An arch with no firmware is SKIPPED cleanly; the test FAILS only if no
# firmware is found at all, or if any pair differs.
#
# Auxiliary single-scenario integration test (opt out of the test-axl.sh
# unit ratchet).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

# shellcheck source=/dev/null
source "$PROJECT_DIR/scripts/axl-common.sh"

# The EFI Shell file GUID (gUefiShellFileGuid) — identical across EDK2 builds.
SHELL_GUID="7C04A583-9E3E-4F1C-AD65-E05268D0B4D1"

PY_EXTRACT="$PROJECT_DIR/scripts/extract-fv-shell.py"

# The host fwtool lives under the X64 build dir (the binary is arch-neutral —
# it parses bytes, not the running arch). Build it via the Makefile so the
# exact host build recipe under test is the one CI uses.
FWTOOL_HOST="$PROJECT_DIR/out/native-x64/build/fwtool-host"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

log_info() { printf '%s\n' "$*"; }

# --- Build the host tool ---------------------------------------------------
log_info "==> building host fwtool"
make -C "$PROJECT_DIR" ARCH=x64 fwtool-host >/dev/null
[[ -x "$FWTOOL_HOST" ]] || { echo "FAIL: host fwtool not built at $FWTOOL_HOST"; exit 1; }

# --- Locate firmware -------------------------------------------------------
# Try find_qemu so the QEMU-bundled firmware (edk2-*-code.fd) is on the search
# path, mirroring the brief / find_shell_efi. A missing QEMU is non-fatal —
# find_firmware also probes the distro package locations.
find_qemu X64       >/dev/null 2>&1 || true

declare -a FW_IMAGES=()
declare -a FW_LABELS=()

for arch in X64 AARCH64; do
    FW_CODE=""
    if find_firmware "$arch" 2>/dev/null && [[ -n "${FW_CODE:-}" && -f "$FW_CODE" ]]; then
        FW_IMAGES+=("$FW_CODE")
        FW_LABELS+=("$arch")
        log_info "==> $arch firmware: $FW_CODE"
    else
        log_info "==> $arch firmware: not found — SKIP"
    fi
done

if [[ ${#FW_IMAGES[@]} -eq 0 ]]; then
    echo "FAIL: no OVMF/AAVMF firmware found for any arch — cannot run the golden test"
    echo "      install ovmf / qemu-efi-aarch64, or set OVMF_CODE / AAVMF_CODE"
    exit 1
fi

# --- Golden compare per image ----------------------------------------------
fail=0
for i in "${!FW_IMAGES[@]}"; do
    fw="${FW_IMAGES[$i]}"
    label="${FW_LABELS[$i]}"
    py_out="$WORK/py-$label.efi"
    c_out="$WORK/c-$label.efi"

    if ! python3 "$PY_EXTRACT" "$fw" -o "$py_out" >/dev/null 2>&1; then
        echo "FAIL [$label]: python extract-fv-shell.py failed on $fw"
        fail=1
        continue
    fi
    if ! "$FWTOOL_HOST" extract "$fw" "$SHELL_GUID" -o "$c_out" >/dev/null 2>&1; then
        echo "FAIL [$label]: host fwtool extract failed on $fw"
        fail=1
        continue
    fi

    # Both must be real PE images (MZ prefix) ...
    if [[ "$(head -c2 "$py_out")" != "MZ" ]]; then
        echo "FAIL [$label]: python output is not an MZ-prefixed PE"
        fail=1
        continue
    fi
    if [[ "$(head -c2 "$c_out")" != "MZ" ]]; then
        echo "FAIL [$label]: host fwtool output is not an MZ-prefixed PE"
        fail=1
        continue
    fi

    # ... and BYTE-IDENTICAL (exact — never weaken this to a substring).
    if cmp -s "$py_out" "$c_out"; then
        log_info "PASS [$label]: byte-identical ($(stat -c%s "$c_out") bytes) — $(basename "$fw")"
    else
        echo "FAIL [$label]: outputs differ"
        cmp "$py_out" "$c_out" || true
        fail=1
    fi
done

echo
if [[ $fail -eq 0 ]]; then
    echo "=== test-fwtool-host: PASS (host fwtool == extract-fv-shell.py on ${#FW_IMAGES[@]} image(s)) ==="
    exit 0
else
    echo "=== test-fwtool-host: FAIL ==="
    exit 1
fi
