/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-compositor.c
    Local in-process compositor (Phase C1 — surfaces + composite + present).
    See docs/AXL-Compositor-Design.md and axl-compositor.h.

    A surface is an AxlNTree node (the scene graph) carrying geometry + a
    back-buffer; node.data points back to the surface. Compositing walks
    the tree in pre-order (a node then its children) into a RAM output
    buffer, skipping invisible subtrees; C1 surfaces are opaque (a clipped
    copy).
    A present flushes only the accumulated damage bbox to the GOP.
**/

#include <axl/axl-compositor.h>
#include <axl/axl-gfx.h>
#include <axl/axl-cursor.h>
#include <axl/axl-ntree.h>
#include <axl/axl-input.h>
#include <axl/axl-log.h>
#include <axl/axl-loop.h>
#include <axl/axl-time.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-array.h>
#include <axl/axl-macros.h>

// The AxlNTree node is EMBEDDED (not heap-allocated separately), so the
// allocating tree calls must never touch these nodes: use only the
// relinking ones (append_child / unlink / move_* / traverse / iter).
// NEVER axl_ntree_free* a surface node — surf_free_recursive owns teardown
// (it frees the AxlSurface, which carries the node).
// One entry on the pointer-grab LIFO stack (C5).
typedef struct {
    AxlSurface         *surface;     // the grabbing surface
    AxlGrabDismissFunc  on_dismiss;  // called when dismissed by an outside press
    void               *user;        // borrowed context for on_dismiss
    bool                chain;       // confinement extends the grab below it
                                     // (a popup chain) vs replaces it (modal)
} AxlGrab;

struct AxlSurface {
    AxlNTree       node;      // embedded scene-graph node (node.data == this)
    AxlCompositor *comp;      // owning compositor
    int32_t        x, y;      // position relative to the parent
    uint32_t       w, h;      // size
    bool           visible;
    bool           opaque;    // occlusion hint (fully covers its rect opaquely)
    uint8_t        opacity;   // constant alpha (255 = opaque copy; <255 = blend)
    bool           per_pixel_alpha; // at opacity 255, source-over the buffer's
                                    // OWN per-pixel alpha instead of a straight
                                    // copy (opaque body + soft alpha edges in
                                    // one surface — popups, the dialog veil)
    AxlGfxBuffer  *buf;       // back-buffer (NULL for the root)

    // --- C4: the seat ---
    AxlSurfaceListener listener;       // copied by value (set_listener)
    void              *listener_user;  // borrowed callback context
    bool               has_listener;
    // Input region (Wayland set_input_region): has_region false = full rect
    // (default); has_region true with n_clips 0 = fully input-transparent;
    // otherwise the n surface-local rects are the input-opaque area.
    AxlGfxClip        *input_clips;
    uint32_t           n_input_clips;
    bool               has_input_region;

    // --- E7: frame callback (Wayland wl_surface.frame), one pending slot ---
    AxlFrameCallback   frame_cb;     // pending one-shot callback (NULL = none)
    void              *frame_user;   // borrowed context for frame_cb
    bool               has_frame;    // a frame callback is pending

    // --- E10: backdrop blur (the dialog veil) — 0 = off ---
    uint32_t           backdrop_blur; // blur the composited backdrop under this
                                      // surface by this radius before blitting
    // Blur cache: a full-rect backdrop blur is expensive and recurs on EVERY
    // present.  When the composited backdrop under this surface is byte-
    // identical to last time (a STATIC modal backdrop — the common case), skip
    // the blur and reuse the prior result.  `blur_src_cache` = pre-blur source
    // snapshot (the compare key); `blur_out_cache` = the blurred result to
    // reuse; both sized blur_cache_w × blur_cache_h.  Freed with the surface;
    // dropped when the radius changes.
    AxlGfxBuffer      *blur_src_cache;
    AxlGfxBuffer      *blur_out_cache;
    uint32_t           blur_cache_w, blur_cache_h;
};

struct AxlCompositor {
    AxlSurface     root;      // embedded tree anchor (no buffer, at 0,0)
    uint32_t       w, h;      // output size
    AxlGfxBuffer  *output;
    AxlGfxRegion  *damage;    // accumulated dirty region (output coords; E2)
    uint32_t       composited; // blit ops in the last composite/present (post-cull;
                               // a surface spanning N damage rects counts N times)
    uint32_t       occ_passes; // occlusion builds in the last composite/present
                               // (1 per present after the once-per-present hoist)
    bool           blur_partial; // E10: this paint is a partial re-blur present,
                                 // so a backdrop blur writes back only the part
                                 // of its rect it got exact and the present
                                 // restores the rest (see axl_compositor_present)
    bool           blur_failed;  // E10: a backdrop blur could not allocate during
                                 // this paint, so part of the frame is un-frosted
                                 // and the present must put the snapshot back

    // --- C4: the seat (one wl_seat embedded in the compositor) ---
    int32_t        ptr_x, ptr_y;   // pointer position in output coords
    bool           has_pointer;    // a pointer event has been seen (gates refocus)
    AxlSurface    *ptr_focus;      // surface under the pointer (NULL = none)
    uint32_t       ptr_buttons;    // current button mask (for change detection)
    // Device path (axl_compositor_attach_pointer): relative-delta seed.
    int32_t        dev_seed_x, dev_seed_y;
    bool           dev_seeded;
    AxlSourceId    ptr_source;     // pointer axl-loop source id (0 = not attached)
    AxlSourceId    touch_source;   // absolute-pointer (touch/BMC) source id (0 = not attached)
    uint32_t       touch_buttons;  // last button mask from the absolute source (edge-detect)
    AxlGesture     touch_gesture;  // click-count / drag recognizer for the absolute path

    // --- C5: grabs + keyboard focus ---
    AxlGrab        grab_stack[AXL_COMPOSITOR_GRAB_MAX];  // LIFO; [count-1] = active
    uint32_t       grab_count;
    AxlSurface    *kbd_focus;       // keyboard-focus surface (NULL = none)
    AxlSourceId    kbd_source;      // keyboard axl-loop source id (0 = not attached)

    // --- C6: the cursor (top overlay, bound to output, driven by the seat) ---
    AxlCursor     *cursor;

    // --- E7: the frame clock (present throttling) ---
    AxlLoop       *frame_loop;       // loop the clock is attached to (NULL = none)
    uint32_t       frame_interval;   // tick period in ms
    AxlSourceId    frame_timer;      // loop source id (0 = not armed)
};

AXL_LOG_DOMAIN("compositor");

#define SURF(ntnode) ((AxlSurface *)(ntnode)->data)

// Frame-clock timer trampoline, defined in the E7 section below.
static bool frame_tick(void *data);

// Blur-cache drop (E10), defined in the compositing section below.
static void blur_cache_drop(AxlSurface *s);

// Seat helpers defined in the seat section below.
static void seat_refocus(AxlCompositor *c);          // re-hit-test after a change
static void seat_purge_subtree(AxlCompositor *c,     // drop dying surfaces from
                               AxlSurface *s);       // pointer/kbd focus + grabs

// --- geometry helpers -----------------------------------------------------

// Absolute (output) origin of @s: sum of x/y up the parent chain.
static void
surf_abs(const AxlSurface *s, int32_t *ax, int32_t *ay)
{
    int32_t x = 0, y = 0;
    for (const AxlNTree *n = &s->node; n != NULL; n = n->parent) {
        x += SURF(n)->x;
        y += SURF(n)->y;
    }
    *ax = x;
    *ay = y;
}

// Screen rect of @s itself (NOT its subtree).
static AxlGfxClip
surf_screen_rect(const AxlSurface *s)
{
    int32_t ax, ay;
    surf_abs(s, &ax, &ay);
    return (AxlGfxClip){ax, ay, s->w, s->h};
}

// Union @add into @acc (both bboxes); empty operands are ignored.
static void
clip_union(AxlGfxClip *acc, AxlGfxClip add)
{
    if (add.w == 0 || add.h == 0) {
        return;
    }
    if (acc->w == 0 || acc->h == 0) {
        *acc = add;
        return;
    }
    int32_t x0 = acc->x < add.x ? acc->x : add.x;
    int32_t y0 = acc->y < add.y ? acc->y : add.y;
    int32_t ax1 = acc->x + (int32_t)acc->w, bx1 = add.x + (int32_t)add.w;
    int32_t ay1 = acc->y + (int32_t)acc->h, by1 = add.y + (int32_t)add.h;
    int32_t x1 = ax1 > bx1 ? ax1 : bx1;
    int32_t y1 = ay1 > by1 ? ay1 : by1;
    acc->x = x0;
    acc->y = y0;
    acc->w = (uint32_t)(x1 - x0);
    acc->h = (uint32_t)(y1 - y0);
}

// Visible screen footprint of @s and its descendants (empty if hidden).
static AxlGfxClip
surf_subtree_bounds(const AxlSurface *s)
{
    AxlGfxClip b = {0, 0, 0, 0};
    if (!s->visible) {
        return b;
    }
    if (s->buf != NULL) {
        b = surf_screen_rect(s);
    }
    for (const AxlNTree *c = s->node.children; c != NULL; c = c->next) {
        clip_union(&b, surf_subtree_bounds(SURF(c)));
    }
    return b;
}

