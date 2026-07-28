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

/* --- E10 backdrop-blur seam helpers ------------------------------------- */

/* Paint @r in BOTH the live backdrop surface and the shadow copy that mirrors
   it, then damage @r. The shadow is the oracle's input, so the two can never
   drift. */
static void
bd_fill(AxlSurface *bd, AxlGfxBuffer *shadow, AxlGfxClip r, AxlGfxPixel col)
{
    axl_gfx_target_buffer(axl_surface_buffer(bd));
    axl_gfx_fill_rect(r.x, r.y, r.w, r.h, col);
    axl_gfx_target_buffer(shadow);
    axl_gfx_fill_rect(r.x, r.y, r.w, r.h, col);
    axl_gfx_target_buffer(NULL);
    axl_surface_damage(bd, r);
}

/* Blur @veil's rect of @img in place, clamping at that rect's own edges —
   exactly the full-veil re-blur a partial re-blur must reproduce. Called once
   per veil back-to-front over a copy of the scene, this builds the oracle
   frame. Returns false on OOM. */
static bool
blur_veil_rect(AxlGfxBuffer *img, uint32_t w, AxlGfxClip veil, uint32_t radius)
{
    AxlGfxPixel  *ip   = axl_gfx_buffer_pixels(img);
    AxlGfxBuffer *crop = axl_gfx_buffer_new(veil.w, veil.h);
    AxlGfxPixel  *cp2  = (crop != NULL) ? axl_gfx_buffer_pixels(crop) : NULL;
    if (ip == NULL || cp2 == NULL) {
        axl_gfx_buffer_free(crop);
        return false;
    }
    for (uint32_t j = 0; j < veil.h; j++) {
        for (uint32_t i = 0; i < veil.w; i++) {
            cp2[j * veil.w + i] = ip[(size_t)(veil.y + (int32_t)j) * w + veil.x + (int32_t)i];
        }
    }
    axl_gfx_buffer_blur(crop, radius);
    for (uint32_t j = 0; j < veil.h; j++) {
        for (uint32_t i = 0; i < veil.w; i++) {
            ip[(size_t)(veil.y + (int32_t)j) * w + veil.x + (int32_t)i] = cp2[j * veil.w + i];
        }
    }
    axl_gfx_buffer_free(crop);
    return true;
}

/* Compare a captured frame against a reference over the WHOLE region — one
   probe pixel cannot see a seam, which is the failure mode this guards.
   Reports the first differing pixel and the total count. RGB only (the
   framebuffer carries no meaningful alpha to read back). */
static void
check_frame_eq(const AxlGfxPixel *cap, const AxlGfxPixel *ref, uint32_t w,
               uint32_t h, const char *label)
{
    uint32_t bad = 0;
    uint32_t fx = 0, fy = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            if (!rgb_eq(cap[(size_t)y * w + x], ref[(size_t)y * w + x])) {
                if (bad == 0) { fx = x; fy = y; }
                bad++;
            }
        }
    }
    if (bad != 0) {
        axl_printf("  %u/%u px differ; first (%u,%u) got %02x%02x%02x want %02x%02x%02x\n",
                   bad, w * h, fx, fy,
                   cap[(size_t)fy * w + fx].red, cap[(size_t)fy * w + fx].green,
                   cap[(size_t)fy * w + fx].blue,
                   ref[(size_t)fy * w + fx].red, ref[(size_t)fy * w + fx].green,
                   ref[(size_t)fy * w + fx].blue);
    }
    check(bad == 0, label);
}

/* A fully transparent per-pixel-alpha surface with a backdrop blur. Because it
   contributes no color of its own, the frame under it IS the blurred backdrop
   — which is what makes the blur independently checkable. */
static AxlSurface *
new_blur_veil(AxlCompositor *c, AxlGfxClip r, uint32_t radius)
{
    AxlSurface *v = axl_surface_new(axl_compositor_root(c), r.w, r.h);
    if (v == NULL) {
        return NULL;
    }
    AxlGfxPixel *vp = axl_gfx_buffer_pixels(axl_surface_buffer(v));
    if (vp == NULL) {
        axl_surface_free(v);
        return NULL;
    }
    for (uint32_t i = 0; i < r.w * r.h; i++) {
        vp[i] = AXL_GFX_RGBA(0, 0, 0, 0);
    }
    axl_surface_set_per_pixel_alpha(v, true);
    axl_surface_move(v, r.x, r.y);
    axl_surface_set_backdrop_blur(v, radius);
    return v;
}

/* Present the pending damage, read the SCREEN back, and compare the whole
   frame against an oracle rebuilt from the shadow scene: the veils blurred
   back-to-front over their full rects. Reading the framebuffer (not the
   compositor's output buffer) also pins the flush region — a partial re-blur
   that forgets to flush a changed pixel shows up here as a stale capture. */
