/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * sendkey-render-selftest.c — does the SCREENSHOT reflect the LAST key?
 *
 * input-keys-selftest.c answers "did the key arrive". That is not the same
 * question, and the difference is the whole point of this app: a consumer
 * reported that after --sendkey-after fixed the LEADING keys, the TRAILING
 * one still went missing under parallel load -- three shift-rights leaving
 * the cursor at offset 2 in the captured frame. Measured here, the guest
 * finishes RECEIVING the last key ~1.2-1.3 s before the shot under both
 * spinner load and 6-VM load. So if the effect is missing from the frame,
 * the gap is between "processed" and "painted", which only a RENDERING app
 * can expose.
 *
 * So: one red square per key, drawn straight to the screen. N keys must
 * leave exactly N squares in the capture. A capture taken mid-repaint shows
 * fewer, and the harness reports how many -- the number, not a pixel diff.
 *
 * Red is chosen because nothing else on the screen produces pure 0xFF0000:
 * the boot text and this app's own markers are greys and yellows, so the
 * count is unambiguous even with console text painted over the frame. That
 * also means this app does NOT need to route its markers away from ConOut
 * the way a pixel-diffing consumer does.
 *
 * Run via: scripts/run-qemu.sh --gpu ... --sendkey ... --screenshot x.ppm
 * Driven by: test/integration/test-sendkey-render-qemu.sh
 */

#include <axl.h>

#ifndef KEYS_EXPECTED
#define KEYS_EXPECTED 3
#endif

#ifndef WATCHDOG_MS
#define WATCHDOG_MS 180000
#endif

/* Deliberate stall before the final square. 0 = paint immediately. */
#ifndef SLOW_PAINT_MS
#define SLOW_PAINT_MS 0
#endif

/* Square geometry. Y is well below the text console so boot output cannot
   overlap the squares; X strides so they never touch each other. */
#define SQ_W     32
#define SQ_H     32
#define SQ_Y     300
#define SQ_X0    40
#define SQ_STRIDE 48

typedef struct {
    AxlLoop *loop;
    int      count;
} RenderState;

static bool
on_key(const AxlInputEvent *ev, void *data)
{
    RenderState *st = (RenderState *)data;

    if (ev->type != AXL_INPUT_KEY_DOWN) {
        return AXL_SOURCE_CONTINUE;
    }

    /* SLOW_PAINT_MS stalls before the FINAL square only.
       Why synthetic: this app's real repaint is a single fill_rect, i.e.
       microseconds, so it can never reproduce a repaint-LATENCY bug no
       matter how loaded the host is. A consumer whose repaint runs a
       syntax scanner over a style buffer is a different animal. Rather
       than pretend otherwise, the latency is injected deliberately and
       labelled -- that makes this a test of the CAPTURE GATE (does the
       harness wait for the paint?), not a reproduction of any particular
       consumer's timing. */
    if (SLOW_PAINT_MS > 0 && st->count + 1 == KEYS_EXPECTED) {
        uint64_t until = axl_time_get_ms() + (uint64_t)SLOW_PAINT_MS;
        while (axl_time_get_ms() < until) {
            /* spin: a sleep could be coalesced by the loop */
        }
    }

    /* Draw straight to the screen: target NULL is the framebuffer. */
    axl_gfx_target_buffer(NULL);
    axl_gfx_fill_rect((uint32_t)(SQ_X0 + st->count * SQ_STRIDE), SQ_Y,
                      SQ_W, SQ_H, AXL_GFX_RGB(0xFF, 0x00, 0x00));

    st->count++;
    axl_printf("RENDER-KEY %d\n", st->count);

    if (st->count >= KEYS_EXPECTED) {
        /* Printed only after the final square is on the framebuffer, so a
           harness can gate the capture on it. */
        axl_printf("RENDER-DONE\n");
    }
    return AXL_SOURCE_CONTINUE;
}

static bool
on_watchdog(void *data)
{
    RenderState *st = (RenderState *)data;
    axl_printf("RENDER-TIMEOUT (saw %d keys)\n", st->count);
    axl_loop_quit(st->loop);
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AxlGfxInfo info;
    if (axl_gfx_get_info(&info) != AXL_OK) {
        axl_printf("RENDER-NOGFX\n");
        return 1;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        axl_printf("RENDER-NOLOOP\n");
        return 1;
    }

    RenderState st = { .loop = loop, .count = 0 };

    if (axl_input_attach_key(loop, on_key, &st) == 0) {
        axl_printf("RENDER-NOINPUT\n");
        axl_loop_free(loop);
        return 1;
    }

    axl_loop_add_timeout(loop, WATCHDOG_MS, on_watchdog, &st);

    /* Deliberately AFTER input is attached: this is what --sendkey-after
       gates on, and a marker printed before ConIn is bound would reinstate
       the very race that flag exists to remove. */
    axl_printf("RENDER-READY\n");
    axl_loop_run(loop);

    axl_loop_free(loop);
    return 0;
}
