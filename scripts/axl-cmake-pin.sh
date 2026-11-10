# axl-cmake-pin.sh -- run THIS tree's build and tests with the CMake CI and
# the release use (docs/AXL-Build-System-Design.md §10.13). Sourced, never run.
#
#   source "$REPO/scripts/axl-cmake-pin.sh"
#   axl_use_pinned_cmake || exit 1
#
# WHY. CI and the release install one pinned CMake (scripts/ci-cmake.sh,
# §10.12); local runs took whatever `cmake` the box had. A test passed on
# every local run and failed only on CI, because CMake 4.2 writes
# compile_commands.json's `output` absolute where 3.31 writes it relative.
# Every entry point that runs cmake or ctest calls axl_use_pinned_cmake once,
# early; what it starts inherits the PATH (test-pinned-cmake.sh holds every
# such script to that).
#
# axl_use_pinned_cmake
#     Make the pinned cmake and ctest the first on PATH (exported).
#     - Already there (CI, after ci-cmake.sh; or a nested entry point): does
#       nothing, prints nothing.
#     - Otherwise from a per-user install shared by every checkout and
#       worktree, ${XDG_CACHE_HOME:-$HOME/.cache}/axl-sdk/cmake/<version>,
#       installed there on first use through `ci-cmake.sh install`
#       (Kitware's tarball, checksum-verified), with ONE line saying so.
#     - Returns 1, naming the command to retry, when that install fails. It
#       NEVER falls back to the system cmake: a silent fallback is exactly
#       the drift this exists to remove.
#     Scope: this repo's dev and CI entry points only. The SDK consumers
#     install (axl-cc, the CMake package, sdk/examples) builds with the
#     consumer's own CMake, at the package's own minimum -- which
#     axl_floor_cmake_bin below exists to test.
#
# axl_floor_cmake_bin
#     Set AXL_FLOOR_CMAKE_BIN to the bin/ of the CONSUMER FLOOR CMake
#     (`ci-cmake.sh --floor-version`, the last release of the minor that
#     sdk/examples and the docs declare), installed the same way and into the
#     same per-user cache as the pinned one, with the same one-line notice on
#     first use. PATH is NOT changed: a test runs it beside the pinned one.
#     Status 0 on success; 1, naming the retry command, when the install
#     fails; 2, relaying ci-cmake.sh's reason, on a machine Kitware built no
#     such release for. Never a fall-back to another cmake.
#
# axl_below_floor_cmake_bin
#     The same for AXL_BELOW_FLOOR_CMAKE_BIN: the release BELOW the floor
#     (`ci-cmake.sh --below-floor-version`) that the generated package must
#     refuse, so a test can watch it do so. x86_64 only (status 2 elsewhere).
#
# axl_cmake_tree_foreign TREE
#     Status 0 when TREE's CMakeCache.txt names a cmake that is neither the
#     one now on PATH nor another binary reporting the pinned version, with
#     that cmake in AXL_CMAKE_TREE_WAS; 1 otherwise (no cache, the same
#     binary, or a pinned-version one). PATH alone never moves an existing
#     tree: ninja's regeneration edge runs the cmake the cache names. So a
#     caller that configures reconfigures a foreign tree `cmake --fresh` --
#     after deleting its build.ninja and CTest registration, so a --fresh
#     that FAILS leaves a tree that reads as unconfigured, not as current
#     (the failed configure still writes a cache naming the pinned cmake).
#
# axl_cmake_foreign_reason TREE-AS-SHOWN
#     The one line a caller prints before that --fresh, after a successful
#     axl_cmake_tree_foreign.

# shellcheck shell=bash

_AXL_CMAKE_PIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# _axl_reports_pinned DIR VERSION -- DIR's cmake AND ctest report VERSION.
_axl_reports_pinned() {
    [[ -x "$1/cmake" && -x "$1/ctest" ]] \
        && [[ "$("$1/cmake" --version 2>/dev/null | head -1)" == "cmake version $2" ]] \
        && [[ "$("$1/ctest" --version 2>/dev/null | head -1)" == "ctest version $2" ]]
}

