/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-display-list.h
    AxlGfx display list (Phase G9): a retained, replayable command
    buffer.  Record a sequence of draw operations into an
    `AxlGfxDisplayList`, then replay them against the active draw
    target as many times as you like, or walk them for trace / debug.

    A display list decouples *what* to draw from *when* to draw it.
    The classic uses are double-buffered retained rendering (record a
    widget tree once, replay every frame), draw-call tracing (assert
    that a widget emitted exactly the expected primitives — superseding
    a hand-rolled recording fixture), and deferred / reordered
    rendering.

    The recorder is explicit: every immediate-mode `axl_gfx_*` draw
    primitive has an `axl_gfx_dl_*` analogue that appends an op instead
    of drawing.  `axl_gfx_display_list_replay` then invokes the real
    primitive for each recorded op, in order.

    @code
    AxlGfxDisplayList *dl = axl_gfx_display_list_new();
    axl_gfx_dl_clear(dl, AXL_GFX_BLACK);
    axl_gfx_dl_fill_rect(dl, 0, 0, 100, 40, AXL_GFX_BLUE);
    axl_gfx_dl_fill_path(dl, glyph_path, AXL_GFX_WHITE);

    // Replay every frame:
    axl_gfx_display_list_replay(dl);

    // Or introspect (e.g. in a test):
    const AxlGfxOp *op = axl_gfx_display_list_op_at(dl, 1);
    // op->kind == AXL_GFX_OP_FILL_RECT, op->u.rect_u.w == 100, ...

    axl_gfx_display_list_free(dl);
    @endcode

    **Ownership.**  The display list copies by-value op data and the
    transient arrays it is handed — polyline point arrays, blit pixel
    buffers, text strings, and stroke dash arrays — so the caller's
    originals may be freed immediately after recording.  It does NOT
    copy the *handle*-typed objects, which have their own lifecycles
    and are expected to outlive the list: `AxlGfxPath` (filled / stroked
    by reference, matching `axl_gfx_path_free` ownership), `AxlFont`,
    and `AxlTtf`.  Those must stay valid until the last replay / free.

    Covers the immediate-mode primitives an `AgtDrawContext` forwards,
    plus gradient fills and the full transform stack (translate / scale
    / rotate / skew / push / pop / reset).  A textual `dump`
    serialization is the one tracked follow-up.
**/

#ifndef AXL_GFX_DISPLAY_LIST_H
#define AXL_GFX_DISPLAY_LIST_H

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-macros.h>
#include <axl/axl-gfx-types.h>
#include <axl/axl-gfx-surface.h>   /* AxlGfxClip */
#include <axl/axl-gfx-draw.h>      /* AxlGfxPoint, AxlFont */
#include <axl/axl-gfx-path.h>      /* AxlGfxPath, AxlGfxStrokeStyle */
#include <axl/axl-truetype.h>      /* AxlTtf */

