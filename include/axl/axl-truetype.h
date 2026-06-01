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
#include <axl/axl-math.h>
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

/// Draw a UTF-8 string through a transform (rotation, shear,
/// non-uniform scale) — the general-transform counterpart to
/// `axl_ttf_draw`'s axis-aligned fast path.
///
/// The glyphs are laid out on the baseline starting at the **local
/// origin (0, 0)** at @a px_size pixel height (advances + kerning in
/// local pixels, exactly as `axl_ttf_draw` lays them out).  The
/// transform @a m then maps that local pixel space into the draw
/// target: its translation places the baseline origin, and its linear
/// part applies any rotation / shear / extra scale.  So
/// `axl_transform_translate(x, y)` reproduces `axl_ttf_draw(x, y, …)`,
/// and `axl_transform_multiply(axl_transform_rotate(a),
/// axl_transform_translate(x, y))` draws a baseline rotated by `a`
/// about `(x, y)` (rotate first, then translate — cairo order).
///
/// Unlike `axl_ttf_draw` (which composites cached coverage bitmaps and
/// cannot rotate), this pulls each glyph's vector outline, transforms
/// the control points, and fills it through the analytic path
/// rasterizer — so it is anti-aliased at any angle.  It is
/// correspondingly heavier per call (no glyph-bitmap cache); keep using
/// `axl_ttf_draw` for upright text.
///
/// The active graphics transform (`axl_gfx_translate` et al.) composes
/// on top: the effective mapping is `CTM × m`.  With the default
/// identity CTM the mapping is just @a m.  Honors the active clip stack
/// (including `axl_gfx_push_clip_quad`) and draw target; @a color alpha
/// is modulated by glyph coverage and blended on buffer targets.
///
/// Invalid UTF-8 renders as U+FFFD; absent codepoints render as
/// `.notdef`.
///
/// @return AXL_OK on success.  AXL_ERR if @a font, @a utf8, or @a m is
///         NULL, @a px_size is <= 0, on allocation failure, or the
///         active target is the screen and GOP is unavailable.
int
axl_ttf_draw_transform(
    AxlTtf              *font,     ///< [in] font
    const char          *utf8,     ///< [in] NUL-terminated UTF-8 string
    float                px_size,  ///< local baseline pixel height (> 0)
    const AxlTransform  *m,        ///< [in] local-space → target transform
    AxlGfxPixel          color     ///< text color (alpha honored on buffer targets)
    );

// ===================================================================
// Boxed / multi-line text (G11)
// ===================================================================

/// Horizontal alignment of each wrapped line within the box.
///
/// These values occupy the low bits of the @a flags argument to
/// `axl_ttf_draw_box`; select exactly one (LEFT is the zero default).
/// The remaining bits are reserved for future layout flags (e.g.
/// vertical alignment, mid-word breaking) so the `flags` argument
/// stays source-compatible as the box layout grows.
#define AXL_TTF_ALIGN_LEFT    0x0u   ///< align each line to the box left edge (default)
#define AXL_TTF_ALIGN_CENTER  0x1u   ///< center each line horizontally in the box
#define AXL_TTF_ALIGN_RIGHT   0x2u   ///< align each line to the box right edge
#define AXL_TTF_ALIGN_MASK    0x3u   ///< mask selecting the horizontal-alignment field

