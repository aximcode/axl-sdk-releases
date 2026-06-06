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

#include <axl/axl-atexit.h>     /* free the shared default font at exit */
#include <axl/axl-gfx.h>
#include <axl/axl-macros.h>
#include <axl/axl-math.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-truetype.h>

#include "axl-gfx-internal.h"   /* axl_gfx_internal_path_verts (point count) */

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
// stb_truetype is a single-header library: STB_TRUETYPE_IMPLEMENTATION
// emits the whole API, most of which AXL doesn't call. Suppress the
// vendored-code "defined but not used" noise rather than editing upstream.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../deps/stb/stb_truetype.h"
#pragma GCC diagnostic pop

// ===================================================================
// Glyph cache configuration
// ===================================================================
//
// axl_ttf_draw re-rasterizes every glyph on every call, which is the
// dominant cost in text-heavy UIs (a single scrolling list redraws
// the same glyphs dozens of times a second).  The cache below stores
// rasterized coverage bitmaps keyed on
//   (codepoint, px_size quantized to 1/64px, horizontal subpixel bin)
// — the font is implicit because the cache is per-AxlTtf.  Identity
// across calls is therefore exact, and a hit skips the stb_truetype
// rasterizer entirely.
//
// Capacity is a fixed per-font slot table with true-LRU eviction
// (monotonic access tick, evict the least-recently-used slot on a
// miss when full).  256 slots covers a full Latin UI working set at a
// couple of sizes with headroom; worst-case memory is bounded by
// 256 * (largest cached glyph bitmap).

#define GLYPH_CACHE_CAP       256u  ///< slots per font
#define GLYPH_SUBPIXEL_BINS   4u    ///< horizontal subpixel quantization (1/4 px)

// ===================================================================
// AxlTtf opaque type
// ===================================================================

/* One cached glyph: the rasterized coverage bitmap plus the key it
 * was rasterized for and its placement offsets.  @a bitmap is owned
 * by the slot (stb_truetype-allocated via axl_malloc; freed with
 * stbtt_FreeBitmap on eviction / font teardown) and is NULL for an
 * empty glyph (whitespace, or a glyph with zero coverage), with
 * @a bw == @a bh == 0 — such results are cached too (negative
 * caching) so spaces don't re-enter the rasterizer. */
typedef struct {
    bool            valid;       ///< slot occupied
    uint32_t        codepoint;   ///< key: Unicode codepoint
    uint32_t        px_q;        ///< key: px_size * 64, rounded (1/64px units)
    uint8_t         subpx_bin;   ///< key: horizontal subpixel bin [0, GLYPH_SUBPIXEL_BINS)
    int             bw;          ///< bitmap width  (0 for empty glyph)
    int             bh;          ///< bitmap height (0 for empty glyph)
    int             xoff;        ///< bitmap left offset from pen origin
    int             yoff;        ///< bitmap top offset from baseline
    unsigned char  *bitmap;      ///< owned coverage bitmap, or NULL (empty)
    uint64_t        last_used;   ///< LRU: tick of most recent access
} GlyphSlot;

typedef struct {
    GlyphSlot  slots[GLYPH_CACHE_CAP];
    uint64_t   tick;             ///< monotonic access counter (LRU clock)
} GlyphCache;

struct AxlTtf {
    stbtt_fontinfo  info;
    GlyphCache     *cache;       ///< lazily allocated on first cached draw, NULL until then
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

    /* axl_malloc does not zero — the glyph cache is allocated lazily
     * on first draw, so start with no cache. */
    ttf->cache = NULL;

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
    if (ttf->cache) {
        for (size_t i = 0; i < GLYPH_CACHE_CAP; i++) {
            if (ttf->cache->slots[i].bitmap) {
                stbtt_FreeBitmap(ttf->cache->slots[i].bitmap, NULL);
            }
        }
        axl_free(ttf->cache);
    }
    axl_free(ttf);
}

// ===================================================================
// Public API — built-in font
// ===================================================================

/* Built-in DejaVu Sans subset, defined in
 * src/gfx/fonts/font-dejavu-default.c.  Both symbols (and the loaded
 * AxlTtf below) are dropped by --gc-sections from any binary that
 * never references axl_ttf_default. */
extern const unsigned char axl_ttf_dejavu_default_data[];
extern const unsigned int  axl_ttf_dejavu_default_data_len;

/* The lazily-loaded shared default font. File-static (not a function
 * local) so the atexit hook below can release it. */
