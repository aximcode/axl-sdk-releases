#!/bin/bash
# test-meta: arch=none needs=cmake,ninja est=20 cpu=2 local-only=0
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
# Host-only: configures the CMake build into a private tree and runs axl-cc's
# and install.sh's argument handling. No QEMU, no build. (The Makefile had
# the variant first; the CMake build took it over in slice 6.)
#
# Usage: ./test/integration/test-toolchain-variant.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Configure the CMake build (the toolchain file, cmake/AxlToolchain.cmake,
# is where the variant is read) into a private tree under $WORK -- never a
# shared build/<preset>, which a running suite builds in. A refusal is a
# FATAL_ERROR in the toolchain file, before any compiler check. ENV pairs
# first, then nothing else: the preset's arch comes from ARCH=.
mk() {   # mk [ARCH=a] [VAR=value...] -> the configure's output; its status
    rm -rf "$WORK/tree"
    reconf "$@"
}
reconf() {   # reconf [ARCH=a] [VAR=value...] -- configure $WORK/tree as it stands
    local arch=x64 envs=() a
    for a in "$@"; do
        case "$a" in ARCH=*) arch="${a#ARCH=}" ;; *) envs+=("$a") ;; esac
    done
    (cd "$PROJECT_DIR" && env -u AXL_TOOLCHAIN -u AXL_X64_GCC -u AXL_X64_GXX \
        -u AXL_X64_BINUTILS_PREFIX -u AXL_AA64_GCC -u AXL_AA64_GXX \
        -u AXL_AA64_BINUTILS_PREFIX ${envs[@]+"${envs[@]}"} \
        cmake --preset "$arch-debug" -B "$WORK/tree" 2>&1)
}
# The C compiler a configure chose (CMake's record of the detected compiler;
# a toolchain file's compiler is not a cache entry), or nothing.
# `|| true`: no such file is an empty answer, never an errexit.
cc_of() { sed -n 's/^set(CMAKE_C_COMPILER "\(.*\)")$/\1/p' "$WORK"/tree/CMakeFiles/*/CMakeCCompiler.cmake 2>/dev/null || true; }

# $1 label, $2 expected-exit(0|nonzero as "fail"), $3 regex the output must
# match, $4.. ARCH= and environment
expect() {
    local label="$1" want="$2" want_re="$3"; shift 3
    local out rc
    out="$(mk "$@")" && rc=0 || rc=$?
    if [[ "$want" == "fail" && "$rc" -eq 0 ]]; then
        fail "$label (expected a refusal, the configure succeeded)"; return
    fi
    if [[ "$want" == "ok" && "$rc" -ne 0 ]]; then
        fail "$label (expected success, the configure exited $rc)"
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

echo "=== AXL_TOOLCHAIN, the CMake build ==="

# 1. An unknown variant is a typo, and the message must name the real ones.
#    Asserting BOTH names, because a message that lists only the default
#    leaves the reader no way to discover the other.
expect "unknown variant is refused, naming axl and cross" fail \
    "AXL_TOOLCHAIN=bogus" AXL_TOOLCHAIN=bogus
expect "the refusal names the 'axl' variant" fail '`axl`' AXL_TOOLCHAIN=bogus
expect "the refusal names the 'cross' variant" fail '`cross`' AXL_TOOLCHAIN=bogus

# 1b. `auto` is the x64 DEFAULT that axl-cc reports and --verbose prints, so a
#     consumer who exports the default's own name must still be able to build
#     the SDK. It maps to `axl`: the SDK builds ITSELF bare-metal either way
#     (AXL-Host-Toolchain-Design.md §8), so this is a spelling the build
#     accepts, not a second resolution path.
expect "auto is accepted" ok "" AXL_TOOLCHAIN=auto
auto_cc="$(cc_of)"
mk AXL_TOOLCHAIN=axl > /dev/null || true
axl_cc_only="$(cc_of)"
if [[ "$auto_cc" == "$axl_cc_only" && -n "$auto_cc" ]]; then
    pass "auto and axl resolve the same compiler"
else
    fail "auto and axl differ ('$auto_cc' vs '$axl_cc_only')"
fi

# 1c-1d. `host` is refused HERE even though axl-cc accepts it, and the refusal
#        has to say why rather than reading as an unknown-variant typo: the
#        variant governs what a CONSUMER compiles, and libaxl.a is built and
#        shipped by the bare-metal toolchain regardless (§8). The first half
#        reads the message and NOT merely the status, because the
#        unknown-variant error echoes back whatever you typed, "host"
#        included.
host_mk="$(mk AXL_TOOLCHAIN=host)" && host_rc=0 || host_rc=$?
if [[ "$host_rc" -ne 0 ]] && grep -qE '\bhost\b' <<<"$host_mk" \
   && ! grep -q 'is not a toolchain variant' <<<"$host_mk"; then
    pass "the build refuses host by name, not as an unknown-variant typo"
else
    fail "host refusal (rc=$host_rc)"
    printf '%s\n' "$host_mk" | head -4 | sed 's/^/      /'
fi
expect "the host refusal says the SDK builds itself bare-metal" fail \
    "bare-metal" AXL_TOOLCHAIN=host
expect "the host refusal points at what host DOES govern" fail \
    "consumer|axl-cc" AXL_TOOLCHAIN=host

# 2. `cross` with nothing named. The message must name the VARIABLES to set --
#    pointing at the installer here would be actively wrong, since the user
#    has just said they are supplying their own.
expect "cross with no locator is refused" fail \
    "AXL_X64_GCC" AXL_TOOLCHAIN=cross
out="$(mk AXL_TOOLCHAIN=cross)" || true
if grep -q 'install-toolchain' <<<"$out"; then
    fail "cross does not tell you to run the installer"
else
    pass "cross does not tell you to run the installer"
fi
# One locator missing is refused too, by ITS name: the defaults are never
# consulted for the rest.
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
expect "cross with the C++ locator missing names it" fail \
    "needs AXL_X64_GXX set" AXL_TOOLCHAIN=cross \
    AXL_X64_GCC="$AXL_X64_GCC_DEFAULT" \
    AXL_X64_BINUTILS_PREFIX="$AXL_X64_BINUTILS_PREFIX_DEFAULT"

# 3. `cross` WITH locators configures, with THOSE tools: the real toolchain
#    under other names (symlinks in $WORK/bin), so a build that consulted the
#    conf defaults instead would be seen choosing /opt.
mkdir -p "$WORK/bin"
ln -sf "$AXL_X64_GCC_DEFAULT" "$WORK/bin/my-gcc"
ln -sf "$AXL_X64_GXX_DEFAULT" "$WORK/bin/my-g++"
for t in "$AXL_X64_BINUTILS_PREFIX_DEFAULT"*; do
    ln -sf "$t" "$WORK/bin/my-${t#"$AXL_X64_BINUTILS_PREFIX_DEFAULT"}"
done
MY=(AXL_TOOLCHAIN=cross AXL_X64_GCC="$WORK/bin/my-gcc" AXL_X64_GXX="$WORK/bin/my-g++"
    AXL_X64_BINUTILS_PREFIX="$WORK/bin/my-")
expect "cross with locators set configures" ok "" "${MY[@]}"
check_mine() {   # check_mine LABEL -- the tree uses $WORK/bin's tools
    local got
    # `|| true`: a count of 0 is grep's exit 1, and errexit would end the run.
    got="$(cc_of) $(grep -cF -- "$WORK/bin/my-ld " "$WORK/tree/build.ninja" || true) $(grep -cF -- "${AXL_X64_BINUTILS_PREFIX_DEFAULT}ld " "$WORK/tree/build.ninja" || true)"
    if [[ "${got%% *}" == "$WORK/bin/my-gcc" && "$(cut -d' ' -f2 <<<"$got")" != 0 \
          && "${got##* }" == 0 ]]; then
        pass "$1"
    else
        fail "$1 (cc, my-ld links, /opt binutils uses: $got)"
    fi
}
check_mine "...and builds with THOSE: its compiler and binutils, none of /opt's"

# 3b. A tree keeps its toolchain. Ninja reconfigures by itself after any
#     CMakeLists.txt edit, in whatever environment the build started from:
#     regenerated WITHOUT the variables, the tree once kept its compiler and
#     linked with /opt's binutils.
out="$(reconf)" && rc=0 || rc=$?
check_mine "a regenerate without the variables keeps the tree's toolchain (rc=$rc)"
# ...and one that NAMES another is refused, rather than ignored.
expect_reconf() {   # expect_reconf LABEL REGEX ENV...
    local label="$1" re="$2" out rc; shift 2
    out="$(reconf "$@")" && rc=0 || rc=$?
    # CMake wraps a message across lines: read it as one.
    if [[ "$rc" -ne 0 ]] && tr -s '\n ' '  ' <<<"$out" | grep -qE "$re"; then pass "$label"
    else fail "$label (rc=$rc)"; printf '%s\n' "$out" | { grep -m3 -E 'Error|configured' || true; } | sed 's/^/      /'; fi
}
expect_reconf "a reconfigure naming another compiler is refused, with the way out" \
    "AXL_X64_GCC=$AXL_X64_GCC_DEFAULT, but this tree was configured with $WORK/bin/my-gcc.*cmake --fresh" \
    AXL_TOOLCHAIN=cross AXL_X64_GCC="$AXL_X64_GCC_DEFAULT"
expect_reconf "a reconfigure naming another variant is refused" \
    "AXL_TOOLCHAIN=axl, but this tree was configured as cross" AXL_TOOLCHAIN=axl

# 3c. One locator overridden under `axl` is the tree's too: a plain
#     regenerate keeps it, and is not mistaken for a moved conf default.
mk AXL_X64_GCC="$WORK/bin/my-gcc" > /dev/null || true
out="$(reconf)" && rc=0 || rc=$?
check_one="$(cc_of) rc=$rc"
if [[ "$check_one" == "$WORK/bin/my-gcc rc=0" ]]; then
    pass "an axl tree with one locator from the environment regenerates without it"
else
    fail "an axl tree with one overridden locator: $check_one"
    printf '%s\n' "$out" | { grep -m3 'Error\|configured' || true; } | sed 's/^/      /'
fi
# 3d. ...while a locator that came from the conf DEFAULT is held to it: a
#     toolchain bump must not leave a tree half on the old one. Played on a
#     copy of the build definition whose conf names another compiler.
CT="$WORK/conftree"; mkdir -p "$CT/scripts"
cp -r "$PROJECT_DIR/cmake" "$CT/"
cp "$PROJECT_DIR/scripts/axl-toolchains.conf" "$CT/scripts/"
printf 'cmake_minimum_required(VERSION 3.29)\nproject(probe LANGUAGES C)\n' > "$CT/CMakeLists.txt"
conf_cfg() { env -u AXL_TOOLCHAIN -u AXL_X64_GCC -u AXL_X64_GXX -u AXL_X64_BINUTILS_PREFIX \
    cmake -S "$CT" -B "$WORK/conftree-b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$CT/cmake/AxlToolchain.cmake" 2>&1; }
out="$(conf_cfg)" && rc=0 || rc=$?
[[ $rc -eq 0 ]] && pass "control: the copied definition configures" || fail "the copied definition does not configure (rc=$rc)"
sed -i.bak "s|^AXL_X64_GCC_DEFAULT=.*|AXL_X64_GCC_DEFAULT=$WORK/bin/my-gcc|" "$CT/scripts/axl-toolchains.conf"
rm -f "$CT/scripts/axl-toolchains.conf.bak"
out="$(conf_cfg)" && rc=0 || rc=$?
if [[ $rc -ne 0 ]] && tr -s '\n ' '  ' <<<"$out" | grep -qF "scripts/axl-toolchains.conf now names $WORK/bin/my-gcc for AXL_X64_GCC, but this tree was configured with $AXL_X64_GCC_DEFAULT"; then
    pass "a conf default that moved is refused, naming both"
else
    fail "a moved conf default (rc=$rc)"
    printf '%s\n' "$out" | { grep -m3 'Error\|configured' || true; } | sed 's/^/      /'
fi

# 3e. newlib's struct mallinfo, PROBED from the toolchain's libc.a: a
#     __malloc_current_mallinfo of 40 bytes is ten ints against a header of
#     ten size_t, and src/cxxrt/axl-cxxrt-alloc.c displaces it only under
#     AXL_NEWLIB_MALLINFO_INT. The Makefile probed this; the CMake build had
#     dropped it (found by the slice-6 sweep). The pinned toolchains are
#     80-byte (correct), so a stub nm stands in for a broken one.
#     Each stub reads its LAST argument as the libc.a and refuses a path that
#     is not one, so the half that passes also proves -print-file-name
#     resolved; the 80-byte half asserts the probe's STATUS line, so a probe
#     that never ran cannot satisfy "no flag".
mi_flag() { grep -c -- '-DAXL_NEWLIB_MALLINFO_INT=1' "$WORK/tree/compile_commands.json" 2>/dev/null || true; }
mi_stub() {   # mi_stub <body run after the libc.a check>
    rm -f "$WORK/bin/my-nm"
    printf '#!/bin/sh\nfor f; do :; done\ncase "$f" in */libc.a) [ -f "$f" ] || exit 9 ;; *) exit 9 ;; esac\n%s\n' "$1" \
        > "$WORK/bin/my-nm"
    chmod +x "$WORK/bin/my-nm"
}
out_mi_ok="$(mk "${MY[@]}" 2>&1)" || true
check_mi_ok="$(mi_flag)"
# The decoy listed first is a LONGER name with the other size: the probe must
# match the whole symbol, not a prefix of it.
mi_stub 'echo "0000000000000000 0000000000000050 b __malloc_current_mallinfo_x"; echo "0000000000000000 0000000000000028 B __malloc_current_mallinfo"'
out_mi_bad="$(mk "${MY[@]}" 2>&1)" || true
check_mi_bad="$(mi_flag)"
if [[ "$check_mi_ok" == 0 && "$out_mi_ok" == *"newlib mallinfo: 80 bytes ("*"/libc.a)"* \
      && "$check_mi_bad" -gt 0 && "$out_mi_bad" == *"newlib mallinfo: 40 bytes ("* ]]; then
    pass "a 40-byte mallinfo in libc.a defines AXL_NEWLIB_MALLINFO_INT; the real 80-byte one does not"
