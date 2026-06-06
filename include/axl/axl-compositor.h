/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-compositor.h
 *
 * Local, in-process compositor for axl-gfx (Phase C1 — skeleton). See
 * docs/AXL-Compositor-Design.md.
 *
 * A compositor owns a full-screen **output** buffer and a **tree of
 * surfaces** (each a CPU-backed rectangle the client draws into). It
 * composites the visible surfaces bottom-to-top — tree pre-order, a node
 * then its children — into the output, and presents the changed region
 * to the GOP. One synchronous in-process client, so a present is the
 * atomicity barrier: the client mutates surfaces, then calls present.
 *
 * Surfaces form a tree (the scene graph). A surface's position is relative
 * to its parent; top-level surfaces are children of the compositor's root
 * (axl_compositor_root). The tree is an AxlNTree under the hood, so
 * stacking is its pre-order and hit-test (a later phase) its reverse.
 *
 * C1 scope: opaque surfaces, create/move/show/hide/damage, composite +
 * damaged present. Stacking is creation order among siblings. Later
 * phases add raise/lower/reparent, opacity/blending, the input seat, and
 * the cursor overlay.
 *
 * @code
 * AxlCompositor *c = axl_compositor_new(1280, 800);
 * AxlSurface *win = axl_surface_create(axl_compositor_root(c), 400, 300);
 * axl_surface_move(win, 100, 80);
 * axl_gfx_target_buffer(axl_surface_buffer(win));   // draw into it
 * axl_gfx_fill_rect(0, 0, 400, 300, AXL_GFX_RGB(0x20, 0x40, 0x80));
 * axl_gfx_target_buffer(NULL);
 * axl_surface_damage(win, (AxlGfxClip){0, 0, 400, 300});
 * axl_compositor_present(c);                          // composite + flush
 * @endcode
 */

#ifndef AXL_COMPOSITOR_H
#define AXL_COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <axl/axl-gfx-surface.h>   /* AxlGfxBuffer, AxlGfxClip */
#include <axl/axl-gfx-region.h>    /* AxlGfxRegion (damage, E2) */
#include <axl/axl-input.h>         /* AxlInputEvent, AxlLoop (the seat, C4) */

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque compositor (owns the output and the surface tree).
typedef struct AxlCompositor AxlCompositor;

/// Opaque surface (a node in the compositor's scene graph).
typedef struct AxlSurface AxlSurface;

/// Software cursor overlay (see <axl/axl-cursor.h>); the compositor owns one
/// and drives it from the seat pointer (C6).
typedef struct AxlCursor AxlCursor;

// ---------------------------------------------------------------------------
// Compositor
// ---------------------------------------------------------------------------

/**
 * @brief Create a compositor with a @p w × @p h output.
 *
 * @return the compositor, or NULL on allocation failure / zero dimensions.
 */
AxlCompositor *
axl_compositor_new(
    uint32_t  w,   ///< output width in pixels (> 0)
    uint32_t  h    ///< output height in pixels (> 0)
);

/**
 * @brief Destroy a compositor and every surface in its tree. NULL-safe.
 */
void
axl_compositor_free(
    AxlCompositor  *c   ///< compositor (NULL-safe)
);

/**
 * @brief The root surface — the tree anchor at (0, 0). Create top-level
 *        surfaces as its children. It has no buffer and is never drawn.
 *
 * @return the root, or NULL if @p c is NULL.
 */
AxlSurface *
axl_compositor_root(
    AxlCompositor  *c   ///< compositor
);

/**
 * @brief The output buffer — the composited output. Read its pixels to
 *        inspect what would be presented. Owned by the compositor.
 *
 * @return the output buffer, or NULL if @p c is NULL.
 */
AxlGfxBuffer *
axl_compositor_output(
    AxlCompositor  *c   ///< compositor
);

/**
 * @brief Recomposite every visible surface into the output (in tree
 *        pre-order). Does not touch the screen — use this to inspect the
 *        composited result, or before reading axl_compositor_output.
 */
void
axl_compositor_composite(
    AxlCompositor  *c   ///< compositor
);

/**
 * @brief Composite and present the damaged region to the GOP.
 *
 * Recomposites the output and flushes the accumulated damage REGION — each
 * disjoint changed rectangle since the last present, not their spanning
 * bounding box — then clears the damage. Sparse changes far apart on screen
 * (a caret blink and a clock, say) flush as small separate rects instead of
 * one large box. See axl_compositor_get_damage_region.
 *
 * "Present" here is the Vulkan / DXGI / X-Present sense — *composite the
 * frame and put it on screen* — NOT Wayland's `wl_surface.commit` (which
 * only applies pending state; the compositor repaints separately). With
 * one synchronous in-process client there is no pending/current
 * double-buffering, so this single call is *both* Wayland's commit (the
 * atomicity barrier — the client mutates surfaces, then presents one
 * coherent frame) and the compositor's repaint.
 *
 * The damage is consumed (cleared) and the output updated **even when
 * AXL_ERR is returned** because the GOP is unavailable (a headless run):
 * the output is current regardless; the return reports only whether the
 * flush reached the screen.
 *
 * @return AXL_OK on success (including a no-op when nothing is damaged),
 *     AXL_ERR if @p c is NULL or the GOP is unavailable.
 */
