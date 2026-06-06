/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cursor.c
    Software mouse-cursor compositor (Option C — see
    docs/AXL-Pointer-Cursor-Design.md, validated by the spike branch).

    The cursor is bound to a back-buffer "scene". Moving it touches only
    the cursor region: erase the old position by re-presenting the clean
    scene there, then composite the sprite over the scene at the new
    position into a small scratch buffer and present that. GOP can't
    blend, so the source-over compositing happens in RAM (the scratch
    buffer), and only the finished pixels are presented.
**/

#include <axl/axl-cursor.h>
#include <axl/axl-gfx.h>
#include <axl/axl-mem.h>
#include <axl/axl-macros.h>

struct AxlCursor {
    AxlGfxBuffer  *scene;        // bound back-buffer (NOT owned); NULL in save-under mode
    bool           save_under;   // Option B: no scene, capture/restore screen pixels
    int32_t        scene_w, scene_h;

    AxlGfxBuffer  *under;         // sprite-sized saved screen pixels (owned; save-under only)
    AxlGfxBuffer  *compose;      // sprite-sized scratch (owned)
    AxlGfxPixel   *sprite;       // owned RGBA copy
    int32_t        spr_w, spr_h;
    int32_t        hot_x, hot_y;

    int32_t        x, y;         // hotspot position (clamped to scene)
    bool           visible;      // show/hide intent
    bool           lifted;       // inside a lift/drop bracket
    bool           on_screen;    // currently composited
    int32_t        vx, vy, vw, vh;  // last drawn on-screen rect (for erase)

    // axl_cursor_attach forwarding
    AxlInputCallback  fwd_cb;
    void             *fwd_data;
    // Relative-pointer tracking: the event carries the device's raw
    // accumulated position, so we move by the per-event delta instead of
    // clamping the (possibly far-off-screen) absolute value.
    int32_t           track_x, track_y;   // last event's absolute coords
    bool              tracking;            // track_x/y seeded yet?
};

// --- Built-in arrow (12x19). 'X' outline, '.' fill, ' ' transparent. ---
#define ARROW_W 12
#define ARROW_H 19
static const char *const ARROW_BITS[ARROW_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
};

static int32_t
clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Resize the owned sprite + scratch to w x h. Returns AXL_OK / AXL_ERR.
static int
ensure_sprite_size(AxlCursor *c, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) {
        return AXL_ERR;
    }
    if (c->spr_w != w || c->spr_h != h || c->sprite == NULL) {
        AxlGfxPixel  *ns = axl_malloc((size_t)w * (size_t)h * sizeof(AxlGfxPixel));
        AxlGfxBuffer *nc = axl_gfx_buffer_new((uint32_t)w, (uint32_t)h);
        // Save-under (Option B) needs a second sprite-sized buffer to hold
        // the screen pixels captured under the cursor for restore-on-erase.
        AxlGfxBuffer *nu = c->save_under ? axl_gfx_buffer_new((uint32_t)w, (uint32_t)h)
                                         : NULL;
        if (ns == NULL || nc == NULL || (c->save_under && nu == NULL)) {
            axl_free(ns);
            if (nc != NULL) {
                axl_gfx_buffer_free(nc);
            }
            if (nu != NULL) {
                axl_gfx_buffer_free(nu);
            }
            return AXL_ERR;
        }
        axl_free(c->sprite);
        if (c->compose != NULL) {
            axl_gfx_buffer_free(c->compose);
        }
        if (c->under != NULL) {
            axl_gfx_buffer_free(c->under);
        }
        c->sprite  = ns;
        c->compose = nc;
        c->under   = nu;
        c->spr_w   = w;
        c->spr_h   = h;
    }
    return AXL_OK;
}

