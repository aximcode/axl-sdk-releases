#!/bin/bash
# check-compile-fixtures.sh -- the compile-only contract gates, compiled as
# the tree compiles a C++ TU (scripts/compile-like.py):
#
#   cb-noexcept        test/cb-noexcept-fixture.cpp must COMPILE (a noexcept
#                      callback is accepted) and must FAIL with -DEXPECT_REJECT
#                      (a throwing one is refused: AXL_CB_NOEXCEPT reaches the
#                      typedefs); then scripts/check-cb-noexcept.py over the
#                      public headers.
#   handle-exclusions  test/handle-exclusion-fixture.cpp must compile (every
#                      bound type is ownable by axl::unique_handle), and each
#                      EXPECT_REJECT_<NAME> must fail WITH its poison message
#                      -- a fixture typo fails identically, so the message is
#                      the test.
#
# Usage: check-compile-fixtures.sh cb-noexcept|handle-exclusions --tree DIR --like SRC
# A command that cannot be had (compile-like exit 125) is CANNOT RUN, never a
# compile that failed -- for the reject cases that would read as a pass.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WHICH="${1:-}"; shift || true
TREE="" LIKE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tree) TREE="$2"; shift 2 ;;
        --like) LIKE="$2"; shift 2 ;;
        *) echo "check-compile-fixtures: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[[ -n "$TREE" && -n "$LIKE" ]] || { echo "usage: $0 cb-noexcept|handle-exclusions --tree DIR --like SRC" >&2; exit 2; }
W="$(mktemp -d -t axl-cfix.XXXXXXXX)"
trap 'rm -rf "$W"' EXIT

# cc FIXTURE [FLAG...] -- compile; output in $W/err; returns the compiler's
# status, and exits the gate on 125.
cc() {
    local src="$1"; shift
    local adds=() f
    for f in "$@"; do adds+=("--add=$f"); done
    python3 "$REPO/scripts/compile-like.py" --tree "$TREE" --like "$LIKE" \
        --src "$REPO/$src" -o "$W/out.o" "${adds[@]}" > "$W/err" 2>&1
    local rc=$?
    if (( rc == 125 )); then
        echo "check-$WHICH: CANNOT RUN — compile-like.py could not build the compile command:"
        sed 's/^/    /' "$W/err" | head -6
        exit 1
    fi
    return $rc
}

case "$WHICH" in
cb-noexcept)
    if ! cc test/cb-noexcept-fixture.cpp; then
        echo "check-cb-noexcept: FAIL -- a noexcept callback was REJECTED"
        head -5 "$W/err" | sed 's/^/    /'
        exit 1
    fi
    if cc test/cb-noexcept-fixture.cpp -DEXPECT_REJECT; then
        echo "check-cb-noexcept: FAIL -- a THROWING callback was ACCEPTED;"
        echo "    AXL_CB_NOEXCEPT is not reaching the typedefs (missing"
        echo "    <axl/axl-macros.h>, or the C++ std is below C++17)"
        exit 1
    fi
    exec python3 "$REPO/scripts/check-cb-noexcept.py" "$REPO/include/axl"
    ;;
handle-exclusions)
    if ! cc test/handle-exclusion-fixture.cpp; then
        echo "check-handle-exclusions: FAIL -- a BOUND type was REJECTED;"
        echo "    axl::unique_handle must compile for every type whose header"
        echo "    invokes AXL_DEFINE_AUTOPTR_CLEANUP (or _ARG)."
        head -8 "$W/err" | sed 's/^/    /'
        exit 1
    fi
    for spec in "SURFACE:owned by the compositor surface tree" \
                "JSON:value type, not a handle"; do
        name=${spec%%:*}; want=${spec#*:}
        if cc test/handle-exclusion-fixture.cpp "-DEXPECT_REJECT_$name"; then
            echo "check-handle-exclusions: FAIL -- EXPECT_REJECT_$name COMPILED;"
            echo "    that type must not be ownable by axl::unique_handle."
            exit 1
        fi
        if ! grep -qF -- "$want" "$W/err"; then
            echo "check-handle-exclusions: FAIL -- EXPECT_REJECT_$name failed,"
            echo "    but not with its poison message. Expected to find:"
            echo "        $want"
            echo "    A fixture typo fails identically; the message IS the test."
            head -8 "$W/err" | sed 's/^/    /'
            exit 1
        fi
    done
    echo "check-handle-exclusions: clean -- bound types ownable, 2 exclusions refused with their reasons"
    ;;
*) echo "usage: $0 cb-noexcept|handle-exclusions --tree DIR --like SRC" >&2; exit 2 ;;
esac
