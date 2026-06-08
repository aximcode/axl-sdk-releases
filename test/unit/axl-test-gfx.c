/** @file axl-test-gfx.c
    Unit tests for the gfx + font modules: default font, glyph lookup,
    advance computation, text measurement. Drawing primitives that
    require GOP are deliberately not exercised here — those need an
    integration test with a display, not a unit test.
**/

#include "axl-test.h"

#include <axl/axl-edid.h>
#include <axl/axl-font.h>
#include <axl/axl-gfx.h>
#include <axl/axl-math.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>

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

// ---------------------------------------------------------------------------
// Color string parsing (axl_gfx_color_parse)
// ---------------------------------------------------------------------------

/* True if @a p has exactly the given r,g,b,a bytes. */
static bool
px_is(AxlGfxPixel p, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return p.red == r && p.green == g && p.blue == b && p.alpha == a;
}

static void
test_color_parse_null_args(void)
{
    AxlGfxPixel out = { 0x11, 0x22, 0x33, 0x44 };  /* b,g,r,a in memory */
    test_check(axl_gfx_color_parse(NULL, &out) == AXL_ERR,
               "color_parse: NULL str -> AXL_ERR");
    test_check(px_is(out, 0x33, 0x22, 0x11, 0x44),
               "color_parse: out untouched on NULL str");
    test_check(axl_gfx_color_parse("#FFFFFF", NULL) == AXL_ERR,
               "color_parse: NULL out -> AXL_ERR");
}

static void
test_color_parse_rrggbb(void)
{
    AxlGfxPixel out;
    int rc = axl_gfx_color_parse("#FF6347", &out);
    test_check(rc == AXL_OK, "color_parse: #RRGGBB returns AXL_OK");
    test_check(px_is(out, 0xFF, 0x63, 0x47, 0xFF),
               "color_parse: #FF6347 -> r=FF g=63 b=47 a=FF (tomato)");
}

static void
test_color_parse_rrggbbaa(void)
{
    AxlGfxPixel out;
    int rc = axl_gfx_color_parse("#11223344", &out);
    test_check(rc == AXL_OK, "color_parse: #RRGGBBAA returns AXL_OK");
    test_check(px_is(out, 0x11, 0x22, 0x33, 0x44),
               "color_parse: #11223344 -> r=11 g=22 b=33 a=44 (explicit alpha)");
}

static void
test_color_parse_rgb_short(void)
{
    AxlGfxPixel out;
    int rc = axl_gfx_color_parse("#F80", &out);
    test_check(rc == AXL_OK, "color_parse: #RGB returns AXL_OK");
    test_check(px_is(out, 0xFF, 0x88, 0x00, 0xFF),
               "color_parse: #F80 -> each nibble doubled, alpha opaque");
}

static void
test_color_parse_rgba_short(void)
{
    AxlGfxPixel out;
    int rc = axl_gfx_color_parse("#1234", &out);
    test_check(rc == AXL_OK, "color_parse: #RGBA returns AXL_OK");
    test_check(px_is(out, 0x11, 0x22, 0x33, 0x44),
               "color_parse: #1234 -> each nibble doubled incl alpha");
    /* Short-form alpha doubling with a non-trivial value (0x8 -> 0x88). */
    axl_gfx_color_parse("#F008", &out);
    test_check(px_is(out, 0xFF, 0x00, 0x00, 0x88),
               "color_parse: #F008 -> short-form alpha doubles (8 -> 0x88)");
}

static void
test_color_parse_case_insensitive(void)
{
    AxlGfxPixel lo, hi;
    test_check(axl_gfx_color_parse("#ff6347", &lo) == AXL_OK
               && axl_gfx_color_parse("#FF6347", &hi) == AXL_OK,
               "color_parse: both cases parse");
    test_check(px_is(lo, 0xFF, 0x63, 0x47, 0xFF)
               && px_is(hi, 0xFF, 0x63, 0x47, 0xFF),
               "color_parse: lower- and upper-case hex are equivalent");
}

static void
test_color_parse_opaque_default(void)
{
    /* The 3- and 6-digit forms default alpha to fully opaque. */
    AxlGfxPixel six, three;
    axl_gfx_color_parse("#102030", &six);
    axl_gfx_color_parse("#123", &three);
    test_check(six.alpha == 0xFF, "color_parse: #RRGGBB defaults alpha to 0xFF");
    test_check(three.alpha == 0xFF, "color_parse: #RGB defaults alpha to 0xFF");
}

static void
test_color_parse_rejects_missing_hash(void)
{
    AxlGfxPixel out = { 1, 2, 3, 4 };
    test_check(axl_gfx_color_parse("FF6347", &out) == AXL_ERR,
               "color_parse: missing leading # -> AXL_ERR");
    test_check(px_is(out, 3, 2, 1, 4),
               "color_parse: out untouched when # missing");
}

static void
test_color_parse_rejects_bad_length(void)
{
    AxlGfxPixel out;
    test_check(axl_gfx_color_parse("#", &out) == AXL_ERR,
               "color_parse: bare # -> AXL_ERR");
    test_check(axl_gfx_color_parse("#12", &out) == AXL_ERR,
               "color_parse: 2 nibbles -> AXL_ERR");
    test_check(axl_gfx_color_parse("#12345", &out) == AXL_ERR,
               "color_parse: 5 nibbles -> AXL_ERR");
    test_check(axl_gfx_color_parse("#1234567", &out) == AXL_ERR,
               "color_parse: 7 nibbles -> AXL_ERR");
    test_check(axl_gfx_color_parse("#123456789", &out) == AXL_ERR,
               "color_parse: 9 nibbles -> AXL_ERR");
}

static void
test_color_parse_rejects_non_hex(void)
{
    AxlGfxPixel out;
    test_check(axl_gfx_color_parse("#GGGGGG", &out) == AXL_ERR,
               "color_parse: non-hex digit -> AXL_ERR");
    test_check(axl_gfx_color_parse("#12345g", &out) == AXL_ERR,
               "color_parse: trailing non-hex digit -> AXL_ERR");
    test_check(axl_gfx_color_parse("#FF6347 ", &out) == AXL_ERR,
               "color_parse: trailing whitespace -> AXL_ERR (no trim)");
    test_check(axl_gfx_color_parse("# F6347", &out) == AXL_ERR,
               "color_parse: interior space -> AXL_ERR");
}

static void
test_color_parse_untouched_on_error(void)
{
    AxlGfxPixel out = { 0xDE, 0xAD, 0xBE, 0xEF };  /* b,g,r,a in memory */
    axl_gfx_color_parse("#xyz", &out);
    test_check(px_is(out, 0xBE, 0xAD, 0xDE, 0xEF),
               "color_parse: out fully untouched on parse error");
}

static void
test_color_parse_roundtrips_dump_format(void)
{
    /* The 8-digit form is exactly what display-list dump emits
       (#RRGGBBAA); parsing it back must reproduce the pixel. */
    AxlGfxPixel out;
    int rc = axl_gfx_color_parse("#0000FFFF", &out);  /* dump form of blue */
    test_check(rc == AXL_OK && px_is(out, 0x00, 0x00, 0xFF, 0xFF),
               "color_parse: round-trips the dump's #RRGGBBAA blue");
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
test_fill_path_orientation_not_flipped(void)
{
    /* G14 analytic rasterizer feeds the outline y-down (no flip).
     * A triangle with its WIDE edge at the TOP (y=4, x 4..16) and
     * its apex at the BOTTOM (10,16) must render wide-at-top: a row
     * near the top has strictly more filled pixels than a row near
     * the bottom.  Catches an upside-down (y-flip) regression. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 4.0f,  4.0f);
    axl_gfx_path_line_to(p, 16.0f, 4.0f);
    axl_gfx_path_line_to(p, 10.0f, 16.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_path(p, red);
    axl_gfx_target_buffer(NULL);

    size_t top_row    = count_non_bg_in_rect(b, 0, 6,  20, 1, bg);
    size_t bottom_row = count_non_bg_in_rect(b, 0, 14, 20, 1, bg);
    test_check(top_row > bottom_row,
               "fill_path: wide-top triangle renders wide at top (not y-flipped)");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_fill_path_analytic_partial_coverage(void)
{
    /* The analytic rasterizer produces exact fractional edge coverage.
     * A rectangle whose left edge sits at x=6.5 leaves column 6 about
     * half covered, so its interior pixels land at a partial alpha
     * strictly between background and full red — proof that smooth AA
     * coverage (not just 0/full) reaches the framebuffer. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(20, 20);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };  /* B=0 G=0 R=0xFF */
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 6.5f,  4.0f);
    axl_gfx_path_line_to(p, 14.0f, 4.0f);
    axl_gfx_path_line_to(p, 14.0f, 14.0f);
    axl_gfx_path_line_to(p, 6.5f,  14.0f);
    axl_gfx_path_close(p);

    axl_gfx_target_buffer(b);
    axl_gfx_fill_path(p, red);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    /* Interior column (10) is fully covered. */
    test_check(px[8 * 20 + 10].red == 0xFF,
               "fill_path: interior pixel is full-coverage red");
    /* Left fractional-edge column (6) at a mid-height row: partial
     * coverage on black bg → 0 < red < 0xFF, with no colour leak. */
    AxlGfxPixel edge = px[8 * 20 + 6];
    test_check(edge.red > 0 && edge.red < 0xFF,
               "fill_path: x=6.5 edge column has partial AA coverage");
    test_check(edge.green == 0 && edge.blue == 0,
               "fill_path: AA edge pixel keeps source colour (no leak)");

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

static void
test_stroke_path_width_band(void)
{
    /* G8a: a width-7 horizontal line must fill a multi-pixel-tall
     * band, not a 1px line.  The old width-ignoring implementation
     * fails this (it drew a single-pixel Bresenham line). */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 8.0f, 20.0f);
    axl_gfx_path_line_to(p, 32.0f, 20.0f);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_stroke_path(p, red, 7.0f);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "stroke: width-7 returns AXL_OK");
    /* Vertical strip through the middle: a 7px stroke fills ~7 rows. */
    size_t band = count_non_bg_in_rect(b, 20, 0, 1, 40, bg);
    test_check(band >= 5,
               "stroke: width-7 line fills a multi-pixel band (width honored)");
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[20 * 40 + 20].red > 0,
               "stroke: band center is filled");
    test_check(px[2 * 40 + 20].red == 0,
               "stroke: row far from the line is untouched");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_stroke_path_width_scales(void)
{
    /* A width-11 stroke covers strictly more pixels than width-3. */
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };

    AxlGfxBuffer *b3 = axl_gfx_buffer_new(40, 40);
    axl_gfx_buffer_clear(b3, bg);
    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 8.0f, 20.0f);
    axl_gfx_path_line_to(p, 32.0f, 20.0f);
    axl_gfx_target_buffer(b3);
    axl_gfx_stroke_path(p, red, 3.0f);
    axl_gfx_target_buffer(NULL);
    size_t n3 = count_non_bg_in_rect(b3, 0, 0, 40, 40, bg);

    AxlGfxBuffer *b11 = axl_gfx_buffer_new(40, 40);
    axl_gfx_buffer_clear(b11, bg);
    axl_gfx_target_buffer(b11);
    axl_gfx_stroke_path(p, red, 11.0f);
    axl_gfx_target_buffer(NULL);
    size_t n11 = count_non_bg_in_rect(b11, 0, 0, 40, 40, bg);

    test_check(n11 > n3,
               "stroke: width-11 covers more pixels than width-3");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b3);
    axl_gfx_buffer_free(b11);
}

/* Helper: stroke a horizontal width-8 line (12,20)->(28,20) with the
 * given cap into a fresh 40x40 buffer; caller frees. */
static AxlGfxBuffer *
stroke_capped_line_(
    AxlGfxLineCap  cap,
    AxlGfxPixel    bg,
    AxlGfxPixel    color
    )
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 12.0f, 20.0f);
    axl_gfx_path_line_to(p, 28.0f, 20.0f);
    AxlGfxStrokeStyle st = { 8.0f, cap, AXL_GFX_JOIN_MITER, 10.0f };
    axl_gfx_target_buffer(b);
    axl_gfx_stroke_path_ex(p, color, &st);
    axl_gfx_target_buffer(NULL);
    axl_gfx_path_free(p);
    return b;
}

static void
test_stroke_round_cap_extends(void)
{
    /* Round cap of radius 4 at x=12: a pixel ~3px LEFT of the start
     * (9,20) is inside the cap and filled; far past it (5,20) is not. */
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    AxlGfxBuffer *b = stroke_capped_line_(AXL_GFX_CAP_ROUND, bg, red);
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[20 * 40 + 9].red > 0,
               "stroke_ex: round cap extends beyond the endpoint");
    test_check(px[20 * 40 + 5].red == 0,
               "stroke_ex: nothing drawn well past the round cap");
    axl_gfx_buffer_free(b);
}

static void
test_stroke_butt_cap_flush(void)
{
    /* Butt cap (the default) ends flush at the endpoint: the line body
     * just inside the start (14,20) is filled, but nothing extends
     * past x=12 — (10,20) stays empty.  Also exercises the plain
     * axl_gfx_stroke_path default style. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 12.0f, 20.0f);
    axl_gfx_path_line_to(p, 28.0f, 20.0f);
    axl_gfx_target_buffer(b);
    axl_gfx_stroke_path(p, red, 8.0f);   /* default = butt + miter */
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[20 * 40 + 14].red > 0,
               "stroke: butt-default line body is filled");
    test_check(px[20 * 40 + 10].red == 0,
               "stroke: butt cap does not extend past the endpoint");
    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_stroke_square_cap_extends(void)
{
    /* Square cap projects r=4 past the endpoint, so (9,20) is filled
     * (like round) but (4,20) is not. */
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    AxlGfxBuffer *b = stroke_capped_line_(AXL_GFX_CAP_SQUARE, bg, red);
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[20 * 40 + 9].red > 0,
               "stroke_ex: square cap extends past the endpoint");
    test_check(px[20 * 40 + 4].red == 0,
               "stroke_ex: square cap stops at width/2 past the end");
    axl_gfx_buffer_free(b);
}

/* Helper: stroke an L-corner (10,12)->(30,12)->(30,32) width 10 with
 * the given join; return the filled-pixel count. */
static size_t
stroke_L_join_pixels_(
    AxlGfxLineJoin  join,
    AxlGfxPixel     bg,
    AxlGfxPixel     color
    )
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(48, 48);
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 10.0f, 12.0f);
    axl_gfx_path_line_to(p, 30.0f, 12.0f);
    axl_gfx_path_line_to(p, 30.0f, 32.0f);
    AxlGfxStrokeStyle st = { 10.0f, AXL_GFX_CAP_BUTT, join, 10.0f };
    axl_gfx_target_buffer(b);
    axl_gfx_stroke_path_ex(p, color, &st);
    axl_gfx_target_buffer(NULL);
    size_t n = count_non_bg_in_rect(b, 0, 0, 48, 48, bg);
    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
    return n;
}

static void
test_stroke_miter_join_fills_corner(void)
{
    /* A miter join fills the outer corner spike; a bevel chamfers it
     * off.  So the same L-corner covers strictly more pixels with a
     * miter join than with a bevel join — proves the join style is
     * honored and the miter geometry reaches past the bevel chord. */
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    size_t n_miter = stroke_L_join_pixels_(AXL_GFX_JOIN_MITER, bg, red);
    size_t n_bevel = stroke_L_join_pixels_(AXL_GFX_JOIN_BEVEL, bg, red);
    size_t n_round = stroke_L_join_pixels_(AXL_GFX_JOIN_ROUND, bg, red);
    test_check(n_miter > n_bevel,
               "stroke_ex: miter join fills more corner than bevel");
    test_check(n_round > n_bevel,
               "stroke_ex: round join fills more corner than bevel");
    test_check(n_miter > 0 && n_bevel > 0 && n_round > 0,
               "stroke_ex: all join styles draw the stroke");
}

