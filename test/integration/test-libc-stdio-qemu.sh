#!/bin/bash
# test-meta: arch=both needs= est=110
# test-libc-stdio-qemu.sh — newlib's stdio runs on AxlStream.
#
# P2 of AXL-Libc-Substrate-Design.md §4d/§4c.1. newlib defines NONE of the
# porting-layer symbols -- it cannot know what EFI_FILE_PROTOCOL is -- so
# write/read/open/close/lseek/fstat are AXL's in every possible design. Until
# P2 they returned -1, so newlib's stdio could not move a byte.
#
# What is asserted is that THIRD-PARTY C works unmodified, which is the actual
# goal: AXL has had axl_printf and axl_fopen all along; what it lacked was
# printf and fopen.
#
# The two halves are asserted separately because they have different
# prerequisites -- the console half needs only the six pre-existing stubs
# implemented, while the file half additionally needs `open`, which AXL did not
# define at all (measured: it was an undefined-reference LINK failure, not a
# runtime one).
#
# Exact byte counts, not "wrote something": %d/%s going through newlib's
# vfprintf rather than AxlFormat is exactly where a bridge silently truncates.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-libc-stdio-qemu.sh [X64|AARCH64|both]

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
SRC="$SCRIPT_DIR/libc-stdio-selftest.cpp"
C_SRC="$SCRIPT_DIR/libc-c-selftest.c"
FD_SRC="$SCRIPT_DIR/libc-fd-selftest.c"
AXL_CC="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-cc"
WORK="$(mktemp -d -t axl-libc-stdio.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# The plain-C half (P3). Separate fixture because it is built with axl-cc and
# never mentions C++ -- the point is that ordinary C gets the C library, with
# no C++ toolchain and no exceptions anywhere near the link.
EXPECTED_C=(
    "cstdio: 7 ok"
    "  printf_ret=13"
    "  PASS: printf() works from plain C"
    "  PASS: malloc() works from plain C"
    "  PASS: strcpy/strcmp round-trip"
    "  PASS: axl_malloc still allocates independently"
    "=== 4 passed, 0 failed ==="
)

