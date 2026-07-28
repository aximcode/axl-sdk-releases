/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * gfx-present-bench.c — quantify the CPU cost of rendering a full-screen frame,
 * to confirm/refute the claim that logic-only test runs waste CPU by
 * incidentally rendering (because OVMF exposes a GOP on x64).
 *
 * Headless (no GOP, e.g. run-qemu --no-gpu): prints a skip line and exits fast
 * — this IS the fix for a logic-only run: nothing to render, no cost.
 *
 * With a GOP: builds a representative frame the way a modal-dialog UI does —
 * clear the backdrop, blend a full-screen translucent veil, backdrop-blur it,
 * then PRESENT to the framebuffer — in a loop, and reports per-phase µs/frame
 * plus a raw-memcpy floor (the theoretical minimum to move a full frame to the
 * framebuffer). That separates the UNAVOIDABLE cost (present ~= memcpy the
 * pixels out) from the render work, and shows the absolute per-frame cost a
 * logic test pays for free when it renders a frame nobody is watching.
 *
 * It then runs the same frame through the COMPOSITOR — a full-screen frosted
 * veil over a backdrop, with a caret-sized change under it each frame (a modal
 * dialog with a blinking cursor). That is the shape E10's partial re-blur
 * targets: the veil is unchanged, so only the damage plus its blur halo needs
 * re-frosting, not the whole screen.
 *
 * Markers for the harness: "BENCH-HEADLESS", "BENCH-FRAME", "BENCH-CLEAR",
 * "BENCH-BLEND", "BENCH-BLUR", "BENCH-PRESENT", "BENCH-MEMCPY",
 * "BENCH-COMP-FULL", "BENCH-COMP-CARET".
 */

#include <axl.h>
#include <axl/axl-compositor.h>

#define ITERS 60

