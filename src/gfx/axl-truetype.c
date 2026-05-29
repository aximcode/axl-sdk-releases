/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-truetype.c
    AxlTtf — vector text rendering backed by stb_truetype.

    Public API (see <axl/axl-truetype.h>) wraps stb_truetype's
    `stbtt_fontinfo` plus axl-sdk-side input validation, UTF-8
    decoding (via axl_utf8_decode), and integration with the axl-
    gfx draw target / clip stack.

    No libm linkage: stb_truetype's STBTT_* math hooks are routed
    to compiler builtins (floor/ceil/fabs/sqrt — single hardware
    instructions on x64-SSE2 and AArch64) and small local helpers
    (fmod).  pow/cos/acos are stubbed since they're only exercised
    by SDF + gamma paths the public API doesn't expose; if a
    consumer needs those a real implementation is straightforward.

    No libc linkage either: stb's STBTT_malloc/free/memcpy/memset/
    strlen are routed to axl-sdk equivalents per the dogfood rule
    (see feedback_dogfood).
**/

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-gfx.h>
#include <axl/axl-macros.h>
#include <axl/axl-math.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-truetype.h>

// ===================================================================
// stb_truetype math + memory shims
// ===================================================================
//
// Routes stb_truetype's STBTT_* math macros to axl-math's libm-free
// implementations.  pow / acos are kept as zero-returning stubs since
// axl-math doesn't provide them — they're only reached from
// stb_truetype's gamma + cubic-bezier-fallback paths, neither of
// which the public AxlTtf API exposes (verified by call-site trace
// at G1 code review).  If a consumer ever hits them the symptom is
// visibly broken glyphs; add real implementations to axl-math then.

static inline double
axl_ttf_stub_unary_(double x)
{
    (void)x;
    return 0.0;
}

static inline double
axl_ttf_stub_binary_(double x, double y)
{
    (void)x;
    (void)y;
    return 0.0;
}

#define STBTT_ifloor(x)        axl_floori(x)
#define STBTT_iceil(x)         axl_ceili(x)
#define STBTT_fabs(x)          axl_fabs(x)
#define STBTT_sqrt(x)          axl_sqrt(x)
#define STBTT_fmod(x, y)       axl_fmod((x), (y))
#define STBTT_pow(x, y)        axl_ttf_stub_binary_((x), (y))
#define STBTT_cos(x)           axl_cos(x)
#define STBTT_acos(x)          axl_ttf_stub_unary_(x)

#define STBTT_malloc(sz, ud)   ((void)(ud), axl_malloc((size_t)(sz)))
#define STBTT_free(p, ud)      ((void)(ud), axl_free(p))
#define STBTT_assert(x)        ((void)(x))

#define STBTT_strlen(s)        axl_strlen(s)
#define STBTT_memcpy           axl_memcpy
#define STBTT_memset           axl_memset

#define STBTT_STATIC

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../deps/stb/stb_truetype.h"

// ===================================================================
// AxlTtf opaque type
// ===================================================================

struct AxlTtf {
    stbtt_fontinfo  info;
};

// ===================================================================
// Public API — load / free
// ===================================================================

/* Pre-validate that @a bytes is a plausible sfnt buffer before
 * handing it to stb_truetype.  stb explicitly documents "no
 * range checking of the offsets found in the file" — passing
 * untrusted data straight to stbtt_InitFont produces wild OOB
 * reads on garbage inputs.  We catch the obvious shapes here:
 *
 *   1. Length must hold the sfnt header (12 bytes).
 *   2. Version magic must be one of the recognized sfnt formats.
 *   3. numTables (uint16 at offset 4 of the font header) must
 *      describe a table directory that fits in the buffer.
 *
 * This is not full validation — a syntactically valid header
 * with broken table offsets can still trip stb's table-content
 * reads — but it eliminates the catastrophic "trust the
 * numTables byte" failure mode that crashes on AArch64 (and
 * gets lucky on x64 because the OOB read happened to land in
 * mapped memory). */
