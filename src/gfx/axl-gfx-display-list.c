/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-display-list.c
    AxlGfx display list (Phase G9) — a retained, replayable command
    buffer.  See <axl/axl-gfx-display-list.h> for the contract.

    Ops are stored in an AxlArray (value mode, element = AxlGfxOp), so
    appending is amortized O(1) and `op_at` is a direct index.  Each
    recorder copies the transient data it is handed (point arrays, blit
    pixels, text strings, dash arrays) and borrows the handle-typed
    objects (paths, fonts, ttf).  Cleanup walks the ops once to free
    the owned copies before the array itself is dropped.
**/

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-array.h>
#include <axl/axl-gfx.h>
#include <axl/axl-gfx-display-list.h>
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>

// ===================================================================
// Type
// ===================================================================

struct AxlGfxDisplayList {
    AxlArray  *ops;   ///< value-mode array of AxlGfxOp
};

// ===================================================================
// Owned-data cleanup
// ===================================================================

/* Free the heap data an op owns (if any).  Borrowed handles
 * (path / font / ttf) are never freed here. */
static void
op_free_owned(
    AxlGfxOp  *op
    )
{
    switch (op->kind) {
    case AXL_GFX_OP_DRAW_POLYLINE:
        axl_free(op->u.polyline.points);
        break;
    case AXL_GFX_OP_BLIT:
        axl_free(op->u.blit.pixels);
        break;
    case AXL_GFX_OP_STROKE_PATH:
        /* style.dashes is an owned copy (const-qualified in the public
         * struct; we allocated it, so the cast-away is sound). */
        axl_free((void *)(uintptr_t)op->u.stroke_path.style.dashes);
        break;
    case AXL_GFX_OP_DRAW_TEXT:
        axl_free(op->u.text.text);
        break;
    case AXL_GFX_OP_DRAW_TEXT_TTF:
        axl_free(op->u.text_ttf.text);
        break;
    default:
        break;
    }
}

/* Free every op's owned data.  Used by both clear and free. */
static void
dl_free_all_owned(
    AxlGfxDisplayList  *dl
    )
{
    size_t n = axl_array_len(dl->ops);
    for (size_t i = 0; i < n; i++) {
        op_free_owned(axl_array_get(dl->ops, i));
    }
}

// ===================================================================
// Lifecycle
// ===================================================================

AxlGfxDisplayList *
axl_gfx_display_list_new(void)
{
    AxlGfxDisplayList *dl = axl_malloc(sizeof *dl);
    if (!dl) {
        return NULL;
    }
    dl->ops = axl_array_new(sizeof(AxlGfxOp));
    if (!dl->ops) {
        axl_free(dl);
        return NULL;
    }
    return dl;
}

void
axl_gfx_display_list_free(
    AxlGfxDisplayList  *dl
    )
{
    if (!dl) {
        return;
    }
    dl_free_all_owned(dl);
    axl_array_free(dl->ops);
    axl_free(dl);
}

void
axl_gfx_display_list_clear(
    AxlGfxDisplayList  *dl
    )
{
    if (!dl) {
        return;
    }
    dl_free_all_owned(dl);
    axl_array_clear(dl->ops);
}

// ===================================================================
// Introspection
// ===================================================================

size_t
axl_gfx_display_list_count(
    const AxlGfxDisplayList  *dl
    )
{
    return dl ? axl_array_len(dl->ops) : 0;
}

const AxlGfxOp *
axl_gfx_display_list_op_at(
    const AxlGfxDisplayList  *dl,
    size_t                    index
    )
{
    if (!dl) {
        return NULL;
    }
    return axl_array_get(dl->ops, index);
}

// ===================================================================
// Recording helpers
// ===================================================================

/* Append @a op (by value) to @a dl.  Returns AXL_OK / AXL_ERR. */
static int
dl_push(
    AxlGfxDisplayList  *dl,
    const AxlGfxOp     *op
    )
{
    return axl_array_append(dl->ops, op);
}

// ===================================================================
// Recording — by-value primitives
// ===================================================================

