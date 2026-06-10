/*
 * pointer-tune-demo.c -- LIVE absolute-pointer tuning over a GOP framebuffer.
 *
 * The remote-console "catch-up lag" tuning bench.  Drag a pointer into the
 * window and the crosshair follows it; on firmware (or QEMU's usb-tablet over
 * VNC) that queues absolute-pointer states FIFO, a fast drag trails and the
 * cursor slowly walks to where the pointer already is.  This demo lets you
 * change EVERY absolute-read lever at runtime with a keypress and watch the
 * lag appear and vanish, so you can find what a given platform handles best:
 *
 *   d  drain N        cycle 1 / 2 / 4 / 8 / 16   (axl_input_set_touch_drain)
 *                     N=1 reproduces the lag; higher coalesces the FIFO
 *                     backlog to the latest position and kills it.
 *   m  read method    EVENT_AND_POLL / EVENT_ONLY / POLL_ONLY
 *                     (axl_input_set_touch_config method).  EVENT_ONLY looks
 *                     dead where the firmware's WaitForInput never signals
 *                     -- that is itself a finding, not a bug.
 *   p  poll interval  cycle 10 / 20 / 30 / 50 ms  (poll_ms).  Reading a real
 *                     BMC pointer too fast can stall/flicker it, so slower can
 *                     be better -- drain, not poll rate, is the lever for lag.
 *   c  ConsoleIn-only bind ONLY gST->ConsoleInHandle (skip the separate
 *                     physical absolute handle).  Suspected best capture path.
 *   r  reset the event counters.       q  quit.
 *
 * Each d/m/p/c reconfigures live: detach the touch source, push the new
 * process-global config, re-attach -- the cycle the APIs require, since
 * axl_input_set_touch_* take effect only on the NEXT axl_input_attach_touch.
 * The relative mouse stays attached throughout (it contributes buttons).
 *
 * HUD: the live config, the rendered cursor position, abs/rel event counts,
 * and an events/sec gauge (recomputed each second).  A fading trail of recent
 * cursor positions makes the catch-up visible: under the lag it smears along
 * the drag path as the cursor walks the backlog; once coalesced it snaps.
 *
 * View over reverse VNC (run-qemu.sh adds usb-tablet = an absolute pointer):
 *   ./scripts/run-qemu.sh --vnc-reverse HOST:PORT pointer-tune-demo.efi
 *
 * Build with: make pointer-tune-demo
 */

#include <axl.h>

#define TRAIL_MAX 32

/* Tunable tables -- the cycle order each key steps through. */
static const uint32_t            DRAINS[]  = { 1, 2, 4, 8, 16 };
static const uint32_t            POLLS[]   = { 10, 20, 30, 50 };
static const AxlInputTouchMethod METHODS[] = {
    AXL_INPUT_TOUCH_EVENT_AND_POLL,
    AXL_INPUT_TOUCH_EVENT_ONLY,
    AXL_INPUT_TOUCH_POLL_ONLY,
};
static const char *const METHOD_NAMES[] = {
    "EVENT_AND_POLL", "EVENT_ONLY", "POLL_ONLY",
};

