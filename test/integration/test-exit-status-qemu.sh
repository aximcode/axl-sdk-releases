#!/bin/bash
# test-meta: arch=x64 needs= est=61 local-only=0
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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# run-qemu.sh takes the uppercase arch (X64/AARCH64); the Makefile takes the
# lowercase one (x64/aa64). Ask for the prefix rather than composing it -- and
# note this must come AFTER PROJECT_DIR, which it did not when the path was a
# literal and ordering did not matter.
case "$ARCH" in
    X64)     OUT="$("$PROJECT_DIR/scripts/build-prefix.sh" x64)";  MAKE_ARCH="x64" ;;
    AARCH64) OUT="$("$PROJECT_DIR/scripts/build-prefix.sh" aa64)"; MAKE_ARCH="aa64" ;;
    *) echo "unknown arch $ARCH"; exit 1 ;;
esac
cd "$PROJECT_DIR"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
APP="$OUT/exit-status-selftest.efi"

PASS=0; FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

# Two builds of the SAME selftest source: the standard (full) runtime and the
# --minimal-runtime variant. The minimal entry point returns from main rather
# than calling axl_exit, so its return path must honor the armed status too;
# that path was missed in v1.7.0 and regressed silently because the full-runtime
# selftest passed.
MIN="$OUT/exit-status-selftest-minimal.efi"
if [[ ! -x "$APP" || ! -x "$MIN" ]]; then
    echo "Building exit-status selftests (ARCH=$MAKE_ARCH)..."
    make exit-status-selftest exit-status-selftest-minimal ARCH="$MAKE_ARCH" || true
fi
[[ -f "$APP" ]] || { echo "FAIL: $APP not found"; exit 1; }
[[ -f "$MIN" ]] || { echo "FAIL: $MIN not found"; exit 1; }

# Run <efi_name> (staged from <app_path>) with hex arg, echo the shell's
# %lasterror%, return it lowercased for a stable compare.
run_case() {
    local efi_name="$1" app_path="$2" arg="$3"
    local nsh out
    nsh="$(mktemp --suffix=.nsh)"
    cat > "$nsh" <<EOF
@echo -off
fs0:
cd \\
$efi_name $arg
echo AXLEXIT_LASTERROR=[%lasterror%]
reset -s
EOF
    out="$(timeout 90s "$RUN_QEMU" --arch "$ARCH" --nsh "$nsh" "$app_path" 2>&1)"
    rm -f "$nsh"
    sed -n 's/.*AXLEXIT_LASTERROR=\[\(.*\)\].*/\1/p' <<< "$out" | tr 'A-F' 'a-f' | head -1
}

# variant_label, efi_name, app_path, arg -> expected %lasterror% (lowercase).
check() {
    local label="$1" efi="$2" path="$3" arg="$4" want="$5" got
    got="$(run_case "$efi" "$path" "$arg")"
    if [[ "$got" == "$want" ]]; then
        pass "$label: err $arg -> %lasterror% == $want (verbatim, overrode rc=1)"
    else
        fail "$label: err $arg -> %lasterror%" "expected '$want', got '$got'"
    fi
}

# Run the same three cases against BOTH runtimes. 0x34 is the consumer parity
# case (a non-error-class code the rc->ABORTED collapse could never produce);
# 0x0 proves an armed success overrides the nonzero rc; 0xff that any byte
# passes through verbatim.
for v in "full|exit-status-selftest.efi|$APP" \
         "minimal|exit-status-selftest-minimal.efi|$MIN"; do
    IFS='|' read -r label efi path <<< "$v"
    echo "=== $label runtime: axl_set_exit_status -> %lasterror% (arch $ARCH) ==="
    check "$label" "$efi" "$path" 34 "0x34"
    check "$label" "$efi" "$path" 0  "0x0"
    check "$label" "$efi" "$path" ff "0xff"
done

echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]]