// Intersect @r with the output; result may be empty (w or h == 0).
static AxlGfxClip
clip_to_output(const AxlCompositor *c, AxlGfxClip r)
{
    int32_t x0 = r.x < 0 ? 0 : r.x;
    int32_t y0 = r.y < 0 ? 0 : r.y;
    int32_t x1 = r.x + (int32_t)r.w, y1 = r.y + (int32_t)r.h;
    if (x1 > (int32_t)c->w) { x1 = (int32_t)c->w; }
    if (y1 > (int32_t)c->h) { y1 = (int32_t)c->h; }
    if (x1 <= x0 || y1 <= y0) {
        return (AxlGfxClip){0, 0, 0, 0};
    }
    return (AxlGfxClip){x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
}

// Intersection of two rects (half-open); {0,0,0,0} when disjoint.
static AxlGfxClip
clip_intersect(AxlGfxClip a, AxlGfxClip b)
{
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t ax1 = a.x + (int32_t)a.w, bx1 = b.x + (int32_t)b.w;
    int32_t ay1 = a.y + (int32_t)a.h, by1 = b.y + (int32_t)b.h;
    int32_t x1 = ax1 < bx1 ? ax1 : bx1;
    int32_t y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0) {
        return (AxlGfxClip){0, 0, 0, 0};
    }
    return (AxlGfxClip){x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
}

// Fill @r (already output-clamped) with @color in the output buffer.
static void
clear_rect(AxlCompositor *c, AxlGfxClip r, AxlGfxPixel color)
{
    AxlGfxPixel *dp = axl_gfx_buffer_pixels(c->output);
    if (dp == NULL || r.w == 0 || r.h == 0) {
        return;
    }
    for (uint32_t j = 0; j < r.h; j++) {
        AxlGfxPixel *row = &dp[(r.y + (int32_t)j) * (int32_t)c->w + r.x];
        for (uint32_t i = 0; i < r.w; i++) {
            row[i] = color;
        }
    }
}

static void
mark_damage(AxlCompositor *c, AxlGfxClip screen_rect)
{
    axl_gfx_region_union_rect(c->damage, clip_to_output(c, screen_rect));
}

// --- compositor lifecycle -------------------------------------------------

AxlCompositor *
axl_compositor_new(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) {
        return NULL;
    }
    AxlCompositor *c = axl_calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->output = axl_gfx_buffer_new(w, h);
    if (c->output == NULL) {
        axl_free(c);
        return NULL;
    }
    c->damage = axl_gfx_region_new();   // accumulated dirty region (E2)
    if (c->damage == NULL) {
        axl_gfx_buffer_free(c->output);
        axl_free(c);
        return NULL;
    }
    c->w = w;
    c->h = h;
    c->root.node.data = &c->root;   // root is the tree anchor
    c->root.comp      = c;
    c->root.w         = w;
    c->root.h         = h;
    c->root.visible   = true;
    c->root.opacity   = 255;
    c->root.buf       = NULL;       // never drawn
    // The cursor overlay binds to the output buffer (Option C). Tolerate a
    // NULL result (OOM): every cursor call is NULL-safe, so the compositor
    // simply has no overlay.
    c->cursor = axl_cursor_new(c->output);
    return c;
}

static void
surf_free_recursive(AxlSurface *s)
{
    AxlNTree *child = s->node.children;
    while (child != NULL) {
        AxlNTree *next = child->next;
        surf_free_recursive(SURF(child));
        child = next;
    }
    axl_gfx_buffer_free(s->buf);
    axl_gfx_buffer_free(s->blur_src_cache);   // E10 blur cache (NULL-safe)
    axl_gfx_buffer_free(s->blur_out_cache);
    axl_free(s->input_clips);
    axl_free(s);
}

void
axl_compositor_free(AxlCompositor *c)
{
    if (c == NULL) {
        return;
    }
    // Remove the frame-clock timer first so no tick fires into freed state
    // (the loop is caller-owned; detach_frame_clock is the explicit path, this
    // is the defensive backstop if the compositor is freed while still armed).
    if (c->frame_timer != 0 && c->frame_loop != NULL) {
        axl_loop_remove_source(c->frame_loop, c->frame_timer);
        c->frame_timer = 0;
    }
    AxlNTree *child = c->root.node.children;
    while (child != NULL) {
        AxlNTree *next = child->next;
        surf_free_recursive(SURF(child));   // root itself is embedded; not freed
        child = next;
    }
    axl_cursor_free(c->cursor);     // hides + frees the overlay before the output
    axl_gfx_buffer_free(c->output);
    axl_gfx_region_free(c->damage);
    axl_free(c);
}

AxlSurface *
axl_compositor_root(AxlCompositor *c)
{
    return c != NULL ? &c->root : NULL;
}

AxlGfxBuffer *
axl_compositor_output(AxlCompositor *c)
{
    return c != NULL ? c->output : NULL;
}

// --- surfaces -------------------------------------------------------------

AxlSurface *
axl_surface_new(AxlSurface *parent, uint32_t w, uint32_t h)
{
    if (parent == NULL || w == 0 || h == 0) {
        return NULL;
    }
    AxlSurface *s = axl_calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->buf = axl_gfx_buffer_new(w, h);
    if (s->buf == NULL) {
        axl_free(s);
        return NULL;
    }
    axl_gfx_buffer_clear(s->buf, AXL_GFX_RGBA(0, 0, 0, 0));
    s->node.data = s;
    s->comp      = parent->comp;
    s->w         = w;
    s->h         = h;
    s->visible   = true;
    s->opacity   = 255;   // opaque by default (calloc would give 0 = transparent)
    axl_ntree_append_child(&parent->node, &s->node);
    mark_damage(s->comp, surf_screen_rect(s));
    seat_refocus(s->comp);   // a new surface may land under the pointer
    return s;
}

void
axl_surface_free(AxlSurface *s)
{
    if (s == NULL || s->node.parent == NULL) {
        return;   // NULL or the root (no parent)
    }
    AxlCompositor *c = s->comp;
    // Drop the dying subtree from all seat state (pointer focus, keyboard
    // focus, grab stack) WITHOUT firing leave/focus_out/dismiss callbacks —
    // the surfaces and their listener contexts are being torn down.
    // seat_refocus below re-enters whatever is now beneath the pointer.
    seat_purge_subtree(c, s);
    mark_damage(c, surf_subtree_bounds(s));
    axl_ntree_unlink(&s->node);
    surf_free_recursive(s);
    seat_refocus(c);
}

AxlGfxBuffer *
axl_surface_buffer(AxlSurface *s)
{
    return s != NULL ? s->buf : NULL;
}

void
axl_surface_move(AxlSurface *s, int32_t x, int32_t y)
{
    if (s == NULL || (s->x == x && s->y == y)) {
        return;
    }
    AxlGfxClip before = surf_subtree_bounds(s);
    s->x = x;
    s->y = y;
    AxlGfxClip after = surf_subtree_bounds(s);
    mark_damage(s->comp, before);   // old + new as separate region rects (E2);
    mark_damage(s->comp, after);    // coalesces if they overlap, stays 2 if not
    seat_refocus(s->comp);
}

void
axl_surface_resize(AxlSurface *s, uint32_t w, uint32_t h)
{
    if (s == NULL || s->node.parent == NULL || w == 0 || h == 0
        || (s->w == w && s->h == h)) {
        return;   // NULL / root / zero dims / unchanged
    }
    // Allocate the new buffer first; on OOM keep the surface intact.
    AxlGfxBuffer *nb = axl_gfx_buffer_new(w, h);
    if (nb == NULL) {
        return;
    }
    axl_gfx_buffer_clear(nb, AXL_GFX_RGBA(0, 0, 0, 0));
    // Preserve the overlapping top-left region (a content-driven resize must
    // not blank existing pixels); the rest stays cleared.
    AxlGfxPixel *dp = axl_gfx_buffer_pixels(nb);
    AxlGfxPixel *sp = axl_gfx_buffer_pixels(s->buf);
    if (dp != NULL && sp != NULL) {
        uint32_t cw = w < s->w ? w : s->w;
        uint32_t ch = h < s->h ? h : s->h;
        for (uint32_t y = 0; y < ch; y++) {
            for (uint32_t x = 0; x < cw; x++) {
                dp[y * w + x] = sp[y * s->w + x];
            }
        }
    }
    AxlGfxClip before = surf_subtree_bounds(s);
    axl_gfx_buffer_free(s->buf);
    s->buf = nb;
    s->w   = w;
    s->h   = h;
    AxlGfxClip after = surf_subtree_bounds(s);
    mark_damage(s->comp, before);   // old + new as separate region rects (E2);
    mark_damage(s->comp, after);    // coalesces if they overlap, stays 2 if not
    seat_refocus(s->comp);
}

void
axl_surface_raise(AxlSurface *s)
{
    if (s == NULL || s->node.parent == NULL) {
        return;   // NULL or the root
    }
    // Move to the end of the sibling list (last child = painted last = top).
    axl_ntree_move_before(s->node.parent, NULL, &s->node);
    mark_damage(s->comp, surf_subtree_bounds(s));
    seat_refocus(s->comp);
}

void
axl_surface_lower(AxlSurface *s)
{
    if (s == NULL || s->node.parent == NULL) {
        return;
    }
    // Move to the front of the sibling list (first child = painted first).
    axl_ntree_move_after(s->node.parent, NULL, &s->node);
    mark_damage(s->comp, surf_subtree_bounds(s));
    seat_refocus(s->comp);
}

void
axl_surface_set_parent(AxlSurface *s, AxlSurface *new_parent)
{
    if (s == NULL || new_parent == NULL || s->node.parent == NULL
        || s->comp != new_parent->comp) {
        return;   // NULL / root / cross-compositor
    }
    AxlGfxClip before = surf_subtree_bounds(s);
    // Append as the new parent's top child; move_* rejects cycles.
    if (axl_ntree_move_before(&new_parent->node, NULL, &s->node) == NULL) {
        return;   // cycle (new_parent is s or a descendant) — unchanged
    }
    AxlGfxClip after = surf_subtree_bounds(s);
    mark_damage(s->comp, before);   // old + new as separate region rects (E2);
    mark_damage(s->comp, after);    // coalesces if they overlap, stays 2 if not
    seat_refocus(s->comp);
}

void
axl_surface_set_opacity(AxlSurface *s, uint8_t opacity)
{
    if (s == NULL || s->opacity == opacity) {
        return;
    }
    s->opacity = opacity;
    mark_damage(s->comp, surf_subtree_bounds(s));
}

void
axl_surface_set_opaque(AxlSurface *s, bool opaque)
{
    if (s != NULL) {
        s->opaque = opaque;   // occlusion hint only; no visible change
    }
}

void
axl_surface_set_per_pixel_alpha(AxlSurface *s, bool on)
{
    if (s == NULL || s->per_pixel_alpha == on) {
        return;
    }
    s->per_pixel_alpha = on;   // changes the blit path → re-composite the rect
    mark_damage(s->comp, surf_subtree_bounds(s));
}

void
axl_surface_set_backdrop_blur(AxlSurface *s, uint32_t radius)
{
    if (s == NULL || s->backdrop_blur == radius) {
        return;
    }
    s->backdrop_blur = radius;   // changes compositing → re-composite the rect
    blur_cache_drop(s);          // a radius change invalidates the cached blur
    mark_damage(s->comp, surf_subtree_bounds(s));
}

void
axl_surface_set_visible(AxlSurface *s, bool visible)
{
    if (s == NULL || s->visible == visible) {
        return;
    }
    // The affected footprint is the subtree as drawn while visible.
    s->visible = true;
    AxlGfxClip footprint = surf_subtree_bounds(s);
    s->visible = visible;
    mark_damage(s->comp, footprint);
    seat_refocus(s->comp);
}

bool
axl_surface_visible(const AxlSurface *s)
{
    return s != NULL && s->visible;
}

void
axl_surface_damage(AxlSurface *s, AxlGfxClip rect)
{
    if (s == NULL) {
        return;
    }
    int32_t ax, ay;
    surf_abs(s, &ax, &ay);
    mark_damage(s->comp, (AxlGfxClip){ax + rect.x, ay + rect.y, rect.w, rect.h});
}