#ifdef __cplusplus
extern "C" {
#endif

/// Forward declaration — `axl_gfx_display_list_dump` writes to an
/// `AxlStream` without this header pulling in the whole stream API.
/// Callers using the dump include `<axl/axl-stream.h>` themselves.
typedef struct AxlStream AxlStream;

// ===================================================================
// Types
// ===================================================================

/// Opaque, growable, replayable list of recorded draw operations.
/// Created with `axl_gfx_display_list_new`; freed with
/// `axl_gfx_display_list_free`.
typedef struct AxlGfxDisplayList AxlGfxDisplayList;

/// Discriminant for `AxlGfxOp` — which immediate-mode primitive the
/// op records.  Stable enough to switch on for replay / trace.
typedef enum {
    AXL_GFX_OP_FILL_RECT,           ///< axl_gfx_fill_rect (u.rect_u)
    AXL_GFX_OP_FILL_RECT_I,         ///< axl_gfx_fill_rect_i (u.rect_i)
    AXL_GFX_OP_DRAW_LINE,           ///< axl_gfx_draw_line (u.line)
    AXL_GFX_OP_DRAW_RECT,           ///< axl_gfx_draw_rect (u.rect_u)
    AXL_GFX_OP_DRAW_POLYLINE,       ///< axl_gfx_draw_polyline (u.polyline)
    AXL_GFX_OP_BLIT,                ///< axl_gfx_blit (u.blit)
    AXL_GFX_OP_CLEAR,               ///< clear active target (u.clear)
    AXL_GFX_OP_PUSH_CLIP,           ///< axl_gfx_push_clip (u.push_clip)
    AXL_GFX_OP_POP_CLIP,            ///< axl_gfx_pop_clip (no payload)
    AXL_GFX_OP_FILL_PATH,           ///< axl_gfx_fill_path (u.fill_path)
    AXL_GFX_OP_STROKE_PATH,         ///< axl_gfx_stroke_path_ex (u.stroke_path)
    AXL_GFX_OP_FILL_ROUNDED_RECT,   ///< axl_gfx_fill_rounded_rect (u.rounded_rect)
    AXL_GFX_OP_DRAW_TEXT,           ///< axl_gfx_draw_text (u.text)
    AXL_GFX_OP_DRAW_TEXT_TTF,       ///< axl_ttf_draw (u.text_ttf)
    AXL_GFX_OP_FILL_RECT_GRADIENT,  ///< axl_gfx_fill_rect_gradient (u.rect_gradient)
    AXL_GFX_OP_FILL_PATH_GRADIENT,  ///< axl_gfx_fill_path_gradient (u.path_gradient)
    AXL_GFX_OP_FILL_ROUNDED_RECT_GRADIENT, ///< axl_gfx_fill_rounded_rect_gradient (u.rounded_rect_gradient)
    AXL_GFX_OP_TRANSLATE,           ///< axl_gfx_translate (u.translate)
    AXL_GFX_OP_SCALE,               ///< axl_gfx_scale (u.scale)
    AXL_GFX_OP_ROTATE,              ///< axl_gfx_rotate (u.rotate)
    AXL_GFX_OP_SKEW,                ///< axl_gfx_skew (u.skew)
    AXL_GFX_OP_PUSH_TRANSFORM,      ///< axl_gfx_push_transform (no payload)
    AXL_GFX_OP_POP_TRANSFORM,       ///< axl_gfx_pop_transform (no payload)
    AXL_GFX_OP_RESET_TRANSFORM,     ///< axl_gfx_reset_transform (no payload)
} AxlGfxOpKind;

/// One recorded draw operation.  @a kind selects the active union
/// member.  All pointers reachable from an op are owned by the
/// display list and stay valid until `axl_gfx_display_list_clear` /
/// `_free` — EXCEPT the borrowed handle fields (`fill_path.path`,
/// `stroke_path.path`, `text.font`, `text_ttf.ttf`), which the caller
/// owns and must keep alive.  Returned by `axl_gfx_display_list_op_at`
/// for read-only introspection; do not mutate or free its fields.
typedef struct {
    AxlGfxOpKind  kind;   ///< which primitive this op records
    union {
        /// FILL_RECT / DRAW_RECT — unsigned-coordinate rectangle.
        struct {
            uint32_t     x, y, w, h;
            AxlGfxPixel  color;
        } rect_u;
        /// FILL_RECT_I — signed-coordinate rectangle.
        struct {
            int32_t      x, y, w, h;
            AxlGfxPixel  color;
        } rect_i;
        /// DRAW_LINE — segment endpoints (inclusive).
        struct {
            int32_t      x0, y0, x1, y1;
            AxlGfxPixel  color;
        } line;
        /// DRAW_POLYLINE — owned copy of the point array.
        struct {
            AxlGfxPoint *points;   ///< owned (freed with the list)
            size_t       count;
            AxlGfxPixel  color;
        } polyline;
        /// BLIT — owned copy of the source pixels.
        struct {
            AxlGfxPixel *pixels;   ///< owned (freed with the list), w*h pixels
            uint32_t     x, y, w, h;
        } blit;
        /// CLEAR — fill the active target with this color.
        struct {
            AxlGfxPixel  color;
        } clear;
        /// PUSH_CLIP — clip rectangle to intersect onto the stack.
        struct {
            AxlGfxClip   rect;
        } push_clip;
        /// FILL_PATH — borrowed path + fill color.
        struct {
            const AxlGfxPath *path;   ///< borrowed (caller-owned)
            AxlGfxPixel       color;
        } fill_path;
        /// STROKE_PATH — borrowed path + color + style (with an owned
        /// copy of the style's dash array, if any).
        struct {
            const AxlGfxPath  *path;   ///< borrowed (caller-owned)
            AxlGfxPixel        color;
            AxlGfxStrokeStyle  style;  ///< style.dashes is owned by the list
        } stroke_path;
        /// FILL_ROUNDED_RECT — signed rect + corner radius.
        struct {
            int32_t      x, y, w, h;
            float        radius;
            AxlGfxPixel  color;
        } rounded_rect;
        /// DRAW_TEXT — bitmap-font text (owned string, borrowed font).
        struct {
            const AxlFont *font;    ///< borrowed (caller-owned)
            uint32_t       x, y;
            char          *text;    ///< owned NUL-terminated copy
            AxlGfxPixel    color;
            uint32_t       scale;
        } text;
        /// DRAW_TEXT_TTF — vector text (owned string, borrowed font).
        struct {
            AxlTtf       *ttf;      ///< borrowed (caller-owned)
            int32_t       x, y;     ///< baseline origin
            char         *text;     ///< owned NUL-terminated copy
            float         px_size;
            AxlGfxPixel   color;
        } text_ttf;
        /// FILL_RECT_GRADIENT — signed rect filled with a gradient.
        struct {
            int32_t               x, y, w, h;
            const AxlGfxGradient *g;   ///< borrowed (caller-owned)
        } rect_gradient;
        /// FILL_PATH_GRADIENT — borrowed path filled with a gradient.
        struct {
            const AxlGfxPath     *path;   ///< borrowed (caller-owned)
            const AxlGfxGradient *g;      ///< borrowed (caller-owned)
        } path_gradient;
        /// FILL_ROUNDED_RECT_GRADIENT — signed rounded rect + gradient.
        struct {
            int32_t               x, y, w, h;
            float                 radius;
            const AxlGfxGradient *g;   ///< borrowed (caller-owned)
        } rounded_rect_gradient;
        /// TRANSLATE — append a translation to the active transform.
        struct {
            double  tx, ty;
        } translate;
        /// SCALE — append a non-uniform scale.
        struct {
            double  sx, sy;
        } scale;
        /// ROTATE — append a rotation (radians).
        struct {
            double  radians;
        } rotate;
        /// SKEW — append a 2D shear.
        struct {
            double  sx, sy;
        } skew;
        /* PUSH_TRANSFORM / POP_TRANSFORM / RESET_TRANSFORM carry no
         * payload. */
    } u;
} AxlGfxOp;

// ===================================================================
// Lifecycle
// ===================================================================

/// Allocate an empty display list.
///
/// @return new list (caller frees with `axl_gfx_display_list_free`),
///         or NULL on allocation failure.
AxlGfxDisplayList *
axl_gfx_display_list_new(void);

/// Free a display list and every op it owns (point arrays, blit
/// pixels, text strings, dash arrays).  Borrowed handles
/// (paths / fonts / ttf) are NOT freed.  Safe to call with NULL.
void
axl_gfx_display_list_free(
    AxlGfxDisplayList  *dl   ///< list to free, or NULL
    );

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlGfxDisplayList, axl_gfx_display_list_free)
#endif