static void
present_check(AxlCompositor *c, AxlGfxBuffer *shadow, AxlGfxPixel *cap,
              uint32_t w, uint32_t h, const AxlGfxClip *veils,
              const uint32_t *radii, size_t nveil, const char *label)
{
    AxlGfxBuffer *ora = axl_gfx_buffer_new(w, h);
    AxlGfxPixel  *op  = (ora != NULL) ? axl_gfx_buffer_pixels(ora) : NULL;
    AxlGfxPixel  *sp  = axl_gfx_buffer_pixels(shadow);
    if (op == NULL || sp == NULL) {
        check(false, label);
        axl_gfx_buffer_free(ora);
        return;
    }
    axl_compositor_present(c);
    if (axl_gfx_capture(cap, 0, 0, w, h) != AXL_OK) {
        check(false, label);
        axl_gfx_buffer_free(ora);
        return;
    }
    axl_memcpy(op, sp, (size_t)w * h * sizeof(AxlGfxPixel));
    for (size_t k = 0; k < nveil; k++) {
        if (!blur_veil_rect(ora, w, veils[k], radii[k])) {
            check(false, label);
            axl_gfx_buffer_free(ora);
            return;
        }
    }
    check_frame_eq(cap, op, w, h, label);
    axl_gfx_buffer_free(ora);
}

/* E10: a partial-damage present under a backdrop-blur veil must be pixel-for-
   pixel identical to a full-veil re-blur. This is the guard the "re-blur only
   the damage plus its blur halo" optimization rests on: the halo ring reads
   the PREVIOUS frame's already-frosted pixels unless the implementation
   handles it, which seams — and compounds frame over frame. Every case here
   compares the ENTIRE frame, because a single probe pixel cannot see a seam. */