static int
load_builtin_arrow(AxlCursor *c)
{
    if (ensure_sprite_size(c, ARROW_W, ARROW_H) != AXL_OK) {
        return AXL_ERR;
    }
    const AxlGfxPixel clear = AXL_GFX_RGBA(0, 0, 0, 0);
    const AxlGfxPixel line  = AXL_GFX_RGBA(0x10, 0x10, 0x10, 0xFF);  // outline
    const AxlGfxPixel fill  = AXL_GFX_RGBA(0xFF, 0xFF, 0xFF, 0xFF);  // body
    for (int32_t j = 0; j < ARROW_H; j++) {
        for (int32_t i = 0; i < ARROW_W; i++) {
            char ch = ARROW_BITS[j][i];
            c->sprite[j * ARROW_W + i] = (ch == 'X') ? line
                                       : (ch == '.') ? fill
                                                     : clear;
        }
    }
    c->hot_x = 0;
    c->hot_y = 0;
    return AXL_OK;
}

// Composite the sprite over the scene at the current position and present
// only the visible sub-rect. Records that rect for the next erase.
static void
draw(AxlCursor *c)
{
    int32_t tlx = c->x - c->hot_x;
    int32_t tly = c->y - c->hot_y;
    int32_t vx  = clampi(tlx, 0, c->scene_w);
    int32_t vy  = clampi(tly, 0, c->scene_h);
    int32_t vx2 = clampi(tlx + c->spr_w, 0, c->scene_w);
    int32_t vy2 = clampi(tly + c->spr_h, 0, c->scene_h);
    int32_t vw  = vx2 - vx;
    int32_t vh  = vy2 - vy;
    if (vw <= 0 || vh <= 0) {            // cursor fully off-screen
        c->on_screen = false;
        c->vw = c->vh = 0;
        return;
    }
    int32_t sx = vx - tlx;               // offset into the sprite
    int32_t sy = vy - tly;

    AxlGfxPixel *comp_px  = axl_gfx_buffer_pixels(c->compose);
    if (comp_px == NULL) {
        return;
    }

    // Source of the pixels under the cursor: the bound scene (Option C),
    // or a fresh capture of the screen (Option B / save-under). In save-
    // under mode the captured pixels also become the restore-on-erase
    // truth, packed at the top-left of `under` (stride = spr_w).
    const AxlGfxPixel *src_px;
    int32_t            src_stride, src_x0, src_y0;
    if (c->save_under) {
        AxlGfxPixel *under_px = axl_gfx_buffer_pixels(c->under);
        if (under_px == NULL) {
            return;
        }
        if (vw == c->spr_w) {            // unclipped rows: one contiguous capture
            if (axl_gfx_capture(under_px, (uint32_t)vx, (uint32_t)vy,
                                (uint32_t)vw, (uint32_t)vh) != AXL_OK) {
                return;
            }
        } else {                         // edge-clipped: capture row by row
            for (int32_t j = 0; j < vh; j++) {
                if (axl_gfx_capture(&under_px[j * c->spr_w], (uint32_t)vx,
                                    (uint32_t)(vy + j), (uint32_t)vw, 1) != AXL_OK) {
                    return;
                }
            }
        }
        src_px = under_px; src_stride = c->spr_w; src_x0 = 0; src_y0 = 0;
    } else {
        AxlGfxPixel *scene_px = axl_gfx_buffer_pixels(c->scene);
        if (scene_px == NULL) {
            return;
        }
        src_px = scene_px; src_stride = c->scene_w; src_x0 = vx; src_y0 = vy;
    }

    for (int32_t j = 0; j < vh; j++) {
        const AxlGfxPixel *srow = &src_px[(src_y0 + j) * src_stride + src_x0];
        const AxlGfxPixel *prow = &c->sprite[(sy + j) * c->spr_w + sx];
        AxlGfxPixel       *crow = &comp_px[j * c->spr_w];   // compose stride = spr_w
        for (int32_t i = 0; i < vw; i++) {
            crow[i] = (prow[i].alpha == 0) ? srow[i]
                                           : axl_gfx_blend(srow[i], prow[i]);
        }
    }
    axl_gfx_buffer_present_rect(c->compose, (uint32_t)vx, (uint32_t)vy,
                               0, 0, (uint32_t)vw, (uint32_t)vh);
    c->vx = vx; c->vy = vy; c->vw = vw; c->vh = vh;
    c->on_screen = true;
}