int
axl_compositor_present(
    AxlCompositor  *c   ///< compositor
);

/**
 * @brief Get the pending damage bbox (the union of surface changes since
 *        the last present), in output coordinates.
 *
 * @return AXL_OK and fills @p out if damage is pending; AXL_ERR (and
 *     leaves @p out untouched) if nothing is damaged or @p c / @p out is
 *     NULL.
 */
int
axl_compositor_get_damage(
    const AxlCompositor  *c,    ///< compositor
    AxlGfxClip           *out   ///< [out] damage bbox (clamped to the output)
);

/**
 * @brief Get the pending damage as a precise REGION (the exact set of
 *        changed rectangles since the last present), in output coordinates.
 *
 * Unlike axl_compositor_get_damage (which returns the bounding box), this
 * is the exact disjoint-rectangle set that present flushes. Borrowed —
 * owned by the compositor, valid until the next surface change or present;
 * do not free or retain it.
 *
 * @return the damage region (empty when nothing is pending), or NULL if
 *     @p c is NULL.
 */
const AxlGfxRegion *
axl_compositor_get_damage_region(
    const AxlCompositor  *c   ///< compositor
);

/**
 * @brief Number of surfaces actually composited (blitted) in the most
 *        recent composite/present — i.e. after occlusion culling.
 *
 * For perf introspection and verifying opaque hints take effect.
 *
 * @return the count, or 0 if @p c is NULL / nothing has been composited.
 */
uint32_t
axl_compositor_composited_count(
    const AxlCompositor  *c   ///< compositor
);

/**
 * @brief Number of occlusion passes (draw-order + visible-region builds)
 *        performed in the most recent composite/present.
 *
 * Occlusion is computed ONCE per present and reused for every damage rect
 * (the visible regions are a global property, independent of which sub-rect
 * is repainted), so a present that flushes N disjoint damage rects still
 * reports 1 — not N. A full axl_compositor_composite is likewise 1. An
 * allocation failure that forces the full-paint fallback still counts as the
 * 1 attempted pass. For perf introspection (the once-per-present hoist, §10).
 *
 * @return the count, or 0 if @p c is NULL / nothing has been composited.
 */
uint32_t
axl_compositor_occlusion_passes(
    const AxlCompositor  *c   ///< compositor
);

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

/**
 * @brief Create a @p w × @p h surface as the last child of @p parent.
 *
 * Top-level surfaces pass axl_compositor_root as @p parent. The surface
 * starts at (0, 0) relative to the parent, visible, with its own cleared
 * back-buffer (draw into it via axl_surface_buffer). Re-hit-tests under the
 * pointer — a surface created over a stationary pointer gets a synthetic
 * @c enter.
 *
 * @return the surface, or NULL on allocation failure / NULL parent / zero
 *     dimensions.
 */
AxlSurface *
axl_surface_create(
    AxlSurface  *parent,   ///< parent surface (compositor root for top-level)
    uint32_t     w,        ///< width in pixels (> 0)
    uint32_t     h         ///< height in pixels (> 0)
);

/**
 * @brief Destroy a surface and its entire subtree (children die with it).
 *        NULL-safe. Marks the vacated screen region damaged.
 *
 * Must not be the compositor root. Drops the subtree from the seat's
 * pointer focus, keyboard focus, and grab stack, then re-hit-tests under
 * the pointer — a surface beneath the destroyed one receives a synthetic
 * @c enter.
 */
void
axl_surface_destroy(
    AxlSurface  *s   ///< surface (NULL-safe)
);

/**
 * @brief The surface's back-buffer — its draw target. Owned by the
 *        surface.
 *
 * @return the buffer, or NULL if @p s is NULL / the root (which has none).
 */
AxlGfxBuffer *
axl_surface_buffer(
    AxlSurface  *s   ///< surface
);

/**
 * @brief Move the surface to (@p x, @p y) relative to its parent. Marks
 *        both the old and new screen regions damaged.
 *
 * Re-hit-tests under the pointer: if the surface under a stationary pointer
 * changed, the seat emits synthetic @c leave / @c enter (no @c motion).
 */
void
axl_surface_move(
    AxlSurface  *s,   ///< surface
    int32_t      x,   ///< x relative to parent origin
    int32_t      y    ///< y relative to parent origin
);

