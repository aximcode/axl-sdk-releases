#!/bin/bash
# check-cxx-entry.sh -- the firmware entry macros keep C linkage from C++.
#
# AXL_DRIVER, AXL_SHARED_DRIVER and AXL_SERVICE_DRIVER each define the entry
# point the firmware calls. Compiled from C++ without AXL_ENTRY_LINKAGE the
# symbol is name-mangled, the crt0 cannot find it, and the image does not
# link (or worse, links a stub). test/cxx-entry-linkage.cpp instantiates each
# macro; this compiles it as the tree compiles a C++ TU (compile-like.py) and
# reads the object: nm for the entry symbols, objdump for the fixture's
# .init_array entry (the runtime half of the contract -- a fixture whose
# constructor registered nothing would let it pass by testing nothing).
#
# THE TOOLS ARE CHECKED FIRST. A tool that cannot START returns an empty
# answer, and empty is indistinguishable from "the symbol is mangled" and "no
# constructor was registered" unless it is checked first. Both were reported,
# wrongly, on the v4.3.0 tag run: the CI container had no libdebuginfod.so.1,
# so the cross objdump could not load, and this gate blamed codegen.
#
# Usage: check-cxx-entry.sh --tree BUILD-DIR --binutils-prefix PREFIX --like SRC
#   (CTest gate.check-cxx-entry passes all three: the arch tree, its
#   binutils, and the C++ TU whose compile the fixture borrows)
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="" CROSS="" LIKE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tree) TREE="$2"; shift 2 ;;
        --binutils-prefix) CROSS="$2"; shift 2 ;;
        --like) LIKE="$2"; shift 2 ;;
        *) echo "check-cxx-entry: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[[ -n "$TREE" && -n "$CROSS" && -n "$LIKE" ]] \
    || { echo "usage: check-cxx-entry.sh --tree DIR --binutils-prefix P --like SRC" >&2; exit 2; }

for t in nm objdump; do
    if ! "${CROSS}$t" --version >/dev/null 2>&1; then
        echo "check-cxx-entry: CANNOT RUN — ${CROSS}$t does not execute."
        "${CROSS}$t" --version 2>&1 | head -3 | sed 's/^/    /'
        echo "  This gate READS the compiled object: nm for the entry symbols,"
        echo "  objdump for the .init_array count. A tool that cannot START"
        echo "  returns an empty answer, which must not read as a verdict."
        exit 1
    fi
done

W="$(mktemp -d -t axl-cxxentry.XXXXXXXX)"
trap 'rm -rf "$W"' EXIT
fail=0
for variant in DRIVER SHARED SERVICE; do
    obj="$W/$variant.o"
    python3 "$REPO/scripts/compile-like.py" --tree "$TREE" --like "$LIKE" \
        --src "$REPO/test/cxx-entry-linkage.cpp" -o "$obj" \
        "--add=-DAXL_ENTRY_FIXTURE_$variant" > "$W/err" 2>&1
    rc=$?
    if (( rc == 125 )); then
        echo "check-cxx-entry: CANNOT RUN — compile-like.py could not build the compile command:"
        sed 's/^/    /' "$W/err" | head -6
        exit 1
    elif (( rc != 0 )); then
        echo "check-cxx-entry: FAIL — AXL_$variant fixture did not compile from C++"
        sed 's/^/    /' "$W/err" | head -12
        fail=1; continue
    fi
    syms="DriverEntry"
    [[ "$variant" == DRIVER ]] && syms="_AxlEntry DriverEntry"
    if ! nmout=$("${CROSS}nm" "$obj" 2>&1); then
        echo "check-cxx-entry: CANNOT RUN — ${CROSS}nm failed on the object"
        printf '%s\n' "$nmout" | head -3 | sed 's/^/    /'
        fail=1; continue
    fi
    for sym in $syms; do
        if grep -qE " T $sym\$" <<< "$nmout"; then
            echo "check-cxx-entry: AXL_$variant — $sym is unmangled (C linkage)"
        else
            echo "check-cxx-entry: FAIL — AXL_$variant: $sym is missing or name-mangled in C++ (needs AXL_ENTRY_LINKAGE)"
            grep -i "$sym" <<< "$nmout" | sed 's/^/    /'
            fail=1
        fi
    done
    if ! odout=$("${CROSS}objdump" -h "$obj" 2>&1); then
        echo "check-cxx-entry: CANNOT RUN — ${CROSS}objdump failed on the object"
        printf '%s\n' "$odout" | head -3 | sed 's/^/    /'
        fail=1; continue
    fi
    n=$(awk '$2 == ".init_array" { print $3 }' <<< "$odout")
    if [[ -z "$n" ]] || (( 0x$n == 0 )); then
        echo "check-cxx-entry: FAIL — AXL_$variant: the fixture's global constructor"
        echo "  registered NO .init_array entry, so the runtime half of this"
        echo "  contract would pass by testing nothing."
        fail=1
    fi
done
(( fail == 0 )) && echo "check-cxx-entry: clean — 3 macros compile from C++, entries unmangled, ctors registered"
exit $fail
