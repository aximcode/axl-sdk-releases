#!/bin/bash
# lint.sh — run the lint gate exactly as CI does: the x64-lint CMake preset.
#
# A thin wrapper (docs/AXL-Build-System-Design.md section 10.1, item 2). The
# engine is cmake/AxlLint.cmake: clang-tidy runs beside every compile of the
# x64-lint build tree (build/x64-lint), so Ninja re-checks only the TUs whose
# source, headers, .clang-tidy or clang-tidy itself changed. The preset's
# CTest test then adds the clang -Wall -Wextra pass. A first run compiles and
# analyses the tree (~60s); after that a run costs what changed (~2s for one
# file, 0.03s for nothing).
#
# Usage: scripts/lint.sh
#
# It takes NO file arguments. Scoping existed to avoid a 55s full run, and
# proved less than it seemed: a finding belongs to a translation unit, so a
# file list missed every other TU including a changed header. The incremental
# build already re-checks exactly what a change can reach, headers included.
#
# The pinned clang-tidy (CI's) is checked by cmake/AxlLint.cmake, which warns
# when the one found is a different LLVM major.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [[ $# -gt 0 ]]; then
    case "$1" in
        -h|--help) awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next }
                        NR>1 { exit }' "$0"; exit 0 ;;
    esac
    echo "lint.sh: takes no arguments (got: $*). The lint preset is incremental:" >&2
    echo "         a plain run re-checks exactly the TUs your change can reach," >&2
    echo "         which a file list never did. Run scripts/lint.sh alone." >&2
    exit 2
fi

# The pinned CMake (scripts/axl-cmake-pin.sh); a tree another cmake
# configured is redone from scratch, since ninja would keep running that one.
# shellcheck source=axl-cmake-pin.sh
source "$REPO_ROOT/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
fresh=()
if axl_cmake_tree_foreign build/x64-lint; then
    echo "lint.sh: $(axl_cmake_foreign_reason build/x64-lint)" >&2
    fresh=(--fresh)
fi
cmake "${fresh[@]}" --preset x64-lint >/dev/null
cmake --build --preset x64-lint
ctest --preset x64-lint -L lint --no-tests=error     # in parallel: the preset says so
echo "lint: clean (clang-tidy over the x64-lint preset: src/, test/unit/, tools/, test/integration/, the host tools and the C++ TUs; clang -Wall -Wextra)"
