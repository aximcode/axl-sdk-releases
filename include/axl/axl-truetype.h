/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-truetype.h
    Vector text rendering from TrueType / OpenType fonts.

    Loads a TTF/OTF font from a byte buffer, measures rendered string
    widths, draws strings into the active axl-gfx draw target.
    Rasterization is internal (stb_truetype-backed); the public API
    is toolkit-neutral — no em units, no line-box metrics, no CSS
    color names.  Consumers (Path A2 HTML painter, AGT widget
    toolkit) translate at their own layer.

    Coordinate convention: the (@a x, @a y) passed to
    `axl_ttf_draw` is the **glyph-row origin** — the pen position at
    the start of the baseline.  Top-vs-baseline positioning is
    consumer policy; consumers compute pen-y from their own line-box
    rules (e.g. `pen_y = box_top + ascent`).

    @code
    // Load font once (bytes must outlive the AxlTtf).
    AxlTtf *font = axl_ttf_load(font_bytes, font_len);
    if (!font) { ... }

    // Measure for layout.
    uint32_t w = axl_ttf_measure(font, "Hello, world!", 16.0f);

    // Compute baseline from metrics.
    float ascent, descent, line_gap;
    axl_ttf_metrics(font, 16.0f, &ascent, &descent, &line_gap);
    int baseline_y = 100 + (int)ascent;

    // Draw at baseline.
    axl_ttf_draw(font, 50, baseline_y, "Hello, world!",
                 16.0f, AXL_GFX_BLACK);

    axl_ttf_free(font);
    @endcode
**/

#ifndef AXL_TRUETYPE_H
#define AXL_TRUETYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-gfx-types.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Types
// ===================================================================

/// Opaque handle to a parsed TrueType / OpenType font.
///
/// Wraps the underlying stb_truetype font info plus axl-sdk-side
/// caching (glyph index lookups, scale factors).  Created with
/// `axl_ttf_load`; freed with `axl_ttf_free`.  Reentrant for read-
/// only ops (`measure`, `measure_prefix`, `metrics`) — multiple
/// callers may share one `AxlTtf *` across logically concurrent
/// reads.  `axl_ttf_draw` mutates the active draw target so callers
/// must serialize draws against the target (same convention as
/// `axl_gfx_fill_rect`).
typedef struct AxlTtf AxlTtf;

// ===================================================================
// Load / free
// ===================================================================

/// Load a TTF / OTF font from a byte buffer.
///
/// @a bytes is referenced by the returned `AxlTtf`, not copied — it
/// MUST remain valid (and unmodified) for the lifetime of the
/// `AxlTtf`.  This matches the zero-copy convention of
/// `axl_gfx_buffer_pixels` and lets consumers embed fonts as `xxd`-
/// generated static arrays with zero runtime allocation for the
/// font data itself.  Free the byte buffer only after
/// `axl_ttf_free`.
///
/// TTC (TrueType Collection) inputs are accepted; only the first
/// font in the collection is loaded.
///
/// @return new `AxlTtf` on success (caller frees with
///         `axl_ttf_free`), or NULL on parse failure, allocation
///         failure, @a bytes NULL, or @a len < minimum TTF header
///         size.
AxlTtf *
axl_ttf_load(
    const uint8_t  *bytes,   ///< [in] TTF/OTF/TTC byte buffer (kept alive by caller)
    size_t          len      ///< buffer length in bytes
    );

/// Free a font loaded with `axl_ttf_load`.
///
/// Safe to call with NULL.  Does NOT free the byte buffer passed
/// to `axl_ttf_load` — the caller owns that.
void
axl_ttf_free(
    AxlTtf  *ttf   ///< font to free, or NULL
    );

// ===================================================================
// Built-in font
// ===================================================================

/// Return a shared, ready-to-use built-in TrueType font.
///
/// Backed by a subset of DejaVu Sans compiled into `libaxl.a`,
/// covering ASCII + Latin-1 (U+0020..U+00FF) plus common
/// typographic punctuation: en/em dash (U+2013/U+2014), curly
/// quotes (U+2018..U+201D), bullet (U+2022), and ellipsis
/// (U+2026).  Sufficient for Western-European UI text without the
/// consumer bundling a font asset.
///
/// The font is loaded once on the first successful call and
/// cached; every call returns the **same** handle.  The caller
/// does **not** own it:
/// do NOT pass the result to `axl_ttf_free` (the backing bytes are
/// static and the handle is shared process-wide).  Mirrors the
/// ownership model of `axl_gfx_default_font`.
///
/// First-call load is not reentrant (it performs the one-time
/// parse + allocation); subsequent calls are read-only.  This
/// matches the single-threaded UEFI boot-services model.
///
/// The embedded font data is dropped by `--gc-sections` from any
/// binary that never references this function, so consumers that
/// load their own font pay no size cost.
///
/// @return shared `AxlTtf *` (never freed by the caller), or NULL
///         if the one-time load fails (allocation failure).
AxlTtf *
axl_ttf_default(void);

// ===================================================================
// Measurement
// ===================================================================