/**
 * @brief Resize the surface's back-buffer to @p w × @p h (content-driven
 *        popups/dialogs, a top-level on a GOP mode change).
 *
 * Pixels in the overlapping top-left region are **preserved**; any newly
 * exposed area is cleared (transparent). Damages the union of the old and
 * new screen bounds and re-hit-tests under the pointer. No-op on the root,
 * on zero dimensions, on an unchanged size, or if the new buffer can't be
 * allocated (the surface keeps its old buffer). The draw target obtained
 * from axl_surface_buffer is invalidated — re-fetch it after a resize.
 */
void
axl_surface_resize(
    AxlSurface  *s,   ///< surface (not the root)
    uint32_t     w,   ///< new width in pixels (> 0)
    uint32_t     h    ///< new height in pixels (> 0)
);

/**
 * @brief Raise the surface to the top of its siblings.
 *
 * It (and its whole subtree) paints last among its siblings, so it sits on
 * top of them — Wayland `place_above` against the topmost sibling. No-op
 * for the root or an only child. Marks the surface's screen region
 * damaged, and re-hit-tests under the pointer (synthetic @c leave / @c enter
 * if the surface under a stationary pointer changed).
 */
void
axl_surface_raise(
    AxlSurface  *s   ///< surface
);

/**
 * @brief Lower the surface to the bottom of its siblings (the inverse of
 *        axl_surface_raise).
 *
 * Re-hit-tests under the pointer (synthetic @c leave / @c enter if the
 * surface under a stationary pointer changed).
 */
void
axl_surface_lower(
    AxlSurface  *s   ///< surface
);

/**
 * @brief Reparent the surface (and its subtree) under @p new_parent, as
 *        its top child.
 *
 * The surface's (x, y) numbers are unchanged but now relative to
 * @p new_parent, so its absolute position moves; both the old and new
 * screen regions are damaged. No-op if @p new_parent belongs to a
 * different compositor, if @p s is the root, or if the move would create a
 * cycle (@p new_parent is @p s or one of its descendants). Re-hit-tests
 * under the pointer (synthetic @c leave / @c enter if the surface under a
 * stationary pointer changed).
 */
void
axl_surface_set_parent(
    AxlSurface  *s,           ///< surface to reparent
    AxlSurface  *new_parent   ///< new parent (same compositor)
);

/**
 * @brief Show or hide the surface (and, visually, its subtree). Marks the
 *        affected screen region damaged.
 *
 * Re-hit-tests under the pointer: hiding a surface beneath the pointer (or
 * revealing one) emits synthetic @c leave / @c enter (no @c motion).
 */
void
axl_surface_set_visible(
    AxlSurface  *s,        ///< surface
    bool         visible   ///< true to show, false to hide
);

/**
 * @brief Set the surface's constant opacity (255 = opaque, 0 = fully
 *        transparent). Default 255.
 *
 * At 255 the surface is composited as an opaque rectangle (a fast copy).
 * Below 255 it is alpha-blended over whatever is beneath it — the
 * translucent veil / dim case — gamma-correct per the global
 * axl_gfx_set_gamma_correct setting; the effective per-pixel alpha is the
 * buffer's alpha scaled by this opacity. Marks the surface region damaged.
 */
void
axl_surface_set_opacity(
    AxlSurface  *s,        ///< surface
    uint8_t      opacity   ///< 0 (transparent) .. 255 (opaque)
);

/**
 * @brief Mark the surface opaque (or not) — an occlusion hint.
 *
 * A surface flagged opaque that fully covers the output lets the
 * compositor skip compositing everything beneath it (see §7). Default is
 * not-opaque (no culling). This is purely an optimization hint in the
 * current phase — it does not change the composited pixels.
 */
void
axl_surface_set_opaque(
    AxlSurface  *s,       ///< surface
    bool         opaque   ///< true if the surface fully covers its rect opaquely
);

/**
 * @brief Honor the buffer's OWN per-pixel alpha at full (255) opacity.
 *
 * Normally a surface at @c opacity 255 is composited as a straight copy
 * (per-pixel buffer alpha ignored — the fast opaque path), and only
 * @c opacity < 255 alpha-blends.  Setting this makes a 255-opacity surface
 * instead source-over its buffer's own alpha channel: an OPAQUE body
 * (alpha 255 pixels), soft ANTI-ALIASED / shadow edges (partial alpha), and
 * fully transparent gaps (alpha 0 — what's beneath shows through), all in one
 * surface, with NO whole-surface dimming.  This is the popup / dropdown /
 * tooltip case (an opaque panel plus a soft drop shadow) and the dialog veil.
 * Default off.
 *
 * Such a surface never occludes what is behind it (its gaps are see-through),
 * so it is excluded from occlusion culling regardless of the @c opaque hint.
 * Combine with @c opacity < 255 to additionally fade the whole surface (the
 * per-pixel alpha is then scaled by the opacity).  Marks the surface damaged.
 */
