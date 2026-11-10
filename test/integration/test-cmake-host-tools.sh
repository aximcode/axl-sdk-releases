#!/bin/bash
# test-meta: arch=none needs=cmake,ninja,clang-tidy est=15 local-only=0 builds=ARCH=x64,fwtool-host
# test-cmake-host-tools.sh -- the HOST tools (axl_host_tool,
# cmake/AxlImages.cmake): pe-set-debug, axl-efi-pack and fwtool-host, built
# with the host compiler by one compiler call each.
#
#   1. Each records the headers ITS SOURCES include (cmake/AxlHostDeps.cmake):
#      a multi-source gcc call keeps only the last TU's -MD output, so the
#      packer was never rebuilt for an edit of src/pack/axl-pack-format.h --
#      which only its FIRST source includes -- and every packed image used a
#      stale packer.
#   2. The lint preset runs clang-tidy over every non-vendored host-tool
#      source (cmake/AxlLint.cmake): they are no CMake target's sources, so
#      no compile database held them, and pe-set-debug -- shipped in the SDK,
#      patching every image -- was outside the lint gate.
#
# Reads the build graphs (`ninja -t deps`, `ninja -t query`). Host-only.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_DIR" || exit 1
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

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
    echo "cmake-host-tools: $PASS passed, $FAIL failed"
    [[ $FAIL -eq 0 ]]
    exit $?
}

WORK="$(mktemp -d -t axl-hosttools.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 axl-busybox fwtool-host > "$WORK/build.log" 2>&1; then
    tail -20 "$WORK/build.log"
    check "the x64 tree builds the host tools" "failed" "built"
    finish
fi
TREE="$(scripts/axl-build.sh --print-prefix --abs ARCH=x64)"

# deps TOOL HEADER -> "recorded" when ninja's record of TOOL names HEADER.
deps() {
    local out
    out="$(ninja -C "$TREE" -t deps "build/$1" 2>&1)" || { echo "ninja: $out"; return; }
    grep -qxF "    $PROJECT_DIR/$2" <<< "$out" && echo recorded || echo "missing (${out%%$'\n'*})"
}
check "control: pe-set-debug's record holds its own source" \
    "$(deps pe-set-debug scripts/pe-set-debug.c)" "recorded"
check "axl-efi-pack: the header its FIRST source includes is recorded" \
    "$(deps axl-efi-pack src/pack/axl-pack-format.h)" "recorded"
check "axl-efi-pack: and a header only a MIDDLE source includes (Bra.c)" \
    "$(deps axl-efi-pack deps/lzma/RotateDefs.h)" "recorded"
check "axl-efi-pack: and its LAST source itself (Bra86.c, in its own rule only)" \
    "$(deps axl-efi-pack deps/lzma/Bra86.c)" "recorded"
check "fwtool-host: a library header its sources include is recorded (smoke)" \
    "$(deps fwtool-host include/axl/axl-fw.h)" "recorded"

# The lint preset: a clang-tidy step for each non-vendored host-tool source,
# and none for vendored code. Configured into a PRIVATE tree: build/x64-lint
# is gate.lint's, and a reconfigure there would rewrite its build.ninja under
# a running lint. The preset refuses to configure without clang-tidy.
if ! command -v clang-tidy-21 > /dev/null && ! command -v clang-tidy > /dev/null; then
    echo "SKIP: no clang-tidy -- the lint preset cannot configure (needs=clang-tidy)"
    exit 77
fi
if ! cmake --preset x64-lint -B "$WORK/lint" > "$WORK/lint.log" 2>&1; then
    tail -20 "$WORK/lint.log"
    check "the x64-lint preset configures" "failed" "configured"
    finish
fi
LINT="$WORK/lint"
steps="$(ninja -C "$LINT" -t query axl_lint_host_tools 2>&1 | sed -n 's|^ *lint-host/\(.*\)\.stamp$|\1|p' | sort | paste -sd' ')"
check "lint: every non-vendored host-tool source has a clang-tidy step, and only they" \
    "$steps" \
    "axl_efi_pack_scripts_axl_efi_pack_c axl_efi_pack_src_pack_axl_pack_format_c fwtool_host_src_data_axl_compress_c fwtool_host_src_data_axl_compress_lzma_c fwtool_host_src_fw_axl_fw_c fwtool_host_tools_fwtool_c pe_set_debug_scripts_pe_set_debug_c"
check "lint: the host-tool steps are part of the lint build (ALL)" \
    "$(ninja -C "$LINT" -t query all 2>&1 | grep -cx '    axl_lint_host_tools')" "1"

finish
