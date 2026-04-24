/**
 * gfx-demo.c -- Graphics demo: rectangles + text rendering.
 *
 * Draws colored rectangles and text on the screen if GOP is available.
 * Falls back to a text message on headless/serial systems.
 *
 * Build with: make gfx-demo
 */

#include <axl.h>

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    AxlGfxInfo info;
    if (!axl_gfx_available()) {
        axl_printf("No graphics output available (headless system).\n");
        return 0;
    }

    axl_gfx_get_info(&info);
    axl_printf("Display: %ux%u (stride=%u, fb=0x%llx)\n",
               info.width, info.height, info.stride,
               (unsigned long long)info.framebuffer);

    /* Draw a dark background */
    AxlGfxPixel bg = {0x2E, 0x1A, 0x1A, 0};   /* dark blue-gray */
    axl_gfx_fill_rect(0, 0, info.width, info.height, bg);

    /* Draw colored rectangles */
    uint32_t cx = info.width / 2;
    uint32_t cy = info.height / 2;
    uint32_t sz = 80;

    AxlGfxPixel red   = {0x00, 0x00, 0xFF, 0};
    AxlGfxPixel green = {0x00, 0xFF, 0x00, 0};
    AxlGfxPixel blue  = {0xFF, 0x00, 0x00, 0};
    AxlGfxPixel white = {0xFF, 0xFF, 0xFF, 0};

    axl_gfx_fill_rect(cx - sz*2, cy - sz/2, sz, sz, red);
    axl_gfx_fill_rect(cx - sz/2, cy - sz/2, sz, sz, green);
    axl_gfx_fill_rect(cx + sz,   cy - sz/2, sz, sz, blue);

    /* Small white square in the center */
    axl_gfx_fill_rect(cx - 10, cy - 10, 20, 20, white);

    /* Text rendering demo */
    AxlGfxPixel yellow = {0x00, 0xFF, 0xFF, 0};

    axl_gfx_draw_text(20, 20, "AXL Graphics Demo", white, 3);
    axl_gfx_draw_text(20, 80, "8x16 VGA bitmap font with scaling", yellow, 2);
    axl_gfx_draw_text(20, 120, "Scale 1: The quick brown fox", green, 1);
    axl_gfx_draw_text(20, info.height - 40,
                      "axl_gfx_draw_text() -- AximCode Library", white, 1);

    axl_printf("Drew rectangles + text. Waiting 5 seconds...\n");
    axl_msleep(5000);

    return 0;
}
