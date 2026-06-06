/** @file axl-test-truetype.c
    Unit tests for the truetype module (G1 — vector text via
    stb_truetype).

    Two batches:
      - NULL / invalid-input contract — exercises early-return paths
        without needing a loaded font.
      - Positive cases — exercises measure / measure_prefix / metrics
        against a real loaded font (ASCII subset of DejaVu Sans
        embedded as test/data/test-font-dejavu-ascii.h).

    Glyph rasterization tests (axl_ttf_draw's pixel-producing path)
    and the G7 glyph-cache + subpixel-positioning guards follow the
    measurement batches.
**/

#include "axl-test.h"

#include <axl/axl-truetype.h>
#include <axl/axl-gfx.h>
#include <axl/axl-mem.h>

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
// Glyph cache + subpixel positioning (G7)
// ---------------------------------------------------------------------------
//
// The glyph cache is internal (no public API) — a transparent perf
// layer behind axl_ttf_draw.  It cannot be observed directly, so the
// tests assert the properties it must preserve when active:
//   - repeated identical renders are byte-for-byte identical (a cache
//     hit must return the exact pixels the first miss produced);
//   - distinct sizes don't collapse to one bitmap (the px_size key
//     dimension works);
//   - a glyph re-rendered after the LRU has evicted it reproduces
//     byte-for-byte (eviction frees safely; re-miss re-rasterizes
//     correctly).
// Subpixel positioning (the second G7 half) is exercised by the
// multi-glyph string repeat test: later glyphs land at fractional pen
// offsets, populating multiple subpixel bins, and must still round-
// trip the cache identically.

/* Render @a utf8 once into a fresh @a w x @a h buffer cleared to
 * @a bg, with the draw target set/restored around the call. Caller
 * frees the returned buffer. */
static AxlGfxBuffer *
render_to_buffer(
    AxlTtf      *ttf,
    uint32_t     w,
    uint32_t     h,
    int32_t      x,
    int32_t      y,
    const char  *utf8,
    float        px,
    AxlGfxPixel  color,
    AxlGfxPixel  bg
    )
{
    AxlGfxBuffer *b = axl_gfx_buffer_new(w, h);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_ttf_draw(ttf, x, y, utf8, px, color);
    axl_gfx_target_buffer(NULL);
    return b;
}

/* Byte-exact pixel equality of two same-size buffers (all 4 channels). */
static bool
buffers_pixels_equal(
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
        if (pa[i].blue  != pb[i].blue
            || pa[i].green != pb[i].green
            || pa[i].red   != pb[i].red
            || pa[i].alpha != pb[i].alpha)
        {
            return false;
        }
    }
    return true;
}

static void
test_cache_repeat_glyph_identical(void)
{
    /* Same glyph, same size, drawn twice: the second draw is a cache
     * hit and must reproduce the first draw byte-for-byte. */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };

    AxlGfxBuffer *b1 = render_to_buffer(ttf, 40, 40, 5, 30, "H", 24.0f, white, bg);
    AxlGfxBuffer *b2 = render_to_buffer(ttf, 40, 40, 5, 30, "H", 24.0f, white, bg);

    test_check(buffers_pixels_equal(b1, b2),
               "cache: repeated identical glyph render is byte-identical");

    axl_gfx_buffer_free(b1);
    axl_gfx_buffer_free(b2);
    axl_ttf_free(ttf);
}

static void
test_cache_string_repeat_identical(void)
{
    /* Multi-glyph string: later glyphs land at fractional pen offsets
     * (multiple subpixel bins).  Drawn twice it must be byte-identical
     * — proves every subpixel bin round-trips the cache correctly. */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };

    AxlGfxBuffer *b1 = render_to_buffer(ttf, 200, 40, 5, 30,
                                        "Hello, world!", 18.0f, white, bg);
    AxlGfxBuffer *b2 = render_to_buffer(ttf, 200, 40, 5, 30,
                                        "Hello, world!", 18.0f, white, bg);

    test_check(buffers_pixels_equal(b1, b2),
               "cache: repeated multi-glyph string render is byte-identical");

    axl_gfx_buffer_free(b1);
    axl_gfx_buffer_free(b2);
    axl_ttf_free(ttf);
}