void
axl_surface_get_geometry(const AxlSurface *s, int32_t *x, int32_t *y,
                         uint32_t *w, uint32_t *h)
{
    if (s == NULL) {
        return;
    }
    if (x != NULL) { *x = s->x; }
    if (y != NULL) { *y = s->y; }
    if (w != NULL) { *w = s->w; }
    if (h != NULL) { *h = s->h; }
}

void
axl_surface_get_absolute(const AxlSurface *s, int32_t *x, int32_t *y)
{
    if (s == NULL) {
        return;
    }
    int32_t ax, ay;
    surf_abs(s, &ax, &ay);
    if (x != NULL) { *x = ax; }
    if (y != NULL) { *y = ay; }
}

void
axl_surface_to_output(const AxlSurface *s, int32_t lx, int32_t ly,
                      int32_t *ox, int32_t *oy)
{
    if (s == NULL) {
        return;
    }
    int32_t ax, ay;
    surf_abs(s, &ax, &ay);
    if (ox != NULL) { *ox = ax + lx; }
    if (oy != NULL) { *oy = ay + ly; }
}

void
axl_surface_from_output(const AxlSurface *s, int32_t ox, int32_t oy,
                        int32_t *lx, int32_t *ly)
{
    if (s == NULL) {
        return;
    }
    int32_t ax, ay;
    surf_abs(s, &ax, &ay);
    if (lx != NULL) { *lx = ox - ax; }
    if (ly != NULL) { *ly = oy - ay; }
}

// --- compositing ----------------------------------------------------------

// Drop @s's blur cache (radius change / teardown). NULL-safe.
static void
blur_cache_drop(AxlSurface *s)
{
    axl_gfx_buffer_free(s->blur_src_cache);   // axl_gfx_buffer_free is NULL-safe
    axl_gfx_buffer_free(s->blur_out_cache);
    s->blur_src_cache = NULL;
    s->blur_out_cache = NULL;
    s->blur_cache_w   = 0;
    s->blur_cache_h   = 0;
}

// The radius axl_gfx_buffer_blur will actually use on a @w x @h buffer: it
// clamps the radius to the buffer's larger side (past that, clamp-to-edge
// sampling makes a bigger radius meaningless). Both E10 rect calculations have
// to mirror it — and note they mirror it for DIFFERENT buffers: the halo for
// the veil-sized one, `blur_valid_rect` for the sub-rect actually blurred.
static uint32_t
blur_clamped_radius(uint32_t w, uint32_t h, uint32_t radius)
{
    uint32_t maxd = (w > h) ? w : h;
    return (radius > maxd) ? maxd : radius;
}

// Copy @sub — a sub-rect of @src_rect — from the packed scratch @src (laid out
// as @src_rect, one row of src_rect.w pixels at a time) into the output at the
// same absolute coords.
static void
blit_scratch(AxlCompositor *c, const AxlGfxPixel *src, AxlGfxClip src_rect,
             AxlGfxClip sub)
{
    AxlGfxPixel *op = axl_gfx_buffer_pixels(c->output);
    if (op == NULL) {
        return;
    }
    for (uint32_t j = 0; j < sub.h; j++) {
        AxlGfxPixel *drow = &op[(sub.y + (int32_t)j) * (int32_t)c->w + sub.x];
        const AxlGfxPixel *srow = &src[(size_t)(sub.y - src_rect.y + (int32_t)j)
                                       * src_rect.w + (sub.x - src_rect.x)];
        axl_memcpy(drow, srow, (size_t)sub.w * sizeof(AxlGfxPixel));
    }
}

