#!/bin/bash
# test-meta: arch=none needs= est=60 local-only=0
# test-install-package-config.sh -- the CMake package names the compilers of
# the INSTALL, and install.sh publishes whatever the environment holds.
#
# WHY. install.sh's sed substituted both arches' compilers into
# axl-config.cmake from the environment of the run that installed. When the
# prefix became install() rules (Build-System-Design §10.16) the substitution
# first moved to configure time -- once, when the tree was first configured,
# and never again, since nothing reconfigures a tree for an environment
# variable. A review found it: an AXL_AA64_GCC override (or a conf bump) on a
# later install left one prefix whose axl-toolchains.conf named one compiler
# and whose axl-config.cmake another. It is generated at install time now;
# this pins that, for the OTHER arch (the tree's own arch is the compiler it
# was built with, which an environment cannot change after configure).
#
# Also: a DESTDIR in the environment must not divert the install --
# `cmake --install` honours it, and the publish then found an empty scratch
# and reported success.
#
# Usage: ./test/integration/test-install-package-config.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

PASS=0; FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

WORK="$(mktemp -d -t axl-pkgcfg.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# The aa64 C compiler the package names (its `set(AXL_C_COMPILER` line in the
# aa64 branch -- the first of the two).
aa64_cc() {
    grep -m1 '^    set(AXL_C_COMPILER ' "$1/lib/cmake/axl/axl-config.cmake" \
        | sed -E 's/^    set\(AXL_C_COMPILER +"(.*)"\)$/\1/'
}
DEFAULT="$(sed -n 's/^AXL_AA64_GCC_DEFAULT=//p' "$PROJECT_DIR/scripts/axl-toolchains.conf")"

echo "=== 1. no override: the conf's default ==="
if "$PROJECT_DIR/scripts/install.sh" --arch x64 --prefix "$WORK/a" > "$WORK/a.log" 2>&1; then
    check "install.sh --arch x64 succeeds" "0" "0"
else
    check "install.sh --arch x64 succeeds" "$?" "0"; tail -15 "$WORK/a.log"
fi
check "the aa64 compiler is axl-toolchains.conf's default" "$(aa64_cc "$WORK/a")" "$DEFAULT"

echo "=== 2. an override on a LATER install of the same tree reaches the package ==="
AXL_AA64_GCC=/nonexistent/aa64-override-gcc \
    "$PROJECT_DIR/scripts/install.sh" --arch x64 --prefix "$WORK/b" > "$WORK/b.log" 2>&1
check "install.sh with AXL_AA64_GCC set succeeds" "$?" "0"
check "the aa64 compiler is the installing environment's AXL_AA64_GCC" \
    "$(aa64_cc "$WORK/b")" "/nonexistent/aa64-override-gcc"

echo "=== 3. a DESTDIR in the environment does not divert the install ==="
mkdir -p "$WORK/destdir"
DESTDIR="$WORK/destdir" "$PROJECT_DIR/scripts/install.sh" --arch x64 --prefix "$WORK/c" \
    > "$WORK/c.log" 2>&1
check "install.sh with DESTDIR set succeeds" "$?" "0"
check "the prefix holds libaxl.a" "$([[ -f "$WORK/c/lib/axl/x64/libaxl.a" ]] && echo yes || echo no)" "yes"
check "nothing went under DESTDIR" "$(find "$WORK/destdir" -mindepth 1 | head -1)" ""

echo ""
echo "install-package-config: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