else
    fail "the mallinfo probe (80-byte: $check_mi_ok uses, 40-byte: $check_mi_bad uses)"
fi
# A probe that cannot see is FATAL, never read as "80 bytes, fine". CMake
# wraps a message across lines, so each is matched with its whitespace folded.
flat() { tr -s ' \n' '  ' <<< "$1"; }
mi_stub 'exit 3'
st=0; out_mi_run="$(mk "${MY[@]}" 2>&1)" || st=$?; out_mi_run="$(flat "$out_mi_run")"
mi_stub 'echo "0000000000000000 0000000000000050 T malloc"'
st2=0; out_mi_none="$(mk "${MY[@]}" 2>&1)" || st2=$?; out_mi_none="$(flat "$out_mi_none")"
mi_stub 'echo "0000000000000000 0000000000000030 B __malloc_current_mallinfo"'
st3=0; out_mi_odd="$(mk "${MY[@]}" 2>&1)" || st3=$?; out_mi_odd="$(flat "$out_mi_odd")"
ln -sf "${AXL_X64_BINUTILS_PREFIX_DEFAULT}nm" "$WORK/bin/my-nm"
if [[ "$st" != 0 && "$out_mi_run" == *"newlib mallinfo probe: $WORK/bin/my-nm on /"*"/libc.a failed (exit 3)"* \
      && "$st2" != 0 && "$out_mi_none" == *"/libc.a defines no __malloc_current_mallinfo with a size"* \
      && "$st3" != 0 && "$out_mi_odd" == *"__malloc_current_mallinfo is 48 bytes in /"*"/libc.a; only 40 (ten int) and 80 (ten size_t) are known"* ]]; then
    pass "a mallinfo probe that cannot run, finds no symbol, or finds an unknown size refuses to configure"