static AxlTtf *g_default_ttf = NULL;

/* Free the shared default font (handle + bounded glyph cache) at process
 * exit. Without this, the singleton — intentionally never freed during
 * the run — shows up as a flood of live allocations in the AXL_MEM_DEBUG
 * leak report, drowning out genuine leaks. axl_atexit fires in LIFO at
 * shutdown; this hook is registered on first use (early), so it runs
 * after any consumer cleanup that might still draw text. */
static void
ttf_default_atexit(void *data)
{
    (void)data;
    axl_ttf_free(g_default_ttf);   /* NULL-safe */
    g_default_ttf = NULL;
}

AxlTtf *
axl_ttf_default(void)
{
    /* Lazy one-time load; the static byte array outlives the process so
     * the zero-copy AxlTtf is safe to share. Single-threaded UEFI boot
     * services — no locking needed. */
    if (g_default_ttf == NULL) {
        g_default_ttf = axl_ttf_load(axl_ttf_dejavu_default_data,
                                     axl_ttf_dejavu_default_data_len);
        if (g_default_ttf != NULL) {
            axl_atexit(ttf_default_atexit, NULL);
        }
    }
    return g_default_ttf;
}

/* Built-in DejaVu Sans MONO subset, defined in
 * src/gfx/fonts/font-dejavu-mono.c — the fixed-width default face (the
 * editor's). Dropped by --gc-sections from any binary that never
 * references axl_ttf_mono_default. */
extern const unsigned char axl_ttf_dejavu_mono_data[];
extern const unsigned int  axl_ttf_dejavu_mono_data_len;

static AxlTtf *g_default_mono_ttf = NULL;

static void
ttf_mono_default_atexit(void *data)
{
    (void)data;
    axl_ttf_free(g_default_mono_ttf);   /* NULL-safe */
    g_default_mono_ttf = NULL;
}

AxlTtf *
axl_ttf_mono_default(void)
{
    /* Lazy one-time load, mirroring axl_ttf_default (see there). */
    if (g_default_mono_ttf == NULL) {
        g_default_mono_ttf = axl_ttf_load(axl_ttf_dejavu_mono_data,
                                          axl_ttf_dejavu_mono_data_len);
        if (g_default_mono_ttf != NULL) {
            axl_atexit(ttf_mono_default_atexit, NULL);
        }
    }
    return g_default_mono_ttf;
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

/* Composite an 8-bit coverage bitmap (@a bw x @a bh) at target
 * top-left (@a dst_x, @a dst_y) in @a color.  Shared by the cache-hit
 * and OOM-fallback draw paths so the per-pixel logic lives once. */
static void
ttf_blit_coverage(
    const unsigned char  *bitmap,
    int                   bw,
    int                   bh,
    int32_t               dst_x,
    int32_t               dst_y,
    AxlGfxPixel           color
    )
{
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            uint8_t cov = bitmap[by * bw + bx];
            ttf_blit_glyph_pixel(dst_x + bx, dst_y + by, cov, color);
        }
    }
}

/* Find — or rasterize and insert — the cached glyph for
 * (@a cp, @a px_q, @a subpx_bin) in @a ttf's per-font cache.
 *
 * @a raster_scale and @a shift_x are pure functions of the key
 * (the quantized pixel size and the bin's fractional offset); the
 * caller passes them so the rasterizer reproduces identical content
 * for a given key regardless of which exact px_size first populated
 * the slot.
 *
 * On a miss with a full cache the least-recently-used slot is
 * evicted (its bitmap freed).  Returns the populated slot, or NULL
 * only if the cache itself could not be allocated (out of memory) —
 * in which case the caller rasterizes directly without caching. */