int
axl_gfx_dl_fill_rect(
    AxlGfxDisplayList  *dl,
    uint32_t            x,
    uint32_t            y,
    uint32_t            w,
    uint32_t            h,
    AxlGfxPixel         color
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_FILL_RECT,
                    .u.rect_u = { x, y, w, h, color } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_fill_rect_i(
    AxlGfxDisplayList  *dl,
    int32_t             x,
    int32_t             y,
    int32_t             w,
    int32_t             h,
    AxlGfxPixel         color
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_FILL_RECT_I,
                    .u.rect_i = { x, y, w, h, color } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_draw_line(
    AxlGfxDisplayList  *dl,
    int32_t             x0,
    int32_t             y0,
    int32_t             x1,
    int32_t             y1,
    AxlGfxPixel         color
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_DRAW_LINE,
                    .u.line = { x0, y0, x1, y1, color } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_draw_rect(
    AxlGfxDisplayList  *dl,
    uint32_t            x,
    uint32_t            y,
    uint32_t            w,
    uint32_t            h,
    AxlGfxPixel         color
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_DRAW_RECT,
                    .u.rect_u = { x, y, w, h, color } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_clear(
    AxlGfxDisplayList  *dl,
    AxlGfxPixel         color
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_CLEAR, .u.clear = { color } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_push_clip(
    AxlGfxDisplayList  *dl,
    AxlGfxClip          rect
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_PUSH_CLIP, .u.push_clip = { rect } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_pop_clip(
    AxlGfxDisplayList  *dl
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_POP_CLIP };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_fill_rounded_rect(
    AxlGfxDisplayList  *dl,
    int32_t             x,
    int32_t             y,
    int32_t             w,
    int32_t             h,
    float               radius,
    AxlGfxPixel         color
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_FILL_ROUNDED_RECT,
                    .u.rounded_rect = { x, y, w, h, radius, color } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_fill_path(
    AxlGfxDisplayList  *dl,
    const AxlGfxPath   *path,
    AxlGfxPixel         color
    )
{
    if (!dl || !path) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_FILL_PATH,
                    .u.fill_path = { path, color } };
    return dl_push(dl, &op);
}

// ===================================================================
// Recording — primitives that copy transient data
// ===================================================================

int
axl_gfx_dl_draw_polyline(
    AxlGfxDisplayList  *dl,
    const AxlGfxPoint  *points,
    size_t              count,
    AxlGfxPixel         color
    )
{
    if (!dl || !points || count < 2) {
        return AXL_ERR;
    }
    size_t bytes = count * sizeof(AxlGfxPoint);
    AxlGfxPoint *copy = axl_malloc(bytes);
    if (!copy) {
        return AXL_ERR;
    }
    axl_memcpy(copy, points, bytes);

    AxlGfxOp op = { .kind = AXL_GFX_OP_DRAW_POLYLINE,
                    .u.polyline = { copy, count, color } };
    if (dl_push(dl, &op) != AXL_OK) {
        axl_free(copy);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_gfx_dl_blit(
    AxlGfxDisplayList  *dl,
    const AxlGfxPixel  *buffer,
    uint32_t            x,
    uint32_t            y,
    uint32_t            w,
    uint32_t            h
    )
{
    if (!dl || !buffer || w == 0 || h == 0) {
        return AXL_ERR;
    }
    size_t bytes = (size_t)w * (size_t)h * sizeof(AxlGfxPixel);
    AxlGfxPixel *copy = axl_malloc(bytes);
    if (!copy) {
        return AXL_ERR;
    }
    axl_memcpy(copy, buffer, bytes);

    AxlGfxOp op = { .kind = AXL_GFX_OP_BLIT,
                    .u.blit = { copy, x, y, w, h } };
    if (dl_push(dl, &op) != AXL_OK) {
        axl_free(copy);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_gfx_dl_stroke_path(
    AxlGfxDisplayList        *dl,
    const AxlGfxPath         *path,
    AxlGfxPixel               color,
    const AxlGfxStrokeStyle  *style
    )
{
    if (!dl || !path || !style) {
        return AXL_ERR;
    }

    AxlGfxOp op = { .kind = AXL_GFX_OP_STROKE_PATH };
    op.u.stroke_path.path  = path;
    op.u.stroke_path.color = color;
    op.u.stroke_path.style = *style;   /* by value */

    /* Deep-copy the dash array so the caller's (often stack/transient)
     * pattern need not outlive the recording. */
    if (style->dashes && style->n_dashes > 0) {
        size_t bytes = style->n_dashes * sizeof(float);
        float *dashes = axl_malloc(bytes);
        if (!dashes) {
            return AXL_ERR;
        }
        axl_memcpy(dashes, style->dashes, bytes);
        op.u.stroke_path.style.dashes = dashes;
    } else {
        op.u.stroke_path.style.dashes   = NULL;
        op.u.stroke_path.style.n_dashes = 0;
    }

    if (dl_push(dl, &op) != AXL_OK) {
        axl_free((void *)(uintptr_t)op.u.stroke_path.style.dashes);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_gfx_dl_draw_text(
    AxlGfxDisplayList  *dl,
    const AxlFont      *font,
    uint32_t            x,
    uint32_t            y,
    const char         *text,
    AxlGfxPixel         color,
    uint32_t            scale
    )
{
    if (!dl || !font || !text) {
        return AXL_ERR;
    }
    char *copy = axl_strdup(text);
    if (!copy) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_DRAW_TEXT };
    op.u.text.font  = font;
    op.u.text.x     = x;
    op.u.text.y     = y;
    op.u.text.text  = copy;
    op.u.text.color = color;
    op.u.text.scale = scale;
    if (dl_push(dl, &op) != AXL_OK) {
        axl_free(copy);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_gfx_dl_draw_text_ttf(
    AxlGfxDisplayList  *dl,
    AxlTtf             *ttf,
    int32_t             x,
    int32_t             y,
    const char         *text,
    float               px_size,
    AxlGfxPixel         color
    )
{
    if (!dl || !ttf || !text) {
        return AXL_ERR;
    }
    char *copy = axl_strdup(text);
    if (!copy) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_DRAW_TEXT_TTF };
    op.u.text_ttf.ttf     = ttf;
    op.u.text_ttf.x       = x;
    op.u.text_ttf.y       = y;
    op.u.text_ttf.text    = copy;
    op.u.text_ttf.px_size = px_size;
    op.u.text_ttf.color   = color;
    if (dl_push(dl, &op) != AXL_OK) {
        axl_free(copy);
        return AXL_ERR;
    }
    return AXL_OK;
}

// ===================================================================
// Recording — gradient fills (G9 slice 2; borrow the gradient)
// ===================================================================

int
axl_gfx_dl_fill_rect_gradient(
    AxlGfxDisplayList     *dl,
    int32_t                x,
    int32_t                y,
    int32_t                w,
    int32_t                h,
    const AxlGfxGradient  *g
    )
{
    if (!dl || !g) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_FILL_RECT_GRADIENT,
                    .u.rect_gradient = { x, y, w, h, g } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_fill_path_gradient(
    AxlGfxDisplayList     *dl,
    const AxlGfxPath      *path,
    const AxlGfxGradient  *g
    )
{
    if (!dl || !path || !g) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_FILL_PATH_GRADIENT,
                    .u.path_gradient = { path, g } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_fill_rounded_rect_gradient(
    AxlGfxDisplayList     *dl,
    int32_t                x,
    int32_t                y,
    int32_t                w,
    int32_t                h,
    float                  radius,
    const AxlGfxGradient  *g
    )
{
    if (!dl || !g) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_FILL_ROUNDED_RECT_GRADIENT,
                    .u.rounded_rect_gradient = { x, y, w, h, radius, g } };
    return dl_push(dl, &op);
}

// ===================================================================
// Recording — transform stack (G9 slice 2; all by value)
// ===================================================================

int
axl_gfx_dl_translate(
    AxlGfxDisplayList  *dl,
    double              tx,
    double              ty
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_TRANSLATE, .u.translate = { tx, ty } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_scale(
    AxlGfxDisplayList  *dl,
    double              sx,
    double              sy
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_SCALE, .u.scale = { sx, sy } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_rotate(
    AxlGfxDisplayList  *dl,
    double              radians
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_ROTATE, .u.rotate = { radians } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_skew(
    AxlGfxDisplayList  *dl,
    double              sx,
    double              sy
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_SKEW, .u.skew = { sx, sy } };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_push_transform(
    AxlGfxDisplayList  *dl
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_PUSH_TRANSFORM };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_pop_transform(
    AxlGfxDisplayList  *dl
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_POP_TRANSFORM };
    return dl_push(dl, &op);
}

int
axl_gfx_dl_reset_transform(
    AxlGfxDisplayList  *dl
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    AxlGfxOp op = { .kind = AXL_GFX_OP_RESET_TRANSFORM };
    return dl_push(dl, &op);
}

// ===================================================================
// Replay
// ===================================================================

/* Replay a CLEAR against whatever target is active: an off-screen
 * buffer clears directly; the screen clears its full extent. */
static int
replay_clear(
    AxlGfxPixel  color
    )
{
    AxlGfxBuffer *target = axl_gfx_get_current_target();
    if (target) {
        return axl_gfx_buffer_clear(target, color);
    }
    AxlGfxInfo info;
    if (axl_gfx_get_info(&info) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_gfx_fill_rect(0, 0, info.width, info.height, color);
}

/* Invoke the immediate-mode primitive for a single op. */
static int
replay_op(
    const AxlGfxOp  *op
    )
{
    switch (op->kind) {
    case AXL_GFX_OP_FILL_RECT:
        return axl_gfx_fill_rect(op->u.rect_u.x, op->u.rect_u.y,
                                 op->u.rect_u.w, op->u.rect_u.h,
                                 op->u.rect_u.color);
    case AXL_GFX_OP_FILL_RECT_I:
        return axl_gfx_fill_rect_i(op->u.rect_i.x, op->u.rect_i.y,
                                   op->u.rect_i.w, op->u.rect_i.h,
                                   op->u.rect_i.color);
    case AXL_GFX_OP_DRAW_LINE:
        return axl_gfx_draw_line(op->u.line.x0, op->u.line.y0,
                                 op->u.line.x1, op->u.line.y1,
                                 op->u.line.color);
    case AXL_GFX_OP_DRAW_RECT:
        return axl_gfx_draw_rect(op->u.rect_u.x, op->u.rect_u.y,
                                 op->u.rect_u.w, op->u.rect_u.h,
                                 op->u.rect_u.color);
    case AXL_GFX_OP_DRAW_POLYLINE:
        return axl_gfx_draw_polyline(op->u.polyline.points,
                                     op->u.polyline.count,
                                     op->u.polyline.color);
    case AXL_GFX_OP_BLIT:
        return axl_gfx_blit(op->u.blit.pixels, op->u.blit.x, op->u.blit.y,
                            op->u.blit.w, op->u.blit.h);
    case AXL_GFX_OP_CLEAR:
        return replay_clear(op->u.clear.color);
    case AXL_GFX_OP_PUSH_CLIP:
        return axl_gfx_push_clip(op->u.push_clip.rect);
    case AXL_GFX_OP_POP_CLIP:
        return axl_gfx_pop_clip();
    case AXL_GFX_OP_FILL_PATH:
        return axl_gfx_fill_path(op->u.fill_path.path, op->u.fill_path.color);
    case AXL_GFX_OP_STROKE_PATH:
        return axl_gfx_stroke_path_ex(op->u.stroke_path.path,
                                      op->u.stroke_path.color,
                                      &op->u.stroke_path.style);
    case AXL_GFX_OP_FILL_ROUNDED_RECT:
        return axl_gfx_fill_rounded_rect(op->u.rounded_rect.x,
                                         op->u.rounded_rect.y,
                                         op->u.rounded_rect.w,
                                         op->u.rounded_rect.h,
                                         op->u.rounded_rect.radius,
                                         op->u.rounded_rect.color);
    case AXL_GFX_OP_DRAW_TEXT:
        return axl_gfx_draw_text(op->u.text.font, op->u.text.x, op->u.text.y,
                                 op->u.text.text, op->u.text.color,
                                 op->u.text.scale);
    case AXL_GFX_OP_DRAW_TEXT_TTF:
        return axl_ttf_draw(op->u.text_ttf.ttf, op->u.text_ttf.x,
                            op->u.text_ttf.y, op->u.text_ttf.text,
                            op->u.text_ttf.px_size, op->u.text_ttf.color);
    case AXL_GFX_OP_FILL_RECT_GRADIENT:
        return axl_gfx_fill_rect_gradient(op->u.rect_gradient.x,
                                          op->u.rect_gradient.y,
                                          op->u.rect_gradient.w,
                                          op->u.rect_gradient.h,
                                          op->u.rect_gradient.g);
    case AXL_GFX_OP_FILL_PATH_GRADIENT:
        return axl_gfx_fill_path_gradient(op->u.path_gradient.path,
                                          op->u.path_gradient.g);
    case AXL_GFX_OP_FILL_ROUNDED_RECT_GRADIENT:
        return axl_gfx_fill_rounded_rect_gradient(
            op->u.rounded_rect_gradient.x, op->u.rounded_rect_gradient.y,
            op->u.rounded_rect_gradient.w, op->u.rounded_rect_gradient.h,
            op->u.rounded_rect_gradient.radius, op->u.rounded_rect_gradient.g);
    case AXL_GFX_OP_TRANSLATE:
        axl_gfx_translate(op->u.translate.tx, op->u.translate.ty);
        return AXL_OK;
    case AXL_GFX_OP_SCALE:
        axl_gfx_scale(op->u.scale.sx, op->u.scale.sy);
        return AXL_OK;
    case AXL_GFX_OP_ROTATE:
        axl_gfx_rotate(op->u.rotate.radians);
        return AXL_OK;
    case AXL_GFX_OP_SKEW:
        axl_gfx_skew(op->u.skew.sx, op->u.skew.sy);
        return AXL_OK;
    case AXL_GFX_OP_PUSH_TRANSFORM:
        return axl_gfx_push_transform();
    case AXL_GFX_OP_POP_TRANSFORM:
        return axl_gfx_pop_transform();
    case AXL_GFX_OP_RESET_TRANSFORM:
        axl_gfx_reset_transform();
        return AXL_OK;
    }
    return AXL_ERR;   /* unreachable: every kind handled above */
}

int
axl_gfx_display_list_replay(
    const AxlGfxDisplayList  *dl
    )
{
    if (!dl) {
        return AXL_ERR;
    }
    int    rc = AXL_OK;
    size_t n  = axl_array_len(dl->ops);
    for (size_t i = 0; i < n; i++) {
        if (replay_op(axl_array_get(dl->ops, i)) != AXL_OK) {
            rc = AXL_ERR;
        }
    }
    return rc;
}

// ===================================================================
// Textual dump (trace / debug)
// ===================================================================

/* Write `#RRGGBBAA` (upper-case) for @a c.  AxlGfxPixel stores BGRA;
 * the trace prints natural red,green,blue,alpha order. */
static int
dump_color(
    AxlStream    *out,
    AxlGfxPixel   c
    )
{
    return axl_fprintf(out, "#%02X%02X%02X%02X",
                       c.red, c.green, c.blue, c.alpha) < 0
           ? AXL_ERR : AXL_OK;
}

/* Write @a text double-quoted with \\, \", \n, \r, \t escaped, so the
 * op stays on a single line.  Other control bytes pass through (the
 * trace is for debugging; we only need the line-structure-breaking
 * characters neutralized). */
static int
dump_text(
    AxlStream   *out,
    const char  *text
    )
{
    if (axl_fprintf(out, "\"") < 0) {
        return AXL_ERR;
    }
    for (const char *p = text; *p; p++) {
        char esc = 0;
        switch (*p) {
        case '\\': esc = '\\'; break;
        case '"':  esc = '"';  break;
        case '\n': esc = 'n';  break;
        case '\r': esc = 'r';  break;
        case '\t': esc = 't';  break;
        default:   break;
        }
        int r;
        if (esc) {
            r = axl_fprintf(out, "\\%c", esc);
        } else {
            r = axl_fprintf(out, "%c", *p);
        }
        if (r < 0) {
            return AXL_ERR;
        }
    }
    return axl_fprintf(out, "\"") < 0 ? AXL_ERR : AXL_OK;
}

/* Emit one op's line body (everything after "<index>: ").  Returns
 * AXL_OK / AXL_ERR; a negative axl_fprintf result maps to AXL_ERR. */
static int
dump_op_body(
    AxlStream       *out,
    const AxlGfxOp  *op
    )
{
    switch (op->kind) {
    case AXL_GFX_OP_FILL_RECT:
        if (axl_fprintf(out, "fill_rect x=%u y=%u w=%u h=%u color=",
                        op->u.rect_u.x, op->u.rect_u.y,
                        op->u.rect_u.w, op->u.rect_u.h) < 0) return AXL_ERR;
        return dump_color(out, op->u.rect_u.color);
    case AXL_GFX_OP_DRAW_RECT:
        if (axl_fprintf(out, "draw_rect x=%u y=%u w=%u h=%u color=",
                        op->u.rect_u.x, op->u.rect_u.y,
                        op->u.rect_u.w, op->u.rect_u.h) < 0) return AXL_ERR;
        return dump_color(out, op->u.rect_u.color);
    case AXL_GFX_OP_FILL_RECT_I:
        if (axl_fprintf(out, "fill_rect_i x=%d y=%d w=%d h=%d color=",
                        op->u.rect_i.x, op->u.rect_i.y,
                        op->u.rect_i.w, op->u.rect_i.h) < 0) return AXL_ERR;
        return dump_color(out, op->u.rect_i.color);
    case AXL_GFX_OP_DRAW_LINE:
        if (axl_fprintf(out, "draw_line x0=%d y0=%d x1=%d y1=%d color=",
                        op->u.line.x0, op->u.line.y0,
                        op->u.line.x1, op->u.line.y1) < 0) return AXL_ERR;
        return dump_color(out, op->u.line.color);
    case AXL_GFX_OP_DRAW_POLYLINE:
        if (axl_fprintf(out, "draw_polyline n=%llu color=",
                        (unsigned long long)op->u.polyline.count) < 0)
            return AXL_ERR;
        return dump_color(out, op->u.polyline.color);
    case AXL_GFX_OP_BLIT:
        return axl_fprintf(out, "blit x=%u y=%u w=%u h=%u",
                           op->u.blit.x, op->u.blit.y,
                           op->u.blit.w, op->u.blit.h) < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_CLEAR:
        if (axl_fprintf(out, "clear color=") < 0) return AXL_ERR;
        return dump_color(out, op->u.clear.color);
    case AXL_GFX_OP_PUSH_CLIP:
        return axl_fprintf(out, "push_clip x=%d y=%d w=%u h=%u",
                           op->u.push_clip.rect.x, op->u.push_clip.rect.y,
                           op->u.push_clip.rect.w,
                           op->u.push_clip.rect.h) < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_POP_CLIP:
        return axl_fprintf(out, "pop_clip") < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_FILL_PATH:
        if (axl_fprintf(out, "fill_path color=") < 0) return AXL_ERR;
        return dump_color(out, op->u.fill_path.color);
    case AXL_GFX_OP_STROKE_PATH:
        if (axl_fprintf(out,
                "stroke_path width=%.3f cap=%d join=%d miter=%.3f dashes=%llu color=",
                (double)op->u.stroke_path.style.width,
                (int)op->u.stroke_path.style.cap,
                (int)op->u.stroke_path.style.join,
                (double)op->u.stroke_path.style.miter_limit,
                (unsigned long long)op->u.stroke_path.style.n_dashes) < 0)
            return AXL_ERR;
        return dump_color(out, op->u.stroke_path.color);
    case AXL_GFX_OP_FILL_ROUNDED_RECT:
        if (axl_fprintf(out,
                "fill_rounded_rect x=%d y=%d w=%d h=%d radius=%.3f color=",
                op->u.rounded_rect.x, op->u.rounded_rect.y,
                op->u.rounded_rect.w, op->u.rounded_rect.h,
                (double)op->u.rounded_rect.radius) < 0) return AXL_ERR;
        return dump_color(out, op->u.rounded_rect.color);
    case AXL_GFX_OP_DRAW_TEXT:
        if (axl_fprintf(out, "draw_text x=%u y=%u scale=%u color=",
                        op->u.text.x, op->u.text.y,
                        op->u.text.scale) < 0) return AXL_ERR;
        if (dump_color(out, op->u.text.color) != AXL_OK) return AXL_ERR;
        if (axl_fprintf(out, " text=") < 0) return AXL_ERR;
        return dump_text(out, op->u.text.text);
    case AXL_GFX_OP_DRAW_TEXT_TTF:
        if (axl_fprintf(out, "draw_text_ttf x=%d y=%d px=%.3f color=",
                        op->u.text_ttf.x, op->u.text_ttf.y,
                        (double)op->u.text_ttf.px_size) < 0) return AXL_ERR;
        if (dump_color(out, op->u.text_ttf.color) != AXL_OK) return AXL_ERR;
        if (axl_fprintf(out, " text=") < 0) return AXL_ERR;
        return dump_text(out, op->u.text_ttf.text);
    case AXL_GFX_OP_FILL_RECT_GRADIENT:
        return axl_fprintf(out, "fill_rect_gradient x=%d y=%d w=%d h=%d",
                           op->u.rect_gradient.x, op->u.rect_gradient.y,
                           op->u.rect_gradient.w,
                           op->u.rect_gradient.h) < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_FILL_PATH_GRADIENT:
        return axl_fprintf(out, "fill_path_gradient") < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_FILL_ROUNDED_RECT_GRADIENT:
        return axl_fprintf(out,
                "fill_rounded_rect_gradient x=%d y=%d w=%d h=%d radius=%.3f",
                op->u.rounded_rect_gradient.x, op->u.rounded_rect_gradient.y,
                op->u.rounded_rect_gradient.w, op->u.rounded_rect_gradient.h,
                (double)op->u.rounded_rect_gradient.radius) < 0
                ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_TRANSLATE:
        return axl_fprintf(out, "translate tx=%.3f ty=%.3f",
                           op->u.translate.tx,
                           op->u.translate.ty) < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_SCALE:
        return axl_fprintf(out, "scale sx=%.3f sy=%.3f",
                           op->u.scale.sx,
                           op->u.scale.sy) < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_ROTATE:
        return axl_fprintf(out, "rotate rad=%.3f",
                           op->u.rotate.radians) < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_SKEW:
        return axl_fprintf(out, "skew sx=%.3f sy=%.3f",
                           op->u.skew.sx,
                           op->u.skew.sy) < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_PUSH_TRANSFORM:
        return axl_fprintf(out, "push_transform") < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_POP_TRANSFORM:
        return axl_fprintf(out, "pop_transform") < 0 ? AXL_ERR : AXL_OK;
    case AXL_GFX_OP_RESET_TRANSFORM:
        return axl_fprintf(out, "reset_transform") < 0 ? AXL_ERR : AXL_OK;
    }
    return AXL_ERR;   /* unreachable: every kind handled above */
}

int
axl_gfx_display_list_dump(
    const AxlGfxDisplayList  *dl,
    AxlStream                *out
    )
{
    if (!dl || !out) {
        return AXL_ERR;
    }
    size_t n = axl_array_len(dl->ops);
    for (size_t i = 0; i < n; i++) {
        if (axl_fprintf(out, "%llu: ", (unsigned long long)i) < 0) {
            return AXL_ERR;
        }
        if (dump_op_body(out, axl_array_get(dl->ops, i)) != AXL_OK) {
            return AXL_ERR;
        }
        if (axl_fprintf(out, "\n") < 0) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}