else
    fail "the mallinfo probe's refusals (nm fails: $st, no symbol: $st2, 48 bytes: $st3)"
fi

# 4. The default is unchanged. This is the regression half: the variant must
#    be invisible to every existing caller.
mk > /dev/null || true
default_cc="$(cc_of)"
if [[ "$default_cc" == "$axl_cc_only" && -n "$default_cc" ]]; then
    pass "unset and AXL_TOOLCHAIN=axl resolve identically"
else
    fail "unset and AXL_TOOLCHAIN=axl differ ('$default_cc' vs '$axl_cc_only')"
fi

# 4b. `cross` on aa64 names the aa64 locators.
expect "cross on aa64 names AXL_AA64_GCC" fail \
    "AXL_AA64_GCC" ARCH=aa64 AXL_TOOLCHAIN=cross

echo "=== AXL_TOOLCHAIN, install.sh (the third build path) ==="

# 4c. install.sh resolves the same toolchain to bake into the generated
#     axl-config.cmake. A variant two build paths honour and the third ignores
#     is the drift `gate.check-flag-parity` exists over -- and the failure is
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
# satisfied by the build's own refusal further down, so deleting install.sh's
# check entirely would still look green -- confirmed by sabotage, which is how this
# assertion came to exist. The value of the early check IS the earliness: the
# alternative is a reader watching "Building (x64, ...)" scroll past before a
# configure error about a variable that was knowably unset beforehand.
if grep -q "Building (" <<<"$out"; then
    fail "install.sh started a build before refusing"
    printf '%s\n' "$out" | grep -n "Building (" | head -2 | sed 's/^/      /'