static const GlyphSlot *
glyph_cache_fetch(
    AxlTtf    *ttf,
    int        cp,
    uint32_t   px_q,
    uint8_t    subpx_bin,
    float      raster_scale,
    float      shift_x
    )
{
    if (!ttf->cache) {
        ttf->cache = axl_malloc(sizeof *ttf->cache);
        if (!ttf->cache) {
            return NULL;
        }
        axl_memset(ttf->cache, 0, sizeof *ttf->cache);
    }
    GlyphCache *c = ttf->cache;

    uint64_t now = ++c->tick;

    GlyphSlot *free_slot = NULL;
    GlyphSlot *lru_slot  = NULL;
    for (size_t i = 0; i < GLYPH_CACHE_CAP; i++) {
        GlyphSlot *s = &c->slots[i];
        if (!s->valid) {
            if (!free_slot) {
                free_slot = s;
            }
            continue;
        }
        if (s->codepoint == (uint32_t)cp
            && s->px_q == px_q
            && s->subpx_bin == subpx_bin)
        {
            s->last_used = now;
            return s;   /* hit */
        }
        if (!lru_slot || s->last_used < lru_slot->last_used) {
            lru_slot = s;
        }
    }

    /* Miss — rasterize.  bitmap may be NULL (empty glyph) with
     * bw == bh == 0; that is a valid result we still cache. */
    int bw = 0, bh = 0, xoff = 0, yoff = 0;
    unsigned char *bitmap = stbtt_GetCodepointBitmapSubpixel(
        &ttf->info, raster_scale, raster_scale, shift_x, 0.0f, cp,
        &bw, &bh, &xoff, &yoff);

    GlyphSlot *slot = free_slot ? free_slot : lru_slot;
    if (slot->bitmap) {
        stbtt_FreeBitmap(slot->bitmap, NULL);   /* free the evicted glyph */
    }
    slot->valid     = true;
    slot->codepoint = (uint32_t)cp;
    slot->px_q      = px_q;
    slot->subpx_bin = subpx_bin;
    slot->bw        = bw;
    slot->bh        = bh;
    slot->xoff      = xoff;
    slot->yoff      = yoff;
    slot->bitmap    = bitmap;
    slot->last_used = now;
    return slot;
}

/* Draw the first @a max_bytes bytes of @a utf8 starting at baseline
 * origin (@a x, @a y).  This is the shared engine behind both
 * `axl_ttf_draw` (called with the full string length) and
 * `axl_ttf_draw_box` (called per wrapped line, where the line is a
 * non-NUL-terminated sub-range of the caller's buffer).  Stops at the
 * NUL terminator or @a max_bytes, whichever comes first, and never
 * counts a codepoint whose bytes would extend past @a max_bytes. */