static void
test_stroke_miter_no_inner_spill(void)
{
    /* H1 regression: a sharp corner between SHORT segments must not
     * spill miter geometry onto the concave (inner) side.  Sharp V,
     * apex (20,24) pointing left, short arms, width 10, high miter
     * limit (miter kept).  The outer miter fills left of the apex
     * (16,24); the concave side past the arm ends (28,24) must stay
     * empty — the old "emit both sides" join spilled an inner miter
     * quad out to ~x=29. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 23.0f, 22.0f);
    axl_gfx_path_line_to(p, 20.0f, 24.0f);
    axl_gfx_path_line_to(p, 23.0f, 26.0f);
    AxlGfxStrokeStyle st = { 10.0f, AXL_GFX_CAP_BUTT, AXL_GFX_JOIN_MITER, 20.0f };
    axl_gfx_target_buffer(b);
    axl_gfx_stroke_path_ex(p, red, &st);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[24 * 40 + 16].red > 0,
               "stroke_ex: sharp miter fills the convex (outer) corner");
    test_check(px[24 * 40 + 28].red == 0,
               "stroke_ex: miter does not spill onto the concave (inner) side");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_stroke_path_ex_null_args(void)
{
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    AxlGfxStrokeStyle st = { 4.0f, AXL_GFX_CAP_BUTT, AXL_GFX_JOIN_MITER, 10.0f };
    AxlGfxPath *p = axl_gfx_path_new();
    test_check(axl_gfx_stroke_path_ex(NULL, red, &st) == AXL_ERR,
               "stroke_ex: NULL path returns AXL_ERR");
    test_check(axl_gfx_stroke_path_ex(p, red, NULL) == AXL_ERR,
               "stroke_ex: NULL style returns AXL_ERR");
    axl_gfx_path_free(p);
}

static void
test_stroke_dashes_create_gaps(void)
{
    /* A [6,6] dash on a horizontal line leaves 6px-on / 6px-off gaps
     * (butt caps).  Line (5,20)->(35,20), width 4: on x[5,11], off
     * [11,17], on [17,23].  And the dashed stroke covers fewer pixels
     * than the equivalent solid stroke. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 5.0f, 20.0f);
    axl_gfx_path_line_to(p, 35.0f, 20.0f);
    static const float dash[2] = { 6.0f, 6.0f };
    AxlGfxStrokeStyle st = {
        4.0f, AXL_GFX_CAP_BUTT, AXL_GFX_JOIN_MITER, 10.0f, dash, 2, 0.0f
    };
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_stroke_path_ex(p, red, &st);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "stroke_ex: dashed returns AXL_OK");
    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[20 * 40 + 8].red > 0,
               "stroke_ex: dash on-segment is filled");
    test_check(px[20 * 40 + 14].red == 0,
               "stroke_ex: dash off-gap is empty");
    test_check(px[20 * 40 + 20].red > 0,
               "stroke_ex: second dash on-segment is filled");
    size_t n_dash = count_non_bg_in_rect(b, 0, 0, 40, 40, bg);

    AxlGfxBuffer *b2 = axl_gfx_buffer_new(40, 40);
    axl_gfx_buffer_clear(b2, bg);
    AxlGfxStrokeStyle solid = {
        4.0f, AXL_GFX_CAP_BUTT, AXL_GFX_JOIN_MITER, 10.0f, NULL, 0, 0.0f
    };
    axl_gfx_target_buffer(b2);
    axl_gfx_stroke_path_ex(p, red, &solid);
    axl_gfx_target_buffer(NULL);
    size_t n_solid = count_non_bg_in_rect(b2, 0, 0, 40, 40, bg);
    test_check(n_dash < n_solid,
               "stroke_ex: dashed covers fewer pixels than solid");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
    axl_gfx_buffer_free(b2);
}

static void
test_stroke_dash_offset_shifts(void)
{
    /* dash_offset = 6 inverts the [6,6] phase: x=8 now falls in an
     * off-gap and x=14 on a dash. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 5.0f, 20.0f);
    axl_gfx_path_line_to(p, 35.0f, 20.0f);
    static const float dash[2] = { 6.0f, 6.0f };
    AxlGfxStrokeStyle st = {
        4.0f, AXL_GFX_CAP_BUTT, AXL_GFX_JOIN_MITER, 10.0f, dash, 2, 6.0f
    };
    axl_gfx_target_buffer(b);
    axl_gfx_stroke_path_ex(p, red, &st);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *px = axl_gfx_buffer_pixels(b);
    test_check(px[20 * 40 + 8].red == 0,
               "stroke_ex: dash_offset puts x=8 in a gap");
    test_check(px[20 * 40 + 14].red > 0,
               "stroke_ex: dash_offset puts x=14 on a dash");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_stroke_dash_degenerate_is_solid(void)
{
    /* A pattern with no element long enough to advance must fall back
     * to a SOLID stroke, not hang (regression: all-near-zero pattern
     * with positive sum).  Test passing at all proves no infinite
     * loop; the filled gap-region pixel proves the solid fallback. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 5.0f, 20.0f);
    axl_gfx_path_line_to(p, 35.0f, 20.0f);
    static const float tiny[2] = { 1e-10f, 1e-10f };
    AxlGfxStrokeStyle st = {
        4.0f, AXL_GFX_CAP_BUTT, AXL_GFX_JOIN_MITER, 10.0f, tiny, 2, 0.0f
    };
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_stroke_path_ex(p, red, &st);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "stroke_ex: degenerate dash returns AXL_OK (no hang)");
    test_check(axl_gfx_buffer_pixels(b)[20 * 40 + 14].red > 0,
               "stroke_ex: degenerate dash falls back to a solid stroke");

    axl_gfx_path_free(p);
    axl_gfx_buffer_free(b);
}

static void
test_stroke_path_zero_width_noop(void)
{
    /* Width <= 0 is a no-op success: nothing drawn. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    AxlGfxPath *p = axl_gfx_path_new();
    axl_gfx_path_move_to(p, 8.0f, 20.0f);
    axl_gfx_path_line_to(p, 32.0f, 20.0f);
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_stroke_path(p, red, 0.0f);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "stroke: zero width returns AXL_OK");
    test_check(count_non_bg_in_rect(b, 0, 0, 40, 40, bg) == 0,
               "stroke: zero width draws nothing");

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
mat3_near_(AxlTransform a, AxlTransform b, double tol)
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
                          axl_transform_identity(), 1e-15),
               "transform: starts as identity after reset");
}

static void
test_transform_translate_composes(void)
{
    axl_gfx_reset_transform();
    axl_gfx_translate(10.0, 20.0);
    AxlTransform M = axl_gfx_get_transform();
    AxlVec2 t = axl_transform_map_point(M, axl_vec2(0.0, 0.0));
    test_check(t.x == 10.0 && t.y == 20.0,
               "transform: translate(10,20) maps origin → (10,20)");
    /* Compose: another translate adds. */
    axl_gfx_translate(5.0, 0.0);
    t = axl_transform_map_point(axl_gfx_get_transform(),
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
    AxlVec2 t = axl_transform_map_point(axl_gfx_get_transform(),
                                         axl_vec2(1.0, 0.0));
    test_check(near_double_(t.x, 22.0, 1e-12) && t.y == 0.0,
               "transform: scale then translate composes as canvas");
    /* Local origin maps to scale * translate * (0,0) = (20, 0). */
    AxlVec2 origin = axl_transform_map_point(axl_gfx_get_transform(),
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
    AxlVec2 t = axl_transform_map_point(axl_gfx_get_transform(),
                                         axl_vec2(0.0, 0.0));
    test_check(t.x == 15.0,
               "transform: inside push, translates cumulate");
    test_check(axl_gfx_pop_transform() == AXL_OK,
               "transform: pop succeeds");
    /* After pop: back to (10, 0). */
    t = axl_transform_map_point(axl_gfx_get_transform(),
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
    AxlVec2 t = axl_transform_map_point(axl_gfx_get_transform(),
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
    AxlVec2 t = axl_transform_map_point(axl_gfx_get_transform(),
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
    AxlVec2 t = axl_transform_map_point(axl_gfx_get_transform(),
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
                          axl_transform_identity(), 1e-15),
               "transform: translate(0,0) preserves identity");
    axl_gfx_scale(1.0, 1.0);
    test_check(mat3_near_(axl_gfx_get_transform(),
                          axl_transform_identity(), 1e-15),
               "transform: scale(1,1) preserves identity");
    axl_gfx_rotate(0.0);
    test_check(mat3_near_(axl_gfx_get_transform(),
                          axl_transform_identity(), 1e-6),
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

static void
test_gradient_sample_query(void)
{
    /* axl_gfx_gradient_sample: NULL / no-stops → transparent;
       otherwise the same color the fills would paint at a pixel. */
    test_check(axl_gfx_gradient_sample(NULL, 0, 0).alpha == 0,
               "gradient_sample: NULL gradient → transparent");
    AxlGfxGradient *empty = axl_gfx_gradient_linear_new(0, 0, 0, 10);
    test_check(axl_gfx_gradient_sample(empty, 0, 0).alpha == 0,
               "gradient_sample: no-stops → transparent");
    axl_gfx_gradient_free(empty);

    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 100);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED);
    axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_BLUE);
    AxlGfxPixel top = axl_gfx_gradient_sample(g, 0, 0);
    AxlGfxPixel bot = axl_gfx_gradient_sample(g, 0, 99);
    test_check(top.red > 200 && top.blue < 55,
               "gradient_sample: near axis start is mostly red");
    test_check(bot.blue > 200 && bot.red < 55,
               "gradient_sample: near axis end is mostly blue");
    axl_gfx_gradient_free(g);
}

static void
test_gradient_fill_path(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(8, 100);
    AxlGfxPixel   bg = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    /* Full-buffer rectangle path. */
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 0, 0);
    axl_gfx_path_line_to(path, 8, 0);
    axl_gfx_path_line_to(path, 8, 100);
    axl_gfx_path_line_to(path, 0, 100);
    axl_gfx_path_close(path);

    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 100);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED);
    axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_BLUE);

    /* Error paths first (target-independent). */
    test_check(axl_gfx_fill_path_gradient(NULL, g) == AXL_ERR,
               "fill_path_gradient: NULL path → AXL_ERR");
    test_check(axl_gfx_fill_path_gradient(path, NULL) == AXL_ERR,
               "fill_path_gradient: NULL gradient → AXL_ERR");

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_path_gradient(path, g);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* Interior pixels (away from AA edges) are fully covered, so they
       carry the raw gradient sample. */
    AxlGfxPixel top = p[5 * 8 + 2];
    AxlGfxPixel bot = p[94 * 8 + 2];
    test_check(rc == AXL_OK, "fill_path_gradient: returns AXL_OK");
    test_check(top.red > 180 && top.blue < 75,
               "fill_path_gradient: interior top is mostly red");
    test_check(bot.blue > 180 && bot.red < 75,
               "fill_path_gradient: interior bottom is mostly blue");
    axl_gfx_gradient_free(g);
    axl_gfx_path_free(path);
    axl_gfx_buffer_free(b);
}

static void
test_gradient_fill_rounded_rect(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel   bg = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    AxlGfxGradient *g = axl_gfx_gradient_radial_new(20, 20, 20);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RED);   /* center */
    axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_BLUE);  /* edge */

    test_check(axl_gfx_fill_rounded_rect_gradient(0, 0, 40, 40, 8, NULL)
               == AXL_ERR,
               "fill_rounded_rect_gradient: NULL gradient → AXL_ERR");

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_rounded_rect_gradient(0, 0, 40, 40, 8, g);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    AxlGfxPixel  center = p[20 * 40 + 20];     /* near radial center */
    AxlGfxPixel  band   = p[2 * 40 + 20];      /* top straight band, far out */
    test_check(rc == AXL_OK, "fill_rounded_rect_gradient: returns AXL_OK");
    test_check(center.red > 200 && center.blue < 55,
               "fill_rounded_rect_gradient: center is mostly red");
    test_check(band.blue > center.blue && band.red < center.red,
               "fill_rounded_rect_gradient: band pixel shades outward (radial)");
    axl_gfx_gradient_free(g);
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// Effects — blur (G6)
// ---------------------------------------------------------------------------

static void
test_blur_null_and_noop(void)
{
    test_check(axl_gfx_buffer_blur(NULL, 4) == AXL_ERR,
               "blur: NULL buffer → AXL_ERR");

    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   c = {0x10, 0x20, 0x30, 0xFF};
    axl_gfx_buffer_clear(b, c);
    int rc = axl_gfx_buffer_blur(b, 0);   /* radius 0 = identity */
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(rc == AXL_OK, "blur: radius 0 returns AXL_OK");
    test_check(p[0].blue == 0x10 && p[5].green == 0x20 && p[15].red == 0x30,
               "blur: radius 0 leaves pixels unchanged");
    axl_gfx_buffer_free(b);
}

static void
test_blur_uniform_stays_uniform(void)
{
    /* Blur of a constant image is the same constant — proves the
       edge-clamp (no border darkening) and the kernel normalization. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(12, 9);
    AxlGfxPixel   c = {0x40, 0x80, 0xC0, 0xFF};
    axl_gfx_buffer_clear(b, c);
    axl_gfx_buffer_blur(b, 4);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    bool uniform = true;
    for (int i = 0; i < 12 * 9; i++) {
        if (p[i].blue != 0x40 || p[i].green != 0x80 ||
            p[i].red != 0xC0 || p[i].alpha != 0xFF) {
            uniform = false;
            break;
        }
    }
    test_check(uniform,
               "blur: uniform image stays uniform (edge-clamp, normalized)");
    axl_gfx_buffer_free(b);
}

static void
test_blur_impulse_properties(void)
{
    /* A 9x1 buffer with one bright green pixel at index 4, blurred
       with radius 2. Assert implementation-agnostic blur invariants:
       energy conservation (normalized kernel), symmetry, monotonic
       falloff from the center, and locality (reach bounded by radius). */
    AxlGfxBuffer *b = axl_gfx_buffer_new(9, 1);
    AxlGfxPixel   black = {0, 0, 0, 0xFF};
    axl_gfx_buffer_clear(b, black);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    p[4].green = 255;

    axl_gfx_buffer_blur(b, 2);

    /* Energy conserved: a normalized blur preserves total intensity
       (the impulse is far enough from the edges that clamp adds none). */
    int sum = 0;
    for (int i = 0; i < 9; i++) sum += p[i].green;
    test_check(sum >= 248 && sum <= 256,
               "blur: total intensity conserved (normalized kernel)");
    /* Spread: center reduced from 255 but still the brightest, lit. */
    test_check(p[4].green > 0 && p[4].green < 255,
               "blur: impulse center spread (reduced but lit)");
    /* Symmetric about the center. */
    test_check(p[3].green == p[5].green && p[2].green == p[6].green,
               "blur: spread is symmetric about the impulse");
    /* Monotonic falloff outward. */
    test_check(p[4].green >= p[3].green && p[3].green >= p[2].green,
               "blur: intensity falls off monotonically from center");
    /* Locality: radius 2 reaches ±2, not ±3+. */
    test_check(p[1].green == 0 && p[7].green == 0 &&
               p[0].green == 0 && p[8].green == 0,
               "blur: reach is bounded by radius (±3 untouched)");
    axl_gfx_buffer_free(b);
}

static void
test_blur_vertical_axis(void)
{
    /* Same impulse along the vertical axis (1x9) — exercises the
       vertical pass independently of the horizontal one. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(1, 9);
    AxlGfxPixel   black = {0, 0, 0, 0xFF};
    axl_gfx_buffer_clear(b, black);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    p[4].green = 255;

    axl_gfx_buffer_blur(b, 2);

    int sum = 0;
    for (int i = 0; i < 9; i++) sum += p[i].green;
    test_check(sum >= 248 && sum <= 256,
               "blur: vertical pass conserves intensity");
    test_check(p[4].green > 0 && p[4].green < 255 &&
               p[3].green == p[5].green && p[2].green == p[6].green,
               "blur: vertical spread is symmetric and lit");
    test_check(p[1].green == 0 && p[7].green == 0,
               "blur: vertical reach bounded by radius");
    axl_gfx_buffer_free(b);
}

static void
test_shadow_null_and_transparent(void)
{
    test_check(axl_gfx_draw_shadow(NULL, 0, 0, AXL_GFX_BLACK, 4) == AXL_ERR,
               "shadow: NULL src → AXL_ERR");

    /* Transparent source casts no shadow — target stays untouched. */
    AxlGfxBuffer *target = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel   white  = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(target, white);
    AxlGfxBuffer *src = axl_gfx_buffer_new(8, 8);
    AxlGfxPixel   clear = {0, 0, 0, 0};
    axl_gfx_buffer_clear(src, clear);   /* fully transparent */

    axl_gfx_target_buffer(target);
    int rc = axl_gfx_draw_shadow(src, 16, 16, AXL_GFX_BLACK, 4);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *tp = axl_gfx_buffer_pixels(target);
    test_check(rc == AXL_OK, "shadow: transparent src returns AXL_OK");
    test_check(tp[20 * 40 + 20].red == 0xFF,
               "shadow: transparent src leaves target unchanged");
    axl_gfx_buffer_free(src);
    axl_gfx_buffer_free(target);
}

static void
test_shadow_soft_and_bounded(void)
{
    /* A solid 8x8 opaque square drawn at (20,20) casts a blurred black
       shadow on a white background. */
    AxlGfxBuffer *target = axl_gfx_buffer_new(48, 48);
    AxlGfxPixel   white  = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(target, white);
    AxlGfxBuffer *src = axl_gfx_buffer_new(8, 8);
    AxlGfxPixel   solid = {0x00, 0x00, 0x00, 0xFF};   /* alpha = full coverage */
    axl_gfx_buffer_clear(src, solid);

    axl_gfx_target_buffer(target);
    axl_gfx_draw_shadow(src, 20, 20, AXL_GFX_BLACK, 4);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *tp = axl_gfx_buffer_pixels(target);
    uint8_t core = tp[24 * 48 + 24].red;   /* shadow centre over white */
    uint8_t edge = tp[24 * 48 + 17].red;   /* soft tail left of the square */
    uint8_t far  = tp[2  * 48 + 2 ].red;   /* well outside the shadow */

    test_check(core < 120,
               "shadow: core is darkened (black shadow over white)");
    test_check(far == 0xFF,
               "shadow: extent is bounded (far pixel untouched)");
    test_check(core < edge && edge < 0xFF,
               "shadow: softly fades outward (core < edge < background)");
    axl_gfx_buffer_free(src);
    axl_gfx_buffer_free(target);
}

static void
test_shadow_color_not_darkened_at_edges(void)
{
    /* A RED shadow over a white background. Red-over-white preserves
       the red channel (255) at every shadow alpha; only green/blue
       drop. If the blur darkened the shadow's RGB toward black at its
       soft edges (the bug the "uniform RGB" fill guards against), the
       edge red channel would dip below 255. So: red stays ~255 across
       the whole shadow, while green is clearly reduced where the
       shadow is dense. */
    AxlGfxBuffer *target = axl_gfx_buffer_new(48, 48);
    AxlGfxPixel   white  = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(target, white);
    AxlGfxBuffer *src = axl_gfx_buffer_new(8, 8);
    AxlGfxPixel   solid = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(src, solid);

    axl_gfx_target_buffer(target);
    axl_gfx_draw_shadow(src, 20, 20, AXL_GFX_RED, 4);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *tp = axl_gfx_buffer_pixels(target);
    bool red_preserved = true;
    for (int i = 0; i < 48 * 48; i++) {
        if (tp[i].red < 250) { red_preserved = false; break; }
    }
    test_check(red_preserved,
               "shadow: colored shadow keeps its hue at soft edges "
               "(no darkening toward black)");
    test_check(tp[24 * 48 + 24].green < 150,
               "shadow: red shadow is actually present (green reduced at core)");
    axl_gfx_buffer_free(src);
    axl_gfx_buffer_free(target);
}

// ---------------------------------------------------------------------------
// Display list (G9) — record / introspect / replay
// ---------------------------------------------------------------------------

/* Byte-exact pixel equality of two same-size buffers (all 4 channels). */
static bool
dl_buffers_equal(
    AxlGfxBuffer  *a,
    AxlGfxBuffer  *b
    )
{
    uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
    axl_gfx_buffer_get_info(a, &aw, &ah);
    axl_gfx_buffer_get_info(b, &bw, &bh);
    if (aw != bw || ah != bh) {
        return false;
    }
    const AxlGfxPixel *pa = axl_gfx_buffer_pixels(a);
    const AxlGfxPixel *pb = axl_gfx_buffer_pixels(b);
    size_t n = (size_t)aw * ah;
    for (size_t i = 0; i < n; i++) {
        if (pa[i].blue != pb[i].blue || pa[i].green != pb[i].green
            || pa[i].red != pb[i].red || pa[i].alpha != pb[i].alpha) {
            return false;
        }
    }
    return true;
}

static const AxlGfxPixel DL_BG    = { 0x00, 0x00, 0x00, 0xFF };
static const AxlGfxPixel DL_BLUE  = { 0xFF, 0x00, 0x00, 0xFF };
static const AxlGfxPixel DL_RED   = { 0x00, 0x00, 0xFF, 0xFF };
static const AxlGfxPixel DL_WHITE = { 0xFF, 0xFF, 0xFF, 0xFF };

// --- lifecycle / introspection --------------------------------------

static void
test_dl_new_empty(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    test_check(dl != NULL, "dl: new returns non-NULL");
    test_check(axl_gfx_display_list_count(dl) == 0, "dl: new list is empty");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_null_safety(void)
{
    axl_gfx_display_list_free(NULL);   /* must not crash */
    axl_gfx_display_list_clear(NULL);  /* must not crash */
    test_check(axl_gfx_display_list_count(NULL) == 0, "dl: count(NULL) == 0");
    test_check(axl_gfx_display_list_op_at(NULL, 0) == NULL,
               "dl: op_at(NULL) == NULL");
    test_check(axl_gfx_dl_fill_rect(NULL, 0, 0, 1, 1, DL_BLUE) == AXL_ERR,
               "dl: record on NULL list returns AXL_ERR");
    test_check(axl_gfx_display_list_replay(NULL) == AXL_ERR,
               "dl: replay(NULL) returns AXL_ERR");
}

static void
test_dl_op_at_out_of_range(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_fill_rect(dl, 0, 0, 1, 1, DL_BLUE);
    test_check(axl_gfx_display_list_op_at(dl, 1) == NULL,
               "dl: op_at past the end returns NULL");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_clear_resets(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_fill_rect(dl, 0, 0, 1, 1, DL_BLUE);
    axl_gfx_dl_clear(dl, DL_RED);
    test_check(axl_gfx_display_list_count(dl) == 2, "dl: two ops recorded");
    axl_gfx_display_list_clear(dl);
    test_check(axl_gfx_display_list_count(dl) == 0,
               "dl: clear() empties the list");
    /* Reusable after clear. */
    axl_gfx_dl_fill_rect(dl, 0, 0, 2, 2, DL_WHITE);
    test_check(axl_gfx_display_list_count(dl) == 1,
               "dl: list is reusable after clear");
    axl_gfx_display_list_free(dl);
}

// --- per-op parameter capture ---------------------------------------

static void
test_dl_records_fill_rect_params(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_fill_rect(dl, 3, 7, 100, 40, DL_BLUE);
    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_FILL_RECT, "dl: fill_rect kind");
    test_check(op->u.rect_u.x == 3 && op->u.rect_u.y == 7
               && op->u.rect_u.w == 100 && op->u.rect_u.h == 40,
               "dl: fill_rect coords captured exactly");
    test_check(op->u.rect_u.color.blue == 0xFF && op->u.rect_u.color.red == 0,
               "dl: fill_rect color captured");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_records_fill_rect_i_negative(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_fill_rect_i(dl, -5, -9, 20, 30, DL_RED);
    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_FILL_RECT_I, "dl: fill_rect_i kind");
    test_check(op->u.rect_i.x == -5 && op->u.rect_i.y == -9
               && op->u.rect_i.w == 20 && op->u.rect_i.h == 30,
               "dl: fill_rect_i preserves negative coords");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_records_line_and_rect_and_rounded(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_draw_line(dl, 1, 2, 3, 4, DL_WHITE);
    axl_gfx_dl_draw_rect(dl, 5, 6, 7, 8, DL_BLUE);
    axl_gfx_dl_fill_rounded_rect(dl, -2, -3, 40, 50, 6.5f, DL_RED);

    const AxlGfxOp *l = axl_gfx_display_list_op_at(dl, 0);
    test_check(l->kind == AXL_GFX_OP_DRAW_LINE
               && l->u.line.x0 == 1 && l->u.line.y0 == 2
               && l->u.line.x1 == 3 && l->u.line.y1 == 4,
               "dl: draw_line endpoints captured");
    const AxlGfxOp *r = axl_gfx_display_list_op_at(dl, 1);
    test_check(r->kind == AXL_GFX_OP_DRAW_RECT
               && r->u.rect_u.w == 7 && r->u.rect_u.h == 8,
               "dl: draw_rect captured");
    const AxlGfxOp *rr = axl_gfx_display_list_op_at(dl, 2);
    test_check(rr->kind == AXL_GFX_OP_FILL_ROUNDED_RECT
               && rr->u.rounded_rect.x == -2 && rr->u.rounded_rect.radius == 6.5f,
               "dl: fill_rounded_rect captured with radius");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_records_clip_ops(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxClip c = { 4, 5, 10, 12 };
    axl_gfx_dl_push_clip(dl, c);
    axl_gfx_dl_pop_clip(dl);
    const AxlGfxOp *p = axl_gfx_display_list_op_at(dl, 0);
    test_check(p->kind == AXL_GFX_OP_PUSH_CLIP
               && p->u.push_clip.rect.x == 4 && p->u.push_clip.rect.w == 10,
               "dl: push_clip rect captured");
    test_check(axl_gfx_display_list_op_at(dl, 1)->kind == AXL_GFX_OP_POP_CLIP,
               "dl: pop_clip recorded");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_records_fill_path_borrows(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 0, 0);
    axl_gfx_path_line_to(path, 10, 0);
    axl_gfx_path_line_to(path, 5, 10);
    axl_gfx_path_close(path);

    axl_gfx_dl_fill_path(dl, path, DL_WHITE);
    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_FILL_PATH, "dl: fill_path kind");
    test_check(op->u.fill_path.path == path,
               "dl: fill_path borrows the caller's path pointer");
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
}

// --- copy independence (the retained-list correctness property) ------

static void
test_dl_polyline_copies_points(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPoint pts[3] = { { 0, 0 }, { 10, 5 }, { 20, 0 } };
    axl_gfx_dl_draw_polyline(dl, pts, 3, DL_WHITE);
    /* Mutate the caller's array AFTER recording: the op must hold an
     * independent copy. */
    pts[1].x = 999;

    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_DRAW_POLYLINE && op->u.polyline.count == 3,
               "dl: polyline count captured");
    test_check(op->u.polyline.points != pts,
               "dl: polyline points are a distinct copy, not the caller's array");
    test_check(op->u.polyline.points[1].x == 10,
               "dl: polyline copy is unaffected by post-record caller mutation");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_text_copies_string(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    char buf[8];
    buf[0] = 'h'; buf[1] = 'i'; buf[2] = '\0';
    axl_gfx_dl_draw_text(dl, axl_gfx_default_font(), 2, 3, buf, DL_WHITE, 1);
    buf[0] = 'X';   /* clobber caller's buffer after recording */

    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_DRAW_TEXT, "dl: draw_text kind");
    test_check(op->u.text.text != buf && axl_strcmp(op->u.text.text, "hi") == 0,
               "dl: draw_text copies the string (caller clobber ignored)");
    test_check(op->u.text.scale == 1 && op->u.text.x == 2,
               "dl: draw_text scale/coords captured");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_text_ttf_copies_string(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    char buf[8];
    buf[0] = 'A'; buf[1] = '\0';
    axl_gfx_dl_draw_text_ttf(dl, axl_ttf_default(), -4, 12, buf, 18.0f, DL_RED);
    buf[0] = 'Z';

    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_DRAW_TEXT_TTF, "dl: draw_text_ttf kind");
    test_check(axl_strcmp(op->u.text_ttf.text, "A") == 0,
               "dl: draw_text_ttf copies the string");
    test_check(op->u.text_ttf.px_size == 18.0f && op->u.text_ttf.x == -4,
               "dl: draw_text_ttf px_size + signed baseline x captured");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_stroke_copies_dashes(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 0, 0);
    axl_gfx_path_line_to(path, 50, 0);

    float dashes[2] = { 4.0f, 2.0f };
    AxlGfxStrokeStyle style = { 0 };
    style.width    = 2.0f;
    style.dashes   = dashes;
    style.n_dashes = 2;
    axl_gfx_dl_stroke_path(dl, path, DL_WHITE, &style);
    dashes[0] = 99.0f;   /* mutate caller's dash array after recording */

    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_STROKE_PATH, "dl: stroke_path kind");
    test_check(op->u.stroke_path.path == path, "dl: stroke_path borrows path");
    test_check(op->u.stroke_path.style.dashes != dashes
               && op->u.stroke_path.style.n_dashes == 2,
               "dl: stroke_path deep-copies the dash array");
    test_check(op->u.stroke_path.style.dashes[0] == 4.0f,
               "dl: dash copy unaffected by post-record caller mutation");
    test_check(op->u.stroke_path.style.width == 2.0f,
               "dl: stroke_path style width captured");
    test_check(op->u.stroke_path.style.cap == style.cap
               && op->u.stroke_path.style.join == style.join,
               "dl: stroke_path copies the full style by value (cap/join)");
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
}