/// Remove all recorded ops, freeing their owned data, while retaining
/// the list's allocation for reuse.  Cheaper than free + new for a
/// list rebuilt every frame.  Safe to call with NULL.
void
axl_gfx_display_list_clear(
    AxlGfxDisplayList  *dl   ///< list to reset, or NULL
    );

// ===================================================================
// Introspection
// ===================================================================

/// Number of ops recorded in @a dl.
///
/// @return op count, or 0 if @a dl is NULL.
size_t
axl_gfx_display_list_count(
    const AxlGfxDisplayList  *dl   ///< [in] list
    );

/// Borrow a read-only pointer to the op at @a index.
///
/// The pointer is valid until the next mutation of @a dl
/// (`axl_gfx_dl_*`, `_clear`, `_free`) — recording may reallocate the
/// backing store.  Do not free or mutate the returned op.
///
/// @return pointer to the op, or NULL if @a dl is NULL or @a index is
///         out of range.
const AxlGfxOp *
axl_gfx_display_list_op_at(
    const AxlGfxDisplayList  *dl,     ///< [in] list
    size_t                    index   ///< 0-based op index
    );

// ===================================================================
// Recording — one appender per immediate-mode primitive
// ===================================================================
//
// Each returns AXL_OK on success, or AXL_ERR if @a dl is NULL, a
// required argument is invalid (mirroring the immediate-mode
// primitive's own validation), or an allocation failed.  On AXL_ERR
// nothing is appended.

