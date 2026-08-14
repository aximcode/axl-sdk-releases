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

# Where the CROSS toolchain keeps its libc headers. clang-tidy replays the
# compile database with clang, infers the freestanding target from the recorded
# compiler's NAME (x86_64-elf-gcc), and then has no cross libc to go with it --
# so <string.h> resolves to the HOST's /usr/include. That is glibc, and it is
# clean on this box only because EL/Fedora keep bits/ directly in /usr/include;
# CI's ubuntu:26.04 puts it in the multiarch subdirectory clang adds only for a
# linux-gnu target, and the job died there. Passing the directory explicitly
# makes tidy read the same headers the objects were compiled against.
#
# The Makefile owns the value (asked of $(CC), so a toolchain override moves it
# too) and this reads it back, rather than keeping a second copy -- the same
# arrangement as LINT_GATES above. It fails rather than reporting an empty
# string, so this cannot silently revert to analyzing the host libc.
#
# -nostdlibinc + -idirafter, NOT -isystem. -isystem puts the libc AHEAD of
# clang's own builtin headers, where the build searches the COMPILER's headers
# first and the libc after -- measured: it flips which limits.h / stdint.h /
# stdatomic.h / tgmath.h is read, and defines PATH_MAX where the gcc build
# leaves it undefined. -nostdlibinc drops /usr/include entirely, so a missing
# cross header is now a loud error instead of a silent fall-back to glibc.
# (What remains is clang's builtin headers standing in for gcc's, which is
# inherent to linting a gcc build with clang and predates this.)
CT_LIBC=(--extra-arg=-nostdlibinc
         "--extra-arg=-idirafter$(make -s print-cc-libc-include)")

echo "==> clang-tidy ($CT) over src/ (-n1, parallel)"
# Mirror ci.yml exactly: per-file (-n1) so path-sensitive analyzer checks are
# deterministic; exclude the backend + the mbedtls platform shim (compile DB
# has no AXL_TLS=1). .clang-tidy's WarningsAsErrors makes any finding non-zero.
find src -name '*.c' \
    -not -path '*/backend/*' \
    -not -name 'axl-mbedtls-platform.c' \
    -print0 \
  | xargs -0 -n1 -P"$(nproc)" "$CT" -p . -quiet "${CT_LIBC[@]}"

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
  | xargs -0 -n1 -P"$(nproc)" "$CT" -p . -quiet "${CT_LIBC[@]}" --checks='-clang-analyzer-*'

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
#
# The CROSS libstdc++, matching $CT_LIBC's cross libc one library down. This
# pass used to read the HOST libstdc++ and said so at this comment: the compile
# database named host `g++`, clang inferred a linux-gnu target from that name
# and found /usr/include/c++. T2 moved x64 C++ to the bare-metal cross, the
# accident stopped working, and every C++ TU failed with `'string' file not
# found` -- the better failure, but still one. So the deferred half landed here.
#
# THREE flags, and each is load-bearing:
#   -nostdinc++     drops clang's own C++ search (libc++ on some boxes), so a
#                   missing cross header is a loud error rather than a silent
#                   fall-back to a different standard library.
#   -isystem <dirs> the cross toolchain's three C++ directories. -isystem, NOT
#                   -idirafter: unlike the libc case these MUST come first --
#                   there is nothing of clang's they could displace, and
#                   -idirafter would leave them behind the (now empty) default
#                   search and find nothing.
#   $CT_LIBC        <string> includes <cstring> includes <string.h>, so the C++
#                   pass needs the cross LIBC too. It was omitted here on the
#                   grounds that it "mixes two libcs", which was true only
#                   while the C++ headers came from the host.
#
# Captured into a variable FIRST, then iterated. `for _d in $(make ...)` would
# swallow the target's exit 1 -- a command substitution in a `for` word list is
# not a command, so `set -e` never sees it -- and the Makefile guard's
# "Refusing to report nothing" would never reach the operator. The pass would
# instead fail every C++ TU on `'string' file not found`, which names the
# symptom three layers below the cause. $CT_LIBC's plain assignment above is
# correct for the same reason, and this used to differ from it.
CXX_INCS=$(make -s print-cxx-include-dirs)
CT_CXX=(--extra-arg=-nostdinc++)
for _d in $CXX_INCS; do
    CT_CXX+=("--extra-arg=-isystem$_d")
done
printf '%s\n' "$CXX_TUS" \
  | xargs -n1 -P"$(nproc)" "$CT" -p . -quiet "${CT_LIBC[@]}" "${CT_CXX[@]}"

# One summary line, so verify.sh's table can quote it without counting steps
# (a hardcoded step count in the caller goes stale the moment a pass is added).
echo "lint: clean (clang -Wall -Wextra; clang-tidy $CT over src/, test/unit/, tools/ and $CXX_COUNT C++ TU(s))"