// E10: the sub-rect of @v a blur over @r reproduces EXACTLY.
//
// The blur clamps at its own buffer's border, so a pixel closer than @radius
// to an edge of @r replicates that edge instead of reading the true neighbours
// beyond it. Where @r's edge IS @v's edge that replication is the intended
// clamp; everywhere else it is a seam, so those pixels must not be written
// back. A full-veil blur (@r == @v) is valid throughout, which keeps the
// full-repaint path byte-identical.
//
// The erosion uses the radius as clamped for THIS rect, which for a sub-rect
// can be smaller than the one a full-veil blur would use. That never yields a
// wrongly-frosted pixel: a smaller clamp means radius > max(r.w, r.h), so
// eroding by max(r.w, r.h) empties the result on any edge that is not the
// veil's — and if all four are the veil's, the two clamps agree anyway.
static AxlGfxClip
blur_valid_rect(AxlGfxClip r, AxlGfxClip v, uint32_t radius)
{
    int32_t  rad = (int32_t)blur_clamped_radius(r.w, r.h, radius);
    int32_t  x0 = r.x, y0 = r.y;
    int32_t  x1 = r.x + (int32_t)r.w, y1 = r.y + (int32_t)r.h;
    if (r.x != v.x)                        { x0 += rad; }
    if (r.y != v.y)                        { y0 += rad; }
    if (x1 != v.x + (int32_t)v.w)          { x1 -= rad; }
    if (y1 != v.y + (int32_t)v.h)          { y1 -= rad; }
    if (x1 <= x0 || y1 <= y0) {
        return (AxlGfxClip){0, 0, 0, 0};
    }
    return (AxlGfxClip){x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
}

// Blur the output region @r in place by @radius (E10 backdrop blur): extract
// it to a scratch buffer, stack-blur, write back. @v is the veil's whole
// output-clipped rect (@r is @v or a sub-rect of it). Returns the region
// actually written — see `blur_valid_rect`; the caller narrows its own blit to
// it.
//
// On OOM the blur is skipped and the surface is left un-frosted — never fatal,
// but during a partial re-blur that would be a lasting artifact (the recomposite
// has already replaced the frost with raw backdrop, and later presents only
// repaint their own halo), so it also raises `blur_failed` and the present puts
// the whole frame back instead of flushing a half-frosted one.
//
// CACHE: a full-rect blur is expensive and recurs every present. When the
// composited backdrop in @r is byte-identical to the last blur (a static modal
// backdrop), reuse @s's stored blurred result instead of recomputing. The
// compare is content-based, so it can never serve a stale blur: a changed
// backdrop fails the memcmp and re-blurs. (The blur is the heavy multi-pass
// cost; the per-frame memcmp + memcpy is a fraction of it.) A partial re-blur
// skips the cache entirely — its rect changes shape frame to frame, so the
// cache would be reallocated every present and never hit.
static AxlGfxClip
blur_output_rect(AxlCompositor *c, AxlSurface *s, AxlGfxClip r, AxlGfxClip v,
                 uint32_t radius)
{
    if (r.w == 0 || r.h == 0 || radius == 0) {
        return r;
    }
    // Only a partial-re-blur present narrows the write-back: everywhere else
    // (a full repaint, an occlusion-split blit) the clamp at @r's edge is the
    // long-standing behaviour and the caller has nothing staged to restore.
    const AxlGfxClip valid = c->blur_partial ? blur_valid_rect(r, v, radius) : r;
    if (valid.w == 0 || valid.h == 0) {
        return valid;   // nothing of this rect would survive: don't blur it
    }
    const bool cache = !c->blur_partial;
    AxlGfxBuffer *tmp = axl_gfx_buffer_new(r.w, r.h);
    if (tmp == NULL) {
        c->blur_failed = true;
        return valid;   // OOM: skip the blur (degrade, not fatal)
    }
    AxlGfxPixel *op = axl_gfx_buffer_pixels(c->output);
    AxlGfxPixel *tp = axl_gfx_buffer_pixels(tmp);
    if (op == NULL || tp == NULL) {
        axl_gfx_buffer_free(tmp);
        c->blur_failed = true;
        return valid;
    }
    // Extract the backdrop region into the scratch (the blur source).
    for (uint32_t j = 0; j < r.h; j++) {
        for (uint32_t i = 0; i < r.w; i++) {
            tp[j * r.w + i] =
                op[(r.y + (int32_t)j) * (int32_t)c->w + r.x + (int32_t)i];
        }
    }
    const size_t bytes = (size_t)r.w * (size_t)r.h * sizeof(AxlGfxPixel);

    // Cache hit: same dims + identical source → reuse the stored blurred result.
    if (cache && s->blur_out_cache != NULL && s->blur_src_cache != NULL
        && s->blur_cache_w == r.w && s->blur_cache_h == r.h) {
        AxlGfxPixel *sc = axl_gfx_buffer_pixels(s->blur_src_cache);
        AxlGfxPixel *oc = axl_gfx_buffer_pixels(s->blur_out_cache);
        if (sc != NULL && oc != NULL && axl_memcmp(sc, tp, bytes) == 0) {
            blit_scratch(c, oc, r, valid);
            axl_gfx_buffer_free(tmp);
            return valid;
        }
    }

    // Cache miss: (re)size the cache to @r if needed, snapshot the source,
    // blur, store the result. A snapshot/alloc failure just disables caching
    // for this pass (still blurs correctly).
    if (cache) {
        if (s->blur_cache_w != r.w || s->blur_cache_h != r.h) {
            blur_cache_drop(s);
            s->blur_src_cache = axl_gfx_buffer_new(r.w, r.h);
            s->blur_out_cache = axl_gfx_buffer_new(r.w, r.h);
            if (s->blur_src_cache != NULL && s->blur_out_cache != NULL) {
                s->blur_cache_w = r.w;
                s->blur_cache_h = r.h;
            } else {
                blur_cache_drop(s);
            }
        }
        if (s->blur_src_cache != NULL) {
            AxlGfxPixel *sc = axl_gfx_buffer_pixels(s->blur_src_cache);
            if (sc != NULL) axl_memcpy(sc, tp, bytes);   // pre-blur snapshot (compare key)
        }
    }
    if (axl_gfx_buffer_blur(tmp, radius) != AXL_OK) {
        c->blur_failed = true;   // its own scratch OOM'd: @tmp is still the raw extract
    }
    if (cache && s->blur_out_cache != NULL) {
        AxlGfxPixel *oc = axl_gfx_buffer_pixels(s->blur_out_cache);
        if (oc != NULL) axl_memcpy(oc, tp, bytes);   // store blurred result
    }
    blit_scratch(c, tp, r, valid);
    axl_gfx_buffer_free(tmp);
    return valid;
}

// Clipped copy of @s's buffer into the output at @s's absolute pos,
// further restricted to @clip (output coords) — the recomposite region.
static void
surf_blit(AxlCompositor *c, const AxlSurface *s, AxlGfxClip clip)
{
    int32_t ax, ay;
    surf_abs(s, &ax, &ay);
    AxlGfxClip v = clip_to_output(c, (AxlGfxClip){ax, ay, s->w, s->h});
    AxlGfxClip r = clip_intersect(v, clip);
    if (r.w == 0 || r.h == 0) {
        return;
    }
    AxlGfxPixel *dp = axl_gfx_buffer_pixels(c->output);
    AxlGfxPixel *sp = axl_gfx_buffer_pixels(s->buf);
    if (dp == NULL || sp == NULL) {
        return;
    }
    c->composited++;
    // E10: frost the composited backdrop under @s before blitting its tint.
    // Back-to-front compositing means everything below @s is already in the
    // output here, so blurring @r in place gives a true backdrop blur. The
    // blur may decline part of @r (a partial re-blur is only exact away from
    // the edges it clamped at), so the tint follows it down to what it wrote.
    if (s->backdrop_blur > 0) {
        // const cast: the blur cache is mutable per-surface scratch; surf_blit
        // is otherwise read-only on @s.
        r = blur_output_rect(c, (AxlSurface *)s, r, v, s->backdrop_blur);
        if (r.w == 0 || r.h == 0) {
            return;
        }
    }
    for (uint32_t j = 0; j < r.h; j++) {
        AxlGfxPixel       *drow = &dp[(r.y + (int32_t)j) * (int32_t)c->w + r.x];
        const AxlGfxPixel *srow = &sp[(r.y + (int32_t)j - ay) * (int32_t)s->w
                                      + (r.x - ax)];
        if (s->opacity == 255 && !s->per_pixel_alpha) {
            for (uint32_t i = 0; i < r.w; i++) {
                drow[i] = srow[i];          // opaque: straight copy
            }
        } else {
            // Source-over with the buffer's alpha scaled by the surface
            // opacity (gamma-correct per the global setting).  Two cases land
            // here: a translucent surface (opacity < 255 → whole-surface fade,
            // per-pixel alpha further scaled), and a per-pixel-alpha surface at
            // full opacity (opacity == 255 → a == srow.alpha, the buffer's own
            // alpha blends untouched: opaque body, soft alpha edges, fully
            // transparent gaps show through).
            for (uint32_t i = 0; i < r.w; i++) {
                uint8_t a = (uint8_t)(((uint32_t)srow[i].alpha * s->opacity) / 255);
                drow[i] = axl_gfx_composite(drow[i],
                              AXL_GFX_RGBA(srow[i].red, srow[i].green, srow[i].blue, a));
            }
        }
    }
}

static void
surf_composite(AxlCompositor *c, const AxlSurface *s, AxlGfxClip clip)
{
    if (!s->visible) {
        return;   // hidden: skip the whole subtree
    }
    if (s->buf != NULL) {
        surf_blit(c, s, clip);
    }
    for (const AxlNTree *child = s->node.children; child != NULL;
         child = child->next) {
        surf_composite(c, SURF(child), clip);
    }
}

// The screen rect a surface opaquely covers (an occluder), or empty.
// Only an opacity==255 + opaque + NON-per-pixel-alpha surface occludes — that
// blit path is a straight copy, so the whole rect is opaque; a translucent
// (<255) surface or a per-pixel-alpha surface (opaque body but transparent /
// soft-alpha gaps — popups, the veil) can't occlude what's behind it.
static AxlGfxClip
surf_opaque_rect(const AxlCompositor *c, const AxlSurface *s)
{
    if (!s->opaque || s->opacity != 255 || s->per_pixel_alpha
        || s->backdrop_blur > 0) {
        return (AxlGfxClip){0, 0, 0, 0};   // translucent / overlay → no occlude
    }
    return clip_to_output(c, surf_screen_rect(s));
}

// Append visible buffer-bearing surfaces to @order in paint order
// (pre-order DFS = back-to-front). Returns false on an append allocation
// failure — the caller must then fall back to a full paint, since a dropped
// surface would otherwise be silently un-painted (stale pixels).
static bool
collect_order(const AxlSurface *s, AxlArray *order)
{
    if (!s->visible) {
        return true;   // hidden subtree
    }
    if (s->buf != NULL) {
        const AxlSurface *p = s;
        if (axl_array_append(order, &p) != AXL_OK) {
            return false;
        }
    }
    for (const AxlNTree *child = s->node.children; child != NULL;
         child = child->next) {
        if (!collect_order(SURF(child), order)) {
            return false;
        }
    }
    return true;
}

// Fallback paint (no occlusion) clipped to @clip — used when an allocation
// in the occlusion path fails: correctness over the culling optimization.
static void
paint_all_clipped(AxlCompositor *c, AxlGfxClip clip)
{
    for (const AxlNTree *child = c->root.node.children; child != NULL;
         child = child->next) {
        surf_composite(c, SURF(child), clip);
    }
}

// Per-present occlusion result: the back-to-front draw order plus each
// surface's globally-visible region — its screen rect minus everything opaque
// in front of it, over the FULL output. Computed ONCE per present (occlusion
// is a global property, independent of which sub-rect is being repainted) and
// then intersected with each damage rect — the once-per-present hoist (§10).
typedef struct {
    AxlArray      *order;   // AxlSurface* in paint order (back-to-front)
    AxlGfxRegion **vis;     // vis[k] = order[k]'s globally-visible region
    size_t         m;       // len(order) == number of vis entries
} CompVis;

static void
comp_vis_free(CompVis *cv)
{
    if (cv->vis != NULL) {
        for (size_t k = 0; k < cv->m; k++) {
            axl_gfx_region_free(cv->vis[k]);   // NULL-safe
        }
        axl_free(cv->vis);
    }
    axl_array_free(cv->order);
    cv->order = NULL;
    cv->vis   = NULL;
    cv->m     = 0;
}

// Build the draw order + per-surface globally-visible regions over the full
// output. Counts one occlusion pass. Front-to-back: each surface's visible
// region is its screen rect minus everything opaque already accumulated in
// front of it, then it adds its own opaque rect to the occluder. Returns false
// on any allocation/degrade in the walk (a region op returning AXL_ERR is an
// OOM degrade — args are non-NULL — that makes the region state unreliable for
// occlusion); @cv is then freed and reset and the caller must fall back to a
// full clipped paint, since occlusion is an optimization, never correctness.
static bool
comp_vis_build(AxlCompositor *c, CompVis *cv)
{
    c->occ_passes++;
    cv->order = axl_array_new(sizeof(AxlSurface *));
    cv->vis   = NULL;
    cv->m     = 0;
    AxlGfxRegion *occluded = axl_gfx_region_new();
    if (cv->order == NULL || occluded == NULL || !collect_order(&c->root, cv->order)) {
        goto fail;
    }
    size_t m = axl_array_len(cv->order);
    /* Array of m region pointers. Spell the element as the explicit pointer
       type, not sizeof(*vis): older clang-tidy's bugprone-sizeof-expression
       flags sizeof of a pointer-to-aggregate *expression* (a real false
       positive here — an array of pointers is exactly what we want). */
    AxlGfxRegion **vis = (m > 0) ? axl_malloc(m * sizeof(AxlGfxRegion *)) : NULL;
    if (m > 0 && vis == NULL) {
        goto fail;
    }
    for (size_t k = 0; k < m; k++) {
        vis[k] = NULL;
    }
    cv->vis = vis;
    cv->m   = m;

    for (size_t k = m; k-- > 0; ) {
        const AxlSurface *s = *(AxlSurface **)axl_array_get(cv->order, k);
        vis[k] = axl_gfx_region_new();
        if (vis[k] == NULL) {
            goto fail;
        }
        if (axl_gfx_region_union_rect(vis[k], clip_to_output(c, surf_screen_rect(s)))
                != AXL_OK
            || axl_gfx_region_subtract(vis[k], occluded) != AXL_OK) {
            goto fail;
        }
        AxlGfxClip op = surf_opaque_rect(c, s);
        if (op.w != 0 && op.h != 0
            && axl_gfx_region_union_rect(occluded, op) != AXL_OK) {
            goto fail;
        }
    }
    axl_gfx_region_free(occluded);
    return true;
fail:
    axl_gfx_region_free(occluded);
    comp_vis_free(cv);
    return false;
}

// Recomposite the @clip rect (output coords) using the precomputed visibility:
// clear it to the background, then blit each surface (back-to-front) restricted
// to its globally-visible region intersected with @clip, so the hidden part of
// an opaque surface overlapped by an opaque surface in front is never blitted.
// Does NOT reset c->composited (the caller does, so present accumulates the
// blit count across damage rects).
static void
paint_clip_with_vis(AxlCompositor *c, const CompVis *cv, AxlGfxClip clip)
{
    clip = clip_to_output(c, clip);
    if (clip.w == 0 || clip.h == 0) {
        return;
    }
    clear_rect(c, clip, AXL_GFX_RGB(0, 0, 0));
    for (size_t k = 0; k < cv->m; k++) {
        const AxlSurface *s = *(AxlSurface **)axl_array_get(cv->order, k);
        size_t nr = axl_gfx_region_num_rects(cv->vis[k]);
        for (size_t j = 0; j < nr; j++) {
            AxlGfxClip part = clip_intersect(axl_gfx_region_get_rect(cv->vis[k], j),
                                             clip);
            if (part.w != 0 && part.h != 0) {
                surf_blit(c, s, part);
            }
        }
    }
}

// Full clipped paint with no occlusion — the fallback when comp_vis_build
// fails: clear @clip then paint every surface clipped to it (correctness over
// the culling optimization).
static void
paint_clip_fallback(AxlCompositor *c, AxlGfxClip clip)
{
    clip = clip_to_output(c, clip);
    if (clip.w == 0 || clip.h == 0) {
        return;
    }
    clear_rect(c, clip, AXL_GFX_RGB(0, 0, 0));
    paint_all_clipped(c, clip);
}

void
axl_compositor_composite(AxlCompositor *c)
{
    if (c == NULL) {
        return;
    }
    // Full repaint of the whole output (the standalone / initial path).
    c->composited = 0;
    c->occ_passes = 0;
    AxlGfxClip full = {0, 0, (int32_t)c->w, (int32_t)c->h};
    CompVis cv;
    if (comp_vis_build(c, &cv)) {
        paint_clip_with_vis(c, &cv, full);
        comp_vis_free(&cv);
    } else {
        paint_clip_fallback(c, full);
    }
}

uint32_t
axl_compositor_composited_count(const AxlCompositor *c)
{
    return c != NULL ? c->composited : 0;
}

uint32_t
axl_compositor_occlusion_passes(const AxlCompositor *c)
{
    return c != NULL ? c->occ_passes : 0;
}

int
axl_compositor_get_damage(const AxlCompositor *c, AxlGfxClip *out)
{
    if (c == NULL || out == NULL || axl_gfx_region_is_empty(c->damage)) {
        return AXL_ERR;
    }
    *out = axl_gfx_region_bounds(c->damage);   // bbox of the damage region
    return AXL_OK;
}

const AxlGfxRegion *
axl_compositor_get_damage_region(const AxlCompositor *c)
{
    return c != NULL ? c->damage : NULL;
}

// E10: union every visible backdrop-blur surface's FULL screen rect into the
// damage if it intersects the pending damage — so the blur reads the whole
// backdrop, not a damage sub-rect (which would clamp mid-surface and seam).
// Returns true if any such surface was found (the caller then composites the
// damage bbox as one rect so banding can't split the surface).
//
// This is the CONSERVATIVE expansion: correct for any scene, but it re-blurs a
// whole veil for any change under it. `veil_expand_halo` below is the cheap
// path a present takes when it can prove the smaller region suffices.
static bool
backdrop_blur_expand(AxlCompositor *c, const AxlSurface *s)
{
    bool found = false;
    if (s->visible && s->buf != NULL && s->backdrop_blur > 0) {
        AxlGfxClip rect = clip_to_output(c, surf_screen_rect(s));
        if (rect.w > 0 && rect.h > 0
            && axl_gfx_region_intersects_rect(c->damage, rect)) {
            axl_gfx_region_union_rect(c->damage, rect);
            found = true;
        }
    }
    for (const AxlNTree *child = s->node.children; child != NULL;
         child = child->next) {
        if (backdrop_blur_expand(c, SURF(child))) {
            found = true;
        }
    }
    return found;
}

// --- E10: the partial re-blur plan ----------------------------------------
//
// A full-screen frosted veil used to cost a FULL-screen re-blur on ANY damage
// under it (~97% of a 1280x800 frame — a blinking caret repainted the world).
// A blur is a bounded neighbourhood read, though, so only a bounded halo
// around the change actually moves:
//
//   halo   (damage ∩ veil) ⊕ 2*radius, clipped to the veil — the raw backdrop
//          the blur must re-read: one radius for how far the frost moves, one
//          more for what each moved pixel samples. This is the rect the
//          present recomposites, so the blur reads real backdrop rather than
//          the previous frame's already-frosted output.
//   valid  the recomposite eroded by the radius on every edge that is not the
//          veil's own (`blur_valid_rect`) — the blur clamps at its buffer's
//          border, so only there does it reproduce a full-veil re-blur.
//   keep   recomposited but neither damaged nor re-frosted: collateral of
//          painting a rect rather than a region. Saved before the paint and
//          restored after, which is what makes the frame byte-identical to a
//          full re-blur (the seam test in compositor-selftest.c pins this).
//
// The plan is taken only when it is provably equivalent; anything else falls
// back to `backdrop_blur_expand`'s whole-veil repaint.
#define COMP_MAX_VEILS 8

typedef struct {
    AxlGfxClip rect;     // output-clipped screen rect (the blur's clamp box)
    uint32_t   radius;
} VeilInfo;

// Flatten the visible backdrop-blur surfaces into @out (hidden subtrees
// skipped, exactly as collect_order does). Returns the number found, which may
// EXCEED @max — the caller then declines the plan rather than reason from a
// truncated list.
static size_t
veil_collect(const AxlCompositor *c, const AxlSurface *s, VeilInfo *out,
             size_t max, size_t n)
{
    if (!s->visible) {
        return n;
    }
    if (s->buf != NULL && s->backdrop_blur > 0) {
        AxlGfxClip v = clip_to_output(c, surf_screen_rect(s));
        if (v.w != 0 && v.h != 0) {
            if (n < max) {
                out[n].rect   = v;
                out[n].radius = s->backdrop_blur;
            }
            n++;
        }
    }
    for (const AxlNTree *child = s->node.children; child != NULL;
         child = child->next) {
        n = veil_collect(c, SURF(child), out, max, n);
    }
    return n;
}

// True if any two veils overlap. A veil over another blurs the LOWER one's
// frosted output as its source — which a partial re-blur has only made exact
// over `valid`. Overlapping veils therefore decline the plan and re-blur whole
// rects, where every veil's output is exact everywhere.
static bool
veils_overlap(const VeilInfo *v, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            AxlGfxClip x = clip_intersect(v[i].rect, v[j].rect);
            if (x.w != 0 && x.h != 0) {
                return true;
            }
        }
    }
    return false;
}

