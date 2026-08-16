#!/bin/bash
# test-meta: arch=x64 needs= est=1 local-only=0
# test-gfx-link-granularity.sh — a display-mode-only consumer must NOT pull
# the FreeType ftgrays rasterizer (and its FTL credit obligation).
#
# The GOP-inventory / mode-query accessors (axl_gfx_output_count / _get /
# _query_mode / _get_pixel_bitmask) are pure EFI_GRAPHICS_OUTPUT_PROTOCOL
# reads — they never rasterize a vector path. But the EFI link is
# `-shared -Bsymbolic`, which exports every global as a --gc-sections root,
# and archive members are selected at object granularity: if the accessors
# share a translation unit with axl_gfx_push_clip_path (-> axl_gfx_rasterize
# _fill -> ftgrays), pulling one accessor drags the whole object in, whose
# exported push_clip_path then roots ftgrays even though nothing calls it.
# Keeping the accessors in their own object (src/gfx/axl-gfx-output.c) breaks
# that coupling. This guard links a GOP-only consumer and asserts ftgrays is
# absent, with a control link that DOES rasterize to prove the check bites.
#
# x64 only: the mechanism (archive-member selection + exported-symbol roots)
# is arch-independent, so x64 validation suffices.
#
# Usage: ./test/integration/test-gfx-link-granularity.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
cd "$PROJECT_DIR"

ARCH=x64
B="$("$PROJECT_DIR/scripts/build-prefix.sh" "$ARCH")/build"
LIB="$("$PROJECT_DIR/scripts/build-prefix.sh" "$ARCH")/lib/libaxl.a"
LDS="scripts/elf_x86_64_efi.lds"
CRT0="$B/axl-crt0-gcc-x86_64.o"

# The probe links against a cross-built libaxl.a, so it must be compiled by the
# SAME compiler -- host gcc would resolve <string.h> to /usr/include and mix a
# host object into a bare-metal link. Read the path from the one manifest that
# holds it (scripts/axl-toolchains.conf), exactly as the Makefile and axl-cc do.
# shellcheck source=/dev/null
source "$PROJECT_DIR/scripts/axl-toolchains.conf"
AXL_CC_BIN="${AXL_X64_GCC:-$AXL_X64_GCC_DEFAULT}"
if [[ ! -x "$AXL_CC_BIN" ]] && ! command -v "$AXL_CC_BIN" &>/dev/null; then
    echo "SKIP: x64 bare-metal gcc not found at $AXL_CC_BIN" >&2
    exit 0
fi

make ARCH="$ARCH" 2>&1 | tail -1

CFLAGS=(-std=gnu2x -ffreestanding -fshort-wchar -fno-stack-protector
        -fno-builtin -fpic -mno-red-zone -march=x86-64
        -ffunction-sections -fdata-sections -DAXL_BACKEND_NATIVE
        -Iinclude -Isrc/backend)
LDFLAGS=(-nostdlib -shared -Bsymbolic --no-warn-rwx-segments
         --no-undefined --gc-sections -T "$LDS")

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Link the named C body into a .so and report its ftgrays symbol count.
# The link mirrors a unit-test EFI link (crt0 entry stub + reloc +
# debug-info + libaxl.a); AXL_APP supplies the int-main -> _AxlEntry
# bridge. set -e aborts on a failed gcc/ld before the nm|grep, so a
# broken build can't yield a spurious 0.
gray_count() {
    local name="$1" body="$2"
    printf '%s\n' "#include <axl.h>" \
        "static int m(int a,char**v){(void)a;(void)v;$body return 0;}" \
        "AXL_APP(m)" > "$WORK/$name.c"
    "$AXL_CC_BIN" "${CFLAGS[@]}" -c "$WORK/$name.c" -o "$WORK/$name.o"
    ld "${LDFLAGS[@]}" -o "$WORK/$name.so" "$CRT0" \
        "$B/axl-reloc.o" "$B/axl-debug-info.o" "$WORK/$name.o" "$LIB"
    nm "$WORK/$name.so" 2>/dev/null | grep -icE 'gray_|ft_grays' || true
}

fail=0

# 1) The real assertion: a GOP-inventory-only consumer pulls no ftgrays.
gop=$(gray_count gop_only '
    AxlGfxOutputMode mi; (void)axl_gfx_output_query_mode(0,0,&mi);
    axl_printf("%zu\n", axl_gfx_output_count());')
if [[ "$gop" -eq 0 ]]; then
    echo "  PASS: GOP-inventory-only consumer pulls 0 ftgrays symbols"
else
    echo "  FAIL: GOP-inventory-only consumer pulled $gop ftgrays symbols"
    fail=1
fi

# 2) Control: a consumer that DOES rasterize must pull ftgrays — proves the
#    check can actually detect the rasterizer (guards against a false PASS,
#    e.g. if ftgrays were renamed or dropped).
ras=$(gray_count rasterizes '
    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p,0,0); axl_gfx_path_line_to(p,9,0);
    axl_gfx_path_line_to(p,9,9);
    (void)axl_gfx_fill_path(p, (AxlGfxPixel){255,255,255,255});
    axl_gfx_path_free(p);')
if [[ "$ras" -gt 0 ]]; then
    echo "  PASS: rasterizing consumer pulls ftgrays ($ras symbols) — check is live"
else
    echo "  FAIL: rasterizing consumer pulled 0 ftgrays — check cannot detect it"
    fail=1
fi

if (( fail )); then
    echo "FAIL: gfx link-granularity"
    exit 1
fi
echo "All gfx link-granularity checks passed."
exit 0