typedef struct {
    AxlGfxBuffer  *bg;          /* static background, re-presented each frame */
    AxlLoop       *loop;
    int32_t        w, h;
    int32_t        x, y;        /* clamped cursor position (pixels) */
    bool           left, right; /* button / contact state */
    bool           absolute;    /* an absolute device is positioning the cursor */
    uint32_t       abs_evts, rel_evts;
    uint32_t       eps;         /* events/sec (recomputed each tick) */
    uint32_t       last_total;  /* abs+rel at the last tick, for eps */

    /* Live absolute-read config (indices into the tables above). */
    uint32_t       drain_idx, method_idx, poll_idx;
    bool           console_only;
    uint32_t       tid;         /* current touch source id (0 = none) */

    /* Relative-pointer delta tracking: the mouse event carries the device's
       raw ACCUMULATED position (unbounded, and over a remote/VNC pointer it
       can drift far past an edge). Track by per-event delta — clamping the
       displayed position, not the accumulator — so the cursor stops at an
       edge yet recovers the instant motion reverses, instead of getting
       stuck off-screen. (Mirrors axl_cursor_move_rel; see AxlCursor.) */
    int32_t        rel_px, rel_py;  /* last mouse event's accumulated coords */
    bool           rel_seeded;      /* rel_px/py seeded yet? */

    /* Fading trail of recent cursor positions (ring). */
    int32_t        tx[TRAIL_MAX], ty[TRAIL_MAX];
    uint32_t       trail_head;  /* next write slot */
    uint32_t       trail_count; /* valid entries (<= TRAIL_MAX) */
} TuneDemo;

static int32_t
clampi(int32_t v, int32_t hi)
{
    return v < 0 ? 0 : (v > hi ? hi : v);
}

/* Record the current cursor position into the trail ring. */
static void
trail_push(TuneDemo *d)
{
    d->tx[d->trail_head] = d->x;
    d->ty[d->trail_head] = d->y;
    d->trail_head = (d->trail_head + 1) % TRAIL_MAX;
    if (d->trail_count < TRAIL_MAX) {
        d->trail_count++;
    }
}

/* Re-present the background, draw the fading trail, the crosshair, and the
   two-line HUD.  Full-frame redraw: simple and flicker-free (the background
   blit erases the previous frame). */
static void
redraw(TuneDemo *d)
{
    axl_gfx_target_buffer(NULL);
    axl_gfx_buffer_present(d->bg, 0, 0);

    /* Trail: oldest faint, newest bright.  The ring's logical order runs
       from (head - count) .. (head - 1); index 0 = oldest. */
    for (uint32_t i = 0; i < d->trail_count; i++) {
        uint32_t slot = (d->trail_head + TRAIL_MAX - d->trail_count + i) % TRAIL_MAX;
        uint32_t lum  = 0x30 + (0xa0 * i) / TRAIL_MAX;   /* 0x30 (old)..0xcb (new) */
        AxlGfxPixel dot = AXL_GFX_RGB((uint8_t)lum, (uint8_t)(lum / 2), 0x20);
        axl_gfx_fill_rect((uint32_t)clampi(d->tx[slot] - 1, d->w - 1),
                          (uint32_t)clampi(d->ty[slot] - 1, d->h - 1), 3, 3, dot);
    }

    AxlGfxPixel c = d->left  ? AXL_GFX_RGB(0x4a, 0xe6, 0x8a)
                  : d->right ? AXL_GFX_RGB(0xff, 0x52, 0x4a)
                             : AXL_GFX_WHITE;
    const int32_t arm = 16;
    axl_gfx_draw_line(d->x - arm, d->y, d->x + arm, d->y, c);
    axl_gfx_draw_line(d->x, d->y - arm, d->x, d->y + arm, c);
    axl_gfx_fill_rect((uint32_t)clampi(d->x - 2, d->w - 1),
                      (uint32_t)clampi(d->y - 2, d->h - 1), 5, 5, c);

    const AxlFont *font = axl_gfx_default_font();
    char buf[160];

    /* Line 1: the live config. */
    axl_gfx_fill_rect(0, 0, (uint32_t)d->w, 50, AXL_GFX_RGB(0x10, 0x12, 0x20));
    axl_snprintf(buf, sizeof(buf),
                 "[d]rain %-2u   [m]ethod %-14s   [p]oll %2u ms   [c]onsole-only %s",
                 (unsigned)DRAINS[d->drain_idx], METHOD_NAMES[d->method_idx],
                 (unsigned)POLLS[d->poll_idx], d->console_only ? "ON " : "off");
    axl_gfx_draw_text(font, 16, 6, buf, AXL_GFX_CYAN, 1);

    /* Line 2: live state. */
    axl_snprintf(buf, sizeof(buf),
                 "cursor (%4d,%4d)  %s%s  src:%s  abs/rel %u/%u  %u ev/s   "
                 "[r]eset  [q]uit",
                 (int)d->x, (int)d->y,
                 d->left ? "L" : "-", d->right ? "R" : "-",
                 d->absolute ? "abs" : "rel",
                 (unsigned)d->abs_evts, (unsigned)d->rel_evts, (unsigned)d->eps);
    axl_gfx_draw_text(font, 16, 28, buf, AXL_GFX_RGB(0x9a, 0xb0, 0xd8), 1);
}

