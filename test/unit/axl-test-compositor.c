/** @file axl-test-compositor.c
    Unit tests for AxlCompositor (Phase C1 — surfaces + composite + present).

    Compositing happens into a RAM output buffer, so the stacking,
    positioning, visibility and damage logic is fully testable here
    without a GOP. The on-screen present path gets a separate GOP
    integration test (compositor-selftest.c).
*/

#include "axl-test.h"
#include <axl/axl-compositor.h>
#include <axl/axl-cursor.h>
#include <axl/axl-gfx.h>
#include <axl/axl-loop.h>

#define SW 100
#define SH 80

/* Read one output pixel. */
static AxlGfxPixel
scan_at(AxlCompositor *c, int32_t x, int32_t y)
{
    AxlGfxPixel *px = axl_gfx_buffer_pixels(axl_compositor_output(c));
    return px[y * SW + x];
}

static bool
rgb_eq(AxlGfxPixel a, AxlGfxPixel b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

/* Fill a surface's whole buffer with one color. */
static void
fill_surface(AxlSurface *s, uint32_t w, uint32_t h, AxlGfxPixel color)
{
    axl_gfx_target_buffer(axl_surface_buffer(s));
    axl_gfx_fill_rect(0, 0, w, h, color);
    axl_gfx_target_buffer(NULL);
}

static void
test_compositor_lifecycle(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    test_check(c != NULL, "compositor: created");
    test_check(axl_compositor_root(c) != NULL, "compositor: has a root");
    test_check(axl_surface_buffer(axl_compositor_root(c)) == NULL,
               "compositor: root has no buffer");
    uint32_t w = 0, h = 0;
    test_check(axl_gfx_buffer_get_info(axl_compositor_output(c), &w, &h) == AXL_OK
               && w == SW && h == SH, "compositor: output buffer is the right size");

    AxlSurface *s = axl_surface_create(axl_compositor_root(c), 40, 30);
    test_check(s != NULL, "compositor: surface created");
    int32_t x = -1, y = -1; uint32_t sw = 0, sh = 0;
    axl_surface_get_geometry(s, &x, &y, &sw, &sh);
    test_check(x == 0 && y == 0 && sw == 40 && sh == 30,
               "compositor: surface starts at (0,0) sized 40x30");
    test_check(axl_surface_visible(s), "compositor: surface starts visible");
    test_check(axl_surface_buffer(s) != NULL, "compositor: surface has a buffer");

    /* NULL / root-guard safety. */
    test_check(axl_surface_create(NULL, 10, 10) == NULL, "compositor: create(NULL parent)");
    test_check(axl_surface_create(axl_compositor_root(c), 0, 10) == NULL,
               "compositor: create(zero w)");
    axl_surface_destroy(NULL);
    axl_compositor_free(c);
}

static void
test_compositor_stacking(void)
{
    const AxlGfxPixel BG  = AXL_GFX_RGB(0x00, 0x00, 0x00);
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x10, 0xE0, 0x10);

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);  /* bottom */
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);  /* top    */
    axl_surface_move(a, 10, 10);   /* covers x,y 10..40 */
    axl_surface_move(b, 25, 25);   /* covers x,y 25..55, overlaps a on 25..40 */
    fill_surface(a, 30, 30, RED);
    fill_surface(b, 30, 30, GRN);

    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 5, 5), BG),    "stack: uncovered area is background");
    test_check(rgb_eq(scan_at(c, 15, 15), RED), "stack: A-only area is red");
    test_check(rgb_eq(scan_at(c, 50, 50), GRN), "stack: B-only area is green");
    test_check(rgb_eq(scan_at(c, 30, 30), GRN), "stack: overlap shows B (created later = top)");

    /* Hide the top surface: the overlap now shows the bottom one. */
    axl_surface_set_visible(b, false);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 30, 30), RED), "stack: hiding B reveals A in the overlap");
    test_check(rgb_eq(scan_at(c, 50, 50), BG),  "stack: B-only area falls back to background");

    /* Move A; its old cells clear, new cells paint. */
    axl_surface_set_visible(b, true);
    axl_surface_move(a, 60, 5);    /* now covers 60..90 x, 5..35 y */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 15, 15), BG),  "stack: A's old position cleared after move");
    test_check(rgb_eq(scan_at(c, 70, 15), RED), "stack: A painted at its new position");

    axl_compositor_free(c);
}

static void
test_compositor_nested(void)
{
    const AxlGfxPixel BG  = AXL_GFX_RGB(0x00, 0x00, 0x00);
    const AxlGfxPixel BLU = AXL_GFX_RGB(0x10, 0x10, 0xE0);
    const AxlGfxPixel YEL = AXL_GFX_RGB(0xE0, 0xE0, 0x10);

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *parent = axl_surface_create(axl_compositor_root(c), 40, 40);
    axl_surface_move(parent, 20, 20);          /* parent abs 20..60 */
    AxlSurface *child = axl_surface_create(parent, 20, 20);
    axl_surface_move(child, 5, 5);             /* child abs = 25..45 (relative!) */
    fill_surface(parent, 40, 40, BLU);
    fill_surface(child, 20, 20, YEL);

    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 22, 22), BLU), "nested: parent shows where the child isn't");
    test_check(rgb_eq(scan_at(c, 30, 30), YEL), "nested: child paints over parent (child is on top)");

    /* Moving the parent carries the child (child position is relative). */
    axl_surface_move(parent, 50, 10);          /* parent 50..90; child abs 55..75, 15..35 */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 22, 22), BG),  "nested: parent's old area cleared");
    test_check(rgb_eq(scan_at(c, 52, 12), BLU), "nested: parent at new position");
    test_check(rgb_eq(scan_at(c, 60, 25), YEL), "nested: child followed the parent (relative pos)");

    /* Destroy the parent: the child dies with it; the area clears. */
    axl_surface_destroy(parent);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 52, 12), BG), "nested: destroying parent removes the subtree");
    test_check(rgb_eq(scan_at(c, 60, 25), BG), "nested: child gone with parent");

    axl_compositor_free(c);
}

static void
test_compositor_offedge(void)
{
    const AxlGfxPixel BG  = AXL_GFX_RGB(0x00, 0x00, 0x00);
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x10, 0xE0, 0x10);

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *s = axl_surface_create(axl_compositor_root(c), 30, 30);
    /* Left 15 cols red, right 15 cols green — so the source-column offset
       is observable. */
    axl_gfx_target_buffer(axl_surface_buffer(s));
    axl_gfx_fill_rect(0, 0, 15, 30, RED);
    axl_gfx_fill_rect(15, 0, 15, 30, GRN);
    axl_gfx_target_buffer(NULL);

    /* Push 10 cols off the left and 5 rows off the top. Visible part is
       source cols 10..30 → screen cols 0..20. So screen col 0 = source
       col 10 (still red), screen col 8 = source col 18 (green). A plain
       (no off-edge) blit would show red at both — this pins the source
       offset. */
    axl_surface_move(s, -10, -5);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 0, 0), RED),  "offedge: screen col 0 maps to source col 10 (red)");
    test_check(rgb_eq(scan_at(c, 8, 0), GRN),  "offedge: screen col 8 maps to source col 18 (green)");
    test_check(rgb_eq(scan_at(c, 0, 24), RED), "offedge: bottom of the visible strip still maps correctly");
    test_check(rgb_eq(scan_at(c, 20, 0), BG),  "offedge: past the visible width is background");
    test_check(rgb_eq(scan_at(c, 0, 25), BG),  "offedge: past the visible height is background");

    axl_compositor_free(c);
}

static void
test_compositor_raise_lower(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x10, 0xE0, 0x10);

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(a, 10, 10);
    axl_surface_move(b, 10, 10);   /* exactly over A — B created later, so on top */
    fill_surface(a, 30, 30, RED);
    fill_surface(b, 30, 30, GRN);

    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 15, 15), GRN), "raise: B on top initially (creation order)");

    axl_surface_raise(a);          /* A to top of siblings */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 15, 15), RED), "raise: A on top after raise(A)");

    axl_surface_lower(a);          /* A back to bottom */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 15, 15), GRN), "raise: B on top again after lower(A)");

    /* NULL / root are no-ops (no crash). */
    axl_surface_raise(NULL);
    axl_surface_lower(NULL);
    axl_surface_raise(axl_compositor_root(c));
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 15, 15), GRN), "raise: NULL/root no-ops leave stacking intact");

    axl_compositor_free(c);
}

static void
test_compositor_reparent(void)
{
    const AxlGfxPixel BG  = AXL_GFX_RGB(0x00, 0x00, 0x00);
    const AxlGfxPixel BLU = AXL_GFX_RGB(0x10, 0x10, 0xE0);
    const AxlGfxPixel YEL = AXL_GFX_RGB(0xE0, 0xE0, 0x10);

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *p = axl_surface_create(axl_compositor_root(c), 40, 40);
    AxlSurface *q = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(p, 20, 20);   /* P abs 20..60 */
    axl_surface_move(q, 5, 5);     /* Q abs 5..25 (top-level) */
    fill_surface(p, 40, 40, BLU);
    fill_surface(q, 20, 20, YEL);

    /* Reparent Q under P: its (5,5) is now relative to P → abs (25,25). */
    axl_surface_set_parent(q, p);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 7, 7), BG),    "reparent: Q's old top-level spot is empty");
    test_check(rgb_eq(scan_at(c, 30, 30), YEL), "reparent: Q now at parent-relative abs (25..45)");

    /* Moving P carries Q. */
    axl_surface_move(p, 50, 10);   /* P abs 50..90; Q abs 55..75, 15..35 */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 60, 20), YEL), "reparent: Q followed P after move");
    test_check(rgb_eq(scan_at(c, 52, 12), BLU), "reparent: P visible where Q doesn't cover");

    /* Cycle reject: P cannot become a child of its own descendant Q. */
    axl_surface_set_parent(p, q);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 52, 12), BLU), "reparent: cycle rejected, tree unchanged");

    axl_compositor_free(c);
}

