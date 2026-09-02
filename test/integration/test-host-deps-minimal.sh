#!/bin/bash
# test-meta: arch=x64 needs=podman est=8 local-only=0
# test-host-deps-minimal.sh — are the SDK's host prerequisites enough?
#
# WHY THIS EXISTS. release.yml's smoke test extracts the SDK tarball and drives
# axl-cc / axl-c++ to PE32+ output, which proves the artifact WORKS. It cannot
# prove the prerequisite list is CORRECT, because it runs on a GitHub runner
# that already ships gcc, g++, binutils and make — it would pass against an
# empty list. That blindness is not hypothetical: it is exactly how a missing
# `g++` dependency once shipped, so a user with gcc but no g++ installed an SDK
# whose axl-c++ did not work.
#
# So this runs the consumer path on an image that has NO toolchain at all, with
# ONLY the declared prerequisites installed. One that is needed and undeclared
# fails here and nowhere else.
#
# WHERE "DECLARED" NOW LIVES. It used to be the .deb's `--depends` metadata,
# read back out of release.yml's fpm block. D2 retired the packages (§17), so
# the declaration moved to the thing that replaced them: packaging/install.sh's
# own `need_cmd` calls, which are what a user actually hits — install.sh
# refuses with "need 'X' on PATH" before touching anything. That is a better
# owner than the fpm metadata was, because it is executable and the installer
# enforces it at run time rather than merely asserting it at build time.
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

# Read the required COMMANDS back from install.sh rather than restating them --
# the point of the test is that THIS set is sufficient, so a second copy here
# would let the two drift and the test would bless the wrong one.
mapfile -t NEEDED < <(grep -oE '^[[:space:]]*need_cmd [a-zA-Z0-9_.+-]+' \
                      packaging/install.sh | awk '{print $2}' | sort -u)
if [[ "${#NEEDED[@]}" -eq 0 ]]; then
    echo "FAIL: no need_cmd calls in packaging/install.sh (it changed shape;"
    echo "      this test must follow it)"
    exit 1
fi

# axl-install-toolchain is not reached through install.sh's need_cmd list, but
# axl-cc names it as THE remedy in every missing-toolchain diagnostic, so a
# user who follows that advice needs its prerequisites too: it curls a tarball
# and `tar -xJf`s it. This pair is exactly the gap that once let curl and
# xz-utils go undeclared -- the four builds below never touch either, because
# the toolchains are MOUNTED here rather than installed.
NEEDED+=(curl xz)

# Command -> Debian package. An unmapped command is a HARD FAILURE, not a
# silent skip: adding a need_cmd must force a decision about how a minimal
# image gets it, which is the whole question this test asks.
declare -A PKG_FOR=( [curl]=curl [tar]=tar [sha256sum]=coreutils
                     [tr]=coreutils [xz]=xz-utils [awk]=mawk
                     [sed]=sed [grep]=grep )
declare -A WANT_PKG=()
unmapped=""
for _c in "${NEEDED[@]}"; do
    if [[ -n "${PKG_FOR[$_c]:-}" ]]; then WANT_PKG[${PKG_FOR[$_c]}]=1
    else unmapped="$unmapped $_c"; fi
done
if [[ -n "$unmapped" ]]; then
    echo "FAIL: install.sh needs$unmapped, and this test has no Debian package"
    echo "      mapping for it. Add one to PKG_FOR -- a command with no known"
    echo "      package is a command a minimal image cannot get."
    exit 1
fi
DEPS="$(printf '%s\n' "${!WANT_PKG[@]}" | sort | tr '\n' ' ' | sed 's/ $//')"
echo "required commands:  ${NEEDED[*]}"
echo "declared packages:  ${DEPS:-<none>}"

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
# The SDK ships bin/axl-install-toolchain, and axl-cc names it as THE remedy
# in every missing-toolchain diagnostic -- so "the declared set is sufficient"
# has to cover that command too, not only axl-cc.
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
        echo "  FAIL axl-install-toolchain needs $t, and no declared package"
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
    echo "host-deps-minimal: PASS — the declared set is sufficient"
else
    echo "host-deps-minimal: FAIL — install.sh's need_cmd set is missing something,"
    echo "  or a shipped build path reaches for a tool the SDK never asked for."
fi
exit $rc
