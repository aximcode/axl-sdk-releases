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
 *   d  drain N        cycle 1 / 2 / 4 / 8 / 16 / 32 / 64 / 128
 *                     (axl_input_set_touch_drain).  N=1 reproduces the lag;
 *                     higher coalesces the FIFO backlog to the latest position
 *                     and kills it.  The drain stops at every button
 *                     transition, so the "clk d/u" counter must keep pace with
 *                     your physical clicks even at high N -- a press/release is
 *                     never coalesced away.  (Watch the counter: if d/u ever
 *                     falls behind your clicks, that is a dropped click.)
 *   m  read method    EVENT_AND_POLL / EVENT_ONLY / POLL_ONLY
 *                     (axl_input_set_touch_config method).  EVENT_ONLY looks
 *                     dead where the firmware's WaitForInput never signals
 *                     -- that is itself a finding, not a bug.
 *   p  poll interval  cycle 10 / 20 / 30 / 50 ms  (poll_ms).  Reading a real
 *                     BMC pointer too fast can stall/flicker it, so slower can
 *                     be better -- drain, not poll rate, is the lever for lag.
 *   c  ConsoleIn-only bind ONLY gST->ConsoleInHandle (skip the separate
 *                     physical absolute handle).  Suspected best capture path.
 *   s  relative scale cycle derived / /1 / /8 / /64 / /512 / /4096 / /8192 /
 *                     /65536.  "derived" divides the relative count delta by
 *                     the device's reported Mode->Resolution (spec-correct
 *                     default); the fixed divisors calibrate a device whose
 *                     reported resolution doesn't track 1:1 (a KVM/iDRAC/VNC
 *                     mouse reports large counts, and axl-input passes them
 *                     through 1 count = 1 pixel, so the cursor overshoots).
 *                     The remainder is accumulated so slow motion isn't lost.
 *   r  reset the event counters.       q  quit.
 *
 * Each d/m/p/c reconfigures live: detach the touch source, push the new
 * process-global config, re-attach -- the cycle the APIs require, since
 * axl_input_set_touch_* take effect only on the NEXT axl_input_attach_touch.
 * The relative mouse stays attached throughout (it contributes buttons).
 *
 * HUD: the live config, the rendered cursor position, abs/rel event counts,
 * a TOUCH down/up click tally ("clk d/u"), and an events/sec gauge (recomputed
 * each second).  The click tally is the drain-safety read: it must track your
 * physical clicks at any drain.  A fading trail of recent
 * cursor positions makes the catch-up visible: under the lag it smears along
 * the drag path as the cursor walks the backlog; once coalesced it snaps.
 *
 * Click-snap (hardware bring-up): a click polls the absolute pointer directly
 * and snaps the cursor to its reported position -- a per-click re-sync to
 * ground truth when a relative-driven cursor has drifted off the host pointer.
 * The "snap" HUD counter increments when a click found a readable absolute
 * device; if it stays 0 while you click, the firmware exposes no absolute
 * pointer to snap to (so relative tracking is all there is on that platform).
 *
 * On startup the demo prints axl_input_probe_pointers() to the serial console
 * BEFORE taking the framebuffer: a full enumeration of every SIMPLE / ABSOLUTE
 * pointer handle with a live GetState, so you can see exactly what a platform
 * publishes. Capture the serial log to diagnose a cursor that won't track.
 *
 * View over reverse VNC (run-qemu.sh adds usb-tablet = an absolute pointer):
 *   ./scripts/run-qemu.sh --vnc-reverse HOST:PORT pointer-tune-demo.efi
 *
 * Build with: make pointer-tune-demo
 */

#include <axl.h>
/* Diagnostic bench only: reaches raw EFI to poll the absolute pointer on a
   click (snap_to_absolute below). A relative click carries no absolute
   coordinate, so this is the only way to re-sync the cursor to the host
   position on a relative-driven pointer -- IF the firmware also publishes an
   absolute pointer we can read. If it proves useful on hardware, promote it to
   a tested axl_input_* one-shot-read API rather than leaving EFI in an
   example. */
#include <uefi/axl-uefi.h>

#define TRAIL_MAX 32

/* Tunable tables -- the cycle order each key steps through. */
static const uint32_t            DRAINS[]  = { 1, 2, 4, 8, 16, 32, 64, 128 };
static const uint32_t            POLLS[]   = { 10, 20, 30, 50 };
static const AxlInputTouchMethod METHODS[] = {
    AXL_INPUT_TOUCH_EVENT_AND_POLL,
    AXL_INPUT_TOUCH_EVENT_ONLY,
    AXL_INPUT_TOUCH_POLL_ONLY,
};
static const char *const METHOD_NAMES[] = {
    "EVENT_AND_POLL", "EVENT_ONLY", "POLL_ONLY",
};
/* Relative-movement scale divisor. Index 0 = "derived" (divide by the device's
   reported Mode->ResolutionX/Y) -- the spec-correct default. The rest are fixed
   divisors for calibrating a device whose reported resolution doesn't yield 1:1
   tracking (Avocent/iDRAC/VNC report large relative counts; axl-input treats
   1 count = 1 pixel, so an un-scaled cursor flies off). 0 in the table is the
   derived sentinel. */