// Restore the clean pixels over the last drawn cursor rect: the bound
// scene region (Option C), or the saved screen pixels (Option B). The
// saved pixels live at the top-left of `under` (source offset 0,0).
static void
erase(AxlCursor *c)
{
    if (c->on_screen && c->vw > 0 && c->vh > 0) {
        if (c->save_under) {
            axl_gfx_buffer_present_rect(c->under,
                                        (uint32_t)c->vx, (uint32_t)c->vy,
                                        0, 0,
                                        (uint32_t)c->vw, (uint32_t)c->vh);
        } else {
            axl_gfx_buffer_present_rect(c->scene,
                                        (uint32_t)c->vx, (uint32_t)c->vy,
                                        (uint32_t)c->vx, (uint32_t)c->vy,
                                        (uint32_t)c->vw, (uint32_t)c->vh);
        }
    }
    c->on_screen = false;
}

AxlCursor *
axl_cursor_new(AxlGfxBuffer *scene)
{
    uint32_t w = 0, h = 0;
    bool     save_under = (scene == NULL);
    if (save_under) {
        // Direct-to-screen save-under mode: the screen is the "scene", so
        // its dimensions come from the GOP. Needs a live framebuffer.
        if (!axl_gfx_available()) {
            return NULL;
        }
        AxlGfxInfo info;
        axl_gfx_get_info(&info);
        w = info.width;
        h = info.height;
    } else if (axl_gfx_buffer_get_info(scene, &w, &h) != AXL_OK) {
        return NULL;
    }
    if (w == 0 || h == 0) {
        return NULL;
    }
    AxlCursor *c = axl_calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->scene      = scene;
    c->save_under = save_under;
    c->scene_w    = (int32_t)w;
    c->scene_h    = (int32_t)h;
    if (load_builtin_arrow(c) != AXL_OK) {
        axl_free(c);
        return NULL;
    }
    return c;
}

void
axl_cursor_free(AxlCursor *c)
{
    if (c == NULL) {
        return;
    }
    if (c->on_screen) {
        erase(c);
    }
    if (c->compose != NULL) {
        axl_gfx_buffer_free(c->compose);
    }
    if (c->under != NULL) {
        axl_gfx_buffer_free(c->under);
    }
    axl_free(c->sprite);
    axl_free(c);
}

int
axl_cursor_set_image(AxlCursor *c, const AxlGfxBuffer *sprite,
                     int32_t hot_x, int32_t hot_y)
{
    if (c == NULL) {
        return AXL_ERR;
    }
    bool was_on = c->on_screen;
    if (was_on) {
        erase(c);
    }
    int rc;
    if (sprite == NULL) {
        rc = load_builtin_arrow(c);
    } else {
        uint32_t w = 0, h = 0;
        const AxlGfxPixel *px = axl_gfx_buffer_pixels((AxlGfxBuffer *)sprite);
        if (axl_gfx_buffer_get_info(sprite, &w, &h) != AXL_OK || px == NULL
            || w == 0 || h == 0) {
            return AXL_ERR;
        }
        rc = ensure_sprite_size(c, (int32_t)w, (int32_t)h);
        if (rc == AXL_OK) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                c->sprite[i] = px[i];
            }
            c->hot_x = hot_x;
            c->hot_y = hot_y;
        }
    }
    if (rc == AXL_OK && was_on) {
        draw(c);
    }
    return rc;
}

void
axl_cursor_show(AxlCursor *c)
{
    if (c == NULL || c->visible) {
        return;
    }
    c->visible = true;
    if (!c->lifted && !c->on_screen) {
        draw(c);
    }
}

void
axl_cursor_hide(AxlCursor *c)
{
    if (c == NULL || !c->visible) {
        return;
    }
    c->visible = false;
    if (c->on_screen) {
        erase(c);
    }
}

bool
axl_cursor_visible(const AxlCursor *c)
{
    return c != NULL && c->visible;
}

