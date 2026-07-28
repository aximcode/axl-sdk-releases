#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-no-gpu-qemu.sh — run-qemu.sh --no-gpu gives the guest NO graphics
# adapter, so OVMF exposes no GOP and axl_gfx_available() is false.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
#
# Two things are checked:
#   1. Behavior. gfx-avail-probe.efi prints "GFX-AVAILABLE: 0|1".
#        - --no-gpu   -> 0 on every arch (no adapter -> no GOP).
#        - default x64 -> 1 (q35's built-in std-VGA gives OVMF a GOP), proving
#          the flag actually removes the adapter. (AARCH64 `virt` has no default
#          display, so a default run is already 0 there — nothing to contrast.)
#   2. Mutual exclusion. --no-gpu combined with --gpu / --screenshot /
#      --display / --gui errors cleanly and does not boot (they need a
#      framebuffer). Host-side, no QEMU boot.
#
# Usage: ./test/integration/test-no-gpu-qemu.sh [--arch X64|AARCH64|both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
export TEST_SKIP_RATCHET=1   # auxiliary; don't touch test-axl.sh's baseline

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Assert the probe, booted with the given extra run-qemu args, reports the
# expected GFX-AVAILABLE value. Bounded by --timeout + an outer timeout.
probe_reports() {
    local arch="$1" want="$2" label="$3" efi="$4"; shift 4
    local log rc
    log="$(mktemp)"
    timeout 90s "$RUN_QEMU" --arch "$arch" --raw --timeout 30 "$@" "$efi" \
        > "$log" 2>&1 || true
    if grep -qE "GFX-AVAILABLE: $want" "$log"; then
        pass "$label -> GFX-AVAILABLE: $want"
    else
        fail "$label -> expected GFX-AVAILABLE: $want, got: $(grep -oE 'GFX-AVAILABLE: [0-9]' "$log" | head -1 || echo none)"
        echo "    --- last serial lines ---"; tail -5 "$log" | sed 's/^/    /'
    fi
    rm -f "$log"
}

# Assert --no-gpu + <conflicting flag> errors cleanly WITHOUT booting.
expect_guard() {
    local arch="$1" label="$2" efi="$3"; shift 3
    local out rc
    # timeout bounds the RED case (unrecognized flag falls through to a boot).
    out="$(timeout 20s "$RUN_QEMU" --arch "$arch" --no-gpu "$@" "$efi" 2>&1)" && rc=0 || rc=$?
    if [[ $rc -ne 0 ]] && grep -qiE "no-gpu.*cannot be combined|cannot be combined.*no-gpu" <<< "$out"; then
        pass "$label -> clean error (rc=$rc)"
    else
        fail "$label -> expected a clean 'cannot be combined' error (rc=$rc)"
        echo "    --- output ---"; sed 's/^/    /' <<< "$out" | head -4
    fi
}

overall=0
for arch in "${ARCHES[@]}"; do
    case "$arch" in
        X64)     native_arch="x64";  out="$PROJECT_DIR/out/native-x64" ;;
        AARCH64) native_arch="aa64"; out="$PROJECT_DIR/out/native-aa64" ;;
    esac
    efi="$out/gfx-avail-probe.efi"

    echo "=== run-qemu --no-gpu ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" gfx-avail-probe 2>&1 | tail -1

    # 1. Behavior: --no-gpu reports no GOP on every arch.
    probe_reports "$arch" 0 "--no-gpu" "$efi" --no-gpu
    # On x64 a default run has a GOP (q35 std-VGA), so it contrasts with above.
    if [[ "$arch" == "X64" ]]; then
        probe_reports "$arch" 1 "default (no flag)" "$efi"
    fi

    # 2. Mutual exclusion — clean error, no boot.
    expect_guard "$arch" "--no-gpu + --gpu"        "$efi" --gpu
    expect_guard "$arch" "--no-gpu + --screenshot" "$efi" --screenshot /tmp/axl-nogpu-shot.png
    expect_guard "$arch" "--no-gpu + --display"    "$efi" --display gtk
    expect_guard "$arch" "--no-gpu + --gui"        "$efi" --gui
done

echo ""
echo "no-gpu test: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]] || overall=1
exit $overall