static void
test_cache_distinct_sizes_differ(void)
{
    /* A 48px 'H' must cover more pixels than a 16px 'H'.  If the
     * px_size key dimension collapsed (every size sharing one cached
     * bitmap) the counts would match — this catches that bug. */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };

    AxlGfxBuffer *b16 = render_to_buffer(ttf, 80, 80, 5, 60, "H", 16.0f, white, bg);
    AxlGfxBuffer *b48 = render_to_buffer(ttf, 80, 80, 5, 60, "H", 48.0f, white, bg);

    size_t n16 = count_non_bg_pixels(b16, 0, 0, 80, 80, bg);
    size_t n48 = count_non_bg_pixels(b48, 0, 0, 80, 80, bg);

    test_check(n48 > n16,
               "cache: 48px glyph covers more pixels than 16px (size key distinct)");

    axl_gfx_buffer_free(b16);
    axl_gfx_buffer_free(b48);
    axl_ttf_free(ttf);
}

static void
test_cache_eviction_preserves_correctness(void)
{
    /* Render 'H'@16 (reference).  Flood the cache with far more
     * distinct sizes than its capacity, forcing the LRU to evict the
     * 16px entry.  Re-render 'H'@16 — the re-miss must re-rasterize a
     * byte-identical bitmap.  Catches eviction double-frees, stale
     * slot dims, and "evicted slot still marked valid" bugs. */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };

    AxlGfxBuffer *ref = render_to_buffer(ttf, 40, 40, 5, 30, "H", 16.0f, white, bg);

    /* 400 distinct sizes (step 0.05px > 1/64px quantum, so each maps to
     * a distinct cache key) far exceeds any reasonable cache capacity. */
    for (int i = 0; i < 400; i++) {
        float px = 17.0f + (float)i * 0.05f;
        AxlGfxBuffer *tmp = render_to_buffer(ttf, 60, 60, 5, 45, "H", px, white, bg);
        axl_gfx_buffer_free(tmp);
    }

    AxlGfxBuffer *again = render_to_buffer(ttf, 40, 40, 5, 30, "H", 16.0f, white, bg);

    test_check(buffers_pixels_equal(ref, again),
               "cache: glyph re-rendered after LRU eviction is byte-identical");

    axl_gfx_buffer_free(ref);
    axl_gfx_buffer_free(again);
    axl_ttf_free(ttf);
}

