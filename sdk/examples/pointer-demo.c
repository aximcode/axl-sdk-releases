/*
 * pointer-demo.c -- live pointer-cursor demo over a GOP framebuffer.
 *
 * Attaches BOTH pointer kinds to one axl-loop and draws a crosshair
 * cursor that follows the physical pointer, with a HUD showing the
 * coordinates and button/contact state.  The loop HOLDS until a key is
 * pressed, so you can move the pointer and watch the cursor track it --
 * unlike a draw-then-exit demo, which would vanish before you could
 * look.
 *
 * Two pointer sources, because the two device classes report position
 * differently and a real GUI consumes whichever is present:
 *
 *   - axl_input_attach_mouse  (EFI_SIMPLE_POINTER, relative motion):
 *     a physical USB/PS2 mouse.  This is the visual companion to the
 *     locate_physical_pointer fix -- if attach succeeds and the cursor
 *     responds, attach bound the *physical* pointer rather than the
 *     dead ConSplitter console aggregator.  Relative deltas accumulate
 *     from the start position; this drives the cursor on real hardware.
 *
 *   - axl_input_attach_touch  (EFI_ABSOLUTE_POINTER, absolute coords):
 *     a touchscreen, or -- crucially for remote viewing -- QEMU's
 *     usb-tablet, which run-qemu.sh adds for live VNC display.  Over
 *     VNC a relative mouse can't track full-range (the host delivers
 *     absolute motion that QEMU mis-scales into relative deltas), so
 *     when an absolute device is present we let IT position the cursor:
 *     absolute events are authoritative, and the relative mouse then
 *     only contributes button state.  On bare metal with no tablet the
 *     mouse drives position as usual.
 *
 * Use a boot-class pointer (QEMU `-device usb-mouse`, not `usb-tablet`)
 * so OVMF binds EFI_SIMPLE_POINTER for the mouse path.
 *
 * View it with the run-qemu.sh windowed modes (mouse-enabled OVMF):
 *   ./scripts/run-qemu.sh --vnc-reverse HOST:PORT pointer-demo.efi
 *
 * VNC caveat: a VNC viewer delivers RELATIVE motion to a relative mouse
 * by differencing the host pointer position scaled into its window.  If
 * the viewer window's aspect ratio differs from the framebuffer's, the
 * two axes scale unequally and the cursor under-tracks on one axis (and
 * "snaps" on window entry).  View at 1:1 / native size to avoid it; on
 * real hardware a relative mouse reports 1:1 counts and tracks exactly.
 * An absolute pointer is immune (its coordinates are framebuffer
 * fractions) where the firmware delivers EFI_ABSOLUTE_POINTER events.
 *
 * Build with: make pointer-demo
 */

#include <axl.h>

typedef struct {
    AxlGfxBuffer  *bg;        /* static background, re-presented each frame */
    int32_t        w, h;
    int32_t        x, y;      /* clamped cursor position (pixels) */
    bool           left;      /* left button / touch contact down */
    bool           right;     /* right button down */
    bool           absolute;  /* an absolute device is positioning the cursor */
    uint32_t       abs_evts;  /* absolute (touch) event count */
    uint32_t       rel_evts;  /* relative (mouse) event count */
    int32_t        max_x;     /* furthest extent reached (for post-run summary) */
    int32_t        max_y;
} PointerDemo;