// Grow @bb by the blur halo of every veil the damage reaches (see above).
// Returns true if any veil is involved.
static bool
veil_expand_halo(const AxlCompositor *c, const VeilInfo *v, size_t n,
                 AxlGfxClip *bb)
{
    bool found = false;
    for (size_t k = 0; k < n; k++) {
        if (!axl_gfx_region_intersects_rect(c->damage, v[k].rect)) {
            continue;
        }
        found = true;
        uint32_t maxd = (v[k].rect.w > v[k].rect.h) ? v[k].rect.w : v[k].rect.h;
        uint32_t rad  = (v[k].radius > maxd) ? maxd : v[k].radius;  // the blur's clamp
        if (rad > maxd / 2) {
            clip_union(bb, v[k].rect);   // the halo would span the veil anyway
            continue;
        }
        AxlGfxClip hull = {0, 0, 0, 0};   // the damage under this veil
        size_t nd = axl_gfx_region_num_rects(c->damage);
        for (size_t i = 0; i < nd; i++) {
            clip_union(&hull,
                       clip_intersect(axl_gfx_region_get_rect(c->damage, i), v[k].rect));
        }
        int32_t g = (int32_t)rad * 2;
        clip_union(bb, clip_intersect((AxlGfxClip){hull.x - g, hull.y - g,
                                                   hull.w + 2u * (uint32_t)g,
                                                   hull.h + 2u * (uint32_t)g},
                                      v[k].rect));
    }
    return found;
}

// The conservative expansion over a complete veil list: union the FULL rect of
// every veil the damage reaches, repeating until none is newly reached — an
// expanded veil can bring another one into the damage, and the second must be
// re-blurred (and flushed) whole too. Returns true if any veil was expanded.
static bool
veil_expand_full(AxlCompositor *c, const VeilInfo *v, size_t n)
{
    bool taken[COMP_MAX_VEILS] = { false };
    bool found = false, again = true;
    while (again) {
        again = false;
        for (size_t k = 0; k < n; k++) {
            if (taken[k]
                || !axl_gfx_region_intersects_rect(c->damage, v[k].rect)) {
                continue;
            }
            axl_gfx_region_union_rect(c->damage, v[k].rect);
            taken[k] = true;
            found = again = true;
        }
    }
    return found;
}

// True if some veil's blit is split by occlusion — an opaque surface in FRONT
// of it leaves a visible region that is not the single rect veil ∩ @bb. Its
// blur then runs per sub-rect and clamps at the occluder's edge instead of the
// veil's, which `blur_valid_rect` cannot account for, so the plan is declined.
static bool
veil_split(const AxlCompositor *c, const CompVis *cv, AxlGfxClip bb)
{
    for (size_t k = 0; k < cv->m; k++) {
        const AxlSurface *s = *(AxlSurface **)axl_array_get(cv->order, k);
        if (s->backdrop_blur == 0) {
            continue;
        }
        AxlGfxClip want = clip_intersect(clip_to_output(c, surf_screen_rect(s)), bb);
        if (want.w == 0 || want.h == 0) {
            continue;   // not painted this present
        }
        AxlGfxClip got = {0, 0, 0, 0};
        size_t seen = 0, nr = axl_gfx_region_num_rects(cv->vis[k]);
        for (size_t j = 0; j < nr; j++) {
            AxlGfxClip part = clip_intersect(axl_gfx_region_get_rect(cv->vis[k], j), bb);
            if (part.w != 0 && part.h != 0) {
                got = part;
                seen++;
            }
        }
        if (seen != 1 || got.x != want.x || got.y != want.y
            || got.w != want.w || got.h != want.h) {
            return true;
        }
    }
    return false;
}

// Build the plan's two regions: @changed — what this frame alters on screen
// (the damage plus every veil's re-frosted rect), which is the flush set — and
// @keep — recomposited but unaltered, saved and restored around the paint.
// Both must come out EXACT: a lossy (bounding-box) degrade would flush stale
// pixels or restore over fresh ones, so imprecision fails the plan.
static bool
veil_plan_regions(const AxlCompositor *c, const VeilInfo *v, size_t n,
                  AxlGfxClip bb, AxlGfxRegion *changed, AxlGfxRegion *keep)
{
    if (axl_gfx_region_copy(changed, c->damage) != AXL_OK) {
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        AxlGfxClip r = clip_intersect(v[k].rect, bb);
        if (r.w == 0 || r.h == 0) {
            continue;
        }
        if (axl_gfx_region_union_rect(changed,
                blur_valid_rect(r, v[k].rect, v[k].radius)) != AXL_OK) {
            return false;
        }
    }
    axl_gfx_region_clear(keep);
    if (axl_gfx_region_union_rect(keep, bb) != AXL_OK
        || axl_gfx_region_subtract(keep, changed) != AXL_OK) {
        return false;
    }
    return !axl_gfx_region_is_lossy(changed) && !axl_gfx_region_is_lossy(keep);
}

// Snapshot the output over @r into a fresh @r-shaped buffer, or NULL on OOM.
// Taken before the paint so any part of @r the paint disturbs without changing
// can be put back verbatim with blit_scratch (which reads exactly this
// layout). Snapshotting the whole rect rather than just the parts expected to
// need it is what lets a mid-paint blur failure restore the frame.
static AxlGfxPixel *
rect_snapshot(AxlCompositor *c, AxlGfxClip r)
{
    AxlGfxPixel *op = axl_gfx_buffer_pixels(c->output);
    if (op == NULL || r.w == 0 || r.h == 0) {
        return NULL;
    }
    AxlGfxPixel *snap = axl_malloc((size_t)r.w * (size_t)r.h * sizeof(AxlGfxPixel));
    if (snap == NULL) {
        return NULL;
    }
    for (uint32_t j = 0; j < r.h; j++) {
        axl_memcpy(&snap[(size_t)j * r.w],
                   &op[(r.y + (int32_t)j) * (int32_t)c->w + r.x],
                   (size_t)r.w * sizeof(AxlGfxPixel));
    }
    return snap;
}