void
axl_surface_set_per_pixel_alpha(
    AxlSurface  *s,    ///< surface
    bool         on    ///< true to blend the buffer's own alpha at full opacity
);

/**
 * @brief Blur the already-composited backdrop under this surface at composite
 *        time (the dialog veil / frosted panel). @p radius 0 = off (default).
 *
 * When this surface is composited, the region of the output it covers — which,
 * because compositing is back-to-front, already holds everything beneath it —
 * is blurred in place by @p radius pixels (the axl-gfx stack blur) BEFORE this
 * surface is blitted on top. Combine with @c opacity < 255 or
 * axl_surface_set_per_pixel_alpha so the frosted backdrop shows through the
 * translucent tint: a dialog veil with NO back-buffer readback in the
 * consumer. A fully-opaque straight-copy surface (opacity 255 + no per-pixel
 * alpha) would overwrite the frosted backdrop entirely, so backdrop blur is
 * only meaningful on a translucent surface. A backdrop-blur surface is excluded
 * from occlusion culling (it is a translucent overlay). Marks the surface's
 * subtree damaged.
 *
 * A present recomposites such a surface over its FULL rect (not a damage
 * sub-rect), so the blur always clamps at the surface's own edges — for a
 * near-fullscreen veil that is the screen edge, so it is invisible. On a
 * scratch-buffer allocation failure the blur is skipped (the surface is simply
 * un-frosted) — never fatal.
 */
void
axl_surface_set_backdrop_blur(
    AxlSurface  *s,       ///< surface
    uint32_t     radius   ///< backdrop blur radius in pixels (0 = off)
);

/**
 * @brief Whether the surface is shown.
 *
 * @return true if visible (NULL-safe; NULL → false).
 */
bool
axl_surface_visible(
    const AxlSurface  *s   ///< surface (NULL-safe)
);

/**
 * @brief Mark a surface-local rectangle dirty, so the next present
 *        recomposites and flushes it.
 */
void
axl_surface_damage(
    AxlSurface  *s,     ///< surface
    AxlGfxClip   rect   ///< dirty rect in surface-local coordinates
);

/**
 * @brief Get the surface geometry: position relative to its parent and
 *        size. Any out-param may be NULL. NULL-safe.
 */
void
axl_surface_get_geometry(
    const AxlSurface  *s,   ///< surface (NULL-safe)
    int32_t           *x,   ///< [out] x relative to parent (may be NULL)
    int32_t           *y,   ///< [out] y relative to parent (may be NULL)
    uint32_t          *w,   ///< [out] width (may be NULL)
    uint32_t          *h    ///< [out] height (may be NULL)
);

/**
 * @brief Get the surface origin in **output (absolute) coordinates** — the
 *        sum of the (x, y) offsets up the parent chain.
 *
 * Saves a toolkit walking the parent chain itself when placing a submenu /
 * tooltip or doing hit math. The root is at (0, 0). Out-params optional;
 * NULL-safe.
 */
void
axl_surface_get_absolute(
    const AxlSurface  *s,   ///< surface (NULL-safe)
    int32_t           *x,   ///< [out] absolute x (may be NULL)
    int32_t           *y    ///< [out] absolute y (may be NULL)
);

/**
 * @brief Map a surface-local point (@p lx, @p ly) to output coordinates.
 *        Out-params optional; NULL-safe.
 */
void
axl_surface_to_output(
    const AxlSurface  *s,    ///< surface (NULL-safe)
    int32_t            lx,   ///< surface-local x
    int32_t            ly,   ///< surface-local y
    int32_t           *ox,   ///< [out] output x (may be NULL)
    int32_t           *oy    ///< [out] output y (may be NULL)
);

/**
 * @brief Map an output point (@p ox, @p oy) to this surface's local
 *        coordinates (the inverse of axl_surface_to_output). Out-params
 *        optional; NULL-safe.
 */
void
axl_surface_from_output(
    const AxlSurface  *s,    ///< surface (NULL-safe)
    int32_t            ox,   ///< output x
    int32_t            oy,   ///< output y
    int32_t           *lx,   ///< [out] surface-local x (may be NULL)
    int32_t           *ly    ///< [out] surface-local y (may be NULL)
);

// ---------------------------------------------------------------------------
// The seat — pointer routing (Phase C4)
// ---------------------------------------------------------------------------
//
// The compositor embeds one **seat** (Wayland `wl_seat` shape): the
// per-input-focus state that routes raw input to surfaces. C4 covers the
// pointer; C5 adds pointer grabs and per-surface keyboard focus.
//
// A surface receives events through an `AxlSurfaceListener` — a struct of
// function pointers plus a `void *user`, the Wayland
// `wl_pointer_listener`/`wl_keyboard_listener` idiom. It is the seam that
// bridges to C++ inheritance: a toolkit fills one static-trampoline
// listener in a base class, passes `this` as @p user, and dispatches to
// virtual methods — written once, inherited by every window. All event
// coordinates are **surface-local**.