static void
test_compositor_occlusion(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x10, 0xE0, 0x10);
    const AxlGfxPixel BLU = AXL_GFX_RGB(0x10, 0x10, 0xE0);

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* full, bottom */
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), SW, SH);  /* full, top */
    fill_surface(b, SW, SH, BLU);
    fill_surface(a, SW, SH, RED);

    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 2, "occlusion: both painted by default");
    test_check(rgb_eq(scan_at(c, 50, 40), RED), "occlusion: top surface shows");

    /* A is opaque and fully covers the output → B beneath is culled. The
       result is identical (culling is invisible), but the work drops. */
    axl_surface_set_opaque(a, true);
    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 1, "occlusion: full-cover opaque culls the surface beneath");
    test_check(rgb_eq(scan_at(c, 50, 40), RED), "occlusion: output unchanged by culling");

    /* A partial opaque surface must NOT cull (no over-cull): make A
       non-opaque, add a half-width opaque D on top. No full-cover opaque
       exists, so all three paint and both halves render correctly. */
    axl_surface_set_opaque(a, false);
    AxlSurface *d = axl_surface_create(axl_compositor_root(c), SW / 2, SH);
    fill_surface(d, SW / 2, SH, GRN);
    axl_surface_set_opaque(d, true);
    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 3, "occlusion: partial opaque does not cull below");
    test_check(rgb_eq(scan_at(c, SW / 4, 40), GRN), "occlusion: left half shows the partial surface");
    test_check(rgb_eq(scan_at(c, 3 * SW / 4, 40), RED), "occlusion: right half shows the surface below it");

    /* A HIDDEN full-cover opaque surface must not cull — it isn't drawn,
       so the surfaces beneath it stay visible. Hide A and D, make A a
       full-cover opaque (but hidden); only B should paint. */
    axl_surface_set_visible(d, false);
    axl_surface_set_visible(a, false);
    axl_surface_set_opaque(a, true);
    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 1, "occlusion: hidden opaque floor does not cull");
    test_check(rgb_eq(scan_at(c, 50, 40), BLU), "occlusion: surface beneath a hidden floor shows");

    axl_compositor_free(c);
}

static void
test_compositor_opacity(void)
{
    const AxlGfxPixel RED  = AXL_GFX_RGB(0xC0, 0x20, 0x20);
    const AxlGfxPixel VEIL = AXL_GFX_RGB(0x10, 0x10, 0x10);  /* dim */

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* backdrop */
    AxlSurface *v = axl_surface_create(axl_compositor_root(c), SW, SH);  /* veil, top */
    fill_surface(b, SW, SH, RED);
    fill_surface(v, SW, SH, VEIL);

    /* 50% veil: each output pixel = source-over of VEIL@128 over RED,
       through the gamma-aware composite the compositor uses. */
    axl_surface_set_opacity(v, 128);
    axl_compositor_composite(c);
    AxlGfxPixel expect = axl_gfx_composite(RED, AXL_GFX_RGBA(0x10, 0x10, 0x10, 128));
    AxlGfxPixel got = scan_at(c, 50, 40);
    test_check(rgb_eq(got, expect), "opacity: 50% veil = source-over composite");
    test_check(!rgb_eq(got, RED) && !rgb_eq(got, VEIL),
               "opacity: blended pixel is neither the backdrop nor the veil");

    /* The veil is gamma-correct: enabling gamma-correct mode changes the
       result (and matches the gamma-aware composite, not the sRGB blend). */
    bool prev_gamma = axl_gfx_get_gamma_correct();
    axl_gfx_set_gamma_correct(true);
    axl_compositor_composite(c);
    AxlGfxPixel got_g  = scan_at(c, 50, 40);
    AxlGfxPixel lin    = axl_gfx_composite(RED, AXL_GFX_RGBA(0x10, 0x10, 0x10, 128));
    AxlGfxPixel srgb   = axl_gfx_blend(RED, AXL_GFX_RGBA(0x10, 0x10, 0x10, 128));
    test_check(rgb_eq(got_g, lin), "opacity: gamma-on veil matches the gamma-aware composite");
    test_check(!rgb_eq(got_g, srgb), "opacity: gamma-on veil differs from the sRGB blend");
    axl_gfx_set_gamma_correct(prev_gamma);   /* restore global state */

    /* 255 = opaque copy: the veil fully replaces the backdrop. */
    axl_surface_set_opacity(v, 255);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 50, 40), VEIL), "opacity: 255 is an opaque copy");

    /* 0 = fully transparent: the backdrop shows through unchanged. */
    axl_surface_set_opacity(v, 0);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 50, 40), RED), "opacity: 0 is fully transparent");

    /* A translucent full-cover surface must NOT occlude the backdrop. */
    axl_surface_set_opacity(v, 128);
    axl_surface_set_opaque(v, true);   /* even if (mistakenly) flagged opaque */
    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 2,
               "opacity: translucent surface never culls beneath it");

    axl_compositor_free(c);
}

/* Per-pixel alpha at full opacity: an opaque body, a transparent gap, and a
   half-alpha edge in one surface, blended over an opaque backdrop. The popup /
   dropdown / tooltip case (opaque panel + soft drop shadow). */
static void
test_compositor_per_pixel_alpha(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xC0, 0x20, 0x20);   /* opaque backdrop */
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x20, 0xC0, 0x20);   /* opaque body     */

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* backdrop */
    AxlSurface *p = axl_surface_create(axl_compositor_root(c), SW, SH);  /* top      */
    fill_surface(b, SW, SH, RED);

    /* Paint the top surface by raw pixel writes: a transparent field with one
       opaque body pixel, one fully transparent pixel, one half-alpha pixel. */
    AxlGfxPixel *pb = axl_gfx_buffer_pixels(axl_surface_buffer(p));
    for (int i = 0; i < SW * SH; i++) pb[i] = AXL_GFX_RGBA(0, 0, 0, 0);
    pb[10 * SW + 10] = AXL_GFX_RGBA(0x20, 0xC0, 0x20, 255);  /* opaque body  */
    pb[40 * SW + 50] = AXL_GFX_RGBA(0, 0, 0, 0);             /* transparent  */
    pb[20 * SW + 20] = AXL_GFX_RGBA(0x20, 0xC0, 0x20, 128);  /* half-alpha   */

    /* Default (per-pixel alpha OFF) at opacity 255 is a straight copy — even
       the transparent pixel is copied raw (rgb 0,0,0), NOT blended. */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 50, 40), AXL_GFX_RGB(0, 0, 0)),
               "ppa off: opacity 255 straight-copies even transparent pixels");

    /* Enable per-pixel alpha: body replaces, gap shows the backdrop, edge
       blends — all at full opacity, no whole-surface dimming. */
    axl_surface_set_per_pixel_alpha(p, true);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 10, 10), GRN),
               "ppa: opaque (alpha 255) body pixel replaces the backdrop");
    test_check(rgb_eq(scan_at(c, 50, 40), RED),
               "ppa: transparent (alpha 0) pixel shows the backdrop through");
    AxlGfxPixel expect = axl_gfx_composite(RED, AXL_GFX_RGBA(0x20, 0xC0, 0x20, 128));
    test_check(rgb_eq(scan_at(c, 20, 20), expect),
               "ppa: half-alpha pixel is source-over of body over backdrop");

    /* A full-cover per-pixel-alpha surface (even mistakenly flagged opaque)
       must NOT occlude the backdrop — its gaps are see-through. */
    axl_surface_set_opaque(p, true);
    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 2,
               "ppa: a per-pixel-alpha surface never culls beneath it");

    axl_compositor_free(c);
}

static void
test_compositor_damage(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(a, 10, 10);
    fill_surface(a, 20, 20, AXL_GFX_RGB(0x80, 0x80, 0x80));

    /* present() composites + flushes + clears the accumulated damage. (The
       return is GOP-dependent — AXL_ERR on a headless arch — so we assert
       the arch-independent accounting: damage is consumed, and a present
       with nothing pending is a no-op AXL_OK. The on-screen flush is the
       GOP integration test's job.) */
    (void)axl_compositor_present(c);
    AxlGfxClip d;
    test_check(axl_compositor_get_damage(c, &d) == AXL_ERR,
               "damage: present clears the damage");
    test_check(axl_compositor_present(c) == AXL_OK,
               "damage: present with nothing pending is a no-op AXL_OK");

    /* A move damages the UNION of the old and new screen rects. */
    axl_surface_move(a, 50, 40);
    test_check(axl_compositor_get_damage(c, &d) == AXL_OK,
               "damage: move marks damage");
    /* old rect (10,10,20,20) ∪ new rect (50,40,20,20) = (10,10)..(70,60). */
    test_check(d.x == 10 && d.y == 10 && d.w == 60 && d.h == 50,
               "damage: move damages union(old, new)");

    /* A surface-local damage rect maps to the surface's screen position. */
    (void)axl_compositor_present(c);
    axl_surface_damage(a, (AxlGfxClip){2, 3, 5, 6});   /* local */
    test_check(axl_compositor_get_damage(c, &d) == AXL_OK
               && d.x == 52 && d.y == 43 && d.w == 5 && d.h == 6,
               "damage: surface-local damage maps to screen coords");

    /* NULL-safety. */
    test_check(axl_compositor_get_damage(NULL, &d) == AXL_ERR, "damage: NULL compositor");
    test_check(axl_compositor_get_damage(c, NULL) == AXL_ERR, "damage: NULL out");

    axl_compositor_free(c);
}

/* E2 — damage is a precise REGION, not a bounding box: disjoint changes
   flush as separate rects (the win for sparse updates). The bbox accessor
   (get_damage) is unchanged; get_damage_region exposes the precision. */
static void
test_compositor_damage_region(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 10, 10);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 10, 10);
    axl_surface_move(a, 0, 0);
    axl_surface_move(b, 60, 60);
    (void)axl_compositor_present(c);   /* consume create/move damage */

    /* Two damages far apart on screen stay TWO rects, not one spanning box. */
    axl_surface_damage(a, (AxlGfxClip){0, 0, 5, 5});   /* screen (0,0,5,5) */
    axl_surface_damage(b, (AxlGfxClip){0, 0, 5, 5});   /* screen (60,60,5,5) */
    const AxlGfxRegion *r = axl_compositor_get_damage_region(c);
    test_check(r != NULL, "damage-region: accessor is non-NULL");
    test_check(axl_gfx_region_num_rects(r) == 2,
               "damage-region: two disjoint damages stay 2 rects (not a bbox)");
    AxlGfxClip d;
    test_check(axl_compositor_get_damage(c, &d) == AXL_OK
               && d.x == 0 && d.y == 0 && d.w == 65 && d.h == 65,
               "damage-region: bbox accessor still spans both");

    /* A disjoint move damages old + new as SEPARATE rects. */
    (void)axl_compositor_present(c);
    axl_surface_move(a, 50, 40);   /* (0,0,10,10) -> (50,40,10,10), disjoint */
    r = axl_compositor_get_damage_region(c);
    test_check(axl_gfx_region_num_rects(r) == 2,
               "damage-region: disjoint move -> old + new as 2 rects");

    /* present clears the region. */
    (void)axl_compositor_present(c);
    r = axl_compositor_get_damage_region(c);
    test_check(axl_gfx_region_is_empty(r), "damage-region: present clears the region");

    test_check(axl_compositor_get_damage_region(NULL) == NULL,
               "damage-region: NULL compositor -> NULL");
    axl_compositor_free(c);
}

