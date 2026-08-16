#!/bin/bash
# test-meta: arch=both needs= est=20 local-only=0
# test-boot-path-qemu.sh — self-relative file access with NO shell.
#
# BdsDxe launching \EFI\BOOT\BOOTx64.EFI directly is the one context where
# AXL sees no shell at all — neither EFI_SHELL_PROTOCOL nor the EFI 1.x
# SHELL_ENVIRONMENT (measured: both absent, both arches). Before the fix,
# axl_app_image_path() sourced its "fsN:" prefix only from
# EFI_SHELL_PROTOCOL->GetMapFromDevicePath, so a boot-option app got a
# volume-less "\EFI\BOOT\BOOTX64.EFI" and axl_app_boot_path() returned
# AXL_ERR — no sidecar, no config, no self-upgrade. And with
# axl_backend_file_open carrying only two branches (modern shell, old shell)
# there was no code path that could open an on-disk file at all.
#
#   RED   BOOTPATH: image_path=\EFI\BOOT\BOOTX64.EFI     (0 passed, 7 failed)
#   GREEN BOOTPATH: image_path=fs0:\EFI\BOOT\BOOTX64.EFI (7 passed, 0 failed)
#
# Two runs per arch, same binary:
#   boot   staged as the boot slot -> no shell -> the fix under test
#   shell  staged normally, launched by startup.nsh -> regression guard that
#          the shell's own "FS0:" mapping still wins verbatim
#
# The fixture asserts EXACT strings and does a real write/read/delete
# round-trip plus reading its own image bytes back — a volume-prefixed
# string that cannot be opened would pass a string-only check.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-boot-path-qemu.sh [X64|AARCH64|both]

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

EXPECTED_CHECKS=7

overall_fail=0

# Run the fixture once and assert its self-reported verdict.
#   $1 arch, $2 mode ("boot" | "shell"), $3.. extra run-qemu.sh args
run_case() {
    local arch="$1" mode="$2"; shift 2
    local out efi log verdict kindline pathline

    case "$arch" in
        X64)     out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)" ;;
        AARCH64) out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs aa64)" ;;
    esac
    efi="$out/boot-path-selftest.efi"

    echo "--- $mode case ($arch) ---"

    log="$(mktemp)"
    # --raw: in --boot-target mode nothing writes the app-start marker that
    # the default filter keys on, so the filtered view is empty (and the
    # script exits non-zero). Take the whole serial log and grep it here.
    timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --raw --timeout 90 "$@" "$efi" > "$log" 2>&1 || true

    grep -E "^(PASS|FAIL|BOOTPATH):" "$log" || true

    kindline="$(grep -E '^BOOTPATH: shell_kind=' "$log" | tail -1 || true)"
    pathline="$(grep -E '^BOOTPATH: image_path=' "$log" | tail -1 || true)"
    verdict="$(grep -E '^BOOTPATH: [0-9]+ passed, [0-9]+ failed' "$log" | tail -1 || true)"

    # Guard the PRECONDITION, not just the verdict: if the boot case ever
    # lands under a shell (staging regression, firmware picking a different
    # boot option), the fixture would assert the shell expectations and pass
    # while testing nothing of the no-shell path.
    local want_kind
    case "$mode" in
        boot)  want_kind="BOOTPATH: shell_kind=0 (none - boot-option case)" ;;
        shell) want_kind="BOOTPATH: shell_kind=1 (shell present - regression case)" ;;
    esac
    if [[ "$kindline" != "$want_kind" ]]; then
        echo "FAIL ($arch/$mode): wrong shell context"
        echo "  want: $want_kind"
        echo "  got:  ${kindline:-<no shell_kind line>}"
        echo "--- serial (tail) ---"; tail -40 "$log"
        rm -f "$log"
        overall_fail=$((overall_fail + 1))
        return
    fi

    rm -f "$log"

    if [[ "$verdict" == "BOOTPATH: $EXPECTED_CHECKS passed, 0 failed" ]]; then
        echo "OK ($arch/$mode): $verdict"
        echo "  $pathline"
    else
        echo "FAIL ($arch/$mode): ${verdict:-no verdict line}"
        echo "  ${pathline:-<no image_path line>}"
        overall_fail=$((overall_fail + 1))
    fi
}

for arch in "${ARCHES[@]}"; do
    case "$arch" in
        X64)     native_arch="x64" ;;
        AARCH64) native_arch="aa64" ;;
    esac

    echo "=== boot-path selftest ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" \
        ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} boot-path-selftest 2>&1 | tail -1

    # No shell anywhere: the app IS the boot loader. run-qemu.sh --boot-target
    # stages it as \EFI\BOOT\BOOT<arch>.EFI, so BdsDxe launches it directly
    # and the staged startup.nsh never runs (nothing reads it without a shell).
    run_case "$arch" boot --boot-target

    # Regression guard: same binary, launched by the shell from the volume
    # root, must keep reporting the shell's own mapping.
    run_case "$arch" shell

    echo
done

if (( overall_fail > 0 )); then
    echo "$overall_fail boot-path check(s) failed"
    exit 1
fi
echo "All boot-path checks passed."
exit 0