static bool
sfnt_header_plausible(
    const uint8_t  *bytes,
    size_t          len,
    size_t          font_offset
    )
{
    if (font_offset > len || len - font_offset < 12) {
        return false;
    }
    const uint8_t *p = bytes + font_offset;

    uint32_t version = ((uint32_t)p[0] << 24)
                     | ((uint32_t)p[1] << 16)
                     | ((uint32_t)p[2] << 8)
                     |  (uint32_t)p[3];
    /* Recognized sfnt versions: TrueType 1.0, OpenType CFF,
     * Apple "true", Apple "typ1".  TTC headers are dispatched
     * separately before this check runs (we land here with the
     * inner-font offset). */
    if (version != 0x00010000u
        && version != 0x4F54544Fu   /* OTTO */
        && version != 0x74727565u   /* true */
        && version != 0x74797031u)  /* typ1 */
    {
        return false;
    }

    uint16_t num_tables = ((uint16_t)p[4] << 8) | (uint16_t)p[5];
    /* Each table directory entry is 16 bytes; the 4 ushort
     * header fields (numTables, searchRange, entrySelector,
     * rangeShift) occupy bytes 4..11.  Directory starts at
     * offset 12 from the font header. */
    size_t dir_end = (size_t)12 + (size_t)num_tables * 16u;
    if (dir_end > len - font_offset) {
        return false;
    }

    return true;
}

AxlTtf *
axl_ttf_load(
    const uint8_t  *bytes,
    size_t          len
    )
{
    /* sfnt header is 12 bytes minimum (4-byte version + 4 ushorts).
     * Anything shorter cannot be a valid TTF/OTF/TTC. */
    if (!bytes || len < 12) {
        return NULL;
    }

    /* TTC: pick first font.  Single-font files: returns 0 for
     * valid sfnt headers; negative for invalid TTC. */
    int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
    if (offset < 0) {
        return NULL;
    }

    if (!sfnt_header_plausible(bytes, len, (size_t)offset)) {
        return NULL;
    }

    AxlTtf *ttf = axl_malloc(sizeof *ttf);
    if (!ttf) {
        return NULL;
    }

    if (!stbtt_InitFont(&ttf->info, bytes, offset)) {
        axl_free(ttf);
        return NULL;
    }

    return ttf;
}

void
axl_ttf_free(
    AxlTtf  *ttf
    )
{
    if (!ttf) {
        return;
    }
    axl_free(ttf);
}

// ===================================================================
// Public API — measurement
// ===================================================================

/* Walk @a utf8 codepoint-by-codepoint up to @a max_bytes, accumulate
 * scaled advances + kerning.  Returns the rendered prefix width in
 * pixels (rounded up). */
static uint32_t
measure_utf8_prefix(
    const stbtt_fontinfo  *info,
    const char            *utf8,
    size_t                 max_bytes,
    float                  scale
    )
{
    float    width   = 0.0f;
    int      prev_cp = -1;
    size_t   pos     = 0;

    while (pos < max_bytes && utf8[pos] != '\0') {
        uint32_t cp;
        size_t n = axl_utf8_decode(utf8 + pos, &cp);
        if (n == 0) {
            break;
        }
        /* Refuse to count a codepoint whose bytes extend past
         * max_bytes — caller asked for an exact byte prefix. */
        if (pos + n > max_bytes) {
            break;
        }

        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, (int)cp, &advance, &lsb);
        width += (float)advance * scale;

        if (prev_cp >= 0) {
            int kern = stbtt_GetCodepointKernAdvance(info, prev_cp, (int)cp);
            width += (float)kern * scale;
        }

        prev_cp = (int)cp;
        pos    += n;
    }

    /* Width is a non-negative pixel count; ceil to whole pixels. */
    int ceil_w = axl_ceili((double)width);
    return ceil_w < 0 ? 0u : (uint32_t)ceil_w;
}

uint32_t
axl_ttf_measure(
    AxlTtf      *ttf,
    const char  *utf8,
    float        px_size
    )
{
    if (!ttf || !utf8 || px_size <= 0.0f) {
        return 0;
    }
    float scale = stbtt_ScaleForPixelHeight(&ttf->info, px_size);
    return measure_utf8_prefix(&ttf->info, utf8, axl_strlen(utf8), scale);
}

uint32_t
axl_ttf_measure_prefix(
    AxlTtf      *ttf,
    const char  *utf8,
    size_t       prefix_bytes,
    float        px_size
    )
{
    if (!ttf || !utf8 || px_size <= 0.0f) {
        return 0;
    }
    if (prefix_bytes == 0) {
        return 0;
    }
    float scale = stbtt_ScaleForPixelHeight(&ttf->info, px_size);
    return measure_utf8_prefix(&ttf->info, utf8, prefix_bytes, scale);
}

// ===================================================================
// Public API — drawing
// ===================================================================

