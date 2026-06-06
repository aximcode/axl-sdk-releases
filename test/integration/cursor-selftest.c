/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * cursor-selftest.c — AxlCursor on-screen compositing self-test.
 *
 * The unit suite (axl-test-cursor.c) runs `-nographic` (no GOP), so it
 * can only assert the Option-C invariant in RAM: the cursor never writes
 * the bound scene.  It CANNOT see what actually lands on the screen,
 * because the cursor composites into a scratch buffer and presents that
 * via the GOP.  This test fills the gap against a REAL linear-framebuffer
 * GOP (the one OVMF exposes under `run-qemu.sh --gpu`): it builds a known
 * scene, presents it, shows + moves the cursor, then reads the
 * framebuffer back in-guest with axl_gfx_capture and asserts:
 *
 *   (a) cursor sprite pixels are present at the new (moved-to) position,
 *   (b) the OLD position reads back as the clean scene — no trail,
 *   (c) the bound scene back-buffer was never modified (Option C).
 *
 * Run via: scripts/run-qemu.sh --gpu cursor-selftest.efi
 * Driven by: test/integration/test-cursor-qemu.sh
 *
 * Emits "CURSOR-SELFTEST: <N> passed, <M> failed" as the final line so
 * the harness can scrape a verdict.
 */

#include <axl.h>
#include <axl/axl-cursor.h>

static int g_pass = 0;
static int g_fail = 0;

static void
check(
    bool         cond,
    const char  *label
    )
{
    if (cond) {
        g_pass++;
        axl_printf("PASS: %s\n", label);
    } else {
        g_fail++;
        axl_printf("FAIL: %s\n", label);
    }
}

/* Compare RGB channels only — GOP capture zeroes the reserved byte, so
   alpha is not meaningful across the round trip. */
