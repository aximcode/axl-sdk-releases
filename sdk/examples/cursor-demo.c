/*
 * cursor-demo.c -- live software-cursor demo over a GOP framebuffer.
 *
 * The AxlCursor-consumer counterpart to pointer-demo.c.  Where
 * pointer-demo hand-rolls a crosshair with a full-frame redraw per event,
 * this builds a scene back-buffer, hands it to AxlCursor, and lets the
 * cursor composite itself: axl_cursor_attach tracks the physical pointer,
 * showing the built-in arrow and moving it on motion -- touching only the
 * cursor region, never the whole frame.  The consumer callback here does
 * nothing but repaint a small HUD on button changes, demonstrating the
 * lift / re-present / drop bracket the cursor documents.
 *
 * The loop HOLDS until a key is pressed, so you can move the pointer and
 * watch the arrow track it.  Use a boot-class pointer (QEMU
 * `-device usb-mouse`, not `usb-tablet`) so OVMF binds EFI_SIMPLE_POINTER.
 *
 * View it with the run-qemu.sh windowed modes (mouse-enabled OVMF):
 *   ./scripts/run-qemu.sh --vnc-reverse HOST:PORT cursor-demo.efi
 *
 * Build with: make cursor-demo
 */

#include <axl.h>

typedef struct {
    AxlGfxBuffer  *scene;   /* back-buffer the cursor is bound to */
    AxlCursor     *cur;
    int32_t        w, h;
    uint32_t       buttons; /* last-seen button state, for the HUD */
} CursorDemo;

/* Repaint the HUD strip into the scene, bracketed by lift/drop so the
   cursor is restored over the freshly-presented pixels. */
static void
hud(CursorDemo *d)
{
    /* Only L/R: EFI_SIMPLE_POINTER reports just LeftButton/RightButton,
       so the middle button is never observable through the mouse path. */
    char buf[96];
    axl_snprintf(buf, sizeof(buf), "buttons: %s%s   (press any key to exit)",
                 (d->buttons & AXL_INPUT_BUTTON_LEFT)  ? "L" : "-",
                 (d->buttons & AXL_INPUT_BUTTON_RIGHT) ? "R" : "-");

    axl_gfx_target_buffer(d->scene);
    axl_gfx_fill_rect(0, 0, (uint32_t)d->w, 28, AXL_GFX_RGB(0x10, 0x12, 0x20));
    axl_gfx_draw_text(axl_gfx_default_font(), 16, 6, buf, AXL_GFX_CYAN, 1);
    axl_gfx_target_buffer(NULL);

    axl_cursor_lift(d->cur);
    axl_gfx_buffer_present_rect(d->scene, 0, 0, 0, 0, (uint32_t)d->w, 28);
    axl_cursor_drop(d->cur);
}

/* AxlCursor has already moved/shown the arrow before forwarding here; we
   only react to button-state changes by refreshing the HUD. */
static bool
on_input(const AxlInputEvent *ev, void *data)
{
    CursorDemo *d = (CursorDemo *)data;
    if (ev->buttons != d->buttons) {
        d->buttons = ev->buttons;
        hud(d);
    }
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

    CursorDemo d = {0};
    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    d.w = (int32_t)info.width;
    d.h = (int32_t)info.height;

    /* Build and present the scene back-buffer once. */
    d.scene = axl_gfx_buffer_new((uint32_t)d.w, (uint32_t)d.h);
    if (d.scene == NULL) {
        axl_printf("cursor-demo: back-buffer allocation failed.\n");
        return 1;
    }
    axl_gfx_target_buffer(d.scene);
    AxlGfxGradient *bg = axl_gfx_gradient_linear_new(0, 0, 0, d.h);
    axl_gfx_gradient_add_stop(bg, 0.0f, AXL_GFX_RGB(0x1b, 0x1e, 0x34));
    axl_gfx_gradient_add_stop(bg, 1.0f, AXL_GFX_RGB(0x09, 0x0a, 0x14));
    axl_gfx_fill_rect_gradient(0, 0, d.w, d.h, bg);
    axl_gfx_gradient_free(bg);
    axl_gfx_draw_text(axl_gfx_default_font(), 16, d.h - 28,
                      "Move the pointer -- the arrow follows it.",
                      AXL_GFX_RGB(0x76, 0x82, 0xa4), 1);
    axl_gfx_target_buffer(NULL);
    axl_gfx_buffer_present(d.scene, 0, 0);

    /* Bind the cursor to the scene and let it track the pointer. */
    d.cur = axl_cursor_new(d.scene);
    if (d.cur == NULL) {
        axl_printf("cursor-demo: cursor creation failed.\n");
        axl_gfx_buffer_free(d.scene);
        return 1;
    }
    /* Park the arrow at center and show it immediately, so it's visible
       before the first pointer motion (a relative pointer otherwise leaves
       it at the (0,0) origin until you move). */
    axl_cursor_move(d.cur, d.w / 2, d.h / 2);
    axl_cursor_show(d.cur);

    AxlLoop *loop = axl_loop_new();
    AxlSourceId mid = axl_cursor_attach(d.cur, loop, on_input, &d);
    axl_printf("cursor-demo: attach mouse %s; %dx%d framebuffer.\n",
               mid ? "OK" : "skipped", (int)d.w, (int)d.h);
    axl_loop_add_key_press(loop, on_key, loop);

    hud(&d);                    /* show the initial HUD */
    axl_loop_run(loop);

    /* Tear down the cursor's pointer source before freeing the loop. */
    axl_cursor_detach(d.cur, loop);
    axl_loop_free(loop);
    axl_cursor_free(d.cur);
    axl_gfx_buffer_free(d.scene);
    return 0;
}
