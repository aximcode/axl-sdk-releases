/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-font.h
    Bitmap font primitives used by axl-gfx and consumers (e.g. AGT).

    Defines the data structures that describe a bitmap font: per-glyph
    metrics + scanline data (`AxlGlyph`) and a font atlas
    (`AxlFont`) that bundles glyphs with font-wide metrics.  Each font
    is a separate compilation unit (see `src/gfx/fonts/`) so consumers
    can pick which fonts to link.

    A small number of built-in fonts are shipped with axl-gfx; consumers
    can also author their own `AxlFont` objects (typically generated
    from BDF or PSF sources via `scripts/gen-bdf-font.py`).

    @code
    const AxlFont *font = axl_gfx_default_font();
    axl_gfx_draw_text(font, 20, 20, "Hello", white, 1);
    @endcode

    Substrate discipline (per docs/AGT-Design.md): pure C, paradigm-
    agnostic — no widget, theme, or registry concepts.  Named-font
    lookup, fallback chains, and theme integration live in AGT.
**/

#ifndef AXL_FONT_H
#define AXL_FONT_H

#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Types
// ===================================================================

/// Maximum packed-bitmap bytes per glyph.
/// Sized to fit 16x16 glyphs (16 rows * 2 bytes/row).  Glyphs larger
/// than this are out of scope for v0.1 and will be addressed when a
/// consumer requires them.
#define AXL_GLYPH_MAX_BYTES  32

/// A single glyph: codepoint + bitmap + positioning metrics.
///
/// Bitmap layout: packed scanlines, row-major, MSB-first within each
/// byte.  Stride (bytes per row) = `(width + 7) / 8`.  Total bytes
/// used = `height * stride`, with remaining `bitmap[]` bytes undefined.
///
/// Default placement is top-of-cell aligned, pen-anchored.  The
/// `x_offset` / `y_offset` fields shift the bitmap from this default:
/// positive `x_offset` moves right (away from pen), positive
/// `y_offset` moves down (away from cell top).  Both are zero for
/// cell-based bitmap fonts where each glyph fills its cell exactly;
/// BDF-derived proportional fonts use non-zero offsets to position
/// glyph bitmaps within their cell.
typedef struct {
    uint32_t  codepoint;            ///< Unicode codepoint
    uint8_t   width;                ///< bitmap width in pixels
    uint8_t   height;               ///< bitmap height in pixels
    int8_t    x_offset;             ///< pixel shift from pen (positive = right)
    int8_t    y_offset;             ///< pixel shift from top of cell (positive = down)
    uint8_t   advance;              ///< pen advance after glyph (== cell_width for monospace)
    uint8_t   _pad[3];              ///< padding to align bitmap[]
    uint8_t   bitmap[AXL_GLYPH_MAX_BYTES];  ///< packed scanlines, stride = (width+7)/8
} AxlGlyph;

/// Font type flags — mirrors PEG's PegFont uType bits where applicable.
#define AXL_FONT_MONOSPACE  (1u << 0)  ///< every glyph's advance == cell_width
#define AXL_FONT_VARIABLE   (1u << 1)  ///< proportional advance widths (per-glyph)
/* Future: AXL_FONT_OUTLINE (2bpp), AXL_FONT_ANTIALIAS (4bpp) when a
 * consumer needs them.  The flag bits are reserved. */

/// A font atlas: array of glyphs sorted by codepoint + font-wide metrics.
///
/// Glyphs MUST be sorted by ascending `codepoint` to enable binary-search
/// lookup via `axl_font_glyph`.  Sparse codepoint coverage is the norm
/// for Unicode subset fonts; contiguous-range fonts simply happen to be
/// densely packed.
typedef struct {
    const char       *name;            ///< short identifier ("edk2-laffstd")
    const char       *description;     ///< human-readable description
    uint32_t          flags;           ///< AXL_FONT_* bitmask
    uint16_t          cell_width;      ///< monospace cell width (max-advance for variable)
    uint16_t          cell_height;     ///< glyph cell height (== ascent + descent)
    uint16_t          ascent;          ///< pixels above baseline
    uint16_t          descent;         ///< pixels below baseline
    uint16_t          line_height;     ///< recommended line-to-line advance
    uint32_t          n_glyphs;        ///< number of entries in `glyphs[]`
    const AxlGlyph   *glyphs;          ///< codepoint-sorted array
    uint32_t          fallback_codepoint;  ///< glyph used when a codepoint is missing
                                           ///< (0 = render blank, no fallback)
} AxlFont;

// ===================================================================
// Glyph lookup
// ===================================================================

/* Lookup-cost design note: codepoint-sorted array + binary search is
 * the chosen data structure for AxlFont, not a hash table or radix tree.
 *
 * For our font sizes (95-400 glyphs in built-in subsets, ~57K for a
 * hypothetical full BMP font) binary search is 7-18 integer compares
 * per lookup — sub-microsecond on UEFI-era hardware.  Hash table or
 * radix tree would add hash/index constant-factor overhead that
 * exceeds binary-search cost at this scale, AND would force runtime
 * initialization (allocating buckets/nodes), losing the static
 * `.rodata` placement that lets fonts live in the binary image with
 * zero init cost and zero allocation.
 *
 * If a future consumer ships full Unicode coverage AND profiles show
 * lookup is a real bottleneck, the right optimization is NOT a hash
 * table — it's a two-tier dense+sparse layout: contiguous ranges
 * (ASCII 0x20-0x7E, Latin-1, box-drawing, CJK Unified Ideographs)
 * become direct `array[cp - base]` indexing (O(1), preserves
 * `.rodata`), with sparse remainder via the current binary search.
 * The generator would identify dense spans and emit per-range index
 * tables.  See docs/AGT-Design.md §"Open questions parked for later".
 *
 * Until that consumer + profile data exist, the current single-tier
 * binary search is the right call.  Benchmark before switching.
 */

/// Look up a glyph by codepoint.
///
/// Performs binary search over the codepoint-sorted glyph array.  If
/// the requested codepoint is absent, returns the fallback glyph (when
/// @a font has a fallback_codepoint defined) or NULL.
///
/// @return pointer to the glyph (owned by @a font), or NULL if the
///         codepoint is missing and there is no fallback.
const AxlGlyph *
axl_font_glyph(
    const AxlFont  *font,        ///< [in] font atlas
    uint32_t        codepoint    ///< Unicode codepoint to look up
    );

/// Compute the advance width of a single codepoint in this font.
///
/// For monospace fonts this returns `cell_width` regardless of whether
/// the glyph is present.  For variable-width fonts, returns the glyph's
/// `advance` (or the fallback glyph's, or `cell_width` if neither
/// exists).
///
/// @return advance width in pixels.
uint16_t
axl_font_advance(
    const AxlFont  *font,        ///< [in] font atlas
    uint32_t        codepoint    ///< Unicode codepoint
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_FONT_H */
