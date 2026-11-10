#!/bin/bash
# check-examples.sh -- every sdk/examples/ source compiles as a CONSUMER would.
#
# The examples ship in the SDK tarball, so a consumer compiles them; 20 of 51
# were once reachable by no build rule at all. Each .c / .cpp is compiled with
# the tree's own application flags (scripts/compile-like.py, borrowing a C and
# a C++ TU's command) MINUS what the SDK does not give a consumer: raw-UEFI
# access (-DAXL_ALLOW_UEFI, unless the example says `axl-example:
# allow-uefi`) and the library's private TLS build (src/net, the mbedtls
# headers and its config). Reusing the library's own flags once granted the
# gate access the SDK denies an application, so an example no consumer could
# build reported clean.
#
# Too FEW sources is a failure, not a pass: a gate that compiles nothing
# reports clean forever.
#
# Usage: check-examples.sh --tree DIR --like-c SRC --like-cxx SRC [--min N] [--dir D]
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="" LIKE_C="" LIKE_CXX="" MIN=20 DIR="$REPO/sdk/examples"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tree) TREE="$2"; shift 2 ;;
        --like-c) LIKE_C="$2"; shift 2 ;;
        --like-cxx) LIKE_CXX="$2"; shift 2 ;;
        --min) MIN="$2"; shift 2 ;;
        --dir) DIR="$2"; shift 2 ;;
        *) echo "check-examples: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[[ -n "$TREE" && -n "$LIKE_C" && -n "$LIKE_CXX" ]] \
    || { echo "usage: $0 --tree DIR --like-c SRC --like-cxx SRC" >&2; exit 2; }

shopt -s nullglob
srcs=("$DIR"/*.c "$DIR"/*.cpp)
shopt -u nullglob
if (( ${#srcs[@]} < MIN )); then
    echo "check-examples: FAIL — found ${#srcs[@]} source(s) under $DIR,"
    echo "  fewer than the $MIN expected. A gate that compiles nothing"
    echo "  reports clean forever, so too few is a failure, not a pass."
    exit 1
fi
W="$(mktemp -d -t axl-chkex.XXXXXXXX)"
trap 'rm -rf "$W"' EXIT
# What a consumer's compile does not have (see the header).
CONSUMER=(--drop=-DAXL_ALLOW_UEFI --drop=-DAXL_HAVE_TLS
          '--drop=-DMBEDTLS_CONFIG_FILE="axl-mbedtls-config.h"'
          "--drop=-I$REPO/src/net" "--drop=-isystem $REPO/deps/mbedtls/include")
fail=0
for src in "${srcs[@]}"; do
    # The consumer drops apply to the C TU's command, which carries them; the
    # C++ TU's carries none of them (compile-like refuses a drop that matches
    # nothing, so a spelling change cannot quietly re-grant them).
    like="$LIKE_C" drops=("${CONSUMER[@]}")
    [[ "$src" == *.cpp ]] && { like="$LIKE_CXX"; drops=(); }
    extra=()
    grep -q 'axl-example: allow-uefi' "$src" && extra=(--add=-DAXL_ALLOW_UEFI)
    python3 "$REPO/scripts/compile-like.py" --tree "$TREE" --like "$like" \
        --src "$src" -o "$W/$(basename "$src").o" "${drops[@]}" "${extra[@]}" \
        > "$W/err" 2>&1
    rc=$?
    if (( rc == 125 )); then
        echo "check-examples: CANNOT RUN — compile-like.py could not build the compile command:"
        sed 's/^/    /' "$W/err" | head -6
        exit 1
    elif (( rc != 0 )); then
        echo "check-examples: FAIL — ${src#"$REPO"/}"
        sed 's/^/    /' "$W/err"
        fail=1
    fi
done
if (( fail )); then
    echo "  Examples ship in the SDK tarball — a consumer compiles these."
    exit 1
fi
echo "check-examples: clean — ${#srcs[@]} ${DIR#"$REPO"/} sources compile."