/// Record `axl_gfx_fill_rect`.
int
axl_gfx_dl_fill_rect(
    AxlGfxDisplayList  *dl,
    uint32_t            x,
    uint32_t            y,
    uint32_t            w,
    uint32_t            h,
    AxlGfxPixel         color
    );

/// Record `axl_gfx_fill_rect_i` (signed coordinates).
int
axl_gfx_dl_fill_rect_i(
    AxlGfxDisplayList  *dl,
    int32_t             x,
    int32_t             y,
    int32_t             w,
    int32_t             h,
    AxlGfxPixel         color
    );

/// Record `axl_gfx_draw_line`.
int
axl_gfx_dl_draw_line(
    AxlGfxDisplayList  *dl,
    int32_t             x0,
    int32_t             y0,
    int32_t             x1,
    int32_t             y1,
    AxlGfxPixel         color
    );

/// Record `axl_gfx_draw_rect`.
int
axl_gfx_dl_draw_rect(
    AxlGfxDisplayList  *dl,
    uint32_t            x,
    uint32_t            y,
    uint32_t            w,
    uint32_t            h,
    AxlGfxPixel         color
    );

/// Record `axl_gfx_draw_polyline`.  Copies @a count points.
///
/// AXL_ERR if @a points is NULL or @a count < 2 (matching
/// `axl_gfx_draw_polyline`).
int
axl_gfx_dl_draw_polyline(
    AxlGfxDisplayList  *dl,       ///< target display list
    const AxlGfxPoint  *points,   ///< [in] copied into the list
    size_t              count,    ///< number of points (>= 2)
    AxlGfxPixel         color     ///< line color
    );

/// Record `axl_gfx_blit`.  Copies @a w * @a h pixels from @a buffer.
///
/// AXL_ERR if @a buffer is NULL or @a w / @a h is 0.
int
axl_gfx_dl_blit(
    AxlGfxDisplayList  *dl,       ///< target display list
    const AxlGfxPixel  *buffer,   ///< [in] copied into the list
    uint32_t            x,        ///< destination x (top-left)
    uint32_t            y,        ///< destination y (top-left)
    uint32_t            w,        ///< source width in pixels
    uint32_t            h         ///< source height in pixels
    );

/// Record a clear of the active draw target to @a color.
///
/// On replay this clears whatever target is active: an off-screen
/// buffer via `axl_gfx_buffer_clear`, or the full screen extent when
/// rendering to the screen.
int
axl_gfx_dl_clear(
    AxlGfxDisplayList  *dl,
    AxlGfxPixel         color
    );