# _axl_cmake_cache_install WHAT VERSION SIZE BASE BIN [--KIND] -- make BIN hold a
# cmake and ctest reporting VERSION, installing it into BASE through
# `ci-cmake.sh install [--KIND] BASE` under a per-user lock when it does not.
# On failure print what failed (two lines) and return 1; the caller says what
# it refuses and how to retry.
_axl_cmake_cache_install() {
    local what="$1" v="$2" size="$3" base="$4" bin="$5" lockfd="" err rc=0
    shift 5
    _axl_reports_pinned "$bin" "$v" && return 0
    mkdir -p "$base" || { echo "axl: cannot create $base" >&2; return 1; }
    # One installer at a time per user: two first uses at once (verify.sh's
    # jobs, two worktrees) would each replace the tree the other is running.
    if command -v flock > /dev/null 2>&1; then
        if ! { exec {lockfd}> "$base.lock"; } 2>/dev/null || ! flock "$lockfd"; then
            echo "axl: cannot lock $base.lock to install the $what CMake $v" >&2
            return 1
        fi
    fi
    if ! _axl_reports_pinned "$bin" "$v"; then   # another one may have finished it
        echo "axl: installing the $what CMake $v (first use) into $base -- $size, once per user" >&2
        err="$("$_AXL_CMAKE_PIN_DIR/ci-cmake.sh" install "$@" "$base" 2>&1 > /dev/null)" || rc=$?
        if (( rc == 0 )) && ! _axl_reports_pinned "$bin" "$v"; then
            err="ci-cmake.sh: installed, but $bin does not hold a cmake and ctest reporting $v"
            rc=1
        fi
    fi
    [[ -n "$lockfd" ]] && exec {lockfd}>&-
    if (( rc != 0 )); then
        {
            echo "axl: could not install the $what CMake $v into $base:"
            tail -1 <<< "$err"
        } >&2
        return 1
    fi
}

# _axl_cmake_cache_root WHAT VERSION -- print the per-user cache root, or
# return 1 saying why. Not ${HOME}/.cache under the callers' `set -u`: an
# unset HOME would abort the caller with "unbound variable" instead.
_axl_cmake_cache_root() {
    if [[ -z "${XDG_CACHE_HOME:-}" && -z "${HOME:-}" ]]; then
        echo "axl: neither XDG_CACHE_HOME nor HOME is set -- nowhere to keep the $1 CMake $2" >&2
        return 1
    fi
    echo "${XDG_CACHE_HOME:-$HOME/.cache}/axl-sdk/cmake"
}

axl_use_pinned_cmake() {
    local v cur ctst root base bin
    v="$("$_AXL_CMAKE_PIN_DIR/ci-cmake.sh" --version)" || {
        echo "axl: cannot read the pinned CMake version from $_AXL_CMAKE_PIN_DIR/ci-cmake.sh" >&2
        return 1
    }
    cur="$(command -v cmake 2>/dev/null)"
    ctst="$(command -v ctest 2>/dev/null)"
    if [[ -n "$cur" && -n "$ctst" ]] && _axl_reports_pinned "$(dirname "$cur")" "$v" \
            && [[ "$(dirname "$ctst")" == "$(dirname "$cur")" ]]; then
        return 0
    fi
    root="$(_axl_cmake_cache_root pinned "$v")" || return 1
    base="$root/$v"
    bin="$base/cmake-$v-linux-$(uname -m)/bin"
    if ! _axl_cmake_cache_install pinned "$v" "~50 MB" "$base" "$bin"; then
        {
            if [[ -n "$cur" ]]; then
                echo "axl: refusing to fall back to $cur ($("$cur" --version 2>/dev/null | head -1)): CI and the release build with $v."
            else
                echo "axl: refusing to go on without it: CI and the release build with $v."
            fi
            echo "axl: retry: $_AXL_CMAKE_PIN_DIR/ci-cmake.sh install $base"
        } >&2
        return 1
    fi
    export PATH="$bin:$PATH"
}

