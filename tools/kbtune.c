/* kbtune.c — keyboard debounce/cadence tuner (GOP UI, in-tool).
 *
 * Diagnoses and lets you tune the fix for "bouncing" / repeating keystrokes at
 * the UEFI shell prompt over a BMC/KVM virtual console (iDRAC, Avocent, IPMI
 * SOL). The KVM's USB-HID key-up (break) latency drives the firmware's typematic
 * repeat, so one physical press can arrive several times.
 *
 * IMPORTANT (2026-07-08 real-hardware finding): axedit is bounce-immune with the
 * software debounce OFF, so its immunity is ARCHITECTURAL — the read + render
 * cadence, not a filter (see docs/AXL-KbTune-Design.md). kbtune therefore exposes
 * the axes as live, orthogonal tunables so you can find what actually helps YOUR
 * link:
 *
 *   MODES (read + render cadence presets) — the three real UEFI consumers:
 *     F1  Shell (greedy)      drain ALL queued keys/pass, immediate redraw
 *                             (reproduces the shell command line)
 *     F2  axEdit (app)        drain ALL/pass, throttled redraw
 *                             (how the axedit app reads + repaints)
 *     F3  edit (throttled)    1 key/pass + a per-pass stall, throttled redraw
 *                             (what the EDK2 `edit` command does: CheckEvent+Stall)
 *
 *   TUNABLES (Left/Right picks one, Up/Down adjusts it) — a mode sets sensible
 *   defaults, then tweak any axis live:
 *     debounce (ms)   same-key repeat filter window (0 = OFF; the optional
 *                     axl_input_key_accept filter — off by default)
 *     stall (ms)      per-pass poll stall (the `edit` cadence throttle)
 *     drain/pass      keys read per pass (0 = all; 1 = one, like `edit`)
 *     redraw (ms)     min interval between repaints (0 = every pass)
 *
 *   F4 = printable-only (debounce exempts navigation keys)   Esc = quit
 *
 * Sweep `drain/pass` (1 -> all) on a bouncing link to see when the bounce
 * appears — that answers whether the drain cadence alone is axedit's immunity.
 *
 * Needs a GOP console (the KVM captures it as video). Build: axl-cc kbtune.c.
 */

#include <axl.h>
#include "kbtune-shared.h"

/* UEFI scan codes (EFI_INPUT_KEY.ScanCode). */
#define SCAN_UP    0x01
#define SCAN_DOWN  0x02
#define SCAN_RIGHT 0x03
#define SCAN_LEFT  0x04
#define SCAN_F1    0x0B
#define SCAN_F2    0x0C
#define SCAN_F3    0x0D
#define SCAN_F4    0x0E
#define SCAN_F10   0x14
#define SCAN_ESC   0x17
#define UNI_TAB    0x09
#define UNI_BS     0x08
#define UNI_CR     0x0D
#define UNI_LF     0x0A

enum { MODE_SHELL = 0, MODE_AXEDIT = 1, MODE_EDIT = 2, MODE_COUNT = 3 };
static const char *MODE_NAME[MODE_COUNT] = {
    "Shell (greedy)", "axEdit (app)", "edit (throttled)"
};

/* Orthogonal tunables. A mode preset seeds them; Left/Right selects one and
   Up/Down adjusts it. Stored as an array so selection is a plain index. */
/* debounce + min-gap are the SOURCE-SIDE conditioners that persist to the shell
   via kbtune-drv (F10 commits them); stall/drain/redraw are UI-only cadence
   simulation. */
enum { TUNE_DEBOUNCE = 0, TUNE_MINGAP, TUNE_STALL, TUNE_DRAIN, TUNE_REDRAW, TUNE_COUNT };
static const struct {
    const char *name;
    const char *unit;
    uint32_t    min, max, step;
} TUNE_META[TUNE_COUNT] = {
    [TUNE_DEBOUNCE] = { "debounce", "ms",   0, 250, 5 },
    [TUNE_MINGAP]   = { "min-gap",  "ms",   0, 250, 5 },
    [TUNE_STALL]    = { "stall",    "ms",   0, 250, 5 },
    [TUNE_DRAIN]    = { "drain",    "/pass",0, 64,  1 },
    [TUNE_REDRAW]   = { "redraw",   "ms",   0, 200, 5 },
};

#define LOG_MAX  12
#define ECHO_MAX 200