static void
test_dl_blit_copies_pixels(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPixel src[4] = { DL_BLUE, DL_RED, DL_WHITE, DL_BG };
    axl_gfx_dl_blit(dl, src, 1, 1, 2, 2);
    src[0] = DL_RED;   /* mutate after recording */

    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 0);
    test_check(op->kind == AXL_GFX_OP_BLIT && op->u.blit.w == 2 && op->u.blit.h == 2,
               "dl: blit dims captured");
    test_check(op->u.blit.x == 1 && op->u.blit.y == 1,
               "dl: blit destination origin captured");
    test_check(op->u.blit.pixels != src && op->u.blit.pixels[0].blue == 0xFF,
               "dl: blit deep-copies the source pixels");
    axl_gfx_display_list_free(dl);
}

// --- validation -----------------------------------------------------

static void
test_dl_validation_errors(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPoint pts[1] = { { 0, 0 } };
    AxlGfxPixel px = DL_BLUE;
    AxlGfxStrokeStyle style = { 0 };
    style.width = 1.0f;
    AxlGfxPath *path = axl_gfx_path_new();

    test_check(axl_gfx_dl_draw_polyline(dl, NULL, 3, px) == AXL_ERR,
               "dl: polyline NULL points -> AXL_ERR");
    test_check(axl_gfx_dl_draw_polyline(dl, pts, 1, px) == AXL_ERR,
               "dl: polyline count < 2 -> AXL_ERR");
    test_check(axl_gfx_dl_blit(dl, NULL, 0, 0, 2, 2) == AXL_ERR,
               "dl: blit NULL buffer -> AXL_ERR");
    test_check(axl_gfx_dl_blit(dl, (const AxlGfxPixel *)pts, 0, 0, 0, 2) == AXL_ERR,
               "dl: blit zero width -> AXL_ERR");
    test_check(axl_gfx_dl_fill_path(dl, NULL, px) == AXL_ERR,
               "dl: fill_path NULL path -> AXL_ERR");
    test_check(axl_gfx_dl_stroke_path(dl, NULL, px, &style) == AXL_ERR,
               "dl: stroke_path NULL path -> AXL_ERR");
    test_check(axl_gfx_dl_stroke_path(dl, path, px, NULL) == AXL_ERR,
               "dl: stroke_path NULL style -> AXL_ERR");
    test_check(axl_gfx_dl_draw_text(dl, NULL, 0, 0, "x", px, 1) == AXL_ERR,
               "dl: draw_text NULL font -> AXL_ERR");
    test_check(axl_gfx_dl_draw_text(dl, axl_gfx_default_font(), 0, 0, NULL, px, 1)
               == AXL_ERR,
               "dl: draw_text NULL text -> AXL_ERR");
    test_check(axl_gfx_dl_draw_text_ttf(dl, NULL, 0, 0, "x", 12.0f, px) == AXL_ERR,
               "dl: draw_text_ttf NULL ttf -> AXL_ERR");
    /* No failed record appended anything. */
    test_check(axl_gfx_display_list_count(dl) == 0,
               "dl: rejected records append nothing");
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
}

// --- replay (byte-identical vs immediate mode) ----------------------

static void
test_dl_replay_matches_immediate(void)
{
    /* Build a small scene two ways: directly (reference) and via a
     * recorded+replayed display list. The buffers must be identical. */
    AxlGfxPath *tri = axl_gfx_path_new();
    axl_gfx_path_move_to(tri, 10, 10);
    axl_gfx_path_line_to(tri, 40, 12);
    axl_gfx_path_line_to(tri, 20, 45);
    axl_gfx_path_close(tri);

    /* Reference: immediate mode. */
    AxlGfxBuffer *ref = axl_gfx_buffer_new(64, 64);
    axl_gfx_buffer_clear(ref, DL_BG);
    axl_gfx_target_buffer(ref);
    axl_gfx_fill_rect_i(2, 2, 30, 20, DL_BLUE);
    axl_gfx_draw_line(0, 0, 63, 63, DL_WHITE);
    axl_gfx_fill_path(tri, DL_RED);
    axl_gfx_fill_rounded_rect(35, 35, 24, 24, 6.0f, DL_WHITE);
    axl_gfx_target_buffer(NULL);

    /* Recorded + replayed. */
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_fill_rect_i(dl, 2, 2, 30, 20, DL_BLUE);
    axl_gfx_dl_draw_line(dl, 0, 0, 63, 63, DL_WHITE);
    axl_gfx_dl_fill_path(dl, tri, DL_RED);
    axl_gfx_dl_fill_rounded_rect(dl, 35, 35, 24, 24, 6.0f, DL_WHITE);

    AxlGfxBuffer *got = axl_gfx_buffer_new(64, 64);
    axl_gfx_buffer_clear(got, DL_BG);
    axl_gfx_target_buffer(got);
    int rc = axl_gfx_display_list_replay(dl);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "dl: replay of a buffer scene returns AXL_OK");
    test_check(dl_buffers_equal(ref, got),
               "dl: replayed scene is byte-identical to immediate mode");

    axl_gfx_buffer_free(ref);
    axl_gfx_buffer_free(got);
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(tri);
}

static void
test_dl_replay_clear_fills_buffer(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(16, 16);
    axl_gfx_buffer_clear(b, DL_BG);
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_clear(dl, DL_RED);

    axl_gfx_target_buffer(b);
    axl_gfx_display_list_replay(dl);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    bool all_red = true;
    for (size_t i = 0; i < 16u * 16u; i++) {
        if (p[i].red != 0xFF || p[i].blue != 0 || p[i].green != 0) {
            all_red = false;
            break;
        }
    }
    test_check(all_red, "dl: replay CLEAR fills the active buffer target");
    axl_gfx_buffer_free(b);
    axl_gfx_display_list_free(dl);
}

static void
test_dl_replay_honors_recorded_clip(void)
{
    /* push_clip + a full-buffer fill + pop_clip: only the clip region
     * should be painted on replay. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    axl_gfx_buffer_clear(b, DL_BG);
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxClip clip = { 10, 10, 8, 8 };
    axl_gfx_dl_push_clip(dl, clip);
    axl_gfx_dl_fill_rect_i(dl, 0, 0, 40, 40, DL_BLUE);
    axl_gfx_dl_pop_clip(dl);

    axl_gfx_target_buffer(b);
    axl_gfx_display_list_replay(dl);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(p[12 * 40 + 12].blue == 0xFF,
               "dl: replayed clip keeps the inside painted");
    test_check(p[0].blue == 0x00 && p[39 * 40 + 39].blue == 0x00,
               "dl: replayed clip rejects pixels outside the clip rect");
    /* The clip was popped during replay — the stack is balanced. */
    test_check(axl_gfx_get_clip(&clip) == AXL_ERR,
               "dl: replay leaves the clip stack balanced (pop ran)");
    axl_gfx_buffer_free(b);
    axl_gfx_display_list_free(dl);
}

static void
test_dl_replay_empty_is_ok(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    test_check(axl_gfx_display_list_replay(dl) == AXL_OK,
               "dl: replay of an empty list returns AXL_OK");
    axl_gfx_display_list_free(dl);
}

// --- OOM ------------------------------------------------------------

static void
test_dl_oom_new(void)
{
    axl_mem_fail_next_alloc(1);
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    test_check(dl == NULL, "dl: new returns NULL when first alloc fails");
}

static void
test_dl_oom_record_copy(void)
{
    /* draw_text's first allocation is the string copy; failing it must
     * reject the record cleanly with nothing appended. */
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_mem_fail_next_alloc(1);
    int rc = axl_gfx_dl_draw_text(dl, axl_gfx_default_font(), 0, 0, "hello",
                                  DL_WHITE, 1);
    test_check(rc == AXL_ERR, "dl: record returns AXL_ERR on copy OOM");
    test_check(axl_gfx_display_list_count(dl) == 0,
               "dl: nothing appended when a copy allocation fails");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_oom_record_append(void)
{
    /* Exercise the dl_push-failed cleanup branch: the string copy
     * succeeds but the backing array's grow-append fails, so the record
     * must free the orphaned copy and append nothing (a leak otherwise).
     *
     * The op array starts at a fixed capacity, so an append only
     * reallocates once the array is full. Fill it to capacity with
     * cheap by-value ops first, then the next copy-record's append is
     * the one that grows — and we fail that grow as the second alloc
     * (copy = first alloc, succeeds; grow = second, fails). */
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    while (axl_gfx_display_list_count(dl) < 16) {
        axl_gfx_dl_fill_rect(dl, 0, 0, 1, 1, DL_BLUE);
    }
    size_t before = axl_gfx_display_list_count(dl);

    axl_mem_fail_next_alloc(2);
    int rc = axl_gfx_dl_draw_text(dl, axl_gfx_default_font(), 0, 0, "hello",
                                  DL_WHITE, 1);
    test_check(rc == AXL_ERR, "dl: record returns AXL_ERR on append-grow OOM");
    test_check(axl_gfx_display_list_count(dl) == before,
               "dl: nothing appended when the array grow fails");
    /* Still usable; the orphaned copy was reclaimed. */
    test_check(axl_gfx_dl_fill_rect(dl, 0, 0, 1, 1, DL_BLUE) == AXL_OK
               && axl_gfx_display_list_count(dl) == before + 1,
               "dl: list still records normally after an append-OOM reject");
    axl_gfx_display_list_free(dl);
}

// --- slice 2: gradient + transform ops ------------------------------

/* Build a simple 2-stop blue→red linear gradient for replay tests. */
static AxlGfxGradient *
dl_make_gradient(void)
{
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 32, 0);
    axl_gfx_gradient_add_stop(g, 0.0f, DL_BLUE);
    axl_gfx_gradient_add_stop(g, 1.0f, DL_RED);
    return g;
}

static void
test_dl_records_gradient_ops(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxGradient *g = dl_make_gradient();
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 0, 0);
    axl_gfx_path_line_to(path, 10, 0);
    axl_gfx_path_line_to(path, 5, 10);
    axl_gfx_path_close(path);

    axl_gfx_dl_fill_rect_gradient(dl, -2, 3, 40, 50, g);
    axl_gfx_dl_fill_path_gradient(dl, path, g);
    axl_gfx_dl_fill_rounded_rect_gradient(dl, 1, 2, 20, 22, 4.5f, g);

    const AxlGfxOp *a = axl_gfx_display_list_op_at(dl, 0);
    test_check(a->kind == AXL_GFX_OP_FILL_RECT_GRADIENT
               && a->u.rect_gradient.x == -2 && a->u.rect_gradient.h == 50
               && a->u.rect_gradient.g == g,
               "dl: fill_rect_gradient captures coords + borrows gradient");
    const AxlGfxOp *b = axl_gfx_display_list_op_at(dl, 1);
    test_check(b->kind == AXL_GFX_OP_FILL_PATH_GRADIENT
               && b->u.path_gradient.path == path && b->u.path_gradient.g == g,
               "dl: fill_path_gradient borrows path + gradient");
    const AxlGfxOp *c = axl_gfx_display_list_op_at(dl, 2);
    test_check(c->kind == AXL_GFX_OP_FILL_ROUNDED_RECT_GRADIENT
               && c->u.rounded_rect_gradient.radius == 4.5f
               && c->u.rounded_rect_gradient.g == g,
               "dl: fill_rounded_rect_gradient captures radius + borrows gradient");

    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
    axl_gfx_gradient_free(g);
}

static void
test_dl_gradient_validation(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPath *path = axl_gfx_path_new();
    AxlGfxGradient *g = dl_make_gradient();
    test_check(axl_gfx_dl_fill_rect_gradient(dl, 0, 0, 1, 1, NULL) == AXL_ERR,
               "dl: fill_rect_gradient NULL gradient -> AXL_ERR");
    test_check(axl_gfx_dl_fill_path_gradient(dl, path, NULL) == AXL_ERR,
               "dl: fill_path_gradient NULL gradient -> AXL_ERR");
    test_check(axl_gfx_dl_fill_path_gradient(dl, NULL, g) == AXL_ERR,
               "dl: fill_path_gradient NULL path -> AXL_ERR");
    test_check(axl_gfx_dl_fill_rounded_rect_gradient(dl, 0, 0, 1, 1, 1.0f, NULL)
               == AXL_ERR,
               "dl: fill_rounded_rect_gradient NULL gradient -> AXL_ERR");
    test_check(axl_gfx_display_list_count(dl) == 0,
               "dl: rejected gradient records append nothing");
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
    axl_gfx_gradient_free(g);
}

static void
test_dl_records_transform_ops(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_translate(dl, 12.5, -7.25);
    axl_gfx_dl_scale(dl, 2.0, 3.0);
    axl_gfx_dl_rotate(dl, 1.5);
    axl_gfx_dl_skew(dl, 0.25, 0.5);
    axl_gfx_dl_push_transform(dl);
    axl_gfx_dl_pop_transform(dl);
    axl_gfx_dl_reset_transform(dl);

    test_check(axl_gfx_display_list_count(dl) == 7,
               "dl: seven transform ops recorded");
    const AxlGfxOp *t = axl_gfx_display_list_op_at(dl, 0);
    test_check(t->kind == AXL_GFX_OP_TRANSLATE
               && t->u.translate.tx == 12.5 && t->u.translate.ty == -7.25,
               "dl: translate args captured exactly");
    const AxlGfxOp *s = axl_gfx_display_list_op_at(dl, 1);
    test_check(s->kind == AXL_GFX_OP_SCALE
               && s->u.scale.sx == 2.0 && s->u.scale.sy == 3.0,
               "dl: scale args captured");
    const AxlGfxOp *r = axl_gfx_display_list_op_at(dl, 2);
    test_check(r->kind == AXL_GFX_OP_ROTATE && r->u.rotate.radians == 1.5,
               "dl: rotate radians captured");
    const AxlGfxOp *k = axl_gfx_display_list_op_at(dl, 3);
    test_check(k->kind == AXL_GFX_OP_SKEW
               && k->u.skew.sx == 0.25 && k->u.skew.sy == 0.5,
               "dl: skew args captured");
    test_check(axl_gfx_display_list_op_at(dl, 4)->kind == AXL_GFX_OP_PUSH_TRANSFORM
               && axl_gfx_display_list_op_at(dl, 5)->kind == AXL_GFX_OP_POP_TRANSFORM
               && axl_gfx_display_list_op_at(dl, 6)->kind == AXL_GFX_OP_RESET_TRANSFORM,
               "dl: push/pop/reset_transform recorded in order");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_replay_gradient_matches_immediate(void)
{
    AxlGfxGradient *g = dl_make_gradient();

    AxlGfxBuffer *ref = axl_gfx_buffer_new(40, 24);
    axl_gfx_buffer_clear(ref, DL_BG);
    axl_gfx_target_buffer(ref);
    axl_gfx_fill_rect_gradient(2, 2, 32, 18, g);
    axl_gfx_target_buffer(NULL);

    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_fill_rect_gradient(dl, 2, 2, 32, 18, g);
    AxlGfxBuffer *got = axl_gfx_buffer_new(40, 24);
    axl_gfx_buffer_clear(got, DL_BG);
    axl_gfx_target_buffer(got);
    int rc = axl_gfx_display_list_replay(dl);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "dl: gradient replay returns AXL_OK");
    test_check(dl_buffers_equal(ref, got),
               "dl: replayed gradient fill is byte-identical to immediate mode");

    axl_gfx_buffer_free(ref);
    axl_gfx_buffer_free(got);
    axl_gfx_display_list_free(dl);
    axl_gfx_gradient_free(g);
}

static void
test_dl_replay_transform_affects_path(void)
{
    /* A recorded translate must shift a subsequently-recorded path fill
     * on replay, identically to immediate mode (paths are transform-
     * aware). */
    AxlGfxPath *tri = axl_gfx_path_new();
    axl_gfx_path_move_to(tri, 0, 0);
    axl_gfx_path_line_to(tri, 12, 0);
    axl_gfx_path_line_to(tri, 6, 12);
    axl_gfx_path_close(tri);

    axl_gfx_reset_transform();
    AxlGfxBuffer *ref = axl_gfx_buffer_new(48, 48);
    axl_gfx_buffer_clear(ref, DL_BG);
    axl_gfx_target_buffer(ref);
    axl_gfx_translate(20.0, 24.0);
    axl_gfx_fill_path(tri, DL_WHITE);
    axl_gfx_target_buffer(NULL);
    axl_gfx_reset_transform();

    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_translate(dl, 20.0, 24.0);
    axl_gfx_dl_fill_path(dl, tri, DL_WHITE);
    AxlGfxBuffer *got = axl_gfx_buffer_new(48, 48);
    axl_gfx_buffer_clear(got, DL_BG);
    axl_gfx_target_buffer(got);
    axl_gfx_display_list_replay(dl);
    axl_gfx_target_buffer(NULL);
    axl_gfx_reset_transform();

    test_check(dl_buffers_equal(ref, got),
               "dl: replayed translate shifts the path fill (transform replayed)");

    axl_gfx_buffer_free(ref);
    axl_gfx_buffer_free(got);
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(tri);
}

static void
test_dl_replay_push_pop_transform_balances(void)
{
    /* push_transform + pop_transform recorded as a pair must leave the
     * transform stack at its pre-replay depth. Start from a known-empty
     * stack (reset), replay the balanced pair, then an extra pop must
     * fail (stack empty) — proving the recorded pop ran. */
    axl_gfx_reset_transform();
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_push_transform(dl);
    axl_gfx_dl_translate(dl, 50.0, 50.0);
    axl_gfx_dl_pop_transform(dl);

    int rc = axl_gfx_display_list_replay(dl);
    test_check(rc == AXL_OK, "dl: balanced transform replay returns AXL_OK");
    test_check(axl_gfx_pop_transform() == AXL_ERR,
               "dl: recorded push/pop balanced (stack empty after replay)");

    axl_gfx_reset_transform();
    axl_gfx_display_list_free(dl);
}

