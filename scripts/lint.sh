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

CT_VERSION=18                       # keep in sync with ci.yml's clang-tidy-NN
CT="clang-tidy-$CT_VERSION"

if ! command -v "$CT" >/dev/null 2>&1; then
    CT="clang-tidy"
    command -v "$CT" >/dev/null 2>&1 || { echo "ERROR: no clang-tidy found" >&2; exit 1; }
    have="$("$CT" --version | grep -oE 'version [0-9]+' | head -1)"
    cat >&2 <<EOF
WARNING: clang-tidy-$CT_VERSION (CI's pinned version) not found — using
         '$(command -v "$CT")' ($have). A different version can disagree with
         CI; a clean run here does NOT guarantee a clean CI run. To match CI:
           sudo apt-get install clang-tidy-$CT_VERSION
EOF
fi

command -v bear >/dev/null 2>&1 || { echo "ERROR: 'bear' not found (apt install bear)" >&2; exit 1; }

echo "==> generating compile_commands.json (bear -- make tests tools)"
rm -f compile_commands.json
bear -- make tests tools >/dev/null

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