/* E3a — present recomposites ONLY the damaged region, not the whole scene.
   A surface whose area is not damaged is not re-blitted; its pixels are
   retained from the prior frame. */
static void
test_compositor_incremental_composite(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel BLU = AXL_GFX_RGB(0x10, 0x10, 0xE0);
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 10, 10);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 10, 10);
    axl_surface_move(a, 0, 0);
    axl_surface_move(b, 60, 60);
    fill_surface(a, 10, 10, RED);
    fill_surface(b, 10, 10, BLU);
    (void)axl_compositor_present(c);   /* initial: both areas damaged + painted */

    /* A full composite paints BOTH surfaces (the baseline). */
    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 2,
               "incremental: full composite paints both surfaces");

    /* Damaging only A and presenting recomposites ONLY A — B is not
       re-blitted, and its prior pixels are retained. */
    axl_surface_damage(a, (AxlGfxClip){0, 0, 5, 5});
    (void)axl_compositor_present(c);
    test_check(axl_compositor_composited_count(c) == 1,
               "incremental: present recomposites only the damaged surface");
    test_check(rgb_eq(scan_at(c, 2, 2), RED),
               "incremental: damaged surface repainted");
    test_check(rgb_eq(scan_at(c, 62, 62), BLU),
               "incremental: undamaged surface retained from prior frame");
    axl_compositor_free(c);
}

/* E3b — region occlusion culling: an opaque surface over a CORNER of a
   full-screen opaque surface occludes only that corner, so the lower one
   paints as an L-shape (2 rects), not its full rect. (composited counts
   blit ops, so this is 1 + 2 = 3; without partial occlusion it'd be
   1 + 1 = 2.) The on-screen result is unchanged — culling is invisible. */
static void
test_compositor_partial_occlusion(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel BLU = AXL_GFX_RGB(0x10, 0x10, 0xE0);
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* full, bottom */
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 20, 20);  /* corner, top */
    axl_surface_move(a, 0, 0);
    fill_surface(b, SW, SH, BLU);
    fill_surface(a, 20, 20, RED);
    axl_surface_set_opaque(a, true);
    axl_surface_set_opaque(b, true);

    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 3,
               "occlusion: corner occluder -> lower paints as L-shape (1 + 2 blits)");
    test_check(rgb_eq(scan_at(c, 5, 5), RED), "occlusion: corner shows the top surface");
    test_check(rgb_eq(scan_at(c, 50, 50), BLU), "occlusion: the rest shows the bottom surface");
    test_check(rgb_eq(scan_at(c, 5, 50), BLU), "occlusion: below the corner shows bottom");
    axl_compositor_free(c);
}

/* E3b OOM — occlusion is an optimization, never correctness: an allocation
   failure at ANY point in the occlusion walk must fall back to a full paint
   and still produce a correct frame, never an over-culled black hole. Two
   disjoint opaque occluders make a lossy (bbox-superset) `occluded` span the
   middle, so a missing is_lossy guard would over-cull the bottom surface
   there — this catches it. */
static void
test_compositor_occlusion_oom(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x10, 0xE0, 0x10);
    const AxlGfxPixel BLU = AXL_GFX_RGB(0x10, 0x10, 0xE0);
    bool ok = true;
    for (size_t n = 1; n <= 16 && ok; n++) {
        AxlCompositor *c = axl_compositor_new(SW, SH);
        AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* full */
        AxlSurface *a1 = axl_surface_create(axl_compositor_root(c), 20, 20); /* corner */
        AxlSurface *a2 = axl_surface_create(axl_compositor_root(c), 20, 20); /* far corner */
        axl_surface_move(a1, 0, 0);
        axl_surface_move(a2, 60, 40);
        fill_surface(b, SW, SH, BLU);
        fill_surface(a1, 20, 20, RED);
        fill_surface(a2, 20, 20, GRN);
        axl_surface_set_opaque(b, true);
        axl_surface_set_opaque(a1, true);
        axl_surface_set_opaque(a2, true);

        axl_mem_fail_next_alloc(n);
        axl_compositor_composite(c);   /* may OOM internally -> fallback */
        axl_mem_fail_next_alloc(0);

        /* The frame is correct regardless of which alloc failed: the middle
           (between the two occluders) shows the bottom surface, never an
           over-cull hole; both occluder corners show their colors. The
           occlusion build counts as exactly one attempted pass even when it
           degrades to the full-paint fallback (pins the accessor's contract). */
        ok = rgb_eq(scan_at(c, 40, 30), BLU)
             && rgb_eq(scan_at(c, 5, 5), RED)
             && rgb_eq(scan_at(c, 65, 45), GRN)
             && axl_compositor_occlusion_passes(c) == 1;
        if (!ok) {
            axl_printf("FAIL: occlusion OOM produced a wrong frame at n=%d\n", (int)n);
        }
        axl_compositor_free(c);
    }
    test_check(ok, "occlusion: OOM at any alloc -> correct frame (fallback, no over-cull holes)");
}

/* E6 — occlusion is hoisted to ONCE per present: the draw order + per-surface
   visible regions are a global property (independent of which sub-rect is
   repainted), so a present flushing N disjoint damage rects computes occlusion
   once and reuses it, not N times. The discriminating signal is
   occlusion_passes (1, not N). The painted pixels are the behavior-preserving
   invariant; the composited (blit) count is unchanged for THIS scene too, but
   in general region-banding can differ — pixels are what the hoist preserves. */
static void
test_compositor_occlusion_hoist(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel BLU = AXL_GFX_RGB(0x10, 0x10, 0xE0);
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* full, bottom */
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 20, 20);  /* corner, top */
    axl_surface_move(a, 0, 0);
    fill_surface(b, SW, SH, BLU);
    fill_surface(a, 20, 20, RED);
    axl_surface_set_opaque(b, true);
    axl_surface_set_opaque(a, true);

    /* A full composite is a single occlusion pass (the baseline). */
    axl_compositor_composite(c);
    test_check(axl_compositor_occlusion_passes(c) == 1,
               "hoist: a full composite is one occlusion pass");
    (void)axl_compositor_present(c);   /* consume all pending damage */

    /* Two disjoint damages, one under the opaque corner and one in the
       bottom-only area, flushed in ONE present. */
    axl_surface_damage(b, (AxlGfxClip){0, 0, 10, 10});     /* screen (0,0,10,10): under A */
    axl_surface_damage(b, (AxlGfxClip){50, 50, 10, 10});   /* screen (50,50,10,10): B only */
    const AxlGfxRegion *r = axl_compositor_get_damage_region(c);
    test_check(axl_gfx_region_num_rects(r) == 2,
               "hoist: the two disjoint damages stay 2 rects");

    (void)axl_compositor_present(c);
    /* THE discriminating assertion: occlusion computed once for both rects. */
    test_check(axl_compositor_occlusion_passes(c) == 1,
               "hoist: a 2-rect present computes occlusion ONCE, not per rect");
    /* Behavior-preserving: A still occludes B in the first rect (A painted,
       B culled there); B paints in the second rect. 1 + 1 = 2 blits. */
    test_check(axl_compositor_composited_count(c) == 2,
               "hoist: culling unchanged -> A in rect 1, B in rect 2 (2 blits)");
    test_check(rgb_eq(scan_at(c, 5, 5), RED),
               "hoist: opaque corner still occludes the surface beneath it");
    test_check(rgb_eq(scan_at(c, 55, 55), BLU),
               "hoist: the bottom-only damage shows the bottom surface");
    axl_compositor_free(c);
}

/* E10 — backdrop blur: a translucent surface frosts the composited backdrop
   beneath it (the dialog veil). Oracle: blur the same pattern independently
   and require an exact match, so the assertion pins the kernel, not a vibe. */
static void
test_compositor_backdrop_blur(void)
{
    const AxlGfxPixel BLK = AXL_GFX_RGB(0x00, 0x00, 0x00);
    const AxlGfxPixel WHT = AXL_GFX_RGB(0xFF, 0xFF, 0xFF);
    const uint32_t R = 6;

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* backdrop */
    AxlSurface *p = axl_surface_create(axl_compositor_root(c), SW, SH);  /* veil, top */

    /* Backdrop: a sharp vertical edge — left half black, right half white. */
    axl_gfx_target_buffer(axl_surface_buffer(b));
    axl_gfx_fill_rect(0, 0, SW, SH, BLK);
    axl_gfx_fill_rect(SW / 2, 0, SW / 2, SH, WHT);
    axl_gfx_target_buffer(NULL);

    /* Veil: fully transparent (per-pixel alpha, all alpha 0) so the output is
       EXACTLY the (blurred) backdrop — isolates the blur from any tint. */
    AxlGfxPixel *pp = axl_gfx_buffer_pixels(axl_surface_buffer(p));
    for (int i = 0; i < SW * SH; i++) { pp[i] = AXL_GFX_RGBA(0, 0, 0, 0); }
    axl_surface_set_per_pixel_alpha(p, true);

    /* Blur OFF (default): the backdrop edge stays sharp. */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, SW / 2 - 1, 40), BLK), "bdblur off: edge sharp (black side)");
    test_check(rgb_eq(scan_at(c, SW / 2,     40), WHT), "bdblur off: edge sharp (white side)");

    /* Oracle: the same pattern blurred independently with the same radius. */
    AxlGfxBuffer *ref = axl_gfx_buffer_new(SW, SH);
    axl_gfx_target_buffer(ref);
    axl_gfx_fill_rect(0, 0, SW, SH, BLK);
    axl_gfx_fill_rect(SW / 2, 0, SW / 2, SH, WHT);
    axl_gfx_target_buffer(NULL);
    axl_gfx_buffer_blur(ref, R);
    AxlGfxPixel *rp = axl_gfx_buffer_pixels(ref);

    /* Blur ON: the backdrop under the veil is frosted — exact-matches the oracle. */
    axl_surface_set_backdrop_blur(p, R);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, SW / 2, 40), rp[40 * SW + SW / 2]),
               "bdblur on: edge pixel matches the independently-blurred oracle");
    test_check(rgb_eq(scan_at(c, 5, 40), rp[40 * SW + 5]),
               "bdblur on: a uniform region matches the oracle");
    AxlGfxPixel edge = scan_at(c, SW / 2, 40);
    test_check(edge.red > 0 && edge.red < 255,
               "bdblur on: the sharp edge became a blurred midtone");

    /* Present path: a PARTIAL-damage repaint must still produce the full-rect
       blur (not a damage-sub-rect blur clamped mid-surface, which would seam).
       Consume pending damage, then damage a small sub-rect straddling the sharp
       edge and present: a point just inside it must match the full-blur oracle —
       which holds only if present recomposites the veil over its FULL rect. */
    (void)axl_compositor_present(c);                       /* consume create damage */
    axl_surface_damage(p, (AxlGfxClip){50, 30, 20, 20});   /* small rect at the edge */
    (void)axl_compositor_present(c);
    test_check(rgb_eq(scan_at(c, 52, 40), rp[40 * SW + 52]),
               "bdblur present: a partial-damage repaint matches the full-blur oracle (no seam)");

    /* A backdrop-blur surface never occludes — it's a translucent overlay. */
    axl_surface_set_opaque(p, true);
    axl_compositor_composite(c);
    test_check(axl_compositor_composited_count(c) == 2,
               "bdblur: a backdrop-blur surface never culls beneath it");

    axl_gfx_buffer_free(ref);
    axl_compositor_free(c);
}