/**
 * @brief Per-surface input listener (the C→C++-virtual seam, §2.3).
 *
 * Every callback is optional (a NULL field is simply not called). All
 * coordinates are surface-local (relative to the surface origin).
 *
 * C4 delivers the pointer callbacks: @p enter, @p leave, @p motion,
 * @p button, @p axis. The @p key / @p focus_in / @p focus_out callbacks
 * are the keyboard-focus seam wired in C5 — declared now so the listener
 * (the expensive-to-change contract a toolkit bridges once) is stable.
 */
typedef struct {
    /// Pointer entered the surface, now at surface-local (@p x, @p y).
    void (*enter)(void *user, int32_t x, int32_t y);
    /// Pointer left the surface.
    void (*leave)(void *user);
    /// Pointer moved to surface-local (@p x, @p y). @p modifiers is the live
    /// AXL_INPUT_MOD_* state from the originating event.
    void (*motion)(void *user, int32_t x, int32_t y, uint32_t modifiers);
    /// A button changed: @p button is the AXL_INPUT_BUTTON_* bit that
    /// transitioned, @p pressed its new state, at surface-local (@p x, @p y).
    /// @p modifiers is the live AXL_INPUT_MOD_* state; @p click_count is the
    /// gesture recognizer's 1/2/3 single/double/triple-click count (0 if not
    /// a click); @p dragging is true once the held-button press has crossed
    /// the drag threshold (for drag-select). If several buttons transition in
    /// one event the callback fires once per changed bit, each carrying the
    /// same per-event @p click_count / @p dragging.
    void (*button)(void *user, uint32_t button, bool pressed,
                   int32_t x, int32_t y,
                   uint32_t modifiers, uint32_t click_count, bool dragging);
    /// Scroll/axis motion by (@p dx, @p dy) notch ticks. @p modifiers is the
    /// live AXL_INPUT_MOD_* state (e.g. Shift+wheel for horizontal scroll).
    void (*axis)(void *user, int32_t dx, int32_t dy, uint32_t modifiers);
    /// Key event routed to the keyboard-focus surface (C5).
    void (*key)(void *user, const AxlInputEvent *ev);
    /// Surface gained keyboard focus (C5).
    void (*focus_in)(void *user);
    /// Surface lost keyboard focus (C5).
    void (*focus_out)(void *user);
} AxlSurfaceListener;

/**
 * @brief Install the surface's input listener (copied by value) and its
 *        @p user pointer. Pass @p listener == NULL to clear it.
 *
 * The struct contents are copied, so a transient (stack) listener is fine;
 * @p user is borrowed and must outlive the surface's event delivery.
 */
void
axl_surface_set_listener(
    AxlSurface                *s,          ///< surface
    const AxlSurfaceListener  *listener,   ///< listener (copied; NULL clears)
    void                      *user        ///< borrowed context passed to callbacks
);

/**
 * @brief Set the surface's input region — the part that catches pointer
 *        events; the rest is input-transparent (events pass through to
 *        whatever is beneath).
 *
 * Wayland `set_input_region` semantics:
 *   - @p clips == NULL → the full surface rect catches input (the default).
 *   - @p clips != NULL, @p n == 0 → the surface is fully input-transparent
 *     (every event passes through — the dim-veil pass-through case).
 *   - otherwise → the union of the @p n surface-local rects is input-opaque.
 *
 * The rects are copied (the caller's array need not persist). The input
 * region is independent of opacity and the @c opaque hint — a fully
 * transparent (opacity 0) surface still catches input unless its region
 * says otherwise.
 */
void
axl_surface_set_input_region(
    AxlSurface        *s,      ///< surface
    const AxlGfxClip  *clips,  ///< surface-local input rects (NULL = full rect)
    size_t             n       ///< number of rects (0 with non-NULL = transparent)
);

/**
 * @brief Feed one raw pointer event into the seat, which hit-tests the
 *        surface tree topmost-first and dispatches to the surface under
 *        the pointer (surface-local) via its listener.
 *
 * @p ev->x / @p ev->y are interpreted as **output (absolute) coordinates**
 * and clamped to the output. Handles AXL_INPUT_MOUSE_MOVE / _BUTTON_DOWN /
 * _BUTTON_UP / _WHEEL; other event types are ignored (key routing is C5).
 * Crossing a surface boundary emits @c leave on the old surface and
 * @c enter on the new one. This is the testable routing core;
 * axl_compositor_attach_pointer drives it from a real device.
 */
void
axl_compositor_pointer_event(
    AxlCompositor        *c,   ///< compositor
    const AxlInputEvent  *ev   ///< pointer event (output coordinates)
);