typedef struct {
    uint64_t    delta;
    uint32_t    scan;
    uint32_t    uni;
    uint32_t    mods;
    const char *tag;
} LogRow;

typedef struct {
    AxlGfxInfo     info;
    const AxlFont *font;

    int       mode;
    uint32_t  tune[TUNE_COUNT];   /* live tunable values (see TUNE_*)          */
    int       sel;                /* which tunable Up/Down adjusts             */
    bool      printable_only;
    AxlKeyDebounce db;            /* debounce recognizer state                 */
    bool      quit;

    char      echo[ECHO_MAX];
    size_t    echo_len;

    LogRow    log[LOG_MAX];
    int       log_count;
    int       log_head;

    bool      have_prev;
    uint64_t  prev_us;
    uint32_t  prev_scan;
    uint32_t  prev_uni;
    uint32_t  burst;
    uint64_t  min_delta;
    uint64_t  total;
    uint64_t  dropped;

    bool      dirty;   /* something changed -> repaint (subject to redraw throttle) */
    bool      force;   /* control change -> repaint now, bypass the throttle        */

    KbTuneVtable *drv;      /* resident kbtune-drv config channel (NULL if none)     */
    const char   *drv_msg;  /* one-line driver / commit status for the HUD           */
} KbTune;

/* The debounce recognizer consults the GLOBAL tuning; push the current window
   on any change. Window 0 disables it (axl_input_key_accept always accepts). */
static void
apply_debounce(KbTune *kb)
{
    axl_input_set_key_debounce(kb->tune[TUNE_DEBOUNCE], kb->printable_only);
}

static void
enter_mode(KbTune *kb, int m)
{
    kb->mode      = m;
    kb->db        = (AxlKeyDebounce){0};   /* fresh recognizer state */
    kb->have_prev = false;                 /* restart delta chain    */
    kb->burst     = 0;

    /* Mode presets over the orthogonal axes. Read cadence + redraw cadence are
       what actually distinguish the three consumers; the debounce filter is
       left OFF (axedit is immune without it — see the file header). */
    switch (m) {
    case MODE_SHELL:                       /* greedy drain, immediate redraw */
        kb->tune[TUNE_DRAIN]  = 0;         /* 0 = drain all */
        kb->tune[TUNE_STALL]  = 0;
        kb->tune[TUNE_REDRAW] = 0;         /* repaint every pass */
        kb->sel = TUNE_DRAIN;
        break;
    case MODE_AXEDIT:                      /* drain-all, throttled redraw */
        kb->tune[TUNE_DRAIN]  = 0;
        kb->tune[TUNE_STALL]  = 0;
        kb->tune[TUNE_REDRAW] = 16;
        kb->sel = TUNE_DEBOUNCE;
        break;
    case MODE_EDIT:                        /* one key/pass + stall, throttled */
        kb->tune[TUNE_DRAIN]  = 1;
        kb->tune[TUNE_STALL]  = 30;
        kb->tune[TUNE_REDRAW] = 16;
        kb->sel = TUNE_STALL;
        break;
    default:
        break;
    }
    kb->tune[TUNE_DEBOUNCE] = 0;           /* filter off by default */
    kb->tune[TUNE_MINGAP]   = 0;           /* spacing off by default */
    apply_debounce(kb);
    kb->dirty = true;
    kb->force = true;
}

/* Commit the current debounce + min-gap window to the resident driver so it
   persists for the shell after kbtune exits. Loads kbtune-drv (staged beside
   this tool) if it isn't resident yet. */
static void
commit_to_driver(KbTune *kb)
{
    void *iface = NULL;
    if (axl_shared_driver_locate_sibling(KBTUNE_SHARED_NAME, "kbtune-drv.efi",
                                         &iface) != AXL_OK || iface == NULL) {
        kb->drv     = NULL;
        kb->drv_msg = "driver: load FAILED (is kbtune-drv.efi beside kbtune?)";
        return;
    }
    /* Guard the vtable ABI before calling through it: the warm resident
       short-circuit can hand back an OLDER kbtune-drv from earlier this boot, and
       a mismatched layout would mean calling get/set at the wrong offset. */
    if (((KbTuneVtable *)iface)->version != KBTUNE_VTABLE_VERSION) {
        kb->drv     = NULL;
        kb->drv_msg = "driver: version mismatch (resident kbtune-drv is a different build)";
        return;
    }
    kb->drv = (KbTuneVtable *)iface;
    AxlKbTuneConfig cfg = {
        .version        = KBTUNE_CONFIG_VERSION,
        .enabled        = true,
        .debounce_ms    = kb->tune[TUNE_DEBOUNCE],
        .min_gap_ms     = kb->tune[TUNE_MINGAP],
        .printable_only = kb->printable_only,
    };
    kb->drv_msg = (kb->drv->set(&cfg) == AXL_OK)
                ? "driver: COMMITTED -- persists for the shell after exit"
                : "driver: set() failed";
}