static const uint32_t     SCALE_DIVS[]  = { 0, 1, 8, 64, 512, 4096, 8192, 65536 };
static const char *const  SCALE_NAMES[] = { "derived", "/1", "/8", "/64",
                                            "/512", "/4096", "/8192", "/65536" };

typedef struct {
    AxlGfxBuffer  *bg;          /* static background, re-presented each frame */
    AxlLoop       *loop;
    int32_t        w, h;
    int32_t        x, y;        /* clamped cursor position (pixels) */
    bool           left, right; /* button / contact state */
    bool           absolute;    /* an absolute device is positioning the cursor */
    uint32_t       abs_evts, rel_evts;
    uint32_t       touch_downs, touch_ups;  /* TOUCH_DOWN / _UP tally (drain-safety read) */
    uint32_t       mouse_downs, mouse_ups;  /* relative MOUSE_BUTTON down/up tally */
    uint32_t       snaps;       /* clicks that found a readable absolute pointer to snap to */

    /* Relative-movement scaling (the lever for high-count devices). */
    uint32_t       scale_idx;   /* index into SCALE_DIVS; 0 = derived from device res */
    int32_t        dev_res_x, dev_res_y;  /* physical relative device Mode->Resolution (0 = unknown) */
    int32_t        rel_acc_x, rel_acc_y;  /* sub-divisor remainder, so slow motion isn't lost */
    uint32_t       eps;         /* events/sec (recomputed each tick) */
    uint32_t       last_total;  /* abs+rel at the last tick, for eps */

    /* Live absolute-read config (indices into the tables above). */
    uint32_t       drain_idx, method_idx, poll_idx;
    bool           console_only;
    AxlSourceId    tid;         /* current touch source id (0 = none) */

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

/* Effective relative-movement divisor for the current scale setting. In
   "derived" mode (index 0) use the device's reported resolution; clamp to >=1
   so a 0/unknown resolution degrades to 1:1 rather than dividing by zero. */
static uint32_t
scale_x(const TuneDemo *d)
{
    if (d->scale_idx == 0) {
        return d->dev_res_x > 0 ? (uint32_t)d->dev_res_x : 1u;
    }
    return SCALE_DIVS[d->scale_idx];
}

static uint32_t
scale_y(const TuneDemo *d)
{
    if (d->scale_idx == 0) {
        return d->dev_res_y > 0 ? (uint32_t)d->dev_res_y : 1u;
    }
    return SCALE_DIVS[d->scale_idx];
}

/* Read the physical relative pointer's Mode->Resolution for "derived" scaling.
   Prefers a non-ConsoleIn handle (the real device, not the aggregator); falls
   back to ConsoleIn. Reaches raw EFI -- diagnostic bench, see the include
   note. Leaves dev_res 0 (=> 1:1) if nothing reports a resolution. */
static void
read_rel_resolution(TuneDemo *d)
{
    d->dev_res_x = d->dev_res_y = 0;

    EFI_GUID    guid    = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    if (gBS->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles) != 0
        || handles == NULL) {
        return;
    }

    /* Mode is `void *` in the generated header (spec-typo workaround); cast to
       the real type, as src/input/axl-input.c does. */
    EFI_HANDLE con_in = (gST != NULL) ? gST->ConsoleInHandle : NULL;
    EFI_SIMPLE_POINTER_MODE *con_mode = NULL;
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp = NULL;
        if (gBS->HandleProtocol(handles[i], &guid, (void **)&sp) != 0
            || sp == NULL || sp->Mode == NULL) {
            continue;
        }
        EFI_SIMPLE_POINTER_MODE *m = (EFI_SIMPLE_POINTER_MODE *)sp->Mode;
        if (handles[i] == con_in) {
            con_mode = m;           /* remember the aggregator as a fallback */
            continue;
        }
        d->dev_res_x = (int32_t)m->ResolutionX;   /* physical device wins */
        d->dev_res_y = (int32_t)m->ResolutionY;
        break;
    }
    if (d->dev_res_x == 0 && con_mode != NULL) {
        d->dev_res_x = (int32_t)con_mode->ResolutionX;
        d->dev_res_y = (int32_t)con_mode->ResolutionY;
    }

    gBS->FreePool(handles);
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

    /* Three lines now (width is tight on a real console): config, scale +
       cursor, counts. */
    axl_gfx_fill_rect(0, 0, (uint32_t)d->w, 72, AXL_GFX_RGB(0x10, 0x12, 0x20));

    /* Line 1: read-config levers. */
    axl_snprintf(buf, sizeof(buf),
                 "[d]rain %-3u  [m]ethod %-14s  [p]oll %2u ms  [c]onsole-only %s",
                 (unsigned)DRAINS[d->drain_idx], METHOD_NAMES[d->method_idx],
                 (unsigned)POLLS[d->poll_idx], d->console_only ? "ON " : "off");
    axl_gfx_draw_text(font, 16, 6, buf, AXL_GFX_CYAN, 1);

    /* Line 2: relative scale + cursor. */
    axl_snprintf(buf, sizeof(buf),
                 "[s]cale %-8s (dev res %d/%d)   cursor (%4d,%4d)  %s%s  src:%s",
                 SCALE_NAMES[d->scale_idx], (int)d->dev_res_x, (int)d->dev_res_y,
                 (int)d->x, (int)d->y,
                 d->left ? "L" : "-", d->right ? "R" : "-",
                 d->absolute ? "abs" : "rel");
    axl_gfx_draw_text(font, 16, 28, buf, AXL_GFX_RGB(0x9a, 0xb0, 0xd8), 1);

    /* Line 3: counters. */
    axl_snprintf(buf, sizeof(buf),
                 "abs/rel %u/%u  touch d/u %u/%u  mouse d/u %u/%u  snap %u  "
                 "%u ev/s   [r]eset [q]uit",
                 (unsigned)d->abs_evts, (unsigned)d->rel_evts,
                 (unsigned)d->touch_downs, (unsigned)d->touch_ups,
                 (unsigned)d->mouse_downs, (unsigned)d->mouse_ups,
                 (unsigned)d->snaps, (unsigned)d->eps);
    axl_gfx_draw_text(font, 16, 50, buf, AXL_GFX_RGB(0x9a, 0xb0, 0xd8), 1);
}