int
axl_compositor_present(AxlCompositor *c)
{
    if (c == NULL) {
        return AXL_ERR;
    }
    if (axl_gfx_region_is_empty(c->damage)) {
        return AXL_OK;   // nothing changed
    }
    // E10: a backdrop-blur surface whose rect intersects the damage must be
    // recomposited over a rect its blur can read WHOLE, not a bare damage
    // sub-rect — else the blur clamps mid-surface and seams. Try the partial
    // plan (recomposite the blur halo, restore the collateral); fall back to
    // expanding the damage over such surfaces' FULL rects. Either way, when a
    // veil is involved the paint covers ONE rect so the region banding can't
    // split a veil into separately-blurred pieces.
    VeilInfo veils[COMP_MAX_VEILS];
    size_t   found     = veil_collect(c, &c->root, veils, COMP_MAX_VEILS, 0);
    bool     truncated = found > COMP_MAX_VEILS;   // more veils than `veils` holds
    size_t   nveil     = truncated ? COMP_MAX_VEILS : found;
    AxlGfxRegion *changed = NULL, *keep = NULL;
    AxlGfxPixel  *saved   = NULL;
    AxlGfxClip    bb      = axl_gfx_region_bounds(c->damage);
    bool veil    = false;
    bool partial = nveil != 0 && !truncated
                   && !veils_overlap(veils, nveil)
                   && veil_expand_halo(c, veils, nveil, &bb);
    if (partial) {
        veil    = true;
        changed = axl_gfx_region_new();
        keep    = axl_gfx_region_new();
        partial = changed != NULL && keep != NULL;
    }
    c->composited = 0;
    c->occ_passes = 0;
    // E3a: recomposite ONLY the damaged rects — areas that didn't change keep
    // their prior pixels, so a caret blink re-blits its rect, not the whole
    // scene. (Correct because every visual change marks damage; app content
    // changes follow the axl_surface_damage contract.) E6: occlusion is
    // computed ONCE for the whole present, then each rect is repainted against
    // the precomputed visible regions (§10).
    CompVis cv;
    bool ok = comp_vis_build(c, &cv);
    if (partial && ok && veil_split(c, &cv, bb)) {
        partial = false;
    }
    if (partial) {
        partial = veil_plan_regions(c, veils, nveil, bb, changed, keep);
    }
    if (partial) {
        saved   = rect_snapshot(c, bb);   // stage the whole recomposite rect
        partial = saved != NULL;
    }
    if (!partial) {
        // Declined: the long-standing whole-veil repaint, flushing the damage.
        // (`saved` is necessarily NULL here — staging it is the last step.)
        axl_gfx_region_free(changed);
        axl_gfx_region_free(keep);
        changed = NULL;
        keep    = NULL;
        if (truncated) {
            // More veils than the plan array holds: walk the tree instead,
            // repeatedly — expanding one veil can bring another into the
            // damage, and that one must be re-blurred whole as well. Each pass
            // takes at least one new veil, so `found` passes reach the fixpoint.
            for (size_t pass = 0; pass < found; pass++) {
                if (!backdrop_blur_expand(c, &c->root)) {
                    break;
                }
                veil = true;
            }
        } else {
            veil = veil_expand_full(c, veils, nveil);
        }
        bb = axl_gfx_region_bounds(c->damage);
    }
    c->blur_partial = partial;
    c->blur_failed  = false;
    size_t n = axl_gfx_region_num_rects(c->damage);
    if (veil) {
        // One full-coverage composite so no backdrop-blur surface is split.
        if (ok) { paint_clip_with_vis(c, &cv, bb); }
        else    { paint_clip_fallback(c, bb); }
    } else {
        for (size_t i = 0; i < n; i++) {
            AxlGfxClip d = axl_gfx_region_get_rect(c->damage, i);
            if (ok) { paint_clip_with_vis(c, &cv, d); }
            else    { paint_clip_fallback(c, d); }
        }
    }
    c->blur_partial = false;
    if (ok) {
        comp_vis_free(&cv);
    }
    // Put back what the halo repaint scribbled over but did not change: those
    // pixels still hold this frame's correct value from before the paint.
    if (saved != NULL) {
        if (c->blur_failed) {
            // A blur could not allocate, so part of `bb` was recomposited to
            // raw backdrop and never frosted. Restore the WHOLE snapshot and
            // flush nothing: the frame is dropped, but the output stays a
            // coherent frosted frame instead of keeping a lasting bright patch
            // that later halo-sized presents would never repaint.
            axl_warning("compositor: backdrop blur OOM - frame dropped");
            blit_scratch(c, saved, bb, bb);
            axl_gfx_region_clear(changed);
        } else {
            size_t nk = axl_gfx_region_num_rects(keep);
            for (size_t i = 0; i < nk; i++) {
                blit_scratch(c, saved, bb, axl_gfx_region_get_rect(keep, i));
            }
        }
        axl_free(saved);
    }
    c->blur_failed = false;
    // C6: bracket the flush so the cursor is the top layer of THIS present,
    // atomically. lift folds the sprite INTO the output (so the damage rects
    // below carry it — no separate erase/redraw to the GOP, hence no flicker
    // at low present rates); drop unfolds, leaving the output cursor-clean for
    // the next partial-damage repaint. No-ops while the cursor is hidden.
    axl_cursor_lift(c->cursor);
    // Flush each disjoint changed rect (E2): sparse changes far apart on screen
    // present as small separate rects, not their spanning bounding box.
    const AxlGfxRegion *flush = (changed != NULL) ? changed : c->damage;
    int r = AXL_OK;
    size_t nf = axl_gfx_region_num_rects(flush);
    for (size_t i = 0; i < nf; i++) {
        AxlGfxClip d = axl_gfx_region_get_rect(flush, i);
        if (axl_gfx_buffer_present_rect(c->output, (uint32_t)d.x, (uint32_t)d.y,
                                        (uint32_t)d.x, (uint32_t)d.y, d.w, d.h) != AXL_OK) {
            r = AXL_ERR;   // GOP unavailable (headless) — reported, not fatal
        }
    }
    axl_cursor_drop(c->cursor);
    // The frame is consumed regardless of whether the flush reached a GOP:
    // the output is up to date, so the damage clears either way.
    axl_gfx_region_clear(c->damage);
    axl_gfx_region_free(changed);
    axl_gfx_region_free(keep);
    return r;
}

// --- C4: the seat (pointer routing) --------------------------------------

void
axl_surface_set_listener(AxlSurface *s, const AxlSurfaceListener *listener,
                         void *user)
{
    if (s == NULL) {
        return;
    }
    if (listener != NULL) {
        s->listener      = *listener;   // copy by value
        s->listener_user = user;
        s->has_listener  = true;
    } else {
        s->has_listener  = false;
        s->listener_user = NULL;
    }
}

void
axl_surface_set_input_region(AxlSurface *s, const AxlGfxClip *clips, size_t n)
{
    if (s == NULL) {
        return;
    }
    axl_free(s->input_clips);
    s->input_clips      = NULL;
    s->n_input_clips    = 0;
    s->has_input_region = (clips != NULL);   // NULL = full rect; else n==0 = transparent
    if (clips != NULL && n > 0) {
        s->input_clips = axl_calloc(n, sizeof(*s->input_clips));
        if (s->input_clips != NULL) {
            for (size_t i = 0; i < n; i++) {
                s->input_clips[i] = clips[i];
            }
            s->n_input_clips = (uint32_t)n;
        } else {
            s->has_input_region = false;   // OOM → fall back to full rect
        }
    }
    // Always re-hit-test: every region change (including a NULL reset back to
    // the full rect) can expose or hide this surface under a stationary pointer.
    seat_refocus(s->comp);
}

// True if surface-local (lx, ly) lies within @s's input region.
static bool
surf_region_contains(const AxlSurface *s, int32_t lx, int32_t ly)
{
    if (!s->has_input_region) {
        return true;   // default: the whole surface rect catches input
    }
    for (uint32_t i = 0; i < s->n_input_clips; i++) {
        const AxlGfxClip *r = &s->input_clips[i];
        if (lx >= r->x && lx < r->x + (int32_t)r->w
            && ly >= r->y && ly < r->y + (int32_t)r->h) {
            return true;
        }
    }
    return false;   // outside every clip (or empty region) → transparent
}

// True if @s and every ancestor up to the root are visible.
static bool
surf_chain_visible(const AxlSurface *s)
{
    for (const AxlNTree *n = &s->node; n != NULL; n = n->parent) {
        if (!SURF(n)->visible) {
            return false;
        }
    }
    return true;
}

// Topmost surface whose input region contains output point (px, py), or
// NULL. On a hit, *lx/*ly receive the surface-local coordinates.
static AxlSurface *
hit_test(AxlCompositor *c, int32_t px, int32_t py, int32_t *lx, int32_t *ly)
{
    AxlNTreeIter it;
    axl_ntree_iter_init_reverse(&it, &c->root.node, AXL_NTREE_ALL);
    for (AxlNTree *n; (n = axl_ntree_iter_next(&it)) != NULL; ) {
        AxlSurface *s = SURF(n);
        if (s->buf == NULL || !surf_chain_visible(s)) {
            continue;   // the root (no buffer) or a hidden subtree
        }
        int32_t ax, ay;
        surf_abs(s, &ax, &ay);
        int32_t x = px - ax, y = py - ay;
        if (x < 0 || y < 0 || x >= (int32_t)s->w || y >= (int32_t)s->h) {
            continue;   // outside the surface rect
        }
        if (!surf_region_contains(s, x, y)) {
            continue;   // inside the rect but input-transparent here
        }
        *lx = x;
        *ly = y;
        return s;
    }
    return NULL;
}

// Move pointer focus to @target (NULL = none), emitting leave on the old
// surface and enter on the new one. @enter_x/y are @target-local.
//
// Note: the button mask (c->ptr_buttons) is intentionally NOT reset here. A
// button held while the pointer crosses a surface boundary therefore has no
// *implicit* drag grab — its release routes to whichever surface is then
// focused. A consumer that needs drag confinement takes an explicit
// axl_compositor_pointer_grab on press (and ungrabs on release); a separate
// automatic implicit-drag grab is intentionally not provided.
static void
seat_set_focus(AxlCompositor *c, AxlSurface *target, int32_t enter_x,
               int32_t enter_y)
{
    if (c->ptr_focus == target) {
        return;
    }
    AxlSurface *old = c->ptr_focus;
    c->ptr_focus = target;
    if (old != NULL && old->has_listener && old->listener.leave != NULL) {
        old->listener.leave(old->listener_user);
    }
    // C6: reset the cursor to the default arrow on every focus change; the new
    // surface's enter handler may then request its own shape via
    // axl_compositor_set_cursor_image.
    if (c->cursor != NULL) {
        axl_cursor_set_image(c->cursor, NULL, 0, 0);
    }
    if (target != NULL && target->has_listener && target->listener.enter != NULL) {
        target->listener.enter(target->listener_user, enter_x, enter_y);
    }
}

