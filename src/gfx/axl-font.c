/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-font.c
    Font lookup helpers — binary search over codepoint-sorted glyph arrays.

    Per substrate discipline (docs/AGT-Design.md §"Substrate discipline
    rules"): pure C, stateless, paradigm-agnostic.  The named-font
    registry, fallback chains, and theme integration that PEG/GTK
    expose live in the toolkit layer above (AGT).
**/

#include <axl/axl-font.h>

/* Binary search over the codepoint-sorted glyph array. */
static const AxlGlyph *
bsearch_glyph(
    const AxlFont  *font,
    uint32_t        codepoint
    )
{
    uint32_t lo = 0;
    uint32_t hi = font->n_glyphs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t cp  = font->glyphs[mid].codepoint;
        if (cp == codepoint) {
            return &font->glyphs[mid];
        }
        if (cp < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const AxlGlyph *
axl_font_glyph(
    const AxlFont  *font,
    uint32_t        codepoint
    )
{
    if (font == NULL || font->glyphs == NULL || font->n_glyphs == 0) {
        return NULL;
    }
    const AxlGlyph *g = bsearch_glyph(font, codepoint);
    if (g != NULL) {
        return g;
    }
    /* Single-step fallback: no recursion to avoid loops if the fallback
       itself is missing or self-referential. */
    if (font->fallback_codepoint == 0 ||
        font->fallback_codepoint == codepoint) {
        return NULL;
    }
    return bsearch_glyph(font, font->fallback_codepoint);
}

uint16_t
axl_font_advance(
    const AxlFont  *font,
    uint32_t        codepoint
    )
{
    if (font == NULL) {
        return 0;
    }
    if (font->flags & AXL_FONT_MONOSPACE) {
        return font->cell_width;
    }
    const AxlGlyph *g = axl_font_glyph(font, codepoint);
    if (g != NULL) {
        return g->advance;
    }
    return font->cell_width;  /* fallback: max advance */
}