EXPECTED=(
    "stdio: printf 42 works"
    "  printf_ret=23"
    "  PASS: printf() returns the byte count it wrote"
    "stdio: fputs works"
    "  PASS: fopen(\"w\") returns a FILE*"
    "  PASS: fwrite() reports 11 bytes written"
    "  PASS: fclose() succeeds"
    "  PASS: fopen(\"r\") reopens the file just written"
    "  PASS: fread() returns the 11 bytes"
    "  PASS: fread() returns the right bytes"
    "=== 16 passed, 0 failed ==="
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

    echo "=== libc-stdio ($arch) ==="
    if [[ ! -x "$AXL_CXX" || ! -f "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/lib/axl/$cc_arch/axl-cxxrt-alloc.o" ]]; then
        echo "  SKIP ($arch): no staged C++ SDK with the porting layer"
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
    n="$(nm_count "$so" "$nm" ' (_vfprintf_r|__smakebuf_r)$')"
    [[ "$n" != "ERR" && "$n" -ge 1 ]]
    check "$?" "$arch: control — the .so is readable and newlib stdio IS linked (found $n)"

    # Under §2-DECISION dlmalloc is the allocator newlib is SUPPOSED to use, so
    # its presence is the healthy state -- this assertion was inverted when the
    # design was one allocator wearing both name sets. It still earns its place:
    # stdio's findfp.o and fvwrite.o allocate through _malloc_r, so a stdio
    # image with no allocator behind it would fail at buffer allocation rather
    # than at I/O, and the runtime symptom is indistinguishable from a broken
    # write().
    n="$(nm_count "$so" "$nm" "$DLMALLOC_SYMS")"
    [[ "$n" != "ERR" && "$n" -ge 3 ]]
    check "$?" "$arch: stdio's allocator (newlib dlmalloc) IS linked (found $n)"

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
    # 3. PLAIN C gets the same library (P3).
    # -----------------------------------------------------------------
    # Built with axl-cc, not axl-c++: before P3 a C link was libaxl.a and
    # nothing else, so printf and malloc were undefined references for the
    # majority of consumers. This also pins the §2-DECISION interaction --
    # malloc reaches dlmalloc, which grows through AXL's sbrk, and on aa64 a
    # missing sbrk is a mutual recursion rather than a link error.
    local cefi="$WORK/cself-$cc_arch.efi"
    "$AXL_CC" --arch "$cc_arch" --release "$C_SRC" -o "$cefi" \
        >"$WORK/cbuild-$cc_arch.log" 2>&1
    rc=$?
    check "$rc" "$arch: the plain-C fixture builds with axl-cc (rc=$rc)"
    if [[ "$rc" -eq 0 ]]; then
        local clog="$WORK/crun-$cc_arch.log"
        timeout 180 "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$arch" --timeout 90 \
            "$cefi" >"$clog" 2>&1
        tr -d '\r' < "$clog" > "$clog.clean"
        for line in "${EXPECTED_C[@]}"; do
            grep -Fxq "$line" "$clog.clean"
            check "$?" "$arch: C output line — ${line# *}"
        done
    else
        grep -E "undefined reference|error" "$WORK/cbuild-$cc_arch.log" \
            | head -4 | sed 's/^/      /'
    fi

    # -----------------------------------------------------------------
    # 4. THE RAW DESCRIPTOR LAYER, called directly rather than through stdio.
    # -----------------------------------------------------------------
    # Everything above reaches open/read/write/lseek/close only THROUGH
    # printf and fopen, which cannot tell "the fd layer is correct" from
    # "stdio happens not to use it that way". The whole of <fstream>, every
    # FILE* and every ported C library sits on these, so they are asserted on
    # their own terms: lseek's three whences including a negative end-relative
    # offset, descriptor reuse after close, the table-full path, and the error
    # returns stdio swallows.
    #
    # The footer count is pinned like the others, and both sabotages tried
    # against it were caught -- dropping the slot release in close() fails 2
    # assertions, turning SEEK_END into SEEK_SET fails 4.
    local fdefi="$WORK/fdself-$cc_arch.efi"
    "$AXL_CC" --arch "$cc_arch" --release "$FD_SRC" -o "$fdefi" \
        >"$WORK/fdbuild-$cc_arch.log" 2>&1
    rc=$?
    check "$rc" "$arch: the raw-descriptor fixture builds (rc=$rc)"
    if [[ "$rc" -eq 0 ]]; then
        local fdlog="$WORK/fdrun-$cc_arch.log"
        timeout 180 "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$arch" --timeout 90 \
            "$fdefi" >"$fdlog" 2>&1
        tr -d '\r' < "$fdlog" > "$fdlog.clean"
        grep -Fxq "=== fd: 27 passed, 0 failed ===" "$fdlog.clean"
        check "$?" "$arch: fd layer — 27 passed, 0 failed"
        # Name what broke, rather than only that something did.
        grep -E '^  FAIL: ' "$fdlog.clean" | head -6 | sed 's/^/      /'
    else
        grep -E "undefined reference|error" "$WORK/fdbuild-$cc_arch.log" \
            | head -4 | sed 's/^/      /'
    fi
}

for a in "${ARCHES[@]}"; do
    run_one "$a"
done

echo ""
echo "libc-stdio: $pass passed, $fail failed, $skipped arch(es) skipped"
# A run where EVERY arch skipped is not a pass — same guard, and same exit code,
# as test-cxx-exceptions-qemu.sh.
if [[ "$skipped" -eq "${#ARCHES[@]}" ]]; then
    echo "libc-stdio: every arch SKIPPED — nothing was actually exercised."
    echo "  Stage the C++ SDK first: scripts/install.sh --arch all --cpp"
    exit 2
fi
[[ "$fail" -eq 0 ]]
