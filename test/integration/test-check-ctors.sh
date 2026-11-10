#!/bin/bash
# test-meta: arch=none needs=gcc,nm est=3 local-only=0
# test-check-ctors.sh -- scripts/check-ctors.sh can fail (CLAUDE.md: a
# detector's silence is worth nothing until it is shown it can fail). The
# real trigger is a compiler without --enable-initfini-array, which no
# sabotage of the tree can produce, so the controls are host-built images:
# one whose .ctors bounds differ (constructors nothing runs) must FAIL, one
# whose bounds are equal must pass, and a tree with no image must FAIL rather
# than report clean over nothing.
#
# Host-only. Usage: ./test/integration/test-check-ctors.sh
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="$SCRIPT_DIR/../../scripts/check-ctors.sh"
W="$(mktemp -d -t axl-ctors.XXXXXXXX)"
trap 'rm -rf "$W"' EXIT
PASS=0 FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1)); fi
}
# image DIR NAME GAP -- a host .so whose __CTOR_LIST__ and __CTOR_END__ are
# GAP bytes apart (0: the empty .ctors every good image has).
image() {
    mkdir -p "$1"
    { printf '.data\n.globl __CTOR_LIST__\n.globl __CTOR_END__\n__CTOR_LIST__:\n'
      (( $3 > 0 )) && printf '.skip %s\n' "$3"
      printf '__CTOR_END__:\n.quad 0\n'; } > "$W/$2.s"
    gcc -shared -nostdlib -o "$1/$2.so" "$W/$2.s" || { echo "FAIL: cannot build a fixture"; exit 1; }
}
run() { local out rc; out="$("$GATE" --tree "$1" --binutils-prefix "" 2>&1)"; rc=$?; printf '%s|%s' "$rc" "$(tail -1 <<< "$out")"; }

image "$W/good" ok 0
check "an image with an empty .ctors passes" "$(run "$W/good")" \
      "0|check-ctors: clean -- 1 image(s), none with unwalked .ctors."
image "$W/bad" ok 0
image "$W/bad/tools" stale 8
r="$(run "$W/bad")"
check "an image with constructors in .ctors FAILS" "${r%%|*}" "1"
# Captured first: `| grep -q` under pipefail fails ON a match (SIGPIPE).
out="$("$GATE" --tree "$W/bad" --binutils-prefix "" 2>&1)"
grep -qxF "check-ctors: FAIL -- $W/bad/tools/stale.so has constructors in .ctors that" <<< "$out"
check "...naming the image, exactly" "$?" "0"
mkdir -p "$W/empty"
check "a tree with no image FAILS (it scanned nothing)" "$(run "$W/empty")" \
      "1|  A gate that scans nothing reports clean forever."
echo ""
echo "check-ctors: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