/* Click-snap hack: poll whatever absolute pointer the firmware publishes and
   snap the displayed cursor to its current position.  Reaches raw EFI on
   purpose (see the include note) -- a relative click gives no absolute coord,
   so a direct GetState is the only ground-truth fix.  Returns true if a device
   reported a position; false means none is readable (snap impossible here).
   Iterates handles rather than LocateProtocol so the dead ConsoleIn aggregator
   (no backing, GetState = NOT_READY) is skipped in favor of a real device. */
static bool
snap_to_absolute(TuneDemo *d)
{
    EFI_GUID    guid    = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;

    if (gBS->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles) != 0
        || handles == NULL) {
        return false;
    }

    bool snapped = false;
    for (UINTN i = 0; i < count && !snapped; i++) {
        EFI_ABSOLUTE_POINTER_PROTOCOL *ap = NULL;
        if (gBS->HandleProtocol(handles[i], &guid, (void **)&ap) != 0
            || ap == NULL || ap->Mode == NULL) {
            continue;
        }
        EFI_ABSOLUTE_POINTER_STATE s;
        if (ap->GetState(ap, &s) != 0) {
            continue;   /* no data on this device right now */
        }
        uint64_t lox = ap->Mode->AbsoluteMinX, hix = ap->Mode->AbsoluteMaxX;
        uint64_t loy = ap->Mode->AbsoluteMinY, hiy = ap->Mode->AbsoluteMaxY;
        if (hix <= lox || hiy <= loy) {
            continue;   /* degenerate range -- can't map */
        }
        d->x = (int32_t)(((s.CurrentX - lox) * (uint64_t)(d->w - 1)) / (hix - lox));
        d->y = (int32_t)(((s.CurrentY - loy) * (uint64_t)(d->h - 1)) / (hiy - loy));
        snapped = true;
    }

    gBS->FreePool(handles);
    return snapped;
}