/// Record `axl_gfx_push_clip`.
int
axl_gfx_dl_push_clip(
    AxlGfxDisplayList  *dl,
    AxlGfxClip          rect
    );

/// Record `axl_gfx_pop_clip`.
int
axl_gfx_dl_pop_clip(
    AxlGfxDisplayList  *dl
    );

/// Record `axl_gfx_fill_path`.  Borrows @a path (caller keeps it
/// alive until the last replay / free).
///
/// AXL_ERR if @a path is NULL.
int
axl_gfx_dl_fill_path(
    AxlGfxDisplayList  *dl,      ///< target display list
    const AxlGfxPath   *path,    ///< borrowed (caller-owned)
    AxlGfxPixel         color    ///< fill color
    );

/// Record `axl_gfx_stroke_path_ex`.  Borrows @a path; copies @a style
/// by value plus an owned copy of its dash array (if any).
///
/// AXL_ERR if @a path or @a style is NULL, or on allocation failure.
int
axl_gfx_dl_stroke_path(
    AxlGfxDisplayList        *dl,      ///< target display list
    const AxlGfxPath         *path,    ///< borrowed (caller-owned)
    AxlGfxPixel               color,   ///< stroke color
    const AxlGfxStrokeStyle  *style    ///< [in] copied (dashes deep-copied)
    );

/// Record `axl_gfx_fill_rounded_rect`.
int
axl_gfx_dl_fill_rounded_rect(
    AxlGfxDisplayList  *dl,
    int32_t             x,
    int32_t             y,
    int32_t             w,
    int32_t             h,
    float               radius,
    AxlGfxPixel         color
    );

/// Record `axl_gfx_draw_text` (bitmap font).  Borrows @a font; copies
/// @a text.
///
/// AXL_ERR if @a font or @a text is NULL.
int
axl_gfx_dl_draw_text(
    AxlGfxDisplayList  *dl,      ///< target display list
    const AxlFont      *font,    ///< borrowed (caller-owned)
    uint32_t            x,       ///< pen x (top-left of the text)
    uint32_t            y,       ///< pen y (top-left of the text)
    const char         *text,    ///< [in] copied into the list
    AxlGfxPixel         color,   ///< text color
    uint32_t            scale    ///< integer pixel scale (1 = native)
    );

/// Record `axl_ttf_draw` (vector text).  Borrows @a ttf; copies
/// @a text.  (@a x, @a y) is the baseline origin, as in `axl_ttf_draw`.
///
/// AXL_ERR if @a ttf or @a text is NULL.
int
axl_gfx_dl_draw_text_ttf(
    AxlGfxDisplayList  *dl,      ///< target display list
    AxlTtf             *ttf,     ///< borrowed (caller-owned)
    int32_t             x,       ///< baseline origin x
    int32_t             y,       ///< baseline origin y
    const char         *text,    ///< [in] copied into the list
    float               px_size, ///< text size in pixels (em height)
    AxlGfxPixel         color    ///< text color
    );

// ===================================================================
// Recording — gradient fills (borrow the gradient)
// ===================================================================

/// Record `axl_gfx_fill_rect_gradient`.  Borrows @a g.
///
/// AXL_ERR if @a dl or @a g is NULL.
int
axl_gfx_dl_fill_rect_gradient(
    AxlGfxDisplayList     *dl,     ///< target display list
    int32_t                x,      ///< rect x (top-left)
    int32_t                y,      ///< rect y (top-left)
    int32_t                w,      ///< rect width
    int32_t                h,      ///< rect height
    const AxlGfxGradient  *g       ///< borrowed (caller-owned)
    );