// --- slice 3: textual dump ------------------------------------------

/* Dump @a dl into a caller buffer, NUL-terminated. Returns byte length
 * written (excluding the NUL), or (size_t)-1 on dump error or if the
 * output would overflow @a bufsz. */
static size_t
dl_dump_to(
    AxlGfxDisplayList  *dl,
    char               *buf,
    size_t              bufsz
    )
{
    AxlStream *s = axl_bufopen();
    if (!s) {
        return (size_t)-1;
    }
    int          rc   = axl_gfx_display_list_dump(dl, s);
    size_t       size = 0;
    const void  *data = axl_bufdata(s, &size);
    size_t       out  = (size_t)-1;
    if (rc == AXL_OK && size < bufsz) {
        if (size > 0) {
            axl_memcpy(buf, data, size);
        }
        buf[size] = '\0';
        out = size;
    }
    axl_fclose(s);
    return out;
}

/* Exact-string compare of a dump against @a expected. */
static bool
dl_dump_equals(
    AxlGfxDisplayList  *dl,
    const char         *expected
    )
{
    char   buf[1024];
    size_t n = dl_dump_to(dl, buf, sizeof buf);
    if (n == (size_t)-1) {
        return false;
    }
    return axl_strcmp(buf, expected) == 0;
}

static void
test_dl_dump_null_args(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlStream *s = axl_bufopen();
    test_check(axl_gfx_display_list_dump(NULL, s) == AXL_ERR,
               "dump: NULL list -> AXL_ERR");
    test_check(axl_gfx_display_list_dump(dl, NULL) == AXL_ERR,
               "dump: NULL stream -> AXL_ERR");
    axl_fclose(s);
    axl_gfx_display_list_free(dl);
}

static void
test_dl_dump_empty_writes_nothing(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    char buf[16];
    size_t n = dl_dump_to(dl, buf, sizeof buf);
    test_check(n == 0, "dump: empty list writes zero bytes");
    test_check(buf[0] == '\0', "dump: empty list yields empty string");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_dump_basic_ops(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_fill_rect(dl, 0, 0, 100, 40, DL_BLUE);
    axl_gfx_dl_fill_rect_i(dl, -5, -9, 20, 30, DL_RED);
    axl_gfx_dl_pop_clip(dl);
    test_check(dl_dump_equals(dl,
        "0: fill_rect x=0 y=0 w=100 h=40 color=#0000FFFF\n"
        "1: fill_rect_i x=-5 y=-9 w=20 h=30 color=#FF0000FF\n"
        "2: pop_clip\n"),
        "dump: basic ops produce exact line-per-op trace");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_dump_clip_and_line(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxClip c = { 4, 5, 10, 12 };
    axl_gfx_dl_push_clip(dl, c);
    axl_gfx_dl_draw_line(dl, 1, 2, 3, 4, DL_WHITE);
    test_check(dl_dump_equals(dl,
        "0: push_clip x=4 y=5 w=10 h=12\n"
        "1: draw_line x0=1 y0=2 x1=3 y1=4 color=#FFFFFFFF\n"),
        "dump: clip + line exact trace");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_dump_floats(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 0, 0);
    axl_gfx_path_line_to(path, 10, 0);
    AxlGfxStrokeStyle style = { 0 };
    style.width = 2.0f;
    axl_gfx_dl_stroke_path(dl, path, DL_WHITE, &style);
    axl_gfx_dl_fill_rounded_rect(dl, 1, 2, 20, 22, 6.5f, DL_RED);
    axl_gfx_dl_translate(dl, 12.5, -7.25);
    test_check(dl_dump_equals(dl,
        "0: stroke_path width=2.000 cap=0 join=0 miter=0.000 dashes=0 color=#FFFFFFFF\n"
        "1: fill_rounded_rect x=1 y=2 w=20 h=22 radius=6.500 color=#FF0000FF\n"
        "2: translate tx=12.500 ty=-7.250\n"),
        "dump: float fields print with 3 decimals");
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
}

static void
test_dl_dump_scale_rotate_skew(void)
{
    /* scale / rotate / skew each have distinct field names + two-vs-one
     * args; pin them exactly so a swapped field or wrong label is
     * caught (the all-kinds test only counts lines). Values are exact
     * dyadic rationals so the %.3f output is arch-independent. */
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_scale(dl, 2.0, 0.5);
    axl_gfx_dl_rotate(dl, 1.25);
    axl_gfx_dl_skew(dl, -0.25, 0.75);
    test_check(dl_dump_equals(dl,
        "0: scale sx=2.000 sy=0.500\n"
        "1: rotate rad=1.250\n"
        "2: skew sx=-0.250 sy=0.750\n"),
        "dump: scale/rotate/skew exact trace");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_dump_gradient_ops(void)
{
    /* The three gradient ops print geometry only (the gradient handle
     * is borrowed, not serialized). Pin field names + signed coords. */
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxGradient *g = dl_make_gradient();
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 0, 0);
    axl_gfx_path_line_to(path, 4, 0);
    axl_gfx_dl_fill_rect_gradient(dl, -2, 3, 40, 50, g);
    axl_gfx_dl_fill_path_gradient(dl, path, g);
    axl_gfx_dl_fill_rounded_rect_gradient(dl, 1, 2, 20, 22, 4.5f, g);
    test_check(dl_dump_equals(dl,
        "0: fill_rect_gradient x=-2 y=3 w=40 h=50\n"
        "1: fill_path_gradient\n"
        "2: fill_rounded_rect_gradient x=1 y=2 w=20 h=22 radius=4.500\n"),
        "dump: gradient ops exact trace");
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
    axl_gfx_gradient_free(g);
}

static void
test_dl_dump_text_escaping(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    /* q " \ <tab> <nl> <cr> z — each special char escaped, one line. */
    char in[] = { 'q', '"', '\\', '\t', '\n', '\r', 'z', '\0' };
    axl_gfx_dl_draw_text(dl, axl_gfx_default_font(), 0, 0, in, DL_BG, 1);
    test_check(dl_dump_equals(dl,
        "0: draw_text x=0 y=0 scale=1 color=#000000FF text=\"q\\\"\\\\\\t\\n\\rz\"\n"),
        "dump: text escapes backslash quote tab nl cr and stays one line");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_dump_text_ttf(void)
{
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_draw_text_ttf(dl, axl_ttf_default(), -4, 12, "A", 18.0f, DL_RED);
    test_check(dl_dump_equals(dl,
        "0: draw_text_ttf x=-4 y=12 px=18.000 color=#FF0000FF text=\"A\"\n"),
        "dump: draw_text_ttf exact trace");
    axl_gfx_display_list_free(dl);
}

static void
test_dl_dump_all_kinds_one_line_each(void)
{
    /* Record one op of every kind; the dump must emit exactly one line
     * per op — proving no kind falls through to the unreachable ERR and
     * every kind has a printable name. */
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 0, 0);
    axl_gfx_path_line_to(path, 4, 0);
    AxlGfxGradient *g = dl_make_gradient();
    AxlGfxPoint pts[2] = { { 0, 0 }, { 5, 5 } };
    AxlGfxPixel src[1] = { DL_BLUE };
    AxlGfxClip clip = { 0, 0, 1, 1 };
    AxlGfxStrokeStyle style = { 0 };
    style.width = 1.0f;

    axl_gfx_dl_fill_rect(dl, 0, 0, 1, 1, DL_BLUE);
    axl_gfx_dl_fill_rect_i(dl, 0, 0, 1, 1, DL_BLUE);
    axl_gfx_dl_draw_line(dl, 0, 0, 1, 1, DL_BLUE);
    axl_gfx_dl_draw_rect(dl, 0, 0, 1, 1, DL_BLUE);
    axl_gfx_dl_draw_polyline(dl, pts, 2, DL_BLUE);
    axl_gfx_dl_blit(dl, src, 0, 0, 1, 1);
    axl_gfx_dl_clear(dl, DL_BLUE);
    axl_gfx_dl_push_clip(dl, clip);
    axl_gfx_dl_pop_clip(dl);
    axl_gfx_dl_fill_path(dl, path, DL_BLUE);
    axl_gfx_dl_stroke_path(dl, path, DL_BLUE, &style);
    axl_gfx_dl_fill_rounded_rect(dl, 0, 0, 1, 1, 0.0f, DL_BLUE);
    axl_gfx_dl_draw_text(dl, axl_gfx_default_font(), 0, 0, "x", DL_BLUE, 1);
    axl_gfx_dl_draw_text_ttf(dl, axl_ttf_default(), 0, 0, "x", 8.0f, DL_BLUE);
    axl_gfx_dl_fill_rect_gradient(dl, 0, 0, 1, 1, g);
    axl_gfx_dl_fill_path_gradient(dl, path, g);
    axl_gfx_dl_fill_rounded_rect_gradient(dl, 0, 0, 1, 1, 0.0f, g);
    axl_gfx_dl_translate(dl, 1, 1);
    axl_gfx_dl_scale(dl, 1, 1);
    axl_gfx_dl_rotate(dl, 1);
    axl_gfx_dl_skew(dl, 1, 1);
    axl_gfx_dl_push_transform(dl);
    axl_gfx_dl_pop_transform(dl);
    axl_gfx_dl_reset_transform(dl);

    size_t count = axl_gfx_display_list_count(dl);
    char   buf[4096];
    size_t n = dl_dump_to(dl, buf, sizeof buf);
    test_check(n != (size_t)-1, "dump: all-kinds dump succeeds and fits");
    size_t lines = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            lines++;
        }
    }
    test_check(count == 24, "dump: all-kinds fixture records all 24 op kinds");
    test_check(lines == count,
               "dump: every recorded op kind emits exactly one line");
    test_check(n > 0 && buf[n - 1] == '\n',
               "dump: trace ends with a newline");
    axl_gfx_display_list_free(dl);
    axl_gfx_path_free(path);
    axl_gfx_gradient_free(g);
}

// ---------------------------------------------------------------------------
// Convex-quad clip (slice 1b)
// ---------------------------------------------------------------------------

/* A 45°-rotated square (L1 "diamond") centered at (16,16), radius 12:
   inside == |x-16| + |y-16| <= 12. Corners at the axis midpoints. */
static void
diamond_corners(AxlGfxPointF q[4])
{
    q[0] = (AxlGfxPointF){ 16,  4 };
    q[1] = (AxlGfxPointF){ 28, 16 };
    q[2] = (AxlGfxPointF){ 16, 28 };
    q[3] = (AxlGfxPointF){  4, 16 };
}

static void
test_clip_quad_axis_aligned_matches_rect(void)
{
    /* An axis-aligned quad must clip identically to the rect (8,8,16,16). */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPointF q[4] = { {8,8}, {24,8}, {24,24}, {8,24} };

    axl_gfx_target_buffer(b);
    test_check(axl_gfx_push_clip_quad(q) == AXL_OK, "push_clip_quad: axis-aligned OK");
    axl_gfx_fill_rect(0, 0, 32, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 32, 16, 16, blu), "quad axis-aligned: center (16,16) filled");
    test_check(px_eq(p, 32,  8,  8, blu), "quad axis-aligned: corner (8,8) filled");
    test_check(px_eq(p, 32, 23, 23, blu), "quad axis-aligned: (23,23) filled");
    test_check(px_eq(p, 32,  7,  7, bg),  "quad axis-aligned: (7,7) outside, bg");
    test_check(px_eq(p, 32, 24, 24, bg),  "quad axis-aligned: (24,24) outside, bg");
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_diamond(void)
{
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPointF q[4]; diamond_corners(q);

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip_quad(q);
    axl_gfx_fill_rect(0, 0, 32, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* Inside the diamond. */
    test_check(px_eq(p, 32, 16, 16, blu), "quad diamond: center filled");
    test_check(px_eq(p, 32, 16,  8, blu), "quad diamond: (16,8) |0.5|+|7.5|=8 inside");
    test_check(px_eq(p, 32, 16, 24, blu), "quad diamond: (16,24) inside");
    /* Outside the diamond but inside its bbox — the rotated-clip payoff. */
    test_check(px_eq(p, 32,  6,  6, bg),  "quad diamond: (6,6) corner outside, bg");
    test_check(px_eq(p, 32, 26,  6, bg),  "quad diamond: (26,6) corner outside, bg");
    test_check(px_eq(p, 32,  6, 26, bg),  "quad diamond: (6,26) corner outside, bg");
    test_check(px_eq(p, 32, 26, 26, bg),  "quad diamond: (26,26) corner outside, bg");
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_winding_independent(void)
{
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPointF ccw[4]; diamond_corners(ccw);
    AxlGfxPointF cw[4]  = { ccw[0], ccw[3], ccw[2], ccw[1] };  /* reversed */

    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_push_clip_quad(cw);
    axl_gfx_fill_rect(0, 0, 32, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 32, 16, 16, blu), "quad winding: reversed winding still fills center");
    test_check(px_eq(p, 32,  6,  6, bg),  "quad winding: reversed winding still rejects corner");
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_intersects_existing_clip(void)
{
    /* Rect clip = left half [0,16); diamond on top. A diamond-inside pixel
       in the right half is excluded by the rect; one in the left half stays. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPointF q[4]; diamond_corners(q);

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip((AxlGfxClip){0, 0, 16, 32});
    axl_gfx_push_clip_quad(q);
    axl_gfx_fill_rect(0, 0, 32, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 32, 12, 16, blu), "quad∩rect: (12,16) in both — filled");
    test_check(px_eq(p, 32, 20, 16, bg),  "quad∩rect: (20,16) in diamond but right half — bg");
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_degenerate_clips_all(void)
{
    AxlGfxBuffer *b   = axl_gfx_buffer_new(16, 16);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPointF q[4] = { {5,5}, {5,5}, {5,5}, {5,5} };   /* zero area */

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip_quad(q);
    axl_gfx_fill_rect(0, 0, 16, 16, blu);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 16, 5, 5, bg) && px_eq(p, 16, 0, 0, bg),
               "quad degenerate: zero-area quad clips everything");
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_bbox_and_pop(void)
{
    AxlGfxPointF q[4]; diamond_corners(q);
    axl_gfx_reset_clip();
    test_check(axl_gfx_push_clip_quad(NULL) == AXL_ERR, "push_clip_quad(NULL): AXL_ERR");
    test_check(axl_gfx_push_clip_quad(q) == AXL_OK, "push_clip_quad: OK");

    AxlGfxClip c;
    test_check(axl_gfx_get_clip(&c) == AXL_OK, "quad bbox: get_clip OK after quad push");
    test_check(c.x == 4 && c.y == 4 && c.w == 24 && c.h == 24,
               "quad bbox: get_clip reports axis-aligned bbox (4,4,24,24)");
    test_check(axl_gfx_pop_clip() == AXL_OK, "quad pop: OK");
    axl_gfx_reset_clip();
}

static void
test_clip_quad_blit_honors(void)
{
    /* Plain axl_gfx_blit (buffer path) must honor the quad clip. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    static AxlGfxPixel src[32 * 32];
    for (int i = 0; i < 32 * 32; i++) { src[i] = blu; }
    AxlGfxPointF q[4]; diamond_corners(q);

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip_quad(q);
    axl_gfx_blit(src, 0, 0, 32, 32);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 32, 16, 16, blu), "quad blit: center inside diamond — blitted");
    test_check(px_eq(p, 32,  6,  6, bg),  "quad blit: corner outside diamond — bg");
    test_check(px_eq(p, 32, 26, 26, bg),  "quad blit: corner outside diamond — bg");
    axl_gfx_buffer_free(b);
}

/* Helper: pixel whose blue=col, green=row encodes its source position,
 * so a blit can be verified by reading back coordinates. */
static AxlGfxPixel
pos_px(uint32_t col, uint32_t row)
{
    AxlGfxPixel px = { (uint8_t)col, (uint8_t)row, 0x00, 0xFF };
    return px;
}

/* Fill @a buf (row stride @a stride, @a rows rows) so each pixel encodes
 * its own (col, row) via pos_px — lets a blit be verified by coords. */
static void
fill_pos_grid(AxlGfxPixel *buf, uint32_t stride, uint32_t rows)
{
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < stride; c++) {
            buf[r * stride + c] = pos_px(c, r);
        }
    }
}

static void
test_blit_rect_equals_blit_for_full_rect(void)
{
    /* axl_gfx_blit_rect(buf, w, 0,0, x,y, w,h) must match axl_gfx_blit. */
    AxlGfxPixel bg = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel tight[4 * 4];
    fill_pos_grid(tight, 4, 4);

    AxlGfxBuffer *b1 = axl_gfx_buffer_new(4, 4);
    AxlGfxBuffer *b2 = axl_gfx_buffer_new(4, 4);
    axl_gfx_buffer_clear(b1, bg);
    axl_gfx_buffer_clear(b2, bg);

    axl_gfx_target_buffer(b1);
    int rc1 = axl_gfx_blit(tight, 0, 0, 4, 4);
    axl_gfx_target_buffer(b2);
    int rc2 = axl_gfx_blit_rect(tight, 4, 0, 0, 0, 0, 4, 4);
    axl_gfx_target_buffer(NULL);

    test_check(rc1 == AXL_OK && rc2 == AXL_OK,
               "blit_rect full: both return AXL_OK");

    AxlGfxPixel *p1 = axl_gfx_buffer_pixels(b1);
    AxlGfxPixel *p2 = axl_gfx_buffer_pixels(b2);
    bool all_eq = true;
    for (uint32_t i = 0; i < 4 * 4; i++) {
        if (p1[i].blue != p2[i].blue || p1[i].green != p2[i].green ||
            p1[i].red != p2[i].red || p1[i].alpha != p2[i].alpha) {
            all_eq = false;
        }
    }
    test_check(all_eq, "blit_rect full: pixel-identical to axl_gfx_blit");

    axl_gfx_buffer_free(b1);
    axl_gfx_buffer_free(b2);
}

static void
test_blit_rect_interior_subrect(void)
{
    /* 8x8 strided source; extract the 3x2 sub-rect at (2,3) to dst (1,1). */
    AxlGfxPixel bg = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel src[8 * 8];
    fill_pos_grid(src, 8, 8);

    AxlGfxBuffer *b = axl_gfx_buffer_new(8, 8);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_blit_rect(src, 8, 2, 3, 1, 1, 3, 2);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "blit_rect interior: returns AXL_OK");
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* dst (1+i, 1+j) == src (2+i, 3+j). */
    test_check(px_eq(p, 8, 1, 1, pos_px(2, 3)),
               "blit_rect interior: dst(1,1) is src(2,3)");
    test_check(px_eq(p, 8, 3, 1, pos_px(4, 3)),
               "blit_rect interior: dst(3,1) is src(4,3)");
    test_check(px_eq(p, 8, 3, 2, pos_px(4, 4)),
               "blit_rect interior: dst(3,2) is src(4,4)");
    /* Pixels outside the destination rect stay bg. */
    test_check(px_eq(p, 8, 0, 0, bg), "blit_rect interior: dst(0,0) untouched");
    test_check(px_eq(p, 8, 4, 1, bg), "blit_rect interior: dst(4,1) past width untouched");
    test_check(px_eq(p, 8, 1, 3, bg), "blit_rect interior: dst(1,3) past height untouched");
    axl_gfx_buffer_free(b);
}

static void
test_blit_rect_honors_stride(void)
{
    /* stride=6 but blit width=2: row N of the sub-rect must come from
     * source offset N*6, not N*2. Pins that src_stride (not w) strides
     * the source. */
    AxlGfxPixel bg = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel src[6 * 3];
    fill_pos_grid(src, 6, 3);

    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_blit_rect(src, 6, 0, 0, 0, 0, 2, 3);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "blit_rect stride: returns AXL_OK");
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    /* If stride were mistakenly w(2), dst(0,1) would read src linear idx
     * 2 = src(2,0) (green 0). With stride 6 it is src(0,1) (green 1). */
    test_check(px_eq(p, 4, 0, 1, pos_px(0, 1)),
               "blit_rect stride: dst(0,1) is src(0,1) — wide stride honored");
    test_check(px_eq(p, 4, 1, 2, pos_px(1, 2)),
               "blit_rect stride: dst(1,2) is src(1,2)");
    axl_gfx_buffer_free(b);
}

static void
test_blit_rect_target_edge_clip(void)
{
    /* Blit a 4x4 sub-rect at dst (6,6) of an 8x8 target: only the 2x2 at
     * the corner fits; the rest is clipped without OOB. */
    AxlGfxPixel bg = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel src[8 * 8];
    fill_pos_grid(src, 8, 8);

    AxlGfxBuffer *b = axl_gfx_buffer_new(8, 8);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_blit_rect(src, 8, 0, 0, 6, 6, 4, 4);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK, "blit_rect edge clip: returns AXL_OK");
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 8, 6, 6, pos_px(0, 0)),
               "blit_rect edge clip: dst(6,6) is src(0,0)");
    test_check(px_eq(p, 8, 7, 7, pos_px(1, 1)),
               "blit_rect edge clip: dst(7,7) is src(1,1)");
    axl_gfx_buffer_free(b);
}