/// Compute the rendered pixel width of a UTF-8 string.
///
/// @a utf8 is decoded as UTF-8; each codepoint contributes its
/// scaled per-glyph advance (including kerning where the font
/// provides it).  Invalid UTF-8 sequences become U+FFFD REPLACEMENT
/// CHARACTER.  Codepoints absent from the font advance by the
/// font's `.notdef` glyph advance.
///
/// @a px_size is the font's vertical pixel height (ascent +
/// |descent|, NOT em-square).  Fractional sizes are accepted —
/// pass `16.0f` for a 16-pixel font, `13.5f` for hi-DPI scaling.
///
/// Does NOT require GOP — pure measurement.
///
/// @return rendered width in pixels (rounded up to the next whole
///         pixel), or 0 if @a ttf is NULL, @a utf8 is NULL, or
///         @a px_size is <= 0.
uint32_t
axl_ttf_measure(
    AxlTtf      *ttf,        ///< [in] font
    const char  *utf8,       ///< [in] NUL-terminated UTF-8 string
    float        px_size     ///< pixel height (e.g. 16.0f)
    );

/// Compute the rendered pixel width of the first @a prefix_bytes
/// bytes of a UTF-8 string.
///
/// Designed for cursor positioning in edit widgets: a binary search
/// over byte positions converts a click x-coordinate to a string
/// byte offset in O(log n * measure) without materializing per-
/// glyph advance arrays.  Equivalent to `axl_ttf_measure` when
/// @a prefix_bytes == `strlen(utf8)`.
///
/// If @a prefix_bytes lands mid-codepoint, the trailing partial
/// codepoint is **not** counted in the returned width (the function
/// stops at the last complete codepoint within the byte limit).
/// Invalid UTF-8 sequences inside the prefix become U+FFFD.
///
/// Does NOT require GOP.
///
/// @return rendered prefix width in pixels (rounded up), 0 for
///         @a prefix_bytes == 0, or 0 on invalid args (@a ttf or
///         @a utf8 NULL, @a px_size <= 0).
uint32_t
axl_ttf_measure_prefix(
    AxlTtf      *ttf,            ///< [in] font
    const char  *utf8,           ///< [in] NUL-terminated UTF-8 string
    size_t       prefix_bytes,   ///< measure first N bytes (0 → 0)
    float        px_size         ///< pixel height
    );

// ===================================================================
// Drawing
// ===================================================================

/// Draw a UTF-8 string into the active axl-gfx draw target.
///
/// (@a x, @a y) is the **baseline origin** — the pen position
/// immediately left of the first glyph's left side-bearing, on the
/// baseline.  Consumers wanting top-aligned text compute
/// `y = top + ascent` using `axl_ttf_metrics`.
///
/// Honors the active clip stack and the current draw target
/// (`axl_gfx_target_buffer`).  On screen targets the rasterizer
/// composites against the framebuffer.  On buffer targets,
/// `color.alpha < 0xFF` produces source-over blending with the
/// existing buffer contents (matching `axl_gfx_fill_rect`
/// semantics).  Anti-aliased glyph coverage modulates the source
/// alpha.
///
/// Invalid UTF-8 sequences render as U+FFFD.  Codepoints absent
/// from the font render as `.notdef` (typically a hollow box) or
/// skip if the font has no `.notdef`.
///
/// Coordinates may be negative — glyphs partially outside the clip
/// or target are clipped, not skipped.
///
/// @return AXL_OK on success.  AXL_ERR if @a ttf is NULL, @a utf8
///         is NULL, @a px_size is <= 0, or the active target is
///         the screen and GOP is unavailable.
int
axl_ttf_draw(
    AxlTtf       *ttf,          ///< [in] font
    int32_t       x,            ///< baseline x origin (may be negative)
    int32_t       y,            ///< baseline y origin (may be negative)
    const char   *utf8,         ///< [in] NUL-terminated UTF-8 string
    float         px_size,      ///< pixel height
    AxlGfxPixel   color         ///< text color (alpha honored on buffer targets)
    );

// ===================================================================
// Font metrics
// ===================================================================

/// Get vertical font metrics at @a px_size.
///
/// All outputs are in pixels, all non-negative:
///   - @a ascent: distance from baseline to top of typographic em-box
///   - @a descent: distance from baseline to bottom of em-box
///     (returned positive — the magnitude of the descent)
///   - @a line_gap: recommended extra leading between lines
///
/// Standard line-height computation:
///     line_height = ascent + descent + line_gap
///
/// Each output pointer may be NULL to skip computing that field.
/// Computed from the font's `hhea` table scaled by @a px_size.
///
/// Does NOT require GOP.
///
/// @return AXL_OK on success.  AXL_ERR if @a ttf is NULL or
///         @a px_size is <= 0 (output pointers untouched on error).
int
axl_ttf_metrics(
    AxlTtf  *ttf,           ///< [in] font
    float    px_size,       ///< pixel height
    float   *out_ascent,    ///< [out] pixels above baseline (NULL OK)
    float   *out_descent,   ///< [out] pixels below baseline, positive (NULL OK)
    float   *out_line_gap   ///< [out] recommended leading in pixels (NULL OK)
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_TRUETYPE_H */
