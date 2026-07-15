/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-mirror.c
    Mirror the firmware console to a byte sink and inject remote input.

    A thin consumer of the console tap (src/util/axl-console-tap.c): the tap does
    the firmware surgery and reports structured console operations, and this file
    is the **VT encoder** that serializes those operations into the UTF-8 + ANSI/VT
    byte stream an xterm-class terminal understands, handing it to a caller sink.

    That split is the point: the VT wire format only exists to serialize a console
    to a REMOTE consumer. A LOCAL renderer skips it and binds AxlConsoleOps straight
    into its cell grid. See docs/AXL-Console-Mirror-Design.md and
    <axl/axl-console-tap.h>.

    Encoder-only state lives here: the sink, and the redundant-cursor dedup (a
    full-screen app re-positions to the same cell to blink its cursor, which would
    otherwise flood the wire with escapes).
**/

#include <axl/axl-console-mirror.h>
#include <axl/axl-console-tap.h>
#include <axl/axl-console-screen.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include "axl-console-vt.h"   /* shared pen->SGR encoder (DRY with axl-console-screen) */

AXL_LOG_DOMAIN("conmirror");

#define MIRROR_DEFAULT_COLS 80
#define MIRROR_DEFAULT_ROWS 25

struct AxlConsoleMirror {
    AxlConsoleTap   *tap;    /* the surgery we consume */

    /* Late-join model: an internal screen fed from our own emitted VT, so
       snapshot() can serialize the current screen for a newly-connected client. */
    AxlConsoleScreen *screen;

    /* Config (copied). */
    AxlConsoleSinkFn sink;
    void            *user;

    /* Cursor-dedup state: the last position EMITTED as an escape (or 0,0 after a
       clear). Encoder-only — the tap separately maintains the true console cursor
       in the Mode it owns. Not advanced from text output (that heuristic drifts on
       line-wrap and would falsely suppress a needed reposition). */
    int32_t cur_row;   /* -1 = unknown */
    int32_t cur_col;
};

static AxlConsoleMirror *g_mirror;  /* one console => one mirror */

/* Create the internal late-join screen model at @p cols x @p rows (0 -> default).
   Returns NULL on allocation failure (the caller decides how to handle it). */
static AxlConsoleScreen *
mirror_new_screen(uint32_t cols, uint32_t rows)
{
    AxlConsoleScreen *s = NULL;
    axl_console_screen_new(&s,
                           rows != 0 ? rows : MIRROR_DEFAULT_ROWS,
                           cols != 0 ? cols : MIRROR_DEFAULT_COLS);
    return s;
}

/* Size the internal model to the tap's RESOLVED geometry — which resolves a
   configured-0 axis to the physical console size — so a mirror installed (or
   resized) with a 0 axis keeps the model matched to what the tap actually runs
   at, not the 80x25 default. A 0 resolved axis (physical size unavailable) leaves
   the model unchanged. */
static void
mirror_sync_screen_size(AxlConsoleMirror *m)
{
    uint32_t cols = 0, rows = 0;
    axl_console_tap_get_size(m->tap, &cols, &rows);
    if (cols != 0 && rows != 0) {
        axl_console_screen_resize(m->screen, rows, cols);
    }
}

// ---------------------------------------------------------------------------
// Sink helpers
// ---------------------------------------------------------------------------

static void
emit(AxlConsoleMirror *m, const char *bytes, size_t len)
{
    if (len == 0) {
        return;
    }
    if (m->sink != NULL) {
        m->sink(bytes, len, m->user);
    }
    /* Tee into the late-join model so snapshot() serializes the current screen.
       The model re-parses our own VT — cheap at console volumes, and it keeps
       alt-screen / cursor state in sync automatically (those escapes flow here). */
    if (m->screen != NULL) {
        axl_console_screen_feed(m->screen, (const uint8_t *)bytes, len);
    }
}

static void
emit_cstr(AxlConsoleMirror *m, const char *s)
{
    emit(m, s, axl_strlen(s));
}

// ---------------------------------------------------------------------------
// The VT encoder — an AxlConsoleOps consumer. Turns structured console
// operations into the xterm/VT byte stream handed to the caller's sink.
// ---------------------------------------------------------------------------

static void
vt_clear_screen(void *user)
{
    AxlConsoleMirror *m = (AxlConsoleMirror *)user;
    emit_cstr(m, "\x1b[2J\x1b[H");
    m->cur_row = 0;   /* the terminal cursor is home; dedup tracks from here */
    m->cur_col = 0;
}

static void
vt_set_cursor(void *user, int32_t row, int32_t col)
{
    AxlConsoleMirror *m = (AxlConsoleMirror *)user;
    /* Dedup: full-screen apps re-position to the same cell to blink the cursor;
       suppress the redundant escape flood. */
    if (row == m->cur_row && col == m->cur_col) {
        return;
    }
    char buf[24];
    int  n = axl_snprintf(buf, sizeof(buf), "\x1b[%u;%uH",
                          (unsigned)(row + 1), (unsigned)(col + 1));
    if (n > 0) {
        emit(m, buf, (size_t)n);
    }
    m->cur_row = row;
    m->cur_col = col;
}