static void
test_backdrop_blur_seam(void)
{
    const uint32_t BW = 160, BH = 120, R = 8;
    const AxlGfxPixel BASE = AXL_GFX_RGB(0x10, 0x18, 0x30);
    const AxlGfxPixel WARM = AXL_GFX_RGB(0xE0, 0xD0, 0x40);
    const AxlGfxPixel COOL = AXL_GFX_RGB(0x30, 0xC0, 0x80);
    const AxlGfxPixel HOT  = AXL_GFX_RGB(0xFF, 0x00, 0x60);

    AxlCompositor *c      = axl_compositor_new(BW, BH);
    AxlGfxBuffer  *shadow = axl_gfx_buffer_new(BW, BH);
    AxlGfxBuffer  *capbuf = axl_gfx_buffer_new(BW, BH);
    AxlSurface    *bd     = (c != NULL)
                          ? axl_surface_new(axl_compositor_root(c), BW, BH) : NULL;
    if (c == NULL || shadow == NULL || capbuf == NULL || bd == NULL) {
        check(false, "bdblur seam: scene allocated");
        axl_gfx_buffer_free(shadow);
        axl_gfx_buffer_free(capbuf);
        axl_compositor_free(c);
        return;
    }
    AxlGfxPixel *cap = axl_gfx_buffer_pixels(capbuf);

    /* A plaid of hard edges in BOTH axes — high frequency, so a blur that
       reads even one wrong source pixel moves the result visibly. */
    bd_fill(bd, shadow, (AxlGfxClip){0, 0, BW, BH}, BASE);
    for (int32_t x = 0; x + 8 <= (int32_t)BW; x += 16) {
        bd_fill(bd, shadow, (AxlGfxClip){x, 0, 8, BH}, WARM);
    }
    for (int32_t y = 0; y + 6 <= (int32_t)BH; y += 24) {
        bd_fill(bd, shadow, (AxlGfxClip){0, y, BW, 6}, COOL);
    }

    AxlGfxClip veil_r  = {0, 0, BW, BH};   /* full-screen veil (a modal dim) */
    uint32_t   veil_rad = R;
    AxlSurface *veil = new_blur_veil(c, veil_r, R);
    check(veil != NULL, "bdblur seam: blur veil created");

    present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1,
                  "bdblur seam: full present matches the whole-frame blur oracle");

    /* The core case: a small change under the veil. */
    bd_fill(bd, shadow, (AxlGfxClip){60, 40, 12, 9}, HOT);
    present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1,
                  "bdblur seam: a small partial damage matches the full-blur oracle");

    /* Edges + corner: the blur clamps at the veil's own border, so a damage
       that touches it must NOT be eroded away there. */
    bd_fill(bd, shadow, (AxlGfxClip){0, 30, 10, 10}, WARM);
    present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1,
                  "bdblur seam: damage on the veil's left edge stays exact");
    bd_fill(bd, shadow, (AxlGfxClip){(int32_t)BW - 6, (int32_t)BH - 5, 6, 5}, HOT);
    present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1,
                  "bdblur seam: damage in the bottom-right corner stays exact");

    /* A single pixel — the smallest possible halo. */
    bd_fill(bd, shadow, (AxlGfxClip){77, 60, 1, 1}, COOL);
    present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1,
                  "bdblur seam: a 1x1 damage stays exact");

    /* A damage larger than the blur radius in both axes. */
    bd_fill(bd, shadow, (AxlGfxClip){40, 20, 70, 60}, BASE);
    present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1,
                  "bdblur seam: a damage much larger than the radius stays exact");

    /* Two disjoint damages in ONE frame. */
    bd_fill(bd, shadow, (AxlGfxClip){5, 5, 8, 8}, HOT);
    bd_fill(bd, shadow, (AxlGfxClip){140, 100, 10, 10}, WARM);
    present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1,
                  "bdblur seam: two disjoint damages in one frame stay exact");

    /* Repeat over the SAME spot: re-blurring an already-frosted halo drifts a
       little per frame, so the compounding is what catches it. */
    for (int i = 0; i < 4; i++) {
        char label[96];
        bd_fill(bd, shadow, (AxlGfxClip){90, 70, 9, 7},
                (i & 1) ? COOL : HOT);
        axl_snprintf(label, sizeof(label),
                     "bdblur seam: repeated re-blur of the same spot, pass %d", i + 1);
        present_check(c, shadow, cap, BW, BH, &veil_r, &veil_rad, 1, label);
    }
    axl_surface_free(veil);

    /* A DIALOG veil (not full-screen): the blur clamps at the dialog's edges,
       and damage outside it must not disturb it. */
    AxlGfxClip dlg_r   = {30, 20, 90, 70};
    uint32_t   dlg_rad = R;
    AxlSurface *dlg = new_blur_veil(c, dlg_r, R);
    check(dlg != NULL, "bdblur seam: dialog veil created");
    present_check(c, shadow, cap, BW, BH, &dlg_r, &dlg_rad, 1,
                  "bdblur seam: dialog veil full present matches the oracle");
    bd_fill(bd, shadow, (AxlGfxClip){20, 50, 30, 20}, HOT);   /* straddles the edge */
    present_check(c, shadow, cap, BW, BH, &dlg_r, &dlg_rad, 1,
                  "bdblur seam: damage straddling the dialog's edge stays exact");
    bd_fill(bd, shadow, (AxlGfxClip){0, 100, 20, 15}, COOL);  /* fully outside */
    present_check(c, shadow, cap, BW, BH, &dlg_r, &dlg_rad, 1,
                  "bdblur seam: damage entirely outside the dialog stays exact");

    /* Two OVERLAPPING veils: the upper one blurs the lower one's frosted
       output, so its source is only valid where the lower one is. */
    AxlGfxClip veils2[2] = { dlg_r, {60, 40, 90, 70} };
    uint32_t   rads2[2]  = { R, 4 };
    AxlSurface *dlg2 = new_blur_veil(c, veils2[1], rads2[1]);
    check(dlg2 != NULL, "bdblur seam: second overlapping veil created");
    present_check(c, shadow, cap, BW, BH, veils2, rads2, 2,
                  "bdblur seam: stacked overlapping veils, full present");
    bd_fill(bd, shadow, (AxlGfxClip){70, 55, 10, 10}, HOT);   /* under both */
    present_check(c, shadow, cap, BW, BH, veils2, rads2, 2,
                  "bdblur seam: damage under two stacked veils stays exact");

    /* Damage under ONLY the upper veil, in the part that overhangs the lower
       one. The lower veil is not reached until the upper one's rect has been
       folded into the damage, so an expansion that visits each veil once (in
       paint order, lower first) leaves the lower one to blur a sub-rect —
       clamped mid-surface, which seams. Pins the fixpoint. */
    bd_fill(bd, shadow, (AxlGfxClip){130, 95, 10, 10}, WARM);
    present_check(c, shadow, cap, BW, BH, veils2, rads2, 2,
                  "bdblur seam: damage reaching only the upper veil re-blurs the lower one");

    axl_gfx_buffer_free(shadow);
    axl_gfx_buffer_free(capbuf);
    axl_compositor_free(c);
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
    AxlSurface *a = axl_surface_new(axl_compositor_root(c), 40, 40); /* bottom */
    AxlSurface *b = axl_surface_new(axl_compositor_root(c), 40, 40); /* top */
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
    AxlSurface *veil = axl_surface_new(axl_compositor_root(c), CW, CH);
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
    axl_surface_free(veil);
    const AxlGfxPixel CYAN = AXL_GFX_RGB(0x10, 0xE0, 0xE0);
    const AxlGfxPixel MAG  = AXL_GFX_RGB(0xFF, 0x00, 0xFF);
    AxlSurface *cs = axl_surface_new(axl_compositor_root(c), 100, 80);
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

    /* E10: partial re-blur under a backdrop-blur veil (own scene + compositor). */
    test_backdrop_blur_seam();

    axl_printf("COMPOSITOR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
