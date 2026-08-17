#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-toolchain-variant.sh — AXL_TOOLCHAIN selects WHICH toolchain provides
# the compiler and binutils, and says so when the answer is unusable.
#
# Why a variant exists at all, when AXL_X64_GCC already overrode the compiler:
# a locator alone cannot express INTENT. Every comparable project pairs the two
# -- Zephyr has ZEPHYR_TOOLCHAIN_VARIANT beside ZEPHYR_SDK_INSTALL_DIR /
# CROSS_COMPILE, EDK2 has TOOL_CHAIN_TAG beside GCC_AARCH64_PREFIX, and the
# Linux kernel has LLVM=1 beside CROSS_COMPILE. None of them selects a
# toolchain by bare prefix alone.
#
# The concrete bug this closes: the README told macOS and native-Windows users
# to build with `make CROSS=<prefix>-`, which has selected only BINUTILS since
# 0bf6ed51 replaced `CC = $(CROSS)gcc` with an AXL_*_GCC lookup. The compiler
# stayed pointed at /opt, so the documented command could not work on a host
# that has no /opt toolchain -- and it failed deep in a recipe rather than at
# the point the intent was expressed.
#
# `cross` therefore does NOT fall back to the axl-toolchains.conf defaults.
# Falling back is what made the failure late and confusing; refusing with the
# variable names is the whole feature.
#
# Host-only: parses the Makefile and runs axl-cc's argument handling. No QEMU,
# no build. Every make invocation passes PREFIX into a scratch dir, because
# changing a toolchain variable trips the build-state signature and WIPES the
# objects under the prefix it resolves -- that is exactly the developer tree
# this suite runs beside.
#
# Usage: ./test/integration/test-toolchain-variant.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Run make far enough to parse the toolchain block, into a scratch prefix.
# -n so nothing is built; the guards are $(error), which fire at parse time.
mk() {
    (cd "$PROJECT_DIR" && make -n PREFIX="$WORK/prefix" "$@" all 2>&1)
}

# $1 label, $2 expected-exit(0|nonzero as "fail"), $3 regex the output must
# match, $4.. make args
expect() {
    local label="$1" want="$2" want_re="$3"; shift 3
    local out rc
    out="$(mk "$@")" && rc=0 || rc=$?
    if [[ "$want" == "fail" && "$rc" -eq 0 ]]; then
        fail "$label (expected a refusal, make succeeded)"; return
    fi
    if [[ "$want" == "ok" && "$rc" -ne 0 ]]; then
        fail "$label (expected success, make exited $rc)"
        printf '%s\n' "$out" | head -5 | sed 's/^/      /'
        return
    fi
    if [[ -n "$want_re" ]] && ! printf '%s\n' "$out" | grep -qE "$want_re"; then
        fail "$label (output did not match /$want_re/)"
        printf '%s\n' "$out" | head -5 | sed 's/^/      /'
        return
    fi
    pass "$label"
}

echo "=== AXL_TOOLCHAIN, the Makefile ==="

# 1. An unknown variant is a typo, and the message must name the real ones.
#    Asserting BOTH names, because a message that lists only the default
#    leaves the reader no way to discover the other.
expect "unknown variant is refused, naming axl and cross" fail \
    "AXL_TOOLCHAIN.*bogus" AXL_TOOLCHAIN=bogus
expect "the refusal names the 'axl' variant" fail "\baxl\b" AXL_TOOLCHAIN=bogus
expect "the refusal names the 'cross' variant" fail "\bcross\b" AXL_TOOLCHAIN=bogus

# 2. `cross` with nothing named. The message must name the VARIABLES to set --
#    pointing at axl-install-toolchain here would be actively wrong, since the
#    user has just said they are supplying their own.
expect "cross with no locator is refused" fail \
    "AXL_X64_GCC" AXL_TOOLCHAIN=cross ARCH=x64
expect "cross does not tell you to run the installer" fail \
    "AXL_TOOLCHAIN=cross" AXL_TOOLCHAIN=cross ARCH=x64

# 3. `cross` WITH locators parses. Uses the real toolchain as the stand-in for
#    a user-supplied one -- the point is that the conf defaults are not being
#    consulted, not which compiler it is.
TC_CONF="$PROJECT_DIR/scripts/axl-toolchains.conf"
# shellcheck disable=SC1090
. "$TC_CONF"
expect "cross with locators set parses" ok "" \
    AXL_TOOLCHAIN=cross ARCH=x64 \
    AXL_X64_GCC="$AXL_X64_GCC_DEFAULT" \
    AXL_X64_GXX="$AXL_X64_GXX_DEFAULT" \
    AXL_X64_BINUTILS_PREFIX="$AXL_X64_BINUTILS_PREFIX_DEFAULT"

# 4. The default is unchanged. This is the regression half: the variant must
#    be invisible to every existing caller, of which there are 149.
default_cc="$(cd "$PROJECT_DIR" && make -s ARCH=x64 print-cc-libc-include 2>&1)"
axl_cc_var="$(cd "$PROJECT_DIR" && AXL_TOOLCHAIN=axl make -s ARCH=x64 \
    print-cc-libc-include 2>&1)"
if [[ "$default_cc" == "$axl_cc_var" && -n "$default_cc" ]]; then
    pass "unset and AXL_TOOLCHAIN=axl resolve identically"
else
    fail "unset and AXL_TOOLCHAIN=axl differ ('$default_cc' vs '$axl_cc_var')"
fi