/**
 * @brief Attach a mouse device on @p loop as the seat's pointer source.
 *
 * Registers `EFI_SIMPLE_POINTER_PROTOCOL` via axl_input_attach_mouse;
 * each device event is converted from accumulated relative motion to a
 * clamped output position (seeded at the output centre) and fed to
 * axl_compositor_pointer_event. The @p loop is owned by the caller (the
 * seat only adds a source).
 *
 * Only one mouse source exists per process, so do not combine this with
 * axl_cursor_attach in the same process (C6 unifies the cursor overlay
 * with the seat). NULL-safe.
 *
 * @return the axl-loop source ID (for axl_loop_remove_source), or 0 on
 *     failure (NULL args, no pointer protocol, or a mouse already attached).
 */
uint32_t
axl_compositor_attach_pointer(
    AxlCompositor  *c,     ///< compositor
    AxlLoop        *loop   ///< caller-owned event loop
);

/**
 * @brief Detach the seat's pointer source from @p loop (inverse of
 *        axl_compositor_attach_pointer). NULL-safe and idempotent.
 */
void
axl_compositor_detach_pointer(
    AxlCompositor  *c,     ///< compositor
    AxlLoop        *loop   ///< the loop the pointer was attached to
);

// ---------------------------------------------------------------------------
// The seat — pointer grabs + keyboard focus (Phase C5)
// ---------------------------------------------------------------------------
//
// A **pointer grab** is the menu / popup mechanism: while a grab is active
// the pointer focus is confined to the grabbing surface's subtree (§2.1) —
// a hit inside the subtree routes normally (so submenus and items still get
// enter/leave/motion/button), a hit outside un-focuses (a leave, no enter).
// Grabs nest as a LIFO stack. A **button press outside the grab subtree** is
// the dismiss signal: the seat pops the grab and invokes its dismiss
// callback, and the toolkit closes the popup chain. No serial-auth gates
// grabs (one trusted in-process client — §2.3).
//
// **Keyboard focus** is per-surface: the seat tracks which surface has the
// key focus and routes key events there. (This is the *surface* focus; the
// within-surface widget focus / tab traversal stays in the toolkit — §4.)
// The firmware owns the keymap and repeat, so the seat just routes the raw
// AxlInputEvent to the focused surface's `key` listener.

/// Called when a pointer grab is dismissed by a button press outside the
/// grabbing surface's subtree. The grab has already been popped when this
/// runs, so the callback may safely re-grab / close popups.
typedef void (*AxlGrabDismissFunc)(
    void *user   ///< the @p user passed to axl_compositor_pointer_grab
);

/// Maximum nesting depth of the pointer-grab stack (a deep popup chain).
#define AXL_COMPOSITOR_GRAB_MAX  16

/**
 * @brief Push an EXCLUSIVE pointer grab for @p s (the top of the LIFO grab
 *        stack) — the modal case.
 *
 * While @p s's grab is active, pointer focus is confined strictly to @p s's
 * subtree; everything below it on the stack is suspended (a modal dialog over
 * a menu blocks the menu). A button press outside @p s's subtree pops this
 * grab and calls @p on_dismiss. No-op if @p s is NULL / the root / a different
 * compositor, or if the grab stack is full (AXL_COMPOSITOR_GRAB_MAX deep).
 *
 * For a popup CHAIN (a menu and its submenus, where the whole open chain stays
 * interactive) use axl_compositor_pointer_grab_chain instead.
 */
void
axl_compositor_pointer_grab(
    AxlCompositor       *c,           ///< compositor
    AxlSurface          *s,           ///< surface to grab the pointer
    AxlGrabDismissFunc   on_dismiss,  ///< dismissed-by-outside-click callback (may be NULL)
    void                *user         ///< borrowed context for @p on_dismiss
);

/**
 * @brief Push a CHAIN pointer grab for @p s — a popup chain (menu bar → menu →
 *        submenu, each a sibling surface).
 *
 * Like axl_compositor_pointer_grab, but the confinement EXTENDS the grab below
 * it instead of replacing it: the active region is the union of @p s's subtree
 * and the contiguous chain grabs beneath it on the stack, stopping at (and
 * including) the first EXCLUSIVE grab. So hover and clicks route freely across
 * the whole open chain — no manual cross-surface routing in the toolkit — while
 * a press outside ALL of it pops the top grab and calls @p on_dismiss (the
 * toolkit then closes the chain). A later exclusive grab (a modal opened over
 * the chain) still blocks it. A lone chain grab confines to its own subtree,
 * exactly like an exclusive one.
 *
 * Same no-op conditions as axl_compositor_pointer_grab.
 */
void
axl_compositor_pointer_grab_chain(
    AxlCompositor       *c,           ///< compositor
    AxlSurface          *s,           ///< surface to grab the pointer (chain member)
    AxlGrabDismissFunc   on_dismiss,  ///< dismissed-by-outside-click callback (may be NULL)
    void                *user         ///< borrowed context for @p on_dismiss
);