// Apply the active pointer grab (if any) to a raw hit-test result. The active
// confinement is the union of a contiguous run of grabs from the top of the
// stack down through the first EXCLUSIVE (non-chain) grab — a popup chain. A
// hit inside any of those grabs' subtrees routes; outside all of them → NULL.
// No grab → the hit unchanged. (A single grab, or an exclusive top grab,
// reduces to "inside the top grab's subtree" — the modal case.)
static AxlSurface *
seat_grab_filter(const AxlCompositor *c, AxlSurface *hit)
{
    if (c->grab_count == 0) {
        return hit;
    }
    if (hit == NULL) {
        return NULL;
    }
    // Walk down from the top while each grab is a chain grab; stop at (and
    // include) the first exclusive grab or the stack bottom. [lo, top] is the
    // active confinement set.
    uint32_t lo = c->grab_count - 1;
    while (lo > 0 && c->grab_stack[lo].chain) {
        lo--;
    }
    for (uint32_t i = lo; i < c->grab_count; i++) {
        const AxlSurface *g = c->grab_stack[i].surface;
        if (hit == g || axl_ntree_is_ancestor(&g->node, &hit->node)) {
            return hit;
        }
    }
    return NULL;
}

// Re-run the hit-test at the current pointer position and update focus
// (synthetic leave/enter, no motion), honoring the active grab. Called after
// surface and grab changes. A no-op until a pointer event has set a position
// (headless / pre-input startup).
static void
seat_refocus(AxlCompositor *c)
{
    if (!c->has_pointer) {
        return;
    }
    int32_t lx = 0, ly = 0;
    AxlSurface *t = hit_test(c, c->ptr_x, c->ptr_y, &lx, &ly);
    seat_set_focus(c, seat_grab_filter(c, t), lx, ly);
}

void
axl_compositor_pointer_event(AxlCompositor *c, const AxlInputEvent *ev)
{
    if (c == NULL || ev == NULL) {
        return;
    }
    switch (ev->type) {
    case AXL_INPUT_MOUSE_MOVE:
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
    case AXL_INPUT_MOUSE_BUTTON_UP:
    case AXL_INPUT_MOUSE_WHEEL:
        break;
    default:
        return;   // key routing is C5; other types are ignored here
    }

    // Clamp the position to the output and remember it.
    int32_t px = ev->x < 0 ? 0 : (ev->x >= (int32_t)c->w ? (int32_t)c->w - 1 : ev->x);
    int32_t py = ev->y < 0 ? 0 : (ev->y >= (int32_t)c->h ? (int32_t)c->h - 1 : ev->y);
    c->ptr_x = px;
    c->ptr_y = py;
    c->has_pointer = true;

    // C6: drive the cursor overlay to the pointer position (move while hidden
    // is just a position update; then show it on the first event).
    if (c->cursor != NULL) {
        axl_cursor_move(c->cursor, px, py);
        if (!axl_cursor_visible(c->cursor)) {
            axl_cursor_show(c->cursor);
        }
    }

    int32_t lx = 0, ly = 0;
    AxlSurface *t = hit_test(c, px, py, &lx, &ly);
    AxlSurface *target = seat_grab_filter(c, t);

    // Active grab, pointer outside its subtree: the event is not delivered to
    // any surface. A button PRESS is the dismiss signal — pop the grab and
    // call its dismiss callback (the toolkit closes the popup chain).
    if (c->grab_count > 0 && target == NULL) {
        seat_set_focus(c, NULL, 0, 0);   // leave any subtree surface
        if (ev->type == AXL_INPUT_MOUSE_BUTTON_DOWN) {
            AxlGrab *g = &c->grab_stack[c->grab_count - 1];
            AxlGrabDismissFunc cb = g->on_dismiss;
            void *u = g->user;
            c->grab_count--;             // pop before the callback (may re-grab)
            c->ptr_buttons = ev->buttons;
            if (cb != NULL) {
                cb(u);
            }
            // The grab changed (popped, maybe re-grabbed in the callback):
            // re-hit-test so a now-active outer grab re-enters the surface
            // under the stationary pointer.
            seat_refocus(c);
            return;
        }
        if (ev->type == AXL_INPUT_MOUSE_BUTTON_UP) {
            c->ptr_buttons = ev->buttons;
        }
        return;
    }

    seat_set_focus(c, target, lx, ly);

    if (target != NULL && target->has_listener) {
        const AxlSurfaceListener *l = &target->listener;
        switch (ev->type) {
        case AXL_INPUT_MOUSE_MOVE:
            if (l->motion != NULL) {
                l->motion(target->listener_user, lx, ly, ev->modifiers);
            }
            break;
        case AXL_INPUT_MOUSE_BUTTON_DOWN:
        case AXL_INPUT_MOUSE_BUTTON_UP:
            if (l->button != NULL) {
                uint32_t changed = c->ptr_buttons ^ ev->buttons;
                while (changed != 0) {
                    uint32_t bit = changed & (~changed + 1u);   // lowest set bit (unsigned, no UB)
                    l->button(target->listener_user, bit,
                              (ev->buttons & bit) != 0, lx, ly,
                              ev->modifiers, ev->click_count, ev->dragging);
                    changed &= ~bit;
                }
            }
            break;
        case AXL_INPUT_MOUSE_WHEEL:
            if (l->axis != NULL) {
                l->axis(target->listener_user, ev->wheel_dx, ev->wheel_dy,
                        ev->modifiers);
            }
            break;
        default:
            break;
        }
    }

    if (ev->type == AXL_INPUT_MOUSE_BUTTON_DOWN
        || ev->type == AXL_INPUT_MOUSE_BUTTON_UP) {
        c->ptr_buttons = ev->buttons;
    }
}

// Device trampoline: convert the mouse source's accumulated relative
// position into a clamped output position, then route it. (Mirrors
// AxlCursor's seed/delta clamp; C6 unifies the two.)
static bool
comp_pointer_trampoline(const AxlInputEvent *ev, void *data)
{
    AxlCompositor *c = data;
    switch (ev->type) {
    case AXL_INPUT_MOUSE_MOVE:
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
    case AXL_INPUT_MOUSE_BUTTON_UP:
    case AXL_INPUT_MOUSE_WHEEL: {
        if (!c->dev_seeded) {
            c->dev_seed_x = ev->x;
            c->dev_seed_y = ev->y;
            c->dev_seeded = true;
        }
        int64_t nx = (int64_t)c->ptr_x + (ev->x - c->dev_seed_x);
        int64_t ny = (int64_t)c->ptr_y + (ev->y - c->dev_seed_y);
        c->dev_seed_x = ev->x;
        c->dev_seed_y = ev->y;
        AxlInputEvent local = *ev;
        local.x = (int32_t)(nx < 0 ? 0 : (nx > (int64_t)c->w - 1 ? (int64_t)c->w - 1 : nx));
        local.y = (int32_t)(ny < 0 ? 0 : (ny > (int64_t)c->h - 1 ? (int64_t)c->h - 1 : ny));
        axl_compositor_pointer_event(c, &local);
        break;
    }
    default:
        break;
    }
    return AXL_SOURCE_CONTINUE;
}

AxlSourceId
axl_compositor_attach_pointer(AxlCompositor *c, AxlLoop *loop)
{
    if (c == NULL || loop == NULL) {
        return 0;
    }
    c->dev_seeded = false;            // re-seed the delta reference
    c->ptr_x = (int32_t)c->w / 2;     // start at the output centre
    c->ptr_y = (int32_t)c->h / 2;
    c->ptr_source = axl_input_attach_mouse(loop, comp_pointer_trampoline, c);
    return c->ptr_source;
}

void
axl_compositor_detach_pointer(AxlCompositor *c, AxlLoop *loop)
{
    if (c == NULL || loop == NULL) {
        return;
    }
    axl_input_detach_mouse(loop);
    c->ptr_source = 0;
    c->dev_seeded = false;
}

// Device trampoline for an ABSOLUTE pointer (touch / digitizer / BMC remote-console
// virtual mouse): the source delivers a position already normalized to
// [0, AXL_INPUT_ABS_RANGE); scale it straight onto the output and route it as a
// pointer event (no relative seed/accumulate — absolute IS the position).
static bool
comp_touch_trampoline(const AxlInputEvent *ev, void *data)
{
    AxlCompositor *c = data;
    switch (ev->type) {
    case AXL_INPUT_TOUCH_MOVE:
    case AXL_INPUT_TOUCH_DOWN:
    case AXL_INPUT_TOUCH_UP: {
        int64_t mw = (c->w > 0) ? (int64_t)c->w - 1 : 0;
        int64_t mh = (c->h > 0) ? (int64_t)c->h - 1 : 0;
        AxlInputEvent local = {0};
        local.timestamp_us = ev->timestamp_us;
        local.modifiers    = ev->modifiers;
        local.x = (int32_t)((int64_t)ev->x * mw / (AXL_INPUT_ABS_RANGE - 1));
        local.y = (int32_t)((int64_t)ev->y * mh / (AXL_INPUT_ABS_RANGE - 1));
        // ActiveButtons: bit0 = contact (left), bit1 = alt (right — a remote
        // console maps the alt/pen-side sensor to a right-click).  Derive the
        // button event from the MASK, not the touch DOWN/UP/MOVE type: the
        // absolute source only flags a 0<->non-zero contact transition, so a
        // change *between* two non-zero masks (e.g. adding right while left is
        // held) arrives as a MOVE — turn each per-button edge into a real
        // BUTTON_DOWN/UP (pointer_event only updates buttons on those).
        uint32_t btns = ((ev->buttons & 0x1u) ? AXL_INPUT_BUTTON_LEFT  : 0u)
                      | ((ev->buttons & 0x2u) ? AXL_INPUT_BUTTON_RIGHT : 0u);
        local.buttons = btns;
        if (btns != c->touch_buttons) {
            local.type = (btns & ~c->touch_buttons) ? AXL_INPUT_MOUSE_BUTTON_DOWN
                                                    : AXL_INPUT_MOUSE_BUTTON_UP;
            c->touch_buttons = btns;
        } else {
            local.type = AXL_INPUT_MOUSE_MOVE;
        }
        // Run the click/drag recognizer so the absolute path gets double-click +
        // drag (the relative path gets these inside axl-input; the touch source
        // emits raw events, so do it here).
        axl_input_gesture_feed(&c->touch_gesture, &local);
        axl_compositor_pointer_event(c, &local);
        break;
    }
    default:
        break;
    }
    return AXL_SOURCE_CONTINUE;
}

