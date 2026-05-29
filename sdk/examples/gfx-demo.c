/**
 * gfx-demo.c -- Graphics demo: rectangles + text rendering.
 *
 * Draws colored rectangles and text on the screen if GOP is available.
 * Falls back to a text message on headless/serial systems.
 *
 * Build with: make gfx-demo
 */

#include <axl.h>

/* Second built-in font from src/gfx/fonts/font-unifont-16.c — exposed
   via extern (no umbrella registry yet; AGT will add named lookup). */
extern const AxlFont axl_font_unifont_16;

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

    /* Draw a dark background — RGB notation: dark blue-gray = #1A1A2E */
    axl_gfx_fill_rect(0, 0, info.width, info.height,
                      AXL_GFX_RGB(0x1A, 0x1A, 0x2E));

    /* Draw colored rectangles using named palette + RGB macros. */
    uint32_t cx = info.width / 2;
    uint32_t cy = info.height / 2;
    uint32_t sz = 80;

    AxlGfxPixel red   = AXL_GFX_RED;
    AxlGfxPixel green = AXL_GFX_GREEN;
    AxlGfxPixel blue  = AXL_GFX_BLUE;
    AxlGfxPixel white = AXL_GFX_WHITE;

    axl_gfx_fill_rect(cx - sz*2, cy - sz/2, sz, sz, red);
    axl_gfx_fill_rect(cx - sz/2, cy - sz/2, sz, sz, green);
    axl_gfx_fill_rect(cx + sz,   cy - sz/2, sz, sz, blue);

    /* Small white square in the center */
    axl_gfx_fill_rect(cx - 10, cy - 10, 20, 20, white);

    /* Text rendering — default font (EDK2 LaffStd 8x16). */
    AxlGfxPixel    yellow  = AXL_GFX_YELLOW;
    AxlGfxPixel    cyan    = AXL_GFX_CYAN;
    const AxlFont *laffstd = axl_gfx_default_font();

    axl_gfx_draw_text(laffstd, 20, 20, "AXL Graphics Demo", white, 3);
    axl_gfx_draw_text(laffstd, 20, 80, "8x16 VGA bitmap font with scaling", yellow, 2);
    axl_gfx_draw_text(laffstd, 20, 120, "Scale 1: The quick brown fox", green, 1);

    /* Unifont — exercises the variable-width and stride=2 (wide-glyph)
       renderer paths through real UTF-8 input.  draw_text decodes UTF-8
       per axl_utf8_decode, so multi-byte characters render as a single
       glyph (not their component bytes). */
    const AxlFont *unifont = &axl_font_unifont_16;

    axl_gfx_draw_text(unifont, 20, 160,
                      "GNU Unifont 16.0.04 (subset):", white, 1);
    /* 2-byte UTF-8 Latin-1 (é è ê ë ö ñ ü ç). */
    axl_gfx_draw_text(unifont, 20, 180,
                      "  Latin-1: \xC3\xA9 \xC3\xA8 \xC3\xAA \xC3\xAB "
                      "\xC3\xB6 \xC3\xB1 \xC3\xBC \xC3\xA7",
                      cyan, 1);
    /* 3-byte UTF-8 box drawing — proves stride=1 (8-wide) renderer with
       3-byte UTF-8 decode. ┌──┐ │ │ └──┘ */
    axl_gfx_draw_text(unifont, 20, 200,
                      "  Box: \xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90 "
                      "\xE2\x94\x82  \xE2\x94\x82 \xE2\x94\x94\xE2\x94\x80"
                      "\xE2\x94\x80\xE2\x94\x98",
                      cyan, 1);
    /* 3-byte UTF-8 arrows. ← ↑ → ↓ */
    axl_gfx_draw_text(unifont, 20, 220,
                      "  Arrows: \xE2\x86\x90 \xE2\x86\x91 \xE2\x86\x92 \xE2\x86\x93",
                      cyan, 1);
    /* 3-byte UTF-8 CJK — exercises stride=2 (16-wide) renderer with
       multi-byte UTF-8 decode.  一 中 文 = U+4E00, U+4E2D, U+6587. */
    axl_gfx_draw_text(unifont, 20, 240,
                      "  CJK: \xE4\xB8\x80 \xE4\xB8\xAD \xE6\x96\x87 "
                      "(16-wide glyphs, stride=2)",
                      cyan, 1);

    /* Clipping demo: a 200x100 cyan-bordered window with a clip rect
       pushed inside it.  We fill a HUGE magenta rect (1000x1000) that
       would normally cover most of the screen; the clip restricts it
       to the window interior.  Text rendered with the clip active is
       cut off at the window edges.  After pop, drawing outside the
       window proves the stack restored cleanly. */
    AxlGfxPixel magenta = AXL_GFX_MAGENTA;
    AxlGfxPixel border  = AXL_GFX_CYAN;

    uint32_t cwx = 60;
    uint32_t cwy = 540;
    uint32_t cww = 360;
    uint32_t cwh = 120;

    /* Draw cyan window border (4 thin rects). */
    axl_gfx_fill_rect(cwx,         cwy,           cww,     1,       border);
    axl_gfx_fill_rect(cwx,         cwy + cwh - 1, cww,     1,       border);
    axl_gfx_fill_rect(cwx,         cwy,           1,       cwh,     border);
    axl_gfx_fill_rect(cwx + cww-1, cwy,           1,       cwh,     border);

    /* Push clip = the window interior (one pixel inside the border). */
    axl_gfx_push_clip((AxlGfxClip){
        .x = (int32_t)(cwx + 1),
        .y = (int32_t)(cwy + 1),
        .w = cww - 2,
        .h = cwh - 2,
    });

    /* Try to fill a massive magenta rect — should only render inside
       the cyan border. */
    axl_gfx_fill_rect(0, 0, 1000, 1000, magenta);

    /* Draw text that overflows the window on both sides; visible
       portion is the part inside the cyan border. */
    axl_gfx_draw_text(laffstd, cwx - 80, cwy + 30,
                      "<-- this text is clipped at both edges -->",
                      white, 1);
    axl_gfx_draw_text(laffstd, cwx + 10, cwy + 60,
                      "clipped fill + clipped text", yellow, 1);
    axl_gfx_draw_text(laffstd, cwx + 10, cwy + 90,
                      "all magenta confined to box.", green, 1);

    axl_gfx_pop_clip();

    /* After pop: drawing outside the window should work normally.
       This green text should appear OUTSIDE the cyan border. */
    axl_gfx_draw_text(laffstd, cwx + cww + 12, cwy + 30,
                      "<- post-pop text (no clip)", green, 1);

    /* Double-buffering demo: render a complete sub-scene into an
       off-screen buffer, then present it to the screen.  Visually
       the result should be indistinguishable from drawing directly. */
    AxlGfxBuffer *back = axl_gfx_buffer_new(200, 100);
    /* RGB notation throughout — much easier to read than BGR. */
    axl_gfx_buffer_clear(back, AXL_GFX_RGB(0x00, 0x20, 0x40));  /* navy bg */

    axl_gfx_target_buffer(back);
    /* Opaque fill + outline + text composed into the back-buffer. */
    axl_gfx_fill_rect(10, 10, 180, 80, AXL_GFX_RGB(0x40, 0x40, 0x80));  /* slate */
    axl_gfx_fill_rect(0,   0,  200, 1,  border);
    axl_gfx_fill_rect(0,   99, 200, 1,  border);
    axl_gfx_fill_rect(0,   0,  1,   100, border);
    axl_gfx_fill_rect(199, 0,  1,   100, border);
    axl_gfx_draw_text(laffstd, 18, 25, "double-buffered", white, 1);
    axl_gfx_draw_text(laffstd, 18, 45, "fill+border+text", yellow, 1);
    axl_gfx_draw_text(laffstd, 18, 65, "rendered into RAM", green, 1);
    /* Alpha-blended overlay: half-transparent green rect covering the
       right half of the back-buffer's interior.  Visible result:
       original slate interior on the left, blended muted blue-green
       on the right where 50%-alpha green composited over the slate. */
    axl_gfx_fill_rect(100, 10, 90, 80,
                      AXL_GFX_RGBA(0x00, 0xFF, 0x00, 0x80));  /* 50% green */
    axl_gfx_target_buffer(NULL);  /* restore screen target */

    /* Present (blit) the composed buffer to the screen. */
    axl_gfx_buffer_present(back, 700, 540);
    axl_gfx_buffer_free(back);

    axl_gfx_draw_text(laffstd, 700, 660,
                      "<- back-buffer presented here", green, 1);

    /* Line / rect-outline / polyline demo (Phase 0e). */
    axl_gfx_draw_text(laffstd, 460, 480,
                      "Lines + outline + polyline:", white, 1);
    /* Horizontal red line. */
    axl_gfx_draw_line(460, 500, 640, 500, AXL_GFX_RED);
    /* Diagonal green line. */
    axl_gfx_draw_line(460, 505, 640, 530, AXL_GFX_GREEN);
    /* Vertical blue line. */
    axl_gfx_draw_line(650, 480, 650, 530, AXL_GFX_BLUE);
    /* Rectangle outline (yellow). */
    axl_gfx_draw_rect(460, 460, 200, 20, AXL_GFX_YELLOW);
    /* Polyline forming an open zig-zag (magenta). */
    AxlGfxPoint zig[] = {
        {460, 540}, {490, 510}, {520, 540}, {550, 510}, {580, 540}, {610, 510},
    };
    axl_gfx_draw_polyline(zig, sizeof(zig) / sizeof(zig[0]), AXL_GFX_MAGENTA);

    axl_gfx_draw_text(laffstd, 20, info.height - 40,
                      "axl_gfx_draw_text() -- AximCode Library", white, 1);

    axl_printf("Drew rectangles + text + clipping + double-buffer + lines demos. "
               "Waiting 5 seconds...\n");
    axl_msleep(5000);

    return 0;
}