static void
ttf_draw_run(
    AxlTtf       *ttf,
    int32_t       x,
    int32_t       y,
    const char   *utf8,
    size_t        max_bytes,
    float         px_size,
    AxlGfxPixel   color
    )
{
    /* Advance / kerning / pen accumulation use the EXACT scale so the
     * pen layout matches axl_ttf_measure bit-for-bit.  Only the glyph
     * bitmap is rasterized at a quantized scale (raster_scale below),
     * keyed by px_q, so the cache content is canonical per size bucket
     * regardless of which exact px_size first populated it. */
    float scale = stbtt_ScaleForPixelHeight(&ttf->info, px_size);

    /* Quantize px_size to 1/64px (26.6) for the cache key.  Whole and
     * half/quarter pixel sizes (the realistic UI cases) quantize to
     * themselves, so raster_scale == scale and the glyph bitmap is
     * bit-identical to the un-quantized render.  Clamp before the
     * narrowing cast: px_size is an unvalidated public input, and a
     * value > UINT32_MAX/64 would make the double->uint32 conversion
     * undefined.  (Such a size is absurd — stb would fail to allocate
     * the bitmap anyway — but the cast must stay defined.) */
    double px_scaled = (double)px_size * 64.0 + 0.5;
    uint32_t px_q = px_scaled >= (double)UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)px_scaled;
    if (px_q == 0u) {
        px_q = 1u;   /* guard: vanishingly small px_size still keys distinctly */
    }
    float raster_scale =
        stbtt_ScaleForPixelHeight(&ttf->info, (float)px_q / 64.0f);

    /* Pen position in floating pixels — accumulated advances plus
     * kerning carry sub-pixel precision across the string.  The
     * fractional part selects a horizontal subpixel bin; the integer
     * part is the glyph's pixel origin. */
    float    pen_x   = (float)x;
    int      prev_cp = -1;
    size_t   pos     = 0;

    while (pos < max_bytes && utf8[pos] != '\0') {
        uint32_t cp;
        size_t n = axl_utf8_decode(utf8 + pos, &cp);
        if (n == 0) {
            break;
        }
        /* Don't render a codepoint whose bytes spill past the byte
         * limit (mirrors axl_ttf_measure_prefix).  Box lines break on
         * whitespace so this never trims a real glyph there; it is a
         * safety net for arbitrary callers. */
        if (pos + n > max_bytes) {
            break;
        }

        if (prev_cp >= 0) {
            int kern = stbtt_GetCodepointKernAdvance(&ttf->info,
                                                     prev_cp, (int)cp);
            pen_x += (float)kern * scale;
        }

        /* Split the pen into an integer origin and a quantized
         * subpixel shift.  axl_floori rounds toward -inf so negative
         * pen positions (off-screen widgets) keep a fractional part
         * in [0, 1).  Rounding the bin can land on GLYPH_SUBPIXEL_BINS
         * (frac just below 1.0); fold that into the next integer pixel
         * at bin 0 so the shift stays in [0, 1). */
        int      ix   = axl_floori((double)pen_x);
        double   frac = (double)pen_x - (double)ix;
        uint32_t bin  = (uint32_t)(frac * (double)GLYPH_SUBPIXEL_BINS + 0.5);
        if (bin >= GLYPH_SUBPIXEL_BINS) {
            bin = 0u;
            ix += 1;
        }
        float shift_x = (float)bin / (float)GLYPH_SUBPIXEL_BINS;

        const GlyphSlot *g = glyph_cache_fetch(ttf, (int)cp, px_q,
                                               (uint8_t)bin,
                                               raster_scale, shift_x);
        if (g) {
            /* Bitmap top-left in target coordinates: integer pen
             * origin + the glyph's left/top offsets (the subpixel
             * shift is already baked into the rasterized coverage and
             * reflected in g->xoff).  g->bitmap is NULL for an empty
             * glyph (negative-cached) — nothing to blit. */
            if (g->bitmap) {
                ttf_blit_coverage(g->bitmap, g->bw, g->bh,
                                  ix + g->xoff, y + g->yoff, color);
            }
        } else {
            /* Cache allocation failed (OOM): rasterize directly so
             * drawing still works, paying the per-glyph cost. */
            int bw = 0, bh = 0, xoff = 0, yoff = 0;
            unsigned char *bitmap = stbtt_GetCodepointBitmapSubpixel(
                &ttf->info, raster_scale, raster_scale, shift_x, 0.0f,
                (int)cp, &bw, &bh, &xoff, &yoff);
            if (bitmap) {
                ttf_blit_coverage(bitmap, bw, bh,
                                  ix + xoff, y + yoff, color);
                stbtt_FreeBitmap(bitmap, NULL);
            }
        }

        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&ttf->info, (int)cp, &advance, &lsb);
        pen_x += (float)advance * scale;

        prev_cp = (int)cp;
        pos    += n;
    }
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
    ttf_draw_run(ttf, x, y, utf8, axl_strlen(utf8), px_size, color);
    return AXL_OK;
}

/* Map a local-space point (lx, ly) through the transform into device. */
static inline void
ttf_transform_pt(const AxlTransform *m, float lx, float ly,
                 float *ox, float *oy)
{
    AxlVec2 d = axl_transform_map_point(*m, axl_vec2(lx, ly));
    *ox = (float)d.x;
    *oy = (float)d.y;
}

static inline float
ttf_absf(float v)
{
    return v < 0.0f ? -v : v;
}

/* Subdivision count for a curve, from the Manhattan length of its
 * projected control polygon (a cheap proxy for on-screen size). */
static int
ttf_curve_segments(float poly)
{
    int n = (int)(poly / 3.0f) + 1;
    if (n < 6)  { n = 6; }
    if (n > 48) { n = 48; }
    return n;
}

/* Flatten a local-space quadratic Bezier (P0 -> control -> P1) into
 * device line segments, sampling and projecting in LOCAL space.  A
 * projective transform maps lines to lines, so flattening locally and
 * projecting the vertices reproduces the exact device curve (unlike
 * projecting the control points and re-fitting a Bezier in device
 * space, which is only correct for an affine transform). */
static void
ttf_flatten_quad(
    AxlGfxPath          *path,
    const AxlTransform  *m,
    float x0, float y0, float cx, float cy, float x1, float y1
    )
{
    float d0x, d0y, dcx, dcy, d1x, d1y;
    ttf_transform_pt(m, x0, y0, &d0x, &d0y);
    ttf_transform_pt(m, cx, cy, &dcx, &dcy);
    ttf_transform_pt(m, x1, y1, &d1x, &d1y);
    int nseg = ttf_curve_segments(ttf_absf(d0x - dcx) + ttf_absf(d0y - dcy)
                                + ttf_absf(dcx - d1x) + ttf_absf(dcy - d1y));
    for (int i = 1; i <= nseg; i++) {
        float t  = (float)i / (float)nseg;
        float mt = 1.0f - t;
        float bx = mt * mt * x0 + 2.0f * mt * t * cx + t * t * x1;
        float by = mt * mt * y0 + 2.0f * mt * t * cy + t * t * y1;
        float dx, dy;
        ttf_transform_pt(m, bx, by, &dx, &dy);
        axl_gfx_path_line_to(path, dx, dy);
    }
}

