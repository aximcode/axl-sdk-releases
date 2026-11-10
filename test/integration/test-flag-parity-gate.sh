#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-flag-parity-gate.sh -- check-flag-parity must SEE the packed-image path.
#
# A wrapped image is produced two ways: the CMake build's wrap
# (cmake/AxlWrapEfi.cmake, every shipped tool) and `axl-cc --compress` (every
# consumer). Both link the axl-pack stub
# with their own objcopy and both run axl-efi-pack. objcopy -j takes EXACT
# section names, so a stub link that drops `.axlpack` produces an image with no
# payload; and the wrap policy lives in axl-efi-pack alone, so a caller passing
# it a tuning flag forks the policy. check-flag-parity compares both.
#
# The retired Makefile spelled the stub's section through a variable, which
# the gate's literal `-j .name` scan never matched -- the first version of the
# stub link passed the gate by not being seen. So the gate RESOLVES shell
# variables/arrays and the CMake wrap's SECTIONS, and an argument it cannot
# resolve is a failure, never a skip. Each case below proves one way it can
# fail.
#
# Host-only: no QEMU, no build. Operates on COPIES of the build files,
# so it runs against the working tree (uncommitted changes included) and never
# mutates a tracked file.
#
# Usage: ./test/integration/test-flag-parity-gate.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
GATE_REL="scripts/check-flag-parity.py"

WORK="$(mktemp -d -t axl-paritygate.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

build_tree() {
    local dst="$1"
    mkdir -p "$dst/scripts"
    cp "$PROJECT_DIR/$GATE_REL" "$PROJECT_DIR/scripts/axl-cc" \
       "$dst/scripts/"
    mkdir -p "$dst/cmake"
    cp "$PROJECT_DIR/cmake/axl-config.cmake.in" "$dst/cmake/"
    cp "$PROJECT_DIR/cmake/AxlFlags.cmake" "$PROJECT_DIR/cmake/AxlImages.cmake" \
       "$PROJECT_DIR/cmake/AxlWrapEfi.cmake" "$dst/cmake/"
}

# The gate reads its paths relative to the cwd, as gate.check-flag-parity
# runs it.
run_gate() { ( cd "$1" && python3 "$GATE_REL" 2>&1 ); }

# apply <tree> <file> <sed-expr> -- refuses a sed that changed nothing, which
# would make the case below it prove nothing.
apply() {
    local f="$1/$2"
    cp "$f" "$f.orig"
    sed -i "$3" "$f"
    if cmp -s "$f" "$f.orig"; then
        fail "fixture did NOT apply to $2 ($3) -- the case proves nothing"
        return 1
    fi
    rm -f "$f.orig"
}

# expect_fail <name> <tree> <exact problem line>
expect_fail() {
    local out rc
    out="$(run_gate "$2")"; rc=$?
    if [[ $rc -eq 1 ]] && grep -Fxq -- "$3" <<< "$out"; then
        pass "$1"
    else
        fail "$1 (rc=$rc)"
        echo "      want line: $3"
        sed 's/^/      got: /' <<< "$out" | head -8
    fi
}

echo "=== 1. baseline: the gate passes the working tree ==="
B="$WORK/base"; build_tree "$B"
out="$(run_gate "$B")"; rc=$?
want="check-flag-parity: clean — 7 critical flags and 14 objcopy sections agree across 2 build path(s); cmake/axl-config.cmake.in delegates to axl-cc; the axl-pack stub link (15 objcopy sections) agrees between CMake wrap (AxlWrapEfi.cmake) and axl-cc --compress, and axl-cc --compress passes axl-efi-pack only --stub-size -o; 6 critical ld flags (-nostdlib -shared -Bsymbolic --no-undefined -z noexecstack --gc-sections) agree between axl-cc LD_BASE_FLAGS, CMake AXL_LDFLAGS_EFI; CMake wrap (AxlWrapEfi.cmake) agrees"
if [[ $rc -eq 0 && "$out" == "$want" ]]; then
    pass "clean tree passes, naming the wrap comparison"
