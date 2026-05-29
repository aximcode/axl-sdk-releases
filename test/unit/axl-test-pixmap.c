/** @file axl-test-pixmap.c
    Unit tests for the pixmap (image-decode) module (G2 — PNG/JPEG/
    GIF/BMP via stb_image).

    NULL/invalid-input contract tests AND positive-case tests
    against a 4x3 RGB PNG fixture with a known color grid
    (test/data/test-image-4x3-png.h).
**/

#include "axl-test.h"

#include <axl/axl-pixmap.h>
#include <axl/axl-gfx.h>

#include "test-image-4x3-png.h"

/* Small invalid byte buffers for axl_pixmap_info / decode failure paths. */
static const uint8_t too_short[4] = { 0x89, 0x50, 0x4e, 0x47 };
static const uint8_t garbage[32]  = { 'n','o','t','_','a','n','_','i',
                                      'm','a','g','e','!','!','!','!',
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// ---------------------------------------------------------------------------
// axl_pixmap_info — input validation
// ---------------------------------------------------------------------------

static void
test_info_null_bytes(void)
{
    uint32_t w = 0, h = 0;
    test_check(axl_pixmap_info(NULL, 100, &w, &h) == AXL_ERR,
               "info: NULL bytes returns AXL_ERR");
}

static void
test_info_zero_len(void)
{
    uint32_t w = 0, h = 0;
    test_check(axl_pixmap_info((const uint8_t *)"x", 0, &w, &h) == AXL_ERR,
               "info: zero len returns AXL_ERR");
}

static void
test_info_too_short(void)
{
    uint32_t w = 0, h = 0;
    test_check(axl_pixmap_info(too_short, sizeof too_short, &w, &h) == AXL_ERR,
               "info: too-short buffer returns AXL_ERR");
}

static void
test_info_garbage_bytes(void)
{
    uint32_t w = 0, h = 0;
    test_check(axl_pixmap_info(garbage, sizeof garbage, &w, &h) == AXL_ERR,
               "info: garbage bytes returns AXL_ERR");
}

static void
test_info_jpeg_too_short(void)
{
    /* JPEG signature is FF D8 FF — 3 bytes minimum. A 1-byte 0xFF
     * fragment must reject without reading bytes[1] or bytes[2]. */
    static const uint8_t one_ff[1] = { 0xFF };
    uint32_t w = 0, h = 0;
    test_check(axl_pixmap_info(one_ff, sizeof one_ff, &w, &h) == AXL_ERR,
               "info: 1-byte JPEG-shaped fragment returns AXL_ERR");
}

static void
test_info_bmp_signature_only(void)
{
    /* BMP signature is "BM" — passes pre-validation but has no
     * header body so stb_image must reject the malformed input. */
    static const uint8_t bm_only[2] = { 'B', 'M' };
    uint32_t w = 0, h = 0;
    test_check(axl_pixmap_info(bm_only, sizeof bm_only, &w, &h) == AXL_ERR,
               "info: signature-only BMP returns AXL_ERR (stb rejects)");
}

// ---------------------------------------------------------------------------
// axl_pixmap_decode — input validation
// ---------------------------------------------------------------------------

static void
test_decode_null_bytes(void)
{
    test_check(axl_pixmap_decode(NULL, 100) == NULL,
               "decode: NULL bytes returns NULL");
}

static void
test_decode_zero_len(void)
{
    test_check(axl_pixmap_decode((const uint8_t *)"x", 0) == NULL,
               "decode: zero len returns NULL");
}

static void
test_decode_garbage_bytes(void)
{
    test_check(axl_pixmap_decode(garbage, sizeof garbage) == NULL,
               "decode: garbage bytes returns NULL");
}

// ---------------------------------------------------------------------------
// Positive cases — 4x3 RGB PNG fixture
// ---------------------------------------------------------------------------

static void
test_info_valid_png_returns_dimensions(void)
{
    uint32_t w = 0, h = 0;
    int rc = axl_pixmap_info(test_image_4x3_png,
                             (size_t)test_image_4x3_png_len,
                             &w, &h);
    test_check(rc == AXL_OK,
               "info: valid 4x3 PNG returns AXL_OK");
    test_check(w == 4,
               "info: width == 4");
    test_check(h == 3,
               "info: height == 3");
}

static void
test_info_all_null_outputs_ok(void)
{
    /* Contract: output pointers may be NULL to skip.  All-NULL
     * should still return AXL_OK on a valid input — caller may be
     * probing format support without reading dimensions. */
    int rc = axl_pixmap_info(test_image_4x3_png,
                             (size_t)test_image_4x3_png_len,
                             NULL, NULL);
    test_check(rc == AXL_OK,
               "info: all-NULL outputs returns AXL_OK on valid PNG");
}

static void
test_info_only_w_output(void)
{
    /* Contract: each output may be NULL independently.  Pass only w,
     * h NULL — verify w gets written, behavior is sane. */
    uint32_t w = 0;
    int rc = axl_pixmap_info(test_image_4x3_png,
                             (size_t)test_image_4x3_png_len,
                             &w, NULL);
    test_check(rc == AXL_OK,
               "info: out_h=NULL returns AXL_OK");
    test_check(w == 4,
               "info: out_h=NULL still writes w == 4");
}

static void
test_info_only_h_output(void)
{
    uint32_t h = 0;
    int rc = axl_pixmap_info(test_image_4x3_png,
                             (size_t)test_image_4x3_png_len,
                             NULL, &h);
    test_check(rc == AXL_OK,
               "info: out_w=NULL returns AXL_OK");
    test_check(h == 3,
               "info: out_w=NULL still writes h == 3");
}

static void
test_decode_valid_png_returns_buffer(void)
{
    AxlGfxBuffer *buf = axl_pixmap_decode(test_image_4x3_png,
                                          (size_t)test_image_4x3_png_len);
    test_check(buf != NULL,
               "decode: valid 4x3 PNG returns non-NULL buffer");
    axl_gfx_buffer_free(buf);
}

static void
test_decode_buffer_has_correct_dimensions(void)
{
    AxlGfxBuffer *buf = axl_pixmap_decode(test_image_4x3_png,
                                          (size_t)test_image_4x3_png_len);
    uint32_t w = 0, h = 0;
    int rc = axl_gfx_buffer_get_info(buf, &w, &h);
    test_check(rc == AXL_OK,
               "decode: buffer_get_info succeeds on decoded buffer");
    test_check(w == 4,
               "decode: buffer width == 4");
    test_check(h == 3,
               "decode: buffer height == 3");
    axl_gfx_buffer_free(buf);
}

static void
test_decode_buffer_pixel_top_left_is_red(void)
{
    /* Fixture row 0 col 0 is red(255, 0, 0).  AxlGfxPixel is BGRA
     * so blue=0 green=0 red=255 alpha=255 (PNG has no alpha; the
     * decoder fills alpha=0xFF). */
    AxlGfxBuffer *buf = axl_pixmap_decode(test_image_4x3_png,
                                          (size_t)test_image_4x3_png_len);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(buf);
    test_check(p != NULL,
               "decode: buffer_pixels returns non-NULL");
    if (p) {
        test_check(p[0].red   == 0xFF, "decode: (0,0) red == 0xFF");
        test_check(p[0].green == 0x00, "decode: (0,0) green == 0x00");
        test_check(p[0].blue  == 0x00, "decode: (0,0) blue == 0x00");
        test_check(p[0].alpha == 0xFF, "decode: (0,0) alpha == 0xFF (filled)");
    }
    axl_gfx_buffer_free(buf);
}

static void
test_decode_buffer_pixel_bottom_right_is_brown(void)
{
    /* Fixture row 2 col 3 is brown(165, 42, 42). */
    AxlGfxBuffer *buf = axl_pixmap_decode(test_image_4x3_png,
                                          (size_t)test_image_4x3_png_len);
    AxlGfxPixel *p = axl_gfx_buffer_pixels(buf);
    /* Stride = 4, row-major.  Pixel (3, 2) is at index 2*4 + 3 = 11. */
    if (p) {
        test_check(p[11].red   == 165,  "decode: (3,2) red == 165");
        test_check(p[11].green == 42,   "decode: (3,2) green == 42");
        test_check(p[11].blue  == 42,   "decode: (3,2) blue == 42");
    }
    axl_gfx_buffer_free(buf);
}

// ---------------------------------------------------------------------------
// Suite entry point
// ---------------------------------------------------------------------------

int
test_pixmap_main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;

    test_print_header("AxlPixmap G2");

    test_info_null_bytes();
    test_info_zero_len();
    test_info_too_short();
    test_info_garbage_bytes();
    test_info_jpeg_too_short();
    test_info_bmp_signature_only();

    test_decode_null_bytes();
    test_decode_zero_len();
    test_decode_garbage_bytes();

    test_info_valid_png_returns_dimensions();
    test_info_all_null_outputs_ok();
    test_info_only_w_output();
    test_info_only_h_output();

    test_decode_valid_png_returns_buffer();
    test_decode_buffer_has_correct_dimensions();
    test_decode_buffer_pixel_top_left_is_red();
    test_decode_buffer_pixel_bottom_right_is_brown();

    return test_print_results();
}

AXL_APP(test_pixmap_main)