/* Clamp @v to [0, hi]. */
static int32_t
clampi(int32_t v, int32_t hi)
{
    if (v < 0) {
        return 0;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Re-present the background, then draw the cursor + HUD at the current
   position.  Full-frame redraw per event: simple and flicker-free
   because the background blit erases the previous cursor. */
static void
redraw(PointerDemo *d)
{
    axl_gfx_target_buffer(NULL);
    axl_gfx_buffer_present(d->bg, 0, 0);

    AxlGfxPixel c = d->left  ? AXL_GFX_RGB(0x4a, 0xe6, 0x8a)   /* green */
                  : d->right ? AXL_GFX_RGB(0xff, 0x52, 0x4a)   /* red   */
                             : AXL_GFX_WHITE;

    /* Crosshair cursor at (x, y). */
    const int32_t arm = 16;
    axl_gfx_draw_line(d->x - arm, d->y, d->x + arm, d->y, c);
    axl_gfx_draw_line(d->x, d->y - arm, d->x, d->y + arm, c);
    axl_gfx_fill_rect((uint32_t)clampi(d->x - 2, d->w - 1),
                      (uint32_t)clampi(d->y - 2, d->h - 1), 5, 5, c);

    /* HUD line near the top. */
    const AxlFont *font = axl_gfx_default_font();
    char buf[128];
    axl_snprintf(buf, sizeof(buf),
                 "cursor (%d, %d)   buttons: %s%s   source: %s   abs/rel: %u/%u",
                 (int)d->x, (int)d->y,
                 d->left  ? "L" : "-",
                 d->right ? "R" : "-",
                 d->absolute ? "absolute" : "relative",
                 (unsigned)d->abs_evts, (unsigned)d->rel_evts);
    axl_gfx_fill_rect(0, 0, (uint32_t)d->w, 28, AXL_GFX_RGB(0x10, 0x12, 0x20));
    axl_gfx_draw_text(font, 16, 6, buf, AXL_GFX_CYAN, 1);
}

static bool
on_pointer(const AxlInputEvent *ev, void *data)
{
    PointerDemo *d = (PointerDemo *)data;
    switch (ev->type) {
    /* Absolute pointer (touchscreen / VNC usb-tablet): coordinates are
       in [0, AXL_INPUT_ABS_RANGE); map onto the framebuffer.  Once seen,
       the absolute device owns cursor position. */
    case AXL_INPUT_TOUCH_DOWN:
        d->left = true;
        /* fallthrough */
    case AXL_INPUT_TOUCH_MOVE:
        d->absolute = true;
        d->x = clampi((int32_t)((int64_t)ev->x * d->w / AXL_INPUT_ABS_RANGE), d->w - 1);
        d->y = clampi((int32_t)((int64_t)ev->y * d->h / AXL_INPUT_ABS_RANGE), d->h - 1);
        d->abs_evts++;
        break;
    case AXL_INPUT_TOUCH_UP:
        d->left = false;
        d->abs_evts++;
        break;

    /* Relative mouse: buttons always apply; position only when no
       absolute device is driving (i.e. real hardware, not VNC). */
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
    case AXL_INPUT_MOUSE_BUTTON_UP:
    case AXL_INPUT_MOUSE_WHEEL:
    case AXL_INPUT_MOUSE_MOVE:
        d->left  = (ev->buttons & AXL_INPUT_BUTTON_LEFT)  != 0;
        d->right = (ev->buttons & AXL_INPUT_BUTTON_RIGHT) != 0;
        if (!d->absolute) {
            d->x = clampi(ev->x, d->w - 1);
            d->y = clampi(ev->y, d->h - 1);
        }
        d->rel_evts++;
        break;
    default:
        return AXL_SOURCE_CONTINUE;
    }

    if (d->x > d->max_x) {
        d->max_x = d->x;
    }
    if (d->y > d->max_y) {
        d->max_y = d->y;
    }
    /* No per-event console output: in graphics mode the UEFI console is
       painted onto the GOP framebuffer, so printing each event would
       scribble over the cursor.  A summary is emitted after exit. */
    redraw(d);
    return AXL_SOURCE_CONTINUE;
}

static bool
on_key(AxlInputKey key, void *data)
{
    (void)key;
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!axl_gfx_available()) {
        axl_printf("No graphics output available (headless system).\n");
        return 0;
    }

    PointerDemo d = {0};
    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    d.w = (int32_t)info.width;
    d.h = (int32_t)info.height;
    d.x = d.w / 2;
    d.y = d.h / 2;

    /* Build the static background once. */
    d.bg = axl_gfx_buffer_new((uint32_t)d.w, (uint32_t)d.h);
    if (d.bg == NULL) {
        axl_printf("pointer-demo: back-buffer allocation failed.\n");
        return 1;
    }
    axl_gfx_target_buffer(d.bg);
    AxlGfxGradient *bg = axl_gfx_gradient_linear_new(0, 0, 0, d.h);
    axl_gfx_gradient_add_stop(bg, 0.0f, AXL_GFX_RGB(0x1b, 0x1e, 0x34));
    axl_gfx_gradient_add_stop(bg, 1.0f, AXL_GFX_RGB(0x09, 0x0a, 0x14));
    axl_gfx_fill_rect_gradient(0, 0, d.w, d.h, bg);
    axl_gfx_gradient_free(bg);
    const AxlFont *font = axl_gfx_default_font();
    axl_gfx_draw_text(font, 16, d.h - 28,
                      "Move the pointer -- cursor follows it.  "
                      "Left=green, right=red.  Press any key to exit.",
                      AXL_GFX_RGB(0x76, 0x82, 0xa4), 1);
    axl_gfx_target_buffer(NULL);

    AxlLoop *loop = axl_loop_new();
    AxlSourceId mid = axl_input_attach_mouse(loop, on_pointer, &d);
    AxlSourceId tid = axl_input_attach_touch(loop, on_pointer, &d);
    axl_printf("pointer-demo: attach mouse %s, touch %s; %dx%d framebuffer.\n",
               mid ? "OK" : "skipped", tid ? "OK" : "skipped",
               (int)d.w, (int)d.h);
    axl_loop_add_key_press(loop, on_key, loop);

    redraw(&d);                 /* show the initial cursor */
    axl_loop_run(loop);

    /* Summary after exit (graphics is done, so console output is fine). */
    axl_printf("pointer-demo: %u abs + %u rel events; furthest (%d, %d) of "
               "%dx%d.\n",
               (unsigned)d.abs_evts, (unsigned)d.rel_evts,
               (int)d.max_x, (int)d.max_y, (int)d.w, (int)d.h);

    /* Tear down the caller-owned pointer sources (their WaitForInput
       events belong to the firmware) BEFORE freeing the loop. */
    if (mid != 0) {
        axl_loop_remove_source(loop, mid);
    }
    if (tid != 0) {
        axl_loop_remove_source(loop, tid);
    }
    axl_loop_free(loop);
    axl_gfx_buffer_free(d.bg);
    return 0;
}
