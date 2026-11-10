#!/bin/bash
# test-meta: arch=none needs=cmake,ninja,ccache est=40 cpu=5 local-only=0
# test-cmake-ccache.sh -- the CMake build compiles through ccache, and a cached
# build is the SAME build.
#
# WHAT IS PINNED (cmake/AxlCcache.cmake):
#
#   - AXL_CCACHE=AUTO (the default) with ccache present puts it in front of
#     every C and C++ compile, and a rebuild after a clean is served from the
#     cache: every compile a HIT, none a miss. "ccache is configured" is not the
#     claim -- a launcher that never hits costs time and saves none.
#   - The cached build's images are BYTE-IDENTICAL to the cold build's. A cache
#     that returns a different object is a wrong build, however fast.
#   - compile_commands.json names the compiler, not ccache: clang-tidy and
#     scripts/cmake-parity.py read that database, and both would see `ccache`
#     as the compiler.
#   - AXL_CCACHE=OFF puts no launcher in build.ninja -- the CONTROL that shows
#     the launcher check below can see a launcher at all.
#   - AXL_CCACHE=ON with no ccache to be found FAILS the configure: asked for
#     and silently absent is the failure this tree is arranged against. AUTO
#     with none found configures without it.
#
# A private CCACHE_DIR, so the hit counts are this test's own and the user's
# cache is neither read nor filled.
#
# Host-only: no QEMU.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# `cmake --preset` reads CMakePresets.json from the cwd: run from the tree,
# whatever the caller's cwd (under ctest it was the build tree).
cd "$PROJECT_DIR" || exit 1
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
set +e
set -uo pipefail
export TEST_SKIP_RATCHET=1

echo "=== CMake build through ccache ==="
echo ""

for t in cmake ninja ccache; do
    command -v "$t" >/dev/null || { echo "SKIP: $t not installed"; exit 77; }
done
# --print-stats, which the hit counts are read from, is ccache 4.
CCACHE_MAJOR="$(ccache --version | awk 'NR == 1 { split($3, v, "."); print v[1] }')"
if [[ ! "$CCACHE_MAJOR" =~ ^[0-9]+$ || "$CCACHE_MAJOR" -lt 4 ]]; then
    echo "SKIP: ccache 4 or later needed (found: $(ccache --version | head -1))"
    exit 77
fi

# The developer's own launcher and ccache switches would decide the result.
unset CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER CCACHE_DISABLE CCACHE_RECACHE
WORK="$(mktemp -d -t axl-cclaunch.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
export CCACHE_DIR="$WORK/cache"
B="$WORK/on"
# The launcher is matched by its RESOLVED PATH, never the bare word: build.ninja
# is full of absolute paths, and any of them may contain "ccache".
CCACHE_BIN="$(command -v ccache)"

# ccstat <key>: one counter from `ccache --print-stats` (tab-separated).
ccstat() { ccache --print-stats | awk -F'\t' -v k="$1" '$1 == k { print $2 }'; }
# Sorted "<sha256>  <relative path>" of every image in a tree.
images() { (cd "$1" && find . -name '*.efi' -type f -print0 | sort -z | xargs -0 sha256sum); }

if ! cmake --preset x64-debug -B "$B" -DAXL_CCACHE=AUTO >"$WORK/cfg.log" 2>&1; then
    test_host_fail "AUTO configures with ccache present"
    tail -5 "$WORK/cfg.log" | sed 's/^/      /'
    test_host_summary "cmake-ccache"; exit
fi
test_host_pass "AUTO configures with ccache present"

if grep -qF "$CCACHE_BIN" "$B/build.ninja"; then
    test_host_pass "AUTO puts ccache in front of the compiles"
else
    test_host_fail "AUTO puts ccache in front of the compiles"
fi

# The C/C++ compiles the tree declares -- what every warm compile must hit.
NCOMP="$(grep -cE '"file": ".*\.(c|cpp)"' "$B/compile_commands.json")"
if grep -q '"command": "[^"]*ccache' "$B/compile_commands.json"; then
    test_host_fail "compile_commands.json names the compiler, not ccache"
else
    test_host_pass "compile_commands.json names the compiler, not ccache ($NCOMP C/C++ entries)"
fi

if ! cmake --build "$B" >"$WORK/cold.log" 2>&1; then
    test_host_fail "the cold build succeeds"
    tail -5 "$WORK/cold.log" | sed 's/^/      /'
    test_host_summary "cmake-ccache"; exit
fi
images "$B" >"$WORK/cold.sums"
NIMG="$(wc -l <"$WORK/cold.sums")"
test_host_pass "the cold build succeeds ($NIMG images)"

ninja -C "$B" -t clean >/dev/null
ccache -z >/dev/null
cmake --build "$B" >"$WORK/warm.log" 2>&1 || test_host_fail "the warm build succeeds"
images "$B" >"$WORK/warm.sums"

HITS=$(( $(ccstat direct_cache_hit) + $(ccstat preprocessed_cache_hit) ))
MISS="$(ccstat cache_miss)"
if [[ "$NCOMP" -gt 0 && "$HITS" -ge "$NCOMP" && "$MISS" -eq 0 ]]; then
    test_host_pass "a rebuild after a clean is served from the cache ($HITS hits, 0 misses, $NCOMP compiles)"
else
    test_host_fail "a rebuild after a clean is served from the cache ($HITS hits, $MISS misses, $NCOMP compiles)"
fi

if [[ "$NIMG" -gt 0 ]] && cmp -s "$WORK/cold.sums" "$WORK/warm.sums"; then
    test_host_pass "the cached build's $NIMG images are byte-identical to the cold build's"
