/** @file axl-test-truetype.c
    Unit tests for the truetype module (G1 — vector text via
    stb_truetype).

    Two batches:
      - NULL / invalid-input contract — exercises early-return paths
        without needing a loaded font.
      - Positive cases — exercises measure / measure_prefix / metrics
        against a real loaded font (ASCII subset of DejaVu Sans
        embedded as test/data/test-font-dejavu-ascii.h).

    Glyph rasterization tests are deferred — axl_ttf_draw's pixel-
    producing path lands in a follow-up batch.
**/

#include "axl-test.h"

#include <axl/axl-truetype.h>
#include <axl/axl-gfx.h>

#include "test-font-dejavu-ascii.h"

/* Sentinel pointer used to exercise non-ttf NULL checks without
 * needing a real loaded font. The real implementation must check
 * the OTHER args (utf8 == NULL, px_size <= 0) before dereferencing
 * ttf — this sentinel would fault on deref, which is the point. */
static AxlTtf *const FAKE_TTF = (AxlTtf *)(uintptr_t)0x1;

/* Small invalid byte buffers for axl_ttf_load failure paths. */
static const uint8_t too_short[4]  = { 0x00, 0x01, 0x00, 0x00 };
static const uint8_t zero_buf[256] = { 0 };
static const uint8_t garbage[16]   = { 'n','o','t','_','a','_','t','t','f',
                                       '_','f','i','l','e','!','!' };

// ---------------------------------------------------------------------------
// axl_ttf_load — input validation
// ---------------------------------------------------------------------------

static void
test_load_null_bytes(void)
{
    test_check(axl_ttf_load(NULL, 100) == NULL,
               "load: NULL bytes returns NULL");
}

static void
test_load_zero_len(void)
{
    test_check(axl_ttf_load(zero_buf, 0) == NULL,
               "load: zero len returns NULL");
}

static void
test_load_too_short(void)
{
    test_check(axl_ttf_load(too_short, sizeof too_short) == NULL,
               "load: buffer smaller than TTF header returns NULL");
}

static void
test_load_zero_buffer(void)
{
    /* All-zero bytes is not a valid TTF signature. */
    test_check(axl_ttf_load(zero_buf, sizeof zero_buf) == NULL,
               "load: all-zero buffer returns NULL");
}

static void
test_load_garbage_bytes(void)
{
    test_check(axl_ttf_load(garbage, sizeof garbage) == NULL,
               "load: non-TTF bytes returns NULL");
}

// ---------------------------------------------------------------------------
// axl_ttf_measure — input validation
// ---------------------------------------------------------------------------

static void
test_measure_null_ttf(void)
{
    test_check(axl_ttf_measure(NULL, "Hello", 16.0f) == 0,
               "measure: NULL ttf returns 0");
}

static void
test_measure_null_utf8(void)
{
    test_check(axl_ttf_measure(FAKE_TTF, NULL, 16.0f) == 0,
               "measure: NULL utf8 returns 0");
}

static void
test_measure_zero_px_size(void)
{
    test_check(axl_ttf_measure(FAKE_TTF, "Hello", 0.0f) == 0,
               "measure: px_size 0 returns 0");
}

static void
test_measure_negative_px_size(void)
{
    test_check(axl_ttf_measure(FAKE_TTF, "Hello", -1.0f) == 0,
               "measure: negative px_size returns 0");
}

// ---------------------------------------------------------------------------
// axl_ttf_measure_prefix — input validation
// ---------------------------------------------------------------------------

static void
test_measure_prefix_null_ttf(void)
{
    test_check(axl_ttf_measure_prefix(NULL, "Hello", 3, 16.0f) == 0,
               "measure_prefix: NULL ttf returns 0");
}

static void
test_measure_prefix_null_utf8(void)
{
    test_check(axl_ttf_measure_prefix(FAKE_TTF, NULL, 3, 16.0f) == 0,
               "measure_prefix: NULL utf8 returns 0");
}

static void
test_measure_prefix_zero_bytes(void)
{
    /* prefix_bytes == 0 means "measure nothing" — width is 0 even
     * for a perfectly valid utf8 string. */
    test_check(axl_ttf_measure_prefix(FAKE_TTF, "Hello", 0, 16.0f) == 0,
               "measure_prefix: prefix_bytes 0 returns 0");
}