# 4b. ARM_TOOLCHAIN is an older override spelling that supplies the aa64 C++
#     compiler when AXL_AA64_GXX is unset. It is a DEFAULT by another name, so
#     `cross` must not consult it either -- otherwise a stale environment
#     variable silently supplies a compiler under the one variant whose whole
#     promise is that nothing is supplied implicitly. Narrow, but it is the
#     exact hole the variant exists to close.
expect "cross ignores ARM_TOOLCHAIN as a C++ fallback" fail \
    "AXL_AA64_GXX" \
    AXL_TOOLCHAIN=cross ARCH=aa64 AXL_CPP=1 \
    AXL_AA64_GCC="$AXL_AA64_GCC_DEFAULT" \
    AXL_AA64_BINUTILS_PREFIX="$AXL_AA64_BINUTILS_PREFIX_DEFAULT" \
    ARM_TOOLCHAIN="$AXL_AA64_TOOLCHAIN_DIR"

echo "=== AXL_TOOLCHAIN, install.sh (the third build path) ==="

# 4c. install.sh resolves the same toolchain to bake into the generated
#     axl-config.cmake. A variant two build paths honour and the third ignores
#     is the drift `make check-flag-parity` exists over -- and the failure is
#     the silent one: `cross` with a forgotten locator would quietly stamp the
#     /opt default into a consumer's CMake package.
out="$(cd "$PROJECT_DIR" && AXL_TOOLCHAIN=cross AXL_X64_GCC= AXL_X64_GXX= \
        AXL_X64_BINUTILS_PREFIX= ./scripts/install.sh --arch x64 \
        --prefix "$WORK/sdk" 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -ne 0 ]] && grep -q "AXL_X64_GCC" <<<"$out"; then
    pass "install.sh refuses cross with no locator, naming the variable"
else
    fail "install.sh cross-without-locator (rc=$rc)"
    printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
fi

# ...and refuses BEFORE starting a build. Without this the assertion above is
# satisfied by make's own refusal further down, so deleting install.sh's check
# entirely would still look green -- confirmed by sabotage, which is how this
# assertion came to exist. The value of the early check IS the earliness: the
# alternative is a reader watching "Building (x64, ...)" scroll past before a
# Makefile error about a variable that was knowably unset beforehand.
if grep -q "Building (" <<<"$out"; then
    fail "install.sh started a build before refusing"
    printf '%s\n' "$out" | grep -n "Building (" | head -2 | sed 's/^/      /'
else
    pass "install.sh refuses before starting a build"
fi

out="$(cd "$PROJECT_DIR" && AXL_TOOLCHAIN=bogus ./scripts/install.sh \
        --arch x64 --prefix "$WORK/sdk" 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -ne 0 ]] && grep -qE "\bcross\b" <<<"$out"; then
    pass "install.sh refuses an unknown variant"
else
    fail "install.sh unknown variant (rc=$rc)"
    printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
fi

echo "=== AXL_TOOLCHAIN, axl-cc ==="

# scripts/axl-cc, NOT out/bin/axl-cc. This exercises argument handling only --
# no staged library, no toolchain -- so the source is both sufficient and the
# right subject: reading the staged copy would test whatever install.sh last
# copied there and report a source fix as absent, which is the stale-artifact
# trap this tree has paid for before.
AXL_CC="$PROJECT_DIR/scripts/axl-cc"
printf 'int main(void){return 0;}\n' > "$WORK/t.c"

# 5-6. The same two refusals, from the consumer-facing driver. axl-cc is the
#      entry point every known consumer uses, so a variant the Makefile
#      understands and axl-cc does not would be the drift check-flag-parity
#      exists to prevent, one layer up.
out="$(AXL_TOOLCHAIN=bogus "$AXL_CC" "$WORK/t.c" -o "$WORK/t.efi" 2>&1)" \
    && rc=0 || rc=$?
if [[ "$rc" -ne 0 ]] && grep -qE "\baxl\b" <<<"$out" \
                     && grep -qE "\bcross\b" <<<"$out"; then
    pass "axl-cc refuses an unknown variant, naming both"
else
    fail "axl-cc unknown variant (rc=$rc)"; printf '%s\n' "$out" | head -4 | sed 's/^/      /'
fi

out="$(AXL_TOOLCHAIN=cross AXL_X64_GCC= AXL_X64_GXX= AXL_X64_BINUTILS_PREFIX= \
       "$AXL_CC" "$WORK/t.c" -o "$WORK/t.efi" 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -ne 0 ]] && grep -q "AXL_X64_GCC" <<<"$out"; then
    pass "axl-cc refuses cross with no locator, naming the variable"
else
    fail "axl-cc cross-without-locator (rc=$rc)"; printf '%s\n' "$out" | head -4 | sed 's/^/      /'
fi

# ...and does not send them to the installer, for the same reason the Makefile
# does not: they have just said they are supplying the toolchain themselves.
if grep -qE "axl-install-toolchain|install-toolchain\.sh" <<<"$out"; then
    fail "axl-cc tells a 'cross' user to run the installer"
    printf '%s\n' "$out" | head -6 | sed 's/^/      /'
else
    pass "axl-cc does not tell a 'cross' user to run the installer"
fi

echo "=== the README no longer documents a build path that cannot work ==="

# 7. `make CROSS=<prefix>-` selected the compiler until 0bf6ed51 and selects
#    only binutils now. It must not survive as a documented build command.
#    Matching the COMMAND shape, not the word CROSS, which legitimately
#    appears when describing binutils.
if grep -qE '^\s*make\b.*\bCROSS=' "$PROJECT_DIR/README.md"; then
    fail "README still documents 'make CROSS=' as a build command"
    grep -nE '^\s*make\b.*\bCROSS=' "$PROJECT_DIR/README.md" | head -3 | sed 's/^/      /'
else
    pass "README documents no 'make CROSS=' build command"
fi

echo
echo "toolchain-variant: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
