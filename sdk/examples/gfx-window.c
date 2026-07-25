/*
 * gfx-window.c -- AXL graphics showcase, for live GOP viewing.
 *
 * Draws a balanced scene that exercises a slice of axl_gfx -- gradients,
 * anti-aliased vector (TrueType) text, alpha compositing, and lines --
 * then BLOCKS on a keypress so the frame stays on screen.  Unlike a
 * draw-then-exit demo, holding on a key lets you actually look at the
 * result in a window.
 *
 * The whole scene is composed into a RAM back-buffer and presented in one
 * operation: atomic (no partial-draw flicker) and fast on any GOP -- a
 * direct-to-screen gradient is also fine since axl_gfx blits opaque
 * unclipped on-screen gradients in a single Blt, but a back-buffer is the
 * idiomatic pattern for a full frame.
 *
 * View it with the run-qemu.sh windowed modes:
 *   ./scripts/run-qemu.sh --gui gfx-window.efi              # GTK window (ssh -Y)
 *   ./scripts/run-qemu.sh --vnc gfx-window.efi              # serve VNC
 *   ./scripts/run-qemu.sh --vnc-reverse HOST:PORT gfx-window.efi
 *
 * Build with: make gfx-window
 */

#include <axl.h>

/* Draw UTF-8 vector text horizontally centered on @cx at @baseline. */
static void
centered(AxlTtf *f, int32_t cx, int32_t baseline, const char *s,
         float px, AxlGfxPixel color)
{
    int32_t w = (int32_t)axl_ttf_measure(f, s, px);
    axl_ttf_draw(f, cx - w / 2, baseline, s, px, color);
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    AxlGfxInfo info;
    if (!axl_gfx_available()) {
        axl_printf("No graphics output available (headless system).\n");
        return 0;
    }
    axl_gfx_get_info(&info);
    /* Serial-only status, emitted before any drawing. */
    axl_printf("gfx-window: %ux%u GOP framebuffer; drawing, then waiting "
               "for a key.\n", info.width, info.height);

    const int32_t W = (int32_t)info.width, H = (int32_t)info.height, cx = W / 2;
    AxlTtf *f = axl_ttf_default();
    const AxlGfxPixel ink = AXL_GFX_RGB(0xc4, 0xcb, 0xde);   /* caption color */

    /* Compose the whole frame in a back-buffer, present once. */
    AxlGfxBuffer *scene = axl_gfx_buffer_new((uint32_t)W, (uint32_t)H);
    if (scene == NULL) {
        axl_printf("gfx-window: back-buffer allocation failed.\n");
        return 1;
    }
    axl_gfx_target_buffer(scene);

    /* Background: vertical gradient. */
    AxlGfxGradient *bg = axl_gfx_gradient_linear_new(0, 0, 0, H);
    axl_gfx_gradient_add_stop(bg, 0.0f, AXL_GFX_RGB(0x1b, 0x1e, 0x34));
    axl_gfx_gradient_add_stop(bg, 1.0f, AXL_GFX_RGB(0x09, 0x0a, 0x14));
    axl_gfx_fill_rect_gradient(0, 0, W, H, bg);
    axl_gfx_gradient_free(bg);

    /* Title + subtitle (anti-aliased vector text). */
    centered(f, cx, 116, "AXL Graphics", 68.0f, AXL_GFX_WHITE);
    centered(f, cx, 162, "live GOP framebuffer - rendered by axl_gfx, viewed over QEMU",
             24.0f, AXL_GFX_RGB(0x96, 0xa2, 0xc6));

    /* Three gradient swatches with captions. */
    const char  *labels[3] = { "linear gradient", "two-stop", "diagonal" };
    AxlGfxPixel  top[3] = { AXL_GFX_RGB(0xff,0x5a,0x52), AXL_GFX_RGB(0x4a,0xe6,0x8a), AXL_GFX_RGB(0x57,0x9b,0xff) };
    AxlGfxPixel  bot[3] = { AXL_GFX_RGB(0x6b,0x10,0x3a), AXL_GFX_RGB(0x10,0x46,0x2e), AXL_GFX_RGB(0x12,0x1e,0x6e) };
    const int32_t sw = 300, sh = 168, sgap = 56;
    const int32_t sx0 = (W - (3 * sw + 2 * sgap)) / 2, sy = 208;
    for (int32_t i = 0; i < 3; i++) {
        int32_t x = sx0 + i * (sw + sgap);
        AxlGfxGradient *g = axl_gfx_gradient_linear_new(x, sy, x + sw, sy + sh);
        axl_gfx_gradient_add_stop(g, 0.0f, top[i]);
        axl_gfx_gradient_add_stop(g, 1.0f, bot[i]);
        axl_gfx_fill_rect_gradient(x, sy, sw, sh, g);
        axl_gfx_gradient_free(g);
        centered(f, x + sw / 2, sy + sh + 32, labels[i], 22.0f, ink);
    }

    /* Alpha compositing: a dark panel + three translucent rects whose
       overlaps source-over blend. */
    const int32_t pax = cx - 330, pay = 452;
    axl_gfx_fill_rect((uint32_t)pax, (uint32_t)pay, 300, 190, AXL_GFX_RGB(0x10, 0x12, 0x20));
    const uint8_t al = 0x9c;
    axl_gfx_fill_rect((uint32_t)(pax+30),  (uint32_t)(pay+20), 120, 120, AXL_GFX_RGBA(0xff,0x46,0x46, al));
    axl_gfx_fill_rect((uint32_t)(pax+110), (uint32_t)(pay+20), 120, 120, AXL_GFX_RGBA(0x46,0xe0,0x55, al));
    axl_gfx_fill_rect((uint32_t)(pax+70),  (uint32_t)(pay+62), 120, 120, AXL_GFX_RGBA(0x55,0x78,0xff, al));
    centered(f, pax + 150, pay + 190 + 30, "alpha compositing", 22.0f, ink);

    /* A color fan of lines. */
    const int32_t fax = cx + 250, fay = pay + 190;
    for (int32_t i = 0; i <= 12; i++) {
        int32_t ex = fax - 175 + i * 29;
        AxlGfxPixel c = AXL_GFX_RGB((uint8_t)(60+i*14), (uint8_t)(210-i*12), (uint8_t)(150+i*8));
        axl_gfx_draw_line(fax, fay, ex, fay - 175, c);
    }
    centered(f, fax, fay + 30, "lines", 22.0f, ink);

    centered(f, cx, H - 34, "Press any key to exit", 22.0f, AXL_GFX_RGB(0x76, 0x82, 0xa4));

    /* Present the composed frame in one operation. */
    axl_gfx_target_buffer(NULL);
    axl_gfx_buffer_present(scene, 0, 0);
    axl_gfx_buffer_free(scene);

    AxlKey k;
    if (axl_console_read_key(UINT64_MAX, &k) != AXL_OK) {
        return 1;   /* no console input available */
    }
    return 0;
}