/* Flatten a local-space cubic Bezier (P0 -> C1 -> C2 -> P1), as
 * ttf_flatten_quad but for the cubic basis. */
static void
ttf_flatten_cubic(
    AxlGfxPath          *path,
    const AxlTransform  *m,
    float x0,  float y0,  float c1x, float c1y,
    float c2x, float c2y, float x1,  float y1
    )
{
    float d0x, d0y, dc1x, dc1y, dc2x, dc2y, d1x, d1y;
    ttf_transform_pt(m, x0,  y0,  &d0x,  &d0y);
    ttf_transform_pt(m, c1x, c1y, &dc1x, &dc1y);
    ttf_transform_pt(m, c2x, c2y, &dc2x, &dc2y);
    ttf_transform_pt(m, x1,  y1,  &d1x,  &d1y);
    int nseg = ttf_curve_segments(
        ttf_absf(d0x - dc1x)  + ttf_absf(d0y - dc1y)
      + ttf_absf(dc1x - dc2x) + ttf_absf(dc1y - dc2y)
      + ttf_absf(dc2x - d1x)  + ttf_absf(dc2y - d1y));
    for (int i = 1; i <= nseg; i++) {
        float t  = (float)i / (float)nseg;
        float mt = 1.0f - t;
        float b0 = mt * mt * mt,       b1 = 3.0f * mt * mt * t;
        float b2 = 3.0f * mt * t * t,  b3 = t * t * t;
        float bx = b0 * x0 + b1 * c1x + b2 * c2x + b3 * x1;
        float by = b0 * y0 + b1 * c1y + b2 * c2y + b3 * y1;
        float dx, dy;
        ttf_transform_pt(m, bx, by, &dx, &dy);
        axl_gfx_path_line_to(path, dx, dy);
    }
}