/**
 * @brief Pop the top pointer grab (inverse of axl_compositor_pointer_grab).
 *        No-op if no grab is active. Re-hit-tests under the pointer.
 */
void
axl_compositor_pointer_ungrab(
    AxlCompositor  *c   ///< compositor
);

/**
 * @brief Set the surface that receives keyboard events (NULL = none).
 *
 * Emits @c focus_out on the previously focused surface and @c focus_in on
 * @p s via their listeners. No-op if @p s belongs to a different compositor.
 */
void
axl_compositor_set_keyboard_focus(
    AxlCompositor  *c,   ///< compositor
    AxlSurface     *s    ///< surface to focus (NULL clears focus)
);

/**
 * @brief The surface with keyboard focus, or NULL if none.
 *
 * @return the focused surface (NULL-safe; NULL → NULL).
 */
AxlSurface *
axl_compositor_keyboard_focus(
    const AxlCompositor  *c   ///< compositor (NULL-safe)
);

/**
 * @brief Route a key event to the keyboard-focus surface's @c key listener.
 *
 * Handles AXL_INPUT_KEY_DOWN / _KEY_UP; other event types are ignored. The
 * raw @p ev is forwarded unchanged (the firmware owns keymap and repeat).
 * No-op if no surface has focus. This is the testable routing core;
 * axl_compositor_attach_keyboard drives it from a real keyboard.
 */
void
axl_compositor_key_event(
    AxlCompositor        *c,   ///< compositor
    const AxlInputEvent  *ev   ///< key event
);

/**
 * @brief Attach a keyboard device on @p loop as the seat's key source.
 *
 * Registers the keyboard via axl_input_attach_key; each event is routed to
 * the keyboard-focus surface through axl_compositor_key_event. The @p loop
 * is owned by the caller. Only one keyboard source exists per process.
 * NULL-safe.
 *
 * @return the axl-loop source ID (for axl_loop_remove_source), or 0 on
 *     failure (NULL args or a keyboard already attached).
 */
uint32_t
axl_compositor_attach_keyboard(
    AxlCompositor  *c,     ///< compositor
    AxlLoop        *loop   ///< caller-owned event loop
);

/**
 * @brief Detach the seat's keyboard source from @p loop (inverse of
 *        axl_compositor_attach_keyboard). NULL-safe and idempotent.
 */
void
axl_compositor_detach_keyboard(
    AxlCompositor  *c,     ///< compositor
    AxlLoop        *loop   ///< the loop the keyboard was attached to
);

// ---------------------------------------------------------------------------
// The cursor (the top overlay) — Phase C6
// ---------------------------------------------------------------------------
//
// The compositor owns one AxlCursor (see <axl/axl-cursor.h>) bound to its
// output buffer — the topmost overlay, never hit-tested, moving at pointer
// rate. The seat drives it: a pointer event moves (and shows) the cursor,
// and axl_compositor_present brackets its GOP flush with the cursor's
// lift/drop so the cursor stays on top with no trail. The cursor never
// writes the output buffer (it composites only to the screen), so reading
// axl_compositor_output never shows cursor pixels.

/**
 * @brief The compositor's cursor overlay (bound to the output, driven from
 *        the seat pointer). Owned by the compositor.
 *
 * Exposed for direct show/hide/position queries; most consumers only set the
 * shape via axl_compositor_set_cursor_image and let the seat drive position.
 *
 * @return the cursor, or NULL (NULL @p c, or no cursor was created).
 */
AxlCursor *
axl_compositor_cursor(
    AxlCompositor  *c   ///< compositor (NULL-safe)
);

/**
 * @brief Set the pointer's cursor image and hotspot — the per-surface shape
 *        (§2.4). Forwards to axl_cursor_set_image on the compositor's cursor.
 *
 * The seat resets to the built-in arrow on every pointer-focus change, so a
 * surface chooses its shape by calling this from its listener @c enter
 * callback (an I-beam over a text field, a resize arrow over a border). Pass
 * a NULL @p sprite for the built-in arrow.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments / no cursor /
 *     allocation failure.
 */
int
axl_compositor_set_cursor_image(
    AxlCompositor       *c,        ///< compositor
    const AxlGfxBuffer  *sprite,   ///< RGBA sprite (copied); NULL = built-in arrow
    int32_t              hot_x,    ///< hotspot x within the sprite
    int32_t              hot_y     ///< hotspot y within the sprite
);

