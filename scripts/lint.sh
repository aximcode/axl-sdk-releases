#!/bin/bash
# lint.sh — run clang-tidy exactly as CI does (the docs/RELEASING.md gate).
#
# CI pins a specific clang-tidy version (see CT_VERSION below, kept in sync
# with .github/workflows/ci.yml). A *newer* local clang-tidy can silently pass
# code an older CI clang-tidy flags — v1.0.0 shipped with a red CI for exactly
# that reason. Running this with the pinned version makes "clean locally" mean
# "clean in CI".
#
# Usage: scripts/lint.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

CT_VERSION=21                       # keep in sync with ci.yml's clang-tidy-NN
CT="clang-tidy-$CT_VERSION"

if ! command -v "$CT" >/dev/null 2>&1; then
    # No versioned binary. Fall back to the unversioned 'clang-tidy' — but only
    # quietly when it IS the pinned major (common on distros that ship a single
    # current clang-tidy, e.g. EL/Fedora, where 'clang-tidy' already == 21).
    # Then "clean here" still means "clean in CI"; only a real version mismatch
    # warns.
    command -v clang-tidy >/dev/null 2>&1 || { echo "ERROR: no clang-tidy found" >&2; exit 1; }
    CT="clang-tidy"
    have="$("$CT" --version | sed -nE 's/.*version ([0-9]+).*/\1/p' | head -1)"
    if [ "$have" = "$CT_VERSION" ]; then
        echo "note: using '$(command -v "$CT")' (LLVM $have == CI's pinned clang-tidy-$CT_VERSION)"
    else
        cat >&2 <<EOF
WARNING: clang-tidy-$CT_VERSION (CI's pinned version) not found — using
         '$(command -v "$CT")' (LLVM $have). A different version can disagree
         with CI; a clean run here does NOT guarantee a clean CI run. To match:
           - Ubuntu 26.04+:  sudo apt-get install clang-tidy-$CT_VERSION
           - other distros:  run CI's lint container (ubuntu:26.04) via
                             podman/docker, same image ci.yml's lint job uses.
EOF
    fi
fi

command -v bear >/dev/null 2>&1 || { echo "ERROR: 'bear' not found (apt install bear)" >&2; exit 1; }

echo "==> generating compile_commands.json (fresh build into a throwaway prefix)"
# Build into a SEPARATE prefix so every translation unit compiles fresh and
# bear captures it. An incremental `make` skips up-to-date objects, and bear
# then omits them from compile_commands.json — so clang-tidy SILENTLY SKIPS
# those files ("Compile command not found") and a stale-tree lint can miss
# exactly the code you just changed. A throwaway prefix forces the full build
# (matching CI's fresh checkout) without clobbering the dev .o cache the
# integration tests reuse.
LINT_PREFIX="out/native-x64-lint"
rm -rf "$LINT_PREFIX" compile_commands.json
bear -- make tests tools PREFIX="$LINT_PREFIX" >/dev/null

echo "==> clang-tidy ($CT) over src/ (-n1, parallel)"
# Mirror ci.yml exactly: per-file (-n1) so path-sensitive analyzer checks are
# deterministic; exclude the backend + the mbedtls platform shim (compile DB
# has no AXL_TLS=1). .clang-tidy's WarningsAsErrors makes any finding non-zero.
find src -name '*.c' \
    -not -path '*/backend/*' \
    -not -name 'axl-mbedtls-platform.c' \
    -print0 \
  | xargs -0 -n1 -P"$(nproc)" "$CT" -p . -quiet

echo "clang-tidy ($CT) clean."
