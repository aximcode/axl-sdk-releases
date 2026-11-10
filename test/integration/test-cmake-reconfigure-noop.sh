#!/bin/bash
# test-meta: arch=none needs=cmake,ninja est=15 local-only=0
# test-cmake-reconfigure-noop.sh -- re-running CMake over an up-to-date tree
# leaves every file a build step reads exactly as it was, so the next build
# has nothing to do.
#
# WHY. Ninja decides "out of date" by mtime. A configure step that rewrites
# one of the build's inputs unconditionally -- `file(WRITE)` rather than
# `file(CONFIGURE)` / `configure_file()`, which write only when the content
# changed -- makes every image downstream of it stale whenever CMake runs
# again, with nothing having changed. CMake runs again more often than it
# looks: ninja re-runs it itself when a CONFIGURE_DEPENDS glob or a listed
# file changes, and a test that configures the shared tree runs it while
# other tests are building there.
#
# That is what failed CI (2026-11-09): src/pack wrote its empty payload
# with file(WRITE); test-ctest-integration.sh's `cmake --preset x64-debug`
# rewrote it a moment after the suite's prep had built everything; the
# empty pack stub went stale, and with it every wrapped tool (62 edges of
# `all`). Three tests then rebuilt the same wraps in one tree at once, and
# test-stderr-row-qemu.sh's ninja stopped on "subcommand failed".
#
# HOW. A PRIVATE tree (never the shared build/x64-debug other tests build
# in): configure it, list every input of `all` that the configure itself
# produced (the files that exist before anything is built), note their
# mtimes, configure again, compare. No build is needed -- the mtime IS what
# ninja compares -- so this costs two configures (~3 s each).
#
# CONTROLS: the list is not empty (a list that matched nothing would compare
# clean), and the comparison reports a file that DID change (one input is
# touched after the second snapshot and must be the one name reported).
#
# Host-only. Usage: ./test/integration/test-cmake-reconfigure-noop.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
# cmake --preset reads CMakePresets.json from the current directory.
cd "$PROJECT_DIR" || exit 1
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

WORK="$(mktemp -d -t axl-cmkreconf.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT
TREE="$WORK/tree"

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
finish() {
    echo ""
    echo "cmake-reconfigure-noop: $PASS passed, $FAIL failed"
    [[ $FAIL -eq 0 ]]
    exit $?
}
configure() {   # configure LOG -- the preset the suite builds, into TREE
    if ! cmake --preset x64-debug -B "$TREE" > "$1" 2>&1; then
        tail -20 "$1"
        check "the x64-debug preset configures a private tree" "failed" "configured"
        finish
    fi
}
snapshot() {    # snapshot FILE... -> "MTIME_NS PATH" lines
    stat -c '%.9Y %n' "$@"
}

echo "=== a second configure rewrites no build input ==="
configure "$WORK/cfg1.log"

# Every input of `all`, as ninja names it (relative to the tree, or absolute);
# the ones that exist now, before any build, came from the configure.
if ! ninja -C "$TREE" -t inputs all > "$WORK/inputs.txt" 2> "$WORK/inputs.err"; then
    cat "$WORK/inputs.err"
    check "ninja -t inputs answers for the private tree" "failed" "answered"
    finish
fi
mapfile -t GEN < <(
    while IFS= read -r p; do
        [[ "$p" == /* ]] || p="$TREE/$p"
        [[ "$p" == "$TREE"/* && -f "$p" ]] && printf '%s\n' "$p"
    done < "$WORK/inputs.txt" | sort -u)
check "the configure produced inputs of the build to watch (control)" \
      "$(( ${#GEN[@]} > 0 ))" "1"
(( ${#GEN[@]} > 0 )) || finish
# ...and among them the one whose rewrite failed CI: a list that lost it
# (the pack stub leaving `all`, its OBJECT_DEPENDS dropped) would compare
# clean while blind to exactly that defect.
check "the pack stub's empty payload is among them (control)" \
      "$(printf '%s\n' "${GEN[@]}" | grep -cx "$TREE/src/pack/empty.pack")" "1"

snapshot "${GEN[@]}" > "$WORK/before.txt"
# A filesystem with coarse timestamps would hide a rewrite inside the same
# tick; a second is more than any of them.
sleep 1
configure "$WORK/cfg2.log"
snapshot "${GEN[@]}" > "$WORK/after.txt"
changed() {     # changed BEFORE AFTER -> the paths whose mtime moved
    diff "$1" "$2" | sed -n 's/^> [0-9.]* //p' | sed "s|^$TREE/||"
}
check "no input of the build was rewritten by the second configure" \
      "$(changed "$WORK/before.txt" "$WORK/after.txt" | paste -sd' ' -)" ""

# The comparison can see a rewrite at all: touch one input and it must be
# exactly the one reported.
touch "${GEN[0]}"
snapshot "${GEN[@]}" > "$WORK/touched.txt"
check "a touched input is reported, and only it (control)" \
      "$(changed "$WORK/after.txt" "$WORK/touched.txt" | paste -sd' ' -)" \
      "${GEN[0]#"$TREE/"}"

finish