static void
test_blit_rect_negatives(void)
{
    AxlGfxPixel bg = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel src[4 * 4];
    for (uint32_t i = 0; i < 4 * 4; i++) { src[i] = bg; }

    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    test_check(axl_gfx_blit_rect(NULL, 4, 0, 0, 0, 0, 4, 4) == AXL_ERR,
               "blit_rect: NULL buffer -> AXL_ERR");
    test_check(axl_gfx_blit_rect(src, 4, 0, 0, 0, 0, 0, 4) == AXL_ERR,
               "blit_rect: zero width -> AXL_ERR");
    test_check(axl_gfx_blit_rect(src, 4, 0, 0, 0, 0, 4, 0) == AXL_ERR,
               "blit_rect: zero height -> AXL_ERR");
    axl_gfx_target_buffer(NULL);
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_alpha_blend_honors(void)
{
    /* Translucent fill under a quad clip: interior blends, exterior is
       untouched background. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   red = {0x00, 0x00, 0xFF, 0xFF};          /* opaque red bg */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0x80};          /* 50% blue */
    axl_gfx_buffer_clear(b, red);
    AxlGfxPointF q[4]; diamond_corners(q);

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip_quad(q);
    axl_gfx_fill_rect(0, 0, 32, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    const AxlGfxPixel *center = &p[16 * 32 + 16];
    test_check(center->blue > 100 && center->red > 100 && center->red < 200,
               "quad alpha: interior is a red/blue blend, not pure bg");
    test_check(px_eq(p, 32, 6, 6, red),
               "quad alpha: exterior corner is untouched red bg");
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_nested(void)
{
    /* Two overlapping diamonds — only their intersection survives. The
       second diamond is shifted +8 in x. At y=16 the first spans x∈[4,28],
       the second x∈[12,36]; so (20,16) is inside both, (8,16) only the
       first, (32,16) only the second. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(40, 32);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPointF q1[4]; diamond_corners(q1);
    AxlGfxPointF q2[4]; diamond_corners(q2);
    for (int i = 0; i < 4; i++) { q2[i].x += 8; }

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip_quad(q1);
    axl_gfx_push_clip_quad(q2);
    axl_gfx_fill_rect(0, 0, 40, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 40, 20, 16, blu), "quad nested: (20,16) in both — filled");
    test_check(px_eq(p, 40,  8, 16, bg),  "quad nested: (8,16) only in first — bg");
    test_check(px_eq(p, 40, 32, 16, bg),  "quad nested: (32,16) only in second — bg");
    axl_gfx_buffer_free(b);
}

static void
test_clip_quad_bitmap_text_honors(void)
{
    /* AxlFont bitmap text (render_text_glyphs direct-write path) must
       honor the quad clip.  Find a glyph-covered pixel by drawing once
       unclipped, then confirm a quad clip that excludes it suppresses
       it. */
    const AxlFont *font = axl_gfx_default_font();
    AxlGfxBuffer  *b    = axl_gfx_buffer_new(64, 24);
    AxlGfxPixel    bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel    wht  = {0xFF, 0xFF, 0xFF, 0xFF};
    const char    *s    = "ABCDEFGH";

    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_draw_text(font, 0, 4, s, wht, 1);
    axl_gfx_target_buffer(NULL);

    /* Locate a lit (glyph) pixel. */
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    int lit_x = -1, lit_y = -1;
    for (int yy = 0; yy < 24 && lit_x < 0; yy++) {
        for (int xx = 0; xx < 64; xx++) {
            if (p[yy * 64 + xx].red == 0xFF) { lit_x = xx; lit_y = yy; break; }
        }
    }
    test_check(lit_x >= 0, "quad text: found a lit glyph pixel unclipped");

    /* A 2x2 quad clip far from the lit pixel excludes it. */
    AxlGfxPointF q[4] = {
        {(float)(lit_x + 10), (float)(lit_y)},
        {(float)(lit_x + 12), (float)(lit_y)},
        {(float)(lit_x + 12), (float)(lit_y + 2)},
        {(float)(lit_x + 10), (float)(lit_y + 2)},
    };
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_push_clip_quad(q);
    axl_gfx_draw_text(font, 0, 4, s, wht, 1);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    test_check(lit_x < 0 || px_eq(p, 64, (uint32_t)lit_x, (uint32_t)lit_y, bg),
               "quad text: glyph pixel outside the quad clip is suppressed");

    /* Converse: a quad clip covering the lit pixel must let it survive. */
    if (lit_x >= 0) {
        AxlGfxPointF inq[4] = {
            {(float)(lit_x - 2), (float)(lit_y - 2)},
            {(float)(lit_x + 3), (float)(lit_y - 2)},
            {(float)(lit_x + 3), (float)(lit_y + 3)},
            {(float)(lit_x - 2), (float)(lit_y + 3)},
        };
        axl_gfx_buffer_clear(b, bg);
        axl_gfx_target_buffer(b);
        axl_gfx_push_clip_quad(inq);
        axl_gfx_draw_text(font, 0, 4, s, wht, 1);
        axl_gfx_pop_clip();
        axl_gfx_target_buffer(NULL);
        test_check(px_eq(p, 64, (uint32_t)lit_x, (uint32_t)lit_y, wht),
                   "quad text: glyph pixel inside the quad clip survives");
    }
    axl_gfx_buffer_free(b);
}

static void
test_clip_rect_transformed(void)
{
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel  *p   = axl_gfx_buffer_pixels(b);

    /* Identity transform clips identically to the rect (4,4,8,8). */
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(b);
    AxlTransform id = axl_transform_identity();
    test_check(axl_gfx_push_clip_rect_transformed((AxlRect){ 4, 4, 8, 8 }, &id) == AXL_OK,
               "clip_rect_transformed: identity OK");
    axl_gfx_fill_rect(0, 0, 32, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);
    test_check(px_eq(p, 32, 6, 6, blu),   "clip_rect_transformed: inside rect filled");
    test_check(px_eq(p, 32, 0, 0, bg),    "clip_rect_transformed: outside rect bg");
    test_check(px_eq(p, 32, 13, 13, bg),  "clip_rect_transformed: past rect (>=12) bg");

    /* A translation shifts the clip region by (+5, 0): now x in [9,17). */
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    AxlTransform t = axl_transform_translate(5, 0);
    axl_gfx_push_clip_rect_transformed((AxlRect){ 4, 4, 8, 8 }, &t);
    axl_gfx_fill_rect(0, 0, 32, 32, blu);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);
    test_check(px_eq(p, 32, 11, 6, blu), "clip_rect_transformed: translate shifts clip (+5)");
    test_check(px_eq(p, 32,  6, 6, bg),  "clip_rect_transformed: pre-translate region now bg");

    test_check(axl_gfx_push_clip_rect_transformed((AxlRect){ 0, 0, 1, 1 }, NULL) == AXL_ERR,
               "clip_rect_transformed: NULL transform -> AXL_ERR");
    axl_gfx_buffer_free(b);
}

static void
test_clip_path_concave(void)
{
    AxlGfxBuffer *b   = axl_gfx_buffer_new(16, 16);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht = {0xFF, 0xFF, 0xFF, 0xFF};
    AxlGfxPixel  *p   = axl_gfx_buffer_pixels(b);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_reset_transform();

    /* A concave "boot" polygon (notch at the bottom-right) — NOT
     * representable as a convex quad clip:
     *   top bar  x in [2,10], y in [2,6]
     *   left arm x in [2,6],  y in [2,14]
     * so (8,10) is in the cut-out notch. */
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 2, 2);
    axl_gfx_path_line_to(path, 10, 2);
    axl_gfx_path_line_to(path, 10, 6);
    axl_gfx_path_line_to(path, 6, 6);
    axl_gfx_path_line_to(path, 6, 14);
    axl_gfx_path_line_to(path, 2, 14);   /* contour auto-closes */

    axl_gfx_target_buffer(b);
    test_check(axl_gfx_push_clip_path(path) == AXL_OK, "clip_path: push OK");
    axl_gfx_fill_rect(0, 0, 16, 16, wht);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    test_check(px_eq(p, 16, 4, 10, wht), "clip_path: inside left arm filled");
    test_check(px_eq(p, 16, 8, 4, wht),  "clip_path: inside top bar filled");
    test_check(px_eq(p, 16, 8, 10, bg),
               "clip_path: concave notch excluded (a quad clip can't do this)");
    test_check(px_eq(p, 16, 13, 13, bg), "clip_path: outside the shape stays bg");

    /* After pop the clip is gone — a fill reaches the notch. */
    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(0, 0, 16, 16, wht);
    axl_gfx_target_buffer(NULL);
    test_check(px_eq(p, 16, 8, 10, wht), "clip_path: pop restores full drawing");

    /* NULL / empty path is rejected. */
    AxlGfxPath *empty = axl_gfx_path_new();
    test_check(axl_gfx_push_clip_path(NULL) == AXL_ERR, "clip_path: NULL path -> AXL_ERR");
    test_check(axl_gfx_push_clip_path(empty) == AXL_ERR, "clip_path: empty path -> AXL_ERR");
    axl_gfx_path_free(empty);
    axl_gfx_path_free(path);
    axl_gfx_buffer_free(b);
}

static void
test_clip_path_nested(void)
{
    /* A path clip intersects the parent clip: rect x>=4 AND the boot
     * shape. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(16, 16);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht = {0xFF, 0xFF, 0xFF, 0xFF};
    AxlGfxPixel  *p   = axl_gfx_buffer_pixels(b);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_reset_transform();

    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 2, 2);
    axl_gfx_path_line_to(path, 10, 2);
    axl_gfx_path_line_to(path, 10, 6);
    axl_gfx_path_line_to(path, 6, 6);
    axl_gfx_path_line_to(path, 6, 14);
    axl_gfx_path_line_to(path, 2, 14);

    axl_gfx_target_buffer(b);
    axl_gfx_push_clip((AxlGfxClip){ 4, 0, 16, 16 });   /* x >= 4 */
    axl_gfx_push_clip_path(path);                       /* AND the boot */
    axl_gfx_fill_rect(0, 0, 16, 16, wht);
    axl_gfx_pop_clip();
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    test_check(px_eq(p, 16, 5, 10, wht), "clip_path nested: in both rect+path filled");
    test_check(px_eq(p, 16, 3, 10, bg),  "clip_path nested: in path but left of rect -> bg");
    test_check(px_eq(p, 16, 8, 10, bg),  "clip_path nested: in rect but in path notch -> bg");
    axl_gfx_path_free(path);
    axl_gfx_buffer_free(b);
}

static void
test_fill_path_tall_narrow_high_offset(void)
{
    /* Regression for the ftgrays pool-estimate underflow: a path whose
       x-extent ends below where its y-extent starts (max_ex < min_ey)
       drove `estimate = (max_ex - min_ey + ...)` negative → huge
       unsigned → pool-allocation overflow → memory corruption / hang.
       A thin, tall rectangle high on the surface reproduces it with no
       text involved. Must fill cleanly and light its interior. */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(32, 96);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlGfxPath *path = axl_gfx_path_new();
    axl_gfx_path_move_to(path, 5, 50);
    axl_gfx_path_line_to(path, 12, 50);
    axl_gfx_path_line_to(path, 12, 90);
    axl_gfx_path_line_to(path, 5, 90);
    axl_gfx_path_close(path);

    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_path(path, wht);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "fill_path tall-narrow high-offset: returns OK (no hang)");
    test_check(px_eq(axl_gfx_buffer_pixels(b), 32, 8, 70, wht),
               "fill_path tall-narrow high-offset: interior pixel filled");
    axl_gfx_path_free(path);
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// Transform-aware text — axl_ttf_draw_transform (slice 2)
// ---------------------------------------------------------------------------

static int
abs_i(int v)
{
    return v < 0 ? -v : v;
}

/* Bounding box + count of non-background pixels in a w*h buffer. */
static int
lit_bbox(const AxlGfxPixel *p, uint32_t w, uint32_t h, AxlGfxPixel bg,
         int *minx, int *miny, int *maxx, int *maxy)
{
    int count = 0;
    *minx = (int)w; *miny = (int)h; *maxx = -1; *maxy = -1;
    for (uint32_t yy = 0; yy < h; yy++) {
        for (uint32_t xx = 0; xx < w; xx++) {
            const AxlGfxPixel *q = &p[yy * w + xx];
            if (q->blue != bg.blue || q->green != bg.green || q->red != bg.red) {
                count++;
                if ((int)xx < *minx) *minx = (int)xx;
                if ((int)xx > *maxx) *maxx = (int)xx;
                if ((int)yy < *miny) *miny = (int)yy;
                if ((int)yy > *maxy) *maxy = (int)yy;
            }
        }
    }
    return count;
}

static void
test_ttf_transform_null_args(void)
{
    AxlTtf       *font = axl_ttf_default();
    AxlTransform  m    = axl_transform_translate(2, 12);
    AxlGfxPixel   c    = {0xFF, 0xFF, 0xFF, 0xFF};
    test_check(font != NULL, "ttf_transform: default font loads");
    test_check(axl_ttf_draw_transform(NULL, "x", 12.0f, &m, c) == AXL_ERR,
               "ttf_transform: NULL font -> AXL_ERR");
    test_check(axl_ttf_draw_transform(font, NULL, 12.0f, &m, c) == AXL_ERR,
               "ttf_transform: NULL utf8 -> AXL_ERR");
    test_check(axl_ttf_draw_transform(font, "x", 0.0f, &m, c) == AXL_ERR,
               "ttf_transform: px_size 0 -> AXL_ERR");
    test_check(axl_ttf_draw_transform(font, "x", 12.0f, NULL, c) == AXL_ERR,
               "ttf_transform: NULL transform -> AXL_ERR");
}

static void
test_ttf_transform_renders_in_bbox(void)
{
    AxlTtf       *font = axl_ttf_default();
    AxlGfxBuffer *b    = axl_gfx_buffer_new(64, 32);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht  = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    AxlTransform m = axl_transform_translate(2, 20);
    axl_gfx_target_buffer(b);
    int rc = axl_ttf_draw_transform(font, "Ag", 16.0f, &m, wht);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "ttf_transform: draw returns AXL_OK");

    int minx, miny, maxx, maxy;
    int n = lit_bbox(axl_gfx_buffer_pixels(b), 64, 32, bg,
                     &minx, &miny, &maxx, &maxy);
    test_check(n > 20, "ttf_transform: something rendered (lit pixels)");
    /* Baseline at y=20, 16px text: glyphs sit above the baseline, the
       'g' descender just below. All within a sane band. */
    test_check(minx >= 2 && maxx < 40, "ttf_transform: glyphs within expected x band");
    test_check(miny >= 4 && maxy <= 26, "ttf_transform: glyphs within expected y band");
    axl_gfx_buffer_free(b);
}

static void
test_ttf_transform_high_offset_renders(void)
{
    /* Regression: a glyph rotated 90° so its narrow x-extent sits at a
       large device-y offset previously triggered an unsigned underflow
       in ftgrays' pool estimate (max_ex < min_ey), corrupting memory
       and hanging. Draw 'F' at y=40 in an 80×80 buffer and confirm it
       both returns AND lights pixels (renders, not silently empty). */
    AxlTtf       *font = axl_ttf_default();
    AxlGfxBuffer *b    = axl_gfx_buffer_new(80, 80);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht  = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    AxlTransform  m    = axl_transform_multiply(
        axl_transform_rotate(AXL_MATH_HALF_PI),
        axl_transform_translate(24, 40));

    axl_gfx_target_buffer(b);
    int rc = axl_ttf_draw_transform(font, "F", 12.0f, &m, wht);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "ttf_transform high-offset: returns AXL_OK (no hang)");

    int x0, y0, x1, y1;
    int n = lit_bbox(axl_gfx_buffer_pixels(b), 80, 80, bg, &x0, &y0, &x1, &y1);
    test_check(n > 0, "ttf_transform high-offset: glyph actually rendered");
    test_check(y0 >= 38, "ttf_transform high-offset: rendered at the large y offset");
    axl_gfx_buffer_free(b);
}

static void
test_ttf_transform_rotation_transposes_bbox(void)
{
    AxlTtf       *font = axl_ttf_default();
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht  = {0xFF, 0xFF, 0xFF, 0xFF};
    const char   *s    = "ABCDEF";

    /* Horizontal: wide and short. */
    AxlGfxBuffer *bh = axl_gfx_buffer_new(80, 80);
    axl_gfx_buffer_clear(bh, bg);
    AxlTransform mh = axl_transform_translate(4, 24);
    axl_gfx_target_buffer(bh);
    int rch = axl_ttf_draw_transform(font, s, 12.0f, &mh, wht);
    axl_gfx_target_buffer(NULL);
    test_check(rch == AXL_OK, "ttf_transform: horizontal multi-glyph draw returns OK");
    int hx0, hy0, hx1, hy1;
    lit_bbox(axl_gfx_buffer_pixels(bh), 80, 80, bg, &hx0, &hy0, &hx1, &hy1);

    /* Rotated 90°: same text, now tall and narrow. */
    AxlGfxBuffer *br = axl_gfx_buffer_new(80, 80);
    axl_gfx_buffer_clear(br, bg);
    AxlTransform mr = axl_transform_multiply(
        axl_transform_rotate(AXL_MATH_HALF_PI),
        axl_transform_translate(24, 4));
    axl_gfx_target_buffer(br);
    axl_ttf_draw_transform(font, s, 12.0f, &mr, wht);
    axl_gfx_target_buffer(NULL);
    int rx0, ry0, rx1, ry1;
    lit_bbox(axl_gfx_buffer_pixels(br), 80, 80, bg, &rx0, &ry0, &rx1, &ry1);

    test_check((hx1 - hx0) > (hy1 - hy0),
               "ttf_transform: horizontal text bbox is wider than tall");
    test_check((ry1 - ry0) > (rx1 - rx0),
               "ttf_transform: 90°-rotated text bbox is taller than wide");
    axl_gfx_buffer_free(bh);
    axl_gfx_buffer_free(br);
}

static void
test_ttf_transform_matches_draw_position(void)
{
    /* m = translate(x, baseline) should land glyphs in essentially the
       same place as axl_ttf_draw (outline AA vs bitmap differ in exact
       pixels, but the lit bounding boxes must broadly coincide). */
    AxlTtf       *font = axl_ttf_default();
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht  = {0xFF, 0xFF, 0xFF, 0xFF};
    const char   *s    = "Hi";

    AxlGfxBuffer *ba = axl_gfx_buffer_new(48, 32);
    axl_gfx_buffer_clear(ba, bg);
    axl_gfx_target_buffer(ba);
    axl_ttf_draw(font, 4, 22, s, 16.0f, wht);
    axl_gfx_target_buffer(NULL);
    int ax0, ay0, ax1, ay1;
    lit_bbox(axl_gfx_buffer_pixels(ba), 48, 32, bg, &ax0, &ay0, &ax1, &ay1);

    AxlGfxBuffer *bb = axl_gfx_buffer_new(48, 32);
    axl_gfx_buffer_clear(bb, bg);
    AxlTransform m = axl_transform_translate(4, 22);
    axl_gfx_target_buffer(bb);
    axl_ttf_draw_transform(font, s, 16.0f, &m, wht);
    axl_gfx_target_buffer(NULL);
    int bx0, by0, bx1, by1;
    lit_bbox(axl_gfx_buffer_pixels(bb), 48, 32, bg, &bx0, &by0, &bx1, &by1);

    test_check(abs_i(ax0 - bx0) <= 2 && abs_i(ax1 - bx1) <= 2,
               "ttf_transform: x-extent matches axl_ttf_draw within 2px");
    test_check(abs_i(ay0 - by0) <= 2 && abs_i(ay1 - by1) <= 2,
               "ttf_transform: y-extent matches axl_ttf_draw within 2px");
    axl_gfx_buffer_free(ba);
    axl_gfx_buffer_free(bb);
}

// ---------------------------------------------------------------------------
// Transform-aware blit — axl_gfx_blit_transform (slice 3)
// ---------------------------------------------------------------------------

static void
test_blit_transform_null_args(void)
{
    AxlGfxBuffer *s = axl_gfx_buffer_new(4, 4);
    AxlTransform  m = axl_transform_identity();
    test_check(axl_gfx_blit_transform(NULL, &m) == AXL_ERR,
               "blit_transform: NULL src -> AXL_ERR");
    test_check(axl_gfx_blit_transform(s, NULL) == AXL_ERR,
               "blit_transform: NULL transform -> AXL_ERR");
    axl_gfx_buffer_free(s);
}