int
axl_ttf_draw_transform(
    AxlTtf              *font,
    const char          *utf8,
    float                px_size,
    const AxlTransform  *m,
    AxlGfxPixel          color
    )
{
    if (!font || !utf8 || !m || px_size <= 0.0f) {
        return AXL_ERR;
    }

    float       scale = stbtt_ScaleForPixelHeight(&font->info, px_size);
    AxlGfxPath *path  = axl_gfx_path_new();
    if (!path) {
        return AXL_ERR;
    }
    /* Affine transforms preserve the Bezier basis (project control
     * points, fill in device space — exact and cheap).  A projective
     * transform does not, so curves are flattened in local space and
     * projected vertex-by-vertex. */
    bool affine = axl_transform_is_affine(*m);

    int      rc      = AXL_OK;
    float    pen_x   = 0.0f;
    uint32_t cp      = 0;
    uint32_t prev_cp = 0;
    size_t   n;
    for (const char *p = utf8; (n = axl_utf8_decode(p, &cp)) > 0; p += n) {
        if (prev_cp != 0) {
            int k = stbtt_GetCodepointKernAdvance(&font->info,
                                                  (int)prev_cp, (int)cp);
            pen_x += (float)k * scale;
        }

        stbtt_vertex *verts = NULL;
        int           nv = stbtt_GetCodepointShape(&font->info, (int)cp, &verts);
        if (nv > 0 && verts != NULL) {
            axl_gfx_path_reset(path);
            float curx = 0.0f, cury = 0.0f;    /* current point, device space */
            float curlx = 0.0f, curly = 0.0f;  /* current point, local space  */
            for (int i = 0; i < nv; i++) {
                stbtt_vertex *v = &verts[i];
                /* Glyph font-units -> local pixels; font y is up, the
                 * target is y-down, so the baseline sits at local y = 0
                 * and the glyph rises into negative y. */
                float lx = pen_x + (float)v->x * scale;
                float ly = -((float)v->y * scale);
                float dx, dy;
                ttf_transform_pt(m, lx, ly, &dx, &dy);

                switch (v->type) {
                case STBTT_vmove:
                    axl_gfx_path_move_to(path, dx, dy);
                    break;
                case STBTT_vline:
                    axl_gfx_path_line_to(path, dx, dy);
                    break;
                case STBTT_vcurve: {
                    float clx = pen_x + (float)v->cx * scale;
                    float cly = -((float)v->cy * scale);
                    if (affine) {
                        /* Affine commutes with the Bezier basis: project
                         * the quadratic control, elevate to cubic in
                         * device space. */
                        float cxd, cyd;
                        ttf_transform_pt(m, clx, cly, &cxd, &cyd);
                        float c1x = curx + (2.0f / 3.0f) * (cxd - curx);
                        float c1y = cury + (2.0f / 3.0f) * (cyd - cury);
                        float c2x = dx + (2.0f / 3.0f) * (cxd - dx);
                        float c2y = dy + (2.0f / 3.0f) * (cyd - dy);
                        axl_gfx_path_curve_to(path, c1x, c1y, c2x, c2y, dx, dy);
                    } else {
                        /* Projective: flatten in local space (lines are
                         * preserved by the projection). */
                        ttf_flatten_quad(path, m, curlx, curly, clx, cly, lx, ly);
                    }
                    break;
                }
                case STBTT_vcubic: {
                    float c1lx = pen_x + (float)v->cx  * scale;
                    float c1ly = -((float)v->cy  * scale);
                    float c2lx = pen_x + (float)v->cx1 * scale;
                    float c2ly = -((float)v->cy1 * scale);
                    if (affine) {
                        float c1xd, c1yd, c2xd, c2yd;
                        ttf_transform_pt(m, c1lx, c1ly, &c1xd, &c1yd);
                        ttf_transform_pt(m, c2lx, c2ly, &c2xd, &c2yd);
                        axl_gfx_path_curve_to(path, c1xd, c1yd, c2xd, c2yd, dx, dy);
                    } else {
                        ttf_flatten_cubic(path, m, curlx, curly,
                                          c1lx, c1ly, c2lx, c2ly, lx, ly);
                    }
                    break;
                }
                default:
                    break;
                }
                curx  = dx;
                cury  = dy;
                curlx = lx;
                curly = ly;
            }
            /* Only fill a fillable contour (>= 3 points). A degenerate
             * glyph is a no-op, not an error — mirrors how an empty
             * (space) glyph is skipped, so it can't turn the whole call
             * into AXL_ERR. */
            size_t pn = 0;
            (void)axl_gfx_internal_path_verts(path, &pn);
            if (pn >= 3 && axl_gfx_fill_path(path, color) == AXL_ERR) {
                rc = AXL_ERR;
            }
        }
        if (verts != NULL) {
            stbtt_FreeShape(&font->info, verts);
        }

        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font->info, (int)cp, &adv, &lsb);
        pen_x += (float)adv * scale;
        prev_cp = cp;
    }

    axl_gfx_path_free(path);
    return rc;
}

// ===================================================================
// Public API — boxed / multi-line text (G11)
// ===================================================================
//
// axl_ttf_draw_box / axl_ttf_measure_box share one line-breaking
// engine (ttf_box_layout): it walks the UTF-8 once, splits on hard
// '\n' breaks, greedily packs whitespace-delimited words while the
// rendered width stays <= the wrap width, and invokes a callback per
// laid-out line.  measure_box accumulates geometry; draw_box renders
// each line.  Both reuse the existing measurement (measure_utf8_prefix)
// and rendering (ttf_draw_run) primitives — no glyph logic lives here.

/* Whitespace that opens a word-break opportunity inside a hard line.
 * '\n' is handled one level up (it ends the hard line); all of these
 * are ASCII single-byte, so splitting on them is always codepoint-
 * safe (UTF-8 continuation bytes are >= 0x80). */
static inline bool
box_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Line-height in pixels: ascent + descent + line_gap, computed term-
 * by-term in the same order as axl_ttf_metrics so callers measuring
 * via the public metrics API get a bit-identical line advance. */
static float
ttf_line_height_px(
    const stbtt_fontinfo  *info,
    float                  scale
    )
{
    int ascent_raw = 0, descent_raw = 0, line_gap_raw = 0;
    stbtt_GetFontVMetrics(info, &ascent_raw, &descent_raw, &line_gap_raw);
    float ascent  = (float)ascent_raw * scale;
    float descent = (float)(-descent_raw) * scale;   /* magnitude */
    float gap     = (float)line_gap_raw * scale;
    return ascent + descent + gap;
}

