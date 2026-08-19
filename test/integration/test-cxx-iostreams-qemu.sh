#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# test-cxx-iostreams-qemu.sh — the REAL <iostream> under UEFI, end to end.
#
# This suite exists because of P4 (docs/AXL-Libc-Substrate-Design.md §4d).
# Until P4 a DEFAULT C++ link carried libaxl-cxx.a and no libstdc++ at all:
# AXL hand-supplied operator new/delete, the five std::__throw_* halts and a
# few container internals, and every std:: stream symbol was an undefined
# reference. P4 swaps that archive for the toolchain's libstdc++, which is
# what makes iostreams reachable — the +100 KB the phase pays for.
#
# What each case pins, and why it is here rather than implied:
#
#   1. The fixture COMPILES and LINKS with no mode flag. Before P4 this
#      failed at link on std::cout / std::basic_ostream, and the compile half
#      always worked — so a compile-only check would have been green
#      throughout and proven nothing.
#   2. A default C++ link NAMES libstdc++.a, read off the link line with
#      --verbose rather than inferred from a link that happened to succeed.
#      This is the exact inverse of what test-cxx-hosted-qemu.sh asserted
#      before P4, and the inversion IS the phase.
#   3. libaxl-cxx.a is named NOWHERE. The archive is deleted, so a link line
#      still mentioning it means a stale staged SDK, which would make every
#      case below measure the previous build.
#   4. No ungated AVX in the produced image. libstdc++ is now IN the image
#      rather than replaced by AXL objects, so the AVX exposure that
#      check-no-avx exists for moved here. UEFI boots CR4.OSXSAVE clear, so a
#      VEX instruction is #UD, not a slowdown.
#   5. It RUNS and prints exact lines. Compile is not link and link is not
#      run — each of those three steps produced a wrong conclusion in the
#      session that built the C++ layer.
#   6. The SIZE BUDGET. §4d asks for a regression test that the accepted cost
#      "has not grown further", so the ceiling is asserted rather than
#      recorded in a doc nobody diffs.
#
# Auxiliary single-binary test (opts out of the test-axl.sh ratchet).
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-cxx-iostreams-qemu.sh [X64|AARCH64|both]

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
SRC="$SCRIPT_DIR/cxx-iostreams-selftest.cpp"
WORK="$(mktemp -d -t axl-cxx-iostreams.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# THE SIZE CEILING, per arch, in .text bytes. §4d asks for this: the P4 cost
# is accepted, so what a test has to pin is that it "has not grown further".
#
# MEASURED at P4 (2026-08-17, --release): x64 734,512 and aa64 702,576. A full
# iostreams image is most of libstdc++ — an order above the 80,912 a
# containers-only image costs, which test-cxx-hosted-qemu.sh pins separately.
#
# The headroom is ~7%, and that number is chosen rather than round: the
# regression this exists to catch is a STRUCTURAL one, and the two known
# candidates are both far larger — libstdc++'s verbose terminate handler
# coming back is ~112 KB (15%), and a second copy of the formatter is ~19 KB
# on top of a stdio that arrives with it. A ceiling loose enough to miss those
# would be decoration. Ordinary drift is nowhere near 50 KB.
declare -A TEXT_CEILING=([X64]=785000 [AARCH64]=755000)

EXPECTED=(
    "ctor: init_array ran"
    "out: text 42 -7 1"
    "out: float 3.5 -0.25"
    "out: widths 4000000000 -9000000000 18000000000"
    "out: from-a-std-string len=17"
    "err: to cerr"
    "out: endl-once"
    "sstr: built n=42 f=1.5"
    "sstr: parsed 7 hello 2.5 ok=1"
    "file: read [line-one 123][line-two 456]"
    "file: fields line-one 123"
    "iostreams: done"
)

pass=0
fail=0
skipped=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "  PASS: $2"; pass=$((pass + 1))
    else echo "  FAIL: $2"; fail=$((fail + 1)); fi
}

