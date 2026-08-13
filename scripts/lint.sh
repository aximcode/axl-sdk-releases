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
# AXL_CPP=1 so the C++ sources land in the DB at all. Without it the tree
# builds zero .cpp, bear records zero C++ entries, and any clang-tidy pass
# over C++ below would silently inspect nothing -- clean forever.
bear -- make tests tools AXL_CPP=1 PREFIX="$LINT_PREFIX" >/dev/null

echo "==> clang -Wall -Wextra over every TU (compiler diagnostics, not tidy)"
# The tree builds with gcc; clang-tidy reports .clang-tidy's checks, NOT the
# clang frontend's own -W diagnostics. Without this pass, a clang-only compiler
# warning has nowhere to surface (a -Wformat "zero field width" reached a commit
# that way). Uses this same compile DB, so no extra build. See the script for
# the scope measurement and the single deliberate suppression.
python3 scripts/check-clang-warnings.py

echo "==> clang-tidy ($CT) over src/ (-n1, parallel)"
# Mirror ci.yml exactly: per-file (-n1) so path-sensitive analyzer checks are
# deterministic; exclude the backend + the mbedtls platform shim (compile DB
# has no AXL_TLS=1). .clang-tidy's WarningsAsErrors makes any finding non-zero.
find src -name '*.c' \
    -not -path '*/backend/*' \
    -not -name 'axl-mbedtls-platform.c' \
    -print0 \
  | xargs -0 -n1 -P"$(nproc)" "$CT" -p . -quiet

echo "==> clang-tidy ($CT) over test/unit/ and tools/ (bugprone-* only)"
# Test sources are where "passes for the wrong reason" lives, and nothing linted
# them: bugprone-branch-clone caught a `count > 0 ? 0 : 0` in a committed test
# only because someone ran tidy by hand. tools/ is the other half of the tree
# that ships and was not linted.
#
# The check set is NARROWED to bugprone-* here (`--checks` appends to
# .clang-tidy, so '-clang-analyzer-*' subtracts the analyzer from the shared
# config -- one source of truth for the disabled-check list). Measured, this is
# the honest line for BOTH directories, for the same reason with different
# specifics:
#
#   - test/unit: bugprone-* is 0 findings, the full config ~40, essentially all
#     of them the analyzer objecting to things unit tests do ON PURPOSE --
#     casting 99 to an enum to pin the bad-enum error path, indexing a buffer
#     the test has already asserted non-NULL.
#   - tools/: bugprone-* is 0 findings (it was 3; one was a real if trivial
#     omission and is fixed, two are documented NOLINTs in sed.c where
#     semantically distinct cases share an implementation). The full config is
#     10, and they are analyzer noise of the same character -- four
#     security.PointerSub on ordinary buffer arithmetic, and a core.DivideZero
#     in crashtest.c, which is a tool whose PURPOSE is to divide by zero.
#
# Enabling the analyzer over either would mean a red gate or ~50 suppressions
# that make real findings harder to see.
#
# test/integration and test/fuzz stay out: they are absent from the compile DB
# (`make tests tools` does not build them), so clang-tidy would fall back to
# default flags and emit nonsense "file not found" errors rather than real
# analysis. See docs/RELEASING.md for the scope table.
find test/unit tools -name '*.c' -print0 \
  | xargs -0 -n1 -P"$(nproc)" "$CT" -p . -quiet --checks='-clang-analyzer-*'

echo "==> clang-tidy ($CT) over C++ translation units"
# The C++ layer. Nothing linted C++ before this: the compile database was
# built without AXL_CPP=1, so it contained zero .cpp entries and any pass over
# them inspected nothing. Turning it on immediately found a real conformance
# error that gcc had been tolerating for as long as the file existed --
# `operator new` declared `noexcept` when <new> declares it without one, which
# clang rejects outright.
#
# The COUNT GUARD below is the point: a C++ pass that silently matches no
# files is worse than no pass, because it reports clean forever. If the DB
# stops carrying C++ TUs (an AXL_CPP regression in the bear line above), this
# fails loudly instead.
CXX_TUS=$(python3 - <<'PYEOF'
import json, pathlib
db = json.loads(pathlib.Path("compile_commands.json").read_text())
print("\n".join(e["file"] for e in db if e["file"].endswith((".cpp", ".cc", ".cxx"))))
PYEOF
)
CXX_COUNT=$(printf '%s\n' "$CXX_TUS" | grep -c . || true)
if [[ "$CXX_COUNT" -lt 1 ]]; then
    echo "ERROR: compile_commands.json has no C++ translation units, so the" >&2
    echo "       C++ lint pass would inspect nothing and report clean forever." >&2
    echo "       Check the AXL_CPP=1 on the bear line above." >&2
    exit 1
fi
printf '%s\n' "$CXX_TUS" \
  | xargs -n1 -P"$(nproc)" "$CT" -p . -quiet

# One summary line, so verify.sh's table can quote it without counting steps
# (a hardcoded step count in the caller goes stale the moment a pass is added).
echo "lint: clean (clang -Wall -Wextra; clang-tidy $CT over src/, test/unit/, tools/ and $CXX_COUNT C++ TU(s))"