static bool
on_pointer(const AxlInputEvent *ev, void *data)
{
    TuneDemo *d = (TuneDemo *)data;
    switch (ev->type) {
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

    case AXL_INPUT_MOUSE_BUTTON_DOWN:
    case AXL_INPUT_MOUSE_BUTTON_UP:
    case AXL_INPUT_MOUSE_WHEEL:
    case AXL_INPUT_MOUSE_MOVE:
        d->left  = (ev->buttons & AXL_INPUT_BUTTON_LEFT)  != 0;
        d->right = (ev->buttons & AXL_INPUT_BUTTON_RIGHT) != 0;
        /* Position only when no absolute device is driving. Move by the delta
           from the previous event (clamping the displayed position) so the
           cursor recovers from an edge instead of clamping a runaway
           accumulator. Seed the reference on the first event. */
        if (!d->absolute) {
            if (!d->rel_seeded) {
                d->rel_px = ev->x;
                d->rel_py = ev->y;
                d->rel_seeded = true;
            }
            d->x = clampi(d->x + (ev->x - d->rel_px), d->w - 1);
            d->y = clampi(d->y + (ev->y - d->rel_py), d->h - 1);
            d->rel_px = ev->x;
            d->rel_py = ev->y;
        }
        d->rel_evts++;
        break;
    default:
        return AXL_SOURCE_CONTINUE;
    }
    trail_push(d);
    redraw(d);
    return AXL_SOURCE_CONTINUE;
}

/* Reconfigure the absolute source live: detach, push the new process-global
   config, re-attach.  Resets the counters + trail so a before/after read is
   clean. */
static void
apply_touch(TuneDemo *d)
{
    if (d->tid != 0) {
        axl_input_detach_touch(d->loop);
        d->tid = 0;
    }
    axl_input_set_touch_config(METHODS[d->method_idx], d->console_only,
                               POLLS[d->poll_idx]);
    axl_input_set_touch_drain(DRAINS[d->drain_idx]);
    d->tid = axl_input_attach_touch(d->loop, on_pointer, d);

    d->abs_evts = d->rel_evts = d->eps = d->last_total = 0;
    d->trail_count = d->trail_head = 0;
    d->absolute = false;       /* let the next absolute event re-latch */
    redraw(d);
}

/* 1 Hz: refresh the events/sec gauge (so it updates even while idle). */
static bool
on_tick(void *data)
{
    TuneDemo *d = (TuneDemo *)data;
    uint32_t total = d->abs_evts + d->rel_evts;
    d->eps = total - d->last_total;
    d->last_total = total;
    redraw(d);
    return AXL_SOURCE_CONTINUE;
}

