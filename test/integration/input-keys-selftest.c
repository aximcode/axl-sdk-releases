/**
 * input-keys-selftest.c — Print each keyboard event delivered through
 * the unified axl-input event source, so a QEMU `sendkey` harness can
 * verify the canonical Ctrl+letter encoding (folded C0 vs letter+MOD).
 *
 * Sequence:
 *   1. axl_loop_new + axl_input_attach_key(loop, on_key, loop).
 *   2. Print "INPUT-READY" so the harness knows ConIn is bound and it
 *      can start injecting `sendkey` events on the QEMU monitor.
 *   3. on_key prints one line per AXL_INPUT_KEY_DOWN:
 *        "KEY u=0x%04x sc=0x%04x m=0x%08x"
 *      (unicode codepoint / raw scan code / modifier bitfield).
 *   4. Quit after a fixed number of keys OR an overall watchdog timer,
 *      so an un-delivered key can never hang the harness.
 *
 * Run via test/integration/test-input-keys-qemu.sh.
 */

#include <axl.h>

/* Stop after this many key-down events, or when the watchdog fires —
   whichever comes first. The harness injects 5 keys: two via the QEMU
   monitor `sendkey` (physical-keyboard / SimpleTextInputEx path) and
   three raw bytes over the serial socket (TerminalDxe path), which
   together expose the divergent Ctrl+letter encodings. */
#define KEYS_EXPECTED 5

typedef struct {
    AxlLoop *loop;
    int      count;
} KeyState;

static bool
on_key(const AxlInputEvent *ev, void *data)
{
    KeyState *st = (KeyState *)data;

    if (ev->type != AXL_INPUT_KEY_DOWN) {
        return AXL_SOURCE_CONTINUE;
    }

    axl_printf("KEY u=0x%04x sc=0x%04x m=0x%08x\n",
               ev->unicode, ev->keycode, ev->modifiers);

    if (++st->count >= KEYS_EXPECTED) {
        axl_printf("INPUT-DONE\n");
        axl_loop_quit(st->loop);
    }
    return AXL_SOURCE_CONTINUE;
}

static bool
on_watchdog(void *data)
{
    KeyState *st = (KeyState *)data;
    axl_printf("INPUT-TIMEOUT (saw %d keys)\n", st->count);
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
        axl_printf("input-keys: axl_loop_new failed\n");
        return 1;
    }

    KeyState st = { .loop = loop, .count = 0 };

    if (axl_input_attach_key(loop, on_key, &st) == 0) {
        axl_printf("input-keys: axl_input_attach_key failed\n");
        axl_loop_free(loop);
        return 1;
    }

    /* Watchdog: never let the harness hang waiting on a key that the
       firmware never delivers. 30 s is generous for slow TCG boots. */
    axl_loop_add_timeout(loop, 30000, on_watchdog, &st);

    axl_printf("INPUT-READY\n");
    axl_loop_run(loop);

    axl_loop_free(loop);
    return 0;
}