AxlSourceId
axl_compositor_attach_touch(AxlCompositor *c, AxlLoop *loop)
{
    if (c == NULL || loop == NULL) {
        return 0;
    }
    c->touch_buttons  = 0;                       // re-seed button edge-detection
    c->touch_gesture  = (AxlGesture){0};         // re-seed the click recognizer
    c->touch_source = axl_input_attach_touch(loop, comp_touch_trampoline, c);
    return c->touch_source;
}

void
axl_compositor_detach_touch(AxlCompositor *c, AxlLoop *loop)
{
    if (c == NULL || loop == NULL) {
        return;
    }
    axl_input_detach_touch(loop);
    c->touch_source = 0;
}

// --- C5: grabs + keyboard focus ------------------------------------------

// Drop @s and its descendants from all seat state (pointer focus, keyboard
// focus, grab stack) WITHOUT firing leave/focus_out/dismiss callbacks — used
// when the subtree is being destroyed. The caller re-hit-tests afterward.
static void
seat_purge_subtree(AxlCompositor *c, AxlSurface *s)
{
    if (c->ptr_focus != NULL
        && (c->ptr_focus == s
            || axl_ntree_is_ancestor(&s->node, &c->ptr_focus->node))) {
        c->ptr_focus = NULL;
    }
    if (c->kbd_focus != NULL
        && (c->kbd_focus == s
            || axl_ntree_is_ancestor(&s->node, &c->kbd_focus->node))) {
        c->kbd_focus = NULL;
    }
    uint32_t w = 0;
    for (uint32_t r = 0; r < c->grab_count; r++) {
        AxlSurface *g = c->grab_stack[r].surface;
        if (g == s || axl_ntree_is_ancestor(&s->node, &g->node)) {
            continue;   // drop grabs owned by the dying subtree
        }
        c->grab_stack[w++] = c->grab_stack[r];
    }
    c->grab_count = w;
}

static void
grab_push(AxlCompositor *c, AxlSurface *s, AxlGrabDismissFunc on_dismiss,
          void *user, bool chain)
{
    if (c == NULL || s == NULL || s->node.parent == NULL || s->comp != c
        || c->grab_count >= AXL_COMPOSITOR_GRAB_MAX) {
        return;   // NULL / root / cross-compositor / stack full
    }
    c->grab_stack[c->grab_count].surface    = s;
    c->grab_stack[c->grab_count].on_dismiss = on_dismiss;
    c->grab_stack[c->grab_count].user       = user;
    c->grab_stack[c->grab_count].chain      = chain;
    c->grab_count++;
    seat_refocus(c);   // confine focus to the new grab (chain or subtree)
}

void
axl_compositor_pointer_grab(AxlCompositor *c, AxlSurface *s,
                            AxlGrabDismissFunc on_dismiss, void *user)
{
    grab_push(c, s, on_dismiss, user, false);   // exclusive (modal)
}

void
axl_compositor_pointer_grab_chain(AxlCompositor *c, AxlSurface *s,
                                  AxlGrabDismissFunc on_dismiss, void *user)
{
    grab_push(c, s, on_dismiss, user, true);    // chain (popup chain)
}

void
axl_compositor_pointer_ungrab(AxlCompositor *c)
{
    if (c == NULL || c->grab_count == 0) {
        return;
    }
    c->grab_count--;
    seat_refocus(c);   // focus follows the now-active (or absent) grab
}

void
axl_compositor_set_keyboard_focus(AxlCompositor *c, AxlSurface *s)
{
    if (c == NULL || c->kbd_focus == s || (s != NULL && s->comp != c)) {
        return;   // unchanged or not our surface
    }
    AxlSurface *old = c->kbd_focus;
    c->kbd_focus = s;
    if (old != NULL && old->has_listener && old->listener.focus_out != NULL) {
        old->listener.focus_out(old->listener_user);
    }
    if (s != NULL && s->has_listener && s->listener.focus_in != NULL) {
        s->listener.focus_in(s->listener_user);
    }
}

AxlSurface *
axl_compositor_keyboard_focus(const AxlCompositor *c)
{
    return c != NULL ? c->kbd_focus : NULL;
}

void
axl_compositor_key_event(AxlCompositor *c, const AxlInputEvent *ev)
{
    if (c == NULL || ev == NULL) {
        return;
    }
    if (ev->type != AXL_INPUT_KEY_DOWN && ev->type != AXL_INPUT_KEY_UP) {
        return;   // pointer events are not the keyboard's concern
    }
    AxlSurface *f = c->kbd_focus;
    if (f != NULL && f->has_listener && f->listener.key != NULL) {
        f->listener.key(f->listener_user, ev);
    }
}

static bool
comp_key_trampoline(const AxlInputEvent *ev, void *data)
{
    axl_compositor_key_event((AxlCompositor *)data, ev);
    return AXL_SOURCE_CONTINUE;
}

AxlSourceId
axl_compositor_attach_keyboard(AxlCompositor *c, AxlLoop *loop)
{
    if (c == NULL || loop == NULL) {
        return 0;
    }
    c->kbd_source = axl_input_attach_key(loop, comp_key_trampoline, c);
    return c->kbd_source;
}

void
axl_compositor_detach_keyboard(AxlCompositor *c, AxlLoop *loop)
{
    if (c == NULL || loop == NULL || c->kbd_source == 0) {
        return;
    }
    // Detach via axl_input_detach_key: it removes the loop source AND frees
    // axl-input's per-process single-keyboard slot, so a later
    // axl_compositor_attach_keyboard on another compositor (a modal dialog's,
    // C7 seat handoff) can succeed — the mirror of axl_input_detach_mouse in
    // detach_pointer. Detach before freeing @c.
    axl_input_detach_key(loop);
    c->kbd_source = 0;
}

// --- C6: the cursor (top overlay) ----------------------------------------

AxlCursor *
axl_compositor_cursor(AxlCompositor *c)
{
    return c != NULL ? c->cursor : NULL;
}

int
axl_compositor_set_cursor_image(AxlCompositor *c, const AxlGfxBuffer *sprite,
                                int32_t hot_x, int32_t hot_y)
{
    if (c == NULL || c->cursor == NULL) {
        return AXL_ERR;
    }
    return axl_cursor_set_image(c->cursor, sprite, hot_x, hot_y);
}

// --- E7: frame callbacks (present throttling) ----------------------------

// A (callback, user) pair snapshotted for one dispatch. Snapshotting the
// pending set before firing lets a re-request made inside a callback queue for
// the NEXT dispatch, and keeps firing decoupled from tree mutation.
typedef struct {
    AxlFrameCallback cb;
    void            *user;
} FramePending;

// Append each surface with a pending frame callback to `out` and clear its
// pending flag (snapshot + clear). Returns false if an append fails: that
// surface keeps its flag (retried next dispatch) and the walk stops — the
// already-snapshotted callbacks still fire, the rest stay pending.
static bool
frame_collect_clear(AxlSurface *s, AxlArray *out)
{
    if (s->has_frame) {
        FramePending fp = {s->frame_cb, s->frame_user};
        s->has_frame = false;
        if (axl_array_append(out, &fp) != AXL_OK) {
            s->has_frame = true;   // restore: fire it on the next dispatch
            return false;
        }
    }
    for (AxlNTree *child = s->node.children; child != NULL; child = child->next) {
        if (!frame_collect_clear(SURF(child), out)) {
            return false;
        }
    }
    return true;
}

static bool
frame_any_pending(const AxlSurface *s)
{
    if (s->has_frame) {
        return true;
    }
    for (const AxlNTree *child = s->node.children; child != NULL;
         child = child->next) {
        if (frame_any_pending(SURF(child))) {
            return true;
        }
    }
    return false;
}

bool
axl_compositor_has_pending_frames(const AxlCompositor *c)
{
    return c != NULL && frame_any_pending(&c->root);
}

void
axl_surface_request_frame(AxlSurface *s, AxlFrameCallback cb, void *user)
{
    if (s == NULL) {
        return;
    }
    s->frame_cb   = cb;
    s->frame_user = user;
    s->has_frame  = (cb != NULL);   // NULL cancels a pending request
    // Arm the clock if it is attached and idle. While a tick is dispatching,
    // its repeating timer is still live (frame_timer != 0), so this same check
    // prevents a re-request mid-dispatch from creating a second timer.
    AxlCompositor *c = s->comp;
    if (s->has_frame && c->frame_loop != NULL && c->frame_timer == 0) {
        c->frame_timer = axl_loop_add_timer(c->frame_loop, c->frame_interval,
                                            frame_tick, c);
    }
}

bool
axl_compositor_dispatch_frame(AxlCompositor *c, uint64_t time_ms)
{
    if (c == NULL) {
        return false;
    }
    AxlArray *snap = axl_array_new(sizeof(FramePending));
    if (snap == NULL) {
        return axl_compositor_has_pending_frames(c);   // OOM: retry next tick
    }
    // Snapshot + clear the pending set, then fire — a re-request made inside a
    // callback sets has_frame again and so queues for the NEXT dispatch.
    frame_collect_clear(&c->root, snap);   // partial on OOM is fine
    size_t n = axl_array_len(snap);
    for (size_t i = 0; i < n; i++) {
        FramePending *fp = axl_array_get(snap, i);
        if (fp->cb != NULL) {
            fp->cb(fp->user, time_ms);
        }
    }
    axl_array_free(snap);
    return axl_compositor_has_pending_frames(c);
}

// Loop timer trampoline: dispatch one frame, then self-cancel if nothing
// re-requested (idle = no busy timer).
static bool
frame_tick(void *data)
{
    AxlCompositor *c = data;
    bool more = axl_compositor_dispatch_frame(c, axl_time_get_ms());
    if (!more) {
        c->frame_timer = 0;
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

AxlSourceId
axl_compositor_attach_frame_clock(AxlCompositor *c, AxlLoop *loop,
                                  uint32_t interval_ms)
{
    if (c == NULL || loop == NULL) {
        return 0;
    }
    c->frame_loop     = loop;
    c->frame_interval = interval_ms > 0 ? interval_ms : 16;   // ~60 fps default
    // Eager-arm so there is a real source id to return; the timer self-cancels
    // on its first idle tick and re-arms on the next request.
    if (c->frame_timer == 0) {
        c->frame_timer = axl_loop_add_timer(loop, c->frame_interval, frame_tick, c);
    }
    return c->frame_timer;
}

void
axl_compositor_detach_frame_clock(AxlCompositor *c, AxlLoop *loop)
{
    if (c == NULL || loop == NULL) {
        return;
    }
    if (c->frame_timer != 0) {
        axl_loop_remove_source(loop, c->frame_timer);
        c->frame_timer = 0;
    }
    c->frame_loop = NULL;
}
