#!/bin/bash
# test-meta: arch=none needs=cmake,ninja est=15 local-only=0 builds=ARCH=x64,sshd
# test-cmake-wrap.sh -- the CMake build's WRAP (cmake/AxlWrapEfi.cmake, run
# by axl_wrap_efi for every shipped image): the build behaviour
# test-efi-pack-qemu.sh asserts only of the Makefile's WRAP_EFI, owed before
# the Makefile goes (docs/AXL-Build-System-Design.md §10.5, §10.10):
#
#   1. AXL_PACK on wraps an image the packer says pays;
#   2. AXL_PACK off copies it raw -- the raw image's bytes, exactly;
#   3. re-enabled, it wraps again, byte-identical to the first wrap;
#   4. a wrap step failing AFTER the stub image is written (pe-set-debug)
#      fails the step, leaves the previous image untouched, and leaves it out
#      of date (older than its raw input, which is what Ninja compares) --
#      and the next run re-wraps it, byte-identical.
#
# THE REAL STEP, NOT A RESTATEMENT: the wrap command is read off the build
# graph (`ninja -t commands`) for tools/sshd.efi in this tree, and replayed
# with only its RAW / OUT / STATE (and, for case 4, PE_SET_DEBUG) pointed at
# this test's own copies -- every other argument is the build's. The shared
# tree is only read: flipping AXL_PACK there would re-wrap images other tests
# are booting.
#
# AND THE TOGGLE REACHES THE STEP: a private tree configured with
# -DAXL_PACK=OFF (configure only, ~3 s) must give the same edge the same
# command except -DMODE=off. Ninja reruns an edge whose command changed (its
# .ninja_log keeps each output's command hash), so on an up-to-date tree the
# toggle takes effect -- the make rule needed a mode stamp for that.
#
# Host-only. Usage: ./test/integration/test-cmake-wrap.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
# cmake --preset reads CMakePresets.json from the current directory.
cd "$PROJECT_DIR" || exit 1
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
# shellcheck source=lib/pack-lib.sh
source "$SCRIPT_DIR/lib/pack-lib.sh"

WORK="$(mktemp -d -t axl-cmkwrap.XXXXXXXX)"
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
finish() {
    echo ""
    echo "cmake-wrap: $PASS passed, $FAIL failed"
    [[ $FAIL -eq 0 ]]
    exit $?
}

if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 sshd > "$WORK/build.log" 2>&1; then
    tail -20 "$WORK/build.log"
    check "the x64 tree builds sshd" "failed" "built"
    finish
fi
TREE="$("$PROJECT_DIR/scripts/axl-build.sh" --print-prefix --abs ARCH=x64)"
# Under make there is no wrap edge to read: this test is the CMake build's.
[[ -f "$TREE/build.ninja" ]] || { check "a CMake tree at $TREE" "none" "build.ninja"; finish; }

# wrap_cmd TREE -- the build's own command for tools/sshd.efi's wrap step.
# Exit 2 when ninja could not answer at all -- not the same fact as a graph
# with no wrap step (exit 0, empty).
wrap_cmd() {
    local all
    all="$(ninja -C "$1" -t commands tools/sshd.efi 2>&1)" || { echo "$all" >&2; return 2; }
    grep -F -- '-P '"$PROJECT_DIR"'/cmake/AxlWrapEfi.cmake' <<< "$all" | tail -1
    return 0
}
if ! CMD="$(wrap_cmd "$TREE")"; then
    check "ninja -t commands answers for $TREE" "failed" "answered"
    finish
fi
check "the graph has a wrap step for tools/sshd.efi" \
      "$([[ -n "$CMD" ]] && echo found)" "found"