else
    fail "clean tree (rc=$rc) -- every negative below is meaningless"
    echo "      want: $want"
    sed 's/^/      got:  /' <<< "$out" | head -8
fi

echo
echo "=== 2. axl-cc passes axl-efi-pack a tuning flag (the control) ==="
T="$WORK/lc-cc"; build_tree "$T"
apply "$T" scripts/axl-cc 's|"\$AXL_EFI_PACK" --stub-size|"$AXL_EFI_PACK" --lc 0 --stub-size|' &&
expect_fail "axl-cc --lc 0 fails the gate" "$T" \
    "  wrap: axl-cc --compress: passes axl-efi-pack --lc -- the wrap policy lives in axl-efi-pack, not in a caller"

echo
echo "=== 3. a tuning flag hidden in a shell array is still seen ==="
T="$WORK/lc-array"; build_tree "$T"
apply "$T" scripts/axl-cc 's|"\$AXL_EFI_PACK" --stub-size|"$AXL_EFI_PACK" "${PACK_EXTRA[@]}" --stub-size|' &&
apply "$T" scripts/axl-cc 's|^PACK_SECTION=".axlpack"$|PACK_EXTRA=(--lc 0)\nPACK_SECTION=".axlpack"|' &&
expect_fail "--lc 0 through \${PACK_EXTRA[@]} fails the gate" "$T" \
    "  wrap: axl-cc --compress: passes axl-efi-pack --lc -- the wrap policy lives in axl-efi-pack, not in a caller"

echo
echo "=== 6. axl-cc's stub link drops the payload section ==="
T="$WORK/drop"; build_tree "$T"
apply "$T" scripts/axl-cc 's| -j "\$PACK_SECTION"||' &&
expect_fail "axl-cc without -j .axlpack fails the gate" "$T" \
    "  wrap: the axl-pack stub link's objcopy -j lists differ -- only in CMake wrap (AxlWrapEfi.cmake): .axlpack; only in axl-cc --compress: (none)"

echo
echo "=== 9. a missing stub link is a failure, not a pass ==="
T="$WORK/nofunc"; build_tree "$T"
apply "$T" scripts/axl-cc 's|^    pack_stub_link() {$|    pack_stub_link_v2() {|' &&
expect_fail "axl-cc with no pack_stub_link() fails the gate" "$T" \
    "  wrap: axl-cc --compress: no pack_stub_link() function -- the stub link cannot be compared"

echo
echo "=== 12. an unresolvable word AFTER the input is not taken for the input ==="
T="$WORK/tail"; build_tree "$T"
apply "$T" scripts/axl-cc 's#-o "\$TMPDIR/payload.pack" "\$LINKED" ||#-o "$TMPDIR/payload.pack" "$LINKED" $PACK_TUNING ||#' &&
expect_fail "a trailing \$PACK_TUNING fails the gate, named" "$T" \
    "  wrap: axl-cc --compress: axl-efi-pack argument \$PACK_TUNING does not resolve"

echo
echo "=== 14. axl-cc's image link drops --no-undefined ==="
T="$WORK/ld-cc"; build_tree "$T"
apply "$T" scripts/axl-cc 's|^               --no-warn-rwx-segments --no-undefined -z noexecstack --gc-sections)$|               --no-warn-rwx-segments -z noexecstack --gc-sections)|' &&
expect_fail "LD_BASE_FLAGS without --no-undefined fails the gate" "$T" \
    "  ld: --no-undefined: in CMake AXL_LDFLAGS_EFI but MISSING from axl-cc LD_BASE_FLAGS"

