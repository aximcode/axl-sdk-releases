#!/bin/bash
# test-meta: arch=both needs= est=70
# test-libc-alloc-qemu.sh — newlib's internals allocate through AXL, not dlmalloc.
#
# P1 of AXL-Libc-Substrate-Design.md §4d. AXL exports the PLAIN allocator names,
# but 49 of newlib's 625 objects allocate through the REENTRANT
# `_malloc_r`/`_free_r`/`_calloc_r`/`_realloc_r` family, which reaches newlib's
# own dlmalloc. Two allocators in one image is safe today ONLY because dlmalloc
# can never obtain memory (`_sbrk` returns -1), so those 49 objects get NULL --
# and stdio is among them (`findfp.o`, `fvwrite.o`).
#
# Two assertions, and neither is sufficient alone:
#
#   1. BEHAVIOURAL. `strdup()` -- the shortest path to `_malloc_r` a consumer
#      can write -- returns memory, copies bytes, and survives being handed to
#      AXL's `free()`. That crossing is the corruption case §4.2 describes: one
#      allocator's pointer reaching the other's bookkeeping.
#   2. STRUCTURAL. dlmalloc is ABSENT from the image. A bridge that worked by
#      accident (say, by giving newlib a heap) would pass assertion 1 while
#      shipping two allocators. `__malloc_av_` is dlmalloc's arena array and is
#      defined only by `libc_a-mallocr.o`, so its presence means that member was
#      pulled -- which is the thing P1 exists to prevent.
#
# Read the symbols off the ELF `.so` axl-cc keeps beside the `.efi`: objcopy
# does not carry `.symtab` into the PE (see scripts/check-no-avx.py), so `nm` on
# the `.efi` would report nothing and the structural assertion would pass while
# blind.
#
# Built with `-fexceptions` because that is the only link mode carrying `libc.a`
# today (§1). Nothing here is about exceptions.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-libc-alloc-qemu.sh [X64|AARCH64|both]

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

AXL_CXX="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-c++"
# The heap-cap case below is plain C -- the knob is the C allocator's, and
# a C consumer is the one most likely to want it.
AXL_CC="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-cc"
SRC="$SCRIPT_DIR/libc-alloc-selftest.cpp"
WORK="$(mktemp -d -t axl-libc-alloc.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

EXPECTED=(
    "  PASS: strdup() returns memory (reaches _malloc_r)"
    "  PASS: strdup() copied the bytes"
    "  PASS: the heap still allocates after a free()"
    "  PASS: strdup() of a 55-byte string returns memory"
    "  PASS: the long copy is intact"
    "  PASS: malloc(16) from newlib's allocator"
    "  PASS: realloc(16 -> 4096) succeeds"
    "  PASS: realloc preserved the contents across the grow"
    "  PASS: axl_malloc still allocates independently"
    "=== 21 passed, 0 failed ==="
)

# dlmalloc's own symbols. Defined ONLY by newlib's allocator members, so any of
# them appearing means a member we meant to displace was pulled in.
DLMALLOC_SYMS='__malloc_av_|__malloc_trim_threshold|__malloc_sbrk_base|__malloc_top_pad'

pass=0
fail=0
skipped=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "  PASS: $2"; pass=$((pass + 1))
    else echo "  FAIL: $2"; fail=$((fail + 1)); fi
}

# Defined symbols in the ELF .so, distinguishing "absent" from "unreadable" --
# the same trap test-cxx-exceptions-qemu.sh documents for eh_section_count.
nm_count() {  # nm_count <so> <nm> <extended-regex> -> count | ERR
    local out
    out="$("$2" --defined-only "$1" 2>/dev/null)" || { echo ERR; return 0; }
    [[ -n "$out" ]]                               || { echo ERR; return 0; }
    grep -cE "$3" <<<"$out"
}