/* E10 cache invalidation: the backdrop-blur cache is content-addressed, so it
   must NEVER serve a stale result. The existing test only ever blurs a STATIC
   backdrop (exercises the hit path); this pins the two invalidation paths the
   whole optimization rests on — a CHANGED backdrop re-blurs, and a RADIUS
   change drops the cache and recomputes. (Against a naive "always reuse"
   cache the "no stale" assertion fails; verified by sabotage during review.) */
static void
test_compositor_backdrop_blur_cache(void)
{
    const AxlGfxPixel BLK = AXL_GFX_RGB(0x00, 0x00, 0x00);
    const AxlGfxPixel WHT = AXL_GFX_RGB(0xFF, 0xFF, 0xFF);
    const uint32_t R = 6, R2 = 2;
    const int32_t  X = 46, Y = 40;   /* just left of the SW/2 edge: the blurred
                                        value differs sharply by pattern + radius */

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), SW, SH);  /* backdrop */
    AxlSurface *p = axl_surface_create(axl_compositor_root(c), SW, SH);  /* veil, top */

    /* Transparent veil so the output IS the blurred backdrop (isolate the blur). */
    AxlGfxPixel *pp = axl_gfx_buffer_pixels(axl_surface_buffer(p));
    for (int i = 0; i < SW * SH; i++) { pp[i] = AXL_GFX_RGBA(0, 0, 0, 0); }
    axl_surface_set_per_pixel_alpha(p, true);
    axl_surface_set_backdrop_blur(p, R);

    /* Helper: paint backdrop `b` as <left>|<right> halves at the SW/2 edge. */
    #define PAINT_HALVES(surf_or_buf, left, right)                       \
        do {                                                             \
            axl_gfx_target_buffer(surf_or_buf);                          \
            axl_gfx_fill_rect(0, 0, SW, SH, (left));                     \
            axl_gfx_fill_rect(SW / 2, 0, SW / 2, SH, (right));           \
            axl_gfx_target_buffer(NULL);                                 \
        } while (0)

    /* Pattern 1 (black|white): first present populates the cache. */
    PAINT_HALVES(axl_surface_buffer(b), BLK, WHT);
    axl_compositor_composite(c);

    AxlGfxBuffer *ora1 = axl_gfx_buffer_new(SW, SH);
    PAINT_HALVES(ora1, BLK, WHT);
    axl_gfx_buffer_blur(ora1, R);
    AxlGfxPixel *o1 = axl_gfx_buffer_pixels(ora1);
    test_check(rgb_eq(scan_at(c, X, Y), o1[Y * SW + X]),
               "bdblur cache: first present matches the pattern-1 oracle");

    /* Pattern 2 (white|black) — a DIFFERENT backdrop under the veil. */
    PAINT_HALVES(axl_surface_buffer(b), WHT, BLK);
    axl_surface_damage(b, (AxlGfxClip){0, 0, SW, SH});
    axl_compositor_composite(c);

    AxlGfxBuffer *ora2 = axl_gfx_buffer_new(SW, SH);
    PAINT_HALVES(ora2, WHT, BLK);
    axl_gfx_buffer_blur(ora2, R);
    AxlGfxPixel *o2 = axl_gfx_buffer_pixels(ora2);
    test_check(!rgb_eq(o1[Y * SW + X], o2[Y * SW + X]),
               "bdblur cache: pattern-1 vs pattern-2 oracles differ at the probe");
    test_check(rgb_eq(scan_at(c, X, Y), o2[Y * SW + X]),
               "bdblur cache: changed backdrop re-blurs (no stale cache served)");

    /* Radius change must drop the cache and recompute at the new radius. */
    axl_surface_set_backdrop_blur(p, R2);
    axl_compositor_composite(c);
    AxlGfxBuffer *ora3 = axl_gfx_buffer_new(SW, SH);
    PAINT_HALVES(ora3, WHT, BLK);
    axl_gfx_buffer_blur(ora3, R2);
    AxlGfxPixel *o3 = axl_gfx_buffer_pixels(ora3);
    test_check(!rgb_eq(o2[Y * SW + X], o3[Y * SW + X]),
               "bdblur cache: radius R vs R2 oracles differ at the probe");
    test_check(rgb_eq(scan_at(c, X, Y), o3[Y * SW + X]),
               "bdblur cache: radius change recomputes at the new radius");

    #undef PAINT_HALVES
    axl_gfx_buffer_free(ora1);
    axl_gfx_buffer_free(ora2);
    axl_gfx_buffer_free(ora3);
    axl_compositor_free(c);
}

/* E7 — frame callbacks (present throttling). The routing core
   (request_frame / dispatch_frame / has_pending_frames) is pure logic,
   testable with synthetic time and no loop. */
typedef struct {
    int               fires;
    uint64_t          last_time;
    AxlSurface       *resurf;       /* re-request on this surface in the cb */
    AxlFrameCallback  recb;
    int               re_remaining; /* re-request this many more times */
} FrameRec;

static void
frame_rec_cb(void *u, uint64_t t)
{
    FrameRec *r = u;
    r->fires++;
    r->last_time = t;
    if (r->re_remaining > 0) {
        r->re_remaining--;
        axl_surface_request_frame(r->resurf, r->recb, u);
    }
}

static void
test_compositor_frame_callbacks(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 10, 10);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 10, 10);

    test_check(!axl_compositor_has_pending_frames(c),
               "frame: none pending initially");
    test_check(!axl_compositor_dispatch_frame(c, 100),
               "frame: dispatch with none pending returns false");

    /* Request on two surfaces -> one dispatch fires both, with the dispatch
       time, and clears them (one-shot). */
    FrameRec ra = {0}, rb = {0};
    axl_surface_request_frame(a, frame_rec_cb, &ra);
    axl_surface_request_frame(b, frame_rec_cb, &rb);
    test_check(axl_compositor_has_pending_frames(c), "frame: pending after request");

    bool more = axl_compositor_dispatch_frame(c, 1000);
    test_check(ra.fires == 1 && rb.fires == 1, "frame: dispatch fires both once");
    test_check(ra.last_time == 1000 && rb.last_time == 1000,
               "frame: callback receives the dispatch time");
    test_check(!more && !axl_compositor_has_pending_frames(c),
               "frame: one-shot — cleared, nothing re-requested");

    axl_compositor_dispatch_frame(c, 2000);
    test_check(ra.fires == 1 && rb.fires == 1, "frame: one-shot not re-fired");

    /* Re-request inside the callback queues for the NEXT dispatch only. */
    FrameRec rc = {0};
    rc.resurf = a; rc.recb = frame_rec_cb; rc.re_remaining = 2;
    axl_surface_request_frame(a, frame_rec_cb, &rc);
    more = axl_compositor_dispatch_frame(c, 3000);   /* fire #1, re-requests */
    test_check(rc.fires == 1, "frame: re-request not fired in the same dispatch");
    test_check(more && axl_compositor_has_pending_frames(c),
               "frame: re-request keeps the clock pending");
    more = axl_compositor_dispatch_frame(c, 3016);   /* fire #2, re-requests */
    test_check(rc.fires == 2 && more, "frame: re-request fires next dispatch");
    more = axl_compositor_dispatch_frame(c, 3032);   /* fire #3, stops */
    test_check(rc.fires == 3 && !more,
               "frame: animation stops when re-requests stop");

    /* NULL cancels a pending request. */
    FrameRec rd = {0};
    axl_surface_request_frame(b, frame_rec_cb, &rd);
    axl_surface_request_frame(b, NULL, NULL);
    test_check(!axl_compositor_has_pending_frames(c), "frame: NULL cancels pending");
    axl_compositor_dispatch_frame(c, 4000);
    test_check(rd.fires == 0, "frame: cancelled callback never fires");

    /* Destroying a surface cancels its pending callback (no stale fire). */
    FrameRec re = {0};
    axl_surface_request_frame(a, frame_rec_cb, &re);
    axl_surface_destroy(a);
    test_check(!axl_compositor_has_pending_frames(c),
               "frame: destroy cancels the pending callback");
    axl_compositor_dispatch_frame(c, 5000);
    test_check(re.fires == 0, "frame: destroyed surface's callback never fires");

    /* NULL-safety. */
    test_check(!axl_compositor_dispatch_frame(NULL, 0), "frame: dispatch(NULL)");
    test_check(!axl_compositor_has_pending_frames(NULL), "frame: pending(NULL)");
    axl_surface_request_frame(NULL, frame_rec_cb, &ra);   /* no crash */

    axl_compositor_free(c);
}

/* E7 — the frame clock: a real loop timer drives dispatch and self-cancels
   when no callback re-requests (idle = no busy timer), then RE-ARMS on a later
   request. The test drives all three: animate, let it idle on its own (the
   AXL_SOURCE_REMOVE self-cancel path), then re-arm from a separate timeout. */
typedef struct {
    int             fires;      /* phase-1 frame count */
    int             target;     /* re-request until this many fires, then stop */
    AxlSurface     *surf;
    AxlLoop        *loop;
    AxlCompositor  *comp;
    bool            idle_seen;  /* phase 2 observed the clock had gone idle */
    bool            rearmed;    /* the re-armed frame actually fired */
} FrameClockRec;

/* Phase 1: animate `target` frames by re-requesting, then STOP (no quit, no
   re-request) so the clock self-cancels through frame_tick's REMOVE branch. */
static void
frame_clock_cb(void *u, uint64_t t)
{
    (void)t;
    FrameClockRec *r = u;
    r->fires++;
    if (r->fires < r->target) {
        axl_surface_request_frame(r->surf, frame_clock_cb, u);
    }
}

/* Phase 3: the frame delivered by the RE-ARMED clock. */
static void
frame_rearm_cb(void *u, uint64_t t)
{
    (void)t;
    FrameClockRec *r = u;
    r->rearmed = true;
    axl_loop_quit(r->loop);
}

/* Phase 2: fires well after phase 1 has finished and the clock self-cancelled.
   It confirms the clock idled, then issues a new request — which must re-arm a
   fresh timer that delivers frame_rearm_cb (proving re-arm via a real timer). */