else
    pass "install.sh refuses before starting a build"
fi

# ...EVERY locator: the CMake build needs the C++ compiler and the binutils
# too, so the C compiler alone would pass this check and then fail the build.
out="$(cd "$PROJECT_DIR" && AXL_TOOLCHAIN=cross AXL_X64_GCC="$WORK/bin/my-gcc" AXL_X64_GXX= \
        AXL_X64_BINUTILS_PREFIX= ./scripts/install.sh --arch x64 \
        --prefix "$WORK/sdk" 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -ne 0 ]] && grep -q "needs AXL_X64_GXX set" <<<"$out" && ! grep -q "Building (" <<<"$out"; then
    pass "install.sh refuses cross with the C++ locator missing, before a build"
else
    fail "install.sh cross-without-GXX (rc=$rc)"
    printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
fi

out="$(cd "$PROJECT_DIR" && AXL_TOOLCHAIN=bogus ./scripts/install.sh \
        --arch x64 --prefix "$WORK/sdk" 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -ne 0 ]] && grep -qE "\bcross\b" <<<"$out"; then
    pass "install.sh refuses an unknown variant"
else
    fail "install.sh unknown variant (rc=$rc)"
    printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
fi

# 4d. The same two answers from the third build path. `host` refused with the
#     §8 reason, `auto` accepted -- the packager must not be the one place
#     where the default's own name is a fatal typo.
out="$(cd "$PROJECT_DIR" && AXL_TOOLCHAIN=host ./scripts/install.sh \
        --arch x64 --prefix "$WORK/sdk" 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -ne 0 ]] && grep -q "bare-metal" <<<"$out" \
                     && grep -qE "consumer|axl-cc" <<<"$out"; then
    pass "install.sh refuses host, with the reason not just the refusal"
else
    fail "install.sh host refusal (rc=$rc)"
    printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
fi

# `auto` must get PAST the variant check. Asserted on the message rather than
# on a successful install, which would cost a full build: the failure this
# guards is the variant being rejected as unknown, and a locator pointed at
# nothing makes the run die immediately afterwards for a reason that is
# visibly NOT that. "It failed" alone would be satisfied by the very refusal
# under test, so the assertion reads WHICH failure it was.
out="$(cd "$PROJECT_DIR" && AXL_TOOLCHAIN=auto AXL_X64_GCC=/nonexistent/gcc \
        ./scripts/install.sh --arch x64 --prefix "$WORK/sdk" 2>&1)" \
        && rc=0 || rc=$?
if grep -q "is not a toolchain variant" <<<"$out"; then
    fail "install.sh rejects auto as an unknown variant (rc=$rc)"
    printf '%s\n' "$out" | grep -m2 "not a toolchain variant" | sed 's/^/      /'
else
    pass "install.sh accepts auto"
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
#      entry point every known consumer uses, so a variant the CMake build
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

# ...and does not send them to the installer, for the same reason the CMake
# build does not: they have just said they are supplying the toolchain
# themselves.
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