/* Warm-attach to an already-resident kbtune-drv (does NOT load it) and pre-seed
   the debounce/min-gap tunables from its live config, so re-running kbtune
   reattaches to re-tune. */
static void
attach_resident_driver(KbTune *kb)
{
    void   *iface = NULL;
    AxlGuid g;
    if (axl_shared_driver_guid(KBTUNE_SHARED_NAME, &g) == AXL_OK
        && axl_protocol_find_guid(&g, &iface) == AXL_OK && iface != NULL
        && ((KbTuneVtable *)iface)->version == KBTUNE_VTABLE_VERSION) {
        kb->drv = (KbTuneVtable *)iface;
        AxlKbTuneConfig cfg;
        if (kb->drv->get(&cfg) == AXL_OK && cfg.version == KBTUNE_CONFIG_VERSION) {
            kb->tune[TUNE_DEBOUNCE] = cfg.debounce_ms;
            kb->tune[TUNE_MINGAP]   = cfg.min_gap_ms;
            kb->printable_only      = cfg.printable_only;
            apply_debounce(kb);
            kb->drv_msg = cfg.enabled
                        ? "driver: RESIDENT (reattached; F10 re-commits)"
                        : "driver: resident but idle (F10 commits)";
            return;
        }
    }
    kb->drv     = NULL;
    kb->drv_msg = "driver: not loaded (F10 commits debounce+min-gap for the shell)";
}

/* Adjust the selected tunable by @p dir steps, clamped to its range. */
static void
adjust_tune(KbTune *kb, int dir)
{
    uint32_t step = TUNE_META[kb->sel].step;
    uint32_t lo   = TUNE_META[kb->sel].min;
    uint32_t hi   = TUNE_META[kb->sel].max;
    uint32_t v    = kb->tune[kb->sel];
    if (dir > 0) {
        v = (v + step > hi) ? hi : v + step;
    } else {
        v = (v < lo + step) ? lo : v - step;
    }
    kb->tune[kb->sel] = v;
    if (kb->sel == TUNE_DEBOUNCE) {
        apply_debounce(kb);
    }
    kb->dirty = true;
    kb->force = true;
}

static void
mods_str(uint32_t m, char *buf, size_t n)
{
    size_t l = 0;
    buf[0] = '\0';
    if (m & AXL_INPUT_MOD_CTRL)  { l += (size_t)axl_snprintf(buf + l, n - l, "Ctrl "); }
    if (m & AXL_INPUT_MOD_SHIFT) { l += (size_t)axl_snprintf(buf + l, n - l, "Shift "); }
    if (m & AXL_INPUT_MOD_ALT)   { l += (size_t)axl_snprintf(buf + l, n - l, "Alt "); }
    if (l == 0) {
        axl_snprintf(buf, n, "-");
    }
}

static const char *
classify(bool same, uint64_t d)
{
    if (!same || d == 0) {
        return "";
    }
    if (d < 5000)                 { return "BOUNCE"; }
    if (d >= 14000 && d <= 26000) { return "typematic~20ms"; }
    if (d < 60000)                { return "fast-repeat"; }
    return "";
}

static void
echo_key(KbTune *kb, uint32_t uni)
{
    if (uni == UNI_BS) {
        if (kb->echo_len > 0) { kb->echo[--kb->echo_len] = '\0'; }
        return;
    }
    if (uni == UNI_CR || uni == UNI_LF) {   /* Enter submits: clear the line. */
        kb->echo_len = 0;
        kb->echo[0]  = '\0';
        return;
    }
    if (uni >= 0x20 && uni < 0x7f && kb->echo_len < ECHO_MAX - 1) {
        kb->echo[kb->echo_len++] = (char)uni;
        kb->echo[kb->echo_len]   = '\0';
    }
}