static void
test_cache_oom_fallback_still_draws(void)
{
    /* The lazy cache-struct allocation is the first allocation inside
     * axl_ttf_draw.  Inject an OOM there: the draw must fall back to
     * direct rasterization and still produce glyph pixels (degraded
     * perf, correct output). */
    AxlTtf *ttf = load_test_font();
    AxlGfxBuffer *b = axl_gfx_buffer_new(40, 40);
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    axl_gfx_buffer_clear(b, bg);

    axl_gfx_target_buffer(b);
    axl_mem_fail_next_alloc(1);   /* fail the lazy GlyphCache allocation */
    int rc = axl_ttf_draw(ttf, 5, 30, "H", 24.0f, white);
    axl_gfx_target_buffer(NULL);

    test_check(rc == AXL_OK,
               "cache: draw returns AXL_OK when cache alloc fails (OOM fallback)");
    size_t drawn = count_non_bg_pixels(b, 0, 0, 40, 40, bg);
    test_check(drawn > 0,
               "cache: OOM fallback still rasterizes glyph pixels directly");

    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

// ---------------------------------------------------------------------------
// Boxed / multi-line text + word wrap (G11)
// ---------------------------------------------------------------------------
//
// measure_box runs the identical line-breaking algorithm as draw_box
// without drawing, so the layout-rule assertions (line counts, wrap
// points, over-wide-word handling, geometry) pin behavior via
// measure_box; the draw_box tests then pin the pixel-level concerns
// the geometry can't observe: alignment shift, vertical advance, and
// box clipping.

/* Leftmost column in @a buf (within full bounds) holding a pixel that
 * differs from @a bg.  Returns buf_w (== "none found") if no pixel was
 * modified. */
static uint32_t
leftmost_modified_col(
    AxlGfxBuffer  *buf,
    AxlGfxPixel    bg
    )
{
    uint32_t buf_w = 0, buf_h = 0;
    axl_gfx_buffer_get_info(buf, &buf_w, &buf_h);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(buf);
    for (uint32_t px = 0; px < buf_w; px++) {
        for (uint32_t py = 0; py < buf_h; py++) {
            const AxlGfxPixel *q = &p[py * buf_w + px];
            if (q->blue != bg.blue || q->green != bg.green || q->red != bg.red) {
                return px;
            }
        }
    }
    return buf_w;
}

/* Topmost row holding a modified pixel within the full buffer.
 * Returns buf_h if none. */
static uint32_t
topmost_modified_row(
    AxlGfxBuffer  *buf,
    AxlGfxPixel    bg
    )
{
    uint32_t buf_w = 0, buf_h = 0;
    axl_gfx_buffer_get_info(buf, &buf_w, &buf_h);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(buf);
    for (uint32_t py = 0; py < buf_h; py++) {
        for (uint32_t px = 0; px < buf_w; px++) {
            const AxlGfxPixel *q = &p[py * buf_w + px];
            if (q->blue != bg.blue || q->green != bg.green || q->red != bg.red) {
                return py;
            }
        }
    }
    return buf_h;
}

// --- input validation -----------------------------------------------

static void
test_box_draw_null_ttf(void)
{
    test_check(axl_ttf_draw_box(NULL, 0, 0, 100, 100, "Hi", 16.0f,
                                AXL_GFX_BLACK, AXL_TTF_ALIGN_LEFT) == AXL_ERR,
               "draw_box: NULL ttf returns AXL_ERR");
}

static void
test_box_draw_null_utf8(void)
{
    test_check(axl_ttf_draw_box(FAKE_TTF, 0, 0, 100, 100, NULL, 16.0f,
                                AXL_GFX_BLACK, AXL_TTF_ALIGN_LEFT) == AXL_ERR,
               "draw_box: NULL utf8 returns AXL_ERR");
}

static void
test_box_draw_zero_px_size(void)
{
    test_check(axl_ttf_draw_box(FAKE_TTF, 0, 0, 100, 100, "Hi", 0.0f,
                                AXL_GFX_BLACK, AXL_TTF_ALIGN_LEFT) == AXL_ERR,
               "draw_box: px_size 0 returns AXL_ERR");
}

static void
test_box_measure_null_ttf(void)
{
    uint32_t lines = 999;
    int rc = axl_ttf_measure_box(NULL, 100, "Hi", 16.0f, NULL, NULL, &lines);
    test_check(rc == AXL_ERR,
               "measure_box: NULL ttf returns AXL_ERR");
    test_check(lines == 999,
               "measure_box: outputs untouched on error");
}

static void
test_box_measure_null_utf8(void)
{
    test_check(axl_ttf_measure_box(FAKE_TTF, 100, NULL, 16.0f,
                                   NULL, NULL, NULL) == AXL_ERR,
               "measure_box: NULL utf8 returns AXL_ERR");
}

static void
test_box_measure_zero_px_size(void)
{
    test_check(axl_ttf_measure_box(FAKE_TTF, 100, "Hi", 0.0f,
                                   NULL, NULL, NULL) == AXL_ERR,
               "measure_box: px_size 0 returns AXL_ERR");
}

static void
test_box_measure_null_outputs_ok(void)
{
    /* All output pointers NULL is a valid "is this layout sound" probe. */
    AxlTtf *ttf = load_test_font();
    test_check(axl_ttf_measure_box(ttf, 100, "Hi", 16.0f,
                                   NULL, NULL, NULL) == AXL_OK,
               "measure_box: all-NULL outputs returns AXL_OK");
    axl_ttf_free(ttf);
}

// --- line counting --------------------------------------------------

static void
test_box_measure_empty_is_zero_lines(void)
{
    AxlTtf *ttf = load_test_font();
    uint32_t w = 999, h = 999, lines = 999;
    int rc = axl_ttf_measure_box(ttf, 200, "", 16.0f, &w, &h, &lines);
    test_check(rc == AXL_OK, "measure_box: empty string returns AXL_OK");
    test_check(lines == 0, "measure_box: empty string is 0 lines");
    test_check(w == 0, "measure_box: empty string width is 0");
    test_check(h == 0, "measure_box: empty string height is 0");
    axl_ttf_free(ttf);
}

static void
test_box_measure_single_line_no_wrap(void)
{
    /* A short string in a generous box stays one line, and its width
     * equals the single-line measure of that string. */
    AxlTtf *ttf = load_test_font();
    const char *s = "Hello world";
    uint32_t want_w = axl_ttf_measure(ttf, s, 16.0f);
    uint32_t w = 0, lines = 0;
    axl_ttf_measure_box(ttf, 10000, s, 16.0f, &w, NULL, &lines);
    test_check(lines == 1, "measure_box: fits-in-box string is 1 line");
    test_check(w == want_w, "measure_box: 1-line width equals measure()");
    axl_ttf_free(ttf);
}

static void
test_box_measure_hard_newlines(void)
{
    /* Three \n-separated tokens in a generous box = 3 lines, widest
     * line == widest token. */
    AxlTtf *ttf = load_test_font();
    uint32_t w = 0, lines = 0;
    axl_ttf_measure_box(ttf, 10000, "a\nbb\nccc", 16.0f, &w, NULL, &lines);
    uint32_t wc = axl_ttf_measure(ttf, "ccc", 16.0f);
    test_check(lines == 3, "measure_box: two \\n yields 3 lines");
    test_check(w == wc, "measure_box: widest line == widest \\n token");
    axl_ttf_free(ttf);
}

static void
test_box_measure_trailing_newline(void)
{
    /* Each \n starts a new line, so a trailing \n yields a trailing
     * blank line (documented decision). */
    AxlTtf *ttf = load_test_font();
    uint32_t lines = 0;
    axl_ttf_measure_box(ttf, 10000, "a\n", 16.0f, NULL, NULL, &lines);
    test_check(lines == 2, "measure_box: trailing \\n yields a blank line");
    axl_ttf_free(ttf);
}

static void
test_box_measure_blank_line_preserved(void)
{
    /* Consecutive newlines produce a blank line between paragraphs. */
    AxlTtf *ttf = load_test_font();
    uint32_t lines = 0;
    axl_ttf_measure_box(ttf, 10000, "a\n\nb", 16.0f, NULL, NULL, &lines);
    test_check(lines == 3, "measure_box: \\n\\n preserves a blank line");
    axl_ttf_free(ttf);
}

static void
test_box_measure_all_whitespace_is_one_blank_line(void)
{
    /* An all-whitespace hard line collapses to a single blank line
     * (width 0), distinct from "" which yields 0 lines. Pins the
     * documented empty-vs-blank boundary. */
    AxlTtf *ttf = load_test_font();
    uint32_t w = 999, lines = 999;
    axl_ttf_measure_box(ttf, 200, "    ", 16.0f, &w, NULL, &lines);
    test_check(lines == 1, "measure_box: all-whitespace is one blank line");
    test_check(w == 0, "measure_box: all-whitespace blank line has width 0");
    axl_ttf_free(ttf);
}

// --- wrap point (the <= fit boundary) -------------------------------

static void
test_box_wrap_fits_at_exact_width(void)
{
    /* "aaaa bbbb": with w == measure("aaaa bbbb") both words fit on
     * one line (the fit test is <=). At one pixel narrower the second
     * word wraps to a second line. This pins the boundary exactly. */
    AxlTtf *ttf = load_test_font();
    const char *s = "aaaa bbbb";
    uint32_t exact = axl_ttf_measure(ttf, s, 16.0f);

    uint32_t lines_fit = 0, lines_wrap = 0;
    axl_ttf_measure_box(ttf, exact, s, 16.0f, NULL, NULL, &lines_fit);
    axl_ttf_measure_box(ttf, exact - 1, s, 16.0f, NULL, NULL, &lines_wrap);

    test_check(lines_fit == 1,
               "measure_box: width == measure() keeps both words on one line");
    test_check(lines_wrap == 2,
               "measure_box: one pixel narrower wraps the second word");
    axl_ttf_free(ttf);
}

static void
test_box_wrap_three_words(void)
{
    /* "aaaa bbbb cccc" with w == measure("aaaa bbbb") wraps after the
     * second word: lines 0 = "aaaa bbbb", line 1 = "cccc". The widest
     * rendered line is "aaaa bbbb", so out_width == w. */
    AxlTtf *ttf = load_test_font();
    uint32_t two = axl_ttf_measure(ttf, "aaaa bbbb", 16.0f);
    uint32_t w = 0, lines = 0;
    axl_ttf_measure_box(ttf, two, "aaaa bbbb cccc", 16.0f, &w, NULL, &lines);
    test_check(lines == 2, "measure_box: 3 words wrap into 2 lines at the fit width");
    test_check(w == two, "measure_box: widest line is the filled first line");
    axl_ttf_free(ttf);
}

// --- over-wide word -------------------------------------------------

static void
test_box_overwide_word_overflows(void)
{
    /* A single word wider than the box is not split: it occupies one
     * line and reports its true (over-width) width. */
    AxlTtf *ttf = load_test_font();
    const char *word = "WWWWWWWWWW";
    uint32_t full = axl_ttf_measure(ttf, word, 16.0f);
    uint32_t w = 0, lines = 0;
    axl_ttf_measure_box(ttf, 1, word, 16.0f, &w, NULL, &lines);
    test_check(lines == 1, "measure_box: over-wide word stays one line (not split)");
    test_check(w == full, "measure_box: over-wide word reports its true width > box");
    test_check(w > 1, "measure_box: reported width exceeds the box width");
    axl_ttf_free(ttf);
}

static void
test_box_overwide_word_wraps_to_own_line(void)
{
    /* "aa WWWWWWWWWW": with a box just wide enough for "aa", the long
     * second word cannot fit after "aa" so it wraps to its own line
     * (and overflows there). 2 lines; widest == the long word. */
    AxlTtf *ttf = load_test_font();
    uint32_t w_aa  = axl_ttf_measure(ttf, "aa", 16.0f);
    uint32_t w_big = axl_ttf_measure(ttf, "WWWWWWWWWW", 16.0f);
    uint32_t w = 0, lines = 0;
    axl_ttf_measure_box(ttf, w_aa, "aa WWWWWWWWWW", 16.0f, &w, NULL, &lines);
    test_check(lines == 2, "measure_box: long word wraps onto its own line");
    test_check(w == w_big, "measure_box: widest line is the overflowing long word");
    axl_ttf_free(ttf);
}

// --- geometry (height) ----------------------------------------------

static void
test_box_measure_height_matches_metrics(void)
{
    /* out_height == ceil(line_count * line_height) where line_height ==
     * ascent + descent + line_gap. Use a 3-line block. */
    AxlTtf *ttf = load_test_font();
    float ascent = 0, descent = 0, line_gap = 0;
    axl_ttf_metrics(ttf, 16.0f, &ascent, &descent, &line_gap);
    float line_h = ascent + descent + line_gap;
    uint32_t expect = (uint32_t)axl_ceili((double)(3.0f * line_h));

    uint32_t h = 0, lines = 0;
    axl_ttf_measure_box(ttf, 10000, "a\nb\nc", 16.0f, NULL, &h, &lines);
    test_check(lines == 3, "measure_box: height test has 3 lines");
    test_check(h == expect, "measure_box: height == ceil(lines * line_height)");
    axl_ttf_free(ttf);
}

// --- draw: alignment shift ------------------------------------------

static void
test_box_draw_alignment_shifts(void)
{
    /* Same single short line in a wide box, drawn LEFT / CENTER /
     * RIGHT. The leftmost modified column must increase strictly
     * across the three: left-aligned hugs the left edge, right-aligned
     * is pushed toward the right. */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    const char *s = "Hi";
    const uint32_t BW = 200, BH = 40;

    AxlGfxBuffer *bl = axl_gfx_buffer_new(BW, BH);
    axl_gfx_buffer_clear(bl, bg);
    axl_gfx_target_buffer(bl);
    axl_ttf_draw_box(ttf, 0, 0, BW, BH, s, 18.0f, white, AXL_TTF_ALIGN_LEFT);
    axl_gfx_target_buffer(NULL);

    AxlGfxBuffer *bc = axl_gfx_buffer_new(BW, BH);
    axl_gfx_buffer_clear(bc, bg);
    axl_gfx_target_buffer(bc);
    axl_ttf_draw_box(ttf, 0, 0, BW, BH, s, 18.0f, white, AXL_TTF_ALIGN_CENTER);
    axl_gfx_target_buffer(NULL);

    AxlGfxBuffer *br = axl_gfx_buffer_new(BW, BH);
    axl_gfx_buffer_clear(br, bg);
    axl_gfx_target_buffer(br);
    axl_ttf_draw_box(ttf, 0, 0, BW, BH, s, 18.0f, white, AXL_TTF_ALIGN_RIGHT);
    axl_gfx_target_buffer(NULL);

    uint32_t left_l = leftmost_modified_col(bl, bg);
    uint32_t left_c = leftmost_modified_col(bc, bg);
    uint32_t left_r = leftmost_modified_col(br, bg);

    test_check(left_l < left_c, "draw_box: center shifts text right of left-align");
    test_check(left_c < left_r, "draw_box: right shifts text right of center-align");

    /* Pin the right-aligned start: its leftmost pixel sits near
     * box_right - line_width (within an AA pixel of the expected
     * origin), proving the shift magnitude, not just ordering. */
    uint32_t lw = axl_ttf_measure(ttf, s, 18.0f);
    uint32_t expect_r = BW - lw;
    /* Two-sided bound: the right-aligned line's leftmost pixel sits
     * within an AA pixel of box_right - line_width, pinning the shift
     * magnitude (not just the ordering) from both directions. The
     * first glyph's left side-bearing means the leftmost lit pixel is
     * at or just right of the pen origin, never left of it. */
    test_check(left_r + 2 >= expect_r && left_r <= expect_r + 4,
               "draw_box: right-aligned text starts at box_right - width");

    axl_gfx_buffer_free(bl);
    axl_gfx_buffer_free(bc);
    axl_gfx_buffer_free(br);
    axl_ttf_free(ttf);
}

// --- draw: vertical advance + clip ----------------------------------

static void
test_box_draw_two_lines_advance(void)
{
    /* "a\nb" draws two glyphs in vertically separated bands. The
     * second line's topmost pixel is roughly one line-height below the
     * first line's. */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    float ascent = 0, descent = 0, line_gap = 0;
    axl_ttf_metrics(ttf, 20.0f, &ascent, &descent, &line_gap);
    float line_h = ascent + descent + line_gap;

    AxlGfxBuffer *one = axl_gfx_buffer_new(60, 80);
    axl_gfx_buffer_clear(one, bg);
    axl_gfx_target_buffer(one);
    axl_ttf_draw_box(ttf, 0, 0, 60, 80, "a", 20.0f, white, AXL_TTF_ALIGN_LEFT);
    axl_gfx_target_buffer(NULL);
    uint32_t top_one = topmost_modified_row(one, bg);

    AxlGfxBuffer *two = axl_gfx_buffer_new(60, 80);
    axl_gfx_buffer_clear(two, bg);
    axl_gfx_target_buffer(two);
    axl_ttf_draw_box(ttf, 0, 0, 60, 80, "\nb", 20.0f, white, AXL_TTF_ALIGN_LEFT);
    axl_gfx_target_buffer(NULL);
    uint32_t top_two = topmost_modified_row(two, bg);

    /* "\nb" puts 'b' on the second line; its top is ~line_h below the
     * first line's 'a'. Allow generous tolerance for glyph bearings. */
    test_check(top_two > top_one + (uint32_t)(line_h / 2.0f),
               "draw_box: a line below advances by ~line-height");
    axl_gfx_buffer_free(one);
    axl_gfx_buffer_free(two);
    axl_ttf_free(ttf);
}

static void
test_box_draw_vertical_clip(void)
{
    /* A box only tall enough for one line, given three lines of text:
     * nothing is drawn below the box bottom (clip honored). */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    float ascent = 0, descent = 0, line_gap = 0;
    axl_ttf_metrics(ttf, 16.0f, &ascent, &descent, &line_gap);
    uint32_t box_h = (uint32_t)axl_ceili((double)(ascent + descent));

    AxlGfxBuffer *b = axl_gfx_buffer_new(60, 120);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    axl_ttf_draw_box(ttf, 0, 0, 60, box_h, "a\nb\nc", 16.0f, white,
                     AXL_TTF_ALIGN_LEFT);
    axl_gfx_target_buffer(NULL);

    /* No pixels at or below the box bottom edge. */
    size_t below = count_non_bg_pixels(b, 0, box_h, 60, 120 - box_h, bg);
    test_check(below == 0,
               "draw_box: clips text below the box height");
    /* First line still drew something inside the box. */
    size_t inside = count_non_bg_pixels(b, 0, 0, 60, box_h, bg);
    test_check(inside > 0,
               "draw_box: first line renders inside the box");
    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

static void
test_box_draw_returns_ok(void)
{
    /* A normal wrapped draw onto a buffer target returns AXL_OK. */
    AxlTtf *ttf = load_test_font();
    AxlGfxPixel bg    = { 0x00, 0x00, 0x00, 0xFF };
    AxlGfxPixel white = { 0xFF, 0xFF, 0xFF, 0xFF };
    AxlGfxBuffer *b = axl_gfx_buffer_new(120, 80);
    axl_gfx_buffer_clear(b, bg);
    axl_gfx_target_buffer(b);
    int rc = axl_ttf_draw_box(ttf, 5, 5, 100, 70,
                              "The quick brown fox jumps", 16.0f, white,
                              AXL_TTF_ALIGN_LEFT);
    axl_gfx_target_buffer(NULL);
    test_check(rc == AXL_OK, "draw_box: normal wrapped draw returns AXL_OK");
    size_t drawn = count_non_bg_pixels(b, 0, 0, 120, 80, bg);
    test_check(drawn > 0, "draw_box: wrapped paragraph renders pixels");
    axl_gfx_buffer_free(b);
    axl_ttf_free(ttf);
}

// ---------------------------------------------------------------------------
// Built-in default font (axl_ttf_default)
// ---------------------------------------------------------------------------

/* UTF-8 literals used as coverage probes. U+4E00 (CJK 一) is the
 * absent-codepoint baseline: it is NOT in the DejaVu subset, so it
 * measures the font's .notdef advance. Coverage is proven by a
 * codepoint's advance differing from this baseline in the direction
 * its real glyph dictates (gaps chosen large enough that pixel
 * rounding cannot flip the inequality). */
#define U_EACUTE   "\xC3\xA9"      /* é  U+00E9 — same advance as 'e' */
#define U_COPY     "\xC2\xA9"      /* ©  U+00A9 — Latin-1, wide (>notdef) */
#define U_DEGREE   "\xC2\xB0"      /* °  U+00B0 — Latin-1, narrow (<notdef) */
#define U_EMDASH   "\xE2\x80\x94"  /* —  U+2014 — full em, very wide */
#define U_ENDASH   "\xE2\x80\x93"  /* –  U+2013 — half em, narrow (<notdef) */
#define U_ELLIPSIS "\xE2\x80\xA6"  /* …  U+2026 — wide */
#define U_LQUOTE   "\xE2\x80\x98"  /* '  U+2018 — left single quote, narrow */
#define U_ABSENT   "\xE4\xB8\x80"  /* 一 U+4E00 — absent → .notdef baseline */

static void
test_default_returns_non_null(void)
{
    test_check(axl_ttf_default() != NULL,
               "default: axl_ttf_default() returns non-NULL");
}

static void
test_default_is_singleton(void)
{
    /* Shared handle — repeated calls return the SAME pointer, never
     * re-parse. Caller must not free it. */
    test_check(axl_ttf_default() == axl_ttf_default(),
               "default: repeated calls return the same handle");
}

static void
test_default_ascii_renders(void)
{
    AxlTtf *f = axl_ttf_default();
    test_check(axl_ttf_measure(f, "Hello", 16.0f) > 0,
               "default: ASCII 'Hello' measures > 0");
}

static void
test_default_latin1_eacute_maps_real_glyph(void)
{
    /* é (U+00E9) shares 'e' advance in DejaVu — proves it maps to the
     * eacute glyph, not a fallback that would advance differently. */
    AxlTtf *f = axl_ttf_default();
    uint32_t w_e  = axl_ttf_measure(f, "e",      16.0f);
    uint32_t w_ea = axl_ttf_measure(f, U_EACUTE, 16.0f);
    test_check(w_ea > 0 && w_ea == w_e,
               "default: é advance equals 'e' (Latin-1 glyph present)");
}

static void
test_default_latin1_present_vs_notdef(void)
{
    /* © is much wider than .notdef; ° is narrower. Both differ from
     * the absent-baseline, so neither is falling back to .notdef. */
    AxlTtf *f = axl_ttf_default();
    uint32_t w_absent = axl_ttf_measure(f, U_ABSENT, 16.0f);
    uint32_t w_copy   = axl_ttf_measure(f, U_COPY,   16.0f);
    uint32_t w_degree = axl_ttf_measure(f, U_DEGREE, 16.0f);
    test_check(w_copy > w_absent,
               "default: © wider than .notdef (Latin-1 present)");
    test_check(w_degree < w_absent,
               "default: ° narrower than .notdef (Latin-1 present)");
}

static void
test_default_punct_emdash_wide(void)
{
    /* Em-dash spans a full em — far wider than an ASCII hyphen. */
    AxlTtf *f = axl_ttf_default();
    uint32_t w_hyphen = axl_ttf_measure(f, "-",      16.0f);
    uint32_t w_emdash = axl_ttf_measure(f, U_EMDASH, 16.0f);
    test_check(w_emdash > w_hyphen,
               "default: em-dash wider than hyphen (punctuation present)");
}

static void
test_default_punct_endash_present(void)
{
    /* En-dash advance is below .notdef — an absent en-dash would
     * instead measure the (wider) .notdef baseline. */
    AxlTtf *f = axl_ttf_default();
    uint32_t w_absent = axl_ttf_measure(f, U_ABSENT, 16.0f);
    uint32_t w_endash = axl_ttf_measure(f, U_ENDASH, 16.0f);
    test_check(w_endash < w_absent,
               "default: en-dash narrower than .notdef (punctuation present)");
}

static void
test_default_punct_curly_quote_present(void)
{
    /* Curly quotes are far narrower than .notdef — an absent quote
     * would instead measure the (wider) .notdef baseline. */
    AxlTtf *f = axl_ttf_default();
    uint32_t w_absent = axl_ttf_measure(f, U_ABSENT, 16.0f);
    uint32_t w_quote  = axl_ttf_measure(f, U_LQUOTE, 16.0f);
    test_check(w_quote < w_absent,
               "default: curly quote narrower than .notdef (punctuation present)");
}

static void
test_default_punct_ellipsis_wide(void)
{
    /* '…' is a single wide glyph — wider than one '.'. */
    AxlTtf *f = axl_ttf_default();
    uint32_t w_dot      = axl_ttf_measure(f, ".",        16.0f);
    uint32_t w_ellipsis = axl_ttf_measure(f, U_ELLIPSIS, 16.0f);
    test_check(w_ellipsis > w_dot,
               "default: ellipsis wider than '.' (punctuation present)");
}

// ---------------------------------------------------------------------------
// axl_ttf_mono_default — the built-in fixed-width face
// ---------------------------------------------------------------------------

static void
test_mono_default_returns_non_null(void)
{
    test_check(axl_ttf_mono_default() != NULL,
               "mono: axl_ttf_mono_default() returns non-NULL");
}

static void
test_mono_default_is_singleton(void)
{
    test_check(axl_ttf_mono_default() == axl_ttf_mono_default(),
               "mono: repeated calls return the same handle");
}

static void
test_mono_default_distinct_from_default(void)
{
    /* A different face entirely from the proportional default. */
    test_check(axl_ttf_mono_default() != axl_ttf_default(),
               "mono: distinct handle from axl_ttf_default()");
}

static void
test_mono_default_is_fixed_width(void)
{
    /* The defining property: every glyph advances by the same width, so a
     * narrow 'i' and a wide 'M' measure identically — which the proportional
     * default does NOT (proven by the contrast assertion). */
    AxlTtf  *mono = axl_ttf_mono_default();
    AxlTtf  *prop = axl_ttf_default();
    uint32_t mi = axl_ttf_measure(mono, "i", 16.0f);
    uint32_t mm = axl_ttf_measure(mono, "M", 16.0f);
    uint32_t pi = axl_ttf_measure(prop, "i", 16.0f);
    uint32_t pm = axl_ttf_measure(prop, "M", 16.0f);
    test_check(mi > 0 && mi == mm,
               "mono: 'i' and 'M' have equal advance (fixed-width)");
    test_check(pi != pm,
               "mono: proportional default has unequal 'i'/'M' (contrast)");
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

    test_print_header("AxlTtf G1+G7");

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

    test_cache_repeat_glyph_identical();
    test_cache_string_repeat_identical();
    test_cache_distinct_sizes_differ();
    test_cache_eviction_preserves_correctness();
    test_cache_oom_fallback_still_draws();

    test_box_draw_null_ttf();
    test_box_draw_null_utf8();
    test_box_draw_zero_px_size();
    test_box_measure_null_ttf();
    test_box_measure_null_utf8();
    test_box_measure_zero_px_size();
    test_box_measure_null_outputs_ok();
    test_box_measure_empty_is_zero_lines();
    test_box_measure_single_line_no_wrap();
    test_box_measure_hard_newlines();
    test_box_measure_trailing_newline();
    test_box_measure_blank_line_preserved();
    test_box_measure_all_whitespace_is_one_blank_line();
    test_box_wrap_fits_at_exact_width();
    test_box_wrap_three_words();
    test_box_overwide_word_overflows();
    test_box_overwide_word_wraps_to_own_line();
    test_box_measure_height_matches_metrics();
    test_box_draw_alignment_shifts();
    test_box_draw_two_lines_advance();
    test_box_draw_vertical_clip();
    test_box_draw_returns_ok();

    test_default_returns_non_null();
    test_default_is_singleton();
    test_default_ascii_renders();
    test_default_latin1_eacute_maps_real_glyph();
    test_default_latin1_present_vs_notdef();
    test_default_punct_emdash_wide();
    test_default_punct_endash_present();
    test_default_punct_curly_quote_present();
    test_default_punct_ellipsis_wide();

    test_mono_default_returns_non_null();
    test_mono_default_is_singleton();
    test_mono_default_distinct_from_default();
    test_mono_default_is_fixed_width();

    return test_print_results();
}

AXL_APP(test_truetype_main)