void
axl_cursor_move(AxlCursor *c, int32_t x, int32_t y)
{
    if (c == NULL) {
        return;
    }
    if (c->on_screen) {
        erase(c);
    }
    c->x = clampi(x, 0, c->scene_w - 1);
    c->y = clampi(y, 0, c->scene_h - 1);
    if (c->visible && !c->lifted) {
        draw(c);
    }
}

void
axl_cursor_move_rel(AxlCursor *c, int32_t dx, int32_t dy)
{
    if (c == NULL) {
        return;
    }
    // Sum in 64-bit and clamp before the int32 hand-off, so a pathological
    // delta on a public call can't overflow c->x + dx. (axl_cursor_move
    // clamps to the framebuffer too; this just keeps the cast defined.)
    int64_t nx = (int64_t)c->x + dx;
    int64_t ny = (int64_t)c->y + dy;
    nx = nx < 0 ? 0 : (nx > c->scene_w - 1 ? c->scene_w - 1 : nx);
    ny = ny < 0 ? 0 : (ny > c->scene_h - 1 ? c->scene_h - 1 : ny);
    axl_cursor_move(c, (int32_t)nx, (int32_t)ny);
}

void
axl_cursor_position(const AxlCursor *c, int32_t *x, int32_t *y)
{
    if (c == NULL) {
        return;
    }
    if (x != NULL) { *x = c->x; }
    if (y != NULL) { *y = c->y; }
}

void
axl_cursor_lift(AxlCursor *c)
{
    if (c == NULL || c->lifted) {
        return;
    }
    if (c->on_screen) {
        erase(c);
    }
    c->lifted = true;
}

void
axl_cursor_drop(AxlCursor *c)
{
    if (c == NULL || !c->lifted) {
        return;
    }
    c->lifted = false;
    if (c->visible && !c->on_screen) {
        draw(c);
    }
}

// Trampoline: track the pointer, then forward to the consumer callback.
// Non-static (no public header) so the unit test can drive it with
// synthetic events — the unit harness has no input layer to exercise the
// seed/delta logic through axl_cursor_attach otherwise.
bool
cursor_input_trampoline(const AxlInputEvent *ev, void *data)
{
    AxlCursor *c = (AxlCursor *)data;
    switch (ev->type) {
    case AXL_INPUT_MOUSE_MOVE:
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
    case AXL_INPUT_MOUSE_BUTTON_UP:
    case AXL_INPUT_MOUSE_WHEEL:
        if (!c->visible) {
            axl_cursor_show(c);
        }
        // ev->x/y is the device's raw accumulated position (unbounded,
        // and over a remote relative pointer it can drift far past an
        // edge). Move by the delta from the previous event so the cursor
        // clamps at the edge yet recovers the instant motion reverses,
        // instead of getting stuck. Seed the reference on the first event.
        if (!c->tracking) {
            c->track_x  = ev->x;
            c->track_y  = ev->y;
            c->tracking = true;
        }
        axl_cursor_move_rel(c, ev->x - c->track_x, ev->y - c->track_y);
        c->track_x = ev->x;
        c->track_y = ev->y;
        break;
    default:
        break;
    }
    return (c->fwd_cb != NULL) ? c->fwd_cb(ev, c->fwd_data)
                               : AXL_SOURCE_CONTINUE;
}

uint32_t
axl_cursor_attach(AxlCursor *c, AxlLoop *loop, AxlInputCallback cb, void *data)
{
    if (c == NULL || loop == NULL) {
        return 0;
    }
    c->fwd_cb   = cb;
    c->fwd_data = data;
    c->tracking = false;   // re-seed the delta reference on the first event
    return axl_input_attach_mouse(loop, cursor_input_trampoline, c);
}

void
axl_cursor_detach(AxlCursor *c, AxlLoop *loop)
{
    if (c == NULL) {
        return;
    }
    axl_input_detach_mouse(loop);
    c->fwd_cb   = NULL;
    c->fwd_data = NULL;
    c->tracking = false;   // next attach re-seeds (attach resets too)
}