static bool
rgb_eq(
    AxlGfxPixel  a,
    AxlGfxPixel  b
    )
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    if (!axl_gfx_available()) {
        axl_printf("CURSOR-SELFTEST: no GOP available (run under --gpu)\n");
        axl_printf("CURSOR-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }

    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    axl_printf("Display: %ux%u stride=%u fb=0x%llx (present path: %s)\n",
               info.width, info.height, info.stride,
               (unsigned long long)info.framebuffer,
               info.framebuffer != 0 ? "direct-FB" : "Blt fallback");

    /* A scene that easily fits the cursor sprite (12x19 built-in arrow)
       at two well-separated positions plus a margin. */
    const uint32_t SW = 128, SH = 96;
    if (info.width < SW || info.height < SH) {
        axl_printf("CURSOR-SELFTEST: screen too small (%ux%u)\n",
                   info.width, info.height);
        axl_printf("CURSOR-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }

    /* Flat scene color distinct from the arrow's outline (0x10) and body
       (0xFF) so a composited arrow pixel is unambiguously not-scene. */
    const AxlGfxPixel SCENE = AXL_GFX_RGB(0x33, 0x66, 0x99);

    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    AxlGfxBuffer *cap   = axl_gfx_buffer_new(SW, SH);
    if (scene == NULL || cap == NULL) {
        axl_printf("CURSOR-SELFTEST: buffer alloc failed\n");
        axl_printf("CURSOR-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }
    axl_gfx_buffer_clear(scene, SCENE);

    /* Snapshot the scene pixels to prove the cursor never touches them. */
    AxlGfxPixel *sp = axl_gfx_buffer_pixels(scene);
    AxlGfxPixel *snap = axl_malloc((size_t)SW * SH * sizeof(AxlGfxPixel));
    if (snap == NULL) {
        axl_printf("CURSOR-SELFTEST: snapshot alloc failed\n");
        axl_printf("CURSOR-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }
    for (size_t i = 0; i < (size_t)SW * SH; i++) {
        snap[i] = sp[i];
    }

    /* Present the clean scene to the top-left of the screen. */
    check(axl_gfx_buffer_present(scene, 0, 0) == AXL_OK,
          "present clean scene returns AXL_OK");

    AxlCursor *cur = axl_cursor_new(scene);
    check(cur != NULL, "cursor created on the scene");
    AxlGfxPixel *cp = axl_gfx_buffer_pixels(cap);

    /* Two well-separated hotspot positions (built-in arrow hotspot is the
       top-left pixel, so the sprite occupies [P .. P+sprite)). */
    const int32_t P1X = 20, P1Y = 20;
    const int32_t P2X = 80, P2Y = 60;

    axl_cursor_show(cur);
    axl_cursor_move(cur, P1X, P1Y);

    /* (a/b setup) Move to P2; P1 must be erased, P2 must show the arrow. */
    axl_cursor_move(cur, P2X, P2Y);

    /* (a) The arrow's top-left outline pixel (the hotspot) sits exactly on
       P2 and is 0x101010 — not the scene color. */
    check(axl_gfx_capture(cp, (uint32_t)P2X, (uint32_t)P2Y, 1, 1) == AXL_OK,
          "capture at new position returns AXL_OK");
    check(!rgb_eq(cp[0], SCENE),
          "cursor sprite is visible at the new position");
    check(rgb_eq(cp[0], AXL_GFX_RGB(0x10, 0x10, 0x10)),
          "new-position pixel is the arrow outline color");

    /* (b) The old position must be back to the clean scene — no trail. */
    check(axl_gfx_capture(cp, (uint32_t)P1X, (uint32_t)P1Y, 1, 1) == AXL_OK,
          "capture at old position returns AXL_OK");
    check(rgb_eq(cp[0], SCENE),
          "old position restored to scene (no trail)");

    /* A pixel well clear of either cursor rect must always be the scene. */
    check(axl_gfx_capture(cp, 4, 90, 1, 1) == AXL_OK,
          "capture clear area returns AXL_OK");
    check(rgb_eq(cp[0], SCENE), "untouched area is still the scene");

    /* Hide: the cursor must be erased back to scene at P2 too. */
    axl_cursor_hide(cur);
    check(axl_gfx_capture(cp, (uint32_t)P2X, (uint32_t)P2Y, 1, 1) == AXL_OK,
          "capture after hide returns AXL_OK");
    check(rgb_eq(cp[0], SCENE), "hide restores scene at the cursor position");

    /* (c) Through all of that, the bound scene back-buffer is unchanged. */
    bool scene_same = true;
    for (size_t i = 0; i < (size_t)SW * SH; i++) {
        if (sp[i].red != snap[i].red || sp[i].green != snap[i].green
            || sp[i].blue != snap[i].blue || sp[i].alpha != snap[i].alpha) {
            scene_same = false;
            break;
        }
    }
    check(scene_same, "bound scene buffer never modified (Option C invariant)");

    axl_cursor_free(cur);

    /* --- Option B: save-under (scene == NULL, direct-to-screen) --- */
    /* No back-buffer: the cursor captures the screen pixels under it and
       restores them on move.  Paint a known background straight to the
       screen (reusing the now-unbound scene buffer as a paint source),
       then drive a NULL-scene cursor over it and read back. */
    const AxlGfxPixel SCENE_B = AXL_GFX_RGB(0x22, 0x55, 0x88);
    axl_gfx_buffer_clear(scene, SCENE_B);
    axl_gfx_buffer_present(scene, 0, 0);
    /* A second known block flush to the screen's RIGHT edge, so a cursor
       moved there clips and exercises the row-by-row (edge-clipped)
       capture branch — the top-left block only hits the contiguous one. */
    const uint32_t EX = info.width - SW;
    axl_gfx_buffer_present(scene, EX, 0);

    AxlCursor *cb = axl_cursor_new(NULL);
    check(cb != NULL, "save-under cursor created with NULL scene (GOP present)");
    if (cb != NULL) {
        axl_cursor_show(cb);
        axl_cursor_move(cb, P1X, P1Y);
        axl_cursor_move(cb, P2X, P2Y);

        check(axl_gfx_capture(cp, (uint32_t)P2X, (uint32_t)P2Y, 1, 1) == AXL_OK,
              "save-under: capture at new position returns AXL_OK");
        check(rgb_eq(cp[0], AXL_GFX_RGB(0x10, 0x10, 0x10)),
              "save-under: arrow outline composited at the new position");

        check(axl_gfx_capture(cp, (uint32_t)P1X, (uint32_t)P1Y, 1, 1) == AXL_OK,
              "save-under: capture at old position returns AXL_OK");
        check(rgb_eq(cp[0], SCENE_B),
              "save-under: old position restored pixel-exact (no trail)");

        /* Right-edge clip: hotspot 3px from the edge, so only 3 sprite
           columns are visible (vw < spr_w) → row-by-row capture. The
           arrow's top-left outline pixel (the hotspot) is still drawn. */
        const int32_t ECX = (int32_t)info.width - 3, ECY = 10;
        axl_cursor_move(cb, ECX, ECY);
        check(axl_gfx_capture(cp, (uint32_t)ECX, (uint32_t)ECY, 1, 1) == AXL_OK,
              "save-under: capture at clipped right edge returns AXL_OK");
        check(rgb_eq(cp[0], AXL_GFX_RGB(0x10, 0x10, 0x10)),
              "save-under edge-clip: arrow outline drawn at the clipped hotspot");

        /* Move away; the clipped region must restore to its background. */
        axl_cursor_move(cb, 40, 40);
        check(axl_gfx_capture(cp, (uint32_t)ECX, (uint32_t)ECY, 1, 1) == AXL_OK,
              "save-under: re-capture clipped region returns AXL_OK");
        check(rgb_eq(cp[0], SCENE_B),
              "save-under edge-clip: clipped region restored pixel-exact");

        axl_cursor_hide(cb);
        check(axl_gfx_capture(cp, 40, 40, 1, 1) == AXL_OK,
              "save-under: capture after hide returns AXL_OK");
        check(rgb_eq(cp[0], SCENE_B),
              "save-under: hide restores the background");

        axl_cursor_free(cb);
    }

    axl_free(snap);
    axl_gfx_buffer_free(scene);
    axl_gfx_buffer_free(cap);

    axl_printf("CURSOR-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
