/** @file axl-test-gfx.c
    Unit tests for the gfx + font modules: default font, glyph lookup,
    advance computation, text measurement. Drawing primitives that
    require GOP are deliberately not exercised here — those need an
    integration test with a display, not a unit test.
**/

#include "axl-test.h"

#include <axl/axl-font.h>
#include <axl/axl-gfx.h>

/* Second built-in font, defined in src/gfx/fonts/font-unifont-16.c. */
extern const AxlFont axl_font_unifont_16;

// ---------------------------------------------------------------------------
// axl_gfx_default_font
// ---------------------------------------------------------------------------

static void
test_default_font_not_null(void)
{
    const AxlFont *f = axl_gfx_default_font();
    test_check(f != NULL,
               "default_font: returns non-NULL");
}

static void
test_default_font_stable_pointer(void)
{
    const AxlFont *f1 = axl_gfx_default_font();
    const AxlFont *f2 = axl_gfx_default_font();
    test_check(f1 == f2,
               "default_font: returns the same pointer across calls");
}

static void
test_default_font_is_edk2_laffstd(void)
{
    const AxlFont *f = axl_gfx_default_font();
    /* Pin name + dimensions: the default font is documented as EDK2
       LaffStd 8x16. If we ever change defaults, this test will flag it. */
    test_check(axl_strcmp(f->name, "edk2-laffstd") == 0,
               "default_font: name == \"edk2-laffstd\"");
    test_check(f->cell_width  == 8,    "default_font: cell_width == 8");
    test_check(f->cell_height == 16,   "default_font: cell_height == 16");
    test_check(f->ascent      == 12,   "default_font: ascent == 12");
    test_check(f->descent     == 4,    "default_font: descent == 4");
    test_check(f->line_height == 16,   "default_font: line_height == 16");
    test_check((f->flags & AXL_FONT_MONOSPACE) != 0,
               "default_font: AXL_FONT_MONOSPACE flag set");
    test_check(f->ascent + f->descent == f->cell_height,
               "default_font: ascent + descent == cell_height");
    test_check(f->n_glyphs == 95,
               "default_font: n_glyphs == 95 (printable ASCII)");
}

// ---------------------------------------------------------------------------
// axl_font_glyph (binary search + fallback)
// ---------------------------------------------------------------------------

static void
test_glyph_null_safety(void)
{
    test_check(axl_font_glyph(NULL, 'A') == NULL,
               "glyph: NULL font returns NULL");
}

static void
test_glyph_first_codepoint(void)
{
    const AxlFont  *f = axl_gfx_default_font();
    const AxlGlyph *g = axl_font_glyph(f, 0x20);  /* space — first in range */
    test_check(g != NULL,                "glyph: U+0020 found");
    test_check(g->codepoint == 0x20,     "glyph: U+0020 codepoint matches");
    test_check(g->width == 8,            "glyph: U+0020 width == 8");
    test_check(g->height == 16,          "glyph: U+0020 height == 16");
    test_check(g->advance == 8,          "glyph: U+0020 advance == 8");
}

static void
test_glyph_last_codepoint(void)
{
    const AxlFont  *f = axl_gfx_default_font();
    const AxlGlyph *g = axl_font_glyph(f, 0x7E);  /* '~' — last in range */
    test_check(g != NULL,                "glyph: U+007E found");
    test_check(g->codepoint == 0x7E,     "glyph: U+007E codepoint matches");
}

static void
test_glyph_middle_codepoint(void)
{
    const AxlFont  *f = axl_gfx_default_font();
    const AxlGlyph *g = axl_font_glyph(f, 'A');  /* exercise binary search mid */
    test_check(g != NULL,                "glyph: 'A' (U+0041) found");
    test_check(g->codepoint == 0x41,     "glyph: 'A' codepoint matches");
    /* Pin first non-zero bitmap byte — 'A' top of stem at row 2 = 0x10. */
    test_check(g->bitmap[2] == 0x10,
               "glyph: 'A' bitmap[row 2] == 0x10 (pin glyph data integrity)");
}

static void
test_glyph_below_range_uses_fallback(void)
{
    /* Codepoint below the font's supported range (0x19) falls back to
       '?' per the default font's fallback_codepoint = '?'. */
    const AxlFont  *f = axl_gfx_default_font();
    const AxlGlyph *g = axl_font_glyph(f, 0x19);
    test_check(g != NULL,             "glyph: 0x19 falls back to a glyph");
    test_check(g != NULL && g->codepoint == 0x3F,
               "glyph: 0x19 fallback glyph is '?' (U+003F)");
}

static void
test_glyph_above_range_uses_fallback(void)
{
    const AxlFont  *f = axl_gfx_default_font();
    const AxlGlyph *g7F   = axl_font_glyph(f, 0x7F);
    const AxlGlyph *gFFFF = axl_font_glyph(f, 0xFFFF);
    test_check(g7F != NULL && g7F->codepoint == 0x3F,
               "glyph: codepoint above range (0x7F) falls back to '?'");
    test_check(gFFFF != NULL && gFFFF->codepoint == 0x3F,
               "glyph: high BMP codepoint (0xFFFF) falls back to '?'");
}

// ---------------------------------------------------------------------------
// axl_font_advance
// ---------------------------------------------------------------------------

static void
test_advance_null_font(void)
{
    test_check(axl_font_advance(NULL, 'A') == 0,
               "advance: NULL font returns 0");
}

static void
test_advance_monospace_always_cell_width(void)
{
    const AxlFont *f = axl_gfx_default_font();
    test_check(axl_font_advance(f, 'A') == 8,
               "advance: monospace returns cell_width for present glyph");
    test_check(axl_font_advance(f, 0xFFFF) == 8,
               "advance: monospace returns cell_width even for missing glyph");
}

// ---------------------------------------------------------------------------
// axl_gfx_measure_text
// ---------------------------------------------------------------------------

static void
test_measure_text_null_font(void)
{
    test_check(axl_gfx_measure_text(NULL, "hello", 1) == 0,
               "measure_text: NULL font returns 0");
}

static void
test_measure_text_null_text(void)
{
    test_check(axl_gfx_measure_text(axl_gfx_default_font(), NULL, 1) == 0,
               "measure_text: NULL text returns 0");
}

static void
test_measure_text_scale_zero(void)
{
    test_check(axl_gfx_measure_text(axl_gfx_default_font(), "hello", 0) == 0,
               "measure_text: scale=0 returns 0");
}

static void
test_measure_text_empty(void)
{
    test_check(axl_gfx_measure_text(axl_gfx_default_font(), "", 1) == 0,
               "measure_text: empty string returns 0");
    test_check(axl_gfx_measure_text(axl_gfx_default_font(), "", 4) == 0,
               "measure_text: empty string returns 0 at any scale");
}

static void
test_measure_text_single_char(void)
{
    const AxlFont *f = axl_gfx_default_font();
    test_check(axl_gfx_measure_text(f, "A", 1) == 8,
               "measure_text: \"A\" at scale 1 == 8 px");
    test_check(axl_gfx_measure_text(f, "A", 2) == 16,
               "measure_text: \"A\" at scale 2 == 16 px");
}

static void
test_measure_text_multi_char(void)
{
    const AxlFont *f = axl_gfx_default_font();
    test_check(axl_gfx_measure_text(f, "hello", 1) == 40,
               "measure_text: \"hello\" at scale 1 == 5*8 == 40 px");
    test_check(axl_gfx_measure_text(f, "hello", 2) == 80,
               "measure_text: \"hello\" at scale 2 == 5*16 == 80 px");
    test_check(axl_gfx_measure_text(f, "hello", 3) == 120,
               "measure_text: \"hello\" at scale 3 == 5*24 == 120 px");
}

static void
test_measure_text_counts_nonprintable(void)
{
    /* Monospace measurement counts codepoints (not bytes).  Every
       codepoint advances by cell_width regardless of glyph presence;
       matches axl_gfx_draw_text behavior. */
    const AxlFont *f = axl_gfx_default_font();
    test_check(axl_gfx_measure_text(f, "\t", 1) == 8,
               "measure_text: tab (< 0x20) consumes one cell at scale 1");
    test_check(axl_gfx_measure_text(f, "\xC3\xBF", 1) == 8,
               "measure_text: U+00FF (2-byte UTF-8 ÿ) consumes one cell");
    test_check(axl_gfx_measure_text(f, "a\tb", 1) == 24,
               "measure_text: \"a\\tb\" == 3 codepoints * 8 == 24 px");
}

static void
test_measure_text_utf8_codepoints(void)
{
    /* Pin that measure_text walks codepoints, not bytes.  Same string
       measured as bytes (5) vs codepoints (3) yields very different
       results for monospace: bytes * 8 = 40 vs codepoints * 8 = 24. */
    const AxlFont *f = axl_gfx_default_font();
    /* "A中Z" UTF-8 = 'A' (1) + 中 (3) + 'Z' (1) = 5 bytes / 3 codepoints. */
    test_check(axl_gfx_measure_text(f, "A\xE4\xB8\xAD" "Z", 1) == 24,
               "measure_text: UTF-8 \"A中Z\" measures 3 codepoints == 24 px (NOT 5 bytes * 8)");
    /* Invalid UTF-8 (orphan continuation byte) → U+FFFD, single codepoint. */
    test_check(axl_gfx_measure_text(f, "\x80", 1) == 8,
               "measure_text: invalid UTF-8 byte 0x80 → 1 codepoint (U+FFFD)");
}

// ---------------------------------------------------------------------------
// Unifont — second built-in font, exercises wide-glyph (stride=2) path
// ---------------------------------------------------------------------------

static void
test_unifont_metadata(void)
{
    const AxlFont *f = &axl_font_unifont_16;
    test_check(axl_strcmp(f->name, "unifont-16") == 0,
               "unifont: name == \"unifont-16\"");
    test_check(f->cell_width  == 16,
               "unifont: cell_width == 16 (BDF FONTBOUNDINGBOX)");
    test_check(f->cell_height == 16,
               "unifont: cell_height == 16");
    test_check(f->ascent == 14,         "unifont: ascent == 14");
    test_check(f->descent == 2,         "unifont: descent == 2");
    test_check(f->ascent + f->descent == f->cell_height,
               "unifont: ascent + descent == cell_height invariant");
    test_check((f->flags & AXL_FONT_VARIABLE) != 0,
               "unifont: AXL_FONT_VARIABLE flag set (mix of 8/16 widths)");
    test_check(f->n_glyphs >= 100,
               "unifont: n_glyphs covers a reasonable subset (>=100)");
}

