/** @file axl-test-cursor.c
    Unit tests for AxlCursor (software cursor compositor).

    The unit harness is -nographic (no GOP), so the on-screen compositing
    can't be inspected here — that's the GOP integration test's job, and
    the spike validated it live. What IS deterministic without a screen:

      - the Option-C invariant that the cursor NEVER modifies the bound
        scene (it only reads it; the sprite is composited elsewhere and
        presented). A trail/corruption bug would show as a changed scene.
      - position tracking + clamp-to-scene.
      - show/hide/lift/drop/set_image/attach state and NULL-safety.
**/

#include "axl-test.h"
#include <axl/axl-cursor.h>
#include <axl/axl-gfx.h>
#include <axl/axl-input.h>

/* The attach trampoline, exposed (no public header) so we can drive its
   relative-pointer seed/delta tracking with synthetic events — the unit
   harness has no input device to exercise it through axl_cursor_attach. */
extern bool
cursor_input_trampoline(const AxlInputEvent *ev, void *data);

#define SW 100
#define SH 60

/* RGB equality (ignores alpha — blends force alpha to 0xFF). */
static bool
rgb_eq(AxlGfxPixel a, AxlGfxPixel b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static void
test_cursor_scene_unchanged(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    test_check(scene != NULL, "cursor: scene buffer created");
    /* Deterministic scene content. */
    axl_gfx_buffer_clear(scene, AXL_GFX_RGB(0x33, 0x66, 0x99));

    /* Snapshot the scene pixels. */
    AxlGfxPixel *px = axl_gfx_buffer_pixels(scene);
    AxlGfxPixel  snap[SW * SH];
    for (int i = 0; i < SW * SH; i++) {
        snap[i] = px[i];
    }

    AxlCursor *c = axl_cursor_new(scene);
    test_check(c != NULL, "cursor: created on a scene");
    test_check(!axl_cursor_visible(c), "cursor: starts hidden");

    axl_cursor_show(c);
    test_check(axl_cursor_visible(c), "cursor: visible after show");

    /* Move all around, including the corners (where the sprite clips). */
    axl_cursor_move(c, 50, 30);
    axl_cursor_move(c, 0, 0);
    axl_cursor_move(c, SW - 1, SH - 1);
    axl_cursor_move(c, 10, 5);
    axl_cursor_lift(c);
    axl_cursor_drop(c);
    axl_cursor_hide(c);

    /* The cursor must never have written into the scene. */
    bool same = true;
    for (int i = 0; i < SW * SH; i++) {
        if (px[i].blue != snap[i].blue || px[i].green != snap[i].green
            || px[i].red != snap[i].red || px[i].alpha != snap[i].alpha) {
            same = false;
            break;
        }
    }
    test_check(same, "cursor: never modifies the bound scene (Option C)");

    axl_cursor_free(c);
    axl_gfx_buffer_free(scene);
}

/* The flicker fix: axl_cursor_lift folds the sprite INTO the bound scene (so
   the compositor's one damage flush carries the cursor — no separate erase/
   redraw to the GOP), and axl_cursor_drop unfolds it byte-exact. This is the
   only on-screen-compositing behavior the -nographic harness CAN observe,
   because the fold lands in the bound back-buffer, not the GOP. */
static void
test_cursor_fold_atomic(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    test_check(scene != NULL, "fold: scene buffer created");
    axl_gfx_buffer_clear(scene, AXL_GFX_RGB(0x33, 0x66, 0x99));

    AxlGfxPixel *px = axl_gfx_buffer_pixels(scene);
    AxlGfxPixel  snap[SW * SH];
    for (int i = 0; i < SW * SH; i++) {
        snap[i] = px[i];
    }

    AxlCursor *c = axl_cursor_new(scene);
    test_check(c != NULL, "fold: cursor created");
    axl_cursor_show(c);
    const int32_t CX = 40, CY = 25;       /* hotspot = arrow top-left pixel */
    axl_cursor_move(c, CX, CY);

    /* A move keeps the scene byte-clean (Option C): it folds, presents, and
       unfolds. So before lift the hotspot pixel is still the scene color. */
    test_check(rgb_eq(px[CY * SW + CX], AXL_GFX_RGB(0x33, 0x66, 0x99)),
               "fold: scene clean after a move (fold is transient)");

    /* lift folds the sprite into the scene: the hotspot pixel becomes the
       arrow outline color (opaque 0x101010 over the scene). */
    axl_cursor_lift(c);
    test_check(rgb_eq(px[CY * SW + CX], AXL_GFX_RGB(0x10, 0x10, 0x10)),
               "fold: lift folds the arrow into the scene (atomic top layer)");

    /* drop unfolds: the scene is restored byte-exact. */
    axl_cursor_drop(c);
    bool same = true;
    for (int i = 0; i < SW * SH; i++) {
        if (px[i].blue != snap[i].blue || px[i].green != snap[i].green
            || px[i].red != snap[i].red || px[i].alpha != snap[i].alpha) {
            same = false;
            break;
        }
    }
    test_check(same, "fold: drop unfolds, scene restored byte-exact");

    /* Regression: hide() called BETWEEN lift and drop must unfold first, so
       the scene is left clean (not folded-dirty). Without the unfold-in-erase
       guard the arrow would stay baked into the scene until drop — and on a
       real GOP, erase would have flushed that ghost to the screen. */
    axl_cursor_show(c);
    axl_cursor_move(c, CX, CY);
    axl_cursor_lift(c);                   /* fold */
    test_check(rgb_eq(px[CY * SW + CX], AXL_GFX_RGB(0x10, 0x10, 0x10)),
               "fold: lifted (folded) before the mid-bracket hide");
    axl_cursor_hide(c);                   /* erase must unfold the scene */
    test_check(rgb_eq(px[CY * SW + CX], AXL_GFX_RGB(0x33, 0x66, 0x99)),
               "fold: hide between lift/drop unfolds (scene clean, no ghost)");
    axl_cursor_drop(c);                   /* unfold now a no-op */
    same = true;
    for (int i = 0; i < SW * SH; i++) {
        if (px[i].blue != snap[i].blue || px[i].green != snap[i].green
            || px[i].red != snap[i].red || px[i].alpha != snap[i].alpha) {
            same = false;
            break;
        }
    }
    test_check(same, "fold: scene byte-exact after mid-bracket hide + drop");

    axl_cursor_free(c);
    axl_gfx_buffer_free(scene);
}

static void
test_cursor_position_clamp(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    AxlCursor    *c     = axl_cursor_new(scene);
    int32_t x = -1, y = -1;

    axl_cursor_move(c, 40, 25);
    axl_cursor_position(c, &x, &y);
    test_check(x == 40 && y == 25, "cursor: position tracks a move");

    axl_cursor_move(c, -10, -10);
    axl_cursor_position(c, &x, &y);
    test_check(x == 0 && y == 0, "cursor: position clamps to (0,0)");

    axl_cursor_move(c, 9999, 9999);
    axl_cursor_position(c, &x, &y);
    test_check(x == SW - 1 && y == SH - 1, "cursor: position clamps to scene max");

    axl_cursor_free(c);
    axl_gfx_buffer_free(scene);
}

static void
test_cursor_set_image(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    AxlCursor    *c     = axl_cursor_new(scene);

    /* Restore the built-in arrow. */
    test_check(axl_cursor_set_image(c, NULL, 0, 0) == AXL_OK,
               "cursor: set_image(NULL) restores built-in arrow");

    /* A custom RGBA sprite. */
    AxlGfxBuffer *spr = axl_gfx_buffer_new(8, 8);
    axl_gfx_buffer_clear(spr, AXL_GFX_RGBA(0xff, 0x00, 0x00, 0xff));
    test_check(axl_cursor_set_image(c, spr, 4, 4) == AXL_OK,
               "cursor: set_image(custom 8x8, hotspot 4,4) returns AXL_OK");
    axl_gfx_buffer_free(spr);

    test_check(axl_cursor_set_image(NULL, NULL, 0, 0) == AXL_ERR,
               "cursor: set_image on NULL cursor returns AXL_ERR");

    axl_cursor_free(c);
    axl_gfx_buffer_free(scene);
}

static void
test_cursor_move_rel(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    AxlCursor *c = axl_cursor_new(scene);
    test_check(c != NULL, "move_rel: cursor created");
    axl_cursor_show(c);

    int32_t x = -1, y = -1;
    axl_cursor_move(c, 50, 30);
    axl_cursor_position(c, &x, &y);
    test_check(x == 50 && y == 30, "move_rel: start at (50,30)");

    /* A delta that overshoots the left edge clamps to 0. */
    axl_cursor_move_rel(c, -80, 0);
    axl_cursor_position(c, &x, &y);
    test_check(x == 0 && y == 30, "move_rel: overshoot left clamps to x=0");

    /* THE FIX: from the clamped edge, a small reverse delta moves
       immediately — the cursor is NOT stuck (the bug was clamping a raw
       accumulator, which would still read x=0 here). */
    axl_cursor_move_rel(c, 10, 0);
    axl_cursor_position(c, &x, &y);
    test_check(x == 10 && y == 30, "move_rel: recovers from the edge at once (no stuck)");

    /* Positive overshoot clamps to the far edge. */
    axl_cursor_move_rel(c, 0, 1000);
    axl_cursor_position(c, &x, &y);
    test_check(y == SH - 1, "move_rel: overshoot down clamps to bottom");
    axl_cursor_move_rel(c, 1000, 0);
    axl_cursor_position(c, &x, &y);
    test_check(x == SW - 1, "move_rel: overshoot right clamps to right edge");

    axl_cursor_move_rel(NULL, 1, 1);   /* NULL-safe no-op */

    axl_cursor_free(c);
    axl_gfx_buffer_free(scene);
}

/* Drive the attach trampoline with synthetic events to lock in the
   relative-pointer fix: the event carries the device's raw accumulated
   position, so the cursor must move by the per-event delta and recover
   from an edge even while that accumulator stays off-screen. */
static void
test_cursor_attach_tracking(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    AxlCursor *c = axl_cursor_new(scene);
    test_check(c != NULL, "track: cursor created");
    int32_t x = -1, y = -1;

    AxlInputEvent ev = {0};
    ev.type = AXL_INPUT_MOUSE_MOVE;

    /* First event seeds the delta reference; position must not jump. */
    ev.x = 50; ev.y = 30;
    cursor_input_trampoline(&ev, c);
    axl_cursor_position(c, &x, &y);
    test_check(x == 0 && y == 0, "track: first event seeds, no jump");

    /* Accumulator drifts far negative (remote pointer off-screen-left);
       the cursor clamps to the left edge. */
    ev.x = -200; ev.y = 30;
    cursor_input_trampoline(&ev, c);
    axl_cursor_position(c, &x, &y);
    test_check(x == 0, "track: negative drift clamps to the left edge");

    /* THE FIX: accumulator still negative (-190) but moved right by 10.
       The old code fed the raw -190 to move() -> stayed clamped at 0
       (stuck); moving by the +10 delta lands at x=10. */
    ev.x = -190; ev.y = 30;
    cursor_input_trampoline(&ev, c);
    axl_cursor_position(c, &x, &y);
    test_check(x == 10, "track: recovers from edge despite negative accumulator");

    axl_cursor_free(c);
    axl_gfx_buffer_free(scene);
}

/* Drive the trampoline with synthetic ABSOLUTE (touch) events to lock in
   the Task-2 contract: axl_cursor_attach now binds the absolute pointer too,
   and the trampoline maps an absolute coordinate ([0, AXL_INPUT_ABS_RANGE))
   straight onto the scene extent — no relative seed/accumulate. Once an
   absolute event is seen it is authoritative: later relative motion no longer
   moves the cursor. */
static void
test_cursor_attach_absolute(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(SW, SH);
    AxlCursor *c = axl_cursor_new(scene);
    test_check(c != NULL, "abs: cursor created");
    int32_t x = -1, y = -1;

    AxlInputEvent ev = {0};

    /* Absolute origin maps to the scene origin. */
    ev.type = AXL_INPUT_TOUCH_MOVE;
    ev.x = 0; ev.y = 0;
    cursor_input_trampoline(&ev, c);
    axl_cursor_position(c, &x, &y);
    test_check(x == 0 && y == 0, "abs: origin maps to (0,0)");
    test_check(axl_cursor_visible(c), "abs: touch shows the cursor");

    /* Absolute max maps to the far scene corner (scene_w-1, scene_h-1). */
    ev.x = AXL_INPUT_ABS_RANGE - 1; ev.y = AXL_INPUT_ABS_RANGE - 1;
    cursor_input_trampoline(&ev, c);
    axl_cursor_position(c, &x, &y);
    test_check(x == SW - 1 && y == SH - 1, "abs: max maps to scene far corner");

    /* Mid-range maps proportionally: 32768/65535 * 99 = 49, * 59 = 29. */
    ev.x = AXL_INPUT_ABS_RANGE / 2; ev.y = AXL_INPUT_ABS_RANGE / 2;
    cursor_input_trampoline(&ev, c);
    axl_cursor_position(c, &x, &y);
    test_check(x == 49 && y == 29, "abs: mid maps proportionally onto the scene");

    /* THE CONTRACT: absolute is now authoritative. A relative mouse move with
       a large delta must NOT move the cursor away from the absolute position. */
    ev.type = AXL_INPUT_MOUSE_MOVE;
    ev.x = 0; ev.y = 0;            /* first relative event would seed... */
    cursor_input_trampoline(&ev, c);
    ev.x = 1000; ev.y = 1000;     /* ...and this delta would jump it, if relative drove */
    cursor_input_trampoline(&ev, c);
    axl_cursor_position(c, &x, &y);
    test_check(x == 49 && y == 29, "abs: relative motion ignored once absolute seen");

    axl_cursor_free(c);
    axl_gfx_buffer_free(scene);
}

static void
test_cursor_null_safety(void)
{
    /* NULL scene selects direct-to-screen save-under mode, which needs a
       live framebuffer. It returns a working cursor when one is present
       and NULL when not; the actual on-screen capture/restore behavior is
       covered by the GOP integration test (cursor-selftest.c). Adapt to
       whichever the harness provides (one assertion either way, so the
       cross-arch test count is stable). */
    AxlCursor *sb = axl_cursor_new(NULL);
    if (axl_gfx_available()) {
        test_check(sb != NULL,
                   "cursor: new(NULL scene) creates a save-under cursor with a framebuffer");
        /* Must be safe to drive even where capture/present degrade. */
        axl_cursor_show(sb);
        axl_cursor_move(sb, 10, 10);
        axl_cursor_hide(sb);
        axl_cursor_free(sb);
    } else {
        test_check(sb == NULL,
                   "cursor: new(NULL scene) returns NULL without a framebuffer");
    }
    test_check(!axl_cursor_visible(NULL), "cursor: visible(NULL) is false");

    /* These must all be safe no-ops. */
    axl_cursor_free(NULL);
    axl_cursor_show(NULL);
    axl_cursor_hide(NULL);
    axl_cursor_move(NULL, 1, 1);
    axl_cursor_lift(NULL);
    axl_cursor_drop(NULL);
    axl_cursor_position(NULL, NULL, NULL);
    axl_cursor_detach(NULL, NULL);
    test_check(axl_cursor_attach(NULL, NULL, NULL, NULL) == 0,
               "cursor: attach(NULL) returns 0");
}

/* C++ RAII autoptr — AXL_AUTOPTR(AxlCursor) must free the cursor at scope
   exit. The scene buffer is borrowed (must outlive the cursor), so only the
   cursor is scope-managed here. Live-allocation count proves the free ran. */
static void
test_autoptr_cursor(void)
{
    AxlGfxBuffer *scene = axl_gfx_buffer_new(64, 32);
    test_check(scene != NULL, "autoptr: cursor scene buffer");

    axl_cursor_free(axl_cursor_new(scene));   /* prime one-time init */

    AxlMemStats before, after;
    axl_mem_get_stats(&before);
    {
        AXL_AUTOPTR(AxlCursor) c = axl_cursor_new(scene);
        test_check(c != NULL, "autoptr: cursor new");
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count, "autoptr: cursor freed at scope exit");

    axl_gfx_buffer_free(scene);
}

int
test_cursor_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlCursor");

    test_cursor_scene_unchanged();
    test_cursor_fold_atomic();
    test_cursor_position_clamp();
    test_cursor_set_image();
    test_cursor_move_rel();
    test_cursor_attach_tracking();
    test_cursor_attach_absolute();
    test_cursor_null_safety();
    test_autoptr_cursor();

    return test_print_results();
}

AXL_APP(test_cursor_main)