/* Per-line callback.  @a line points into the caller's UTF-8 buffer
 * (NOT NUL-terminated at @a line_len); @a line_width is its rendered
 * pixel width; @a line_index is the 0-based line number.  Returns true
 * to keep laying out, false to stop early (draw uses this to bail once
 * lines fall below the box). */
typedef bool (*box_line_fn)(
    void        *ctx,
    const char  *line,
    size_t       line_len,
    uint32_t     line_width,
    size_t       line_index
    );

/* Lay out @a utf8 into wrapped lines for wrap width @a w, invoking
 * @a emit once per line.  Returns the number of lines emitted (for an
 * uninterrupted run this is the total line count; if @a emit returns
 * false it is the count emitted so far). */
static size_t
ttf_box_layout(
    const stbtt_fontinfo  *info,
    const char            *utf8,
    uint32_t               w,
    float                  scale,
    box_line_fn            emit,
    void                  *ctx
    )
{
    size_t n = axl_strlen(utf8);
    if (n == 0) {
        return 0;   /* empty input → no lines */
    }

    size_t line_index = 0;
    size_t i          = 0;
    bool   keep_going = true;

    while (i <= n && keep_going) {
        /* Carve out the next hard line [hl_start, hl_end) up to the
         * next '\n' (or end of string). */
        size_t hl_start = i;
        size_t hl_end   = hl_start;
        while (hl_end < n && utf8[hl_end] != '\n') {
            hl_end++;
        }

        /* Skip leading whitespace of the hard line. */
        size_t j = hl_start;
        while (j < hl_end && box_is_ws(utf8[j])) {
            j++;
        }

        if (j == hl_end) {
            /* Empty or all-whitespace hard line → one blank line. */
            keep_going = emit(ctx, utf8 + hl_start, 0, 0, line_index++);
        } else {
            size_t   line_start = j;
            size_t   cur_end    = j;   /* committed content end (exclusive) */
            uint32_t cur_w      = 0;   /* width of [line_start, cur_end) */

            while (j < hl_end && keep_going) {
                while (j < hl_end && box_is_ws(utf8[j])) {
                    j++;
                }
                if (j == hl_end) {
                    break;             /* trailing whitespace — discard */
                }
                size_t word_start = j;
                while (j < hl_end && !box_is_ws(utf8[j])) {
                    j++;
                }
                size_t word_end = j;

                if (cur_end == line_start) {
                    /* First word on the line: always accept, even if it
                     * alone overflows @a w (over-wide words are not
                     * split — they overflow on their own line). */
                    cur_end = word_end;
                    cur_w   = measure_utf8_prefix(info, utf8 + line_start,
                                                  word_end - line_start, scale);
                } else {
                    uint32_t cand_w = measure_utf8_prefix(
                        info, utf8 + line_start, word_end - line_start, scale);
                    if (cand_w <= w) {
                        cur_end = word_end;
                        cur_w   = cand_w;
                    } else {
                        /* Doesn't fit: flush the committed line, then
                         * start a fresh line with this word. */
                        keep_going = emit(ctx, utf8 + line_start,
                                          cur_end - line_start, cur_w,
                                          line_index++);
                        if (!keep_going) {
                            break;
                        }
                        line_start = word_start;
                        cur_end    = word_end;
                        cur_w      = measure_utf8_prefix(
                            info, utf8 + word_start,
                            word_end - word_start, scale);
                    }
                }
            }

            if (keep_going) {
                keep_going = emit(ctx, utf8 + line_start,
                                  cur_end - line_start, cur_w, line_index++);
            }
        }

        if (hl_end >= n) {
            break;            /* consumed the whole string */
        }
        i = hl_end + 1;       /* step past the '\n' (may leave i == n,
                               * producing a trailing blank line) */
    }

    return line_index;
}

/* measure_box accumulator: widest line seen. */
typedef struct {
    uint32_t  max_w;
} BoxMeasureCtx;

static bool
box_measure_emit(
    void        *vctx,
    const char  *line,
    size_t       line_len,
    uint32_t     line_width,
    size_t       line_index
    )
{
    (void)line;
    (void)line_len;
    (void)line_index;
    BoxMeasureCtx *c = vctx;
    if (line_width > c->max_w) {
        c->max_w = line_width;
    }
    return true;
}