static void
vt_output_text(void *user, const char *utf8, size_t len)
{
    emit((AxlConsoleMirror *)user, utf8, len);
}

/* UEFI-indexed pen -> ANSI SGR "ESC[0;fg;bgm". fg 0..15, bg 0..7. This is the old
   vt_set_attr body, unchanged, so the golden VT stream stays byte-identical. */
static void
vt_emit_indexed_sgr(AxlConsoleMirror *m, uint8_t fg, uint8_t bg)
{
    /* UEFI fg 0-15 -> ANSI SGR. 0-7 standard (30-37), 8-15 bright (90-97),
       EXCEPT index 14: ANSI bright-yellow (93) renders as lime/green on many
       terminals, so map UEFI "yellow" to plain 33 (matches the EDK2 original
       - deliberate, do not "fix" to 93). */
    static const uint8_t fg_map[16] = {
        30, 34, 32, 36, 31, 35, 33, 37,
        90, 94, 92, 96, 91, 95, 33, 97
    };
    static const uint8_t bg_map[8] = { 40, 44, 42, 46, 41, 45, 43, 47 };

    char buf[20];
    int  n = axl_snprintf(buf, sizeof(buf), "\x1b[0;%u;%um",
                          fg_map[fg & 0x0F], bg_map[bg & 0x07]);
    if (n > 0) {
        emit(m, buf, (size_t)n);   /* emit() feeds the model even when sink is NULL */
    }
}

/* Full SGR for a general pen (default / indexed / truecolour + style bits), via the
   encoder shared with axl-console-screen. Reachable only from axl-vterm — the tap
   always produces a both-indexed pen and takes the fast path in vt_set_pen. */
static void
vt_emit_full_sgr(AxlConsoleMirror *m, const AxlConsolePen *pen)
{
    char   buf[64];
    size_t n = axl_console_pen_to_sgr(buf, sizeof(buf), pen);
    emit(m, buf, n);
}

/* The pen snapshot -> SGR. The tap only ever produces DEFAULT or INDEXED colours;
   emit exactly the bytes the previous vt_set_attr(fg, bg) emitted for the indexed
   case, so the golden stream does not move. The general path is reachable only from
   axl-vterm. */
static void
vt_set_pen(void *user, const AxlConsolePen *pen)
{
    AxlConsoleMirror *m = (AxlConsoleMirror *)user;
    if (pen->fg.kind == AXL_CONSOLE_COLOR_INDEXED &&
        pen->bg.kind == AXL_CONSOLE_COLOR_INDEXED) {
        vt_emit_indexed_sgr(m, pen->fg.idx, pen->bg.idx);
        return;
    }
    vt_emit_full_sgr(m, pen);
}

static void
vt_emit_dectcem(AxlConsoleMirror *m, bool visible)
{
    emit_cstr(m, visible ? "\x1b[?25h" : "\x1b[?25l");
}

/* SetMode has no VT representation in this encoder (the remote terminal's size
   is driven by the consumer's resize, not the guest's mode change). */
static void
vt_set_mode(void *user, uint32_t mode)
{
    (void)user;
    (void)mode;
}

static void
vt_emit_alt_screen(AxlConsoleMirror *m, bool enter)
{
    emit_cstr(m, enter ? "\x1b[?1049h" : "\x1b[?1049l");
}

/* One dispatcher for every terminal property the VT wire can carry. Accept-and-
   ignore anything else: the wire has no representation for the other props today. */
static int
vt_set_term_prop(void *user, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    AxlConsoleMirror *m = (AxlConsoleMirror *)user;
    switch (prop) {
    case AXL_CONSOLE_PROP_CURSOR_VISIBLE:
        vt_emit_dectcem(m, val->u.boolean);
        return 1;
    case AXL_CONSOLE_PROP_ALT_SCREEN:
        vt_emit_alt_screen(m, val->u.boolean);
        return 1;
    default:
        return 1;
    }
}

static const AxlConsoleOps vt_ops = {
    .clear_screen  = vt_clear_screen,
    .set_cursor    = vt_set_cursor,
    .output_text   = vt_output_text,
    .set_pen       = vt_set_pen,
    .set_mode      = vt_set_mode,
    .set_term_prop = vt_set_term_prop,
    /* set_cell_rule: the mirror re-encodes to a VT wire and never rasterizes, so
       cell width is the far-end terminal's problem. Deliberately unbound. */
};

// ---------------------------------------------------------------------------
// Public API — install a tap with this encoder bound, delegate the rest.
// ---------------------------------------------------------------------------