static bool
frame_phase2(void *data)
{
    FrameClockRec *r = data;
    r->idle_seen = !axl_compositor_has_pending_frames(r->comp);
    axl_surface_request_frame(r->surf, frame_rearm_cb, r);
    return AXL_SOURCE_REMOVE;
}

static bool
frame_safety_quit(void *data)
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

static void
test_compositor_frame_clock(void)
{
    AxlLoop *loop = axl_loop_new();
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 10, 10);

    uint32_t id = axl_compositor_attach_frame_clock(c, loop, 10);
    test_check(id != 0, "frame-clock: attached, non-zero source id");

    FrameClockRec r = {0};
    r.target = 3; r.surf = a; r.loop = loop; r.comp = c;
    axl_surface_request_frame(a, frame_clock_cb, &r);

    /* Phase 2 runs long after phase 1's 3 frames (~30ms) have idled the clock.
       Safety net: a broken clock must not hang the suite. */
    axl_loop_add_timeout(loop, 500, frame_phase2, &r);
    axl_loop_add_timeout(loop, 4000, frame_safety_quit, loop);
    axl_loop_run(loop);

    test_check(r.fires == 3, "frame-clock: timer drove exactly 3 frames then idled");
    test_check(r.idle_seen,
               "frame-clock: clock self-cancelled when re-requests stopped");
    test_check(r.rearmed,
               "frame-clock: a later request re-armed the self-cancelled clock");
    test_check(!axl_compositor_has_pending_frames(c),
               "frame-clock: idle again after the re-armed frame");

    axl_compositor_detach_frame_clock(c, loop);
    axl_compositor_free(c);
    axl_loop_free(loop);
}

/* ======================================================================
   C4 — the seat: pointer routing, the AxlSurfaceListener, hit-test,
   enter/leave, re-hit-test on surface changes, input regions.

   All pure-RAM: axl_compositor_pointer_event takes output-coordinate
   synthetic events, so the routing math is fully testable without a GOP
   or a real pointer device (the device path is axl_compositor_attach_*).
   ====================================================================== */

/* Records listener callbacks for one surface. */
typedef struct {
    int      enters, leaves, motions, buttons, axes;
    int32_t  last_x, last_y;        /* last enter/motion/button local coords */
    uint32_t last_button;
    bool     last_pressed;
    int32_t  last_axis_dx, last_axis_dy;
    uint32_t last_modifiers;        /* P0: modifiers on motion/button/axis */
    uint32_t last_click_count;      /* P0: click_count on button */
    bool     last_dragging;         /* P0: gesture drag latch on button */
    int      keys, focus_ins, focus_outs;   /* C5 */
    uint32_t last_unicode;
} Recorder;

static void rec_enter(void *u, int32_t x, int32_t y)
{
    Recorder *r = u; r->enters++; r->last_x = x; r->last_y = y;
}
static void rec_leave(void *u) { Recorder *r = u; r->leaves++; }
static void rec_motion(void *u, int32_t x, int32_t y, uint32_t modifiers)
{
    Recorder *r = u; r->motions++; r->last_x = x; r->last_y = y;
    r->last_modifiers = modifiers;
}
static void rec_button(void *u, uint32_t b, bool pressed, int32_t x, int32_t y,
                       uint32_t modifiers, uint32_t click_count, bool dragging)
{
    Recorder *r = u;
    r->buttons++; r->last_button = b; r->last_pressed = pressed;
    r->last_x = x; r->last_y = y;
    r->last_modifiers = modifiers; r->last_click_count = click_count;
    r->last_dragging = dragging;
}
static void rec_axis(void *u, int32_t dx, int32_t dy, uint32_t modifiers)
{
    Recorder *r = u; r->axes++; r->last_axis_dx = dx; r->last_axis_dy = dy;
    r->last_modifiers = modifiers;
}
static void rec_key(void *u, const AxlInputEvent *ev)
{
    Recorder *r = u; r->keys++; r->last_unicode = ev->unicode;
}
static void rec_focus_in(void *u)  { Recorder *r = u; r->focus_ins++; }
static void rec_focus_out(void *u) { Recorder *r = u; r->focus_outs++; }

static const AxlSurfaceListener REC_LISTENER = {
    .enter = rec_enter, .leave = rec_leave, .motion = rec_motion,
    .button = rec_button, .axis = rec_axis,
    .key = rec_key, .focus_in = rec_focus_in, .focus_out = rec_focus_out,
};

static AxlInputEvent
move_ev(int32_t x, int32_t y)
{
    return (AxlInputEvent){ .type = AXL_INPUT_MOUSE_MOVE, .x = x, .y = y };
}

static void
test_surface_resize(void)
{
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x10, 0xE0, 0x10);
    const AxlGfxPixel BG  = AXL_GFX_RGB(0x00, 0x00, 0x00);

    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *s = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(s, 10, 10);          /* abs 10..30 */
    fill_surface(s, 20, 20, RED);
    (void)axl_compositor_present(c);       /* consume the create/move damage */

    /* Grow it: geometry updates; damage = union(old 10..30, new 10..50). */
    axl_surface_resize(s, 40, 40);
    uint32_t w = 0, h = 0;
    axl_surface_get_geometry(s, NULL, NULL, &w, &h);
    test_check(w == 40 && h == 40, "resize: geometry reflects the new size");
    AxlGfxClip d;
    test_check(axl_compositor_get_damage(c, &d) == AXL_OK
               && d.x == 10 && d.y == 10 && d.w == 40 && d.h == 40,
               "resize: damages union(old, new) bounds");

    /* Overlap pixels preserved; the newly exposed area is cleared. */
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 15, 15), RED), "resize: overlap pixels preserved");
    test_check(rgb_eq(scan_at(c, 45, 45), BG),  "resize: newly exposed area starts cleared");

    /* The grown buffer is drawable across its full new extent. */
    axl_gfx_target_buffer(axl_surface_buffer(s));
    axl_gfx_fill_rect(0, 0, 40, 40, GRN);
    axl_gfx_target_buffer(NULL);
    axl_compositor_composite(c);
    test_check(rgb_eq(scan_at(c, 45, 45), GRN), "resize: the grown buffer is fully drawable");

    /* No-ops: root / zero dims / unchanged size / NULL — size stays 40x40. */
    axl_surface_resize(axl_compositor_root(c), 10, 10);
    axl_surface_resize(s, 0, 10);
    axl_surface_resize(s, 40, 40);
    axl_surface_resize(NULL, 5, 5);
    axl_surface_get_geometry(s, NULL, NULL, &w, &h);
    test_check(w == 40 && h == 40, "resize: root/zero/unchanged/NULL are no-ops");

    axl_compositor_free(c);
}

static void
test_surface_absolute(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *p = axl_surface_create(axl_compositor_root(c), 50, 50);
    AxlSurface *ch = axl_surface_create(p, 20, 20);
    axl_surface_move(p, 40, 30);    /* parent abs (40,30) */
    axl_surface_move(ch, 10, 10);   /* relative → child abs (50,40) */

    int32_t x = -1, y = -1;
    axl_surface_get_absolute(ch, &x, &y);
    test_check(x == 50 && y == 40, "absolute: nested child origin in output coords");

    axl_surface_get_absolute(axl_compositor_root(c), &x, &y);
    test_check(x == 0 && y == 0, "absolute: root origin is (0,0)");

    int32_t ox = -1, oy = -1;
    axl_surface_to_output(ch, 5, 5, &ox, &oy);
    test_check(ox == 55 && oy == 45, "absolute: to_output maps local → output");

    int32_t lx = -1, ly = -1;
    axl_surface_from_output(ch, 55, 45, &lx, &ly);
    test_check(lx == 5 && ly == 5, "absolute: from_output maps output → local");

    axl_surface_move(p, 60, 60);    /* child abs → (70,70) */
    axl_surface_get_absolute(ch, &x, &y);
    test_check(x == 70 && y == 70, "absolute: a parent move shifts the child absolute origin");

    /* NULL surface / NULL out-params are safe no-ops. */
    axl_surface_get_absolute(NULL, &x, &y);
    axl_surface_get_absolute(ch, NULL, NULL);
    axl_surface_to_output(NULL, 0, 0, &ox, &oy);
    axl_surface_from_output(NULL, 0, 0, &lx, &ly);
    test_check(x == 70 && y == 70, "absolute: NULL surface / NULL out-params are no-ops");

    axl_compositor_free(c);
}

static void
test_seat_hittest(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(a, 10, 10);   /* abs 10..40 */
    axl_surface_move(b, 25, 25);   /* abs 25..55, overlaps a on 25..40 */
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    /* Into the overlap: the topmost surface (b, created later) wins. */
    AxlInputEvent e = move_ev(30, 30);
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == 1 && rb.motions == 1,
               "seat: topmost surface in the overlap gets enter+motion");
    test_check(rb.last_x == 5 && rb.last_y == 5,
               "seat: delivered coords are surface-local (30-25, 30-25)");
    test_check(ra.enters == 0, "seat: the surface beneath is not entered");

    /* Into a's exclusive area: b leaves, a enters. */
    e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(rb.leaves == 1, "seat: leaving the overlap leaves b");
    test_check(ra.enters == 1 && ra.last_x == 5 && ra.last_y == 5,
               "seat: a entered surface-local (15-10, 15-10)");

    /* Into empty space: a leaves, nobody enters. */
    e = move_ev(80, 70);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.leaves == 1, "seat: moving onto empty output leaves a");
    test_check(ra.enters == 1 && rb.enters == 1, "seat: empty output enters nothing");

    axl_compositor_free(c);
}