static bool
on_key(AxlInputKey key, void *data)
{
    TuneDemo *d = (TuneDemo *)data;
    uint16_t ch = key.unicode_char;
    if (ch >= 'A' && ch <= 'Z') {
        ch = (uint16_t)(ch + 0x20);   /* fold case */
    }
    switch (ch) {
    case 'd':
        d->drain_idx = (d->drain_idx + 1) % (sizeof(DRAINS) / sizeof(DRAINS[0]));
        apply_touch(d);
        break;
    case 'm':
        d->method_idx = (d->method_idx + 1) % (sizeof(METHODS) / sizeof(METHODS[0]));
        apply_touch(d);
        break;
    case 'p':
        d->poll_idx = (d->poll_idx + 1) % (sizeof(POLLS) / sizeof(POLLS[0]));
        apply_touch(d);
        break;
    case 'c':
        d->console_only = !d->console_only;
        apply_touch(d);
        break;
    case 'r':
        d->abs_evts = d->rel_evts = d->eps = d->last_total = 0;
        d->trail_count = d->trail_head = 0;
        redraw(d);
        break;
    case 'q':
        axl_loop_quit(d->loop);
        return AXL_SOURCE_REMOVE;
    default:
        break;
    }
    return AXL_SOURCE_CONTINUE;
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!axl_gfx_available()) {
        axl_printf("No graphics output available (headless system).\n");
        return 0;
    }

    TuneDemo d = {0};
    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    d.w = (int32_t)info.width;
    d.h = (int32_t)info.height;
    d.x = d.w / 2;
    d.y = d.h / 2;
    d.method_idx = 0;   /* EVENT_AND_POLL */
    d.poll_idx   = 2;   /* 30 ms (the library default) */
    d.drain_idx  = 0;   /* 1 -- the lag, by design, so you SEE it first */

    d.bg = axl_gfx_buffer_new((uint32_t)d.w, (uint32_t)d.h);
    if (d.bg == NULL) {
        axl_printf("pointer-tune-demo: back-buffer allocation failed.\n");
        return 1;
    }
    axl_gfx_target_buffer(d.bg);
    AxlGfxGradient *bg = axl_gfx_gradient_linear_new(0, 0, 0, d.h);
    axl_gfx_gradient_add_stop(bg, 0.0f, AXL_GFX_RGB(0x1b, 0x1e, 0x34));
    axl_gfx_gradient_add_stop(bg, 1.0f, AXL_GFX_RGB(0x09, 0x0a, 0x14));
    axl_gfx_fill_rect_gradient(0, 0, d.w, d.h, bg);
    axl_gfx_gradient_free(bg);
    axl_gfx_draw_text(axl_gfx_default_font(), 16, d.h - 28,
                      "Drag the pointer.  Tune the absolute-read levers with "
                      "the keys shown -- watch the lag appear and vanish.",
                      AXL_GFX_RGB(0x76, 0x82, 0xa4), 1);
    axl_gfx_target_buffer(NULL);

    d.loop = axl_loop_new();

    /* Relative mouse stays attached for the whole run (contributes buttons /
       drives position on bare metal with no absolute device). */
    uint32_t mid = axl_input_attach_mouse(d.loop, on_pointer, &d);

    /* Bring up the absolute source at the initial config. */
    apply_touch(&d);

    axl_loop_add_timer(d.loop, 1000, on_tick, &d);
    axl_loop_add_key_press(d.loop, on_key, &d);

    axl_printf("pointer-tune-demo: mouse %s, touch %s; %dx%d.  "
               "Keys: d/m/p/c tune, r reset, q quit.\n",
               mid ? "OK" : "skipped", d.tid ? "OK" : "skipped",
               (int)d.w, (int)d.h);

    redraw(&d);
    axl_loop_run(d.loop);

    axl_printf("pointer-tune-demo: final {drain %u, method %s, poll %u ms, "
               "console_only %s}; %u abs + %u rel events.\n",
               (unsigned)DRAINS[d.drain_idx], METHOD_NAMES[d.method_idx],
               (unsigned)POLLS[d.poll_idx], d.console_only ? "on" : "off",
               (unsigned)d.abs_evts, (unsigned)d.rel_evts);

    if (d.tid != 0) {
        axl_input_detach_touch(d.loop);
    }
    if (mid != 0) {
        axl_input_detach_mouse(d.loop);
    }
    axl_loop_free(d.loop);
    axl_gfx_buffer_free(d.bg);
    return 0;
}
