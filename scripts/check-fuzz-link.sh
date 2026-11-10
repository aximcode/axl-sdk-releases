#!/bin/bash
# check-fuzz-link.sh -- the host fuzz harnesses (test/fuzz) still LINK, and
# every committed seed corpus still replays clean under ASan.
#
# The harnesses are deliberately outside the UEFI build: libFuzzer and ASan
# need a HOST clang. Nothing else referenced test/fuzz, which is how json_fuzz
# sat un-linkable for months while the parser it targets was rewritten under
# it -- ASan coverage of the JSON read path was silently ZERO, and two OOM
# defects there had to be caught by review instead. So: build the `fuzz`
# preset and run its seed replays (fuzz.seeds.<h>, -runs=0, ~0.1 s each).
#
# `--fresh`, always: configuring an existing build/fuzz with a DIFFERENT
# compiler makes CMake wipe its cache and re-run, and that re-run lost
# AXL_FUZZ -- the root fell through to the cross toolchain and generated the
# whole UEFI tree into build/fuzz (1,298 targets, a false FAIL).
#
# The replay count is held to the committed corpora, so a harness dropped
# from test/fuzz/CMakeLists.txt is a failure, not "clean -- 3".
#
# Skips when clang is absent so a gcc-only checkout can run the gate set --
# but under CI (or AXL_FUZZ_REQUIRED=1) a missing clang is a FAILURE: a gate
# that silently cannot see reports green forever.
#
# Usage: check-fuzz-link.sh        (FUZZ_CC names the clang; default clang)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1
FUZZ_CC="${FUZZ_CC:-clang}"
if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
    if [[ -n "${AXL_FUZZ_REQUIRED:-}${CI:-}" ]]; then
        echo "check-fuzz-link: FAIL — $FUZZ_CC not found, and CI / AXL_FUZZ_REQUIRED"
        echo "  says this gate must actually run. Install clang or set FUZZ_CC."
        exit 1
    fi
    echo "check-fuzz-link: SKIP — $FUZZ_CC not found (libFuzzer is clang-only)."
    exit 0
fi
# The pinned CMake (scripts/axl-cmake-pin.sh). The configure below is --fresh
# every time, so no tree another cmake configured survives it.
# shellcheck source=axl-cmake-pin.sh
source "$REPO/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
for t in cmake ninja ctest; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "check-fuzz-link: FAIL — $t is required (the \`fuzz\` preset)."; exit 1; }
done
mkdir -p build
LOG=build/fuzz.log
if ! cmake --fresh --preset fuzz -DCMAKE_C_COMPILER="$FUZZ_CC" > "$LOG" 2>&1 \
   || ! cmake --build --preset fuzz >> "$LOG" 2>&1; then
    echo "check-fuzz-link: FAIL — a harness in test/fuzz no longer builds."
    tail -25 "$LOG"
    exit 1
fi
if ! ctest --preset fuzz >> "$LOG" 2>&1; then
    echo "check-fuzz-link: FAIL — a harness crashed on its own seed corpus."
    grep -E 'Failed|ERROR|SUMMARY' "$LOG" | head -30
    exit 1
fi
n=$(grep -c 'fuzz\.seeds\..*Passed' "$LOG")
want=$(find test/fuzz -maxdepth 1 -type d -name '*_corpus' | wc -l)
if (( n != want )) || (( want == 0 )); then
    echo "check-fuzz-link: FAIL — $n seed replay(s) passed, $want corpora committed."
    exit 1
fi
echo "check-fuzz-link: clean — $n harnesses build; every seed corpus replays."