/// Draw word-wrapped UTF-8 text inside a rectangle.
///
/// Lays the text out into lines and draws each line with
/// `axl_ttf_draw`, building purely on the existing measurement +
/// glyph primitives (no shaping, no external dependency).
///
/// **Layout rules**
///   - **Hard breaks:** every `\n` ends the current line and starts a
///     new one.  Consecutive newlines therefore produce blank lines,
///     and a trailing `\n` produces a trailing blank line (each `\n`
///     starts a line).  `\r` and `\t` are treated as whitespace.
///   - **Word wrap:** within a hard line, text is broken on runs of
///     whitespace (space / tab / CR).  Words are kept whole; a line
///     grows greedily while the rendered width of its content is
///     `<= w`.  Whitespace runs are break opportunities — leading and
///     trailing whitespace on a wrapped line is not rendered, but
///     whitespace *interior* to a line is preserved.
///   - **Grapheme safety:** breaks only ever fall on whitespace, never
///     inside a multi-byte UTF-8 sequence.  (Full UAX-29 grapheme
///     segmentation — e.g. ZWJ emoji — is a consumer-layer / FreeType
///     concern; the lean tier breaks on whitespace + codepoint
///     boundaries.)
///   - **Over-wide word:** a single word wider than @a w is **not
///     split**; it is placed on its own line and overflows the box's
///     right edge.  The overflow is clipped to the box when drawn (see
///     clipping below); `axl_ttf_measure_box` reports the true
///     (possibly `> w`) width so callers can detect it.
///   - **Line advance:** baselines step by
///     `ascent + descent + line_gap` (the `axl_ttf_metrics`
///     line-height).  The first baseline is at `y + ascent`, so line
///     @a i occupies vertical span `[y + i*line_height,
///     y + i*line_height + ascent + descent]`.
///
/// **Alignment** is selected by @a flags (`AXL_TTF_ALIGN_*`); each
/// line is shifted independently within @a w.
///
/// **Clipping:** the box `(x, y, w, h)` is pushed onto the clip stack
/// (intersected with any active clip) for the duration of the draw, so
/// content is clamped to the rectangle — horizontally over-wide words
/// are cut at the right edge and a final line taller than the
/// remaining height is cut at the bottom.  Lines that start fully
/// below the box are skipped entirely.  Use `axl_ttf_measure_box`
/// first if you need to size the box to the text.
///
/// (@a x, @a y) is the box's top-left corner and may be negative.
///
/// @return AXL_OK on success.  AXL_ERR if @a ttf is NULL, @a utf8 is
///         NULL, @a px_size is <= 0, the active target is the screen
///         and GOP is unavailable, or the clip stack is full.
int
axl_ttf_draw_box(
    AxlTtf       *ttf,          ///< [in] font
    int32_t       x,            ///< box left edge (may be negative)
    int32_t       y,            ///< box top edge (may be negative)
    uint32_t      w,            ///< box width in pixels (wrap width)
    uint32_t      h,            ///< box height in pixels (vertical clip)
    const char   *utf8,         ///< [in] NUL-terminated UTF-8 string
    float         px_size,      ///< pixel height
    AxlGfxPixel   color,        ///< text color (alpha honored on buffer targets)
    uint32_t      flags         ///< AXL_TTF_ALIGN_* (other bits reserved, pass 0)
    );

/// Measure word-wrapped UTF-8 text for layout-before-draw.
///
/// Runs the identical line-breaking algorithm as `axl_ttf_draw_box`
/// for wrap width @a w (the same hard-break, word-wrap, over-wide-word
/// rules) without drawing, and reports the resulting block geometry.
/// Alignment does not affect geometry, so there is no @a flags
/// argument.
///
/// Outputs (each pointer may be NULL to skip it):
///   - @a out_width:  widest rendered line in pixels.  May exceed @a w
///     when an unbreakable word overflows; 0 for empty input.
///   - @a out_height: total block height in pixels, rounded up —
///     `ceil(line_count * (ascent + descent + line_gap))`.  0 for
///     empty input.
///   - @a out_lines:  number of rendered lines (including blank lines
///     from `\n`).  0 for empty input ("" or NULL-equivalent yields
///     no lines).
///
/// Does NOT require GOP — pure measurement.
///
/// @return AXL_OK on success (outputs written).  AXL_ERR if @a ttf is
///         NULL, @a utf8 is NULL, or @a px_size is <= 0 (outputs
///         untouched on error).
int
axl_ttf_measure_box(
    AxlTtf       *ttf,          ///< [in] font
    uint32_t      w,            ///< wrap width in pixels
    const char   *utf8,         ///< [in] NUL-terminated UTF-8 string
    float         px_size,      ///< pixel height
    uint32_t     *out_width,    ///< [out] widest line width, px (NULL OK)
    uint32_t     *out_height,   ///< [out] total block height, px (NULL OK)
    uint32_t     *out_lines     ///< [out] rendered line count (NULL OK)
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
