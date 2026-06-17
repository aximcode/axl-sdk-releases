/**
 * input-demo.c — Phase 0j integration demo.
 *
 * Exercises the full axl-input substrate: attaches mouse, keyboard,
 * and touch sources to an axl-loop, dispatches via a single unified
 * AxlInputCallback, and renders feedback via axl-gfx.
 *
 * Visual proof:
 *   - Title + prompts at known positions (static, regression check)
 *   - Status panel showing "Mouse / Key / Touch attached: yes/no"
 *   - Live cursor sprite that follows mouse motion (red dot)
 *   - Last-key indicator showing scan code + unicode of most recent
 *     key event
 *
 * Runs for 5 seconds, then exits.  In automated testing the
 * screenshot captures the static prompts; on hardware with real
 * input you see the cursor follow + key feedback live.
 *
 * Build with: make input-demo
 */

#include <axl.h>

extern const AxlFont axl_font_unifont_16;

typedef struct {
    AxlGfxInfo  info;
    int32_t     cursor_x;
    int32_t     cursor_y;
    uint32_t    last_key_scan;
    uint32_t    last_key_unicode;
    int32_t     touch_x;
    int32_t     touch_y;
    bool        touch_active;
    uint32_t    mouse_events;
    uint32_t    key_events;
    uint32_t    touch_events;
} DemoState;

static void
redraw_status(const DemoState *s, const AxlFont *font)
{
    /* Clear status strip across the bottom. */
    axl_gfx_fill_rect(0, s->info.height - 120, s->info.width, 120,
                      AXL_GFX_RGB(0x10, 0x10, 0x20));

    char buf[128];
    axl_snprintf(buf, sizeof(buf),
                 "Mouse: %u events, cursor (%d, %d)",
                 (unsigned)s->mouse_events, (int)s->cursor_x, (int)s->cursor_y);
    axl_gfx_draw_text(font, 20, s->info.height - 110, buf, AXL_GFX_CYAN, 1);

    axl_snprintf(buf, sizeof(buf),
                 "Key:   %u events, last scan=0x%04X unicode=0x%04X '%c'",
                 (unsigned)s->key_events,
                 (unsigned)s->last_key_scan,
                 (unsigned)s->last_key_unicode,
                 (s->last_key_unicode >= 0x20 && s->last_key_unicode < 0x7F)
                     ? (char)s->last_key_unicode : '.');
    axl_gfx_draw_text(font, 20, s->info.height - 90, buf, AXL_GFX_YELLOW, 1);

    axl_snprintf(buf, sizeof(buf),
                 "Touch: %u events, %s contact (%d, %d)",
                 (unsigned)s->touch_events,
                 s->touch_active ? "active" : "no",
                 (int)s->touch_x, (int)s->touch_y);
    axl_gfx_draw_text(font, 20, s->info.height - 70, buf, AXL_GFX_GREEN, 1);

    axl_gfx_draw_text(font, 20, s->info.height - 40,
                      "axl-input substrate demo (5s timeout)  --  AximCode Library",
                      AXL_GFX_WHITE, 1);
}

static bool
on_input(
    const AxlInputEvent  *ev,
    void                 *data
    )
{
    DemoState *s = (DemoState *)data;
    switch (ev->type) {
    /* All four mouse event kinds share the same status-panel update:
       refresh cursor position + bump the mouse-event counter. */
    case AXL_INPUT_MOUSE_MOVE:
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
    case AXL_INPUT_MOUSE_BUTTON_UP:
    case AXL_INPUT_MOUSE_WHEEL:
        s->cursor_x = ev->x;
        s->cursor_y = ev->y;
        s->mouse_events++;
        break;
    case AXL_INPUT_KEY_DOWN:
    case AXL_INPUT_KEY_UP:
        s->last_key_scan    = ev->keycode;
        s->last_key_unicode = ev->unicode;
        s->key_events++;
        break;
    case AXL_INPUT_TOUCH_DOWN:
        s->touch_active = true;
        s->touch_x = ev->x; s->touch_y = ev->y;
        s->touch_events++;
        break;
    case AXL_INPUT_TOUCH_UP:
        s->touch_active = false;
        s->touch_events++;
        break;
    case AXL_INPUT_TOUCH_MOVE:
        s->touch_x = ev->x; s->touch_y = ev->y;
        s->touch_events++;
        break;
    default:
        break;
    }
    redraw_status(s, &axl_font_unifont_16);
    return AXL_SOURCE_CONTINUE;
}

static bool
on_quit_timer(
    void  *data
    )
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    if (!axl_gfx_available()) {
        axl_printf("No graphics output available (headless system).\n");
        return 0;
    }

    DemoState s = {0};
    axl_gfx_get_info(&s.info);

    const AxlFont *font = axl_gfx_default_font();

    /* Initial scene */
    axl_gfx_fill_rect(0, 0, s.info.width, s.info.height,
                      AXL_GFX_RGB(0x1A, 0x1A, 0x2E));
    axl_gfx_draw_text(font, 20, 20,  "AXL Input Demo", AXL_GFX_WHITE, 3);
    axl_gfx_draw_text(font, 20, 80,
        "Substrate attachment test (mouse / key / touch -> axl-loop)",
        AXL_GFX_CYAN, 2);

    /* Create the loop and attach all three input sources.  Print
       success/failure to console so consumers can see what's wired. */
    AxlLoop *loop = axl_loop_new();
    AxlSourceId mid = axl_input_attach_mouse(loop, on_input, &s);
    AxlSourceId kid = axl_input_attach_key(loop, on_input, &s);
    AxlSourceId tid = axl_input_attach_touch(loop, on_input, &s);
    axl_printf("attach mouse: %s (id=%llu)\n", mid ? "OK" : "skipped", (unsigned long long)mid);
    axl_printf("attach key:   %s (id=%llu)\n", kid ? "OK" : "skipped", (unsigned long long)kid);
    axl_printf("attach touch: %s (id=%llu)\n", tid ? "OK" : "skipped", (unsigned long long)tid);

    axl_gfx_draw_text(font, 20, 140,
        mid ? "[X] mouse attached (move the mouse)" : "[ ] mouse not available",
        mid ? AXL_GFX_GREEN : AXL_GFX_GRAY, 1);
    axl_gfx_draw_text(font, 20, 160,
        kid ? "[X] keyboard attached (press any key)" : "[ ] keyboard not available",
        kid ? AXL_GFX_GREEN : AXL_GFX_GRAY, 1);
    axl_gfx_draw_text(font, 20, 180,
        tid ? "[X] touch attached (tap the screen)" : "[ ] touch not available",
        tid ? AXL_GFX_GREEN : AXL_GFX_GRAY, 1);

    /* Initial status panel render. */
    redraw_status(&s, &axl_font_unifont_16);

    /* Schedule auto-quit after 5 seconds and run the loop. */
    axl_loop_add_timeout(loop, 5000, on_quit_timer, loop);
    axl_loop_run(loop);

    /* Final stats to console. */
    axl_printf("Done. mouse=%u key=%u touch=%u events\n",
               (unsigned)s.mouse_events, (unsigned)s.key_events,
               (unsigned)s.touch_events);

    axl_loop_free(loop);
    return 0;
}