static void
test_unifont_ascii_glyph_half_width(void)
{
    const AxlFont  *f = &axl_font_unifont_16;
    const AxlGlyph *g = axl_font_glyph(f, 'A');
    test_check(g != NULL,            "unifont: 'A' present");
    test_check(g->width == 8,        "unifont: 'A' is 8-wide (half-width Latin)");
    test_check(g->advance == 8,      "unifont: 'A' advance == 8");
}

static void
test_unifont_box_drawing_present(void)
{
    const AxlFont *f = &axl_font_unifont_16;
    test_check(axl_font_glyph(f, 0x2500) != NULL,
               "unifont: U+2500 (box horizontal) present");
    test_check(axl_font_glyph(f, 0x2588) != NULL,
               "unifont: U+2588 (full block) present");
    test_check(axl_font_glyph(f, 0x2192) != NULL,
               "unifont: U+2192 (right arrow) present");
}

static void
test_unifont_cjk_glyph_full_width(void)
{
    /* Exercises the wide-glyph (stride=2) data path: CJK glyphs are 16
       pixels wide, requiring 2 bytes per row in the bitmap. */
    const AxlFont  *f = &axl_font_unifont_16;
    const AxlGlyph *g = axl_font_glyph(f, 0x4E2D);  /* 中 (middle) */
    test_check(g != NULL,            "unifont: U+4E2D (中) present");
    test_check(g->width   == 16,     "unifont: U+4E2D is 16-wide (full-width CJK)");
    test_check(g->height  == 16,     "unifont: U+4E2D is 16-tall");
    test_check(g->advance == 16,     "unifont: U+4E2D advance == 16");
    /* Pin one bitmap byte to verify the stride=2 packing.
       Row 4 of 中 is 0x3F, 0xF8 (top of the central box border). */
    test_check(g->bitmap[8]  == 0x3F,
               "unifont: U+4E2D bitmap[row 4 hi byte] == 0x3F (stride=2 packing)");
    test_check(g->bitmap[9]  == 0xF8,
               "unifont: U+4E2D bitmap[row 4 lo byte] == 0xF8 (stride=2 packing)");
}

static void
test_unifont_variable_advance(void)
{
    /* Variable-width font: axl_font_advance returns per-glyph .advance,
       not cell_width.  Half-width ASCII returns 8, full-width CJK returns 16. */
    const AxlFont *f = &axl_font_unifont_16;
    test_check(axl_font_advance(f, 'A')    == 8,
               "unifont: advance('A') == 8 (half-width)");
    test_check(axl_font_advance(f, 0x4E00) == 16,
               "unifont: advance(U+4E00 一) == 16 (full-width)");
}

static void
test_unifont_missing_glyph_falls_back_to_question(void)
{
    const AxlFont *f = &axl_font_unifont_16;
    /* Codepoint not in our subset (e.g., U+2603 SNOWMAN).  Built-in
       fonts use '?' (U+003F) as fallback so missing/invalid codepoints
       render visibly rather than disappearing. */
    const AxlGlyph *g = axl_font_glyph(f, 0x2603);
    test_check(g != NULL,
               "unifont: out-of-subset codepoint falls back to a glyph");
    test_check(g != NULL && g->codepoint == 0x3F,
               "unifont: fallback glyph is '?' (U+003F)");
    /* Self-loop guard: looking up the fallback itself must NOT recurse
       infinitely.  axl_font_glyph short-circuits if codepoint ==
       fallback_codepoint already, but verify by direct lookup. */
    test_check(axl_font_glyph(f, 0x3F) != NULL,
               "unifont: '?' itself looks up normally (no recursion)");
}

static void
test_default_font_invalid_utf8_renders_fallback(void)
{
    /* axl_utf8_decode returns U+FFFD for invalid sequences.  The font
       maps unknown codepoints to its fallback ('?').  Net effect:
       invalid UTF-8 in user input renders as visible '?', not silent
       blank — debug-friendly for a diagnostic library. */
    const AxlFont  *f = axl_gfx_default_font();
    /* Confirm fallback wiring at the lookup level. */
    const AxlGlyph *g = axl_font_glyph(f, 0xFFFD);
    test_check(g != NULL && g->codepoint == 0x3F,
               "default_font: U+FFFD falls back to '?' glyph");
    /* And that the codepoint exists for direct lookup. */
    test_check(axl_font_glyph(f, 0x3F) != NULL,
               "default_font: '?' itself looks up normally");
}

static void
test_measure_text_unifont_per_glyph_advance(void)
{
    /* Variable-width fonts: measure_text must sum per-glyph .advance,
       not (codepoint count * cell_width).  In our Unifont subset,
       U+00AD (soft hyphen) renders as a visible 16-wide placeholder
       (Unifont's convention) — distinct from neighboring 8-wide
       Latin-1 chars.  UTF-8: U+00AD = 0xC2 0xAD (2 bytes). */
    const AxlFont *f = &axl_font_unifont_16;
    test_check(axl_gfx_measure_text(f, "A", 1) == 8,
               "measure_text: unifont \"A\" == 8 (half-width)");
    test_check(axl_gfx_measure_text(f, "\xC2\xAD", 1) == 16,
               "measure_text: unifont U+00AD (16-wide placeholder, UTF-8 2-byte) == 16");
    test_check(axl_gfx_measure_text(f, "A\xC2\xAD" "B", 1) == 8 + 16 + 8,
               "measure_text: unifont mixed 'A'+U+00AD+'B' == 8+16+8 == 32 (UTF-8)");
    /* Real CJK via UTF-8: 中 = 0xE4 0xB8 0xAD = U+4E2D, 16-wide. */
    test_check(axl_gfx_measure_text(f, "\xE4\xB8\xAD", 1) == 16,
               "measure_text: unifont '中' (3-byte UTF-8, 16-wide CJK) == 16");
    test_check(axl_gfx_measure_text(f, "A\xE4\xB8\xAD" "Z", 1) == 8 + 16 + 8,
               "measure_text: unifont 'A中Z' UTF-8 (5 bytes, 3 codepoints) == 32");
    /* Pure ASCII string: 5 chars * 8 px = 40 px (all half-width). */
    test_check(axl_gfx_measure_text(f, "hello", 1) == 40,
               "measure_text: unifont ASCII \"hello\" == 5*8 == 40 px");
}

// ---------------------------------------------------------------------------
// Clipping (push/pop/get/reset/intersection math)
// ---------------------------------------------------------------------------

static void
test_clip_reset_clears_stack(void)
{
    /* Reset is safe to call repeatedly; leaves no-clip state. */
    axl_gfx_reset_clip();
    AxlGfxClip out;
    test_check(axl_gfx_get_clip(&out) == AXL_ERR,
               "clip: get_clip returns AXL_ERR after reset (no active clip)");
}

static void
test_clip_push_then_get(void)
{
    axl_gfx_reset_clip();
    AxlGfxClip c = { .x = 10, .y = 20, .w = 100, .h = 200 };
    test_check(axl_gfx_push_clip(c) == AXL_OK,
               "clip: push_clip returns AXL_OK on empty stack");

    AxlGfxClip out;
    test_check(axl_gfx_get_clip(&out) == AXL_OK,
               "clip: get_clip returns AXL_OK after push");
    test_check(out.x == 10 && out.y == 20 && out.w == 100 && out.h == 200,
               "clip: get_clip returns the pushed rect when stack was empty");

    axl_gfx_reset_clip();
}

static void
test_clip_push_intersection(void)
{
    /* Nested push computes intersection with previous top:
       (10,10,100,100) ∩ (50,50,100,100) == (50,50,60,60). */
    axl_gfx_reset_clip();
    axl_gfx_push_clip((AxlGfxClip){.x=10, .y=10, .w=100, .h=100});
    axl_gfx_push_clip((AxlGfxClip){.x=50, .y=50, .w=100, .h=100});

    AxlGfxClip out;
    axl_gfx_get_clip(&out);
    test_check(out.x == 50 && out.y == 50 && out.w == 60 && out.h == 60,
               "clip: nested push intersects (50,50,60,60)");
    axl_gfx_reset_clip();
}

static void
test_clip_pop_restores_previous(void)
{
    axl_gfx_reset_clip();
    axl_gfx_push_clip((AxlGfxClip){.x=10, .y=10, .w=100, .h=100});
    axl_gfx_push_clip((AxlGfxClip){.x=50, .y=50, .w=100, .h=100});

    test_check(axl_gfx_pop_clip() == AXL_OK,
               "clip: pop_clip returns AXL_OK with non-empty stack");

    AxlGfxClip out;
    axl_gfx_get_clip(&out);
    test_check(out.x == 10 && out.y == 10 && out.w == 100 && out.h == 100,
               "clip: pop restores previous (10,10,100,100)");

    /* Pop again → stack empty, no active clip. */
    axl_gfx_pop_clip();
    test_check(axl_gfx_get_clip(&out) == AXL_ERR,
               "clip: pop to empty leaves no active clip");
}

static void
test_clip_pop_empty_errors(void)
{
    axl_gfx_reset_clip();
    test_check(axl_gfx_pop_clip() == AXL_ERR,
               "clip: pop on empty stack returns AXL_ERR");
}

static void
test_clip_push_overflow_errors(void)
{
    axl_gfx_reset_clip();
    AxlGfxClip c = { .x = 0, .y = 0, .w = 1000, .h = 1000 };
    /* Fill the stack to capacity. */
    for (int i = 0; i < AXL_GFX_CLIP_STACK_MAX; i++) {
        test_check(axl_gfx_push_clip(c) == AXL_OK,
                   "clip: push fills stack");
    }
    /* One more should fail. */
    test_check(axl_gfx_push_clip(c) == AXL_ERR,
               "clip: push beyond AXL_GFX_CLIP_STACK_MAX returns AXL_ERR");
    axl_gfx_reset_clip();
}