static void
log_push(KbTune *kb, uint64_t d, uint32_t scan, uint32_t uni, uint32_t mods,
         const char *tag)
{
    int idx = (kb->log_head + kb->log_count) % LOG_MAX;
    if (kb->log_count < LOG_MAX) {
        kb->log_count++;
    } else {
        kb->log_head = (kb->log_head + 1) % LOG_MAX;
    }
    kb->log[idx].delta = d;
    kb->log[idx].scan  = scan;
    kb->log[idx].uni   = uni;
    kb->log[idx].mods  = mods;
    kb->log[idx].tag   = tag;
}

/* Returns true if the keystroke was a control key (consumed, not echoed). */
static bool
handle_control(KbTune *kb, uint32_t scan, uint32_t uni)
{
    switch (scan) {
    case SCAN_ESC:   kb->quit = true;                                    return true;
    case SCAN_F10:   commit_to_driver(kb); kb->dirty = true; kb->force = true; return true;
    case SCAN_F1:    enter_mode(kb, MODE_SHELL);                         return true;
    case SCAN_F2:    enter_mode(kb, MODE_AXEDIT);                        return true;
    case SCAN_F3:    enter_mode(kb, MODE_EDIT);                          return true;
    case SCAN_F4:    kb->printable_only = !kb->printable_only;
                     apply_debounce(kb); kb->dirty = true; kb->force = true; return true;
    case SCAN_UP:    adjust_tune(kb, +1);                                return true;
    case SCAN_DOWN:  adjust_tune(kb, -1);                                return true;
    case SCAN_LEFT:  kb->sel = (kb->sel + TUNE_COUNT - 1) % TUNE_COUNT;
                     kb->dirty = true; kb->force = true;                 return true;
    case SCAN_RIGHT: kb->sel = (kb->sel + 1) % TUNE_COUNT;
                     kb->dirty = true; kb->force = true;                 return true;
    default: break;
    }
    if (uni == UNI_TAB) {
        enter_mode(kb, (kb->mode + 1) % MODE_COUNT);
        return true;
    }
    return false;
}

/* Process one raw keystroke. @p filter runs the debounce recognizer (on when the
   debounce window is > 0). */
static void
process_key(KbTune *kb, const AxlKey *k, bool filter)
{
    uint32_t scan = k->scan_code;
    uint32_t uni  = k->unicode_char;
    uint32_t mods = k->modifiers;
    uint64_t now  = axl_time_get_us();

    if (handle_control(kb, scan, uni)) {
        return;
    }

    uint64_t d    = kb->have_prev ? (now - kb->prev_us) : 0;
    bool     same = kb->have_prev && scan == kb->prev_scan && uni == kb->prev_uni;

    bool dropped = false;
    if (filter) {
        AxlInputEvent ev = {
            .type         = AXL_INPUT_KEY_DOWN,
            .keycode      = scan,
            .unicode      = uni,
            .modifiers    = mods,
            .timestamp_us = now,
        };
        dropped = !axl_input_key_accept(&kb->db, &ev);
    }

    log_push(kb, d, scan, uni, mods, dropped ? "DROP (debounced)" : classify(same, d));
    if (dropped) {
        kb->dropped++;
    } else {
        echo_key(kb, uni);
    }

    if (same && d > 0 && d < 60000) {
        kb->burst++;
        if (kb->min_delta == 0 || d < kb->min_delta) { kb->min_delta = d; }
    } else {
        kb->burst = 1;
    }
    kb->total++;
    kb->prev_us   = now;
    kb->prev_scan = scan;
    kb->prev_uni  = uni;
    kb->have_prev = true;
    kb->dirty     = true;
}

static void
char_name(uint32_t uni, char *buf, size_t n)
{
    if (uni >= 0x20 && uni < 0x7f) {
        axl_snprintf(buf, n, "'%c'", (char)uni);
    } else {
        axl_snprintf(buf, n, "0x%04x", (unsigned)uni);
    }
}

static void
draw_mode_buttons(KbTune *kb, uint32_t x, uint32_t y)
{
    for (int i = 0; i < MODE_COUNT; i++) {
        char label[40];
        axl_snprintf(label, sizeof label, "F%d %s", i + 1, MODE_NAME[i]);
        uint32_t w = axl_gfx_measure_text(kb->font, label, 1) + 16;
        bool active = (i == kb->mode);
        axl_gfx_fill_rect(x, y, w, 24,
                          active ? AXL_GFX_RGB(0x1e, 0x50, 0x28)
                                 : AXL_GFX_RGB(0x1c, 0x1e, 0x2c));
        axl_gfx_draw_rect(x, y, w, 24, active ? AXL_GFX_GREEN : AXL_GFX_GRAY);
        axl_gfx_draw_text(kb->font, x + 8, y + 5, label,
                          active ? AXL_GFX_WHITE : AXL_GFX_GRAY, 1);
        x += w + 12;
    }
}