[[ -n "$CMD" ]] || finish
arg() {     # arg NAME -- the value of -DNAME= in $CMD
    [[ "$CMD" =~ (^|[[:space:]])-D$1=([^[:space:]]*) ]] && printf '%s' "${BASH_REMATCH[2]}"
}
RAW0="$(arg RAW)"; OUT0="$(arg OUT)"; STATE0="$(arg STATE)"; PSD0="$(arg PE_SET_DEBUG)"
check "in the default tree the step runs with -DMODE=on" "$(arg MODE)" "on"
for v in RAW0 OUT0 STATE0 PSD0; do
    [[ -n "${!v}" ]] || { check "the step names ${v%0}" "nothing" "a path"; finish; }
    # A quoted argument (a path with a space) would be cut at the space here.
    [[ "${!v}" != \"* ]] || { check "the step's ${v%0} is a plain word" "${!v}" "unquoted"; finish; }
done

RAW="$WORK/sshd-raw.efi"; OUT="$WORK/sshd.efi"
cp "$RAW0" "$RAW"
# run_wrap MODE [PE_SET_DEBUG] -- the build's step on this test's copies.
# The patterns are QUOTED (a path's [ * ? are literal), and the result is
# checked before it runs: a substitution that did not apply would replay the
# step on the SHARED tree -- an `off` run would leave a raw sshd.efi there,
# newer than its inputs, which Ninja would then ship as up to date.
run_wrap() {
    local c="$CMD"
    c="${c//"-DRAW=$RAW0"/"-DRAW=$RAW"}"
    c="${c//"-DOUT=$OUT0"/"-DOUT=$OUT"}"
    c="${c//"-DSTATE=$STATE0"/"-DSTATE=$WORK/state"}"
    c="${c//"-DMODE=on"/"-DMODE=$1"}"
    c="${c//"-DPE_SET_DEBUG=$PSD0"/"-DPE_SET_DEBUG=${2:-$PSD0}"}"
    if [[ "$c" == *"$OUT0"* || "$c" == *"$STATE0"* || "$c" == *"-DRAW=$RAW0"* \
          || "$c" != *"-DOUT=$OUT "* || "$c" != *"-DMODE=$1 "* ]]; then
        check "the replay is redirected to this test's copies" "not redirected" "redirected"
        finish
    fi
    bash -c "$c" >> "$WORK/wrap.log" 2>&1
}

echo "=== the step's behaviour ==="
run_wrap on
check "1. AXL_PACK on: the step succeeds" "$?" "0"
hdr="$(pack_header_offset "$OUT" 34404)"
check "1. ...and wraps (a packed payload in the image)" \
      "$([[ "$hdr" =~ ^[0-9]+$ ]] && echo wrapped || echo "$hdr")" "wrapped"
check "1. ...smaller than the raw image" \
      "$(( $(stat -c %s "$OUT") < $(stat -c %s "$RAW") ))" "1"
cp "$OUT" "$WORK/wrapped.efi"

run_wrap off
check "2. AXL_PACK off: the step succeeds" "$?" "0"
cmp -s "$OUT" "$RAW"
check "2. ...and the image is the raw image's bytes" "$?" "0"

run_wrap on
cmp -s "$OUT" "$WORK/wrapped.efi"
check "3. re-enabled: wrapped again, byte-identical to the first wrap" "$?" "0"

# It records that it ran: the failure is then known to be pe-set-debug's,
# AFTER the stub image was written -- not the packer, cc or ld dying first.
printf '#!/bin/sh\ntouch "%s"\nexit 1\n' "$WORK/psd-called" > "$WORK/pe-set-debug-fails"
chmod +x "$WORK/pe-set-debug-fails"
touch -d '-1 minute' "$OUT"
touch "$RAW"
run_wrap on "$WORK/pe-set-debug-fails"
rc=$?
check "4. a failing pe-set-debug fails the step" "$(( rc != 0 ))" "1"
check "4. ...failing AT pe-set-debug, the stub image written (OUT.tmp)" \
      "$([[ -f "$WORK/psd-called" && -f "$OUT.tmp" ]] && echo yes || echo no)" "yes"
cmp -s "$OUT" "$WORK/wrapped.efi"
check "4. ...and leaves the previous image untouched" "$?" "0"
check "4. ...still out of date (older than its raw input)" \
      "$([[ "$OUT" -ot "$RAW" ]] && echo stale || echo fresh)" "stale"
run_wrap on
cmp -s "$OUT" "$WORK/wrapped.efi"
check "4. the next run re-wraps it, byte-identical" "$?" "0"
check "4. ...and it is up to date again" \
      "$([[ "$OUT" -ot "$RAW" ]] && echo stale || echo fresh)" "fresh"

echo "=== the toggle reaches the step ==="
if cmake --preset x64-debug -B "$WORK/tree-off" -DAXL_PACK=OFF \
        > "$WORK/configure.log" 2>&1; then
    OFF_CMD="$(wrap_cmd "$WORK/tree-off")"
    # The same edge, but for the tree path and the mode.
    norm_on="${CMD//$TREE/TREE}"
    norm_off="${OFF_CMD//$WORK\/tree-off/TREE}"
    check "AXL_PACK=OFF: the same step, with -DMODE=off and nothing else changed" \
          "${norm_off}" "${norm_on//-DMODE=on/-DMODE=off}"
else
    tail -20 "$WORK/configure.log"
    check "a private tree configures with -DAXL_PACK=OFF" "failed" "configured"
fi

finish
