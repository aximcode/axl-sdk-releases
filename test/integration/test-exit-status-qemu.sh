#!/bin/bash
# test-exit-status-qemu.sh -- end-to-end proof that axl_set_exit_status sets the
# UEFI shell's %lasterror% to a caller-chosen, VERBATIM EFI_STATUS.
#
# exit-status-selftest.efi arms argv[1] (hex) as its exit status and returns a
# NONZERO rc. Without the primitive the runtime collapses that to EFI_ABORTED
# (0x15); with it, %lasterror% must be the exact armed value -- including a
# non-error-class code (0x34) and an armed success (0x0) that overrides the
# nonzero rc. This is the consumer's `do err <N>` parity case.
set -u

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        *) echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

# run-qemu.sh takes the uppercase arch (X64/AARCH64); the Makefile takes the
# lowercase one (x64/aa64) and builds into out/native-<lower>.
case "$ARCH" in
    X64)     OUT="out/native-x64";  MAKE_ARCH="x64" ;;
    AARCH64) OUT="out/native-aa64"; MAKE_ARCH="aa64" ;;
    *) echo "unknown arch $ARCH"; exit 1 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_DIR"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
APP="$OUT/exit-status-selftest.efi"

PASS=0; FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

if [[ ! -x "$APP" ]]; then
    echo "Building exit-status-selftest (ARCH=$MAKE_ARCH)..."
    make exit-status-selftest ARCH="$MAKE_ARCH" || true
fi
[[ -f "$APP" ]] || { echo "FAIL: $APP not found"; exit 1; }

# Run the app with hex arg $1, return the shell's %lasterror% text.
run_case() {
    local arg="$1"
    local nsh out
    nsh="$(mktemp --suffix=.nsh)"
    cat > "$nsh" <<EOF
@echo -off
fs0:
cd \\
exit-status-selftest.efi $arg
echo AXLEXIT_LASTERROR=[%lasterror%]
reset -s
EOF
    out="$(timeout 90s "$RUN_QEMU" --arch "$ARCH" --nsh "$nsh" "$APP" 2>&1)"
    rm -f "$nsh"
    # Extract the bracketed value, lowercased for a stable compare.
    sed -n 's/.*AXLEXIT_LASTERROR=\[\(.*\)\].*/\1/p' <<< "$out" | tr 'A-F' 'a-f' | head -1
}

# arg -> expected %lasterror% (lowercase). Each arg is parsed as hex by the app.
check() {
    local arg="$1" want="$2" got
    got="$(run_case "$arg")"
    if [[ "$got" == "$want" ]]; then
        pass "err $arg -> %lasterror% == $want (verbatim, overrode rc=1)"
    else
        fail "err $arg -> %lasterror%" "expected '$want', got '$got'"
    fi
}

echo "=== axl_set_exit_status -> %lasterror% (arch $ARCH) ==="
# 0x34: a non-error-class code (top bit clear) the old rc->ABORTED map could
# never produce. THE consumer parity case (`do err 34` -> %lasterror% == 0x34).
check 34 "0x34"
# An armed success overrides the nonzero rc entirely.
check 0  "0x0"
# Not special-cased: any byte value passes through verbatim.
check ff "0xff"

echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]]