/* Composite a single glyph pixel into the active draw target.
 * @a coverage is the 8-bit coverage value from stb's glyph bitmap
 * (0 = no glyph, 255 = full glyph).  We modulate @a base_color's
 * alpha by coverage so the resulting pixel blends correctly on
 * buffer targets, then forward to axl_gfx_fill_rect_i — which
 * already handles the active clip stack, target dispatch, and
 * alpha blending for buffer destinations. */
static void
ttf_blit_glyph_pixel(
    int32_t      px,
    int32_t      py,
    uint8_t      coverage,
    AxlGfxPixel  base_color
    )
{
    if (coverage == 0) {
        return;
    }
    AxlGfxPixel out = base_color;
    out.alpha = (uint8_t)(((uint32_t)base_color.alpha * coverage + 127) / 255);
    if (out.alpha == 0) {
        return;
    }
    axl_gfx_fill_rect_i(px, py, 1, 1, out);
}

int
axl_ttf_draw(
    AxlTtf       *ttf,
    int32_t       x,
    int32_t       y,
    const char   *utf8,
    float         px_size,
    AxlGfxPixel   color
    )
{
    if (!ttf || !utf8 || px_size <= 0.0f) {
        return AXL_ERR;
    }

    float scale = stbtt_ScaleForPixelHeight(&ttf->info, px_size);

    /* Pen position in floating pixels — accumulated advances plus
     * kerning carry sub-pixel precision across the string; we
     * snap to integer coordinates only for the per-glyph blit. */
    float    pen_x   = (float)x;
    int      prev_cp = -1;
    size_t   pos     = 0;

    while (utf8[pos] != '\0') {
        uint32_t cp;
        size_t n = axl_utf8_decode(utf8 + pos, &cp);
        if (n == 0) {
            break;
        }

        if (prev_cp >= 0) {
            int kern = stbtt_GetCodepointKernAdvance(&ttf->info,
                                                     prev_cp, (int)cp);
            pen_x += (float)kern * scale;
        }

        int bw = 0, bh = 0, xoff = 0, yoff = 0;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(
            &ttf->info, scale, scale, (int)cp,
            &bw, &bh, &xoff, &yoff);

        if (bitmap) {
            /* Bitmap top-left in target coordinates:
             *   pen_x rounded to nearest int + xoff (left side bearing)
             *   baseline_y + yoff (yoff is typically negative — glyph
             *                      extends above baseline)
             *
             * Use floor(pen_x + 0.5) for the rounding so negative
             * pen_x values (caller passed negative @a x for off-
             * screen widgets) round AWAY from zero correctly.
             * Plain `(int)(pen_x + 0.5)` truncates toward zero and
             * produces an off-by-one for negative coords. */
            int32_t dst_x = (int32_t)axl_floori(
                                (double)pen_x + 0.5) + xoff;
            int32_t dst_y = y + yoff;

            for (int by = 0; by < bh; by++) {
                for (int bx = 0; bx < bw; bx++) {
                    uint8_t cov = bitmap[by * bw + bx];
                    ttf_blit_glyph_pixel(dst_x + bx, dst_y + by,
                                         cov, color);
                }
            }
            stbtt_FreeBitmap(bitmap, NULL);
        }

        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&ttf->info, (int)cp, &advance, &lsb);
        pen_x += (float)advance * scale;

        prev_cp = (int)cp;
        pos    += n;
    }

    return AXL_OK;
}

// ===================================================================
// Public API — metrics
// ===================================================================

int
axl_ttf_metrics(
    AxlTtf  *ttf,
    float    px_size,
    float   *out_ascent,
    float   *out_descent,
    float   *out_line_gap
    )
{
    if (!ttf || px_size <= 0.0f) {
        return AXL_ERR;
    }

    float scale = stbtt_ScaleForPixelHeight(&ttf->info, px_size);
    int   ascent_raw, descent_raw, line_gap_raw;
    stbtt_GetFontVMetrics(&ttf->info, &ascent_raw, &descent_raw, &line_gap_raw);

    if (out_ascent) {
        *out_ascent   = (float)ascent_raw * scale;
    }
    if (out_descent) {
        /* Normalize: stb returns descent as a NEGATIVE pixel count
         * (distance below baseline).  Expose the magnitude so
         * line_height = ascent + descent + line_gap is intuitive. */
        *out_descent  = (float)(-descent_raw) * scale;
    }
    if (out_line_gap) {
        *out_line_gap = (float)line_gap_raw * scale;
    }

    return AXL_OK;
}