static void
test_blit_transform_identity_places_image(void)
{
    /* An 8x8 solid source placed via translate(10,5) lands at (10,5). */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(8, 8);
    axl_gfx_buffer_clear(s, blu);
    AxlGfxBuffer *d   = axl_gfx_buffer_new(32, 24);
    axl_gfx_buffer_clear(d, bg);

    AxlTransform m = axl_transform_translate(10, 5);
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    int rc = axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "blit_transform: returns AXL_OK");

    AxlGfxPixel *p = axl_gfx_buffer_pixels(d);
    test_check(px_eq(p, 32, 14, 9, blu),  "blit_transform identity: interior (14,9) is source");
    test_check(px_eq(p, 32,  2, 2, bg),   "blit_transform identity: outside image stays bg");
    test_check(px_eq(p, 32, 25, 20, bg),  "blit_transform identity: far corner stays bg");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_scale_covers_area(void)
{
    /* A 4x4 source scaled 4x covers ~16x16 device pixels. */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(4, 4);
    axl_gfx_buffer_clear(s, blu);
    AxlGfxBuffer *d   = axl_gfx_buffer_new(32, 32);
    axl_gfx_buffer_clear(d, bg);

    AxlTransform m = axl_transform_multiply(
        axl_transform_scale(4, 4), axl_transform_translate(4, 4));
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);

    int x0, y0, x1, y1;
    int n = lit_bbox(axl_gfx_buffer_pixels(d), 32, 32, bg, &x0, &y0, &x1, &y1);
    test_check(n > 200, "blit_transform scale: covers a ~16x16 region");
    test_check(x0 >= 3 && x0 <= 5 && y0 >= 3 && y0 <= 5,
               "blit_transform scale: top-left near (4,4)");
    test_check(x1 >= 18 && x1 <= 20 && y1 >= 18 && y1 <= 20,
               "blit_transform scale: bottom-right near (20,20)");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_rotation_transposes(void)
{
    /* A 16x4 (wide) source rotated 90° becomes ~4x16 (tall) on target. */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(16, 4);
    axl_gfx_buffer_clear(s, blu);
    AxlGfxBuffer *d   = axl_gfx_buffer_new(40, 40);
    axl_gfx_buffer_clear(d, bg);

    AxlTransform m = axl_transform_multiply(
        axl_transform_rotate(AXL_MATH_HALF_PI),
        axl_transform_translate(20, 4));
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);

    int x0, y0, x1, y1;
    lit_bbox(axl_gfx_buffer_pixels(d), 40, 40, bg, &x0, &y0, &x1, &y1);
    test_check((y1 - y0) > (x1 - x0),
               "blit_transform rotation: 90°-rotated wide image is taller than wide");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_bilinear_blends(void)
{
    /* 2x1 source: texel(0)=blue, texel(1)=red. Scaled 8x, the device
       column at the texel boundary samples a blue/red blend. */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   red = {0x00, 0x00, 0xFF, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(2, 1);
    AxlGfxPixel  *sp  = axl_gfx_buffer_pixels(s);
    sp[0] = blu; sp[1] = red;
    AxlGfxBuffer *d   = axl_gfx_buffer_new(24, 12);
    axl_gfx_buffer_clear(d, bg);

    AxlTransform m = axl_transform_multiply(
        axl_transform_scale(8, 8), axl_transform_translate(0, 0));
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);

    /* Source u=1.0 (texel boundary) maps to device x=8. That column is
       a ~50/50 blue/red mix; interior of texel 0 (device x~2) is blue. */
    AxlGfxPixel *p = axl_gfx_buffer_pixels(d);
    const AxlGfxPixel *mid  = &p[4 * 24 + 8];
    test_check(mid->blue > 60 && mid->red > 60,
               "blit_transform bilinear: boundary column blends blue+red");
    test_check(px_eq(p, 24, 2, 4, blu),
               "blit_transform bilinear: deep inside texel 0 is pure blue");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_alpha_source(void)
{
    /* A semi-transparent source blends source-over onto the target. */
    AxlGfxPixel   red   = {0x00, 0x00, 0xFF, 0xFF};       /* opaque red bg */
    AxlGfxPixel   blu_a = {0xFF, 0x00, 0x00, 0x80};       /* 50% blue */
    AxlGfxBuffer *s     = axl_gfx_buffer_new(8, 8);
    axl_gfx_buffer_clear(s, blu_a);
    AxlGfxBuffer *d     = axl_gfx_buffer_new(24, 24);
    axl_gfx_buffer_clear(d, red);

    AxlTransform m = axl_transform_translate(6, 6);
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(d);
    const AxlGfxPixel *c = &p[10 * 24 + 10];   /* inside the blitted region */
    test_check(c->blue > 60 && c->red > 60 && c->red < 255,
               "blit_transform alpha: translucent source blends over bg");
    test_check(px_eq(p, 24, 1, 1, red),
               "blit_transform alpha: outside image stays opaque red");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_minify(void)
{
    /* Downscale (0.25x) must render a small image without crashing. */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(16, 16);
    axl_gfx_buffer_clear(s, blu);
    AxlGfxBuffer *d   = axl_gfx_buffer_new(24, 24);
    axl_gfx_buffer_clear(d, bg);

    AxlTransform m = axl_transform_multiply(
        axl_transform_scale(0.25f, 0.25f), axl_transform_translate(4, 4));
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    int rc = axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "blit_transform minify: returns AXL_OK");

    int x0, y0, x1, y1;
    int n = lit_bbox(axl_gfx_buffer_pixels(d), 24, 24, bg, &x0, &y0, &x1, &y1);
    test_check(n >= 9 && n <= 25,
               "blit_transform minify: 16x16 -> ~4x4 device region");
    test_check(px_eq(axl_gfx_buffer_pixels(d), 24, 5, 5, blu),
               "blit_transform minify: sampled interior is source color");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_shear(void)
{
    /* Shear x by y: x' = x + 0.5*y + 4, y' = y + 4. Maps the square to a
       parallelogram leaning right toward the bottom. */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(8, 8);
    axl_gfx_buffer_clear(s, blu);
    AxlGfxBuffer *d   = axl_gfx_buffer_new(24, 20);
    axl_gfx_buffer_clear(d, bg);

    /* x' = x + 0.5*y + 4, y' = y + 4 — row-major [a b tx; c d ty; 0 0 1]. */
    AxlTransform m = { .m = { 1.0, 0.5, 4.0,  0.0, 1.0, 4.0,  0.0, 0.0, 1.0 } };
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(d);
    test_check(px_eq(p, 24, 10, 8, blu),
               "blit_transform shear: interior of the parallelogram is filled");
    test_check(px_eq(p, 24, 5, 11, bg),
               "blit_transform shear: bottom-left (sheared away) stays bg");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_perspective_trapezoid(void)
{
    /* Map a solid source square onto a trapezoid wider at the bottom —
     * a shape an affine blit (which can only produce a parallelogram)
     * cannot make.  The discriminating pixel (36,28) lies inside the
     * trapezoid but outside any affine image of the source, so it is
     * filled only by a perspective-correct blit. */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(16, 16);
    axl_gfx_buffer_clear(s, blu);
    AxlGfxBuffer *d   = axl_gfx_buffer_new(48, 40);
    axl_gfx_buffer_clear(d, bg);

    AxlVec2 srcq[4] = { axl_vec2(0, 0),  axl_vec2(16, 0),
                        axl_vec2(16, 16), axl_vec2(0, 16) };
    AxlVec2 devq[4] = { axl_vec2(16, 8), axl_vec2(32, 8),
                        axl_vec2(40, 30), axl_vec2(8, 30) };
    AxlTransform m;
    test_check(axl_transform_quad_to_quad(srcq, devq, &m),
               "blit_transform perspective: quad_to_quad built");
    test_check(axl_transform_classify(m) == AXL_TRANSFORM_PROJECTIVE,
               "blit_transform perspective: transform is projective");

    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(d);
    test_check(px_eq(p, 48, 24, 12, blu),
               "blit_transform perspective: top-center inside trapezoid filled");
    test_check(px_eq(p, 48, 24, 28, blu),
               "blit_transform perspective: bottom-center filled");
    test_check(px_eq(p, 48, 36, 28, blu),
               "blit_transform perspective: wide-bottom (outside any affine image) filled");
    test_check(px_eq(p, 48, 2, 2, bg),
               "blit_transform perspective: above-left of trapezoid stays bg");
    test_check(px_eq(p, 48, 46, 6, bg),
               "blit_transform perspective: top-right corner stays bg");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_blit_transform_perspective_negative_orientation(void)
{
    /* Same trapezoid region but with reversed corner winding, so the
     * effective transform has a NEGATIVE determinant — exercises the
     * horizon guard's negative-orientation branch (the front-facing sign
     * is derived per-transform, so it must work either way). */
    AxlGfxPixel   blu = {0xFF, 0x00, 0x00, 0xFF};
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxBuffer *s   = axl_gfx_buffer_new(16, 16);
    axl_gfx_buffer_clear(s, blu);
    AxlGfxBuffer *d   = axl_gfx_buffer_new(48, 40);
    axl_gfx_buffer_clear(d, bg);

    AxlVec2 srcq[4] = { axl_vec2(0, 0),  axl_vec2(16, 0),
                        axl_vec2(16, 16), axl_vec2(0, 16) };
    AxlVec2 devq[4] = { axl_vec2(32, 8), axl_vec2(16, 8),
                        axl_vec2(8, 30),  axl_vec2(40, 30) };  /* reversed winding */
    AxlTransform m;
    test_check(axl_transform_quad_to_quad(srcq, devq, &m),
               "blit_transform neg-orient: quad_to_quad built");
    test_check(axl_transform_determinant(m) < 0.0,
               "blit_transform neg-orient: determinant is negative");

    axl_gfx_reset_transform();
    axl_gfx_target_buffer(d);
    axl_gfx_blit_transform(s, &m);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(d);
    test_check(px_eq(p, 48, 24, 12, blu),
               "blit_transform neg-orient: top-center filled");
    test_check(px_eq(p, 48, 36, 28, blu),
               "blit_transform neg-orient: wide-bottom filled (guard ok for det<0)");
    test_check(px_eq(p, 48, 2, 2, bg),
               "blit_transform neg-orient: outside stays bg");
    axl_gfx_buffer_free(s);
    axl_gfx_buffer_free(d);
}

static void
test_ttf_transform_latin1_codepoint(void)
{
    /* Non-ASCII Latin-1 ('é' U+00E9) exercises the multibyte UTF-8 path
       through the outline pipeline; the built-in font covers Latin-1. */
    AxlTtf       *font = axl_ttf_default();
    AxlGfxBuffer *b    = axl_gfx_buffer_new(32, 32);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht  = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    AxlTransform m = axl_transform_translate(4, 22);
    axl_gfx_target_buffer(b);
    int rc = axl_ttf_draw_transform(font, "\xC3\xA9", 18.0f, &m, wht);  /* "é" */
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "ttf_transform latin1: 'é' returns AXL_OK");

    int x0, y0, x1, y1;
    int n = lit_bbox(axl_gfx_buffer_pixels(b), 32, 32, bg, &x0, &y0, &x1, &y1);
    test_check(n > 10, "ttf_transform latin1: 'é' rendered glyph pixels");
    axl_gfx_buffer_free(b);
}

static void
test_ttf_transform_multicontour_hole(void)
{
    /* 'O' is two contours (outer + inner); even-odd fill must leave the
       counter (hole) empty. The bbox center sits in the hole. */
    AxlTtf       *font = axl_ttf_default();
    AxlGfxBuffer *b    = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht  = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_buffer_clear(b, bg);

    AxlTransform m = axl_transform_translate(4, 32);
    axl_gfx_target_buffer(b);
    axl_ttf_draw_transform(font, "O", 32.0f, &m, wht);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    int x0, y0, x1, y1;
    int n = lit_bbox(p, 40, 40, bg, &x0, &y0, &x1, &y1);
    test_check(n > 60, "ttf_transform hole: 'O' ring rendered");
    uint32_t cxp = (uint32_t)((x0 + x1) / 2), cyp = (uint32_t)((y0 + y1) / 2);
    test_check(px_eq(p, 40, cxp, cyp, bg),
               "ttf_transform hole: 'O' counter (bbox center) is unfilled");
    axl_gfx_buffer_free(b);
}

static void
test_ttf_transform_perspective_renders(void)
{
    AxlTtf       *font = axl_ttf_default();
    AxlGfxPixel   bg   = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   wht  = {0xFF, 0xFF, 0xFF, 0xFF};

    /* 'O' under a moderate perspective takes the projective path, which
     * flattens the ring in local space. The ring must render and the
     * counter (hole) must survive — i.e. both contours stay closed and
     * correctly wound through the projection. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(48, 48);
    axl_gfx_buffer_clear(b, bg);
    AxlTransform persp = axl_transform_multiply(
        axl_transform_perspective(0.008, 0.0), axl_transform_translate(6, 34));
    test_check(axl_transform_classify(persp) == AXL_TRANSFORM_PROJECTIVE,
               "ttf_transform perspective: transform is projective");
    axl_gfx_reset_transform();
    axl_gfx_target_buffer(b);
    int rc = axl_ttf_draw_transform(font, "O", 32.0f, &persp, wht);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "ttf_transform perspective: draw returns OK");

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    int x0, y0, x1, y1;
    int n = lit_bbox(p, 48, 48, bg, &x0, &y0, &x1, &y1);
    test_check(n > 50, "ttf_transform perspective: 'O' ring rendered");
    uint32_t cxp = (uint32_t)((x0 + x1) / 2), cyp = (uint32_t)((y0 + y1) / 2);
    test_check(px_eq(p, 48, cxp, cyp, bg),
               "ttf_transform perspective: 'O' counter (hole) preserved");
    axl_gfx_buffer_free(b);

    /* Near-affine limit: a tiny perspective term still routes through the
     * local-flatten path but must render almost the same coverage as the
     * pure-affine path — a regression guard against a broken flattener. */
    AxlGfxBuffer *ba = axl_gfx_buffer_new(48, 48);
    AxlGfxBuffer *bp = axl_gfx_buffer_new(48, 48);
    axl_gfx_buffer_clear(ba, bg);
    axl_gfx_buffer_clear(bp, bg);
    AxlTransform aff  = axl_transform_translate(6, 34);
    AxlTransform near = axl_transform_multiply(
        axl_transform_perspective(0.0005, 0.0), axl_transform_translate(6, 34));
    axl_gfx_target_buffer(ba);
    axl_ttf_draw_transform(font, "O", 32.0f, &aff, wht);
    axl_gfx_target_buffer(bp);
    axl_ttf_draw_transform(font, "O", 32.0f, &near, wht);
    axl_gfx_target_buffer(NULL);
    int xa0, ya0, xa1, ya1, xp0, yp0, xp1, yp1;
    int na = lit_bbox(axl_gfx_buffer_pixels(ba), 48, 48, bg, &xa0, &ya0, &xa1, &ya1);
    int np = lit_bbox(axl_gfx_buffer_pixels(bp), 48, 48, bg, &xp0, &yp0, &xp1, &yp1);
    int diff = na > np ? na - np : np - na;
    test_check(na > 0 && diff * 100 < na * 20,
               "ttf_transform perspective: near-affine coverage ~ affine (within 20%)");
    axl_gfx_buffer_free(ba);
    axl_gfx_buffer_free(bp);
}

// ---------------------------------------------------------------------------
// Blend modes — G13
// ---------------------------------------------------------------------------

static AxlGfxPixel
gray_px(uint8_t v)
{
    return (AxlGfxPixel){ v, v, v, 0xFF };
}

static void
test_blend_ex_modes(void)
{
    /* Exact 8-bit math, opaque source (a=255 → out == B(Cb, Cs)).
       Backdrop Cb=200, source Cs=100 unless noted. */
    AxlGfxPixel db = gray_px(200), sb = gray_px(100);

    test_check(axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_OVER).blue == 100,
               "blend OVER: opaque src overwrites (100)");
    test_check(axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_MULTIPLY).blue == 78,
               "blend MULTIPLY: 200*100/255 = 78");
    test_check(axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_SCREEN).blue == 222,
               "blend SCREEN: 255-(55*155)/255 = 222");
    test_check(axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_DARKEN).blue == 100,
               "blend DARKEN: min(200,100) = 100");
    test_check(axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_LIGHTEN).blue == 200,
               "blend LIGHTEN: max(200,100) = 200");
    test_check(axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_OVERLAY).blue == 188,
               "blend OVERLAY: Cb=200>=128 -> screen-side = 188");
    test_check(axl_gfx_blend_ex(gray_px(100), gray_px(50),
                                AXL_GFX_BLEND_OVERLAY).blue == 39,
               "blend OVERLAY: Cb=100<128 -> multiply-side = 39");
    test_check(axl_gfx_blend_ex(gray_px(100), gray_px(50),
                                AXL_GFX_BLEND_ADD).blue == 150,
               "blend ADD: 100+50 = 150");
    test_check(axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_ADD).blue == 255,
               "blend ADD: 200+100 clamps to 255");

    /* axl_gfx_blend is exactly blend_ex(OVER). */
    test_check(axl_gfx_blend(db, sb).blue ==
               axl_gfx_blend_ex(db, sb, AXL_GFX_BLEND_OVER).blue,
               "blend: axl_gfx_blend == blend_ex OVER");

    /* Translucent source composites the blended color by alpha.
       MULTIPLY with a=128: B=78, out = (127*200 + 128*78 + 127)/255 = 139. */
    AxlGfxPixel st = { 100, 100, 100, 0x80 };
    test_check(axl_gfx_blend_ex(db, st, AXL_GFX_BLEND_MULTIPLY).blue == 139,
               "blend MULTIPLY translucent: alpha-composites B over Cb (139)");
    /* Fully transparent source: no change regardless of mode. */
    AxlGfxPixel s0 = { 100, 100, 100, 0x00 };
    test_check(axl_gfx_blend_ex(db, s0, AXL_GFX_BLEND_MULTIPLY).blue == 200,
               "blend: transparent src leaves backdrop unchanged");

    /* Per-channel (non-gray) — catches any R/G/B channel swap.
       dst{b200,g40,r90} * src{b100,g200,r10} = {78,31,4}. */
    AxlGfxPixel cd = { 200, 40, 90, 0xFF };
    AxlGfxPixel cs = { 100, 200, 10, 0xFF };
    AxlGfxPixel cm = axl_gfx_blend_ex(cd, cs, AXL_GFX_BLEND_MULTIPLY);
    test_check(cm.blue == 78 && cm.green == 31 && cm.red == 4,
               "blend MULTIPLY per-channel: {b78,g31,r4} (no channel swap)");
}

static void
test_blend_mode_state_and_fill(void)
{
    test_check(axl_gfx_get_blend_mode() == AXL_GFX_BLEND_OVER,
               "blend mode: default is OVER");

    AxlGfxBuffer *b = axl_gfx_buffer_new(8, 8);
    axl_gfx_buffer_clear(b, gray_px(200));
    axl_gfx_target_buffer(b);

    /* An OPAQUE fill under MULTIPLY must multiply (not overwrite) — the
       opaque fast path honors the mode. */
    axl_gfx_set_blend_mode(AXL_GFX_BLEND_MULTIPLY);
    axl_gfx_fill_rect(0, 0, 8, 8, gray_px(100));
    axl_gfx_set_blend_mode(AXL_GFX_BLEND_OVER);
    axl_gfx_target_buffer(NULL);

    test_check(px_eq(axl_gfx_buffer_pixels(b), 8, 4, 4, gray_px(78)),
               "blend mode: opaque MULTIPLY fill multiplies (200*100/255=78)");
    test_check(axl_gfx_get_blend_mode() == AXL_GFX_BLEND_OVER,
               "blend mode: restored to OVER");

    /* Sanity: OVER opaque fill still overwrites. */
    axl_gfx_buffer_clear(b, gray_px(200));
    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(0, 0, 8, 8, gray_px(100));
    axl_gfx_target_buffer(NULL);
    test_check(px_eq(axl_gfx_buffer_pixels(b), 8, 4, 4, gray_px(100)),
               "blend mode: OVER opaque fill overwrites (100)");

    /* SCREEN through the draw path (not just blend_ex): 255-(55*155)/255
       = 222. */
    axl_gfx_buffer_clear(b, gray_px(200));
    axl_gfx_target_buffer(b);
    axl_gfx_set_blend_mode(AXL_GFX_BLEND_SCREEN);
    axl_gfx_fill_rect(0, 0, 8, 8, gray_px(100));
    axl_gfx_reset_blend_mode();
    axl_gfx_target_buffer(NULL);
    test_check(px_eq(axl_gfx_buffer_pixels(b), 8, 4, 4, gray_px(222)),
               "blend mode: opaque SCREEN fill through draw path (222)");
    test_check(axl_gfx_get_blend_mode() == AXL_GFX_BLEND_OVER,
               "blend mode: reset_blend_mode -> OVER");
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// G17 — pixel packing for direct framebuffer present
// ---------------------------------------------------------------------------

static void
test_pack_pixel_bgra_is_identity(void)
{
    /* AxlGfxPixel storage is BGRA: {blue, green, red, alpha}.  For a BGR
       framebuffer the packed word is the in-memory bit pattern itself:
       alpha<<24 | red<<16 | green<<8 | blue. */
    AxlGfxPixel px = AXL_GFX_RGBA(0x11, 0x22, 0x33, 0x44);  /* r,g,b,a */
    test_check(axl_gfx_pack_pixel(px, AXL_GFX_PIXEL_BGRA) == 0x44112233u,
               "pack_pixel: BGRA packs to a<<24|r<<16|g<<8|b (0x44112233)");
}

static void
test_pack_pixel_rgba_swaps_red_blue(void)
{
    /* For an RGB framebuffer red and blue swap positions:
       alpha<<24 | blue<<16 | green<<8 | red. */
    AxlGfxPixel px = AXL_GFX_RGBA(0x11, 0x22, 0x33, 0x44);  /* r,g,b,a */
    test_check(axl_gfx_pack_pixel(px, AXL_GFX_PIXEL_RGBA) == 0x44332211u,
               "pack_pixel: RGBA packs to a<<24|b<<16|g<<8|r (0x44332211)");
}

static void
test_pack_pixel_preserves_alpha_byte(void)
{
    /* The reserved/alpha byte rides through in the high byte for both
       orders (firmware ignores it, but the conversion must not corrupt
       the color channels by dropping it). */
    AxlGfxPixel opaque = AXL_GFX_RGB(0x80, 0x40, 0x20);     /* a = 0xFF */
    test_check(axl_gfx_pack_pixel(opaque, AXL_GFX_PIXEL_BGRA) == 0xFF804020u,
               "pack_pixel: BGRA carries opaque alpha (0xFF804020)");
    test_check(axl_gfx_pack_pixel(opaque, AXL_GFX_PIXEL_RGBA) == 0xFF204080u,
               "pack_pixel: RGBA carries opaque alpha (0xFF204080)");
}

// ---------------------------------------------------------------------------
// G18 — per-buffer damage tracking
// ---------------------------------------------------------------------------

static bool
clip_eq(
    AxlGfxClip  c,
    int32_t     x,
    int32_t     y,
    uint32_t    w,
    uint32_t    h
    )
{
    return c.x == x && c.y == y && c.w == w && c.h == h;
}

static void
test_damage_fresh_buffer_is_clean(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(64, 64);
    AxlGfxClip    d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_ERR,
               "damage: fresh buffer reports no damage");
    axl_gfx_buffer_free(b);
}

