#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# test-lsproto-qemu.sh — lsproto names live protocols by their canonical spec
# identifiers (EFI_*_PROTOCOL), not the Shell's short labels.
#
# Boots the tool under the shell and asserts against protocols OVMF reliably
# publishes on both arches: EFI_BLOCK_IO_PROTOCOL and EFI_DEVICE_PATH_PROTOCOL
# (fixed, spec-assigned GUIDs). Pins:
#   - default/present view names a present protocol + its exact GUID
#   - -p <name> resolves a spec NAME to its handles (the name->GUID reverse)
#   - -a dictionary carries a known name that is NOT present on a bare boot
#     (proves the dictionary is the full set, not just what's installed)
#   - -j emits parseable JSON with the spec name
#   - --version stamps the tool
#
# Exact-string assertions (grep -F on whole tokens), per the axl-sdk workflow.
# Ratchet-exempt (end-to-end scenario, not a unit binary's count).
#
# Usage: ./test/integration/test-lsproto-qemu.sh [X64|AARCH64|both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

# Fixed, spec-assigned GUIDs — identical on every arch/firmware.
BLOCK_IO_GUID="964e5b21-6459-11d2-8e39-00a0c969723b"
DEVPATH_GUID="09576e91-6d3f-11d2-8e39-00a0c969723b"

overall_fail=0

run_one() {
    local arch="$1" native_arch out efi log nsh pass fail
    case "$arch" in
        X64)     native_arch="x64";  out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)" ;;
        AARCH64) native_arch="aa64"; out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs aa64)" ;;
    esac
    efi="$out/tools/lsproto.efi"

    echo "=== lsproto ($arch) ==="
    # Relative target so it matches the Makefile's $(PREFIX)/tools/... rule
    # (an absolute path is a different target and only "works" when the file
    # already happens to exist).
    make -C "$PROJECT_DIR" ARCH="$native_arch" \
        ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} "$("$PROJECT_DIR/scripts/build-prefix.sh" "$native_arch")/tools/lsproto.efi" 2>&1 | tail -1

    nsh="$(mktemp)"
    cat > "$nsh" <<'NSH'
@echo -off
echo ==A-BLOCK==
lsproto block
echo ==B-PVIA==
lsproto -p EFI_BLOCK_IO_PROTOCOL
echo ==C-DICT==
lsproto -a EFI_TCP4_PROTOCOL
echo ==D-JSON==
lsproto -j -p EFI_DEVICE_PATH_PROTOCOL
echo ==E-VER==
lsproto --version
echo ==END==
reset -s
NSH

    log="$(mktemp)"
    timeout 150s "$RUN_QEMU" --arch "$arch" --timeout 90 --nsh "$nsh" \
        "$efi" > "$log" 2>&1 || true
    rm -f "$nsh"

    pass=0
    fail=0
    ck() {  # ck "<description>" <grep-args...>
        local desc="$1"; shift
        if grep "$@" "$log" >/dev/null 2>&1; then
            echo "  PASS: $desc"; pass=$((pass + 1))
        else
            echo "  FAIL: $desc"; fail=$((fail + 1))
        fi
    }

    # Present view: name + exact GUID + a handle-count 3rd column. The count
    # column is what makes this the PRESENT line, not the -p header (which is
    # "name  guid" with no trailing count) — so a present-view regression can't
    # be masked by -p still working.
    ck "present view names EFI_BLOCK_IO_PROTOCOL" -Eq "^EFI_BLOCK_IO_PROTOCOL[[:space:]]+${BLOCK_IO_GUID}[[:space:]]+[0-9]"
    # -p resolves a spec NAME to its GUID + at least one handle.
    ck "-p EFI_BLOCK_IO_PROTOCOL prints its GUID"  -Fq "EFI_BLOCK_IO_PROTOCOL  ${BLOCK_IO_GUID}"
    ck "-p reports a handle"                       -Eq "^[0-9]+ handle"
    # Dictionary carries a name that is NOT present on a bare boot.
    ck "-a dictionary has EFI_TCP4_PROTOCOL"       -Fq "EFI_TCP4_PROTOCOL"
    # JSON output is parseable and spec-named.
    ck "-j emits the spec name in JSON"            -Fq "\"name\":\"EFI_DEVICE_PATH_PROTOCOL\""
    ck "-j emits the device-path GUID in JSON"     -Fq "\"guid\":\"${DEVPATH_GUID}\""
    # Version stamp.
    ck "--version stamps the tool"                 -Eq "^lsproto [0-9]+\."

    echo "  lsproto: $pass passed, $fail failed ($arch)"
    if [[ $fail -gt 0 ]]; then
        echo "--- serial (tail) ---"; tail -50 "$log"
        overall_fail=$((overall_fail + 1))
    fi
    rm -f "$log"
    echo
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
done

if (( overall_fail > 0 )); then
    echo "$overall_fail lsproto arch(es) failed"
    exit 1
fi
echo "All lsproto checks passed."
exit 0