int
axl_console_mirror_install(AxlConsoleMirror **out, const AxlConsoleMirrorConfig *cfg)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL || cfg == NULL || cfg->sink == NULL) {
        return AXL_ERR;
    }
    if (g_mirror != NULL) {
        axl_warning("console mirror already installed");
        return AXL_ERR;
    }

    AxlConsoleMirror *m = axl_calloc(1, sizeof(*m));
    if (m == NULL) {
        return AXL_ERR;
    }
    m->sink    = cfg->sink;
    m->user    = cfg->user;
    m->cur_row = -1;
    m->cur_col = -1;
    m->screen  = mirror_new_screen(cfg->cols, cfg->rows);
    if (m->screen == NULL) {
        axl_free(m);
        return AXL_ERR;
    }

    AxlConsoleTapConfig tcfg = {
        .cols              = cfg->cols,
        .rows              = cfg->rows,
        .passthrough_local = cfg->passthrough_local,
        .auto_alt_screen   = cfg->auto_alt_screen,
        .input_capture     = cfg->input_capture,
    };
    if (axl_console_tap_install(&m->tap, &vt_ops, m, &tcfg) != AXL_OK) {
        axl_console_screen_free(m->screen);
        axl_free(m);
        return AXL_ERR;
    }
    mirror_sync_screen_size(m);   /* match the model to the tap's resolved size */

    g_mirror = m;
    *out = m;
    return AXL_OK;
}

void
axl_console_mirror_uninstall(AxlConsoleMirror *m)
{
    if (m == NULL || g_mirror != m) {
        return;
    }
    axl_console_tap_uninstall(m->tap);
    axl_console_screen_free(m->screen);
    g_mirror = NULL;
    axl_free(m);
}

int
axl_console_mirror_inject_key(AxlConsoleMirror *m, uint16_t scan, uint16_t unicode)
{
    return (m != NULL) ? axl_console_tap_inject_key(m->tap, scan, unicode) : AXL_ERR;
}

int
axl_console_mirror_inject_text(AxlConsoleMirror *m, const char *bytes, size_t len)
{
    if (m == NULL || bytes == NULL) {
        return AXL_ERR;
    }
    return axl_console_tap_inject_text(m->tap, bytes, len);
}

void
axl_console_mirror_set_size(AxlConsoleMirror *m, uint32_t cols, uint32_t rows)
{
    if (m != NULL) {
        axl_console_tap_set_size(m->tap, cols, rows);
        /* Keep the late-join model in lockstep with the tap's RESOLVED size, so a
           partial-zero resize (e.g. cols set, rows -> physical) tracks both axes
           rather than dropping the whole resize on the 0. */
        mirror_sync_screen_size(m);
    }
}

int
axl_console_mirror_snapshot(AxlConsoleMirror *m, AxlConsoleScreenSink sink, void *user)
{
    if (m == NULL || sink == NULL) {
        return AXL_ERR;
    }
    return axl_console_screen_snapshot(m->screen, sink, user);
}

void
axl_console_mirror_reset(AxlConsoleMirror *m)
{
    if (m == NULL) {
        return;
    }
    m->cur_row = -1;   /* forget the emitted-cursor dedup baseline */
    m->cur_col = -1;
    axl_console_tap_reset(m->tap);
}

void
axl_console_mirror_enter_alt_screen(AxlConsoleMirror *m)
{
    if (m != NULL) {
        axl_console_tap_enter_alt_screen(m->tap);
    }
}

void
axl_console_mirror_leave_alt_screen(AxlConsoleMirror *m)
{
    if (m != NULL) {
        axl_console_tap_leave_alt_screen(m->tap);
    }
}

bool
axl_console_mirror_in_alt_screen(const AxlConsoleMirror *m)
{
    return m != NULL && axl_console_tap_in_alt_screen(m->tap);
}

// ---------------------------------------------------------------------------
// Test seam (no public header). Builds a bare, un-installed mirror whose VT
// encoder can be bound over a headless tap, so the emitted byte stream is
// assertable without wrapping the live console (a real install wedges the
// combined unit boot — see the AxlConsoleMirror note in axl-test-util.c).
// ---------------------------------------------------------------------------

AxlConsoleMirror *
_axl_console_mirror_new_for_test(void)
{
    AxlConsoleMirror *m = axl_calloc(1, sizeof(*m));
    if (m != NULL) {
        m->cur_row = -1;
        m->cur_col = -1;
        m->screen  = mirror_new_screen(MIRROR_DEFAULT_COLS, MIRROR_DEFAULT_ROWS);
    }
    return m;
}

/* Bind this mirror's encoder to a (headless) tap and hand back the ops table +
   context so the caller can drive the tap's wraps into it. */
void
_axl_console_mirror_test_bind(AxlConsoleMirror *m, AxlConsoleSinkFn sink, void *user,
                              AxlConsoleTap *tap, const AxlConsoleOps **ops,
                              void **ops_user)
{
    if (m == NULL) {
        return;
    }
    m->tap     = tap;
    m->sink    = sink;
    m->user    = user;
    m->cur_row = -1;
    m->cur_col = -1;
    if (ops != NULL) {
        *ops = &vt_ops;
    }
    if (ops_user != NULL) {
        *ops_user = m;
    }
}

AxlConsoleScreen *
_axl_console_mirror_test_screen(AxlConsoleMirror *m)
{
    return (m != NULL) ? m->screen : NULL;
}

void
_axl_console_mirror_test_free(AxlConsoleMirror *m)
{
    if (m != NULL) {
        axl_console_screen_free(m->screen);
        axl_free(m);
    }
}

