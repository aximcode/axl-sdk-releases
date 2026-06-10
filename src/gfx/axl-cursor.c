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
    bool           lifted;       // inside a lift/drop (fold) bracket
    bool           on_screen;    // currently composited
    int32_t        vx, vy, vw, vh;  // last drawn on-screen rect (for erase)

    // Scene-bound atomic fold (Option C): a present folds the sprite INTO
    // the bound scene as its top layer (so the caller's one scene flush
    // carries the cursor — no separate erase/redraw to the GOP = no flicker),
    // then unfolds to keep the scene byte-clean. `under` holds the saved
    // pixels (packed at stride spr_w); fr* is the folded rect.
    bool           folded;
    int32_t        frx, fry, frw, frh;

    // axl_cursor_attach forwarding
    AxlInputCallback  fwd_cb;
    void             *fwd_data;
    // Relative-pointer tracking: the event carries the device's raw
    // accumulated position, so we move by the per-event delta instead of
    // clamping the (possibly far-off-screen) absolute value.
    int32_t           track_x, track_y;   // last event's absolute coords
    bool              tracking;            // track_x/y seeded yet?
    // Absolute pointer (touch / digitizer / remote-console virtual mouse):
    // its coordinates ARE the position (mapped onto the scene), so once one
    // arrives it is latched authoritative and relative motion stops moving
    // the cursor for the lifetime of the attach.
    bool              saw_absolute;        // an absolute event has been seen
    bool              bound_mouse;         // axl_cursor_attach bound the relative source
    bool              bound_touch;         // axl_cursor_attach bound the absolute source
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