// ---------------------------------------------------------------------------
// Frame callbacks — present throttling (Phase E7)
// ---------------------------------------------------------------------------
//
// A toolkit that animates (transitions, a blinking caret) must redraw at a
// sensible cadence and go **idle** when nothing animates — not busy-present.
// This is Wayland's `wl_surface.frame`: request a one-shot callback, get woken
// at a good time, draw + present + re-request to keep going. Adapted to this
// single-client synchronous compositor, "a good time" is a target interval
// elapsing, driven by a self-cancelling loop timer (the **frame clock**).
//
// The callbacks are **time-based**: each carries a monotonic millisecond
// timestamp, so animation computes elapsed time and stays correct even if the
// tick cadence jitters. axl_compositor_dispatch_frame is the testable routing
// core (drive it from any clock); axl_compositor_attach_frame_clock wires it
// to an AxlLoop timer that ticks only while callbacks are pending.

/**
 * @brief One-shot frame callback (Wayland `wl_surface.frame`).
 *
 * @p time_ms is the monotonic frame timestamp (the axl_time_get_ms epoch) —
 * use the delta between successive callbacks to advance animation.
 */
typedef void (*AxlFrameCallback)(
    void      *user,     ///< the @p user passed to axl_surface_request_frame
    uint64_t   time_ms   ///< monotonic frame time in milliseconds
);

/**
 * @brief Request a one-shot frame callback for @p s (Wayland
 *        `wl_surface.frame`).
 *
 * Fired once at the next frame tick, then cleared; **re-request inside the
 * callback** to keep animating (that is the throttle — one redraw per tick).
 * A second request before the tick replaces the pending callback (latest
 * wins); passing @p cb == NULL cancels a pending request. Allowed on any
 * surface including the compositor root (a screen-wide transition holder).
 * Destroying the surface cancels its pending callback (it is never fired with
 * a stale @p user).
 *
 * The callback is delivered by axl_compositor_dispatch_frame — either the
 * attached frame clock (axl_compositor_attach_frame_clock) or a direct call.
 * Requesting with no frame clock attached simply leaves the callback pending
 * until something dispatches.
 */
void
axl_surface_request_frame(
    AxlSurface        *s,      ///< surface (NULL-safe)
    AxlFrameCallback   cb,     ///< callback (NULL cancels a pending request)
    void              *user    ///< borrowed context passed to @p cb
);

/**
 * @brief Fire every frame callback pending at entry with @p time_ms, clearing
 *        them — the testable routing core (drive it from any clock).
 *
 * Snapshots the callbacks pending at entry, clears them, then fires each, so a
 * **re-request made inside a callback queues for the NEXT dispatch**, not this
 * one (matching Wayland). An allocation failure skips the frame (callbacks
 * stay pending for the next dispatch) rather than dropping them.
 *
 * Precondition: a callback may mutate surface content, mark damage, present,
 * and re-request frames, but must **not create or destroy surfaces** — defer
 * teardown past the present, exactly as a Wayland client never frees a surface
 * inside its own frame handler.
 *
 * @return true if any callback is pending after this dispatch (i.e. a callback
 *     re-requested, or an allocation failure left some unfired) — the frame
 *     clock uses this to decide whether to keep ticking. NULL-safe (→ false).
 */
bool
axl_compositor_dispatch_frame(
    AxlCompositor  *c,        ///< compositor
    uint64_t        time_ms   ///< monotonic frame time passed to each callback
);

/**
 * @brief Whether any surface has a pending frame callback.
 *
 * @return true if a frame is pending (NULL-safe; NULL → false).
 */
bool
axl_compositor_has_pending_frames(
    const AxlCompositor  *c   ///< compositor (NULL-safe)
);

/**
 * @brief Attach the compositor's frame clock to @p loop — the present
 *        throttle.
 *
 * Adds a self-cancelling repeating timer on @p loop: each tick calls
 * axl_compositor_dispatch_frame with the current axl_time_get_ms, and the
 * timer removes itself once no callbacks remain pending (idle = no wakeups).
 * A later axl_surface_request_frame re-arms it. @p interval_ms is the target
 * frame period (0 → a ~60 fps default). The @p loop is caller-owned (the clock
 * only adds a source).
 *
 * The returned value is a non-zero loop source id on success, but the clock
 * manages its own timer lifecycle (lazy re-arm, self-cancel), so stop it via
 * axl_compositor_detach_frame_clock — do **not** pass the id to
 * axl_loop_remove_source yourself. Detach (or free the compositor) before the
 * loop, so no tick fires into freed state.
 *
 * @return a non-zero source id on success, or 0 on failure (NULL args / the
 *     timer could not be added).
 */
uint32_t
axl_compositor_attach_frame_clock(
    AxlCompositor  *c,           ///< compositor
    AxlLoop        *loop,        ///< caller-owned event loop
    uint32_t        interval_ms  ///< target frame period in ms (0 = ~60 fps)
);

/**
 * @brief Detach the frame clock from @p loop (inverse of
 *        axl_compositor_attach_frame_clock). NULL-safe and idempotent.
 */
void
axl_compositor_detach_frame_clock(
    AxlCompositor  *c,     ///< compositor
    AxlLoop        *loop   ///< the loop the frame clock was attached to
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_COMPOSITOR_H */
