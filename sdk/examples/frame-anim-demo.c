/*
 * frame-anim-demo.c -- the E7 frame-clock animation recipe.
 *
 * The pattern a toolkit (AGT) should use to animate. Instead of each
 * animated widget owning its own axl_loop_add_timer -- which presents
 * independently, never coalesces, and leaves a timer armed -- ONE frame
 * callback per window surface drives a per-window animation COORDINATOR:
 *
 *   1. axl_compositor_attach_frame_clock() once, at startup.
 *   2. axl_surface_request_frame(window, on_frame, coordinator) to start.
 *   3. on_frame() ticks EVERY active animation by the elapsed time, redraws
 *      the window once, presents once, and re-requests a frame ONLY while
 *      something is still animating.
 *   4. When the last animation stops, on_frame stops re-requesting -> the
 *      compositor's self-cancelling frame clock goes idle (no busy timer).
 *
 * Widgets paint into the one window buffer (they are not separate surfaces),
 * so a single per-window callback servicing all of them is the natural shape
 * -- and the per-surface one-shot slot fits exactly. Animations are
 * TIME-BASED (each gets the frame timestamp), so different rates coexist and
 * cadence jitter never corrupts motion.
 *
 * Here two independent animations share the one callback: an infinite spinner
 * and a progress bar that fills once and COMPLETES. The progress bar ending
 * while the spinner runs shows per-animation lifecycle inside one coordinator;
 * press any key to stop the spinner too and watch the clock idle.
 *
 * View it (mouse not needed):
 *   ./scripts/run-qemu.sh --gpu frame-anim-demo.efi
 *   ./scripts/run-qemu.sh --vnc-reverse HOST:PORT frame-anim-demo.efi
 *
 * Build with: make frame-anim-demo
 */

#include <axl.h>
#include <axl/axl-compositor.h>

#define SPIN_STEPS   12      /* dots in the spinner ring */
#define SPIN_STEP_MS 80      /* advance one dot every 80 ms (~12.5 fps look) */
#define PROG_STEP_MS 30      /* progress advances 1% every 30 ms (~3 s total) */

/* --- the animation model: a tick() that advances by elapsed time and
   reports whether it is still animating. This is the reusable shape a
   real toolkit's animation driver generalizes (a list of these). --- */
typedef struct Anim Anim;
typedef bool (*AnimTick)(Anim *a, uint64_t dt_ms);   /* true = still active */

struct Anim {
    AnimTick  tick;
    bool      active;
    uint32_t  accum_ms;     /* elapsed-time accumulator, for rate-gating */
    uint32_t  interval_ms;  /* this animation's own step interval */
    int       state;        /* phase (spinner) / percent (progress) */
};

/* The per-window coordinator: holds the one frame callback and every active
   animation. A toolkit attaches one of these per AgtWindow. */
typedef struct {
    AxlCompositor *comp;
    AxlSurface    *win;
    AxlLoop       *loop;
    int32_t        w, h;
    Anim           spinner;
    Anim           progress;
    uint64_t       last_ms;
    bool           seeded;
} AnimDemo;

/* Spinner: advance one dot per interval; runs until stopped externally. */
static bool
spinner_tick(Anim *a, uint64_t dt_ms)
{
    a->accum_ms += (uint32_t)dt_ms;
    while (a->accum_ms >= a->interval_ms) {
        a->accum_ms -= a->interval_ms;
        a->state = (a->state + 1) % SPIN_STEPS;
    }
    return true;
}

/* Progress: fill to 100% then COMPLETE (deactivate -> stops re-requesting). */
static bool
progress_tick(Anim *a, uint64_t dt_ms)
{
    a->accum_ms += (uint32_t)dt_ms;
    while (a->accum_ms >= a->interval_ms && a->state < 100) {
        a->accum_ms -= a->interval_ms;
        a->state++;
    }
    if (a->state >= 100) {
        a->active = false;   /* finished: the coordinator drops it */
        return false;
    }
    return true;
}

/* Paint both animations into the window buffer and mark it damaged. One
   redraw + one present services every animation this frame. */