run_one() {
    local arch="$1" cc_arch lib_dir efi so log rc text
    case "$arch" in
        X64)     cc_arch="x64"  ;;
        AARCH64) cc_arch="aa64" ;;
    esac
    lib_dir="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/lib/axl/$cc_arch"

    echo "=== cxx-iostreams ($arch) ==="
    # Keyed on a cxxrt OBJECT, not on libaxl-cxx.a: P4 deletes that archive,
    # so keying the skip on it would make this suite skip forever, silently.
    if [[ ! -x "$AXL_CXX" || ! -f "$lib_dir/axl-cxxrt-terminate.o" ]]; then
        echo "  SKIP ($arch): no staged C++ SDK at $lib_dir"
        echo "        run: scripts/install.sh --arch $cc_arch --cpp"
        skipped=$((skipped + 1))
        return
    fi

    # The staged SDK is a SECOND TREE. An edit to include/axl that has not
    # been re-staged means everything below exercises the PREVIOUS build.
    # Compared by content, not mtime — install.sh deliberately avoids mtime
    # churn (install -C), so an mtime test reports drift that is not there.
    diff -rq "$PROJECT_DIR/include/axl" \
             "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/include/axl-sdk/axl" \
             >"$WORK/stage.diff" 2>&1
    check "$?" "$arch: staged headers match include/axl (else: install.sh --cpp)"
    [[ -s "$WORK/stage.diff" ]] && sed 's/^/      /' "$WORK/stage.diff" | head -5

    # -----------------------------------------------------------------
    # 1. Builds with NO mode flag, and 2/3 read the link line.
    # -----------------------------------------------------------------
    efi="$WORK/iostreams-$cc_arch.efi"
    so="${efi%.efi}.so"
    "$AXL_CXX" --arch "$cc_arch" --release --verbose "$SRC" -o "$efi" \
        >"$WORK/build.log" 2>&1
    rc=$?
    [[ "$rc" -eq 0 && -f "$efi" ]]
    check "$?" "$arch: <iostream> compiles and LINKS with no mode flag (rc=$rc)"
    if [[ ! -f "$efi" ]]; then
        grep -E "undefined reference|error:" "$WORK/build.log" \
            | head -12 | sed 's/^/      /'
        return
    fi

    # The exit status is checked ABOVE and the greps below only run on a link
    # that produced an image, so neither absence nor presence can be read off
    # a build that did not happen.
    grep -q "libstdc++\.a" "$WORK/build.log"
    check "$?" "$arch: a default C++ link NAMES libstdc++.a (P4 inversion)"

    ! grep -q "libaxl-cxx\.a" "$WORK/build.log"
    check "$?" "$arch: libaxl-cxx.a appears nowhere (the archive is retired)"

    # -----------------------------------------------------------------
    # 4. No ungated AVX reached the image.
    # -----------------------------------------------------------------
    python3 "$PROJECT_DIR/scripts/check-no-avx.py" "$so" >"$WORK/avx.log" 2>&1
    check "$?" "$arch: no ungated VEX/EVEX in the iostreams image"
    [[ -s "$WORK/avx.log" ]] && sed 's/^/      /' "$WORK/avx.log" | head -5

    # -----------------------------------------------------------------
    # 6. The size budget.
    # -----------------------------------------------------------------
    local size_bin="size"
    [[ "$arch" == "AARCH64" ]] && size_bin="aarch64-linux-gnu-size"
    text="$("$size_bin" -A "$so" 2>/dev/null | awk '$1==".text"{print $2}')"
    if [[ -z "$text" ]]; then
        # A ceiling that cannot read the size is a check that cannot fail.
        check 1 "$arch: could not read .text from $so ($size_bin)"
    else
        [[ "$text" -le "${TEXT_CEILING[$arch]}" ]]
        check "$?" "$arch: .text $text <= ceiling ${TEXT_CEILING[$arch]}"
    fi

    # -----------------------------------------------------------------
    # 5. It runs, and prints exactly these lines.
    # -----------------------------------------------------------------
    log="$WORK/run-$cc_arch.log"
    timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --timeout 90 "$efi" >"$log" 2>&1
    # Serial output carries CR; strip it so grep -Fx compares the line itself.
    tr -d '\r' < "$log" > "$log.clean"
    local line
    for line in "${EXPECTED[@]}"; do
        grep -Fxq "$line" "$log.clean"
        check "$?" "$arch: output line — $line"
    done

    # ONE CRLF per LF, asserted on the RAW log. Every check above reads
    # "$log.clean", which is `tr -d '\r'` -- all carriage returns deleted --
    # so a doubled \r\r\n is invisible to them by construction. The thing
    # that could regress is AXL's console transcode (console_transcode_crlf)
    # translating a LF that libstdc++'s filebuf already translated.
    #
    # Anchored on a line the fixture is known to emit, and PAIRED with a
    # positive control: if the marker is absent from the raw log the absence
    # of \r\r proves nothing, so the control is what stops this passing on a
    # boot that never printed.
    grep -qF 'out: endl-once' "$log"
    check "$?" "$arch: raw log carries the endl marker (control for the next check)"

    ! grep -qP 'out: endl-once\r\r' "$log"
    check "$?" "$arch: one LF becomes ONE CRLF on the wire (no \\r\\r\\n)"
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
    echo
done

if (( skipped == ${#ARCHES[@]} )); then
    echo "cxx-iostreams: every arch SKIPPED — nothing was verified."
    echo "  Stage the C++ SDK first: scripts/install.sh --arch all --cpp"
    exit 2
fi

echo "cxx-iostreams: $pass passed, $fail failed, $skipped arch(es) skipped"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