static uint64_t
now_us(void)
{
    return axl_time_get_us();
}

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!axl_gfx_available()) {
        axl_printf("BENCH-HEADLESS: no GOP — render skipped (0 cost)\n");
        return 0;
    }

    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    uint32_t W = info.width, H = info.height;
    axl_printf("BENCH: display %ux%u fb=0x%llx\n", W, H,
               (unsigned long long)info.framebuffer);

    AxlGfxBuffer *fb = axl_gfx_buffer_new(W, H);
    if (fb == NULL) {
        axl_printf("BENCH: alloc failed\n");
        return 1;
    }
    AxlGfxPixel *px = axl_gfx_buffer_pixels(fb);
    size_t nbytes = (size_t)W * H * sizeof(AxlGfxPixel);

    /* Raw-memcpy floor: the least work to move a full frame's pixels. */
    void *scratch = axl_malloc(nbytes);
    uint64_t mc = 0;
    if (scratch != NULL) {
        uint64_t t0 = now_us();
        for (int i = 0; i < ITERS; i++) {
            axl_memcpy(scratch, px, nbytes);
        }
        mc = (now_us() - t0) / ITERS;
        axl_free(scratch);
    }

    const AxlGfxPixel BG   = AXL_GFX_RGB(0x20, 0x28, 0x40);
    const AxlGfxPixel VEIL = AXL_GFX_RGBA(0x00, 0x00, 0x00, 0x80);   /* 50% black */

    /* Per-phase timing over ITERS frames. build = clear + full-screen veil
       blend + backdrop blur (the frame a modal dialog composites); present =
       push it to the framebuffer. */
    uint64_t clr = 0, blend = 0, blur = 0, present = 0;
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_us();
        axl_gfx_buffer_clear(fb, BG);
        uint64_t t1 = now_us();
        axl_gfx_target_buffer(fb);
        axl_gfx_fill_rect(0, 0, W, H, VEIL);   /* alpha<255 -> blend path */
        axl_gfx_target_buffer(NULL);
        uint64_t t2 = now_us();
        axl_gfx_buffer_blur(fb, 12);           /* backdrop blur (O(w*h)) */
        uint64_t t3 = now_us();
        axl_gfx_buffer_present(fb, 0, 0);
        uint64_t t4 = now_us();
        clr     += (t1 - t0);
        blend   += (t2 - t1);
        blur    += (t3 - t2);
        present += (t4 - t3);
    }
    axl_gfx_buffer_free(fb);

    uint64_t c = clr / ITERS, bl = blend / ITERS, bu = blur / ITERS,
             p = present / ITERS, frame = c + bl + bu + p;
    axl_printf("BENCH-FRAME:   %llu us/frame\n", (unsigned long long)frame);
    axl_printf("BENCH-CLEAR:   %llu us\n", (unsigned long long)c);
    axl_printf("BENCH-BLEND:   %llu us (full-screen veil)\n", (unsigned long long)bl);
    axl_printf("BENCH-BLUR:    %llu us (radius 12)\n", (unsigned long long)bu);
    axl_printf("BENCH-PRESENT: %llu us\n", (unsigned long long)p);
    axl_printf("BENCH-MEMCPY:  %llu us (raw-copy floor for %ux%u)\n",
               (unsigned long long)mc, W, H);

    /* --- the compositor path: a frosted veil with a caret blinking under it -- */
    AxlCompositor *comp = axl_compositor_new(W, H);
    if (comp == NULL) {
        axl_printf("BENCH: compositor alloc failed\n");
        return 1;
    }
    AxlSurface *bd   = axl_surface_new(axl_compositor_root(comp), W, H);
    AxlSurface *veil = axl_surface_new(axl_compositor_root(comp), W, H);
    if (bd == NULL || veil == NULL) {
        axl_printf("BENCH: surface alloc failed\n");
        axl_compositor_free(comp);
        return 1;
    }
    axl_gfx_target_buffer(axl_surface_buffer(bd));
    axl_gfx_fill_rect(0, 0, W, H, BG);
    for (uint32_t x = 0; x + 40 <= W; x += 80) {
        axl_gfx_fill_rect((int32_t)x, 0, 40, H, AXL_GFX_RGB(0x40, 0x50, 0x80));
    }
    axl_gfx_target_buffer(NULL);
    AxlGfxPixel *vp = axl_gfx_buffer_pixels(axl_surface_buffer(veil));
    if (vp == NULL) {
        axl_printf("BENCH: veil buffer unavailable\n");
        axl_compositor_free(comp);
        return 1;
    }
    for (size_t i = 0; i < (size_t)W * H; i++) {
        vp[i] = AXL_GFX_RGBA(0x00, 0x00, 0x00, 0x80);   /* 50% black frosted dim */
    }
    axl_surface_set_per_pixel_alpha(veil, true);
    axl_surface_set_backdrop_blur(veil, 12);

    uint64_t t0 = now_us();
    axl_compositor_present(comp);          /* first frame: the whole screen */
    uint64_t comp_full = now_us() - t0;

    /* A 12x18 caret toggling under the veil, one present per frame. */
    uint64_t caret = 0;
    for (int i = 0; i < ITERS; i++) {
        axl_gfx_target_buffer(axl_surface_buffer(bd));
        axl_gfx_fill_rect((int32_t)(W / 2), (int32_t)(H / 2), 12, 18,
                          (i & 1) ? BG : AXL_GFX_RGB(0xF0, 0xF0, 0xF0));
        axl_gfx_target_buffer(NULL);
        axl_surface_damage(bd, (AxlGfxClip){(int32_t)(W / 2), (int32_t)(H / 2), 12, 18});
        uint64_t s0 = now_us();
        axl_compositor_present(comp);
        caret += now_us() - s0;
    }
    axl_compositor_free(comp);
    axl_printf("BENCH-COMP-FULL:  %llu us (first frame, whole screen)\n",
               (unsigned long long)comp_full);
    axl_printf("BENCH-COMP-CARET: %llu us/frame (12x18 damage under a full-screen veil)\n",
               (unsigned long long)(caret / ITERS));
    return 0;
}
