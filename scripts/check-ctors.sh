#!/bin/bash
# check-ctors.sh -- global constructors that NOTHING WILL RUN.
#
# AXL walks .init_array only (src/runtime/axl-cxxabi.c). The linker scripts
# also bound the legacy .ctors with __CTOR_LIST__/__CTOR_END__ and nothing
# reads those, so a non-empty .ctors is code the author expects to execute
# and that silently does not. axl-cc checks this per link; the in-tree build
# runs its own ld for every image, so this scans what the build PRODUCED: one
# pass over the tree's .so images (beside each .efi), covering every image.
#
# The reachable cause is a compiler, not a source edit: GCC's x86_64-*-elf
# target defaults to .ctors, and AXL's own toolchain escapes it only via
# --enable-initfini-array (14.3.0-axl2), so a stale AXL_X64_GXX/GCC silently
# brings it back.
#
# Usage: check-ctors.sh --tree DIR --binutils-prefix PREFIX
set -uo pipefail
TREE="" CROSS="" CROSS_SET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tree) TREE="$2"; shift 2 ;;
        --binutils-prefix) CROSS="$2"; CROSS_SET=1; shift 2 ;;
        *) echo "check-ctors: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
# An EMPTY prefix is allowed (the host nm: the self-test's fixtures); an
# absent one is not.
[[ -n "$TREE" && "$CROSS_SET" == 1 ]] || { echo "usage: $0 --tree DIR --binutils-prefix P" >&2; exit 2; }
n=0 bad=0
shopt -s nullglob
for so in "$TREE"/*.so "$TREE"/tools/*.so "$TREE"/drivers/*.so; do
    if ! syms=$("${CROSS}nm" "$so" 2>&1); then
        echo "check-ctors: FAIL -- ${CROSS}nm could not read $so."
        echo "  A gate that cannot see reports clean forever."
        echo "  $syms"
        exit 1
    fi
    lo=$(awk '$3 == "__CTOR_LIST__" { print $1 }' <<< "$syms")
    hi=$(awk '$3 == "__CTOR_END__" { print $1 }' <<< "$syms")
    [[ -n "$lo" && -n "$hi" ]] || continue
    n=$((n + 1))
    if [[ "$lo" != "$hi" ]]; then
        echo "check-ctors: FAIL -- $so has constructors in .ctors that"
        echo "  nothing runs (__CTOR_LIST__=$lo __CTOR_END__=$hi)."
        bad=$((bad + 1))
    fi
done
if (( bad )); then
    echo "  Cause: a compiler without --enable-initfini-array. AXL's own"
    echo "  toolchain has it from 14.3.0-axl2; check AXL_X64_GXX / _GCC (AXL_AA64_* on aa64)."
    exit 1
fi
if (( n == 0 )); then
    echo "check-ctors: no .so image with the .ctors bounds under $TREE -- build it first."
    echo "  A gate that scans nothing reports clean forever."
    exit 1
fi
echo "check-ctors: clean -- $n image(s), none with unwalked .ctors."