echo
echo "=== 15. -z split from its value is not -z noexecstack ==="
T="$WORK/ld-split"; build_tree "$T"
apply "$T" scripts/axl-cc 's|^               --no-warn-rwx-segments --no-undefined -z noexecstack --gc-sections)$|               --no-warn-rwx-segments --no-undefined -z --gc-sections noexecstack)|' &&
expect_fail "-z with another value fails the gate" "$T" \
    "  ld: -z noexecstack: in CMake AXL_LDFLAGS_EFI but MISSING from axl-cc LD_BASE_FLAGS"

echo
echo "=== 16. a link-flag list the gate cannot find is a failure ==="
T="$WORK/ld-none"; build_tree "$T"
apply "$T" scripts/axl-cc 's|^LD_BASE_FLAGS=(|LD_FLAGS_V2=(|' &&
expect_fail "axl-cc with no LD_BASE_FLAGS array fails the gate" "$T" \
    "  ld: axl-cc LD_BASE_FLAGS: not found -- the link flags cannot be compared"

echo
echo "=== 17. the CMake build drops a critical compile flag ==="
T="$WORK/cm-wchar"; build_tree "$T"
apply "$T" cmake/AxlFlags.cmake 's/ -fshort-wchar//g; s/^    -fshort-wchar /    /' &&
expect_fail "CMake without -fshort-wchar fails the gate, naming it" "$T" \
    "  -fshort-wchar: present in scripts/axl-cc but MISSING from CMake build"

echo
echo "=== 18. the CMake image link drops --no-undefined ==="
T="$WORK/cm-ld"; build_tree "$T"
apply "$T" cmake/AxlImages.cmake 's/^    --no-undefined -z noexecstack/    -z noexecstack/' &&
expect_fail "CMake's AXL_LDFLAGS_EFI without --no-undefined fails the gate" "$T" \
    "  ld: --no-undefined: in axl-cc LD_BASE_FLAGS but MISSING from CMake AXL_LDFLAGS_EFI"

echo
echo "=== 19. the CMake objcopy list drops a section ==="
T="$WORK/cm-j"; build_tree "$T"
apply "$T" cmake/AxlImages.cmake 's/ -j \.rela\.dyn//' &&
expect_fail "CMake's -j list without .rela.dyn fails the gate" "$T" \
    "  objcopy -j: CMake build does not carry .rela.dyn (other paths do). \`-j\` takes EXACT section names."

echo
echo "=== 20. the CMake wrap's stub link drops the payload section ==="
T="$WORK/cm-wrapj"; build_tree "$T"
apply "$T" cmake/AxlWrapEfi.cmake 's/ -j \.axlpack//' &&
expect_fail "AxlWrapEfi.cmake without -j .axlpack fails the gate" "$T" \
    "  wrap: CMake wrap (AxlWrapEfi.cmake): the stub link does not carry \`-j .axlpack\` -- an image with no payload"

echo
echo "=== 21. the CMake wrap passes the packer a tuning flag ==="
T="$WORK/cm-wraptune"; build_tree "$T"
apply "$T" cmake/AxlWrapEfi.cmake 's/\${PACK_FLAGS} --stub-size/${PACK_FLAGS} --level 9 --stub-size/' &&
expect_fail "AxlWrapEfi.cmake passing --level fails the gate" "$T" \
    "  wrap: CMake wrap (AxlWrapEfi.cmake): passes axl-efi-pack --level -- the wrap policy lives in axl-efi-pack, not in a caller"

echo
echo "=== 22. the CMake wrap's stub link is not handed the image list ==="
T="$WORK/cm-wrapsec"; build_tree "$T"
apply "$T" cmake/AxlWrapEfi.cmake 's/\${OBJCOPY} \${SECTIONS} -j \.axlpack/${OBJCOPY} -j .text -j .axlpack/' &&
expect_fail "a wrap stub link not given SECTIONS fails the gate (cannot be compared)" "$T" \
    "  wrap: CMake wrap (AxlWrapEfi.cmake): its stub's -j list is not AXL_OBJCOPY_SECTIONS handed in as SECTIONS -- the stub link cannot be compared"