static void
test_measure_prefix_zero_px_size(void)
{
    test_check(axl_ttf_measure_prefix(FAKE_TTF, "Hello", 3, 0.0f) == 0,
               "measure_prefix: px_size 0 returns 0");
}

// ---------------------------------------------------------------------------
// axl_ttf_draw — input validation
// ---------------------------------------------------------------------------

static void
test_draw_null_ttf(void)
{
    test_check(axl_ttf_draw(NULL, 0, 0, "Hello", 16.0f, AXL_GFX_BLACK) == AXL_ERR,
               "draw: NULL ttf returns AXL_ERR");
}

static void
test_draw_null_utf8(void)
{
    test_check(axl_ttf_draw(FAKE_TTF, 0, 0, NULL, 16.0f, AXL_GFX_BLACK) == AXL_ERR,
               "draw: NULL utf8 returns AXL_ERR");
}

static void
test_draw_zero_px_size(void)
{
    test_check(axl_ttf_draw(FAKE_TTF, 0, 0, "Hello", 0.0f, AXL_GFX_BLACK) == AXL_ERR,
               "draw: px_size 0 returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_ttf_metrics — input validation
// ---------------------------------------------------------------------------

static void
test_metrics_null_ttf(void)
{
    float a, d, g;
    test_check(axl_ttf_metrics(NULL, 16.0f, &a, &d, &g) == AXL_ERR,
               "metrics: NULL ttf returns AXL_ERR");
}

static void
test_metrics_zero_px_size(void)
{
    float a, d, g;
    test_check(axl_ttf_metrics(FAKE_TTF, 0.0f, &a, &d, &g) == AXL_ERR,
               "metrics: px_size 0 returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// Positive cases — real loaded font
// ---------------------------------------------------------------------------

/* Load helper: every positive test starts by loading the embedded
 * DejaVu ASCII subset.  The subset is small enough (~14KB) that
 * the per-test load cost is negligible against the QEMU round-trip. */
static AxlTtf *
load_test_font(void)
{
    return axl_ttf_load(test_font_dejavu_ascii,
                        (size_t)test_font_dejavu_ascii_len);
}

static void
test_load_valid_font_returns_non_null(void)
{
    AxlTtf *ttf = load_test_font();
    test_check(ttf != NULL,
               "load: valid DejaVu ASCII subset returns non-NULL");
    axl_ttf_free(ttf);
}

static void
test_measure_returns_positive_for_non_empty(void)
{
    AxlTtf *ttf = load_test_font();
    test_check(axl_ttf_measure(ttf, "H", 16.0f) > 0,
               "measure: 'H' at 16px is > 0 pixels wide");
    axl_ttf_free(ttf);
}

static void
test_measure_monotonic_in_length(void)
{
    AxlTtf *ttf = load_test_font();
    uint32_t w_h     = axl_ttf_measure(ttf, "H",     16.0f);
    uint32_t w_hi    = axl_ttf_measure(ttf, "Hi",    16.0f);
    uint32_t w_hello = axl_ttf_measure(ttf, "Hello", 16.0f);
    test_check(w_hi > w_h,
               "measure: 'Hi' wider than 'H'");
    test_check(w_hello > w_hi,
               "measure: 'Hello' wider than 'Hi'");
    axl_ttf_free(ttf);
}

static void
test_measure_empty_returns_zero(void)
{
    AxlTtf *ttf = load_test_font();
    test_check(axl_ttf_measure(ttf, "", 16.0f) == 0,
               "measure: empty string returns 0");
    axl_ttf_free(ttf);
}

static void
test_measure_scales_with_px_size(void)
{
    AxlTtf *ttf = load_test_font();
    uint32_t w16 = axl_ttf_measure(ttf, "Hello", 16.0f);
    uint32_t w32 = axl_ttf_measure(ttf, "Hello", 32.0f);
    /* Vector text — doubling the size doubles the rendered width
     * (modulo rounding).  Be permissive (>= 1.8x, <= 2.2x) to
     * tolerate per-glyph rounding accumulation. */
    test_check(w32 >= w16 * 18 / 10,
               "measure: 32px width is >= 1.8x 16px width");
    test_check(w32 <= w16 * 22 / 10,
               "measure: 32px width is <= 2.2x 16px width");
    axl_ttf_free(ttf);
}

static void
test_measure_prefix_equals_full_for_strlen(void)
{
    AxlTtf *ttf = load_test_font();
    const char *s = "Hello";
    uint32_t whole  = axl_ttf_measure(ttf, s, 16.0f);
    uint32_t prefix = axl_ttf_measure_prefix(ttf, s, 5, 16.0f);
    test_check(whole == prefix,
               "measure_prefix: full byte length matches measure()");
    axl_ttf_free(ttf);
}

static void
test_measure_prefix_zero_returns_zero(void)
{
    AxlTtf *ttf = load_test_font();
    test_check(axl_ttf_measure_prefix(ttf, "Hello", 0, 16.0f) == 0,
               "measure_prefix: 0 bytes returns 0");
    axl_ttf_free(ttf);
}

static void
test_measure_prefix_monotonic(void)
{
    /* Cursor-positioning correctness: prefix width must increase as
     * the byte count grows.  Use STRICT > (not >=) since every glyph
     * in "Hello" has positive advance — a constant-width bug would
     * pass a non-strict check. */
    AxlTtf *ttf = load_test_font();
    const char *s = "Hello";
    uint32_t prev = 0;
    bool strictly_increasing = true;
    for (size_t n = 1; n <= 5; n++) {
        uint32_t w = axl_ttf_measure_prefix(ttf, s, n, 16.0f);
        if (w <= prev) {
            strictly_increasing = false;
            break;
        }
        prev = w;
    }
    test_check(strictly_increasing,
               "measure_prefix: width strictly increasing for ASCII");
    axl_ttf_free(ttf);
}

static void
test_measure_prefix_mid_codepoint_stops(void)
{
    /* Contract (axl-truetype.h): "If prefix_bytes lands mid-codepoint,
     * the trailing partial codepoint is NOT counted."
     *
     * "A中B" — 'A' is 1 byte, '中' is 3 bytes (0xE4 0xB8 0xAD), 'B'
     * is 1 byte. Total 5 bytes.  Byte positions 2 and 3 land inside
     * '中' — prefix width should equal prefix(1) (just 'A').  Byte
     * position 4 includes the complete '中' (which renders as the
     * font's .notdef glyph since the ASCII subset doesn't carry it).
     */
    AxlTtf *ttf = load_test_font();
    /* String-literal concatenation terminates the trailing hex escape
     * so the following 'B' doesn't get absorbed (`\xADB` would
     * silently truncate to 0xDB). */
    const char *s = "A\xE4\xB8\xAD" "B";  /* "A中B" */

    uint32_t w1 = axl_ttf_measure_prefix(ttf, s, 1, 16.0f);
    uint32_t w2 = axl_ttf_measure_prefix(ttf, s, 2, 16.0f);
    uint32_t w3 = axl_ttf_measure_prefix(ttf, s, 3, 16.0f);
    uint32_t w4 = axl_ttf_measure_prefix(ttf, s, 4, 16.0f);
    uint32_t w5 = axl_ttf_measure_prefix(ttf, s, 5, 16.0f);

    test_check(w2 == w1,
               "measure_prefix: byte 2 lands mid-codepoint, width == prefix(1)");
    test_check(w3 == w1,
               "measure_prefix: byte 3 still mid-codepoint, width == prefix(1)");
    test_check(w4 >= w1,
               "measure_prefix: byte 4 includes complete codepoint, width >= prefix(1)");
    test_check(w5 > w4,
               "measure_prefix: trailing ASCII 'B' increments width past byte 4");
    axl_ttf_free(ttf);
}

static void
test_measure_invalid_utf8_continues(void)
{
    /* Contract (axl-truetype.h): "Invalid UTF-8 sequences become
     * U+FFFD REPLACEMENT CHARACTER."  Implementation must walk past
     * the invalid byte rather than bailing at byte 0. */
    AxlTtf *ttf = load_test_font();
    const char *just_a   = "A";
    const char *a_garbage = "A\xff";  /* 'A' then invalid lead byte */

    uint32_t w_a   = axl_ttf_measure(ttf, just_a,   16.0f);
    uint32_t w_a_g = axl_ttf_measure(ttf, a_garbage, 16.0f);

    test_check(w_a > 0,
               "measure: 'A' has positive width");
    /* The invalid byte becomes U+FFFD and renders as .notdef (which
     * may or may not have width); strict >= 'A' alone proves the
     * decoder didn't bail at the invalid byte. */
    test_check(w_a_g >= w_a,
               "measure: invalid UTF-8 byte advances pen (U+FFFD substitution)");
    axl_ttf_free(ttf);
}

static void
test_metrics_ascent_descent_positive(void)
{
    AxlTtf *ttf = load_test_font();
    float ascent = 0.0f, descent = 0.0f, line_gap = -1.0f;
    int rc = axl_ttf_metrics(ttf, 16.0f, &ascent, &descent, &line_gap);
    test_check(rc == AXL_OK,
               "metrics: valid call returns AXL_OK");
    test_check(ascent > 0.0f,
               "metrics: ascent is positive");
    test_check(descent > 0.0f,
               "metrics: descent normalized to positive magnitude");
    test_check(line_gap >= 0.0f,
               "metrics: line_gap is non-negative");
    axl_ttf_free(ttf);
}

static void
test_metrics_all_null_outputs(void)
{
    /* Contract: each output pointer may be NULL to skip computing
     * that field.  All-NULL should still return AXL_OK (caller may
     * just be probing whether the font is usable). */
    AxlTtf *ttf = load_test_font();
    int rc = axl_ttf_metrics(ttf, 16.0f, NULL, NULL, NULL);
    test_check(rc == AXL_OK,
               "metrics: all-NULL outputs returns AXL_OK");
    axl_ttf_free(ttf);
}

static void
test_metrics_scales_linearly(void)
{
    AxlTtf *ttf = load_test_font();
    float a16 = 0.0f, a32 = 0.0f;
    axl_ttf_metrics(ttf, 16.0f, &a16, NULL, NULL);
    axl_ttf_metrics(ttf, 32.0f, &a32, NULL, NULL);
    /* Linear scaling: 32px ascent should be ~2x 16px ascent.
     * Allow tight tolerance since metrics aren't rounded. */
    test_check(a32 >= a16 * 1.95f && a32 <= a16 * 2.05f,
               "metrics: ascent scales linearly with px_size");
    axl_ttf_free(ttf);
}

// ---------------------------------------------------------------------------
// Rasterization — pixel-producing axl_ttf_draw
// ---------------------------------------------------------------------------

/* Count pixels in @a buf inside the rect (@a x, @a y, @a w, @a h)
 * that differ from @a bg.  Loose "did anything draw here?" check
 * tolerant of font-specific glyph shapes. */
static size_t
count_non_bg_pixels(
    AxlGfxBuffer  *buf,
    uint32_t       x,
    uint32_t       y,
    uint32_t       w,
    uint32_t       h,
    AxlGfxPixel    bg
    )
{
    uint32_t buf_w = 0, buf_h = 0;
    axl_gfx_buffer_get_info(buf, &buf_w, &buf_h);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(buf);
    size_t n = 0;
    for (uint32_t py = y; py < y + h && py < buf_h; py++) {
        for (uint32_t px = x; px < x + w && px < buf_w; px++) {
            const AxlGfxPixel *q = &p[py * buf_w + px];
            if (q->blue != bg.blue
                || q->green != bg.green
                || q->red != bg.red)
            {
                n++;
            }
        }
    }
    return n;
}

static void
test_draw_modifies_buffer_pixels(void)
{
    /* Draw 'H' at baseline (5, 20) on a 30x30 buffer.  Glyph
     * occupies roughly the rect (5..15, 5..20).  At least some
     * pixels in that region should be the draw color, AND no
     * pixels to the LEFT of the glyph should be modified. */
    AxlTtf *ttf = load_test_font();
    AxlGfxBuffer *b = axl_gfx_buffer_new(30, 30);
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int rc = axl_ttf_draw(ttf, 5, 20, "H", 16.0f, white);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "draw: returns AXL_OK on valid input");

    /* Inside glyph bbox: 'H' has two vertical strokes ~9px tall,
     * so dozens of pixels should be filled even before AA edges. */
    size_t inside = count_non_bg_pixels(b, 5, 5, 15, 20, bg);
    test_check(inside > 5,
               "draw: 'H' modifies > 5 pixels inside its bbox");

    /* Left of the glyph (columns 0..4): no pixels modified. */
    size_t outside_left = count_non_bg_pixels(b, 0, 0, 5, 30, bg);
    test_check(outside_left == 0,
               "draw: no pixels modified left of glyph origin");

    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

static void
test_draw_pen_advances_for_multi_char(void)
{
    /* "HH": two glyphs side-by-side.  The pixels modified by 'HH'
     * should outnumber the pixels modified by a single 'H' at the
     * same start position (the second glyph adds coverage to the
     * right of the first). */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };

    AxlGfxBuffer *b1 = axl_gfx_buffer_new(40, 30);
    axl_gfx_buffer_clear(b1, bg);
    axl_gfx_target_buffer(b1);
    axl_ttf_draw(ttf, 5, 20, "H", 16.0f, white);
    axl_gfx_target_buffer(NULL);
    size_t n_single = count_non_bg_pixels(b1, 0, 0, 40, 30, bg);

    AxlGfxBuffer *b2 = axl_gfx_buffer_new(40, 30);
    axl_gfx_buffer_clear(b2, bg);
    axl_gfx_target_buffer(b2);
    axl_ttf_draw(ttf, 5, 20, "HH", 16.0f, white);
    axl_gfx_target_buffer(NULL);
    size_t n_double = count_non_bg_pixels(b2, 0, 0, 40, 30, bg);

    test_check(n_double > n_single,
               "draw: 'HH' modifies more pixels than 'H'");

    axl_gfx_buffer_free(b1);
    axl_gfx_buffer_free(b2);
    axl_ttf_free(ttf);
}

static void
test_draw_outside_clip_no_change(void)
{
    /* Push a clip restricting writes to (0,0,5,5).  Draw a glyph
     * at (50, 50) — fully outside the clip.  No pixels should
     * change. */
    AxlTtf *ttf = load_test_font();
    AxlGfxBuffer *b = axl_gfx_buffer_new(80, 80);
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    AxlGfxClip clip = { 0, 0, 5, 5 };
    axl_gfx_push_clip(clip);
    axl_ttf_draw(ttf, 50, 60, "H", 16.0f, white);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    size_t modified = count_non_bg_pixels(b, 0, 0, 80, 80, bg);
    test_check(modified == 0,
               "draw: glyph outside clip rect produces no pixel changes");

    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

static void
test_draw_color_propagates(void)
{
    /* Glyph drawn with red should produce red pixels (red > 0) and
     * crucially NO green or blue contamination — proves the source
     * color is carried into the output, not a random fill or a
     * channel-swizzle bug. */
    AxlTtf *ttf = load_test_font();
    AxlGfxBuffer *b = axl_gfx_buffer_new(30, 30);
    AxlGfxPixel bg  = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel red = { 0x00, 0x00, 0xFF, 0xFF };  /* B=0 G=0 R=0xFF */
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_ttf_draw(ttf, 5, 20, "H", 16.0f, red);
    axl_gfx_target_buffer(NULL);

    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    size_t total = 30 * 30;
    bool any_red    = false;
    bool any_g_or_b = false;
    for (size_t i = 0; i < total; i++) {
        if (p[i].red > 0) {
            any_red = true;
        }
        if (p[i].green > 0 || p[i].blue > 0) {
            any_g_or_b = true;
        }
    }
    test_check(any_red,
               "draw: glyph pixels carry the source red channel");
    test_check(!any_g_or_b,
               "draw: glyph pixels carry NO green/blue (color preserved)");

    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

static void
test_draw_negative_coords_no_crash(void)
{
    /* Drawing at negative coordinates clips against (0,0); no
     * crash, some pixels in the visible region drawn.  Asserting
     * pixels modified catches "negative coord skipped the whole
     * glyph" regressions (e.g. truncate-toward-zero rounding). */
    AxlTtf *ttf = load_test_font();
    AxlGfxBuffer *b = axl_gfx_buffer_new(30, 30);
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    int rc = axl_ttf_draw(ttf, -3, 20, "H", 16.0f, white);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "draw: negative x baseline returns AXL_OK (no crash)");
    size_t modified = count_non_bg_pixels(b, 0, 0, 30, 30, bg);
    test_check(modified > 0,
               "draw: negative-x glyph still renders visible portion");

    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

static void
test_draw_clip_positive_partial(void)
{
    /* C1 — clip restricting writes to a small rect overlapping the
     * glyph.  Verify pixels INSIDE the clip ARE modified and
     * pixels OUTSIDE the clip are NOT.  Pairs with the
     * outside-clip-no-change test to prove the clip is actually
     * applied per pixel, not just "no draw happened at all". */
    AxlTtf *ttf = load_test_font();
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    /* Glyph rendered at baseline (10, 25) — bbox roughly (10..20, 13..25).
     * Push a clip restricting to (12, 15, 4, 6) — fully inside that bbox. */
    AxlGfxClip clip = { 12, 15, 4, 6 };
    axl_gfx_push_clip(clip);
    axl_ttf_draw(ttf, 10, 25, "H", 16.0f, white);
    axl_gfx_pop_clip();
    axl_gfx_target_buffer(NULL);

    size_t inside  = count_non_bg_pixels(b, 12, 15, 4, 6, bg);
    /* Strip OUTSIDE the clip: count modifications in the LEFT band
     * (columns 0..11) and the TOP band (rows 0..14) which the clip
     * does not cover. */
    size_t left   = count_non_bg_pixels(b, 0, 0, 12, 40, bg);
    size_t top    = count_non_bg_pixels(b, 0, 0, 40, 15, bg);
    test_check(inside > 0,
               "draw: pixels inside clip rect are modified");
    test_check(left == 0,
               "draw: no pixels modified left of clip");
    test_check(top == 0,
               "draw: no pixels modified above clip");

    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

static void
test_draw_alpha_modulates(void)
{
    /* C2 — drawing with alpha=0x80 (half) on a black bg should
     * produce gray pixels (no color shift; R==G==B; mid-range).
     * Catches per-pixel alpha-modulation arithmetic bugs (e.g.,
     * truncation /255 instead of round-nearest). */
    AxlTtf *ttf = load_test_font();
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg         = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel half_white = { 0xFF, 0xFF, 0xFF, 0x80 };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_ttf_draw(ttf, 10, 30, "H", 24.0f, half_white);
    axl_gfx_target_buffer(NULL);

    /* Scan for any pixel that's plausibly-half-bright gray:
     * R == G == B, all in the (90..170) range — wide enough to
     * tolerate AA edges and the exact 0x80 stroke-interior. */
    AxlGfxPixel *p = axl_gfx_buffer_pixels(b);
    size_t total = 40 * 40;
    bool found_half_gray = false;
    for (size_t i = 0; i < total; i++) {
        uint8_t r = p[i].red, g = p[i].green, bl = p[i].blue;
        if (r == g && r == bl && r >= 90 && r <= 170) {
            found_half_gray = true;
            break;
        }
    }
    test_check(found_half_gray,
               "draw: alpha=0x80 produces half-bright gray pixels");

    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

// ---------------------------------------------------------------------------
// Suite entry point
// ---------------------------------------------------------------------------

int
test_truetype_main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;

    test_print_header("AxlTtf G1");

    test_load_null_bytes();
    test_load_zero_len();
    test_load_too_short();
    test_load_zero_buffer();
    test_load_garbage_bytes();

    test_measure_null_ttf();
    test_measure_null_utf8();
    test_measure_zero_px_size();
    test_measure_negative_px_size();

    test_measure_prefix_null_ttf();
    test_measure_prefix_null_utf8();
    test_measure_prefix_zero_bytes();
    test_measure_prefix_zero_px_size();

    test_draw_null_ttf();
    test_draw_null_utf8();
    test_draw_zero_px_size();

    test_metrics_null_ttf();
    test_metrics_zero_px_size();

    test_load_valid_font_returns_non_null();
    test_measure_returns_positive_for_non_empty();
    test_measure_monotonic_in_length();
    test_measure_empty_returns_zero();
    test_measure_scales_with_px_size();
    test_measure_prefix_equals_full_for_strlen();
    test_measure_prefix_zero_returns_zero();
    test_measure_prefix_monotonic();
    test_measure_prefix_mid_codepoint_stops();
    test_measure_invalid_utf8_continues();
    test_metrics_ascent_descent_positive();
    test_metrics_all_null_outputs();
    test_metrics_scales_linearly();

    test_draw_modifies_buffer_pixels();
    test_draw_pen_advances_for_multi_char();
    test_draw_outside_clip_no_change();
    test_draw_clip_positive_partial();
    test_draw_color_propagates();
    test_draw_alpha_modulates();
    test_draw_negative_coords_no_crash();

    return test_print_results();
}

AXL_APP(test_truetype_main)
