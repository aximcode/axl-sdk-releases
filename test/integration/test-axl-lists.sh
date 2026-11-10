#!/bin/bash
# test-meta: arch=none needs= est=5 local-only=0
# test-axl-lists.sh -- scripts/axl-list.sh, the reader of the build's named
# lists (scripts/axl-lists.conf), which replaced `make -s print-<NAME>` for
# packaging, gates and tests (docs/AXL-Build-System-Design.md §10.9).
#
# Fixtures pin the reader's grammar, exactly: continuation, comments, a
# $(NAME) reference, and every refusal (unknown name, undefined or forward
# reference, a make construct it does not implement, an empty or duplicate
# list, a continuation off the end) -- each refusal prints NOTHING on stdout,
# because a short list is how a packaging step ships too little. Then the
# real file: the names it must define, and SHIPPED_TOOL_NAMES composed from
# its parts.
#
# Host-only. Usage: ./test/integration/test-axl-lists.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LIST="$PROJECT_DIR/scripts/axl-list.sh"

WORK="$(mktemp -d -t axl-lists.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
# run FILE ARGS... -> "rc|stdout|first stderr line"
run() {
    local f="$1" out err rc
    shift
    out="$(AXL_LISTS_FILE="$f" "$LIST" "$@" 2>"$WORK/err")"; rc=$?
    err="$(head -1 "$WORK/err")"
    printf '%s|%s|%s' "$rc" "$out" "$err"
}
fx() {      # fx NAME LINES... -- a fixture file
    printf '%s\n' "${@:2}" > "$WORK/$1"
    printf '%s' "$WORK/$1"
}

echo "=== the grammar ==="
F=$(fx good '# a comment' '' 'A = one two \' '    three' '# between' 'B = $(A) four' 'C=five')
check "a continued list, one line"       "$(run "$F" A)"   "0|one two three|"
check "a reference, expanded in place"   "$(run "$F" B)"   "0|one two three four|"
check "no spaces around '='"             "$(run "$F" C)"   "0|five|"
check "several names, one line each"     "$(run "$F" C A)" "0|five
one two three|"
check "--names, in file order"           "$(run "$F" --names)" "0|A
B
C|"

echo "=== the refusals: exit 2, nothing on stdout ==="
check "an unknown name" "$(run "$F" A NOPE)" \
      "2||axl-list.sh: no list named 'NOPE' in $F (it defines: A B C)"
F=$(fx fwd 'A = $(B)' 'B = x')
check "a forward reference" "$(run "$F" A)" \
      "2||axl-list.sh: $F:1: A uses \$(B) before it is defined"
F=$(fx imm 'A := x')
check "make's := is not read as words" "$(run "$F" A)" \
      "2||axl-list.sh: $F:1: not 'NAME = words': A := x"
F=$(fx fn 'A = $(wildcard *.c)')
check "a make function is refused" "$(run "$F" A)" \
      "2||axl-list.sh: $F:1: A: '\$(wildcard' is not a plain word or \$(NAME)"
F=$(fx cmt 'A = x y # note')
check "a trailing comment is refused, not read as words" "$(run "$F" A)" \
      "2||axl-list.sh: $F:1: A: '#' is not a plain word or \$(NAME)"
touch "$WORK/s1.c" "$WORK/s2.c"
F=$(fx glob 'A = *.c')
check "a glob is refused, never expanded against the cwd" "$(cd "$WORK" && run "$F" A)" \
      "2||axl-list.sh: $F:1: A: '*.c' is not a plain word or \$(NAME)"
F=$(fx empty 'A =')
check "an empty list" "$(run "$F" A)" "2||axl-list.sh: $F:1: A is empty"
F=$(fx dup 'A = x' 'A = y')
check "a list defined twice" "$(run "$F" A)" "2||axl-list.sh: $F:2: A defined twice"
F=$(fx tail 'A = x \')
check "a continuation off the end" "$(run "$F" A)" \
      "2||axl-list.sh: $F:1: a continued line runs off the end of the file"
check "a missing file" "$(run "$WORK/none" A)" "2||axl-list.sh: cannot read $WORK/none"
check "no arguments" "$(run "$WORK/good")" "2||axl-list.sh: usage: axl-list.sh NAME... | --names"

echo "=== the real file ==="
real="$("$LIST" --names | paste -sd' ')"
check "it defines the five lists" "$real" \
      "TOOL_NAMES TOOL_EXTRA_APPS SHIPPED_TOOL_NAMES HOST_TOOL_FILES NON_ARTIFACT_GOALS"
# The goals suite-prep builds that make no gated artifact (moved here from the
# Makefile when it was retired; cmake/AxlGoals.cmake refuses a name it does not know).
check "NON_ARTIFACT_GOALS is tool-sizes" "$("$LIST" NON_ARTIFACT_GOALS)" "tool-sizes"
composed="$("$LIST" TOOL_NAMES TOOL_EXTRA_APPS | paste -sd' ') kbtune-drv SftpServer sshd-full"
check "SHIPPED_TOOL_NAMES = TOOL_NAMES + TOOL_EXTRA_APPS + three" \
      "$("$LIST" SHIPPED_TOOL_NAMES)" "$composed"
n=$("$LIST" TOOL_NAMES | wc -w)
check "TOOL_NAMES is the full tool set (>= 30)" "$(( n >= 30 ))" "1"

echo ""
echo "axl-lists: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