int
axl_ttf_measure_box(
    AxlTtf       *ttf,
    uint32_t      w,
    const char   *utf8,
    float         px_size,
    uint32_t     *out_width,
    uint32_t     *out_height,
    uint32_t     *out_lines
    )
{
    if (!ttf || !utf8 || px_size <= 0.0f) {
        return AXL_ERR;
    }

    float scale = stbtt_ScaleForPixelHeight(&ttf->info, px_size);
    BoxMeasureCtx ctx = { 0u };
    size_t lines = ttf_box_layout(&ttf->info, utf8, w, scale,
                                  box_measure_emit, &ctx);

    if (out_width) {
        *out_width = ctx.max_w;
    }
    if (out_lines) {
        *out_lines = (uint32_t)lines;
    }
    if (out_height) {
        if (lines == 0) {
            *out_height = 0u;
        } else {
            float line_h = ttf_line_height_px(&ttf->info, scale);
            int   hgt    = axl_ceili((double)((float)lines * line_h));
            *out_height  = hgt < 0 ? 0u : (uint32_t)hgt;
        }
    }
    return AXL_OK;
}

/* draw_box per-line state. */
typedef struct {
    AxlTtf       *ttf;
    int32_t       x;
    int32_t       y;
    uint32_t      w;
    uint32_t      h;
    float         px_size;
    AxlGfxPixel   color;
    uint32_t      align;     /* AXL_TTF_ALIGN_* */
    float         ascent;    /* pixels, baseline of line 0 = y + ascent */
    float         line_h;    /* pixels per line advance */
} BoxDrawCtx;

static bool
box_draw_emit(
    void        *vctx,
    const char  *line,
    size_t       line_len,
    uint32_t     line_width,
    size_t       line_index
    )
{
    BoxDrawCtx *c = vctx;

    /* Vertical: once a line's top edge is at/below the box bottom, it
     * and every later line are fully outside — stop.  (Lines that
     * straddle the bottom are still drawn and clipped per-pixel by the
     * box clip pushed in axl_ttf_draw_box.) */
    int64_t top = (int64_t)c->y
                + axl_floori((double)((float)line_index * c->line_h) + 0.5);
    if (top >= (int64_t)c->y + (int64_t)c->h) {
        return false;
    }

    if (line_len == 0) {
        return true;   /* blank line: advance only, nothing to draw */
    }

    int32_t lx;
    switch (c->align) {
    case AXL_TTF_ALIGN_CENTER:
        lx = c->x + ((int32_t)c->w - (int32_t)line_width) / 2;
        break;
    case AXL_TTF_ALIGN_RIGHT:
        lx = c->x + ((int32_t)c->w - (int32_t)line_width);
        break;
    case AXL_TTF_ALIGN_LEFT:
    default:
        lx = c->x;
        break;
    }

    int32_t baseline = c->y + axl_floori(
        (double)(c->ascent + (float)line_index * c->line_h) + 0.5);

    ttf_draw_run(c->ttf, lx, baseline, line, line_len, c->px_size, c->color);
    return true;
}

int
axl_ttf_draw_box(
    AxlTtf       *ttf,
    int32_t       x,
    int32_t       y,
    uint32_t      w,
    uint32_t      h,
    const char   *utf8,
    float         px_size,
    AxlGfxPixel   color,
    uint32_t      flags
    )
{
    if (!ttf || !utf8 || px_size <= 0.0f) {
        return AXL_ERR;
    }

    /* Clip all content to the box (intersected with any active clip) so
     * over-wide words are cut at the right edge and a final partial
     * line is cut at the bottom. */
    AxlGfxClip box = { x, y, w, h };
    if (axl_gfx_push_clip(box) != AXL_OK) {
        return AXL_ERR;
    }

    float scale = stbtt_ScaleForPixelHeight(&ttf->info, px_size);
    int   ascent_raw = 0, descent_raw = 0, line_gap_raw = 0;
    stbtt_GetFontVMetrics(&ttf->info, &ascent_raw, &descent_raw, &line_gap_raw);

    BoxDrawCtx ctx = {
        .ttf     = ttf,
        .x       = x,
        .y       = y,
        .w       = w,
        .h       = h,
        .px_size = px_size,
        .color   = color,
        .align   = flags & AXL_TTF_ALIGN_MASK,
        .ascent  = (float)ascent_raw * scale,
        .line_h  = ttf_line_height_px(&ttf->info, scale),
    };

    ttf_box_layout(&ttf->info, utf8, w, scale, box_draw_emit, &ctx);

    axl_gfx_pop_clip();
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