static void
test_damage_single_rect_roundtrips(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 100);
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){10, 20, 30, 40});
    AxlGfxClip d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_OK
               && clip_eq(d, 10, 20, 30, 40),
               "damage: single add roundtrips {10,20,30,40}");
    axl_gfx_buffer_free(b);
}

static void
test_damage_unions_bbox(void)
{
    /* {10,20,30,40} spans x∈[10,40) y∈[20,60); {5,5,10,10} spans
       x∈[5,15) y∈[5,15).  Union bbox: x∈[5,40) y∈[5,60). */
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 100);
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){10, 20, 30, 40});
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){5, 5, 10, 10});
    AxlGfxClip d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_OK
               && clip_eq(d, 5, 5, 35, 55),
               "damage: two adds union to bbox {5,5,35,55}");
    axl_gfx_buffer_free(b);
}

static void
test_damage_clamps_to_buffer(void)
{
    /* {90,90,50,50} on a 100x100 buffer clamps to x∈[90,100) y∈[90,100). */
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 100);
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){90, 90, 50, 50});
    AxlGfxClip d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_OK
               && clip_eq(d, 90, 90, 10, 10),
               "damage: oversize rect clamps to buffer edge {90,90,10,10}");
    axl_gfx_buffer_free(b);
}

static void
test_damage_clamps_negative_origin(void)
{
    /* {-10,-10,30,30} clamps its negative origin to 0 → x∈[0,20) y∈[0,20). */
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 100);
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){-10, -10, 30, 30});
    AxlGfxClip d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_OK
               && clip_eq(d, 0, 0, 20, 20),
               "damage: negative origin clamps to 0 {0,0,20,20}");
    axl_gfx_buffer_free(b);
}

static void
test_damage_empty_rect_is_noop(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 100);
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){10, 10, 0, 0});
    AxlGfxClip d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_ERR,
               "damage: empty (w==0,h==0) rect contributes nothing");
    axl_gfx_buffer_free(b);
}

static void
test_damage_fully_out_of_bounds_is_noop(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 100);
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){200, 200, 10, 10});
    AxlGfxClip d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_ERR,
               "damage: fully out-of-bounds rect contributes nothing");
    axl_gfx_buffer_free(b);
}

static void
test_damage_clear_resets(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(100, 100);
    axl_gfx_buffer_add_damage(b, (AxlGfxClip){10, 20, 30, 40});
    test_check(axl_gfx_buffer_clear_damage(b) == AXL_OK,
               "damage: clear returns AXL_OK");
    AxlGfxClip d;
    test_check(axl_gfx_buffer_get_damage(b, &d) == AXL_ERR,
               "damage: cleared buffer reports no damage");
    axl_gfx_buffer_free(b);
}

static void
test_damage_null_safe(void)
{
    AxlGfxClip d;
    test_check(axl_gfx_buffer_add_damage(NULL, (AxlGfxClip){0, 0, 1, 1}) == AXL_ERR,
               "damage: add_damage NULL buf -> AXL_ERR");
    test_check(axl_gfx_buffer_get_damage(NULL, &d) == AXL_ERR,
               "damage: get_damage NULL buf -> AXL_ERR");
    test_check(axl_gfx_buffer_clear_damage(NULL) == AXL_ERR,
               "damage: clear_damage NULL buf -> AXL_ERR");
    AxlGfxBuffer *b = axl_gfx_buffer_new(8, 8);
    test_check(axl_gfx_buffer_get_damage(b, NULL) == AXL_ERR,
               "damage: get_damage NULL out -> AXL_ERR");
    axl_gfx_buffer_free(b);
}

static void
test_present_rect_null_buf_errors(void)
{
    /* Region present rejects a NULL buffer regardless of GOP state. */
    test_check(axl_gfx_buffer_present_rect(NULL, 0, 0, 0, 0, 4, 4) == AXL_ERR,
               "present_rect: NULL buf -> AXL_ERR");
}

static void
test_present_damage_null_buf_errors(void)
{
    test_check(axl_gfx_buffer_present_damage(NULL, 0, 0) == AXL_ERR,
               "present_damage: NULL buf -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Display modes (GOP QueryMode / SetMode).  The -nographic unit suite has no
// GOP, so the live enumerate/switch path is verified by the --gpu
// gfx-mode-selftest; here we lock the NULL-arg + no-GOP contract (and stay
// correct if a GOP IS present, e.g. a --gpu run reuses this binary).
// ---------------------------------------------------------------------------

static void
test_display_modes_null_and_headless_contract(void)
{
    /* NULL-argument guards hold regardless of GOP state. */
    test_check(axl_gfx_query_mode(0, NULL) == AXL_ERR,
               "query_mode: NULL out -> AXL_ERR");
    test_check(axl_gfx_current_mode(NULL) == AXL_ERR,
               "current_mode: NULL out -> AXL_ERR");
    test_check(axl_gfx_find_mode(640, 480, NULL) == AXL_ERR,
               "find_mode: NULL out -> AXL_ERR");
    test_check(axl_gfx_max_mode(NULL) == AXL_ERR,
               "max_mode: NULL out -> AXL_ERR");

    uint32_t   idx = 12345;
    AxlGfxMode m;
    uint32_t   n = axl_gfx_mode_count();
    if (n == 0) {
        /* -nographic: no GOP, every mode op fails safe. */
        test_check(axl_gfx_query_mode(0, &m) == AXL_ERR,
                   "query_mode: no GOP -> AXL_ERR");
        test_check(axl_gfx_current_mode(&idx) == AXL_ERR,
                   "current_mode: no GOP -> AXL_ERR");
        test_check(axl_gfx_find_mode(640, 480, &idx) == AXL_ERR,
                   "find_mode: no GOP -> AXL_ERR");
        test_check(axl_gfx_max_mode(&m) == AXL_ERR,
                   "max_mode: no GOP -> AXL_ERR");
        test_check(axl_gfx_set_mode(0) == AXL_ERR,
                   "set_mode: no GOP -> AXL_ERR");
    } else {
        /* A GOP is present: basic invariants + out-of-range rejection.
           This branch asserts the SAME number of test_checks as the no-GOP
           branch above, so the unit ratchet stays balanced across arches:
           one arch's firmware exposes a headless GOP under -nographic and the
           other reports no modes, so the two branches must count equally. */
        test_check(axl_gfx_query_mode(0, &m) == AXL_OK && m.width > 0,
                   "query_mode: mode 0 has a positive width");
        test_check(axl_gfx_find_mode(m.width, m.height, &idx) == AXL_OK
                   && idx < n,
                   "find_mode: mode 0's dimensions are findable");
        test_check(axl_gfx_query_mode(n, &m) == AXL_ERR,
                   "query_mode: index == count -> AXL_ERR");
        test_check(axl_gfx_current_mode(&idx) == AXL_OK && idx < n,
                   "current_mode: in [0, count)");
        /* The max mode is enumerable and no smaller (by area) than mode 0. */
        AxlGfxMode mx, m0;
        test_check(axl_gfx_max_mode(&mx) == AXL_OK && mx.index < n
                   && axl_gfx_query_mode(0, &m0) == AXL_OK
                   && (uint64_t)mx.width * mx.height
                          >= (uint64_t)m0.width * m0.height,
                   "max_mode: largest enumerable area");
    }
}

// ---------------------------------------------------------------------------
// G12 — pattern fill / tile blit
// ---------------------------------------------------------------------------

/* A 2x2 pattern with four distinct texels:  [A B ; C D]. */
static const AxlGfxPixel PAT_A = {0x11, 0x00, 0x00, 0xFF};
static const AxlGfxPixel PAT_B = {0x00, 0x22, 0x00, 0xFF};
static const AxlGfxPixel PAT_C = {0x00, 0x00, 0x33, 0xFF};
static const AxlGfxPixel PAT_D = {0x44, 0x44, 0x44, 0xFF};

static AxlGfxBuffer *
make_pat2x2(void)
{
    AxlGfxBuffer *p = axl_gfx_buffer_new(2, 2);
    AxlGfxPixel  *px = axl_gfx_buffer_pixels(p);
    px[0] = PAT_A; px[1] = PAT_B;   /* row 0 */
    px[2] = PAT_C; px[3] = PAT_D;   /* row 1 */
    return p;
}

static void
test_pattern_repeat_both(void)
{
    AxlGfxBuffer *pat = make_pat2x2();
    AxlGfxBuffer *b   = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    int rc = axl_gfx_fill_pattern(0, 0, 4, 4, pat, AXL_GFX_REPEAT_BOTH);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(rc == AXL_OK, "pattern: repeat-both returns AXL_OK");
    test_check(px_eq(p, 4, 0, 0, PAT_A), "pattern: (0,0)=A");
    test_check(px_eq(p, 4, 1, 0, PAT_B), "pattern: (1,0)=B");
    test_check(px_eq(p, 4, 2, 0, PAT_A), "pattern: (2,0)=A (x wraps)");
    test_check(px_eq(p, 4, 1, 1, PAT_D), "pattern: (1,1)=D");
    test_check(px_eq(p, 4, 0, 2, PAT_A), "pattern: (0,2)=A (y wraps)");
    test_check(px_eq(p, 4, 3, 3, PAT_D), "pattern: (3,3)=D (both wrap)");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

static void
test_pattern_repeat_x(void)
{
    AxlGfxBuffer *pat = make_pat2x2();
    AxlGfxBuffer *b   = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_pattern(0, 0, 4, 4, pat, AXL_GFX_REPEAT_X);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 4, 2, 0, PAT_A), "pattern-x: (2,0)=A (x tiles)");
    test_check(px_eq(p, 4, 1, 1, PAT_D), "pattern-x: (1,1)=D");
    test_check(px_eq(p, 4, 0, 2, bg),    "pattern-x: (0,2)=bg (no y tile)");
    test_check(px_eq(p, 4, 3, 3, bg),    "pattern-x: (3,3)=bg");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

static void
test_pattern_repeat_y(void)
{
    AxlGfxBuffer *pat = make_pat2x2();
    AxlGfxBuffer *b   = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_pattern(0, 0, 4, 4, pat, AXL_GFX_REPEAT_Y);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 4, 0, 2, PAT_A), "pattern-y: (0,2)=A (y tiles)");
    test_check(px_eq(p, 4, 1, 1, PAT_D), "pattern-y: (1,1)=D");
    test_check(px_eq(p, 4, 2, 0, bg),    "pattern-y: (2,0)=bg (no x tile)");
    test_check(px_eq(p, 4, 3, 3, bg),    "pattern-y: (3,3)=bg");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

static void
test_pattern_repeat_none(void)
{
    AxlGfxBuffer *pat = make_pat2x2();
    AxlGfxBuffer *b   = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_pattern(0, 0, 4, 4, pat, AXL_GFX_REPEAT_NONE);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 4, 1, 1, PAT_D), "pattern-none: (1,1)=D (single copy)");
    test_check(px_eq(p, 4, 2, 0, bg),    "pattern-none: (2,0)=bg");
    test_check(px_eq(p, 4, 0, 2, bg),    "pattern-none: (0,2)=bg");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

static void
test_pattern_anchored_at_origin(void)
{
    /* Texel (0,0) lands at the rect's top-left (signed) origin. */
    AxlGfxBuffer *pat = make_pat2x2();
    AxlGfxBuffer *b   = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_pattern(1, 1, 2, 2, pat, AXL_GFX_REPEAT_BOTH);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 4, 1, 1, PAT_A), "pattern: anchored — texel(0,0) at (1,1)");
    test_check(px_eq(p, 4, 2, 2, PAT_D), "pattern: anchored — texel(1,1) at (2,2)");
    test_check(px_eq(p, 4, 0, 0, bg),    "pattern: anchored — (0,0) outside fill = bg");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

static void
test_pattern_honors_clip(void)
{
    AxlGfxBuffer *pat = make_pat2x2();
    AxlGfxBuffer *b   = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   bg  = {0x00, 0x00, 0x00, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_push_clip((AxlGfxClip){1, 1, 2, 2});
    axl_gfx_fill_pattern(0, 0, 4, 4, pat, AXL_GFX_REPEAT_BOTH);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 4, 1, 1, PAT_D), "pattern: clip — (1,1) inside drawn");
    test_check(px_eq(p, 4, 0, 0, bg),    "pattern: clip — (0,0) outside clip = bg");
    test_check(px_eq(p, 4, 3, 3, bg),    "pattern: clip — (3,3) outside clip = bg");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

static void
test_pattern_transparent_texel_shows_through(void)
{
    /* A fully-transparent texel leaves the destination untouched. */
    AxlGfxBuffer *pat = axl_gfx_buffer_new(2, 1);
    AxlGfxPixel  *pp  = axl_gfx_buffer_pixels(pat);
    pp[0] = PAT_A;
    pp[1] = (AxlGfxPixel){0, 0, 0, 0};   /* transparent */
    AxlGfxBuffer *b   = axl_gfx_buffer_new(4, 1);
    AxlGfxPixel   bg  = {0x09, 0x09, 0x09, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_pattern(0, 0, 4, 1, pat, AXL_GFX_REPEAT_BOTH);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    test_check(px_eq(p, 4, 0, 0, PAT_A), "pattern: opaque texel drawn");
    test_check(px_eq(p, 4, 1, 0, bg),    "pattern: transparent texel shows bg through");
    test_check(px_eq(p, 4, 2, 0, PAT_A), "pattern: opaque texel drawn (wrap)");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

static void
test_pattern_error_paths(void)
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   bg = {0x07, 0x07, 0x07, 0xFF};
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    test_check(axl_gfx_fill_pattern(0, 0, 4, 4, NULL, AXL_GFX_REPEAT_BOTH) == AXL_ERR,
               "pattern: NULL pattern -> AXL_ERR");
    AxlGfxBuffer *pat = make_pat2x2();
    int rc = axl_gfx_fill_pattern(0, 0, 0, 4, pat, AXL_GFX_REPEAT_BOTH);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "pattern: zero width -> AXL_OK no-op");
    test_check(px_eq(axl_gfx_buffer_pixels(b), 4, 0, 0, bg),
               "pattern: zero width leaves buffer unchanged");
    axl_gfx_buffer_free(pat);
    axl_gfx_buffer_free(b);
}

// ---------------------------------------------------------------------------
// G15 — gamma-correct (linear-light) compositing
// ---------------------------------------------------------------------------

static void
test_gamma_transfer_helpers(void)
{
    /* sRGB EOTF endpoints + the 128 reference (~0.2158 linear). */
    test_check(axl_gfx_srgb_to_linear(0) == 0.0f, "srgb_to_linear(0) = 0");
    test_check(axl_gfx_srgb_to_linear(255) > 0.999f, "srgb_to_linear(255) = 1");
    float l128 = axl_gfx_srgb_to_linear(128);
    test_check(l128 > 0.214f && l128 < 0.218f, "srgb_to_linear(128) ~ 0.2158");
    /* inverse OETF endpoints + the linear-0.5 reference (~188 sRGB). */
    test_check(axl_gfx_linear_to_srgb(0.0f) == 0, "linear_to_srgb(0) = 0");
    test_check(axl_gfx_linear_to_srgb(1.0f) == 255, "linear_to_srgb(1) = 255");
    uint8_t s50 = axl_gfx_linear_to_srgb(0.5f);
    test_check(s50 >= 187 && s50 <= 189, "linear_to_srgb(0.5) ~ 188");
    /* clamp out-of-range. */
    test_check(axl_gfx_linear_to_srgb(2.0f) == 255, "linear_to_srgb clamps high");
    test_check(axl_gfx_linear_to_srgb(-1.0f) == 0, "linear_to_srgb clamps low");
    /* round-trip within 1 LSB on a few codes. */
    bool rt_ok = true;
    uint8_t probes[] = {1, 17, 64, 130, 200, 254};
    for (size_t i = 0; i < sizeof(probes); i++) {
        uint8_t r = axl_gfx_linear_to_srgb(axl_gfx_srgb_to_linear(probes[i]));
        int d = (int)r - (int)probes[i];
        if (d < -1 || d > 1) rt_ok = false;
    }
    test_check(rt_ok, "srgb<->linear round-trips within 1 LSB");
}

static void
test_gamma_flag_state(void)
{
    test_check(axl_gfx_get_gamma_correct() == false, "gamma: default off");
    axl_gfx_set_gamma_correct(true);
    test_check(axl_gfx_get_gamma_correct() == true, "gamma: set true");
    axl_gfx_reset_gamma_correct();
    test_check(axl_gfx_get_gamma_correct() == false, "gamma: reset to off");
}

static void
test_gamma_composite_lightens_midpoint(void)
{
    /* 50%-alpha white over black: sRGB blend gives 128; linear-light
       gives ~188 (the physically-even midpoint). */
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   black = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   halfw = AXL_GFX_RGBA(0xFF, 0xFF, 0xFF, 0x80);  /* alpha 128 */

    axl_gfx_buffer_clear(b, black);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(0, 0, 4, 4, halfw);
    axl_gfx_target_buffer(NULL);
    uint8_t off = axl_gfx_buffer_pixels(b)[0].blue;

    axl_gfx_buffer_clear(b, black);
    axl_gfx_set_gamma_correct(true);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(0, 0, 4, 4, halfw);
    axl_gfx_target_buffer(NULL);
    uint8_t on = axl_gfx_buffer_pixels(b)[0].blue;
    axl_gfx_reset_gamma_correct();

    test_check(off == 128, "gamma off: 50% white/black composites to 128 (sRGB)");
    test_check(on >= 185 && on <= 191, "gamma on: 50% white/black composites to ~188 (linear)");
    axl_gfx_buffer_free(b);
}

static void
test_gamma_opaque_unaffected(void)
{
    /* Opaque draws don't blend, so gamma must not change them. */
    AxlGfxBuffer *b = axl_gfx_buffer_new(4, 4);
    AxlGfxPixel   black = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel   red   = AXL_GFX_RGB(0xC0, 0x20, 0x40);   /* opaque */

    axl_gfx_buffer_clear(b, black);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(0, 0, 4, 4, red);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel off = axl_gfx_buffer_pixels(b)[0];

    axl_gfx_buffer_clear(b, black);
    axl_gfx_set_gamma_correct(true);
    axl_gfx_target_buffer(b);
    axl_gfx_fill_rect(0, 0, 4, 4, red);
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel on = axl_gfx_buffer_pixels(b)[0];
    axl_gfx_reset_gamma_correct();

    test_check(px_eq(&on, 1, 0, 0, red), "gamma: opaque fill is exact (red)");
    test_check(on.blue == off.blue && on.green == off.green && on.red == off.red,
               "gamma: opaque fill identical on/off");
    axl_gfx_buffer_free(b);
}

static void
test_gamma_gradient_ramp_linear(void)
{
    /* G15b: with gamma on, a black→white gradient interpolates in linear
       light, so the midpoint is ~188 instead of the sRGB 128. */
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0.0f, 0.0f, 256.0f, 0.0f);
    AxlGfxPixel     black = {0x00, 0x00, 0x00, 0xFF};
    AxlGfxPixel     white = {0xFF, 0xFF, 0xFF, 0xFF};
    axl_gfx_gradient_add_stop(g, 0.0f, black);
    axl_gfx_gradient_add_stop(g, 1.0f, white);

    AxlGfxPixel off = axl_gfx_gradient_sample(g, 128, 0);
    axl_gfx_set_gamma_correct(true);
    AxlGfxPixel on = axl_gfx_gradient_sample(g, 128, 0);
    axl_gfx_reset_gamma_correct();

    test_check(off.blue >= 127 && off.blue <= 129,
               "gradient off: black→white midpoint ~128 (sRGB ramp)");
    test_check(on.blue >= 185 && on.blue <= 191,
               "gradient on: black→white midpoint ~188 (linear ramp)");
    axl_gfx_gradient_free(g);
}

