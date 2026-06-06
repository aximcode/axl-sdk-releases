/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * compositor-selftest.c — AxlCompositor end-to-end present test (Phase C1).
 *
 * The unit suite (axl-test-compositor.c) verifies compositing into the RAM
 * output, but can't see what reaches the screen (the -nographic harness
 * has no GOP, and aa64 has none at all). This runs against a real
 * linear-framebuffer GOP (run-qemu.sh --gpu): it composites two
 * overlapping surfaces, presents, reads the framebuffer back in-guest with
 * axl_gfx_capture, and asserts the on-screen stacking is correct; then it
 * moves a surface, presents again, and asserts the screen updated.
 *
 * (The "only the damage bbox is flushed" optimization is covered by the
 * unit damage tests + the gfx present_rect tests, not re-proven here —
 * every mutator damages, so the output and screen never legitimately
 * diverge outside the damage to observe via capture.)
 *
 * Run via: scripts/run-qemu.sh --gpu compositor-selftest.efi
 * Driven by: test/integration/test-compositor-qemu.sh
 *
 * Emits "COMPOSITOR-SELFTEST: <N> passed, <M> failed" as the final line.
 */

#include <axl.h>
#include <axl/axl-compositor.h>

static int g_pass = 0;
static int g_fail = 0;

static void
check(bool cond, const char *label)
{
    if (cond) { g_pass++; axl_printf("PASS: %s\n", label); }
    else      { g_fail++; axl_printf("FAIL: %s\n", label); }
}