static void
test_clip_empty_intersection(void)
{
    /* Disjoint rects intersect to an empty rect (w==0 or h==0). */
    axl_gfx_reset_clip();
    axl_gfx_push_clip((AxlGfxClip){.x=0,   .y=0,   .w=100, .h=100});
    axl_gfx_push_clip((AxlGfxClip){.x=200, .y=200, .w=100, .h=100});

    AxlGfxClip out;
    axl_gfx_get_clip(&out);
    test_check(out.w == 0 || out.h == 0,
               "clip: disjoint push yields empty intersection (w==0 or h==0)");
    axl_gfx_reset_clip();
}

static void
test_clip_get_null_safe(void)
{
    axl_gfx_reset_clip();
    test_check(axl_gfx_get_clip(NULL) == AXL_ERR,
               "clip: get_clip(NULL) returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// Off-screen buffers (double-buffering)
// ---------------------------------------------------------------------------

static void
test_buffer_new_basic(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 50);
    test_check(b != NULL, "buffer_new: 100x50 returns non-NULL");
    test_check(axl_gfx_buffer_pixels(b) != NULL,
               "buffer_new: pixels accessor returns non-NULL");
    uint32_t w = 0, h = 0;
    axl_gfx_buffer_get_info(b, &w, &h);
    test_check(w == 100 && h == 50,
               "buffer_new: dimensions match constructor args");
    axl_gfx_buffer_free(b);
}

static void
test_buffer_new_zero_dim_returns_null(void)
{
    test_check(axl_gfx_buffer_new(0, 10)  == NULL,
               "buffer_new(0, 10): zero width returns NULL");
    test_check(axl_gfx_buffer_new(10, 0)  == NULL,
               "buffer_new(10, 0): zero height returns NULL");
}

static void
test_buffer_free_null_safe(void)
{
    axl_gfx_buffer_free(NULL);   /* must not crash */
    test_check(true, "buffer_free: NULL is safe (no crash)");
}

static void
test_buffer_get_info_null_safe(void)
{
    uint32_t w = 999, h = 999;
    test_check(axl_gfx_buffer_get_info(NULL, &w, &h) == AXL_ERR,
               "buffer_get_info(NULL): returns AXL_ERR");
    /* NULL out pointers are individually optional. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(50, 25);
    test_check(axl_gfx_buffer_get_info(b, NULL, NULL) == AXL_OK,
               "buffer_get_info: both out NULL is OK (call succeeds)");
    test_check(axl_gfx_buffer_get_info(b, &w, NULL) == AXL_OK && w == 50,
               "buffer_get_info: out_h NULL is OK; out_w gets width");
    test_check(axl_gfx_buffer_get_info(b, NULL, &h) == AXL_OK && h == 25,
               "buffer_get_info: out_w NULL is OK; out_h gets height");
    axl_gfx_buffer_free(b);
}

static void
test_buffer_clear_fills_all_pixels(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(8, 4);
    AxlGfxPixel  red = {0x00, 0x00, 0xFF, 0xFF};
    test_check(axl_gfx_buffer_clear(b, red) == AXL_OK,
               "buffer_clear: returns AXL_OK");
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    bool all_red = true;
    for (uint32_t i = 0; i < 8 * 4; i++) {
        if (p[i].blue != 0x00 || p[i].green != 0x00 ||
            p[i].red != 0xFF) {
            all_red = false;
            break;
        }
    }
    test_check(all_red, "buffer_clear: every pixel (32 total) is red");
    axl_gfx_buffer_free(b);
}

static void
test_buffer_clear_null_safe(void)
{
    AxlGfxPixel  red = {0x00, 0x00, 0xFF, 0xFF};
    test_check(axl_gfx_buffer_clear(NULL, red) == AXL_ERR,
               "buffer_clear(NULL): returns AXL_ERR");
}

static void
test_buffer_pixels_null_safe(void)
{
    test_check(axl_gfx_buffer_pixels(NULL) == NULL,
               "buffer_pixels(NULL): returns NULL");
}

static void
test_buffer_pixels_writable(void)
{
    /* Pixels accessor returns a stable, writable pointer.  Write a
       known pattern, read it back. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 2);
    AxlGfxPixel  *p = axl_gfx_buffer_pixels(b);
    for (uint32_t i = 0; i < 4 * 2; i++) {
        p[i].blue     = (uint8_t)i;
        p[i].green    = (uint8_t)(i + 1);
        p[i].red      = (uint8_t)(i + 2);
        p[i].alpha    = 0xFF;
    }
    /* Second call returns the same pointer (no allocation). */
    test_check(axl_gfx_buffer_pixels(b) == p,
               "buffer_pixels: returns the same pointer across calls");
    /* Round-trip: pattern preserved. */
    test_check(p[5].blue == 5 && p[5].green == 6 && p[5].red == 7,
               "buffer_pixels: writes are visible via subsequent reads");
    axl_gfx_buffer_free(b);
}

static void
test_target_buffer_redirects_fill(void)
{
    /* fill_rect routed to a buffer target writes pixels into the
       buffer (no GOP required).  Verify by reading the buffer back. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(2, 2, 5, 5, blue);
    axl_gfx_target_buffer(NULL);     /* restore screen target */

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* Pixel (3,3) — inside the rect — should be blue. */
    test_check(p[3 * 10 + 3].blue == 0xFF && p[3 * 10 + 3].red == 0,
               "target_buffer: fill_rect writes inside rect (3,3) blue");
    /* Pixel (0,0) — outside the rect — should remain bg (black). */
    test_check(p[0].blue == 0 && p[0].green == 0 && p[0].red == 0,
               "target_buffer: fill_rect leaves outside-rect (0,0) untouched");
    /* Pixel (6,6) — last pixel inside rect (x=2..6, y=2..6) — blue. */
    test_check(p[6 * 10 + 6].blue == 0xFF,
               "target_buffer: fill_rect right-edge (6,6) is blue (off-by-one guard)");
    /* Pixel (7,7) — just outside the 5x5 rect at (2,2) — bg. */
    test_check(p[7 * 10 + 7].blue == 0,
               "target_buffer: fill_rect respects width (7,7) untouched");
    axl_gfx_buffer_free(b);
}

static void
test_target_buffer_honors_clip(void)
{
    /* Push a clip that restricts to (5,5,5,5).  A fill of (0,0,10,10)
       should only touch pixels inside the clip. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   green = {0x00, 0xFF, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip((AxlGfxClip){.x = 5, .y = 5, .w = 5, .h = 5});
    axl_gfx_fill_rect(0, 0, 10, 10, green);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* (0,0) outside clip → bg. */
    test_check(p[0].green == 0,
               "target_buffer + clip: (0,0) outside clip stays bg");
    /* (4,4) outside clip → bg. */
    test_check(p[4 * 10 + 4].green == 0,
               "target_buffer + clip: (4,4) just outside clip stays bg");
    /* (7,7) inside clip → green. */
    test_check(p[7 * 10 + 7].green == 0xFF,
               "target_buffer + clip: (7,7) inside clip gets green");
    axl_gfx_buffer_free(b);
}

static void
test_get_current_target_default_is_null(void)
{
    /* With no buffer set, screen rendering is the default — query
       must return NULL.  Reset first so prior tests don't leak. */
    axl_gfx_target_buffer(NULL);
    test_check(axl_gfx_get_current_target() == NULL,
               "get_current_target: default (screen) returns NULL");
}

static void
test_get_current_target_roundtrips_set(void)
{
    /* Setting a buffer and querying must return the same pointer. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    axl_gfx_target_buffer(b);
    test_check(axl_gfx_get_current_target() == b,
               "get_current_target: returns the set buffer");
    axl_gfx_target_buffer(NULL);
    test_check(axl_gfx_get_current_target() == NULL,
               "get_current_target: returns NULL after reset");
    axl_gfx_buffer_free(b);
}

static void
test_get_current_target_cleared_on_buffer_free(void)
{
    /* axl_gfx_buffer_free already drops target_buf if it's pointing
       at the freed buffer; verify the query reflects that. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    axl_gfx_target_buffer(b);
    test_check(axl_gfx_get_current_target() == b,
               "get_current_target: target set before free");
    axl_gfx_buffer_free(b);
    test_check(axl_gfx_get_current_target() == NULL,
               "get_current_target: cleared after target buffer freed");
}

// ---------------------------------------------------------------------------
// Alpha blending (axl_gfx_blend) + alpha-aware fill_rect on buffer
// ---------------------------------------------------------------------------

static void
test_blend_alpha_full_opaque_yields_src(void)
{
    AxlGfxPixel dst = {0x00, 0x00, 0x00, 0xFF};        /* black */
    AxlGfxPixel src = {0xFF, 0xFF, 0xFF, 0xFF};        /* white, opaque */
    AxlGfxPixel out = axl_gfx_blend(dst, src);
    test_check(out.blue == 0xFF && out.green == 0xFF && out.red == 0xFF,
               "blend: alpha=0xFF returns src color exactly");
    test_check(out.alpha == 0xFF,
               "blend: result alpha is always 0xFF (opaque dst convention)");
}

static void
test_blend_alpha_zero_yields_dst(void)
{
    AxlGfxPixel dst = {0x10, 0x20, 0x30, 0xFF};
    AxlGfxPixel src = {0xFF, 0xFF, 0xFF, 0x00};        /* white, transparent */
    AxlGfxPixel out = axl_gfx_blend(dst, src);
    test_check(out.blue == 0x10 && out.green == 0x20 && out.red == 0x30,
               "blend: alpha=0 returns dst color unchanged");
}

static void
test_blend_alpha_half(void)
{
    /* alpha = 128 (~50.2%) blending black + white = (128*255 + 127*0 + 127)/255
       = (32640 + 127)/255 = 32767/255 = 128.499... → 128 in int math.
       Exact rounded value: 128.  Allow ±1 for rounding mode tolerance. */
    AxlGfxPixel dst = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel src = {0xFF, 0xFF, 0xFF, 0x80};        /* white, ~half-alpha */
    AxlGfxPixel out = axl_gfx_blend(dst, src);
    test_check(out.blue >= 127 && out.blue <= 129,
               "blend: alpha=0x80 white over black ≈ 128 (±1)");
    test_check(out.green == out.blue && out.red == out.blue,
               "blend: alpha=0x80 white over black is gray (R=G=B)");
}

static void
test_blend_per_channel_independent(void)
{
    /* Different RGB values in src and dst — blending should preserve
       channel independence. */
    AxlGfxPixel dst = {0xFF, 0x00, 0x00, 0xFF};        /* pure blue */
    AxlGfxPixel src = {0x00, 0xFF, 0x00, 0x80};        /* green, half */
    AxlGfxPixel out = axl_gfx_blend(dst, src);
    /* blue: (0*128 + 255*127 + 127)/255 = 32512/255 = ~127 */
    /* green: (255*128 + 0*127 + 127)/255 = 32767/255 = ~128 */
    /* red:   0 (both inputs 0) */
    test_check(out.blue >= 126 && out.blue <= 128,
               "blend: half-green-over-blue keeps half of original blue");
    test_check(out.green >= 127 && out.green <= 129,
               "blend: half-green-over-blue contributes half new green");
    test_check(out.red == 0,
               "blend: half-green-over-blue red channel stays 0");
}

static void
test_fill_rect_alpha_on_buffer_blends(void)
{
    /* Fill the buffer with red, then fill a sub-rect with half-alpha
       blue.  Sub-rect pixels should be (255, 0, 127) — half-blue
       blended with half-red.  Outside sub-rect stays pure red. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel  red       = {0x00, 0x00, 0xFF, 0xFF};
    AxlGfxPixel  half_blue = {0xFF, 0x00, 0x00, 0x80};
    axl_gfx_buffer_clear(b, red);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(2, 2, 5, 5, half_blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* (3,3) inside sub-rect: half red + half blue → (127, 0, 127) ±1. */
    AxlGfxPixel inside = p[3 * 10 + 3];
    test_check(inside.blue >= 126 && inside.blue <= 128,
               "fill_rect alpha: inside sub-rect blue ≈ 127");
    test_check(inside.red >= 126 && inside.red <= 128,
               "fill_rect alpha: inside sub-rect red ≈ 127 (half original)");
    test_check(inside.green == 0,
               "fill_rect alpha: inside sub-rect green channel stays 0");
    /* (0,0) outside sub-rect: still pure red. */
    AxlGfxPixel outside = p[0];
    test_check(outside.red == 0xFF && outside.blue == 0,
               "fill_rect alpha: outside sub-rect untouched (still red)");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_alpha_zero_is_noop(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel  red          = {0x00, 0x00, 0xFF, 0xFF};
    AxlGfxPixel  transparent  = {0xFF, 0xFF, 0xFF, 0x00};
    axl_gfx_buffer_clear(b, red);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(2, 2, 5, 5, transparent);
    axl_gfx_target_buffer(NULL);

    /* Spot-check (3,3) — should still be red.  Note: the actual
       regression guard for "no-op" is the alpha==0 short-circuit in
       axl_gfx_fill_rect's buffer branch; the blend math alone also
       happens to produce the same pixel value for alpha=0 (since
       blend(dst, alpha=0) = dst exactly), so this single-pixel check
       wouldn't distinguish a missing short-circuit from a correct
       blend.  Both paths are correct, so behavior is right either
       way; the short-circuit is purely a perf optimization. */
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(p[3 * 10 + 3].red == 0xFF && p[3 * 10 + 3].blue == 0,
               "fill_rect alpha=0: pixel unchanged (red)");
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// axl_gfx_fill_rect_i (signed-coord variant)
// ---------------------------------------------------------------------------

/* Helper: pixel at (col, row) of a w-wide buffer matches `expected`. */
static bool
px_eq(const AxlGfxPixel *p, uint32_t stride, uint32_t col, uint32_t row,
      AxlGfxPixel expected)
{
    const AxlGfxPixel *q = &p[row * stride + col];
    return q->blue == expected.blue
        && q->green == expected.green
        && q->red == expected.red;
}

static void
test_fill_rect_i_positive_parity(void)
{
    /* Positive coords: signed variant must produce the same pixels as
     * the unsigned variant.  Inside-rect blue, outside-rect bg. */
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect_i(2, 2, 5, 5, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 10, 3, 3, blue),
               "fill_rect_i: positive (2,2,5,5) — inside (3,3) blue");
    test_check(px_eq(p, 10, 6, 6, blue),
               "fill_rect_i: positive (2,2,5,5) — right-edge (6,6) blue");
    test_check(px_eq(p, 10, 7, 7, bg),
               "fill_rect_i: positive (2,2,5,5) — outside (7,7) bg");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_i_negative_x_clamped(void)
{
    /* x=-2, w=4 → visible columns 0..1 (2 pixels wide). */
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect_i(-2, 3, 4, 2, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 10, 0, 3, blue),
               "fill_rect_i: x=-2 — col 0 blue");
    test_check(px_eq(p, 10, 1, 3, blue),
               "fill_rect_i: x=-2 — col 1 blue");
    test_check(px_eq(p, 10, 2, 3, bg),
               "fill_rect_i: x=-2 — col 2 untouched (beyond clamped width)");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_i_negative_y_clamped(void)
{
    /* y=-3, h=5 → visible rows 0..1. */
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect_i(3, -3, 2, 5, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 10, 3, 0, blue),
               "fill_rect_i: y=-3 — row 0 blue");
    test_check(px_eq(p, 10, 3, 1, blue),
               "fill_rect_i: y=-3 — row 1 blue");
    test_check(px_eq(p, 10, 3, 2, bg),
               "fill_rect_i: y=-3 — row 2 untouched");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_i_zero_w_noop(void)
{
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int ret = axl_gfx_fill_rect_i(2, 2, 0, 5, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(ret == AXL_OK,
               "fill_rect_i: w=0 — returns AXL_OK");
    test_check(px_eq(p, 10, 2, 2, bg),
               "fill_rect_i: w=0 — buffer unchanged at (2,2)");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_i_zero_h_noop(void)
{
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int ret = axl_gfx_fill_rect_i(2, 2, 5, 0, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(ret == AXL_OK,
               "fill_rect_i: h=0 — returns AXL_OK");
    test_check(px_eq(p, 10, 2, 2, bg),
               "fill_rect_i: h=0 — buffer unchanged at (2,2)");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_i_negative_w_noop(void)
{
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int ret = axl_gfx_fill_rect_i(2, 2, -3, 5, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(ret == AXL_OK,
               "fill_rect_i: w=-3 — returns AXL_OK");
    test_check(px_eq(p, 10, 2, 2, bg),
               "fill_rect_i: w=-3 — buffer unchanged at (2,2)");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_i_negative_h_noop(void)
{
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int ret = axl_gfx_fill_rect_i(2, 2, 5, -3, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(ret == AXL_OK,
               "fill_rect_i: h=-3 — returns AXL_OK");
    test_check(px_eq(p, 10, 2, 2, bg),
               "fill_rect_i: h=-3 — buffer unchanged at (2,2)");
    axl_gfx_buffer_free(b);
}

static void
test_fill_rect_i_fully_off_top_left(void)
{
    /* x=-10, y=-10, w=3, h=3 — rect ends at (-7,-7), entirely off-
     * screen to the top-left.  Clamp leaves zero visible area. */
    AxlGfxBuffer *b    = axl_gfx_buffer_new(10, 10);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blue = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int ret = axl_gfx_fill_rect_i(-10, -10, 3, 3, blue);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(ret == AXL_OK,
               "fill_rect_i: fully off top-left — returns AXL_OK");
    test_check(px_eq(p, 10, 0, 0, bg),
               "fill_rect_i: fully off top-left — (0,0) untouched");
    test_check(px_eq(p, 10, 5, 5, bg),
               "fill_rect_i: fully off top-left — interior untouched");
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// G3 — AxlGfxPath + fill_path + fill_rounded_rect
// ---------------------------------------------------------------------------

static size_t
count_non_bg_in_rect(
    AxlGfxBuffer *b,
    uint32_t x, uint32_t y,
    uint32_t w, uint32_t h,
    AxlGfxPixel bg
    )
{
    uint32_t bw = 0, bh = 0;
    axl_gfx_buffer_get_info(b, &bw, &bh);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    size_t n = 0;
    for (uint32_t py = y; py < y + h && py < bh; py++) {
        for (uint32_t px = x; px < x + w && px < bw; px++) {
            const AxlGfxPixel *q = &p[py * bw + px];
            if (q->blue != bg.blue || q->green != bg.green || q->red != bg.red) {
                n++;
            }
        }
    }
    return n;
}

static void
test_path_new_returns_non_null(void)
{
    AxlGfxPath *p = axl_gfx_path_new();
    test_check(p != NULL, "path_new: returns non-NULL");
    axl_gfx_path_free(p);
}

static void
test_path_free_null_safe(void)
{
    /* Call path_free(NULL); if it crashed we wouldn't reach the
     * subsequent allocation.  Assert that path_new still works
     * after a NULL-free — proves the free didn't corrupt anything
     * recoverable AND avoids the no-op tautology pattern. */
    axl_gfx_path_free(NULL);
    AxlGfxPath *p = axl_gfx_path_new();
    test_check(p != NULL,
               "path_free: NULL doesn't disrupt subsequent path_new");
    axl_gfx_path_free(p);
}

static void
test_fill_path_null_returns_err(void)
{
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    test_check(axl_gfx_fill_path(NULL, red) == AXL_ERR,
               "fill_path: NULL path returns AXL_ERR");
}

static void
test_fill_path_empty_returns_err(void)
{
    AxlGfxPath *p = axl_gfx_path_new();
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    test_check(axl_gfx_fill_path(p, red) == AXL_ERR,
               "fill_path: empty path returns AXL_ERR");
    axl_gfx_path_free(p);
}

static void
test_fill_path_rectangle(void)
{
    /* Build a 6x6 rectangle via move/line/close and fill it.
     * Verify the rectangle interior is fully filled with red. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 4.0f, 4.0f);
    axl_gfx_path_line_to(p, 10.0f, 4.0f);
    axl_gfx_path_line_to(p, 10.0f, 10.0f);
    axl_gfx_path_line_to(p, 4.0f, 10.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_path(p, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_path: rectangle returns AXL_OK");
    /* Center pixel of rect — fully inside.  All 16 supersamples
     * land inside the rect, so coverage = 16 → modulated alpha
     * = 0xFF → opaque red overwrites bg. */
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[7 * 20 + 7].red == 0xFF,
               "fill_path: rect interior (7,7) is full-coverage red");
    /* Far outside the rect — unchanged. */
    test_check(px[0].red == 0 && px[0].green == 0 && px[0].blue == 0,
               "fill_path: (0,0) outside rect untouched");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_fill_path_triangle(void)
{
    /* Triangle: (5,5), (15,5), (10,15).  Fill, expect modified
     * pixels in the triangle bbox. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 5.0f,  5.0f);
    axl_gfx_path_line_to(p, 15.0f, 5.0f);
    axl_gfx_path_line_to(p, 10.0f, 15.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_path(p, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_path: triangle returns AXL_OK");
    /* Centroid is approximately (10, 8) — should be filled. */
    size_t inside = count_non_bg_in_rect(b, 8, 7, 4, 3, bg);
    test_check(inside > 0,
               "fill_path: triangle centroid region modified");
    /* Far corner is outside the triangle — should be untouched. */
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[0].red == 0,
               "fill_path: (0,0) outside triangle untouched");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_fill_rounded_rect_zero_radius_is_plain(void)
{
    /* Radius 0 should be indistinguishable from fill_rect_i. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rounded_rect(3, 3, 8, 8, 0.0f, red);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    /* All 64 interior pixels should be red. */
    size_t red_count = 0;
    for (uint32_t py = 3; py < 11; py++) {
        for (uint32_t pxx = 3; pxx < 11; pxx++) {
            if (px[py * 20 + pxx].red > 0) {
                red_count++;
            }
        }
    }
    test_check(red_count == 64,
               "fill_rounded_rect: radius 0 fills all 64 interior pixels");
    /* Corner (3, 3) should be filled (not rounded off). */
    test_check(px[3 * 20 + 3].red > 0,
               "fill_rounded_rect: radius 0 leaves corner pixel filled");

    axl_gfx_buffer_free(b);
}

static void
test_fill_rounded_rect_corners_not_filled(void)
{
    /* 20x20 rect at (2, 2) with radius 6.  The four extreme corners
     * (2,2), (21,2), (2,21), (21,21) should NOT be filled (the
     * curvature rounds them off).  Center pixel should be filled. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(30, 30);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rounded_rect(2, 2, 20, 20, 6.0f, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_rounded_rect: returns AXL_OK");
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);

    /* Corner pixels of the bounding rect should be background. */
    test_check(px[2 * 30 + 2].red == 0,
               "fill_rounded_rect: TL corner (2,2) is bg (rounded off)");
    test_check(px[2 * 30 + 21].red == 0,
               "fill_rounded_rect: TR corner (21,2) is bg");
    test_check(px[21 * 30 + 2].red == 0,
               "fill_rounded_rect: BL corner (2,21) is bg");
    test_check(px[21 * 30 + 21].red == 0,
               "fill_rounded_rect: BR corner (21,21) is bg");
    /* Center pixel of rect (12, 12) should be filled. */
    test_check(px[12 * 30 + 12].red > 0,
               "fill_rounded_rect: center pixel (12,12) is filled");

    axl_gfx_buffer_free(b);
}

static void
test_fill_rounded_rect_zero_w_noop(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rounded_rect(2, 2, 0, 10, 3.0f, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_rounded_rect: w=0 returns AXL_OK");
    size_t modified = count_non_bg_in_rect(b, 0, 0, 20, 20, bg);
    test_check(modified == 0,
               "fill_rounded_rect: w=0 buffer unchanged");

    axl_gfx_buffer_free(b);
}

static void
test_fill_rounded_rect_negative_h_noop(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rounded_rect(2, 2, 10, -5, 3.0f, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_rounded_rect: h=-5 returns AXL_OK");
    size_t modified = count_non_bg_in_rect(b, 0, 0, 20, 20, bg);
    test_check(modified == 0,
               "fill_rounded_rect: h=-5 buffer unchanged");

    axl_gfx_buffer_free(b);
}

static void
test_stroke_path_null_returns_err(void)
{
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    test_check(axl_gfx_stroke_path(NULL, red, 1.0f) == AXL_ERR,
               "stroke_path: NULL path returns AXL_ERR");
}

static void
test_fill_path_curve_subdivides(void)
{
    /* C1 — cubic Bezier curve from (3,12) up to (15,12) with
     * control points pulling up.  Close back to start, fill.
     * Pixels in the bowed-arch region should be modified. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p,  3.0f, 12.0f);
    axl_gfx_path_curve_to(p, 7.0f,  3.0f, 11.0f, 3.0f, 15.0f, 12.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_path(p, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_path: curve_to subdivides + returns AXL_OK");
    /* Center of the bowed-arch (~9, 8) should be inside the
     * filled region. */
    size_t inside = count_non_bg_in_rect(b, 7, 7, 5, 4, bg);
    test_check(inside > 0,
               "fill_path: curve interior region modified");
    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_fill_path_arc_subdivides(void)
{
    /* C2 — arc with sweep > π (270°).  Regression for the chord-
     * through-center collapse bug. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(30, 30);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    /* 270° arc centered at (15,15) radius 8, sweeping from 0 to
     * 3π/2 ≈ 4.712.  Close the shape via line to chord+center. */
    axl_gfx_path_arc(p, 15.0f, 15.0f, 8.0f, 0.0f, 4.712389f);
    axl_gfx_path_line_to(p, 15.0f, 15.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_path(p, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_path: arc path returns AXL_OK");
    /* Right of center (e.g., 20, 15) is inside the arc sweep. */
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[15 * 30 + 20].red > 0,
               "fill_path: pixel inside 270° arc sweep is filled");
    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_fill_rounded_rect_negative_coords_clipped(void)
{
    /* C3 — rounded rect at negative origin partly clipped against
     * (0,0).  Visible portion in the buffer should be drawn. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rounded_rect(-3, -3, 10, 10, 3.0f, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_rounded_rect: negative coords returns AXL_OK");
    size_t visible = count_non_bg_in_rect(b, 0, 0, 7, 7, bg);
    test_check(visible > 0,
               "fill_rounded_rect: visible portion modified despite negative origin");
    axl_gfx_buffer_free(b);
}

static void
test_fill_path_multi_subpath_even_odd(void)
{
    /* C4 — outer square + inner square subpath; even-odd rule
     * should leave the inner region as a hole (unfilled) inside
     * the outer filled region. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    /* Outer 10x10 at (4,4)..(14,14). */
    axl_gfx_path_move_to(p,  4.0f,  4.0f);
    axl_gfx_path_line_to(p, 14.0f,  4.0f);
    axl_gfx_path_line_to(p, 14.0f, 14.0f);
    axl_gfx_path_line_to(p,  4.0f, 14.0f);
    axl_gfx_path_close(p);
    /* Inner 4x4 hole at (8,8)..(12,12). */
    axl_gfx_path_move_to(p,  8.0f,  8.0f);
    axl_gfx_path_line_to(p, 12.0f,  8.0f);
    axl_gfx_path_line_to(p, 12.0f, 12.0f);
    axl_gfx_path_line_to(p,  8.0f, 12.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_path(p, red);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "fill_path: multi-subpath returns AXL_OK");
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    /* (6, 6) is inside outer, outside inner — fully covered (full red). */
    test_check(px[6 * 20 + 6].red == 0xFF,
               "fill_path: outer-ring pixel (6,6) filled");
    /* (10, 10) is inside the inner hole — should remain bg. */
    test_check(px[10 * 20 + 10].red == 0,
               "fill_path: inner-hole pixel (10,10) is bg (even-odd)");
    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_stroke_path_rectangle_outlines(void)
{
    /* Build a 4x4 rectangle, stroke it.  Verify outline pixels
     * are modified and the interior is NOT. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 5.0f, 5.0f);
    axl_gfx_path_line_to(p, 9.0f, 5.0f);
    axl_gfx_path_line_to(p, 9.0f, 9.0f);
    axl_gfx_path_line_to(p, 5.0f, 9.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_stroke_path(p, red, 1.0f);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "stroke_path: returns AXL_OK");
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    /* Top edge midpoint (7, 5) should be on the stroke. */
    test_check(px[5 * 20 + 7].red > 0,
               "stroke_path: top-edge pixel modified");
    /* Interior pixel (7, 7) should be untouched (only outline drawn). */
    test_check(px[7 * 20 + 7].red == 0,
               "stroke_path: interior (7,7) untouched");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// Line / rect-outline / polyline drawing (Phase 0e)
// ---------------------------------------------------------------------------

static void
test_draw_line_horizontal_on_buffer(void)
{
    /* Horizontal line from (1,3) to (8,3) on a 10×10 buffer.
       Bresenham should hit pixels x=1..8 on row 3. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    axl_gfx_buffer_clear(b, AXL_GFX_BLACK);

    axl_gfx_target_buffer(b);
    axl_gfx_draw_line(1, 3, 8, 3, AXL_GFX_RED);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* Endpoints inclusive: x=1 and x=8 on row 3 should be red. */
    test_check(p[3 * 10 + 1].red == 0xFF,
               "draw_line H: left endpoint (1,3) is red");
    test_check(p[3 * 10 + 8].red == 0xFF,
               "draw_line H: right endpoint (8,3) is red");
    test_check(p[3 * 10 + 5].red == 0xFF,
               "draw_line H: middle pixel (5,3) is red");
    /* Off-line pixels untouched. */
    test_check(p[3 * 10 + 0].red == 0,
               "draw_line H: x=0 (off line) untouched");
    test_check(p[2 * 10 + 5].red == 0,
               "draw_line H: row above untouched");
    test_check(p[4 * 10 + 5].red == 0,
               "draw_line H: row below untouched");
    axl_gfx_buffer_free(b);
}

static void
test_draw_line_vertical_on_buffer(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    axl_gfx_buffer_clear(b, AXL_GFX_BLACK);

    axl_gfx_target_buffer(b);
    axl_gfx_draw_line(4, 1, 4, 7, AXL_GFX_GREEN);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(p[1 * 10 + 4].green == 0xFF,
               "draw_line V: top endpoint (4,1) is green");
    test_check(p[7 * 10 + 4].green == 0xFF,
               "draw_line V: bottom endpoint (4,7) is green");
    test_check(p[4 * 10 + 4].green == 0xFF,
               "draw_line V: middle (4,4) is green");
    test_check(p[4 * 10 + 3].green == 0,
               "draw_line V: x=3 (off line) untouched");
    axl_gfx_buffer_free(b);
}

static void
test_draw_line_diagonal_on_buffer(void)
{
    /* 45° diagonal from (0,0) to (5,5): pixels (0,0),(1,1),...,(5,5). */
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    axl_gfx_buffer_clear(b, AXL_GFX_BLACK);

    axl_gfx_target_buffer(b);
    axl_gfx_draw_line(0, 0, 5, 5, AXL_GFX_BLUE);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    for (int i = 0; i <= 5; i++) {
        test_check(p[(size_t)i * 10 + i].blue == 0xFF,
                   "draw_line diag: (i,i) on diagonal is blue");
    }
    test_check(p[1 * 10 + 0].blue == 0,
               "draw_line diag: off-diagonal (0,1) untouched");
    axl_gfx_buffer_free(b);
}

static void
test_draw_line_single_point(void)
{
    /* x0 == x1 and y0 == y1: should still write the single pixel. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    axl_gfx_buffer_clear(b, AXL_GFX_BLACK);

    axl_gfx_target_buffer(b);
    axl_gfx_draw_line(5, 5, 5, 5, AXL_GFX_WHITE);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(p[5 * 10 + 5].red == 0xFF,
               "draw_line: single-point line writes one pixel");
    axl_gfx_buffer_free(b);
}

static void
test_draw_line_off_buffer_clipped(void)
{
    /* Line extending off-buffer should be silently clipped — no crash. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    axl_gfx_buffer_clear(b, AXL_GFX_BLACK);

    axl_gfx_target_buffer(b);
    /* Diagonal from (-5,-5) to (15,15) crosses the buffer through
       (0,0) to (9,9) inclusive — those pixels should be set. */
    axl_gfx_draw_line(-5, -5, 15, 15, AXL_GFX_WHITE);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(p[0].red == 0xFF, "draw_line off-buf: (0,0) visible");
    test_check(p[9 * 10 + 9].red == 0xFF,
               "draw_line off-buf: (9,9) visible");
    axl_gfx_buffer_free(b);
}

static void
test_draw_rect_outline_on_buffer(void)
{
    /* draw_rect(2, 2, 6, 4) outlines a 6-wide 4-tall rect with corners
       at (2,2), (7,2), (2,5), (7,5).  Interior pixels (3,3),(4,3) etc.
       remain bg. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    axl_gfx_buffer_clear(b, AXL_GFX_BLACK);

    axl_gfx_target_buffer(b);
    axl_gfx_draw_rect(2, 2, 6, 4, AXL_GFX_YELLOW);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* Yellow = R=FF, G=FF (BGRA: 0,FF,FF,FF). Use red as cheap detector. */
    /* Corners. */
    test_check(p[2 * 10 + 2].red == 0xFF,
               "draw_rect: top-left corner (2,2) is yellow");
    test_check(p[2 * 10 + 7].red == 0xFF,
               "draw_rect: top-right corner (7,2) is yellow");
    test_check(p[5 * 10 + 2].red == 0xFF,
               "draw_rect: bottom-left corner (2,5) is yellow");
    test_check(p[5 * 10 + 7].red == 0xFF,
               "draw_rect: bottom-right corner (7,5) is yellow");
    /* Edge midpoints. */
    test_check(p[2 * 10 + 4].red == 0xFF,
               "draw_rect: top edge mid (4,2) is yellow");
    test_check(p[5 * 10 + 4].red == 0xFF,
               "draw_rect: bottom edge mid (4,5) is yellow");
    /* Interior untouched. */
    test_check(p[3 * 10 + 4].red == 0,
               "draw_rect: interior (4,3) untouched (outline only)");
    test_check(p[4 * 10 + 5].red == 0,
               "draw_rect: interior (5,4) untouched");
    /* Outside untouched. */
    test_check(p[0].red == 0,
               "draw_rect: outside (0,0) untouched");
    axl_gfx_buffer_free(b);
}

static void
test_draw_polyline_connects_points(void)
{
    /* Polyline through (1,1),(8,1),(8,8) — an L shape — should hit
       both segments. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(10, 10);
    axl_gfx_buffer_clear(b, AXL_GFX_BLACK);

    AxlGfxPoint pts[] = {
        {1, 1}, {8, 1}, {8, 8},
    };

    axl_gfx_target_buffer(b);
    axl_gfx_draw_polyline(pts, 3, AXL_GFX_MAGENTA);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* Horizontal segment (1..8, 1). */
    test_check(p[1 * 10 + 4].red == 0xFF,
               "draw_polyline: horizontal segment hit at (4,1)");
    test_check(p[1 * 10 + 8].red == 0xFF,
               "draw_polyline: corner (8,1) hit");
    /* Vertical segment (8, 1..8). */
    test_check(p[5 * 10 + 8].red == 0xFF,
               "draw_polyline: vertical segment hit at (8,5)");
    test_check(p[8 * 10 + 8].red == 0xFF,
               "draw_polyline: final endpoint (8,8) hit");
    axl_gfx_buffer_free(b);
}

static void
test_draw_polyline_null_safety(void)
{
    test_check(axl_gfx_draw_polyline(NULL, 5, AXL_GFX_RED) == AXL_ERR,
               "draw_polyline: NULL points returns AXL_ERR");
    AxlGfxPoint p[] = {{0, 0}};
    test_check(axl_gfx_draw_polyline(p, 1, AXL_GFX_RED) == AXL_ERR,
               "draw_polyline: count < 2 returns AXL_ERR");
    test_check(axl_gfx_draw_polyline(p, 0, AXL_GFX_RED) == AXL_ERR,
               "draw_polyline: count == 0 returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// Transform stack — Phase G4
// ---------------------------------------------------------------------------

static bool
near_double_(double actual, double expected, double tol)
{
    double d = actual - expected;
    if (d < 0) d = -d;
    return d <= tol;
}

static bool
mat3_near_(AxlMat3 a, AxlMat3 b, double tol)
{
    for (int i = 0; i < 9; i++) {
        if (!near_double_(a.m[i], b.m[i], tol)) {
            return false;
        }
    }
    return true;
}

static void
test_transform_default_identity(void)
{
    axl_gfx_reset_transform();
    test_check(mat3_near_(axl_gfx_get_transform(),
                          axl_mat3_identity(), 1e-15),
               "transform: starts as identity after reset");
}

static void
test_transform_translate_composes(void)
{
    axl_gfx_reset_transform();
    axl_gfx_translate(10.0, 20.0);
    AxlMat3 M = axl_gfx_get_transform();
    AxlVec2 t = axl_mat3_transform_point(M, axl_vec2(0.0, 0.0));
    test_check(t.x == 10.0 && t.y == 20.0,
               "transform: translate(10,20) maps origin → (10,20)");
    /* Compose: another translate adds. */
    axl_gfx_translate(5.0, 0.0);
    t = axl_mat3_transform_point(axl_gfx_get_transform(),
                                 axl_vec2(0.0, 0.0));
    test_check(t.x == 15.0 && t.y == 20.0,
               "transform: translates compose additively");
    axl_gfx_reset_transform();
}

static void
test_transform_scale_then_translate(void)
{
    axl_gfx_reset_transform();
    /* HTML canvas convention: each new op right-multiplies onto the
     * current transform.  After scale(2,1) + translate(10,0), the
     * effective mapping is "first translate, then scale":
     *   local (1, 0) → translate → (11, 0) → scale → (22, 0).
     * Drawing a unit rect at local (0, 0) appears 2 units wide at
     * world (20, 0).  Matches HTML canvas / Cairo / Skia behavior. */
    axl_gfx_scale(2.0, 1.0);
    axl_gfx_translate(10.0, 0.0);
    AxlVec2 t = axl_mat3_transform_point(axl_gfx_get_transform(),
                                         axl_vec2(1.0, 0.0));
    test_check(near_double_(t.x, 22.0, 1e-12) && t.y == 0.0,
               "transform: scale then translate composes as canvas");
    /* Local origin maps to scale * translate * (0,0) = (20, 0). */
    AxlVec2 origin = axl_mat3_transform_point(axl_gfx_get_transform(),
                                              axl_vec2(0.0, 0.0));
    test_check(near_double_(origin.x, 20.0, 1e-12) && origin.y == 0.0,
               "transform: local origin → (20, 0) under scale(2)+trans(10)");
    axl_gfx_reset_transform();
}

static void
test_transform_push_pop(void)
{
    axl_gfx_reset_transform();
    axl_gfx_translate(10.0, 0.0);
    test_check(axl_gfx_push_transform() == AXL_OK,
               "transform: push succeeds");
    axl_gfx_translate(5.0, 0.0);
    /* Inside push: cumulative (15, 0). */
    AxlVec2 t = axl_mat3_transform_point(axl_gfx_get_transform(),
                                         axl_vec2(0.0, 0.0));
    test_check(t.x == 15.0,
               "transform: inside push, translates cumulate");
    test_check(axl_gfx_pop_transform() == AXL_OK,
               "transform: pop succeeds");
    /* After pop: back to (10, 0). */
    t = axl_mat3_transform_point(axl_gfx_get_transform(),
                                 axl_vec2(0.0, 0.0));
    test_check(t.x == 10.0,
               "transform: pop restores pre-push state");
    axl_gfx_reset_transform();
}

static void
test_transform_pop_empty_returns_err(void)
{
    axl_gfx_reset_transform();
    test_check(axl_gfx_pop_transform() == AXL_ERR,
               "transform: pop on empty stack → AXL_ERR");
}

static void
test_transform_push_overflow_returns_err(void)
{
    axl_gfx_reset_transform();
    int rc = AXL_OK;
    for (int i = 0; i < AXL_GFX_TRANSFORM_STACK_MAX; i++) {
        rc = axl_gfx_push_transform();
        if (rc != AXL_OK) break;
    }
    test_check(rc == AXL_OK,
               "transform: AXL_GFX_TRANSFORM_STACK_MAX pushes succeed");
    test_check(axl_gfx_push_transform() == AXL_ERR,
               "transform: push past max → AXL_ERR");
    /* Count exactly how many pops succeed — must equal MAX so push
     * and pop bookkeeping agree.  Without the count, a +1/-1 drift
     * would silently pass. */
    int pops = 0;
    while (axl_gfx_pop_transform() == AXL_OK) {
        pops++;
    }
    test_check(pops == AXL_GFX_TRANSFORM_STACK_MAX,
               "transform: pop count matches push count exactly");
    axl_gfx_reset_transform();
}

static void
test_transform_rotate(void)
{
    axl_gfx_reset_transform();
    axl_gfx_rotate(AXL_MATH_HALF_PI);
    /* Rotate (1, 0) by π/2 → (0, 1) in math coords. */
    AxlVec2 t = axl_mat3_transform_point(axl_gfx_get_transform(),
                                         axl_vec2(1.0, 0.0));
    test_check(near_double_(t.x, 0.0, 1e-6)
               && near_double_(t.y, 1.0, 1e-6),
               "transform: rotate(π/2) maps (1,0) → (0,1)");
    axl_gfx_reset_transform();
}

static void
test_transform_skew(void)
{
    axl_gfx_reset_transform();
    axl_gfx_skew(0.5, 0.0);
    /* Skew x by 0.5*y; point (0, 1) → (0.5, 1). */
    AxlVec2 t = axl_mat3_transform_point(axl_gfx_get_transform(),
                                         axl_vec2(0.0, 1.0));
    test_check(t.x == 0.5 && t.y == 1.0,
               "transform: skew(0.5, 0) maps (0,1) → (0.5, 1)");
    axl_gfx_reset_transform();
}

/* Path-side integration: fill a small path with a translate active,
 * verify the painted pixels land at the translated location. */
static void
test_transform_path_translate_paints_at_offset(void)
{
    AxlGfxBuffer *buf = axl_gfx_buffer_new(64, 64);
    test_check(buf != NULL, "transform: buffer alloc for path test");
    if (!buf) return;
    axl_gfx_buffer_clear(buf, AXL_GFX_BLACK);
    axl_gfx_target_buffer(buf);
    axl_gfx_reset_transform();
    axl_gfx_translate(20.0, 30.0);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 0, 0);
    axl_gfx_path_line_to(p, 5, 0);
    axl_gfx_path_line_to(p, 5, 5);
    axl_gfx_path_line_to(p, 0, 5);
    axl_gfx_path_close(p);
    axl_gfx_fill_path(p, AXL_GFX_RED);
    axl_gfx_path_free(p);

    axl_gfx_target_buffer(NULL);
    axl_gfx_reset_transform();

    /* Pin pixels at multiple positions to make the test fail loud
     * if the transform direction/magnitude is wrong: the 5x5 rect's
     * top-left should be exactly (20, 30), the interior should be
     * red, anything outside the rect (including the untransformed
     * region) should be black. */
    AxlGfxPixel *px = axl_gfx_buffer_pixels(buf);
    test_check(px[32 * 64 + 22].red == 0xFF,
               "transform: interior pixel (22, 32) is red");
    test_check(px[30 * 64 + 20].red == 0xFF,
               "transform: top-left corner pixel (20, 30) is red");
    test_check(px[30 * 64 + 19].red == 0x00,
               "transform: pixel just left of translated rect is black");
    test_check(px[29 * 64 + 20].red == 0x00,
               "transform: pixel just above translated rect is black");
    test_check(px[2 * 64 + 2].red == 0x00,
               "transform: untransformed local (2, 2) is black");
    test_check(px[0 * 64 + 0].red == 0x00,
               "transform: untransformed origin is black");

    axl_gfx_buffer_free(buf);
}

static void
test_transform_rotate_then_translate(void)
{
    /* Composition cross-check vs scale-then-translate:
     * rotate(π/2) then translate(10, 0) applied to (0, 0).
     * In canvas semantics: translate first, then rotate.
     *   (0, 0) → translate → (10, 0) → rotate(π/2) → (0, 10).
     * If composition is swapped, would get (10, 0). */
    axl_gfx_reset_transform();
    axl_gfx_rotate(AXL_MATH_HALF_PI);
    axl_gfx_translate(10.0, 0.0);
    AxlVec2 t = axl_mat3_transform_point(axl_gfx_get_transform(),
                                         axl_vec2(0.0, 0.0));
    test_check(near_double_(t.x, 0.0, 1e-6)
               && near_double_(t.y, 10.0, 1e-6),
               "transform: rotate then translate on origin → (0, 10)");
    axl_gfx_reset_transform();
}

static void
test_transform_identity_ops_preserve(void)
{
    /* translate(0,0), scale(1,1), rotate(0) should each leave the
     * transform unchanged.  Catches a future "optimize identity"
     * mistake that returns a non-identity matrix. */
    axl_gfx_reset_transform();
    axl_gfx_translate(0.0, 0.0);
    test_check(mat3_near_(axl_gfx_get_transform(),
                          axl_mat3_identity(), 1e-15),
               "transform: translate(0,0) preserves identity");
    axl_gfx_scale(1.0, 1.0);
    test_check(mat3_near_(axl_gfx_get_transform(),
                          axl_mat3_identity(), 1e-15),
               "transform: scale(1,1) preserves identity");
    axl_gfx_rotate(0.0);
    test_check(mat3_near_(axl_gfx_get_transform(),
                          axl_mat3_identity(), 1e-6),
               "transform: rotate(0) preserves identity (within sin/cos)");
    axl_gfx_reset_transform();
}

// ---------------------------------------------------------------------------
// Gradients (G5)
// ---------------------------------------------------------------------------

static void
test_gradient_new_non_null(void)
{
    AxlGfxGradient *lin = axl_gfx_gradient_linear_new(0, 0, 0, 100);
    AxlGfxGradient *rad = axl_gfx_gradient_radial_new(10, 10, 10);
    test_check(lin != NULL, "gradient: linear_new returns non-NULL");
    test_check(rad != NULL, "gradient: radial_new returns non-NULL");
    axl_gfx_gradient_free(lin);
    axl_gfx_gradient_free(rad);
}

static void
test_gradient_add_stop_errors(void)
{
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 10);
    test_check(axl_gfx_gradient_add_stop(NULL, 0.0f, AXL_GFX_RED) == AXL_ERR,
               "gradient: add_stop(NULL) returns AXL_ERR");
    test_check(axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED) == AXL_OK,
               "gradient: add_stop on valid gradient returns AXL_OK");
    /* Fill to the cap, then one past it. */
    int rc = AXL_OK;
    for (int i = 1; i < AXL_GFX_GRADIENT_MAX_STOPS; i++) {
        rc = axl_gfx_gradient_add_stop(g, (float)i / AXL_GFX_GRADIENT_MAX_STOPS,
                                       AXL_GFX_BLUE);
    }
    test_check(rc == AXL_OK,
               "gradient: filling to MAX_STOPS stays AXL_OK");
    test_check(axl_gfx_gradient_add_stop(g, 0.5f, AXL_GFX_GREEN) == AXL_ERR,
               "gradient: add_stop past MAX_STOPS returns AXL_ERR");
    axl_gfx_gradient_free(g);
}

static void
test_gradient_free_null_safe(void)
{
    /* free(NULL) must be a safe no-op: the allocator/state stays
       intact, proven by a subsequent successful allocation. */
    axl_gfx_gradient_free(NULL);
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 1, 1);
    test_check(g != NULL,
               "gradient: free(NULL) is a safe no-op (allocation still works after)");
    axl_gfx_gradient_free(g);
}

static void
test_gradient_single_stop_uniform(void)
{
    /* One stop → every filled pixel is exactly that color, regardless
       of position or sampling convention. */
    AxlGfxBuffer   *b = axl_gfx_buffer_new(8, 8);
    AxlGfxPixel     bg = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 8);
    axl_gfx_gradient_add_stop(g, 0.5f, AXL_GFX_GREEN);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rect_gradient(0, 0, 8, 8, g);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(rc == AXL_OK, "gradient: single-stop fill returns AXL_OK");
    test_check(p[0].green == 0xFF && p[0].red == 0 && p[0].blue == 0,
               "gradient: single-stop (0,0) is exactly the stop color");
    test_check(p[7 * 8 + 7].green == 0xFF && p[7 * 8 + 7].red == 0,
               "gradient: single-stop (7,7) is exactly the stop color");
    axl_gfx_gradient_free(g);
    axl_gfx_buffer_free(b);
}

static void
test_gradient_no_stops_noop(void)
{
    AxlGfxBuffer   *b = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel     bg = {0x11, 0x22, 0x33, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 4);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rect_gradient(0, 0, 4, 4, g);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(rc == AXL_OK, "gradient: no-stops fill returns AXL_OK");
    test_check(p[0].blue == 0x11 && p[0].green == 0x22 && p[0].red == 0x33,
               "gradient: no-stops fill leaves pixels untouched");
    axl_gfx_gradient_free(g);
    axl_gfx_buffer_free(b);
}

static void
test_gradient_fill_null_returns_err(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rect_gradient(0, 0, 4, 4, NULL);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_ERR, "gradient: fill with NULL gradient returns AXL_ERR");
    axl_gfx_buffer_free(b);
}

static void
test_gradient_fill_zero_dim_noop(void)
{
    AxlGfxBuffer   *b = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel     bg = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 4);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rect_gradient(0, 0, 0, 4, g);   /* w == 0 */
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(rc == AXL_OK, "gradient: zero-width fill returns AXL_OK");
    test_check(p[0].red == 0,
               "gradient: zero-width fill is a no-op (pixel untouched)");
    axl_gfx_gradient_free(g);
    axl_gfx_buffer_free(b);
}

/* Build a vertical red→blue linear gradient over a 4xH buffer and
   return it filled (caller frees). Axis runs (0,0)→(0,axis_h). */
static AxlGfxBuffer *
build_vertical_red_blue(uint32_t h, float axis_h)
{
    AxlGfxBuffer   *b = axl_gfx_buffer_new(4, h);
    AxlGfxPixel     bg = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, axis_h);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED);
    axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_BLUE);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect_gradient(0, 0, 4, (int32_t)h, g);
    axl_gfx_target_buffer(NULL);
    axl_gfx_gradient_free(g);
    return b;
}

static void
test_gradient_linear_vertical_endpoints(void)
{
    AxlGfxBuffer *b = build_vertical_red_blue(100, 100.0f);
    AxlGfxPixel  *p = axl_gfx_buffer_pixels(b);
    AxlGfxPixel   top = p[0 * 4 + 0];
    AxlGfxPixel   bot = p[99 * 4 + 0];
    test_check(top.red > 200 && top.blue < 55,
               "gradient: linear top end is mostly red");
    test_check(bot.blue > 200 && bot.red < 55,
               "gradient: linear bottom end is mostly blue");
    axl_gfx_buffer_free(b);
}

static void
test_gradient_linear_monotonic_and_conserved(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 100);
    AxlGfxBuffer *b2 = build_vertical_red_blue(100, 100.0f);
    axl_gfx_buffer_free(b);
    AxlGfxPixel  *p = axl_gfx_buffer_pixels(b2);
    AxlGfxPixel   hi = p[10 * 4 + 0];   /* near top  (more red) */
    AxlGfxPixel   lo = p[90 * 4 + 0];   /* near bottom (more blue) */
    AxlGfxPixel   mid = p[50 * 4 + 0];
    test_check(hi.red > lo.red,
               "gradient: red decreases top→bottom (monotonic)");
    test_check(lo.blue > hi.blue,
               "gradient: blue increases top→bottom (monotonic)");
    /* red↔blue interpolation: channels sum to ~255 (green stays 0). */
    int sum = (int)mid.red + (int)mid.blue;
    test_check(sum >= 250 && sum <= 256,
               "gradient: mid red+blue conserves ~255 (linear interp)");
    test_check(mid.green == 0,
               "gradient: mid green stays 0 (no spurious channel)");
    axl_gfx_buffer_free(b2);
}

static void
test_gradient_linear_clamp_beyond_axis(void)
{
    /* Axis ends at y=10 but the rect is 20 tall: pixels below y=10
       clamp to the last stop (blue). */
    AxlGfxBuffer *b = build_vertical_red_blue(20, 10.0f);
    AxlGfxPixel  *p = axl_gfx_buffer_pixels(b);
    AxlGfxPixel   below = p[18 * 4 + 0];
    /* t clamps to exactly 1.0 here → the boundary guard returns the
       last stop verbatim, so the color is EXACTLY blue (no lerp). */
    test_check(below.blue == 0xFF && below.green == 0 && below.red == 0,
               "gradient: pixels past axis end are exactly the last stop (blue)");
    axl_gfx_buffer_free(b);
}

static void
test_gradient_three_stops_bracket_selection(void)
{
    /* red@0 → green@0.5 → blue@1.0 over a 100px vertical axis.  The
       lower half must interpolate red↔green (no blue), the upper half
       green↔blue (no red).  Pins that sample_stops_ selects the
       correct adjacent pair, not just the endpoints. */
    AxlGfxBuffer   *b = axl_gfx_buffer_new(4, 100);
    AxlGfxPixel     bg = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 100);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED);
    axl_gfx_gradient_add_stop(g, 0.5f, AXL_GFX_GREEN);
    axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_BLUE);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect_gradient(0, 0, 4, 100, g);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    AxlGfxPixel  lower = p[25 * 4 + 0];   /* t≈0.255 → red/green pair */
    AxlGfxPixel  upper = p[75 * 4 + 0];   /* t≈0.755 → green/blue pair */
    test_check(lower.blue < 30 && lower.red > 50 && lower.green > 50,
               "gradient: 3-stop lower half mixes red/green (no blue)");
    test_check(upper.red < 30 && upper.blue > 50 && upper.green > 50,
               "gradient: 3-stop upper half mixes green/blue (no red)");
    axl_gfx_gradient_free(g);
    axl_gfx_buffer_free(b);
}

static void
test_gradient_radial_center_vs_edge(void)
{
    AxlGfxBuffer   *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel     bg = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxGradient *g = axl_gfx_gradient_radial_new(10, 10, 10);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED);   /* center */
    axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_BLUE);  /* edge */

    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect_gradient(0, 0, 20, 20, g);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    AxlGfxPixel  center = p[10 * 20 + 10];
    AxlGfxPixel  corner = p[0 * 20 + 0];   /* dist ~14 > radius → clamp blue */
    test_check(center.red > 200 && center.blue < 55,
               "gradient: radial center is mostly red");
    test_check(corner.blue > 200 && corner.red < 55,
               "gradient: radial corner (beyond radius) clamps to blue");
    axl_gfx_gradient_free(g);
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_gfx_main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;

    test_print_header("AxlTestGfx");

    test_default_font_not_null();
    test_default_font_stable_pointer();
    test_default_font_is_edk2_laffstd();

    test_glyph_null_safety();
    test_glyph_first_codepoint();
    test_glyph_last_codepoint();
    test_glyph_middle_codepoint();
    test_glyph_below_range_uses_fallback();
    test_glyph_above_range_uses_fallback();

    test_advance_null_font();
    test_advance_monospace_always_cell_width();

    test_measure_text_null_font();
    test_measure_text_null_text();
    test_measure_text_scale_zero();
    test_measure_text_empty();
    test_measure_text_single_char();
    test_measure_text_multi_char();
    test_measure_text_counts_nonprintable();
    test_measure_text_utf8_codepoints();

    test_unifont_metadata();
    test_unifont_ascii_glyph_half_width();
    test_unifont_box_drawing_present();
    test_unifont_cjk_glyph_full_width();
    test_unifont_variable_advance();
    test_unifont_missing_glyph_falls_back_to_question();
    test_default_font_invalid_utf8_renders_fallback();
    test_measure_text_unifont_per_glyph_advance();

    test_clip_reset_clears_stack();
    test_clip_push_then_get();
    test_clip_push_intersection();
    test_clip_pop_restores_previous();
    test_clip_pop_empty_errors();
    test_clip_push_overflow_errors();
    test_clip_empty_intersection();
    test_clip_get_null_safe();

    test_buffer_new_basic();
    test_buffer_new_zero_dim_returns_null();
    test_buffer_free_null_safe();
    test_buffer_get_info_null_safe();
    test_buffer_clear_fills_all_pixels();
    test_buffer_clear_null_safe();
    test_buffer_pixels_null_safe();
    test_buffer_pixels_writable();
    test_target_buffer_redirects_fill();
    test_target_buffer_honors_clip();
    test_get_current_target_default_is_null();
    test_get_current_target_roundtrips_set();
    test_get_current_target_cleared_on_buffer_free();

    test_blend_alpha_full_opaque_yields_src();
    test_blend_alpha_zero_yields_dst();
    test_blend_alpha_half();
    test_blend_per_channel_independent();
    test_fill_rect_alpha_on_buffer_blends();
    test_fill_rect_alpha_zero_is_noop();

    test_fill_rect_i_positive_parity();
    test_fill_rect_i_negative_x_clamped();
    test_fill_rect_i_negative_y_clamped();
    test_fill_rect_i_zero_w_noop();
    test_fill_rect_i_zero_h_noop();
    test_fill_rect_i_negative_w_noop();
    test_fill_rect_i_negative_h_noop();
    test_fill_rect_i_fully_off_top_left();

    test_path_new_returns_non_null();
    test_path_free_null_safe();
    test_fill_path_null_returns_err();
    test_fill_path_empty_returns_err();
    test_fill_path_rectangle();
    test_fill_path_triangle();
    test_fill_path_curve_subdivides();
    test_fill_path_arc_subdivides();
    test_fill_path_multi_subpath_even_odd();
    test_fill_rounded_rect_zero_radius_is_plain();
    test_fill_rounded_rect_corners_not_filled();
    test_fill_rounded_rect_zero_w_noop();
    test_fill_rounded_rect_negative_h_noop();
    test_fill_rounded_rect_negative_coords_clipped();
    test_stroke_path_null_returns_err();
    test_stroke_path_rectangle_outlines();

    test_draw_line_horizontal_on_buffer();
    test_draw_line_vertical_on_buffer();
    test_draw_line_diagonal_on_buffer();
    test_draw_line_single_point();
    test_draw_line_off_buffer_clipped();
    test_draw_rect_outline_on_buffer();
    test_draw_polyline_connects_points();
    test_draw_polyline_null_safety();

    test_transform_default_identity();
    test_transform_translate_composes();
    test_transform_scale_then_translate();
    test_transform_push_pop();
    test_transform_pop_empty_returns_err();
    test_transform_push_overflow_returns_err();
    test_transform_rotate();
    test_transform_skew();
    test_transform_path_translate_paints_at_offset();
    test_transform_rotate_then_translate();
    test_transform_identity_ops_preserve();

    test_gradient_new_non_null();
    test_gradient_add_stop_errors();
    test_gradient_free_null_safe();
    test_gradient_single_stop_uniform();
    test_gradient_no_stops_noop();
    test_gradient_fill_null_returns_err();
    test_gradient_fill_zero_dim_noop();
    test_gradient_linear_vertical_endpoints();
    test_gradient_linear_monotonic_and_conserved();
    test_gradient_linear_clamp_beyond_axis();
    test_gradient_three_stops_bracket_selection();
    test_gradient_radial_center_vs_edge();

    return test_print_results();
}

AXL_APP(test_gfx_main)