// ---------------------------------------------------------------------------
// AxlEdid — EDID base-block parser (pure; unit-tested against a canned
// blob so the decode logic gets real coverage without a display)
// ---------------------------------------------------------------------------

/* Build a spec-faithful EDID 1.4 base block for a fictional 1920x1080
   digital "DELL U2412M" panel. Bytes are placed at their EDID offsets
   independently of the parser; byte 127 (checksum) is computed last so
   the block validates. */
static void
make_test_edid(
    uint8_t  e[128]
    )
{
    for (int i = 0; i < 128; i++) { e[i] = 0; }
    /* Header magic 00 FF FF FF FF FF FF 00. */
    e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xFF;
    /* Manufacturer "DEL" = (4<<10)|(5<<5)|12 = 0x10AC, big-endian. */
    e[8] = 0x10; e[9] = 0xAC;
    /* Product code 0x51A0 (LE), serial 0x04030201 (LE). */
    e[10] = 0xA0; e[11] = 0x51;
    e[12] = 0x01; e[13] = 0x02; e[14] = 0x03; e[15] = 0x04;
    e[16] = 16;   /* week 16 */
    e[17] = 33;   /* year 1990 + 33 = 2023 */
    e[18] = 1; e[19] = 4;   /* EDID 1.4 */
    e[20] = 0x80;           /* digital input (bit 7) */
    e[21] = 52; e[22] = 32; /* max image size cm (not asserted) */

    /* Detailed Timing Descriptor #1 at offset 54: 1920x1080,
       pixel clock 148.5 MHz, image 477x268 mm. */
    int d = 54;
    e[d + 0] = 0x02; e[d + 1] = 0x3A;  /* clock 14850 (x10 kHz) = 148500 kHz, LE */
    e[d + 2] = 0x80;  /* h active low  (1920 & 0xFF) */
    e[d + 3] = 0x18;  /* h blank  low  (280  & 0xFF) */
    e[d + 4] = 0x71;  /* hi nibbles: h active 7 (0x780), h blank 1 (0x118) */
    e[d + 5] = 0x38;  /* v active low  (1080 & 0xFF) */
    e[d + 6] = 0x2D;  /* v blank  low  (45) */
    e[d + 7] = 0x40;  /* hi nibbles: v active 4 (0x438), v blank 0 */
    e[d + 12] = 0xDD; /* h image size low (477 & 0xFF) */
    e[d + 13] = 0x0C; /* v image size low (268 & 0xFF) */
    e[d + 14] = 0x11; /* hi nibbles: h size 1 (0x1DD), v size 1 (0x10C) */

    /* Monitor Name descriptor (0xFC) at offset 72: "DELL U2412M". */
    int n = 72;
    e[n + 3] = 0xFC;
    const char *name = "DELL U2412M";
    int k = 0;
    for (; name[k]; k++) { e[n + 5 + k] = (uint8_t)name[k]; }
    e[n + 5 + k] = 0x0A;
    for (int j = n + 5 + k + 1; j < n + 18; j++) { e[j] = 0x20; }

    /* Monitor Serial descriptor (0xFF) at offset 90: "ABC123". */
    int s = 90;
    e[s + 3] = 0xFF;
    const char *ser = "ABC123";
    k = 0;
    for (; ser[k]; k++) { e[s + 5 + k] = (uint8_t)ser[k]; }
    e[s + 5 + k] = 0x0A;
    for (int j = s + 5 + k + 1; j < s + 18; j++) { e[j] = 0x20; }

    /* Unused dummy descriptor at offset 108. */
    e[108 + 3] = 0x10;

    e[126] = 1;  /* extension count */

    /* Checksum: all 128 bytes sum to 0 mod 256. */
    unsigned sum = 0;
    for (int i = 0; i < 127; i++) { sum += e[i]; }
    e[127] = (uint8_t)((256u - (sum & 0xFFu)) & 0xFFu);
}

static void
test_edid_parse_valid(void)
{
    uint8_t e[128];
    make_test_edid(e);
    AxlEdidInfo info;
    test_check(axl_edid_parse(e, sizeof e, &info) == AXL_OK,
               "edid: valid base block parses");
    test_check(axl_strcmp(info.manufacturer, "DEL") == 0,
               "edid: manufacturer == DEL");
    test_check(info.product_code == 0x51A0, "edid: product_code == 0x51A0");
    test_check(info.serial_number == 0x04030201u,
               "edid: serial_number == 0x04030201");
    test_check(info.manufacture_week == 16, "edid: week == 16");
    test_check(info.manufacture_year == 2023, "edid: year == 2023");
    test_check(info.version == 1 && info.revision == 4,
               "edid: version 1.4");
    test_check(info.digital, "edid: digital input");
    test_check(info.native_width == 1920, "edid: native_width == 1920");
    test_check(info.native_height == 1080, "edid: native_height == 1080");
    test_check(info.native_pixel_clock_khz == 148500,
               "edid: pixel clock == 148500 kHz");
    test_check(info.image_width_mm == 477, "edid: image_width_mm == 477");
    test_check(info.image_height_mm == 268, "edid: image_height_mm == 268");
    test_check(info.extension_count == 1, "edid: extension_count == 1");
    test_check(axl_strcmp(info.monitor_name, "DELL U2412M") == 0,
               "edid: monitor_name == 'DELL U2412M'");
    test_check(axl_strcmp(info.monitor_serial, "ABC123") == 0,
               "edid: monitor_serial == 'ABC123'");
}

static void
test_edid_dpi(void)
{
    uint8_t e[128];
    make_test_edid(e);
    AxlEdidInfo info;
    (void)axl_edid_parse(e, sizeof e, &info);
    uint32_t dx = 0, dy = 0;
    /* 1920 * 25.4 / 477 = 102.2; 1080 * 25.4 / 268 = 102.4 → both 102. */
    test_check(axl_edid_dpi(&info, &dx, &dy) == AXL_OK, "edid: dpi computes");
    test_check(dx == 102, "edid: dpi_x == 102");
    test_check(dy == 102, "edid: dpi_y == 102");
    /* A zero-size EDID can't yield DPI. */
    AxlEdidInfo zero = {0};
    test_check(axl_edid_dpi(&zero, &dx, &dy) == AXL_ERR,
               "edid: dpi errors on zero image size");
}

static void
test_edid_rejects_bad(void)
{
    uint8_t e[128];
    make_test_edid(e);
    AxlEdidInfo info;
    /* Corrupt a payload byte without fixing the checksum → reject. */
    uint8_t saved = e[10];
    e[10] = (uint8_t)(saved ^ 0xFF);
    test_check(axl_edid_parse(e, sizeof e, &info) == AXL_ERR,
               "edid: bad checksum rejected");
    e[10] = saved;
    test_check(axl_edid_parse(e, sizeof e, &info) == AXL_OK,
               "edid: restored block valid again");
    /* Bad header magic. */
    e[0] = 0x01;
    test_check(axl_edid_parse(e, sizeof e, &info) == AXL_ERR,
               "edid: bad header magic rejected");
    /* Short buffer. */
    make_test_edid(e);
    test_check(axl_edid_parse(e, 127, &info) == AXL_ERR,
               "edid: buffer < 128 bytes rejected");
    /* NULL args. */
    test_check(axl_edid_parse(NULL, 128, &info) == AXL_ERR,
               "edid: NULL edid rejected");
    test_check(axl_edid_parse(e, 128, NULL) == AXL_ERR,
               "edid: NULL out rejected");
}

// ---------------------------------------------------------------------------
// AxlGfx pixel-format / EDID accessors. NULL-arg guards hold regardless
// of GOP state; the available-vs-headless behavior is split into two
// branches with EQUAL test_check counts so the cross-arch ratchet stays
// balanced — x64 OVMF exposes a GOP even under -nographic while aa64
// reports none (same split the modes contract test handles).
// ---------------------------------------------------------------------------

static void
test_gfx_pixel_accessors_contract(void)
{
    AxlGfxPixelFormat  fmt = AXL_GFX_PIXEL_FORMAT_RGBX8;
    AxlGfxPixelBitmask bm;
    const uint8_t     *edid = NULL;
    size_t             len  = 0;

    /* NULL-argument guards — true on every platform. */
    test_check(axl_gfx_get_pixel_format(NULL) == AXL_ERR,
               "gfx: get_pixel_format rejects NULL out");
    test_check(axl_gfx_get_pixel_bitmask(NULL) == AXL_ERR,
               "gfx: get_pixel_bitmask rejects NULL out");
    test_check(axl_gfx_get_edid(NULL, &len) == AXL_ERR,
               "gfx: get_edid rejects NULL bytes");
    test_check(axl_gfx_get_edid(&edid, NULL) == AXL_ERR,
               "gfx: get_edid rejects NULL len");

    if (axl_gfx_available()) {
        /* A real GOP is present: the positive path resolves a valid
           format; bitmask reads succeed iff the format is bitmask; an
           EDID, if published, is at least a full base block. */
        test_check(axl_gfx_get_pixel_format(&fmt) == AXL_OK
                   && fmt <= AXL_GFX_PIXEL_FORMAT_BLT_ONLY,
                   "gfx: get_pixel_format resolves a valid format");
        test_check((axl_gfx_get_pixel_bitmask(&bm) == AXL_OK)
                   == (fmt == AXL_GFX_PIXEL_FORMAT_BITMASK),
                   "gfx: get_pixel_bitmask succeeds iff format is bitmask");
        test_check(axl_gfx_get_edid(&edid, &len) != AXL_OK
                   || (edid != NULL && len >= AXL_EDID_BLOCK_SIZE),
                   "gfx: get_edid, when present, yields a full base block");
    } else {
        /* Headless (no GOP): every accessor fails safe. */
        test_check(axl_gfx_get_pixel_format(&fmt) == AXL_ERR,
                   "gfx: get_pixel_format AXL_ERR when no GOP");
        test_check(axl_gfx_get_pixel_bitmask(&bm) == AXL_ERR,
                   "gfx: get_pixel_bitmask AXL_ERR when no GOP");
        test_check(axl_gfx_get_edid(&edid, &len) == AXL_ERR,
                   "gfx: get_edid AXL_ERR when no GOP");
    }
}

// ---------------------------------------------------------------------------
// axl_gfx_set_native_mode contract. No display in QEMU publishes EDID,
// so the native timing never resolves and the call reports AXL_ERR on
// both arches — and, critically, must do so WITHOUT switching the mode
// (the EDID checks precede SetMode). The positive switch-to-native path
// is real-hardware-only (needs a panel with EDID). Two checks per
// branch keep the cross-arch ratchet balanced.
// ---------------------------------------------------------------------------

static void
test_gfx_native_mode_contract(void)
{
    uint32_t before = 0, after = 0;
    bool had_mode = (axl_gfx_current_mode(&before) == AXL_OK);

    test_check(axl_gfx_set_native_mode() == AXL_ERR,
               "gfx: set_native_mode AXL_ERR when no EDID native timing");

    if (had_mode) {
        /* It failed before reaching SetMode, so the mode is untouched. */
        test_check(axl_gfx_current_mode(&after) == AXL_OK && after == before,
                   "gfx: failed set_native_mode left the mode unchanged");
    } else {
        test_check(axl_gfx_current_mode(&after) == AXL_ERR,
                   "gfx: no GOP -> current_mode still AXL_ERR after attempt");
    }
}

// ---------------------------------------------------------------------------
// DPI / scale. axl_gfx_scale_for_dpi is pure (exact-value tested, arch-
// independent); axl_gfx_get_dpi / recommended_scale are EDID glue, so
// their EDID-present path is real-hardware-only — here we pin the
// no-EDID contract (AXL_ERR / scale 1), identical on both arches.
// ---------------------------------------------------------------------------

static void
test_gfx_scale_for_dpi(void)
{
    /* Threshold boundaries: <144 → 1, 144..239 → 2, >=240 → 3. */
    test_check(axl_gfx_scale_for_dpi(0) == 1,   "scale_for_dpi: 0 -> 1");
    test_check(axl_gfx_scale_for_dpi(96) == 1,  "scale_for_dpi: 96 -> 1");
    test_check(axl_gfx_scale_for_dpi(143) == 1, "scale_for_dpi: 143 -> 1");
    test_check(axl_gfx_scale_for_dpi(144) == 2, "scale_for_dpi: 144 -> 2");
    test_check(axl_gfx_scale_for_dpi(200) == 2, "scale_for_dpi: 200 -> 2");
    test_check(axl_gfx_scale_for_dpi(239) == 2, "scale_for_dpi: 239 -> 2");
    test_check(axl_gfx_scale_for_dpi(240) == 3, "scale_for_dpi: 240 -> 3");
    test_check(axl_gfx_scale_for_dpi(400) == 3, "scale_for_dpi: 400 -> 3");
}

static void
test_gfx_dpi_contract(void)
{
    /* No EDID in QEMU → DPI unavailable on both arches. */
    uint32_t dx = 7, dy = 7;
    test_check(axl_gfx_get_dpi(&dx, &dy) == AXL_ERR,
               "gfx: get_dpi AXL_ERR without EDID");
    /* recommended_scale defaults to 1 (no scaling) when DPI is unknown. */
    test_check(axl_gfx_recommended_scale() == 1,
               "gfx: recommended_scale defaults to 1 without EDID");
}

// ---------------------------------------------------------------------------
// Multi-output enumeration. Unlike the EDID-gated helpers, this only
// needs a GOP — so the positive path IS exercised on x64 (OVMF exposes a
// GOP under -nographic) while aa64 takes the no-output branch. Balanced
// check counts keep the cross-arch ratchet even.
// ---------------------------------------------------------------------------

static void
test_gfx_output_contract(void)
{
    AxlGfxOutput o;
    size_t       n = axl_gfx_output_count();

    /* Guards — both arches. */
    test_check(axl_gfx_output_get(0, NULL) == AXL_ERR,
               "gfx: output_get rejects NULL out");
    test_check(axl_gfx_output_get(n + 100, &o) == AXL_ERR,
               "gfx: output_get rejects out-of-range index");

    if (n > 0) {
        test_check(axl_gfx_output_get(0, &o) == AXL_OK
                   && o.width >= 1 && o.height >= 1 && o.stride >= o.width
                   && o.pixel_format <= AXL_GFX_PIXEL_FORMAT_BLT_ONLY,
                   "gfx: output_get(0) populates a valid output");
        test_check(o.edid == NULL ? (o.edid_len == 0)
                                  : (o.edid_len >= AXL_EDID_BLOCK_SIZE),
                   "gfx: output EDID is NULL/empty or a full base block");
    } else {
        test_check(axl_gfx_output_get(0, &o) == AXL_ERR,
                   "gfx: output_get(0) AXL_ERR when no outputs");
        test_check(axl_gfx_output_count() == 0,
                   "gfx: output_count 0 when no GOP");
    }
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


    test_clip_quad_axis_aligned_matches_rect();
    test_clip_quad_diamond();
    test_clip_quad_winding_independent();
    test_clip_quad_intersects_existing_clip();
    test_clip_quad_degenerate_clips_all();
    test_clip_quad_bbox_and_pop();
    test_clip_quad_blit_honors();
    test_blit_rect_equals_blit_for_full_rect();
    test_blit_rect_interior_subrect();
    test_blit_rect_honors_stride();
    test_blit_rect_target_edge_clip();
    test_blit_rect_negatives();
    test_clip_quad_alpha_blend_honors();
    test_clip_quad_nested();
    test_clip_quad_bitmap_text_honors();
    test_clip_rect_transformed();
    test_clip_path_concave();
    test_clip_path_nested();

    test_fill_path_tall_narrow_high_offset();

    test_ttf_transform_null_args();
    test_ttf_transform_renders_in_bbox();
    test_ttf_transform_high_offset_renders();
    test_ttf_transform_rotation_transposes_bbox();
    test_ttf_transform_matches_draw_position();
    test_ttf_transform_latin1_codepoint();
    test_ttf_transform_multicontour_hole();
    test_ttf_transform_perspective_renders();

    test_blit_transform_null_args();
    test_blit_transform_identity_places_image();
    test_blit_transform_scale_covers_area();
    test_blit_transform_rotation_transposes();
    test_blit_transform_bilinear_blends();
    test_blit_transform_alpha_source();
    test_blit_transform_minify();
    test_blit_transform_shear();
    test_blit_transform_perspective_trapezoid();
    test_blit_transform_perspective_negative_orientation();

    test_blend_ex_modes();
    test_blend_mode_state_and_fill();

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

    test_color_parse_null_args();
    test_color_parse_rrggbb();
    test_color_parse_rrggbbaa();
    test_color_parse_rgb_short();
    test_color_parse_rgba_short();
    test_color_parse_case_insensitive();
    test_color_parse_opaque_default();
    test_color_parse_rejects_missing_hash();
    test_color_parse_rejects_bad_length();
    test_color_parse_rejects_non_hex();
    test_color_parse_untouched_on_error();
    test_color_parse_roundtrips_dump_format();

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
    test_fill_path_orientation_not_flipped();
    test_fill_path_analytic_partial_coverage();
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
    test_stroke_path_width_band();
    test_stroke_path_width_scales();
    test_stroke_round_cap_extends();
    test_stroke_butt_cap_flush();
    test_stroke_square_cap_extends();
    test_stroke_miter_join_fills_corner();
    test_stroke_miter_no_inner_spill();
    test_stroke_dashes_create_gaps();
    test_stroke_dash_offset_shifts();
    test_stroke_dash_degenerate_is_solid();
    test_stroke_path_ex_null_args();
    test_stroke_path_zero_width_noop();

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
    test_gradient_sample_query();
    test_gradient_fill_path();
    test_gradient_fill_rounded_rect();

    test_blur_null_and_noop();
    test_blur_uniform_stays_uniform();
    test_blur_impulse_properties();
    test_blur_vertical_axis();
    test_shadow_null_and_transparent();
    test_shadow_soft_and_bounded();
    test_shadow_color_not_darkened_at_edges();

    test_dl_new_empty();
    test_dl_null_safety();
    test_dl_op_at_out_of_range();
    test_dl_clear_resets();
    test_dl_records_fill_rect_params();
    test_dl_records_fill_rect_i_negative();
    test_dl_records_line_and_rect_and_rounded();
    test_dl_records_clip_ops();
    test_dl_records_fill_path_borrows();
    test_dl_polyline_copies_points();
    test_dl_text_copies_string();
    test_dl_text_ttf_copies_string();
    test_dl_stroke_copies_dashes();
    test_dl_blit_copies_pixels();
    test_dl_validation_errors();
    test_dl_replay_matches_immediate();
    test_dl_replay_clear_fills_buffer();
    test_dl_replay_honors_recorded_clip();
    test_dl_replay_empty_is_ok();
    test_dl_oom_new();
    test_dl_oom_record_copy();
    test_dl_oom_record_append();

    test_dl_records_gradient_ops();
    test_dl_gradient_validation();
    test_dl_records_transform_ops();
    test_dl_replay_gradient_matches_immediate();
    test_dl_replay_transform_affects_path();
    test_dl_replay_push_pop_transform_balances();

    test_dl_dump_null_args();
    test_dl_dump_empty_writes_nothing();
    test_dl_dump_basic_ops();
    test_dl_dump_clip_and_line();
    test_dl_dump_floats();
    test_dl_dump_scale_rotate_skew();
    test_dl_dump_gradient_ops();
    test_dl_dump_text_escaping();
    test_dl_dump_text_ttf();
    test_dl_dump_all_kinds_one_line_each();

    test_pack_pixel_bgra_is_identity();
    test_pack_pixel_rgba_swaps_red_blue();
    test_pack_pixel_preserves_alpha_byte();

    test_damage_fresh_buffer_is_clean();
    test_damage_single_rect_roundtrips();
    test_damage_unions_bbox();
    test_damage_clamps_to_buffer();
    test_damage_clamps_negative_origin();
    test_damage_empty_rect_is_noop();
    test_damage_fully_out_of_bounds_is_noop();
    test_damage_clear_resets();
    test_damage_null_safe();
    test_present_rect_null_buf_errors();
    test_present_damage_null_buf_errors();
    test_display_modes_null_and_headless_contract();

    test_pattern_repeat_both();
    test_pattern_repeat_x();
    test_pattern_repeat_y();
    test_pattern_repeat_none();
    test_pattern_anchored_at_origin();
    test_pattern_honors_clip();
    test_pattern_transparent_texel_shows_through();
    test_pattern_error_paths();

    test_gamma_transfer_helpers();
    test_gamma_flag_state();
    test_gamma_composite_lightens_midpoint();
    test_gamma_opaque_unaffected();
    test_gamma_gradient_ramp_linear();

    test_edid_parse_valid();
    test_edid_dpi();
    test_edid_rejects_bad();

    test_gfx_pixel_accessors_contract();
    test_gfx_native_mode_contract();
    test_gfx_scale_for_dpi();
    test_gfx_dpi_contract();
    test_gfx_output_contract();

    return test_print_results();
}

AXL_APP(test_gfx_main)