# _axl_extra_cmake_bin KIND VAR SIZE PURPOSE -- the body of
# axl_floor_cmake_bin and axl_below_floor_cmake_bin: install ci-cmake.sh's
# KIND (floor, below-floor) into the per-user cache and leave its bin/ in VAR.
# The machine check and the tarball's name are ci-cmake.sh's (`name`), which
# knows which releases Kitware built for what. (axl_use_pinned_cmake spells
# the pin's name itself: it is `linux` for every release from 3.20, and its
# fast path must not cost a subprocess.)
_axl_extra_cmake_bin() {
    local kind="$1" var="$2" size="$3" purpose="$4" v name root base rc=0
    printf -v "$var" '%s' ""
    v="$("$_AXL_CMAKE_PIN_DIR/ci-cmake.sh" "--$kind-version")" || {
        echo "axl: cannot read the $kind CMake version from $_AXL_CMAKE_PIN_DIR/ci-cmake.sh" >&2
        return 1
    }
    # Only ci-cmake.sh's 3 is "no build for this machine" (our 2): its own
    # 2 is a usage error -- a stale or broken installer -- which must FAIL a
    # test rather than let it skip.
    name="$("$_AXL_CMAKE_PIN_DIR/ci-cmake.sh" name "--$kind" 2>&1)" || rc=$?
    if (( rc != 0 )); then
        echo "$name" >&2
        (( rc == 3 )) && return 2
        return 1
    fi
    root="$(_axl_cmake_cache_root "$kind" "$v")" || return 1
    base="$root/$v"
    if ! _axl_cmake_cache_install "$kind" "$v" "$size" "$base" "$base/$name/bin" "--$kind"; then
        {
            echo "axl: refusing to go on without it: $purpose is tested with $v, not another cmake."
            echo "axl: retry: $_AXL_CMAKE_PIN_DIR/ci-cmake.sh install --$kind $base"
        } >&2
        return 1
    fi
    printf -v "$var" '%s' "$base/$name/bin"
}

axl_floor_cmake_bin() {
    _axl_extra_cmake_bin floor AXL_FLOOR_CMAKE_BIN "~45 MB" "the consumer floor"
}

axl_below_floor_cmake_bin() {
    _axl_extra_cmake_bin below-floor AXL_BELOW_FLOOR_CMAKE_BIN "~40 MB" \
        "the package's refusal of a CMake below the floor"
}

axl_cmake_tree_foreign() {
    local cache="$1/CMakeCache.txt" was now v
    AXL_CMAKE_TREE_WAS=""
    [[ -f "$cache" ]] || return 1
    was="$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' "$cache" | head -1)"
    [[ -n "$was" ]] || return 1
    now="$(command -v cmake 2>/dev/null)" || return 1
    AXL_CMAKE_TREE_WAS="$was"
    # The binary on PATH, through any symlink: the same one, not foreign.
    [[ "$(readlink -f "$was" 2>/dev/null || echo "$was")" == "$(readlink -f "$now")" ]] && return 1
    # Another path that still runs the pinned VERSION is not foreign either:
    # `command -v cmake` can be a wrapper (pip's, a snap's) while the cache
    # records the real binary behind it, and reconfiguring every tree on
    # every build for that would be all cost. One that is gone (CI's
    # $RUNNER_TEMP) or reports another version is foreign: ninja's
    # regeneration edge runs exactly it.
    v="$("$_AXL_CMAKE_PIN_DIR/ci-cmake.sh" --version 2>/dev/null)" || return 0
    [[ "$("$was" --version 2>/dev/null | head -1)" == "cmake version $v" ]] && return 1
    return 0
}

axl_cmake_foreign_reason() {
    echo "$1 was configured by $AXL_CMAKE_TREE_WAS, not the pinned $(command -v cmake) -- reconfiguring it --fresh"
}