static void
test_seat_buttons_axis(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 40, 40);
    axl_surface_move(a, 10, 10);
    Recorder ra = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);

    AxlInputEvent e = move_ev(20, 20);          /* enter a (local 10,10) */
    axl_compositor_pointer_event(c, &e);

    /* Press LEFT. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN, .x = 20, .y = 20,
                         .buttons = AXL_INPUT_BUTTON_LEFT };
    axl_compositor_pointer_event(c, &e);
    test_check(ra.buttons == 1 && ra.last_button == AXL_INPUT_BUTTON_LEFT
               && ra.last_pressed, "seat: button down delivers LEFT pressed");
    test_check(ra.last_x == 10 && ra.last_y == 10, "seat: button coords surface-local");

    /* Press RIGHT while LEFT held: only the changed bit (RIGHT) reported. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN, .x = 20, .y = 20,
                         .buttons = AXL_INPUT_BUTTON_LEFT | AXL_INPUT_BUTTON_RIGHT };
    axl_compositor_pointer_event(c, &e);
    test_check(ra.buttons == 2 && ra.last_button == AXL_INPUT_BUTTON_RIGHT
               && ra.last_pressed, "seat: second press reports only the changed bit (RIGHT)");

    /* Release LEFT (RIGHT still held): reports LEFT up. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_UP, .x = 20, .y = 20,
                         .buttons = AXL_INPUT_BUTTON_RIGHT };
    axl_compositor_pointer_event(c, &e);
    test_check(ra.buttons == 3 && ra.last_button == AXL_INPUT_BUTTON_LEFT
               && !ra.last_pressed, "seat: release reports LEFT up (changed bit)");

    /* Wheel → axis. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_WHEEL, .x = 20, .y = 20,
                         .wheel_dx = 0, .wheel_dy = 3 };
    axl_compositor_pointer_event(c, &e);
    test_check(ra.axes == 1 && ra.last_axis_dy == 3 && ra.last_axis_dx == 0,
               "seat: wheel delivers an axis event");

    axl_compositor_free(c);
}

static void
test_seat_modifiers_clicks(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 40, 40);
    axl_surface_move(a, 10, 10);
    Recorder ra = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);

    /* Motion carries the live modifier state. */
    AxlInputEvent e = move_ev(20, 20);
    e.modifiers = AXL_INPUT_MOD_LSHIFT;
    axl_compositor_pointer_event(c, &e);
    test_check(ra.last_modifiers == AXL_INPUT_MOD_LSHIFT, "p0: motion carries modifiers");

    /* Button carries modifiers + the gesture's click_count and drag latch —
       the fields AGT's double/triple-click + shift-click selection read. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN, .x = 20, .y = 20,
                         .buttons = AXL_INPUT_BUTTON_LEFT,
                         .modifiers = AXL_INPUT_MOD_LCTRL,
                         .click_count = 2, .dragging = true };
    axl_compositor_pointer_event(c, &e);
    test_check(ra.last_modifiers == AXL_INPUT_MOD_LCTRL, "p0: button carries modifiers");
    test_check(ra.last_click_count == 2, "p0: button carries click_count (double-click)");
    test_check(ra.last_dragging, "p0: button carries the gesture drag latch");

    /* Axis carries modifiers — Shift+wheel = horizontal scroll. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_WHEEL, .x = 20, .y = 20,
                         .wheel_dx = 0, .wheel_dy = 1,
                         .modifiers = AXL_INPUT_MOD_RSHIFT };
    axl_compositor_pointer_event(c, &e);
    test_check(ra.last_modifiers == AXL_INPUT_MOD_RSHIFT, "p0: axis carries modifiers (Shift+wheel)");

    axl_compositor_free(c);
}

static void
test_seat_refocus(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    /* a and b fully overlap at abs 10..40; b on top initially. */
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(a, 10, 10);
    axl_surface_move(b, 10, 10);
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    AxlInputEvent e = move_ev(15, 15);          /* b (top) enters */
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == 1, "refocus: b focused initially");
    int b_motions = rb.motions;

    /* Raise a under a stationary pointer: synthetic b-leave + a-enter, no motion. */
    axl_surface_raise(a);
    test_check(rb.leaves == 1 && ra.enters == 1, "refocus: raise re-hit-tests (b leave, a enter)");
    test_check(rb.motions == b_motions && ra.motions == 0, "refocus: re-hit-test emits no motion");

    /* Hide a (focused): a leaves, b re-enters. */
    axl_surface_set_visible(a, false);
    test_check(ra.leaves == 1 && rb.enters == 2, "refocus: hiding focused a reveals b");

    /* Move b off the pointer (a still hidden): b leaves, nothing enters. */
    axl_surface_move(b, 60, 60);
    test_check(rb.leaves == 2, "refocus: moving focused b off the pointer leaves it");
    test_check(ra.enters == 1, "refocus: nothing under the pointer after the move");

    axl_compositor_free(c);
}

static void
test_seat_refocus_lower_reparent(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);
    AxlSurface *p = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(a, 10, 10);   /* abs 10..40 */
    axl_surface_move(b, 10, 10);   /* abs 10..40, over a */
    axl_surface_move(p, 60, 60);   /* abs 60..80, clear of the pointer */
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    AxlInputEvent e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == 1, "refocus2: b focused (created after a)");
    int rb_motions = rb.motions, ra_motions = ra.motions;

    /* lower(b) under a stationary pointer → b leaves, a enters, no motion. */
    axl_surface_lower(b);
    test_check(rb.leaves == 1 && ra.enters == 1, "refocus2: lower re-hit-tests (b leave, a enter)");
    test_check(rb.motions == rb_motions && ra.motions == ra_motions,
               "refocus2: lower-driven refocus emits no motion");

    /* Reparent a under p (far away): a's (10,10) becomes relative to p → it
       leaves the pointer, and b underneath re-enters. */
    axl_surface_set_parent(a, p);
    test_check(ra.leaves == 1, "refocus2: reparent moves a off the pointer (a leaves)");
    test_check(rb.enters == 2, "refocus2: reparent re-exposes b under the pointer (b enters)");

    axl_compositor_free(c);
}

static void
test_seat_destroy_refocus(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(a, 10, 10);
    axl_surface_move(b, 10, 10);   /* fully over a, on top */
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    AxlInputEvent e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == 1, "destroy: b focused before destroy");

    /* Destroy the focused surface: the one beneath re-enters (no leave on the
       dead surface). */
    axl_surface_destroy(b);
    test_check(ra.enters == 1, "destroy: destroying focused b re-enters a beneath");
    test_check(ra.leaves == 0, "destroy: a only enters (was never focused before)");

    axl_compositor_free(c);
}

static void
test_seat_input_region(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 40, 40);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 40, 40);
    axl_surface_move(a, 10, 10);   /* abs 10..50 */
    axl_surface_move(b, 10, 10);   /* exactly over a, on top */
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    /* b catches input only in its top-left 20x20 quarter. */
    AxlGfxClip region = {0, 0, 20, 20};
    axl_surface_set_input_region(b, &region, 1);

    /* Inside the region: hits b. */
    AxlInputEvent e = move_ev(15, 15);          /* b-local 5,5 — in region */
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == 1, "region: a point in b's input region hits b");

    /* Inside b's rect but outside the region: passes through to a. */
    e = move_ev(45, 45);                          /* b-local 35,35 outside region; a-local 35,35 */
    axl_compositor_pointer_event(c, &e);
    test_check(rb.leaves == 1 && ra.enters == 1,
               "region: outside b's region the event passes through to a");

    /* Empty region (non-NULL, n == 0): b becomes fully input-transparent. */
    int rb_enters = rb.enters;                    /* == 1 */
    int ra_motions = ra.motions;
    AxlGfxClip dummy = {0, 0, 1, 1};
    axl_surface_set_input_region(b, &dummy, 0);
    e = move_ev(15, 15);                           /* would hit b if region were full */
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == rb_enters, "region: an empty region never catches input");
    test_check(ra.motions == ra_motions + 1, "region: the transparent surface passes the event to a");

    /* NULL resets to the full rect: b catches again. */
    axl_surface_set_input_region(b, NULL, 0);
    e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == rb_enters + 1, "region: NULL restores the full input rect");

    axl_compositor_free(c);
}

static void
test_seat_nulls(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(a, 10, 10);

    /* A listener with NULL callback fields: the seat skips them (no crash),
       and the non-NULL ones still fire. */
    AxlSurfaceListener partial = { .motion = rec_motion };   /* enter/leave NULL */
    Recorder ra = {0};
    axl_surface_set_listener(a, &partial, &ra);
    AxlInputEvent e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.motions == 1, "seat: NULL listener callbacks are skipped; motion still delivered");

    /* NULL-arg seat calls are no-ops, and delivery still works afterwards. */
    axl_compositor_pointer_event(NULL, &e);
    axl_compositor_pointer_event(c, NULL);
    axl_surface_set_listener(NULL, &partial, &ra);
    axl_surface_set_input_region(NULL, NULL, 0);
    e = move_ev(16, 16);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.motions == 2, "seat: NULL-arg calls are no-ops; routing still works");

    axl_compositor_free(c);
}

static void
test_seat_pointer_gating(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(a, 10, 10);
    Recorder ra = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);

    /* No pointer event has ever arrived: surface changes synthesize nothing
       (a stationary pointer needs a known position first — headless arches
       and pre-input startup must stay silent). */
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(b, 60, 50);
    test_check(ra.enters == 0, "gating: no enter before the first pointer event");

    /* The first pointer event activates routing. */
    AxlInputEvent e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 1, "gating: the first pointer event activates the seat");

    axl_compositor_free(c);
}

static void
test_seat_hidden_parent(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *parent = axl_surface_create(axl_compositor_root(c), 40, 40);
    axl_surface_move(parent, 10, 10);            /* abs 10..50 */
    AxlSurface *child = axl_surface_create(parent, 20, 20);
    axl_surface_move(child, 5, 5);               /* abs 15..35 (parent-relative) */
    Recorder rc = {0};
    axl_surface_set_listener(child, &REC_LISTENER, &rc);

    /* Child visible (parent visible) under the pointer → it gets enter. */
    AxlInputEvent e = move_ev(20, 20);           /* inside child abs 15..35 */
    axl_compositor_pointer_event(c, &e);
    test_check(rc.enters == 1, "hidden-parent: visible child is hit while the parent is visible");

    /* Hide the PARENT (child stays visible): the child must not be hittable.
       The refocus on hide leaves the child; a fresh event over it enters nobody. */
    axl_surface_set_visible(parent, false);
    test_check(rc.leaves == 1, "hidden-parent: hiding the parent leaves the still-visible child");
    e = move_ev(20, 20);
    axl_compositor_pointer_event(c, &e);
    test_check(rc.enters == 1, "hidden-parent: a visible child of a hidden parent is not hit");

    axl_compositor_free(c);
}

/* ======================================================================
   C5 — the seat: pointer grabs (LIFO + dismiss-on-outside-press) and
   per-surface keyboard focus + key routing.
   ====================================================================== */

static void rec_dismiss(void *u) { (*(int *)u)++; }

