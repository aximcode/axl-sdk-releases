/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * gfx-present-selftest.c — GOP present-pipeline round-trip self-test.
 *
 * Exercises the Phase G17 (direct-framebuffer present) + G18 (region /
 * damage present) paths against a REAL linear-framebuffer GOP — the one
 * OVMF exposes when QEMU is launched with a virtual GPU.  The unit suite
 * runs `-nographic` (no GOP), so it can only verify the pure logic
 * (axl_gfx_pack_pixel, damage bbox math); this app fills the gap by
 * presenting a known pattern to the screen, reading it back in-guest via
 * axl_gfx_capture (GOP VideoToBltBuffer, which normalizes to BGRA), and
 * asserting the pixels survived the framebuffer write + format
 * conversion intact.  A wrong red/blue swap in the direct-FB path shows
 * up as swapped colors here.
 *
 * Run via: scripts/run-qemu.sh --gpu gfx-present-selftest.efi
 * Driven by: test/integration/test-gfx-present-qemu.sh
 *
 * Emits "PRESENT-SELFTEST: <N> passed, <M> failed" as the final line so
 * the harness can scrape a verdict.
 */

#include <axl.h>

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
        /* No GOP — the harness only runs us under --gpu, so this means a
           setup problem.  Emit a recognizable line and a nonzero verdict. */
        axl_printf("PRESENT-SELFTEST: no GOP available (run under --gpu)\n");
        axl_printf("PRESENT-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }

    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    /* A non-zero framebuffer base means OVMF exposed a linear FB, so the
       present path takes the G17 direct-write branch; a zero base means
       it falls back to GOP Blt.  Surfacing it documents which branch this
       run actually exercised. */
    axl_printf("Display: %ux%u stride=%u fb=0x%llx (present path: %s)\n",
               info.width, info.height, info.stride,
               (unsigned long long)info.framebuffer,
               info.framebuffer != 0 ? "direct-FB" : "Blt fallback");

    /* Four asymmetric quadrant colors — any channel permutation in the
       present path produces a mismatch on read-back. */
    const AxlGfxPixel cA = AXL_GFX_RGB(0xFF, 0x10, 0x20);  /* mostly red   */
    const AxlGfxPixel cB = AXL_GFX_RGB(0x20, 0xFF, 0x10);  /* mostly green */
    const AxlGfxPixel cC = AXL_GFX_RGB(0x10, 0x20, 0xFF);  /* mostly blue  */
    const AxlGfxPixel cD = AXL_GFX_RGB(0xAA, 0xBB, 0xCC);  /* mixed        */

    const uint32_t BW = 64, BH = 64;   /* buffer / pattern size */
    const uint32_t HALF = 32;

    AxlGfxBuffer *buf = axl_gfx_buffer_new(BW, BH);
    AxlGfxBuffer *cap = axl_gfx_buffer_new(BW, BH);
    if (buf == NULL || cap == NULL) {
        axl_printf("PRESENT-SELFTEST: buffer alloc failed\n");
        axl_printf("PRESENT-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }
    AxlGfxPixel *cp = axl_gfx_buffer_pixels(cap);

    /* Paint the four quadrants into the buffer (draw on the buffer
       target, then reset to screen). */
    axl_gfx_target_buffer(buf);
    axl_gfx_fill_rect(0,    0,    HALF, HALF, cA);   /* TL */
    axl_gfx_fill_rect(HALF, 0,    HALF, HALF, cB);   /* TR */
    axl_gfx_fill_rect(0,    HALF, HALF, HALF, cC);   /* BL */
    axl_gfx_fill_rect(HALF, HALF, HALF, HALF, cD);   /* BR */
    axl_gfx_target_buffer(NULL);

    /* --- G17: full-buffer present + capture round-trip --- */
    const uint32_t DX = 0, DY = 0;
    check(axl_gfx_buffer_present(buf, DX, DY) == AXL_OK,
          "G17 present whole buffer returns AXL_OK");
    check(axl_gfx_capture(cp, DX, DY, BW, BH) == AXL_OK,
          "G17 capture region returns AXL_OK");

    /* Sample one pixel per quadrant (centers) and confirm the color
       survived the FB write + format conversion. */
    check(rgb_eq(cp[16 * BW + 16], cA), "G17 TL quadrant reads back mostly-red");
    check(rgb_eq(cp[16 * BW + 48], cB), "G17 TR quadrant reads back mostly-green");
    check(rgb_eq(cp[48 * BW + 16], cC), "G17 BL quadrant reads back mostly-blue");
    check(rgb_eq(cp[48 * BW + 48], cD), "G17 BR quadrant reads back mixed");

    /* Explicit red/blue non-swap guard: TL must NOT read back as cC. */
    check(!rgb_eq(cp[16 * BW + 16], cC),
          "G17 red quadrant is not blue (no R/B swap)");

    /* Odd (non-16-byte-aligned) dst_x exercises the NT-store kernel's
       unaligned prefix — a row whose start isn't 16-aligned must NOT
       feed MOVNTDQ (which #GPs on misalignment).  Present at x=3 and
       read back the TL quadrant. */
    if (info.width >= 3 + BW && info.height >= 64 + BH) {
        check(axl_gfx_buffer_present(buf, 3, 64) == AXL_OK,
              "G17 present at odd dst_x=3 returns AXL_OK");
        check(axl_gfx_capture(cp, 3, 64, BW, BH) == AXL_OK,
              "G17 capture odd-offset region returns AXL_OK");
        check(rgb_eq(cp[16 * BW + 16], cA),
              "G17 odd dst_x: TL quadrant survives the unaligned NT prefix");
        check(rgb_eq(cp[48 * BW + 48], cD),
              "G17 odd dst_x: BR quadrant survives");
    } else {
        axl_printf("PRESENT-SELFTEST: screen too small for odd-offset test, skipping\n");
    }

    /* --- G18: region present --- */
    /* Present only the TR quadrant of `buf` to a fresh screen location;
       capture there and confirm just that 32x32 sub-region landed. */
    const uint32_t RX = 128, RY = 0;
    if (info.width >= RX + HALF && info.height >= RY + HALF) {
        check(axl_gfx_buffer_present_rect(buf, RX, RY, HALF, 0, HALF, HALF) == AXL_OK,
              "G18 present_rect TR sub-region returns AXL_OK");
        check(axl_gfx_capture(cp, RX, RY, HALF, HALF) == AXL_OK,
              "G18 capture sub-region returns AXL_OK");
        check(rgb_eq(cp[2 * HALF + 2], cB),
              "G18 present_rect placed the TR (green) quadrant");
    } else {
        axl_printf("PRESENT-SELFTEST: screen too small for region test, skipping\n");
    }

    /* --- G18: damage present --- */
    /* Repaint the whole buffer to a flat background, present it, then
       dirty one small rect, mark it as damage, and present_damage.
       Read back: the damaged rect shows the new color, the rest stays
       background — proving only the damage bbox was flushed. */
    const uint32_t GX = 0, GY = 128;
    if (info.width >= GX + BW && info.height >= GY + BH) {
        const AxlGfxPixel bg  = AXL_GFX_RGB(0x05, 0x06, 0x07);
        const AxlGfxPixel hot = AXL_GFX_RGB(0xF0, 0xE0, 0xD0);

        axl_gfx_target_buffer(buf);
        axl_gfx_fill_rect(0, 0, BW, BH, bg);
        axl_gfx_target_buffer(NULL);
        axl_gfx_buffer_present(buf, GX, GY);
        axl_gfx_buffer_clear_damage(buf);

        /* Dirty a 16x16 rect at (40,40) and mark exactly it as damage. */
        axl_gfx_target_buffer(buf);
        axl_gfx_fill_rect(40, 40, 16, 16, hot);
        axl_gfx_target_buffer(NULL);
        axl_gfx_buffer_add_damage(buf, (AxlGfxClip){40, 40, 16, 16});

        AxlGfxClip dmg;
        check(axl_gfx_buffer_get_damage(buf, &dmg) == AXL_OK
              && dmg.x == 40 && dmg.y == 40 && dmg.w == 16 && dmg.h == 16,
              "G18 damage bbox is exactly the dirtied rect");

        check(axl_gfx_buffer_present_damage(buf, GX, GY) == AXL_OK,
              "G18 present_damage returns AXL_OK");
        check(axl_gfx_buffer_get_damage(buf, &dmg) == AXL_ERR,
              "G18 present_damage clears the damage");

        /* Read back the whole region; the hot rect center must be `hot`,
           a pixel outside the damage must still be `bg`. */
        check(axl_gfx_capture(cp, GX, GY, BW, BH) == AXL_OK,
              "G18 capture damage region returns AXL_OK");
        check(rgb_eq(cp[48 * BW + 48], hot),
              "G18 present_damage flushed the dirtied rect");
        check(rgb_eq(cp[8 * BW + 8], bg),
              "G18 present_damage left the clean area untouched");
    } else {
        axl_printf("PRESENT-SELFTEST: screen too small for damage test, skipping\n");
    }

    axl_gfx_buffer_free(buf);
    axl_gfx_buffer_free(cap);

    axl_printf("PRESENT-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
