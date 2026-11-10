# cmake-stub.sh -- stub cmake/ctest that the pinned-CMake helper accepts.
# Sourced by tests that run a COPY of an entry point (axl-build.sh,
# release-suite.sh, ...) in a stub repository with stub tools on PATH.
#
# Every such entry point now calls axl_use_pinned_cmake
# (scripts/axl-cmake-pin.sh), which keeps the cmake first on PATH only when
# it AND the ctest beside it report the pinned version -- otherwise it puts
# the real pinned one first, and the test would silently run REAL cmake
# instead of its stub. So a stub answers --version first, and the stub repo
# carries the helper and a ci-cmake.sh that reports the pinned version.
#
#   cmake_stub_version_line
#       The line to put right after a stub's #! line: `--version` answers
#       "<its own name> version <pinned>" and exits, logging nothing.
#   cmake_stub_repo REPO BINDIR
#       Copy the helper into REPO/scripts beside a ci-cmake.sh that refuses
#       every install (exit 97: nothing is ever downloaded), and give BINDIR a
#       version-only ctest (or cmake) where the test stubs just one of them;
#       any other call of that one fails loudly (exit 97), never silently.

# shellcheck shell=bash

_CMAKE_STUB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CMAKE_STUB_VERSION="$("$_CMAKE_STUB_ROOT/scripts/ci-cmake.sh" --version)"
[[ -n "$CMAKE_STUB_VERSION" ]] || { echo "cmake-stub.sh: cannot read the pinned version from ci-cmake.sh" >&2; exit 1; }

cmake_stub_version_line() {
    printf '[[ "${1:-}" == --version ]] && { echo "${0##*/} version %s"; exit 0; }\n' \
        "$CMAKE_STUB_VERSION"
}

cmake_stub_repo() {
    local t
    mkdir -p "$1/scripts" "$2"
    cp "$_CMAKE_STUB_ROOT/scripts/axl-cmake-pin.sh" "$1/scripts/"
    # A ci-cmake.sh that knows the version and REFUSES to install: should a
    # stub ever stop passing as pinned, the helper must fail loudly here, not
    # download the real CMake into the user's cache and run it on the stub.
    printf '#!/bin/bash
[[ "${1:-}" == --version ]] && { echo %s; exit 0; }
echo "cmake-stub: ci-cmake.sh $* refused: the stub cmake did not pass as pinned" >&2
exit 97
' \
        "$CMAKE_STUB_VERSION" > "$1/scripts/ci-cmake.sh"
    chmod +x "$1/scripts/ci-cmake.sh"
    for t in cmake ctest; do
        [[ -e "$2/$t" ]] && continue
        { echo '#!/bin/bash'; cmake_stub_version_line
          echo 'echo "cmake-stub: unexpected call: $0 $*" >&2; exit 97'; } > "$2/$t"
        chmod +x "$2/$t"
    done
}