/// Record `axl_gfx_fill_path_gradient`.  Borrows @a path and @a g.
///
/// AXL_ERR if @a dl, @a path, or @a g is NULL.
int
axl_gfx_dl_fill_path_gradient(
    AxlGfxDisplayList     *dl,      ///< target display list
    const AxlGfxPath      *path,    ///< borrowed (caller-owned)
    const AxlGfxGradient  *g        ///< borrowed (caller-owned)
    );

/// Record `axl_gfx_fill_rounded_rect_gradient`.  Borrows @a g.
///
/// AXL_ERR if @a dl or @a g is NULL.
int
axl_gfx_dl_fill_rounded_rect_gradient(
    AxlGfxDisplayList     *dl,      ///< target display list
    int32_t                x,       ///< rect x (top-left)
    int32_t                y,       ///< rect y (top-left)
    int32_t                w,       ///< rect width
    int32_t                h,       ///< rect height
    float                  radius,  ///< corner radius in pixels
    const AxlGfxGradient  *g        ///< borrowed (caller-owned)
    );

// ===================================================================
// Recording — transform stack (all by value)
// ===================================================================

/// Record `axl_gfx_translate`.
int
axl_gfx_dl_translate(
    AxlGfxDisplayList  *dl,
    double              tx,
    double              ty
    );

/// Record `axl_gfx_scale`.
int
axl_gfx_dl_scale(
    AxlGfxDisplayList  *dl,
    double              sx,
    double              sy
    );

/// Record `axl_gfx_rotate` (radians).
int
axl_gfx_dl_rotate(
    AxlGfxDisplayList  *dl,
    double              radians
    );

/// Record `axl_gfx_skew`.
int
axl_gfx_dl_skew(
    AxlGfxDisplayList  *dl,
    double              sx,
    double              sy
    );

/// Record `axl_gfx_push_transform`.
int
axl_gfx_dl_push_transform(
    AxlGfxDisplayList  *dl
    );

/// Record `axl_gfx_pop_transform`.
int
axl_gfx_dl_pop_transform(
    AxlGfxDisplayList  *dl
    );

/// Record `axl_gfx_reset_transform`.
int
axl_gfx_dl_reset_transform(
    AxlGfxDisplayList  *dl
    );

// ===================================================================
// Replay
// ===================================================================

/// Replay every recorded op against the active draw target, in order,
/// by invoking the corresponding immediate-mode primitive.
///
/// Replays the whole list even if an individual op fails (e.g. GOP
/// unavailable on a screen target).
///
/// @return AXL_OK if @a dl is non-NULL and every op succeeded;
///         AXL_ERR if @a dl is NULL or any op's primitive reported
///         failure.
int
axl_gfx_display_list_replay(
    const AxlGfxDisplayList  *dl   ///< [in] list to replay
    );

// ===================================================================
// Textual dump (trace / debug)
// ===================================================================

/// Write a human-readable, line-per-op textual trace of @a dl to
/// @a out.
///
/// Each recorded op produces exactly one `\n`-terminated line of the
/// form `<index>: <op-name> <field>=<value> ...`, in record order.
/// The format is for debugging / golden-trace tests, not a
/// round-trippable serialization (there is no loader) — it does not
/// emit borrowed-handle contents (a path's vertices, a gradient's
/// stops, a font's glyphs), only that the op references one.
///
/// Conventions:
///   - colors print as `#RRGGBBAA` (8 hex digits, upper-case);
///   - floats (width, radius, px, transform args) print with 3
///     decimal places;
///   - text strings print double-quoted with `\\`, `\"`, `\n`, `\r`,
///     and `\t` backslash-escaped so each op stays on one line.
///
/// An empty list writes nothing and returns AXL_OK.
///
/// @return AXL_OK on success.  AXL_ERR if @a dl or @a out is NULL, or
///         a write to @a out fails.
int
axl_gfx_display_list_dump(
    const AxlGfxDisplayList  *dl,    ///< [in] list to dump
    AxlStream                *out    ///< [in] destination stream
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_DISPLAY_LIST_H */
