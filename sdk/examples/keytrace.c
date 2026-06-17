/*
 * keytrace.c -- keyboard inter-arrival diagnostic for tuning the
 * key-debounce default (docs/AXL-Pointer-Cursor-Design.md, sec 3.4).
 *
 * Logs every KEY_DOWN with its character and the microseconds since the
 * previous KEY_DOWN, then prints a bucket histogram on exit.  Run it over
 * a REAL iDRAC session (where keystroke duplicates appear) and type
 * normally for a while: the histogram reveals whether the duplicates
 * cluster at the ~20 ms USB typematic rate (firmware repeat, latency-
 * induced) or elsewhere, which pins a `min_repeat_ms` window that
 * suppresses them without clipping genuine fast typing (same-key
 * digraphs land in the ~80-300 ms range).  Feed the chosen value to
 * axl_input_set_key_debounce.
 *
 * Keyboard only, no graphics: output goes to the console (serial / SOL /
 * iDRAC virtual console), so it works headless.  Press ESC to exit (also
 * auto-quits after 5 min).
 *
 * Build with: make keytrace
 */

#include <axl.h>

/* dt buckets (microseconds) chosen around the decision boundaries:
   < 10 ms        : implausibly fast -> almost certainly a duplicate
   10-30 ms       : ~USB typematic rate (HZ/50 = 20 ms) -> latency dupes
   30-80 ms       : fast, ambiguous
   80-300 ms      : human same-key digraph ("ll", "ss") -> keep
   > 300 ms       : deliberate keystrokes */
enum { B_SUB10, B_10_30, B_30_80, B_80_300, B_OVER300, B_COUNT };
static const char *const BUCKET_LABEL[B_COUNT] = {
    "  < 10 ms (duplicate?)   ",
    " 10-30 ms (~typematic)   ",
    " 30-80 ms (ambiguous)    ",
    " 80-300 ms (human digraph)",
    "  > 300 ms (deliberate)  ",
};

typedef struct {
    AxlLoop  *loop;
    uint64_t  last_us;
    uint32_t  keys;
    uint32_t  buckets[B_COUNT];
    uint64_t  min_dt;      /* smallest gap seen between same... any keys */
} KeyTrace;

static int
bucket_of(uint64_t dt_us)
{
    uint64_t ms = dt_us / 1000;
    if (ms < 10)  { return B_SUB10; }
    if (ms < 30)  { return B_10_30; }
    if (ms < 80)  { return B_30_80; }
    if (ms < 300) { return B_80_300; }
    return B_OVER300;
}

static bool
on_key(const AxlInputEvent *ev, void *data)
{
    KeyTrace *t = (KeyTrace *)data;
    if (ev->type != AXL_INPUT_KEY_DOWN) {
        return AXL_SOURCE_CONTINUE;
    }
    uint64_t dt = (t->last_us == 0) ? 0 : ev->timestamp_us - t->last_us;
    t->last_us = ev->timestamp_us;
    t->keys++;

    char c = (ev->unicode >= 0x20 && ev->unicode < 0x7F) ? (char)ev->unicode : '.';
    axl_printf("KEY #%u  '%c' uni=0x%04X scan=0x%04X  dt=%lu us\n",
               (unsigned)t->keys, c, (unsigned)ev->unicode,
               (unsigned)ev->keycode, (unsigned long)dt);

    if (dt > 0) {
        t->buckets[bucket_of(dt)]++;
        if (t->min_dt == 0 || dt < t->min_dt) {
            t->min_dt = dt;
        }
    }
    if (ev->unicode == 0x1B) {        /* ESC quits */
        axl_loop_quit(t->loop);
    }
    return AXL_SOURCE_CONTINUE;
}

static bool
on_timeout(void *data)
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    KeyTrace t = {0};
    t.loop = axl_loop_new();

    AxlSourceId kid = axl_input_attach_key(t.loop, on_key, &t);
    if (kid == 0) {
        axl_printf("keytrace: no keyboard available.\n");
        axl_loop_free(t.loop);
        return 1;
    }
    axl_printf("keytrace: type normally; ESC to finish (auto-quits in 5 min).\n");

    axl_loop_add_timeout(t.loop, 300000, on_timeout, t.loop);
    axl_loop_run(t.loop);

    axl_printf("\nkeytrace: %u KEY_DOWN events; smallest gap %lu us.\n",
               (unsigned)t.keys, (unsigned long)t.min_dt);
    axl_printf("inter-arrival histogram (gap to previous KEY_DOWN):\n");
    for (int b = 0; b < B_COUNT; b++) {
        axl_printf("  %s : %u\n", BUCKET_LABEL[b], (unsigned)t.buckets[b]);
    }

    axl_loop_free(t.loop);
    return 0;
}