static bool
rgb_eq(AxlGfxPixel a, AxlGfxPixel b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static void
fill(AxlSurface *s, uint32_t w, uint32_t h, AxlGfxPixel color)
{
    axl_gfx_target_buffer(axl_surface_buffer(s));
    axl_gfx_fill_rect(0, 0, w, h, color);
    axl_gfx_target_buffer(NULL);
}

/* C6: a surface that requests a custom cursor shape on pointer enter — the
   per-surface-shape seam (§2.4). */
static AxlCompositor  *g_comp;
static AxlGfxBuffer   *g_spr;
static void
cur_enter(void *user, int32_t x, int32_t y)
{
    (void)user; (void)x; (void)y;
    axl_compositor_set_cursor_image(g_comp, g_spr, 0, 0);
}
static const AxlSurfaceListener CUR_LISTENER = { .enter = cur_enter };

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!axl_gfx_available()) {
        axl_printf("COMPOSITOR-SELFTEST: no GOP available (run under --gpu)\n");
        axl_printf("COMPOSITOR-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }

    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    const uint32_t CW = 128, CH = 96;
    if (info.width < CW || info.height < CH) {
        axl_printf("COMPOSITOR-SELFTEST: screen too small (%ux%u)\n",
                   info.width, info.height);
        axl_printf("COMPOSITOR-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }
    axl_printf("Display: %ux%u (present path: %s)\n", info.width, info.height,
               info.framebuffer != 0 ? "direct-FB" : "Blt fallback");

    const AxlGfxPixel BG  = AXL_GFX_RGB(0x00, 0x00, 0x00);
    const AxlGfxPixel RED = AXL_GFX_RGB(0xE0, 0x10, 0x10);
    const AxlGfxPixel GRN = AXL_GFX_RGB(0x10, 0xE0, 0x10);

    /* The compositor presents its output to the screen at matching coords
       (damage bbox), so screen (x,y) == output (x,y) for x<CW, y<CH. */
    AxlCompositor *c = axl_compositor_new(CW, CH);
    check(c != NULL, "compositor created");
    if (c == NULL) {
        axl_printf("COMPOSITOR-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }
    AxlSurface *a = axl_surface_create(axl_compositor_root(c), 40, 40); /* bottom */
    AxlSurface *b = axl_surface_create(axl_compositor_root(c), 40, 40); /* top */
    axl_surface_move(a, 10, 10);   /* 10..50 */
    axl_surface_move(b, 30, 30);   /* 30..70, overlaps a on 30..50 */
    fill(a, 40, 40, RED);
    fill(b, 40, 40, GRN);

    check(axl_compositor_present(c) == AXL_OK, "present full frame returns AXL_OK");

    AxlGfxBuffer *cap = axl_gfx_buffer_new(CW, CH);
    AxlGfxPixel  *cp  = axl_gfx_buffer_pixels(cap);
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "capture frame returns AXL_OK");

    check(rgb_eq(cp[5 * CW + 5], BG),    "screen: uncovered area is background");
    check(rgb_eq(cp[15 * CW + 15], RED), "screen: A-only area is red");
    check(rgb_eq(cp[55 * CW + 55], GRN), "screen: B-only area is green");
    check(rgb_eq(cp[35 * CW + 35], GRN), "screen: overlap shows B (top)");

    /* Move A clear of the overlap; present; the screen must reflect it. */
    axl_surface_move(a, 80, 5);    /* now 80..120, 5..45 */
    check(axl_compositor_present(c) == AXL_OK, "present after move returns AXL_OK");
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "recapture returns AXL_OK");
    check(rgb_eq(cp[15 * CW + 15], BG),  "screen: A's old position cleared");
    check(rgb_eq(cp[15 * CW + 90], RED), "screen: A painted at its new position");
    check(rgb_eq(cp[55 * CW + 55], GRN), "screen: B unchanged by A's move");

    /* C2: bring A back over B and raise it — the overlap must show A. */
    axl_surface_move(a, 10, 10);   /* overlaps B (30..50) again */
    axl_surface_raise(a);
    check(axl_compositor_present(c) == AXL_OK, "present after raise returns AXL_OK");
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "recapture after raise returns AXL_OK");
    check(rgb_eq(cp[35 * CW + 35], RED), "screen: raise(A) puts A on top in the overlap");
    axl_surface_raise(b);
    check(axl_compositor_present(c) == AXL_OK, "present after raise(B) returns AXL_OK");
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "recapture returns AXL_OK");
    check(rgb_eq(cp[35 * CW + 35], GRN), "screen: raise(B) restores B on top");

    /* C3: a translucent full-screen veil dims everything beneath it. The
       overlap pixel (GRN) must read back as GRN blended with the veil. */
    AxlSurface *veil = axl_surface_create(axl_compositor_root(c), CW, CH);
    fill(veil, CW, CH, AXL_GFX_RGB(0x10, 0x10, 0x10));
    axl_surface_set_opacity(veil, 128);
    check(axl_compositor_present(c) == AXL_OK, "present veil returns AXL_OK");
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "recapture after veil returns AXL_OK");
    AxlGfxPixel veil_over_grn = axl_gfx_composite(GRN, AXL_GFX_RGBA(0x10, 0x10, 0x10, 128));
    check(rgb_eq(cp[35 * CW + 35], veil_over_grn), "screen: veil dims content beneath (blended)");
    check(!rgb_eq(cp[35 * CW + 35], GRN), "screen: veil actually changed the pixel");

    /* C6: the cursor is the top overlay, driven by the seat pointer, with a
       per-surface shape. Use a solid surface + opaque sprite for determinism.
       Drop the veil and lay a solid CYAN surface on top as the scene. */
    axl_surface_destroy(veil);
    const AxlGfxPixel CYAN = AXL_GFX_RGB(0x10, 0xE0, 0xE0);
    const AxlGfxPixel MAG  = AXL_GFX_RGB(0xFF, 0x00, 0xFF);
    AxlSurface *cs = axl_surface_create(axl_compositor_root(c), 100, 80);
    axl_surface_move(cs, 10, 10);     /* abs 10..110 / 10..90 */
    fill(cs, 100, 80, CYAN);
    g_comp = c;
    g_spr  = axl_gfx_buffer_new(8, 8);
    axl_gfx_target_buffer(g_spr);
    axl_gfx_fill_rect(0, 0, 8, 8, AXL_GFX_RGBA(0xFF, 0x00, 0xFF, 0xFF));  /* opaque magenta */
    axl_gfx_target_buffer(NULL);
    axl_surface_set_listener(cs, &CUR_LISTENER, NULL);
    check(axl_compositor_present(c) == AXL_OK, "present cs (cursor scene) returns AXL_OK");
    check(axl_compositor_cursor(c) != NULL, "cursor: compositor owns a cursor overlay");

    /* Move the pointer over cs at P1: enter sets the custom sprite, which
       composites over the scene at the hotspot. */
    AxlInputEvent e = (AxlInputEvent){ .type = AXL_INPUT_MOUSE_MOVE, .x = 30, .y = 30 };
    axl_compositor_pointer_event(c, &e);
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "capture with cursor at P1 returns AXL_OK");
    check(rgb_eq(cp[30 * CW + 30], MAG),  "cursor: custom sprite shows over the output at the hotspot");
    check(rgb_eq(cp[30 * CW + 60], CYAN), "cursor: the surface shows where the sprite isn't");

    /* Move to P2 (still inside cs, so the shape stays): the old position must
       restore to the scene (no trail) and the sprite appears at the new one. */
    e.x = 70; e.y = 60;
    axl_compositor_pointer_event(c, &e);
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "recapture with cursor at P2 returns AXL_OK");
    check(rgb_eq(cp[30 * CW + 30], CYAN), "cursor: old position restored — no trail");
    check(rgb_eq(cp[60 * CW + 70], MAG),  "cursor: sprite drawn at the new position");

    /* Move onto background (outside cs): focus leaves, so the shape resets and
       the sprite is un-drawn at P2 (restored to scene). */
    e.x = 120; e.y = 5;
    axl_compositor_pointer_event(c, &e);
    check(axl_gfx_capture(cp, 0, 0, CW, CH) == AXL_OK, "recapture cursor on background returns AXL_OK");
    check(rgb_eq(cp[60 * CW + 70], CYAN), "cursor: P2 restored after the cursor moved away (no trail)");
    /* The shape reset to the arrow on leaving cs: the custom magenta sprite is
       no longer drawn at the cursor position (a wrongly-kept shape → MAG). */
    check(!rgb_eq(cp[5 * CW + 120], MAG), "cursor: shape reset to the arrow on leaving the surface");

    axl_gfx_buffer_free(g_spr);
    axl_compositor_free(c);
    axl_gfx_buffer_free(cap);

    axl_printf("COMPOSITOR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