run_one() {
    local arch="$1" cc_arch nm rc n line
    # shellcheck source=/dev/null
    . "$PROJECT_DIR/scripts/axl-toolchains.conf"
    case "$arch" in
        X64)     cc_arch="x64"
                 nm="${AXL_X64_BINUTILS_PREFIX:-$AXL_X64_BINUTILS_PREFIX_DEFAULT}nm" ;;
        AARCH64) cc_arch="aa64"
                 nm="${AXL_AA64_BINUTILS_PREFIX:-$AXL_AA64_BINUTILS_PREFIX_DEFAULT}nm" ;;
    esac

    echo "=== libc-alloc ($arch) ==="
    if [[ ! -x "$AXL_CXX" || ! -f "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/lib/axl/$cc_arch/axl-cxxrt-alloc.o" ]]; then
        echo "  SKIP ($arch): no staged C++ SDK with the allocator bridge"
        echo "        run: scripts/install.sh --arch $cc_arch --cpp"
        skipped=$((skipped + 1))
        return
    fi

    local efi="$WORK/alloc-$cc_arch.efi"
    "$AXL_CXX" --arch "$cc_arch" --release -fexceptions "$SRC" -o "$efi" \
        >"$WORK/build-$cc_arch.log" 2>&1
    rc=$?
    check "$rc" "$arch: the fixture builds (rc=$rc)"
    if [[ "$rc" -ne 0 ]]; then
        grep -E "undefined reference|error" "$WORK/build-$cc_arch.log" \
            | head -5 | sed 's/^/      /'
        return
    fi

    # -----------------------------------------------------------------
    # 1. STRUCTURAL: dlmalloc is not in the image.
    # -----------------------------------------------------------------
    local so="${efi%.efi}.so"
    n="$(nm_count "$so" "$nm" '_malloc_r$')"
    [[ "$n" != "ERR" && "$n" -ge 1 ]]
    check "$?" "$arch: control — the .so is readable and defines _malloc_r (found $n)"

    # INVERTED by §2-DECISION. dlmalloc used to be the thing to keep out; it is
    # now the allocator newlib is supposed to be using, so its absence would
    # mean the split silently did not happen.
    n="$(nm_count "$so" "$nm" "$DLMALLOC_SYMS")"
    [[ "$n" != "ERR" && "$n" -ge 3 ]]
    check "$?" "$arch: newlib's dlmalloc IS linked (found $n of its state symbols)"

    # And AXL must NOT be defining the C allocator names any more -- that
    # half-split is the corruption case §2a documents.
    n="$(nm_count "$so" "$nm" ' (_malloc_r|_free_r)$')"
    [[ "$n" != "ERR" ]]
    check "$?" "$arch: control — symbol read succeeded"

    # -----------------------------------------------------------------
    # 2. BEHAVIOURAL: it runs, and the crossing is safe.
    # -----------------------------------------------------------------
    local log="$WORK/run-$cc_arch.log"
    timeout 180 "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$arch" --timeout 90 \
        "$efi" >"$log" 2>&1
    tr -d '\r' < "$log" > "$log.clean"
    for line in "${EXPECTED[@]}"; do
        grep -Fxq "$line" "$log.clean"
        check "$?" "$arch: output line — ${line# *}"
    done

    # -----------------------------------------------------------------
    # 3. THE HEAP CAP, driven through the shell environment.
    # -----------------------------------------------------------------
    # AXL_LIBC_HEAP_MAX (MiB) bounds the total the C heap may own. Needs a
    # startup.nsh because it is a SHELL variable -- the bare-.efi run above
    # has no way to set one, which is why this is a second boot rather than
    # another assertion on the first.
    #
    # TWO values, not one. A single capped run cannot tell "the cap works"
    # from "the heap never grew that far anyway"; the pair moves the ceiling
    # and requires the answer to move with it. The uncapped figure is
    # hundreds of MiB, so both are far below it.
    local capsrc="$WORK/cap-$cc_arch.c" capefi="$WORK/cap-$cc_arch.efi"
    cat > "$capsrc" <<'CAPSRC'
#include <stdlib.h>
#include <string.h>
#include <axl.h>
int main(void) {
    static void *keep[512];
    size_t n = 0;
    for (; n < 512; n++) {
        keep[n] = malloc(1u << 20);
        if (keep[n] == NULL) break;
        memset(keep[n], 0xA5, 1u << 20);
    }
    axl_printf("cap: grew to %lu MiB\r\n", (unsigned long) n);
    for (size_t i = 0; i < n; i++) free(keep[i]);
    return 0;
}
CAPSRC
    if "$AXL_CC" --arch "$cc_arch" --release "$capsrc" -o "$capefi" \
            >"$WORK/cap-build.log" 2>&1; then
        cat > "$WORK/cap.nsh" <<'CAPNSH'
@echo -off
fs0:
cd \
set AXL_LIBC_HEAP_MAX 8
cap.efi
set AXL_LIBC_HEAP_MAX 40
cap.efi
reset -s
CAPNSH
        cp -f "$capefi" "$WORK/cap.efi"
        timeout 240 "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$arch" \
            --timeout 180 --nsh "$WORK/cap.nsh" "$WORK/cap.efi" \
            >"$WORK/cap-run.log" 2>&1
        tr -d '\r' < "$WORK/cap-run.log" > "$WORK/cap-run.clean"

        # dlmalloc's own bookkeeping lives inside the cap, so the reachable
        # figure is a MiB or so under it. Asserted as a RANGE rather than an
        # exact number: pinning the overhead would fail on a newlib bump for
        # a reason that is not a defect.
        local got8 got40
        got8="$(grep -m1 -oP 'cap: grew to \K[0-9]+' "$WORK/cap-run.clean" || echo -1)"
        got40="$(grep -oP 'cap: grew to \K[0-9]+' "$WORK/cap-run.clean" | sed -n 2p || echo -1)"

        [[ "$got8"  -ge 4 && "$got8"  -le 8  ]]
        check "$?" "$arch: AXL_LIBC_HEAP_MAX=8 caps the heap (grew ${got8} MiB)"
        [[ "$got40" -ge 30 && "$got40" -le 40 ]]
        check "$?" "$arch: AXL_LIBC_HEAP_MAX=40 raises the cap (grew ${got40} MiB)"
        [[ "$got40" -gt "$got8" ]]
        check "$?" "$arch: the cap MOVES the ceiling (${got8} -> ${got40} MiB)"
    else
        check 1 "$arch: the heap-cap fixture builds"
        grep -E 'error' "$WORK/cap-build.log" | head -3 | sed 's/^/      /'
    fi
}

for a in "${ARCHES[@]}"; do
    run_one "$a"
done

echo ""
echo "libc-alloc: $pass passed, $fail failed, $skipped arch(es) skipped"
# A run where EVERY arch skipped is not a pass — same guard, and same exit code,
# as test-cxx-exceptions-qemu.sh.
if [[ "$skipped" -eq "${#ARCHES[@]}" ]]; then
    echo "libc-alloc: every arch SKIPPED — nothing was actually exercised."
    echo "  Stage the C++ SDK first: scripts/install.sh --arch all --cpp"
    exit 2
fi
[[ "$fail" -eq 0 ]]