static bool
on_pointer(const AxlInputEvent *ev, void *data)
{
    TuneDemo *d = (TuneDemo *)data;
    switch (ev->type) {
    case AXL_INPUT_TOUCH_DOWN:
        d->left = true;
        d->touch_downs++;   /* drain-safety read: must track physical clicks */
        /* fallthrough */
    case AXL_INPUT_TOUCH_MOVE:
        d->absolute = true;
        d->x = clampi((int32_t)((int64_t)ev->x * d->w / AXL_INPUT_ABS_RANGE), d->w - 1);
        d->y = clampi((int32_t)((int64_t)ev->y * d->h / AXL_INPUT_ABS_RANGE), d->h - 1);
        d->abs_evts++;
        break;
    case AXL_INPUT_TOUCH_UP:
        d->left = false;
        d->touch_ups++;
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
            /* Scale the raw count delta by the current divisor, accumulating
               the remainder so sub-divisor movement isn't lost over time.
               axl-input reports unscaled counts (1 count = 1 pixel); a device
               that reports large counts (Avocent/iDRAC/VNC) needs this or the
               cursor overshoots. */
            int32_t  raw_dx = ev->x - d->rel_px;
            int32_t  raw_dy = ev->y - d->rel_py;
            d->rel_px = ev->x;
            d->rel_py = ev->y;
            int32_t  dvx = (int32_t)scale_x(d), dvy = (int32_t)scale_y(d);
            d->rel_acc_x += raw_dx;
            d->rel_acc_y += raw_dy;
            int32_t  step_x = d->rel_acc_x / dvx;
            int32_t  step_y = d->rel_acc_y / dvy;
            d->rel_acc_x -= step_x * dvx;
            d->rel_acc_y -= step_y * dvy;
            d->x = clampi(d->x + step_x, d->w - 1);
            d->y = clampi(d->y + step_y, d->h - 1);
        }
        if (ev->type == AXL_INPUT_MOUSE_BUTTON_DOWN) {
            d->mouse_downs++;
        } else if (ev->type == AXL_INPUT_MOUSE_BUTTON_UP) {
            d->mouse_ups++;
        }
        /* Click-snap: a relative click carries no absolute coord, so poll the
           absolute pointer and snap to ground truth. The snap is the final
           word (overrides the scaled position above); relative deltas then
           continue from the snapped point. */
        if (ev->type == AXL_INPUT_MOUSE_BUTTON_DOWN && snap_to_absolute(d)) {
            d->snaps++;
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
    d->touch_downs = d->touch_ups = d->mouse_downs = d->mouse_ups = d->snaps = 0;
    d->rel_acc_x = d->rel_acc_y = 0;
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
    case 's':
        /* Cycle the relative-movement scale. No re-attach needed (scaling is
           applied in on_pointer, not in the touch config); reset the remainder
           so the change doesn't carry a stale partial step. */
        d->scale_idx = (d->scale_idx + 1) % (sizeof(SCALE_DIVS) / sizeof(SCALE_DIVS[0]));
        d->rel_acc_x = d->rel_acc_y = 0;
        redraw(d);
        break;
    case 'r':
        d->abs_evts = d->rel_evts = d->eps = d->last_total = 0;
    d->touch_downs = d->touch_ups = d->mouse_downs = d->mouse_ups = d->snaps = 0;
    d->rel_acc_x = d->rel_acc_y = 0;
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

    /* Enumerate every pointer device the firmware publishes (SIMPLE +
       ABSOLUTE, with a live GetState) to the serial console BEFORE the GUI
       takes the framebuffer.  Capture this to see what a platform actually
       exposes -- e.g. whether an Avocent/iDRAC console presents an absolute
       pointer at all, or only a relative mouse. */
    axl_input_probe_pointers(NULL);

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
    d.scale_idx  = 0;   /* derived (divide by the device's reported resolution) */

    /* Read the relative device's resolution for the "derived" scale mode. */
    read_rel_resolution(&d);
    axl_printf("pointer-tune-demo: relative device resolution X/Y = %d/%d "
               "(derived scale divides by this; press 's' to try fixed divisors).\n",
               (int)d.dev_res_x, (int)d.dev_res_y);

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
    AxlSourceId mid = axl_input_attach_mouse(d.loop, on_pointer, &d);

    /* Bring up the absolute source at the initial config. */
    apply_touch(&d);

    axl_loop_add_timer(d.loop, 1000, on_tick, &d);
    axl_loop_add_key_press(d.loop, on_key, &d);

    axl_printf("pointer-tune-demo: mouse %s, touch %s; %dx%d.  "
               "Keys: d/m/p/c/s tune, r reset, q quit.\n",
               mid ? "OK" : "skipped", d.tid ? "OK" : "skipped",
               (int)d.w, (int)d.h);

    redraw(&d);
    axl_loop_run(d.loop);

    axl_printf("pointer-tune-demo: final {drain %u, method %s, poll %u ms, "
               "console_only %s, scale %s (dev res %d/%d)}; %u abs + %u rel events; "
               "touch clicks d/u %u/%u; mouse clicks d/u %u/%u; click-snaps %u.\n",
               (unsigned)DRAINS[d.drain_idx], METHOD_NAMES[d.method_idx],
               (unsigned)POLLS[d.poll_idx], d.console_only ? "on" : "off",
               SCALE_NAMES[d.scale_idx], (int)d.dev_res_x, (int)d.dev_res_y,
               (unsigned)d.abs_evts, (unsigned)d.rel_evts,
               (unsigned)d.touch_downs, (unsigned)d.touch_ups,
               (unsigned)d.mouse_downs, (unsigned)d.mouse_ups, (unsigned)d.snaps);

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