echo
echo "=== 23. a section axl-cc's stub carries that the CMake one does not ==="
T="$WORK/cc-extra"; build_tree "$T"
apply "$T" scripts/axl-cc 's| -j "\$PACK_SECTION"| -j "$PACK_SECTION" -j .extra|' &&
expect_fail "axl-cc's stub with an extra -j .extra fails the gate" "$T" \
    "  wrap: the axl-pack stub link's objcopy -j lists differ -- only in CMake wrap (AxlWrapEfi.cmake): (none); only in axl-cc --compress: .extra"

echo
echo "=== 24. the CMake image rules hand the wrap a tuning flag as PACK_FLAGS ==="
T="$WORK/cm-packflags"; build_tree "$T"
apply "$T" cmake/AxlImages.cmake 's/-DPACK_FLAGS=\$<\$<BOOL:\${A_FORCE}>:--force>/-DPACK_FLAGS=--lc|0/' &&
expect_fail "-DPACK_FLAGS=--lc|0 fails the gate" "$T" \
    "  wrap: cmake/AxlImages.cmake hands the wrap -DPACK_FLAGS=--lc|0; only -DPACK_FLAGS=\$<\$<BOOL:\${A_FORCE}>:--force> (--force, test images) is allowed"

echo
echo "=== 25. an unresolvable reference in the CMake wrap's packer call ==="
T="$WORK/cm-packref"; build_tree "$T"
apply "$T" cmake/AxlWrapEfi.cmake 's/\${PACK_FLAGS} --stub-size/${PACK_FLAGS} ${PACK_TUNING} --stub-size/' &&
expect_fail "\${PACK_TUNING} in the packer call fails the gate, named" "$T" \
    "  wrap: CMake wrap (AxlWrapEfi.cmake): axl-efi-pack argument \${PACK_TUNING} does not resolve"

echo
echo "=== 26. the CMake wrap appends a tuning flag to its own PACK_FLAGS ==="
T="$WORK/cm-packappend"; build_tree "$T"
apply "$T" cmake/AxlWrapEfi.cmake 's/^file(REMOVE "\${OUT}.tmp")$/list(APPEND PACK_FLAGS --lc 0)\nfile(REMOVE "${OUT}.tmp")/' &&
expect_fail "list(APPEND PACK_FLAGS --lc 0) in the wrap fails the gate" "$T" \
    "  wrap: CMake wrap (AxlWrapEfi.cmake): list(...) writes PACK_FLAGS -- the packer's arguments must be what the caller handed in"

echo
echo "=== 27. the image rules hand the wrap no PACK_FLAGS at all ==="
T="$WORK/cm-nopackflags"; build_tree "$T"
apply "$T" cmake/AxlImages.cmake 's/ -DPACK_FLAGS=\$<\$<BOOL:\${A_FORCE}>:--force>//' &&
expect_fail "no -DPACK_FLAGS= fails the gate (cannot be compared)" "$T" \
    "  wrap: cmake/AxlImages.cmake hands the wrap no -DPACK_FLAGS= -- the packer's caller flags cannot be compared"

echo
echo "=== 28. a second packer call in the CMake wrap ==="
T="$WORK/cm-twopack"; build_tree "$T"
apply "$T" cmake/AxlWrapEfi.cmake 's/^file(REMOVE "\${OUT}.tmp")$/execute_process(COMMAND ${PACK_TOOL} --stub-size 1 -o x y)\nfile(REMOVE "${OUT}.tmp")/' &&
expect_fail "two packer calls fail the gate (cannot be compared)" "$T" \
    "  wrap: CMake wrap (AxlWrapEfi.cmake): 2 packer call(s) found (want exactly one) -- the wrap cannot be compared"

echo
echo "flag-parity-gate: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 && $PASS -gt 0 ]]
