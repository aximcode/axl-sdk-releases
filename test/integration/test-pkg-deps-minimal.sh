#!/bin/bash
# test-meta: arch=x64 needs=podman est=40 local-only=0
# test-pkg-deps-minimal.sh — are the package's DECLARED dependencies enough?
#
# WHY THIS EXISTS. release.yml's .deb smoke test installs the package and drives
# axl-cc / axl-c++ to PE32+ output, which proves the package WORKS. It cannot
# prove the dependency list is CORRECT, because it runs on a GitHub runner that
# already ships gcc, g++, binutils and make — it would pass against an empty
# --depends. That blindness is not hypothetical: it is exactly how a missing
# `g++` dependency shipped, so a user with gcc but no g++ installed a package
# whose axl-c++ did not work.
#
# So this runs the consumer path on an image that has NO toolchain at all, with
# ONLY the declared dependencies installed. A dependency that is needed and not
# declared fails here and nowhere else.
#
# WHAT IT DOES NOT COVER. The cross toolchains are MOUNTED rather than
# installed, because they are not apt packages today — install-toolchain.sh
# fetches them to /opt. Once they ship as their own packages, the mounts become
# `apt-get install axl-sdk-toolchain-*` and this gets stronger. Until then it
# answers "given the toolchains, is the DECLARED apt set sufficient?", which is
# the question the smoke test cannot answer at all.
#
# The aa64 rows are the load-bearing ones: they pass with no aarch64 package
# installed, which is what makes dropping gcc-aarch64-linux-gnu and
# binutils-aarch64-linux-gnu correct rather than merely untested.

set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$DIR"

command -v podman >/dev/null 2>&1 || { echo "SKIP: podman not available"; exit 0; }

# Read the declared deps back from the workflow rather than restating them --
# the point of the test is that THIS list is sufficient, so a second copy here
# would let the two drift and the test would bless the wrong set.
# Every --depends in the FIRST .deb fpm invocation -- the axl-sdk package --
# however many there are. Two earlier shapes were wrong: matching a fixed
# "--depends A --depends B" failed the moment one was dropped, and an unbounded
# range swept in the devkit package's fpm block further down the file (qemu,
# ovmf, mtools...). `exit` on the first `-C stage` bounds it to one package.
FPM_BLOCK=$(awk '/fpm -s dir -t deb/{f=1} f{print} f&&/-C stage/{exit}' \
            .github/workflows/release.yml)
# An EMPTY dependency list is the expected end state, not a parse failure --
# T2 dropped the last entry (host g++) when x64 C++ moved to our own toolchain.
# So the parse is validated on the BLOCK, not on the result: no fpm block means
# the workflow changed shape and this test must follow it, while a block with no
# --depends means the package genuinely needs nothing from the host.
if [[ -z "$FPM_BLOCK" ]]; then
    echo "FAIL: could not find the .deb fpm invocation in"
    echo "      .github/workflows/release.yml (it changed shape; this test"
    echo "      must follow it)"
    exit 1
fi
DEPS=$(printf '%s\n' "$FPM_BLOCK" \
       | grep -oE -- '--depends [a-zA-Z0-9.+-]+' | awk '{print $2}' | tr '\n' ' ' \
       | sed 's/ $//')
echo "declared .deb dependencies: ${DEPS:-<none>}"

# shellcheck source=/dev/null
. scripts/axl-toolchains.conf
X64_TC="${AXL_X64_TOOLCHAIN_DIR}"
AA64_TC="${AXL_AA64_TOOLCHAIN_DIR}"
for d in "$X64_TC" "$AA64_TC"; do
    [[ -d "$d" ]] || { echo "SKIP: toolchain not installed: $d"; exit 0; }
done
# --abs: this path is both tested here and MOUNTED into the container
# below, so a repo-relative answer would depend on the caller's cwd.
STAGE="$("$DIR/scripts/sdk-prefix.sh" --abs)"
[[ -x "$STAGE/bin/axl-cc" ]] || { echo "SKIP: SDK not staged (run ./scripts/install.sh --arch all --cpp)"; exit 0; }

OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

podman run --rm \
    -v "$STAGE:/opt/axl-sdk:ro" \
    -v "$X64_TC:$X64_TC:ro" \
    -v "$AA64_TC:$AA64_TC:ro" \
    -v "$DIR/sdk/examples:/ex:ro" \
    -e "AXL_DEPS=$DEPS" \
    debian:stable-slim bash -c '
set -u
export DEBIAN_FRONTEND=noninteractive
# The image must genuinely lack a toolchain, or the run proves nothing.
for t in gcc g++ ld ar objcopy; do
    command -v "$t" >/dev/null 2>&1 && { echo "PRECONDITION-FAIL: $t already present"; exit 1; }
done
echo "precondition: image has no gcc/g++/ld/ar/objcopy"
# Skipping apt entirely when nothing is declared is not a shortcut -- it is what
# makes the run STRONGER. `apt-get install` with no arguments still refreshes
# indexes and can pull recommends, so running it would leave the question of
# whether the empty list was really empty. Not running it at all means the four
# builds below execute on the image exactly as the precondition found it.
if [ -n "$AXL_DEPS" ]; then
    apt-get update -qq >/dev/null 2>&1
    # shellcheck disable=SC2086
    apt-get install -y -qq $AXL_DEPS >/dev/null 2>&1 || { echo "FAIL: apt could not install: $AXL_DEPS"; exit 1; }
else
    echo "no declared dependencies -- installing nothing"
fi

# PE header check without `file`, which the slim image does not carry: the
# e_lfanew at 0x3c points at "PE\0\0" followed by the machine word.
pe_machine() {
    local off
    off=$(od -An -tu4 -j60 -N4 "$1" | tr -d " ")
    od -An -tx2 -j $((off + 4)) -N2 "$1" | tr -d " "
}
rc=0
# The package ships bin/axl-install-toolchain, and axl-cc names it as THE
# remedy in every missing-toolchain diagnostic -- so "the declared deps are
# sufficient" has to cover that command too, not only axl-cc.
#
# Asserted as TOOL PRESENCE rather than by running it: the real thing downloads
# ~150 MB of toolchain per run, which is not a per-test cost worth paying to
# learn something `command -v` already answers. This is the gap that let curl
# and xz-utils go undeclared -- the four builds below never touch either,
# because the toolchains are MOUNTED here rather than installed.
for t in curl xz; do
    if command -v "$t" >/dev/null 2>&1; then
        echo "  PASS axl-install-toolchain prerequisite: $t"
    else
        echo "  FAIL axl-install-toolchain needs $t, and no declared dependency"
        echo "       provides it -- a user following axl-cc s own advice would"
        echo "       get \"$t: command not found\""
        rc=1
    fi
done
check() {  # label driver extra-args source expected-machine
    if /opt/axl-sdk/bin/$2 $3 "$4" -o /tmp/o.efi >/tmp/err 2>&1; then
        local m; m=$(pe_machine /tmp/o.efi)
        if [ "$m" = "$5" ]; then echo "  PASS $1 (machine 0x$m)"
        else echo "  FAIL $1: machine 0x$m, expected 0x$5"; rc=1; fi
    else
        echo "  FAIL $1: build failed"; sed "s/^/       /" /tmp/err | tail -3; rc=1
    fi
}
check "x64 C"    axl-cc  ""             /ex/hello.c   8664
check "x64 C++"  axl-c++ ""             /ex/hello.cpp 8664
check "aa64 C"   axl-cc  "--arch aa64"  /ex/hello.c   aa64
check "aa64 C++" axl-c++ "--arch aa64"  /ex/hello.cpp aa64
exit $rc
' 2>&1 | tee "$OUT/run.log"
rc=${PIPESTATUS[0]}

echo ""
if [[ $rc -eq 0 ]]; then
    echo "pkg-deps-minimal: PASS — the declared set is sufficient"
else
    echo "pkg-deps-minimal: FAIL — a needed dependency is missing from --depends,"
    echo "  or a shipped build path reaches for a tool the package never asked for."
fi
exit $rc