static void
test_seat_grab(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a  = axl_surface_create(axl_compositor_root(c), 20, 20);
    AxlSurface *m  = axl_surface_create(axl_compositor_root(c), 20, 20);  /* "menu" */
    AxlSurface *mc = axl_surface_create(m, 20, 20);                        /* submenu child of m */
    axl_surface_move(a, 5, 5);     /* abs 5..25 */
    axl_surface_move(m, 40, 40);   /* abs 40..60 */
    axl_surface_move(mc, 0, 20);   /* rel → abs x 40..60, y 60..80 */
    Recorder ra = {0}, rm = {0}, rmc = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(m, &REC_LISTENER, &rm);
    axl_surface_set_listener(mc, &REC_LISTENER, &rmc);

    int dismissed = 0;
    axl_compositor_pointer_grab(c, m, rec_dismiss, &dismissed);

    /* Inside the grab subtree (m itself): routes normally. */
    AxlInputEvent e = move_ev(50, 50);
    axl_compositor_pointer_event(c, &e);
    test_check(rm.enters == 1, "grab: pointer over the grab surface enters it");

    /* A subtree child (mc, child of m): routes normally. */
    e = move_ev(50, 70);
    axl_compositor_pointer_event(c, &e);
    test_check(rmc.enters == 1 && rm.leaves == 1, "grab: pointer over a subtree child routes to it");

    /* Outside the subtree (over a): focus is confined — a is NOT entered and
       the subtree surface leaves. */
    e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 0, "grab: a surface outside the grab subtree is not entered");
    test_check(rmc.leaves == 1, "grab: leaving the subtree un-focuses the subtree surface");

    /* A press outside the subtree dismisses the grab (pop + callback) and is
       not delivered. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN, .x = 15, .y = 15,
                         .buttons = AXL_INPUT_BUTTON_LEFT };
    axl_compositor_pointer_event(c, &e);
    test_check(dismissed == 1, "grab: a press outside the subtree dismisses the grab");
    test_check(ra.buttons == 0, "grab: the dismissing press is consumed, not delivered");

    /* Grab gone: normal routing resumes. */
    e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 1, "grab: after dismiss, routing returns to normal");

    axl_compositor_free(c);
}

static void
test_seat_grab_inside_click(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *m = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(m, 20, 20);
    Recorder rm = {0};
    axl_surface_set_listener(m, &REC_LISTENER, &rm);
    int dismissed = 0;
    axl_compositor_pointer_grab(c, m, rec_dismiss, &dismissed);

    AxlInputEvent e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN,
                         .x = 30, .y = 30, .buttons = AXL_INPUT_BUTTON_LEFT };
    axl_compositor_pointer_event(c, &e);
    test_check(rm.buttons == 1 && rm.last_pressed, "grab: a press inside the subtree is delivered");
    test_check(dismissed == 0, "grab: an inside press does not dismiss");

    axl_compositor_free(c);
}

static void
test_seat_grab_dismiss_empty(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *m = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(m, 40, 40);   /* abs 40..60 */
    Recorder rm = {0};
    axl_surface_set_listener(m, &REC_LISTENER, &rm);
    int dismissed = 0;
    axl_compositor_pointer_grab(c, m, rec_dismiss, &dismissed);

    /* A press over empty output (no surface there) is "outside the subtree"
       and dismisses the grab. */
    AxlInputEvent e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN,
                         .x = 5, .y = 5, .buttons = AXL_INPUT_BUTTON_LEFT };
    axl_compositor_pointer_event(c, &e);
    test_check(dismissed == 1, "grab: a press over empty space dismisses the grab");

    axl_compositor_free(c);
}

static void
test_seat_grab_nested(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    /* Outer grab a (big); inner grab b is a CHILD of a (so a's body is inside
       a's subtree but outside b's subtree). */
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 60, 60);
    axl_surface_move(a, 5, 5);          /* abs 5..65 */
    AxlSurface *b = axl_surface_create(a, 20, 20);
    axl_surface_move(b, 40, 40);        /* rel → abs 45..65 */
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    axl_compositor_pointer_grab(c, a, NULL, NULL);              /* outer */
    int dismissed = 0;
    axl_compositor_pointer_grab(c, b, rec_dismiss, &dismissed); /* inner, top */

    /* Pointer inside a's subtree but outside b's: the inner grab confines, so
       a's body is not entered. */
    AxlInputEvent e = move_ev(10, 10);   /* in a (5..65), not in b (45..65) */
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 0, "nested: the inner grab confines focus (a's body not entered)");

    /* A press there dismisses the inner grab; the OUTER grab is now active and
       the pointer is inside its subtree, so a must be re-entered (regression:
       a stale NULL focus after dismiss would leave a un-entered). */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN, .x = 10, .y = 10,
                         .buttons = AXL_INPUT_BUTTON_LEFT };
    axl_compositor_pointer_event(c, &e);
    test_check(dismissed == 1, "nested: a press outside the inner grab dismisses it");
    test_check(ra.enters == 1,
               "nested: after the inner dismiss, the outer-grab surface re-enters under the pointer");

    axl_compositor_free(c);
}

/* Chain grabs: a popup chain (menu + submenu as SIBLING surfaces, both
   chain-grabbed) keeps the whole chain interactive — hover/click route across
   every member, not just the top grab. The discriminator vs an exclusive grab
   (test_seat_grab_nested/lifo) is that the LOWER grab still routes. */
static void
test_seat_grab_chain(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 20, 20);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(a, 5, 5);     /* abs 5..25  (the menu)    */
    axl_surface_move(b, 50, 50);   /* abs 50..70 (its submenu) */
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    axl_compositor_pointer_grab_chain(c, a, NULL, NULL);                /* menu    */
    int dismissed = 0;
    axl_compositor_pointer_grab_chain(c, b, rec_dismiss, &dismissed);   /* submenu, top */

    /* Over the top grab b: routes to b. */
    AxlInputEvent e = move_ev(60, 60);
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == 1, "chain: pointer over the top grab routes to it");

    /* Over the LOWER chain grab a: routes to a — THE discriminating case (an
       exclusive top grab would block a, leaving it un-entered). */
    e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 1, "chain: a LOWER chain grab still routes (union, not top-only)");
    test_check(rb.leaves == 1, "chain: crossing from the top to a lower member emits leave");

    /* Over empty space outside the WHOLE chain: focus leaves, no surface. */
    e = move_ev(90, 10);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.leaves == 1, "chain: moving outside the whole chain leaves the member");

    /* A press outside the whole chain dismisses the top grab. */
    e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN, .x = 90, .y = 10,
                         .buttons = AXL_INPUT_BUTTON_LEFT };
    axl_compositor_pointer_event(c, &e);
    test_check(dismissed == 1, "chain: a press outside the whole chain dismisses the top grab");

    axl_compositor_free(c);
}

static void
test_seat_grab_lifo(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 20, 20);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(a, 5, 5);     /* abs 5..25 */
    axl_surface_move(b, 50, 50);   /* abs 50..70 */
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    axl_compositor_pointer_grab(c, a, NULL, NULL);
    axl_compositor_pointer_grab(c, b, NULL, NULL);   /* top = b */

    AxlInputEvent e = move_ev(15, 15);               /* over a, outside b */
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 0, "grab-lifo: the top grab (b) confines focus, a not entered");
    e = move_ev(60, 60);                              /* over b */
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == 1, "grab-lifo: the top grab (b) routes to b");

    axl_compositor_pointer_ungrab(c);                /* top = a */
    e = move_ev(15, 15);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 1, "grab-lifo: after one ungrab, grab a routes to a");
    int rb_enters = rb.enters;
    e = move_ev(60, 60);                              /* over b, outside a */
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == rb_enters, "grab-lifo: grab a confines focus, b not entered");

    axl_compositor_pointer_ungrab(c);                /* no grab */
    e = move_ev(60, 60);
    axl_compositor_pointer_event(c, &e);
    test_check(rb.enters == rb_enters + 1, "grab-lifo: after full ungrab, normal routing");

    axl_compositor_pointer_ungrab(c);                /* empty-stack ungrab is a no-op */

    axl_compositor_free(c);
}

static void
test_seat_keyboard(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 20, 20);
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 20, 20);
    Recorder ra = {0}, rb = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);
    axl_surface_set_listener(b, &REC_LISTENER, &rb);

    test_check(axl_compositor_keyboard_focus(c) == NULL, "kbd: no focus initially");
    axl_compositor_set_keyboard_focus(c, a);
    test_check(ra.focus_ins == 1 && axl_compositor_keyboard_focus(c) == a,
               "kbd: focusing a emits a focus_in");
    axl_compositor_set_keyboard_focus(c, b);
    test_check(ra.focus_outs == 1 && rb.focus_ins == 1, "kbd: switching focus → a out, b in");

    AxlInputEvent e = (AxlInputEvent){ .type = AXL_INPUT_KEY_DOWN, .unicode = 'q' };
    axl_compositor_key_event(c, &e);
    test_check(rb.keys == 1 && rb.last_unicode == 'q' && ra.keys == 0,
               "kbd: a key routes to the focused surface (b)");

    axl_compositor_set_keyboard_focus(c, NULL);
    test_check(rb.focus_outs == 1, "kbd: clearing focus emits b focus_out");
    e.unicode = 'z';
    axl_compositor_key_event(c, &e);
    test_check(rb.keys == 1, "kbd: with no focus, keys are dropped");

    /* Non-key event and NULLs are no-ops; routing still works afterward. */
    AxlInputEvent mv = move_ev(1, 1);
    axl_compositor_key_event(c, &mv);
    axl_compositor_key_event(NULL, &e);
    axl_compositor_key_event(c, NULL);
    axl_compositor_set_keyboard_focus(c, a);
    e.unicode = 'k';
    axl_compositor_key_event(c, &e);
    test_check(ra.keys == 1 && ra.last_unicode == 'k', "kbd: routing still works after no-op calls");

    axl_compositor_free(c);
}

static void
test_seat_destroy_purges(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(a, 10, 10);
    Recorder ra = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);

    int dismissed = 0;
    axl_compositor_pointer_grab(c, a, rec_dismiss, &dismissed);
    axl_compositor_set_keyboard_focus(c, a);

    /* Destroy the grabbing + keyboard-focused surface: seat state is purged,
       no callbacks on the dead surface, no crash. */
    axl_surface_destroy(a);
    test_check(axl_compositor_keyboard_focus(c) == NULL, "purge: destroy clears keyboard focus");

    /* The grab is gone: a fresh surface routes normally, with no stale-grab
       confinement and no dismiss callback firing. */
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 30, 30);
    axl_surface_move(b, 10, 10);
    Recorder rb = {0};
    axl_surface_set_listener(b, &REC_LISTENER, &rb);
    AxlInputEvent e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_BUTTON_DOWN,
                         .x = 20, .y = 20, .buttons = AXL_INPUT_BUTTON_LEFT };
    axl_compositor_pointer_event(c, &e);
    test_check(rb.buttons == 1 && dismissed == 0,
               "purge: destroy removed the grab (normal routing, no dismiss)");

    /* A key after the focused surface died is dropped (no crash). */
    AxlInputEvent k = (AxlInputEvent){ .type = AXL_INPUT_KEY_DOWN, .unicode = 'x' };
    axl_compositor_key_event(c, &k);
    test_check(rb.keys == 0, "purge: key dropped after the focused surface was destroyed");

    axl_compositor_free(c);
}