else
    test_host_fail "the cached build's images are byte-identical to the cold build's"
    diff "$WORK/cold.sums" "$WORK/warm.sums" | head -5 | sed 's/^/      /'
fi

# The control: the same check over a tree with the launcher OFF must see none.
if cmake --preset x64-debug -B "$WORK/off" -DAXL_CCACHE=OFF >"$WORK/off.log" 2>&1 \
   && ! grep -qF "$CCACHE_BIN" "$WORK/off/build.ninja"; then
    test_host_pass "OFF puts no launcher in build.ninja"
else
    test_host_fail "OFF puts no launcher in build.ninja"
fi

# CMake (3.17+) seeds CMAKE_<LANG>_COMPILER_LAUNCHER from the environment
# variable of the same name, so OFF must clear it rather than merely not set it.
if CMAKE_C_COMPILER_LAUNCHER="$CCACHE_BIN" CMAKE_CXX_COMPILER_LAUNCHER="$CCACHE_BIN" \
        cmake --preset x64-debug -B "$WORK/off-env" -DAXL_CCACHE=OFF >"$WORK/offenv.log" 2>&1 \
   && ! grep -qF "$CCACHE_BIN" "$WORK/off-env/build.ninja"; then
    test_host_pass "OFF wins over a launcher exported in the environment"
else
    test_host_fail "OFF wins over a launcher exported in the environment"
fi

# A launcher the user chose is theirs: AUTO leaves it in place, and ON, which
# cannot have both, refuses to configure.
# A path no build line names otherwise; configure does not run it.
OTHER=/nonexistent/axl-test-launcher/sccache
if cmake --preset x64-debug -B "$WORK/user" -DAXL_CCACHE=AUTO \
        -DCMAKE_C_COMPILER_LAUNCHER="$OTHER" -DCMAKE_CXX_COMPILER_LAUNCHER="$OTHER" \
        >"$WORK/user.log" 2>&1 \
   && grep -qF "$OTHER" "$WORK/user/build.ninja" \
   && ! grep -qF "$CCACHE_BIN" "$WORK/user/build.ninja"; then
    test_host_pass "AUTO keeps a launcher the user set"
else
    test_host_fail "AUTO keeps a launcher the user set"
fi
if cmake --preset x64-debug -B "$WORK/user-on" -DAXL_CCACHE=ON \
        -DCMAKE_C_COMPILER_LAUNCHER="$OTHER" >"$WORK/useron.log" 2>&1; then
    test_host_fail "ON with another launcher set fails the configure"
elif grep -q 'CMAKE_C_COMPILER_LAUNCHER' "$WORK/useron.log"; then
    test_host_pass "ON with another launcher set fails the configure, naming it"
else
    test_host_fail "ON with another launcher set fails the configure, naming it"
    tail -5 "$WORK/useron.log" | sed 's/^/      /'
fi

# A ccache the user named as the launcher IS the ccache, even where the
# search would not find one: ON accepts it.
if cmake --preset x64-debug -B "$WORK/user-cc" -DAXL_CCACHE=ON \
        -DAXL_CCACHE_PROGRAM=/nonexistent/ccache \
        -DCMAKE_C_COMPILER_LAUNCHER="$CCACHE_BIN" -DCMAKE_CXX_COMPILER_LAUNCHER="$CCACHE_BIN" \
        >"$WORK/usercc.log" 2>&1 \
   && grep -qF "$CCACHE_BIN" "$WORK/user-cc/build.ninja"; then
    test_host_pass "ON accepts a ccache named as the launcher"
else
    test_host_fail "ON accepts a ccache named as the launcher"
fi

# A path find_program cached that has since gone (ccache moved or upgraded to
# another prefix) is searched for again, not reported as "none found". The
# internal record of what the search found stands in for the earlier configure.
if cmake --preset x64-debug -B "$WORK/stale" -DAXL_CCACHE=AUTO \
        -DAXL_CCACHE_PROGRAM=/nonexistent/ccache -D_AXL_CCACHE_FOUND=/nonexistent/ccache \
        >"$WORK/stale.log" 2>&1 \
   && grep -qF "$CCACHE_BIN" "$WORK/stale/build.ninja"; then
    test_host_pass "a cached ccache path that has gone is searched for again"
else
    test_host_fail "a cached ccache path that has gone is searched for again"
fi

# Asked for and not found. A program path that does not exist stands in for a
# host without ccache: the tool itself lives beside python3 and the compilers
# in /usr/bin, so hiding the directory would hide them too.
if cmake --preset x64-debug -B "$WORK/on-missing" -DAXL_CCACHE=ON \
        -DAXL_CCACHE_PROGRAM=/nonexistent/ccache >"$WORK/miss.log" 2>&1; then
    test_host_fail "ON with no ccache fails the configure"
elif grep -q 'AXL_CCACHE=ON' "$WORK/miss.log"; then
    test_host_pass "ON with no ccache fails the configure, naming AXL_CCACHE=ON"
else
    test_host_fail "ON with no ccache fails the configure, naming AXL_CCACHE=ON"
    tail -5 "$WORK/miss.log" | sed 's/^/      /'
fi

if cmake --preset x64-debug -B "$WORK/auto-missing" -DAXL_CCACHE=AUTO \
        -DAXL_CCACHE_PROGRAM=/nonexistent/ccache >"$WORK/amiss.log" 2>&1 \
   && ! grep -qF "$CCACHE_BIN" "$WORK/auto-missing/build.ninja"; then
    test_host_pass "AUTO with no ccache configures without it"
else
    test_host_fail "AUTO with no ccache configures without it"
fi

echo ""
test_host_summary "cmake-ccache"