/* One box per tunable; the selected one (Up/Down target) is highlighted. The
   debounce box also shows OFF when the window is 0. */
static void
draw_tunables(KbTune *kb, uint32_t x, uint32_t y)
{
    for (int i = 0; i < TUNE_COUNT; i++) {
        char label[48];
        uint32_t v = kb->tune[i];
        if (i == TUNE_DEBOUNCE && v == 0) {
            axl_snprintf(label, sizeof label, "%s=OFF", TUNE_META[i].name);
        } else if (i == TUNE_DRAIN && v == 0) {
            axl_snprintf(label, sizeof label, "%s=all", TUNE_META[i].name);
        } else {
            axl_snprintf(label, sizeof label, "%s=%u%s",
                         TUNE_META[i].name, (unsigned)v, TUNE_META[i].unit);
        }
        uint32_t w = axl_gfx_measure_text(kb->font, label, 1) + 14;
        bool active = (i == kb->sel);
        axl_gfx_fill_rect(x, y, w, 22,
                          active ? AXL_GFX_RGB(0x40, 0x38, 0x12)
                                 : AXL_GFX_RGB(0x1c, 0x1e, 0x2c));
        axl_gfx_draw_rect(x, y, w, 22, active ? AXL_GFX_YELLOW : AXL_GFX_GRAY);
        axl_gfx_draw_text(kb->font, x + 7, y + 4, label,
                          active ? AXL_GFX_WHITE : AXL_GFX_GRAY, 1);
        x += w + 10;
    }
}

static void
render(KbTune *kb)
{
    uint32_t W = kb->info.width;
    uint32_t H = kb->info.height;
    const AxlFont *f = kb->font;
    char buf[192];

    axl_gfx_fill_rect(0, 0, W, H, AXL_GFX_RGB(0x0b, 0x0d, 0x16));
    axl_gfx_draw_text(f, 16, 12, "kbtune -- keyboard bounce tuner", AXL_GFX_CYAN, 2);

    draw_mode_buttons(kb, 16, 50);
    draw_tunables(kb, 16, 84);

    axl_gfx_draw_text(f, 16, 112,
        "F1/F2/F3=mode  Tab=cycle  Left/Right=pick tunable  Up/Down=adjust  "
        "F4=printable-only  F10=commit(debounce+min-gap)->shell  Esc=quit",
        AXL_GFX_GRAY, 1);
    if (kb->drv_msg != NULL) {
        AxlGfxPixel c = (kb->drv_msg[8] == 'C')                     /* COMMITTED */
                      ? AXL_GFX_GREEN
                      : (kb->drv_msg[8] == 'R' || kb->drv_msg[8] == 'r')
                          ? AXL_GFX_CYAN : AXL_GFX_GRAY;
        axl_gfx_draw_text(f, 16, 128, kb->drv_msg, c, 1);
    }

    /* Echo pane. The echoed line is drawn at font scale 2 (glyph cell 16px ->
       32px tall), so the box is 40px tall and the text vertically centered. */
    axl_gfx_draw_text(f, 16, 152, "Echo (type here; Enter clears):", AXL_GFX_WHITE, 1);
    axl_gfx_draw_rect(16, 172, W - 32, 40, AXL_GFX_GRAY);
    axl_gfx_draw_text(f, 24, 176, kb->echo, AXL_GFX_GREEN, 2);

    /* Event log. */
    axl_gfx_draw_text(f, 16, 220,
        "Event log (one physical press = one row unless it bounces):", AXL_GFX_WHITE, 1);
    uint32_t row_y = 244;
    for (int i = 0; i < kb->log_count; i++) {
        int idx = (kb->log_head + i) % LOG_MAX;
        LogRow *r = &kb->log[idx];
        char cb[12];
        char mb[24];
        char_name(r->uni, cb, sizeof cb);
        mods_str(r->mods, mb, sizeof mb);
        axl_snprintf(buf, sizeof buf,
                     "#%-3d  d=%7lu us  scan=0x%04x  char=%-8s  mod=%-12s %s",
                     i + 1, (unsigned long)r->delta, (unsigned)r->scan, cb, mb, r->tag);
        AxlGfxPixel c;
        if (r->tag[0] == 'B')                       { c = AXL_GFX_RED;    }   /* BOUNCE */
        else if (r->tag[0] == 'D')                  { c = AXL_GFX_GRAY;   }   /* DROP   */
        else if (r->tag[0] != '\0')                 { c = AXL_GFX_YELLOW; }   /* typematic/fast */
        else                                        { c = AXL_GFX_CYAN;   }
        axl_gfx_draw_text(f, 24, row_y, buf, c, 1);
        row_y += 18;
    }

    /* Stat strip. */
    char cb2[12];
    char_name(kb->prev_uni, cb2, sizeof cb2);
    axl_snprintf(buf, sizeof buf,
                 "mode=%s   seen=%lu   debounced-drops=%lu   last=%s   burst=%u   "
                 "min gap=%lu us",
                 MODE_NAME[kb->mode], (unsigned long)kb->total,
                 (unsigned long)kb->dropped, cb2, (unsigned)kb->burst,
                 (unsigned long)kb->min_delta);
    axl_gfx_draw_text(f, 16, H - 28, buf, AXL_GFX_WHITE, 1);
}