// The cursor's on-screen rect at the current position, clamped to the scene,
// plus the matching offset into the sprite (sx,sy). Returns false when the
// cursor is fully off-screen (nothing visible). Shared by the scratch-compose
// draw (save-under) and the in-place scene fold (Option C).
static bool
cursor_rect(const AxlCursor *c, int32_t *vx, int32_t *vy, int32_t *vw,
            int32_t *vh, int32_t *sx, int32_t *sy)
{
    int32_t tlx = c->x - c->hot_x;
    int32_t tly = c->y - c->hot_y;
    int32_t ax  = clampi(tlx, 0, c->scene_w);
    int32_t ay  = clampi(tly, 0, c->scene_h);
    int32_t ax2 = clampi(tlx + c->spr_w, 0, c->scene_w);
    int32_t ay2 = clampi(tly + c->spr_h, 0, c->scene_h);
    int32_t aw  = ax2 - ax;
    int32_t ah  = ay2 - ay;
    if (aw <= 0 || ah <= 0) {
        return false;
    }
    *vx = ax; *vy = ay; *vw = aw; *vh = ah;
    *sx = ax - tlx; *sy = ay - tly;
    return true;
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
        // Both modes need a second sprite-sized buffer: save-under (Option B)
        // holds the screen pixels captured under the cursor for restore-on-
        // erase; the scene-bound (Option C) fold saves the scene pixels it
        // overwrites so the unfold can restore them.
        AxlGfxBuffer *nu = axl_gfx_buffer_new((uint32_t)w, (uint32_t)h);
        if (ns == NULL || nc == NULL || nu == NULL) {
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

static void unfold_from_scene(AxlCursor *c);   // restore a folded scene (below)

// Save-under (Option B) draw: capture the screen pixels under the cursor into
// `under` (the restore-on-erase truth, packed at stride spr_w), composite the
// sprite over them in the `compose` scratch, and present that. Used only by
// the direct-to-screen path; the scene-bound (Option C) path folds the sprite
// into the bound scene in place instead (fold_into_scene / scene_present).
static void
draw(AxlCursor *c)
{
    int32_t vx, vy, vw, vh, sx, sy;
    if (!cursor_rect(c, &vx, &vy, &vw, &vh, &sx, &sy)) {  // fully off-screen
        c->on_screen = false;
        c->vw = c->vh = 0;
        return;
    }

    AxlGfxPixel *comp_px  = axl_gfx_buffer_pixels(c->compose);
    AxlGfxPixel *under_px = axl_gfx_buffer_pixels(c->under);
    if (comp_px == NULL || under_px == NULL) {
        return;
    }
    if (vw == c->spr_w) {                // unclipped rows: one contiguous capture
        if (axl_gfx_capture(under_px, (uint32_t)vx, (uint32_t)vy,
                            (uint32_t)vw, (uint32_t)vh) != AXL_OK) {
            return;
        }
    } else {                             // edge-clipped: capture row by row
        for (int32_t j = 0; j < vh; j++) {
            if (axl_gfx_capture(&under_px[j * c->spr_w], (uint32_t)vx,
                                (uint32_t)(vy + j), (uint32_t)vw, 1) != AXL_OK) {
                return;
            }
        }
    }

    for (int32_t j = 0; j < vh; j++) {
        const AxlGfxPixel *srow = &under_px[j * c->spr_w];   // captured, packed
        const AxlGfxPixel *prow = &c->sprite[(sy + j) * c->spr_w + sx];
        AxlGfxPixel       *crow = &comp_px[j * c->spr_w];    // compose stride = spr_w
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
    // If the cursor is folded into the scene (inside a lift/drop bracket),
    // unfold first so we present the CLEAN scene region, not the baked-in
    // sprite — otherwise a move/hide/set_image called between lift and drop
    // would flush a ghost arrow to the GOP. No-op outside a bracket; save-
    // under never folds.
    if (c->folded) {
        unfold_from_scene(c);
    }
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

// --- Scene-bound (Option C) atomic fold ----------------------------------
//
// The flicker fix (see the struct comment): instead of presenting the cursor
// to the GOP as a SEPARATE op from the scene (erase old / draw new, with a
// gap where the screen shows no cursor), we composite the sprite INTO the
// bound scene as its top layer, then let ONE scene flush carry it. This
// mirrors how real software-cursor compositors work — wlroots
// (wlr_output_render_software_cursors composites the cursor into the buffer
// as the last layer before one commit) and Qt's direct-framebuffer QFbCursor
// (cursor composited over the repainted scene, region flushed once). The
// fold is transient: unfold_from_scene restores the saved pixels so the scene
// stays byte-clean for the compositor's next partial-damage repaint.

// Composite the sprite into the bound scene at the current position, saving
// the overwritten scene pixels into `under` (packed, stride spr_w) so
// unfold_from_scene can restore them. No-op (folded=false) when the cursor is
// off-screen or the buffers are unavailable. Scene-bound only.
static void
fold_into_scene(AxlCursor *c)
{
    c->folded = false;
    int32_t vx, vy, vw, vh, sx, sy;
    if (!cursor_rect(c, &vx, &vy, &vw, &vh, &sx, &sy)) {
        return;
    }
    AxlGfxPixel *scene_px = axl_gfx_buffer_pixels(c->scene);
    AxlGfxPixel *under_px = axl_gfx_buffer_pixels(c->under);
    if (scene_px == NULL || under_px == NULL) {
        return;
    }
    for (int32_t j = 0; j < vh; j++) {
        AxlGfxPixel       *drow = &scene_px[(vy + j) * c->scene_w + vx];
        const AxlGfxPixel *prow = &c->sprite[(sy + j) * c->spr_w + sx];
        AxlGfxPixel       *urow = &under_px[j * c->spr_w];   // packed save
        for (int32_t i = 0; i < vw; i++) {
            urow[i] = drow[i];                               // save scene pixel
            if (prow[i].alpha != 0) {
                drow[i] = axl_gfx_blend(drow[i], prow[i]);   // sprite over scene
            }
        }
    }
    c->frx = vx; c->fry = vy; c->frw = vw; c->frh = vh;
    c->folded = true;
}

// Restore the scene pixels saved by fold_into_scene, leaving the bound scene
// byte-identical to before the fold (Option C invariant).
static void
unfold_from_scene(AxlCursor *c)
{
    if (!c->folded) {
        return;
    }
    c->folded = false;
    AxlGfxPixel *scene_px = axl_gfx_buffer_pixels(c->scene);
    AxlGfxPixel *under_px = axl_gfx_buffer_pixels(c->under);
    if (scene_px == NULL || under_px == NULL) {
        return;
    }
    for (int32_t j = 0; j < c->frh; j++) {
        AxlGfxPixel       *drow = &scene_px[(c->fry + j) * c->scene_w + c->frx];
        const AxlGfxPixel *urow = &under_px[j * c->spr_w];
        for (int32_t i = 0; i < c->frw; i++) {
            drow[i] = urow[i];
        }
    }
}

// Present a scene rect (clamped to the scene) straight to the GOP at the same
// coordinates. With the cursor folded in, the folded region carries it;
// elsewhere the scene is clean, so this also erases an old cursor position.
static void
present_scene_rect(AxlCursor *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t x2 = clampi(x + w, 0, c->scene_w);
    int32_t y2 = clampi(y + h, 0, c->scene_h);
    x = clampi(x, 0, c->scene_w);
    y = clampi(y, 0, c->scene_h);
    if (x2 <= x || y2 <= y) {
        return;
    }
    axl_gfx_buffer_present_rect(c->scene, (uint32_t)x, (uint32_t)y,
                                (uint32_t)x, (uint32_t)y,
                                (uint32_t)(x2 - x), (uint32_t)(y2 - y));
}

// Scene-bound atomic (re)present of the cursor at its current position: fold
// the sprite into the scene, then flush the cursor — plus the old footprint
// when moving — in as few presents as possible (one union present when the
// old and new rects overlap, so a small move never shows a cursor-less gap),
// then unfold so the scene stays clean. Replaces the save-under erase()+draw()
// pair for the scene-bound path.
static void
scene_present(AxlCursor *c)
{
    int32_t orx = c->vx, ory = c->vy, orw = c->vw, orh = c->vh;
    bool    had_old = c->on_screen && orw > 0 && orh > 0;

    fold_into_scene(c);
    if (c->folded) {
        int32_t nrx = c->frx, nry = c->fry, nrw = c->frw, nrh = c->frh;
        bool overlap = had_old
            && orx < nrx + nrw && nrx < orx + orw
            && ory < nry + nrh && nry < ory + orh;
        if (overlap) {                       // one atomic present over old∪new
            int32_t x0 = orx < nrx ? orx : nrx;
            int32_t y0 = ory < nry ? ory : nry;
            int32_t x1 = orx + orw > nrx + nrw ? orx + orw : nrx + nrw;
            int32_t y1 = ory + orh > nry + nrh ? ory + orh : nry + nrh;
            present_scene_rect(c, x0, y0, x1 - x0, y1 - y0);
        } else {
            if (had_old) {                   // disjoint: erase old, draw new
                present_scene_rect(c, orx, ory, orw, orh);
            }
            present_scene_rect(c, nrx, nry, nrw, nrh);
        }
        c->vx = nrx; c->vy = nry; c->vw = nrw; c->vh = nrh;
        c->on_screen = true;
    } else if (had_old) {                    // moved fully off-screen: erase old
        present_scene_rect(c, orx, ory, orw, orh);
        c->on_screen = false;
        c->vw = c->vh = 0;
    }
    unfold_from_scene(c);
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
        if (c->save_under) { draw(c); } else { scene_present(c); }
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
        if (c->save_under) { draw(c); } else { scene_present(c); }
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
        erase(c);   // present clean scene over the cursor (both modes)
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
    c->x = clampi(x, 0, c->scene_w - 1);
    c->y = clampi(y, 0, c->scene_h - 1);
    if (c->save_under) {
        // Option B: erase the old rect, then draw the new via the scratch.
        if (c->on_screen) {
            erase(c);
        }
        if (c->visible && !c->lifted) {
            draw(c);
        }
    } else if (c->visible && !c->lifted) {
        scene_present(c);          // Option C: atomic erase-old + draw-new
    } else if (c->on_screen) {
        erase(c);                  // hidden/lifted: just remove the old cursor
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
    c->lifted = true;
    if (c->save_under) {
        // Option B: no bound scene to fold into — erase to the GOP and let
        // drop redraw (the legacy bracket; the compositor never uses this).
        if (c->on_screen) {
            erase(c);
        }
    } else if (c->visible && c->on_screen) {
        // Option C: fold the sprite INTO the scene so the caller's damage
        // flush carries the cursor in one present — no cursor-less gap. The
        // GOP footprint is unchanged; only the scene is transiently dirtied.
        fold_into_scene(c);
    }
}

void
axl_cursor_drop(AxlCursor *c)
{
    if (c == NULL || !c->lifted) {
        return;
    }
    c->lifted = false;
    if (c->save_under) {
        if (c->visible && !c->on_screen) {
            draw(c);
        }
    } else {
        unfold_from_scene(c);   // restore the scene; no-op if lift didn't fold
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
    // Absolute pointer (touchscreen / digitizer / remote-console virtual
    // mouse / VNC usb-tablet): the coordinate IS the position, normalized to
    // [0, AXL_INPUT_ABS_RANGE). Map it straight onto the scene extent (the
    // same extent axl_cursor_move clamps to) and latch absolute authoritative.
    case AXL_INPUT_TOUCH_DOWN:
    case AXL_INPUT_TOUCH_UP:
    case AXL_INPUT_TOUCH_MOVE:
        if (!c->visible) {
            axl_cursor_show(c);
        }
        c->saw_absolute = true;
        axl_cursor_move(
            c,
            (int32_t)((int64_t)ev->x * (c->scene_w - 1) / (AXL_INPUT_ABS_RANGE - 1)),
            (int32_t)((int64_t)ev->y * (c->scene_h - 1) / (AXL_INPUT_ABS_RANGE - 1)));
        break;
    case AXL_INPUT_MOUSE_MOVE:
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
    case AXL_INPUT_MOUSE_BUTTON_UP:
    case AXL_INPUT_MOUSE_WHEEL:
        if (!c->visible) {
            axl_cursor_show(c);
        }
        // Once an absolute device is driving, it owns position; the relative
        // mouse then only contributes events (buttons / wheel) to the
        // consumer's callback, never moving the cursor.
        if (!c->saw_absolute) {
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
        }
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
    return axl_cursor_attach_ex(c, loop, cb, data, NULL);
}

uint32_t
axl_cursor_attach_ex(AxlCursor *c, AxlLoop *loop, AxlInputCallback cb,
                     void *data, const AxlCursorConfig *cfg)
{
    if (c == NULL || loop == NULL) {
        return 0;
    }
    c->fwd_cb       = cb;
    c->fwd_data     = data;
    c->tracking     = false;   // re-seed the delta reference on the first event
    c->saw_absolute = false;   // relative drives until an absolute event arrives
    c->bound_mouse  = false;
    c->bound_touch  = false;

    bool bind_mouse = (cfg == NULL) || !cfg->skip_mouse;
    bool bind_touch = (cfg == NULL) || !cfg->skip_touch;

    // A non-NULL config applies its absolute-read settings process-globally
    // before the touch attach reads them (see axl_input_set_touch_*).
    if (cfg != NULL && bind_touch) {
        axl_input_set_touch_config(cfg->touch_method, !cfg->touch_all_handles,
                                   cfg->touch_poll_ms);
        axl_input_set_touch_drain(cfg->touch_drain);
    }

    uint32_t mouse_src = 0, touch_src = 0;
    if (bind_mouse) {
        mouse_src = axl_input_attach_mouse(loop, cursor_input_trampoline, c);
        c->bound_mouse = (mouse_src != 0);
    }
    if (bind_touch) {
        touch_src = axl_input_attach_touch(loop, cursor_input_trampoline, c);
        c->bound_touch = (touch_src != 0);
    }
    // Return a non-zero source ID on any success (prefer axl_cursor_detach to
    // tear down — a single ID can't remove both sources).
    return (mouse_src != 0) ? mouse_src : touch_src;
}

void
axl_cursor_detach(AxlCursor *c, AxlLoop *loop)
{
    if (c == NULL) {
        return;
    }
    if (c->bound_mouse) {
        axl_input_detach_mouse(loop);
    }
    if (c->bound_touch) {
        axl_input_detach_touch(loop);
    }
    c->bound_mouse  = false;
    c->bound_touch  = false;
    c->fwd_cb       = NULL;
    c->fwd_data     = NULL;
    c->tracking     = false;   // next attach re-seeds (attach resets too)
    c->saw_absolute = false;
}