static void
redraw(AnimDemo *d)
{
    const AxlGfxPixel BG = AXL_GFX_RGB(0x10, 0x12, 0x1e);
    AxlGfxBuffer *buf = axl_surface_buffer(d->win);
    axl_gfx_target_buffer(buf);
    axl_gfx_fill_rect(0, 0, (uint32_t)d->w, (uint32_t)d->h, BG);

    /* Spinner: a ring of dots, brightest at the current phase, trailing off. */
    int cx = d->w / 2, cy = d->h / 2 - 40, ring = 60, dot = 12;
    for (int i = 0; i < SPIN_STEPS; i++) {
        double ang = 2.0 * 3.14159265358979 * (double)i / (double)SPIN_STEPS;
        int x = cx + (int)(ring * axl_cos(ang)) - dot / 2;
        int y = cy + (int)(ring * axl_sin(ang)) - dot / 2;
        int behind = (d->spinner.state - i + SPIN_STEPS) % SPIN_STEPS;
        uint8_t lvl = (uint8_t)(230 - behind * (190 / SPIN_STEPS));
        axl_gfx_fill_rect(x, y, (uint32_t)dot, (uint32_t)dot,
                          AXL_GFX_RGB(lvl, lvl, lvl));
    }

    /* Progress bar below the spinner. */
    int bw = 320, bh = 22, bx = cx - bw / 2, by = cy + 130;
    axl_gfx_fill_rect(bx, by, (uint32_t)bw, (uint32_t)bh,
                      AXL_GFX_RGB(0x22, 0x26, 0x34));
    axl_gfx_fill_rect(bx, by, (uint32_t)(bw * d->progress.state / 100),
                      (uint32_t)bh, AXL_GFX_RGB(0x40, 0xc0, 0x80));

    axl_gfx_draw_text(axl_gfx_default_font(), cx - 150, by + 44,
                      "frame-clock animation -- press any key to stop",
                      AXL_GFX_RGB(0x76, 0x82, 0xa4), 1);
    axl_gfx_target_buffer(NULL);

    /* Damage the whole window so present recomposites + flushes it. */
    axl_surface_damage(d->win, (AxlGfxClip){0, 0, (uint32_t)d->w, (uint32_t)d->h});
}

/* THE RECIPE: the one per-window frame callback. Ticks every active
   animation by the elapsed time, redraws + presents once, and re-requests a
   frame only while something is still animating. */
static void
on_frame(void *user, uint64_t time_ms)
{
    AnimDemo *d = user;
    uint64_t dt = d->seeded ? time_ms - d->last_ms : 0;
    d->last_ms = time_ms;
    d->seeded  = true;

    bool any = false;
    if (d->spinner.active  && d->spinner.tick(&d->spinner, dt))   { any = true; }
    if (d->progress.active && d->progress.tick(&d->progress, dt)) { any = true; }

    redraw(d);
    axl_compositor_present(d->comp);     /* ONE present for all animations */

    if (any) {
        axl_surface_request_frame(d->win, on_frame, d);   /* keep the clock ticking */
    }
    /* else: stop re-requesting -> the frame clock self-cancels (idle). */
}

/* A key press stops every animation (so the next frame finds nothing active
   and the clock self-cancels) and quits the loop. */
static bool
on_key(AxlInputKey key, void *data)
{
    (void)key;
    AnimDemo *d = data;
    d->spinner.active  = false;
    d->progress.active = false;
    axl_loop_quit(d->loop);
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!axl_gfx_available()) {
        axl_printf("frame-anim-demo: no graphics output (headless). "
                   "Run under --gpu.\n");
        return 0;
    }

    AxlGfxInfo info;
    axl_gfx_get_info(&info);

    AnimDemo d = {0};
    d.w = (int32_t)info.width;
    d.h = (int32_t)info.height;
    d.spinner  = (Anim){spinner_tick,  true, 0, SPIN_STEP_MS, 0};
    d.progress = (Anim){progress_tick, true, 0, PROG_STEP_MS, 0};

    d.comp = axl_compositor_new((uint32_t)d.w, (uint32_t)d.h);
    if (d.comp == NULL) {
        axl_printf("frame-anim-demo: compositor alloc failed.\n");
        return 1;
    }
    /* One full-screen opaque window surface; widgets would paint into it. */
    d.win = axl_surface_new(axl_compositor_root(d.comp),
                               (uint32_t)d.w, (uint32_t)d.h);
    if (d.win == NULL) {
        axl_printf("frame-anim-demo: surface alloc failed.\n");
        axl_compositor_free(d.comp);
        return 1;
    }
    axl_surface_set_opaque(d.win, true);

    AxlLoop *loop = axl_loop_new();
    d.loop = loop;

    /* 1. Attach the frame clock once (~60 fps). */
    axl_compositor_attach_frame_clock(d.comp, loop, 16);
    /* A key press stops the animations and quits. */
    axl_loop_add_key_press(loop, on_key, &d);

    /* 2. Kick off: request the first frame. The clock drives on_frame from
          here; on_frame re-requests while anything animates. */
    axl_surface_request_frame(d.win, on_frame, &d);

    axl_printf("frame-anim-demo: animating on a %dx%d framebuffer; "
               "press any key to stop.\n", (int)d.w, (int)d.h);
    axl_loop_run(loop);

    /* Detach the frame clock before freeing the loop (no tick into freed
       state), then tear down. */
    axl_compositor_detach_frame_clock(d.comp, loop);
    axl_loop_free(loop);
    axl_compositor_free(d.comp);   /* frees the surface tree */
    return 0;
}