static int
run_kbtune(AxlArgs *a)
{
    (void)a;

    if (!axl_gfx_available()) {
        axl_printf("kbtune: no graphics output (headless). This tool needs a GOP "
                   "console (run it over the KVM's video console).\r\n");
        return 0;
    }

    KbTune kb = {0};
    axl_gfx_get_info(&kb.info);
    kb.font           = axl_gfx_default_font();
    kb.printable_only = true;
    enter_mode(&kb, MODE_SHELL);   /* seeds the tunables + selection */

    /* Reattach to an already-resident kbtune-drv (from an earlier run this boot)
       and pre-seed the persisted tunables from it, so re-running re-tunes. Does
       NOT load the driver — that happens on the first F10 commit. */
    attach_resident_driver(&kb);

    render(&kb);
    kb.dirty = false;
    kb.force = false;
    uint64_t last_render = axl_time_get_us();

    /* Hand-rolled poll loop. The mode's `drain/pass` sets the read cadence
       (0 = drain the whole firmware queue like the shell/axedit; 1 = one key per
       pass like EDK2 `edit`), and `redraw` throttles repaints so a burst of keys
       coalesces into one paint instead of one-per-key. A control change forces an
       immediate repaint (kb.force) so the HUD stays responsive. */
    while (!kb.quit && !axl_interrupted()) {
        bool     filter  = kb.tune[TUNE_DEBOUNCE] > 0;
        uint32_t cap     = kb.tune[TUNE_DRAIN];   /* 0 = unlimited */
        uint32_t drained = 0;
        AxlKey   k;
        while (axl_console_read_key(0, &k) == AXL_OK) {
            process_key(&kb, &k, filter);
            if (kb.quit) { break; }
            drained++;
            if (cap != 0 && drained >= cap) { break; }
        }

        uint64_t now = axl_time_get_us();
        uint32_t rd  = kb.tune[TUNE_REDRAW];
        if (kb.dirty && (kb.force || rd == 0 || now - last_render >= (uint64_t)rd * 1000)) {
            render(&kb);
            kb.dirty     = false;
            kb.force     = false;
            last_render  = now;
        }

        uint32_t stall = kb.tune[TUNE_STALL];
        axl_msleep(stall ? stall : 2);
    }

    axl_printf("kbtune: exit (mode=%s, debounce=%ums, min-gap=%ums, stall=%ums, "
               "drain=%u, redraw=%ums, seen=%lu, dropped=%lu)\r\n",
               MODE_NAME[kb.mode], (unsigned)kb.tune[TUNE_DEBOUNCE],
               (unsigned)kb.tune[TUNE_MINGAP], (unsigned)kb.tune[TUNE_STALL],
               (unsigned)kb.tune[TUNE_DRAIN], (unsigned)kb.tune[TUNE_REDRAW],
               (unsigned long)kb.total, (unsigned long)kb.dropped);
    return 0;
}

AXL_TOOL_MAIN(kbtune)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name    = "kbtune",
        .help    = "Interactive keyboard debounce tuner (needs a GOP console)",
        .handler = run_kbtune,
    });
}
