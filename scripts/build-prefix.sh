#!/bin/bash
# build-prefix.sh — where a given configuration's artefacts land.
#
# Usage:
#   scripts/build-prefix.sh [ARCH] [--abs]
#
#   ARCH   x64 | aa64 | X64 | AARCH64   (default: $TEST_ARCH, else x64)
#   --abs  absolute path instead of the repo-relative prefix
#
# Honours ARCH, BUILD and AXL_TLS together, because it asks the Makefile
# rather than restating its rule.
#
# WHY THIS EXISTS. PREFIX is a function of three inputs and grew a fourth:
# ARCH always, BUILD since the RELEASE tree split, and AXL_TLS now that a TLS
# build gets its own tree (a toggle WIPES the objects, and test-axl.sh builds
# TLS-off while run-integration.sh exports AXL_TLS=1, so sharing one prefix
# rebuilt ~300 objects on every alternation). Every hand-written
# "out/native-$arch" is a copy of that rule frozen at one input, and 98 of
# them across 66 scripts silently pointed at a tree nothing had built the
# first time the TLS suffix appeared.
#
# It is a SCRIPT, not a shell function, because 51 of those scripts source
# nothing at all -- they are standalone and cannot use a helper from
# common-test.sh. Scripts that DO source it get test_build_prefix /
# test_build_dir, which memoize around this.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ARCH_IN=""
ABS=false
for arg in "$@"; do
    case "$arg" in
        --abs) ABS=true ;;
        -*)    echo "ERROR: unknown flag '$arg'" >&2; exit 2 ;;
        *)     ARCH_IN="$arg" ;;
    esac
done

case "${ARCH_IN:-${TEST_ARCH:-x64}}" in
    AARCH64|aarch64|aa64) ARCH=aa64 ;;
    *)                    ARCH=x64  ;;
esac

PREFIX="$(make -s -C "$REPO_ROOT" ARCH="$ARCH" print-prefix)"

if $ABS; then
    printf '%s/%s\n' "$REPO_ROOT" "$PREFIX"
else
    printf '%s\n' "$PREFIX"
fi
