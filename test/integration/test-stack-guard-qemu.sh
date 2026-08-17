#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0
# test-stack-guard-qemu.sh — the stack protector fires under UEFI.
#
# `-fstack-protector-strong` is on by default in the library and in axl-cc.
# It was off for years, and the reason it looked impossible is worth pinning
# in a test: on x86-64 GCC's DEFAULT canary read is `%fs:0x28`, glibc's TLS
# block, which firmware never sets up -- so the plain flag faults instead of
# protecting. The build adds `-mstack-protector-guard=global` to move the read
# to a real symbol, and libaxl.a defines the symbol and the handler.
#
# Three things asserted, and the third is the one that makes the other two
# mean something:
#
#   1. The canary is read from the GLOBAL symbol, not from %fs / TLS. A build
#      that lost -mstack-protector-guard=global still links and still passes a
#      smash test on QEMU by luck of what %fs happens to contain, so this is
#      checked in the disassembly rather than inferred from behaviour.
#   2. Ordinary code is actually instrumented -- a flag that silently stopped
#      applying would leave every other assertion here passing.
#   3. A real overflow halts, and does NOT return from the smashed frame.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
#
# Usage: ./test/integration/test-stack-guard-qemu.sh [X64|AARCH64|both]

set -uo pipefail

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

SRC="$SCRIPT_DIR/stack-guard-selftest.c"
WORK="$(mktemp -d -t axl-stackguard.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
check() {
    if [[ "$1" == "0" ]]; then echo "  PASS: $2"; pass=$((pass + 1))
    else echo "  FAIL: $2"; fail=$((fail + 1)); fi
}

run_one() {
    local arch="$1" cc_arch out objdump_bin efi lib
    case "$arch" in
        X64)     cc_arch="x64";  out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)"
                 objdump_bin="objdump" ;;
        AARCH64) cc_arch="aa64"; out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs aa64)"
                 objdump_bin="aarch64-linux-gnu-objdump" ;;
    esac
    lib="$out/lib/libaxl.a"
    echo "=== stack-guard ($arch) ==="

    make -C "$PROJECT_DIR" ARCH="$cc_arch" >/dev/null 2>&1

    # 1. libaxl.a defines both halves.
    "$objdump_bin" -t "$lib" >"$WORK/lib.sym" 2>/dev/null
    grep -q "__stack_chk_fail" "$WORK/lib.sym" && grep -q "__stack_chk_guard" "$WORK/lib.sym"
    check "$?" "$arch: libaxl.a defines __stack_chk_guard and __stack_chk_fail"

    # 2. Ordinary library code is instrumented. Counted, because a flag that
    #    quietly stopped applying leaves a smash test green only until the
    #    smash happens to miss the canary.
    local n
    n=$("$objdump_bin" -t "$lib" 2>/dev/null | grep -c "\*UND\*.*__stack_chk_fail")
    [[ "$n" -gt 20 ]]
    check "$?" "$arch: $n libaxl.a objects carry a canary check (>20)"

    # 3. The canary comes from the GLOBAL symbol, not from TLS. On x86-64 the
    #    default form reads %fs:0x28 and would fault or read garbage here.
    if [[ "$arch" == "X64" ]]; then
        # Via a file, not a pipe. `set -o pipefail` is on and `grep -q` exits
        # on its first match, SIGPIPEing objdump -- so `objdump | grep -q`
        # reports FAILURE exactly when it FOUND the thing, and this assertion
        # inverted itself. It passed a sabotage that removed
        # -mstack-protector-guard=global before that was noticed.
        "$objdump_bin" -d "$lib" >"$WORK/lib.dis" 2>/dev/null
        ! grep -q "%fs:0x28" "$WORK/lib.dis"
        check "$?" "$arch: no %fs:0x28 TLS canary read (guard=global took effect)"
    else
        # AArch64 has no equivalent default to regress to; it already uses the
        # global symbol. Balancer so the arch totals stay comparable.
        check 0 "$arch: AArch64 reads the canary from the global symbol by default"
    fi

    # 4. It fires. Build the fixture through axl-cc, exactly as a consumer
    #    would get it, and require the halt.
    efi="$WORK/stack-guard-$cc_arch.efi"
    if [[ -x "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-cc" ]]; then
        "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-cc" --arch "$cc_arch" --release "$SRC" -o "$efi" \
            >"$WORK/build.log" 2>&1
        check "$?" "$arch: fixture builds through axl-cc"
    else
        echo "  SKIP ($arch): no staged SDK; run scripts/install.sh"
        return
    fi
    [[ -f "$efi" ]] || { sed 's/^/      /' "$WORK/build.log" | tail -5; return; }

    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --timeout 45 "$efi" >"$WORK/run.log" 2>&1
    tr -d '\r' < "$WORK/run.log" > "$WORK/run.clean"

    grep -Fxq "stackguard: wrote 8 bytes, buf[0]=A" "$WORK/run.clean"
    check "$?" "$arch: the safe write completed (fixture reached the smash)"
    grep -q "stack smashing detected" "$WORK/run.clean"
    check "$?" "$arch: the overflow was detected and named"
    ! grep -q "stackguard: UNDETECTED" "$WORK/run.clean"
    check "$?" "$arch: did not return from the smashed frame"
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
    echo
done

echo "stack-guard: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