static void
test_seat_grab_nulls(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 20, 20);
    axl_surface_move(a, 5, 5);
    Recorder ra = {0};
    axl_surface_set_listener(a, &REC_LISTENER, &ra);

    axl_compositor_pointer_grab(NULL, a, NULL, NULL);
    axl_compositor_pointer_grab(c, NULL, NULL, NULL);
    axl_compositor_pointer_grab(c, axl_compositor_root(c), NULL, NULL);  /* root not grabbable */
    axl_compositor_pointer_ungrab(NULL);
    axl_compositor_pointer_ungrab(c);   /* empty stack */

    AxlInputEvent e = move_ev(10, 10);
    axl_compositor_pointer_event(c, &e);
    test_check(ra.enters == 1, "grab-nulls: bad/NULL grab calls are no-ops; normal routing intact");

    axl_compositor_free(c);
}

/* ======================================================================
   C6 — the cursor: the compositor's top overlay, driven by the seat.

   The "no trail" / "shape changes on enter" visual checks need a GOP, so
   they live in compositor-selftest.c. Here we pin the arch-independent
   invariants: the compositor owns a cursor, the seat drives its position,
   and the cursor never writes the composited output (Option C).
   ====================================================================== */

static void
test_seat_cursor(void)
{
    AxlCompositor *c = axl_compositor_new(SW, SH);
    AxlCursor *cur = axl_compositor_cursor(c);
    test_check(cur != NULL, "cursor: the compositor owns a cursor overlay");

    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 40, 40);
    axl_surface_move(a, 10, 10);
    fill_surface(a, 40, 40, AXL_GFX_RGB(0xC0, 0x30, 0x30));
    axl_compositor_composite(c);
    AxlGfxPixel before = scan_at(c, 20, 20);   /* the surface's pixel */

    /* A pointer event moves the cursor to (20,20) but must NOT touch the
       output buffer (Option C — the cursor composites only to the screen). */
    AxlInputEvent e = move_ev(20, 20);
    axl_compositor_pointer_event(c, &e);
    int32_t cx = -1, cy = -1;
    axl_cursor_position(cur, &cx, &cy);
    test_check(cx == 20 && cy == 20, "cursor: tracks the seat pointer position");
    test_check(rgb_eq(scan_at(c, 20, 20), before),
               "cursor: pointer events never write the composited output");

    /* set_cursor_image forwards to the cursor; NULL restores the arrow. */
    test_check(axl_compositor_set_cursor_image(c, NULL, 0, 0) == AXL_OK,
               "cursor: set_cursor_image(arrow) succeeds");
    test_check(axl_compositor_set_cursor_image(NULL, NULL, 0, 0) == AXL_ERR,
               "cursor: set_cursor_image(NULL compositor) errors");
    test_check(axl_compositor_cursor(NULL) == NULL, "cursor: NULL compositor has no cursor");

    axl_compositor_free(c);
}

/* ======================================================================
   E4 — property / fuzz: drive randomized op sequences over the surface
   tree + seat and assert invariants after every op. The strong observable
   invariant is that keyboard focus is always NULL or a LIVE surface (a
   dangling focus after a destroy is a use-after-free), plus crash-freedom
   (a composite/present + pointer/key routing over the random tree exercises
   the destroy-purge of focus/grab and the occlusion walk). A parent-tracked
   pool keeps subtree destroys consistent with the real tree.
   ====================================================================== */

#define FZ_POOL 12

typedef struct {
    AxlSurface *s;
    int         parent;   /* pool index of parent, or -1 for root */
    bool        alive;
} FzSlot;

static uint32_t fz_rng = 0xC0FFEEu;
static uint32_t
fz_next(void)
{
    fz_rng ^= fz_rng << 13;
    fz_rng ^= fz_rng >> 17;
    fz_rng ^= fz_rng << 5;
    return fz_rng;
}

/* Is pool index @a an ancestor-or-self of @b (via tracked parents)? */
static bool
fz_is_ancestor(const FzSlot pool[], int a, int b)
{
    for (int i = b; i != -1; i = pool[i].parent) {
        if (i == a) {
            return true;
        }
    }
    return false;
}

/* Mark @idx and all its descendants dead (mirrors a subtree destroy). */
static void
fz_kill_subtree(FzSlot pool[], int idx)
{
    for (int i = 0; i < FZ_POOL; i++) {
        if (pool[i].alive && fz_is_ancestor(pool, idx, i)) {
            pool[i].alive = false;
        }
    }
}

static int
fz_random_alive(const FzSlot pool[])   /* a live pool index, or -1 if none */
{
    int live[FZ_POOL], n = 0;
    for (int i = 0; i < FZ_POOL; i++) {
        if (pool[i].alive) live[n++] = i;
    }
    return (n == 0) ? -1 : live[fz_next() % (uint32_t)n];
}

static bool
fz_focus_is_live(const AxlCompositor *c, const FzSlot pool[])
{
    const AxlSurface *f = axl_compositor_keyboard_focus(c);
    if (f == NULL) {
        return true;   /* NULL focus is always valid */
    }
    for (int i = 0; i < FZ_POOL; i++) {
        if (pool[i].alive && pool[i].s == f) {
            return true;
        }
    }
    return false;   /* focus points at a non-live (dangling) surface! */
}

static void
fz_dismiss(void *user)   /* grab-dismiss callback (no-op) */
{
    (void)user;
}

static void
test_compositor_fuzz(void)
{
    bool ok = true;
    int fail_seq = -1;

    for (int seq = 0; seq < 60 && ok; seq++) {
        AxlCompositor *c = axl_compositor_new(SW, SH);
        if (c == NULL) {
            break;
        }
        FzSlot pool[FZ_POOL];
        for (int i = 0; i < FZ_POOL; i++) {
            pool[i] = (FzSlot){ NULL, -1, false };
        }

        for (int step = 0; step < 40 && ok; step++) {
            uint32_t op = fz_next() % 11;
            int idx = fz_random_alive(pool);
            switch (op) {
            case 0: {   /* create (child of root or a live surface) */
                int slot = -1;
                for (int i = 0; i < FZ_POOL; i++) {
                    if (!pool[i].alive) { slot = i; break; }
                }
                if (slot < 0) break;   /* pool full */
                int pidx = (fz_next() & 1) ? idx : -1;   /* -1 = root */
                AxlSurface *par = (pidx == -1) ? axl_compositor_root(c) : pool[pidx].s;
                AxlSurface *ns = axl_surface_create(par, 1 + fz_next() % 30, 1 + fz_next() % 24);
                if (ns != NULL) {
                    axl_surface_move(ns, (int32_t)(fz_next() % SW), (int32_t)(fz_next() % SH));
                    pool[slot] = (FzSlot){ ns, pidx, true };
                }
                break;
            }
            case 1:
                if (idx >= 0) {
                    axl_surface_move(pool[idx].s, (int32_t)(fz_next() % SW),
                                     (int32_t)(fz_next() % SH));
                }
                break;
            case 2: if (idx >= 0) axl_surface_raise(pool[idx].s); break;
            case 3: if (idx >= 0) axl_surface_lower(pool[idx].s); break;
            case 4: if (idx >= 0) axl_surface_set_visible(pool[idx].s, (fz_next() & 1) != 0); break;
            case 5: if (idx >= 0) axl_surface_set_opaque(pool[idx].s, (fz_next() & 1) != 0); break;
            case 6: {   /* set_parent — only when the lib won't reject (no cycle) */
                int p = fz_random_alive(pool);
                if (idx >= 0 && p >= 0 && p != idx && !fz_is_ancestor(pool, idx, p)) {
                    axl_surface_set_parent(pool[idx].s, pool[p].s);
                    pool[idx].parent = p;
                }
                break;
            }
            case 7: if (idx >= 0) {   /* destroy a subtree */
                axl_surface_destroy(pool[idx].s);
                fz_kill_subtree(pool, idx);
                break;
            }
            case 8: axl_compositor_set_keyboard_focus(c, idx >= 0 ? pool[idx].s : NULL); break;
            case 9: if (idx >= 0) axl_compositor_pointer_grab(c, pool[idx].s, fz_dismiss, NULL);
                    else axl_compositor_pointer_ungrab(c);
                    break;
            case 10: {   /* a pointer event (hit-test + focus/grab routing) */
                AxlInputEvent ev = {0};
                ev.type = (fz_next() & 1) ? AXL_INPUT_MOUSE_BUTTON_DOWN : AXL_INPUT_MOUSE_MOVE;
                ev.x = (int32_t)(fz_next() % SW);
                ev.y = (int32_t)(fz_next() % SH);
                ev.buttons = AXL_INPUT_BUTTON_LEFT;
                axl_compositor_pointer_event(c, &ev);
                break;
            }
            default: break;
            }

            /* Invariants after every op. */
            if (!fz_focus_is_live(c, pool)) {
                ok = false; break;   /* dangling keyboard focus — UAF */
            }
            (void)axl_compositor_present(c);   /* composite the random tree (no crash) */
            const AxlGfxRegion *d = axl_compositor_get_damage_region(c);
            if (!axl_gfx_region_is_empty(d)) {
                ok = false; break;   /* present must clear the damage */
            }
        }
        axl_compositor_free(c);
        if (!ok) fail_seq = seq;
    }
    if (!ok) {
        axl_printf("FAIL: compositor fuzz invariant broke at seq %d\n", fail_seq);
    }
    test_check(ok, "fuzz: 60x40 random tree+seat ops keep focus live + present clears damage");
}

int
test_compositor_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlCompositor");

    test_compositor_lifecycle();
    test_compositor_stacking();
    test_compositor_nested();
    test_compositor_offedge();
    test_compositor_raise_lower();
    test_compositor_reparent();
    test_compositor_occlusion();
    test_compositor_opacity();
    test_compositor_per_pixel_alpha();
    test_compositor_damage();
    test_compositor_damage_region();
    test_compositor_incremental_composite();
    test_compositor_partial_occlusion();
    test_compositor_occlusion_oom();
    test_compositor_occlusion_hoist();
    test_compositor_backdrop_blur();
    test_compositor_backdrop_blur_cache();
    test_compositor_frame_callbacks();
    test_compositor_frame_clock();
    test_compositor_fuzz();
    test_surface_resize();
    test_surface_absolute();

    test_seat_hittest();
    test_seat_buttons_axis();
    test_seat_modifiers_clicks();
    test_seat_refocus();
    test_seat_refocus_lower_reparent();
    test_seat_destroy_refocus();
    test_seat_input_region();
    test_seat_hidden_parent();
    test_seat_nulls();
    test_seat_pointer_gating();

    test_seat_grab();
    test_seat_grab_inside_click();
    test_seat_grab_dismiss_empty();
    test_seat_grab_nested();
    test_seat_grab_chain();
    test_seat_grab_lifo();
    test_seat_keyboard();
    test_seat_destroy_purges();
    test_seat_grab_nulls();

    test_seat_cursor();

    return test_print_results();
}

AXL_APP(test_compositor_main)
