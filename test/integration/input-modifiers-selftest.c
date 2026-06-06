/**
 * input-modifiers-selftest.c — Prove that a HELD keyboard modifier is
 * stamped onto pointer events (Ctrl+click, Shift+wheel) through the real
 * OVMF firmware path, not just the axl unit mocks.
 *
 * Background: UEFI delivers modifier state only WITH a keystroke. To make
 * Shift+wheel / Ctrl+click work, axl-input enables EFI_KEY_STATE_EXPOSED
 * (so the firmware emits a modifier-only "partial keystroke" when a
 * modifier alone goes down/up) and stamps the live modifier state onto
 * every pointer event. The serial unit harness has no SimpleTextInputEx,
 * so this end-to-end path can only be exercised with a real UEFI keyboard
 * stack — which OVMF's UsbKbDxe / Ps2KeyboardDxe provide under QEMU.
 *
 * Sequence:
 *   1. axl_input_attach_key  — enables EFI_KEY_STATE_EXPOSED and tracks
 *      the live modifier state (partial keystrokes update it silently).
 *   2. axl_input_attach_mouse — relative pointer (EFI_SIMPLE_POINTER).
 *      Returns 0 when the firmware publishes no pointer (e.g. aa64 AAVMF
 *      under QEMU); the app prints "PTR-UNAVAILABLE" so the harness can
 *      record a balanced SKIP instead of failing.
 *   3. Print "PTR-READY" once both sources are up.
 *   4. on_ptr prints one line per button / wheel event:
 *        "PTR BTN-DOWN b=0x%08x m=0x%08x"
 *        "PTR BTN-UP   b=0x%08x m=0x%08x"
 *        "PTR WHEEL    dy=%d m=0x%08x"
 *      The harness injects (via QMP input-send-event) a held Ctrl + left
 *      click and a held Shift + wheel, then asserts the EXACT modifier
 *      bits on the BTN-DOWN / WHEEL lines.
 *   5. A watchdog quits the app so an undelivered event can never hang the
 *      harness; the harness itself ends the run once it sees its lines.
 *
 * Run via test/integration/test-input-modifiers-qemu.sh.
 */

#include <axl.h>

/* Safety cap: stop after this many pointer events even if the harness
   never tears the run down (it normally does, once its lines appear). */
#define PTR_EVENTS_MAX 64

typedef struct {
    AxlLoop *loop;
    int      count;
} PtrState;

static bool
on_key(const AxlInputEvent *ev, void *data)
{
    /* Required so attach_key enables EFI_KEY_STATE_EXPOSED and tracks the
       live modifier state. The held modifiers under test arrive as partial
       keystrokes, which axl-input consumes internally (updating the live
       state) and never delivers here — so this callback does nothing. */
    (void)ev;
    (void)data;
    return AXL_SOURCE_CONTINUE;
}

static bool
on_ptr(const AxlInputEvent *ev, void *data)
{
    PtrState *st = (PtrState *)data;

    switch (ev->type) {
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
        axl_printf("PTR BTN-DOWN b=0x%08x m=0x%08x\n", ev->buttons, ev->modifiers);
        break;
    case AXL_INPUT_MOUSE_BUTTON_UP:
        axl_printf("PTR BTN-UP   b=0x%08x m=0x%08x\n", ev->buttons, ev->modifiers);
        break;
    case AXL_INPUT_MOUSE_WHEEL:
        axl_printf("PTR WHEEL    dy=%d m=0x%08x\n", ev->wheel_dy, ev->modifiers);
        break;
    default:
        /* Ignore motion — it is noisy and not under test. */
        return AXL_SOURCE_CONTINUE;
    }

    if (++st->count >= PTR_EVENTS_MAX) {
        axl_printf("INPUT-DONE\n");
        axl_loop_quit(st->loop);
    }
    return AXL_SOURCE_CONTINUE;
}

static bool
on_watchdog(void *data)
{
    PtrState *st = (PtrState *)data;
    axl_printf("INPUT-TIMEOUT (saw %d pointer events)\n", st->count);
    axl_loop_quit(st->loop);
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        axl_printf("input-modifiers: axl_loop_new failed\n");
        return 1;
    }

    PtrState st = { .loop = loop, .count = 0 };

    /* Keyboard first: this enables EFI_KEY_STATE_EXPOSED so the firmware
       emits modifier-only partial keystrokes that update the live state. */
    if (axl_input_attach_key(loop, on_key, &st) == 0) {
        axl_printf("input-modifiers: axl_input_attach_key failed\n");
        axl_loop_free(loop);
        return 1;
    }

    if (axl_input_attach_mouse(loop, on_ptr, &st) == 0) {
        /* No physical pointer (relative EFI_SIMPLE_POINTER). The harness
           treats this as a SKIP — the modifier-stamping logic itself is
           unit-tested cross-arch; this integration test validates the
           real OVMF firmware path, which needs a pointer device. */
        axl_printf("PTR-UNAVAILABLE\n");
        axl_loop_free(loop);
        return 0;
    }

    /* Watchdog: never let the harness hang on an event the firmware never
       delivers. Generous for slow TCG boots. */
    axl_loop_add_timeout(loop, 30000, on_watchdog, &st);

    axl_printf("PTR-READY\n");
    axl_loop_run(loop);

    axl_loop_free(loop);
    return 0;
}
