#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-axl-build-backend.sh -- scripts/axl-build.sh builds with CMake, the
# only build, and its "pre-built by the runner" fingerprint watches the tree
# a call builds into.
#
# AXL_BUILD_BACKEND chose make or cmake until the Makefile was retired in
# 5.0.0 (docs/AXL-Build-System-Design.md §10.12): `make` is refused by name,
# never quietly run as cmake -- a caller still asking for the Makefile is
# told it is gone.
#
# The fingerprint's state part is the tree's build.ninja (and
# CMakeFiles/rules.ninja), which a reconfigure with other options rewrites
# and a wipe deletes: a wiped build/ must not leave every skip standing over
# images that are gone.
#
# Runs a COPY of axl-build.sh in a fake tree; nothing is built.
#
# Usage: ./test/integration/test-axl-build-backend.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

WORK="$(mktemp -d -t axl-backend.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT
TREE="$WORK/tree"
mkdir -p "$TREE/scripts" "$TREE/build/x64-debug" "$TREE/build/aa64-release"
SOURCE_SCRIPT="$PROJECT_DIR/scripts/axl-build.sh"   # (a variable: not a build line to check-test-meta)
cp "$SOURCE_SCRIPT" "$TREE/scripts/"
BUILDER="$TREE/scripts/axl-build.sh"
printf 'rule cc-v1\n' > "$TREE/build/x64-debug/build.ninja"

clean() { env -u AXL_BUILD_BACKEND -u ARCH -u BUILD -u PREFIX -u RTK_USB_UNDI "$@"; }
state() { clean "$@" | sed -n 's/.* state=\([^ ]*\).*/\1/p'; }

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
differ() {  # differ NAME A B
    if [[ -n "$2" && "$2" != "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    a: $2"; echo "    b: $3"; FAIL=$((FAIL + 1))
    fi
}

echo "=== axl-build.sh: the default backend ==="
check "no AXL_BUILD_BACKEND: the CMake tree's prefix" \
    "$(clean "$BUILDER" --print-prefix ARCH=x64)" "build/x64-debug"
check "... per ARCH x BUILD" \
    "$(clean "$BUILDER" --print-prefix ARCH=aa64 BUILD=RELEASE)" "build/aa64-release"
check "no AXL_BUILD_BACKEND: the fingerprint names cmake" \
    "$(clean "$BUILDER" --env-fingerprint ARCH=x64 agent | cut -d' ' -f1)" "backend=cmake"
check "AXL_BUILD_BACKEND=cmake is the same build" \
    "$(clean AXL_BUILD_BACKEND=cmake "$BUILDER" --print-prefix ARCH=x64)" "build/x64-debug"
out=$(clean AXL_BUILD_BACKEND=make "$BUILDER" --print-prefix ARCH=x64 2>&1); rc=$?
check "AXL_BUILD_BACKEND=make is refused (2), saying the Makefile is gone" "$rc $out" \
    "2 axl-build.sh: AXL_BUILD_BACKEND=make -- the Makefile was retired in 5.0.0; the build is CMake + Ninja (unset AXL_BUILD_BACKEND)"
out=$(clean AXL_BUILD_BACKEND=ninja "$BUILDER" ARCH=x64 agent 2>&1); rc=$?
check "any other AXL_BUILD_BACKEND is refused (2), by name" "$rc $out" \
    "2 axl-build.sh: AXL_BUILD_BACKEND='ninja' -- the only build is cmake"
out=$(clean "$BUILDER" ARCH=x64 PREFIX=out/native-x64 agent 2>&1); rc=$?
check "make's PREFIX= is refused (2), not dropped" "$rc" "2"

echo "=== the fingerprint watches the call's tree ==="
c1=$(state "$BUILDER" --env-fingerprint ARCH=x64 agent)
printf 'rule cc-v2\n' > "$TREE/build/x64-debug/build.ninja"
differ "cmake: build.ninja rewritten (a reconfigure) moves it" \
    "$(state "$BUILDER" --env-fingerprint ARCH=x64 agent)" "$c1"
c2=$(state "$BUILDER" --env-fingerprint ARCH=x64 agent)
rm "$TREE/build/x64-debug/build.ninja"
differ "cmake: the tree wiped moves it" \
    "$(state "$BUILDER" --env-fingerprint ARCH=x64 agent)" "$c2"
printf 'rule cc-v1\n' > "$TREE/build/x64-debug/build.ninja"
check "cmake: the call's own tree only (aa64-release changing is not x64-debug's)" \
    "$(printf 'x\n' > "$TREE/build/aa64-release/build.ninja"; state "$BUILDER" --env-fingerprint ARCH=x64 agent)" "$c1"
mkdir -p "$TREE/build/x64-debug/CMakeFiles"
printf 'rule cc\n  command = gcc-v1\n' > "$TREE/build/x64-debug/CMakeFiles/rules.ninja"
c3=$(state "$BUILDER" --env-fingerprint ARCH=x64 agent)
printf 'rule cc\n  command = gcc-v2\n' > "$TREE/build/x64-debug/CMakeFiles/rules.ninja"
differ "cmake: a reconfigure that rewrites only rules.ninja (another compiler) moves it" \
    "$(state "$BUILDER" --env-fingerprint ARCH=x64 agent)" "$c3"
# An arch or build no preset names: still one whole line, exit 0 -- an empty
# fingerprint in the runner's manifest would read as "no state" to the next.
fp_bad=$(clean "$BUILDER" --env-fingerprint ARCH=bogus agent); rc=$?
check "cmake: an arch it cannot name still prints a fingerprint (exit 0)" \
    "$rc $(cut -d' ' -f1 <<< "$fp_bad")" "0 backend=cmake"

echo "axl-build-backend: $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
