/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-vt-enc.c
    The REMOTE sink for AxlConsoleOps: structured console operations -> the UTF-8 +
    ANSI/VT byte stream an xterm-class terminal understands.

    This is the encoder `axl-console-mirror` has always been, lifted out of it so the
    OTHER producer can use it. The mirror bundles tap+encoder and is the swap-strategy
    convenience wrapper; `axl-console-device` (take-over strategy) had no path to a
    remote terminal at all before this, because the only public AxlConsoleOps consumer
    was `axl-console-term`, which rasterizes to a GOP grid rather than a wire.

    Encoder-only state lives here: the sink, the late-join screen model, and the
    redundant-cursor dedup (a full-screen app re-positions to the same cell to blink
    its cursor, which would otherwise flood the wire with escapes).

    The op bodies are the mirror's, moved verbatim — the emitted byte stream is
    load-bearing (goldens, and every already-deployed consumer), so the refactor is
    a move, not a rewrite. See docs/AXL-Console-Mirror-Design.md.
**/

#include <axl/axl-console-vt-enc.h>
#include <axl/axl-console-screen.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include "axl-console-vt.h"   /* shared pen->SGR encoder (DRY with axl-console-screen) */

AXL_LOG_DOMAIN("console");

#define VT_ENC_DEFAULT_COLS 80
#define VT_ENC_DEFAULT_ROWS 25

/* Safety cap on the coalesce buffer: a consumer flushes it per loop tick, so it
   normally holds well under one screen of output, but a redraw between ticks (or
   a consumer that forgets to flush) must not grow it without bound. At the cap the
   buffer auto-flushes mid-turn — a bounded extra frame, not a leak. Kept under the
   WS single-frame budget (WS_OUT_MAX_BYTES, 512 KB) so a flush is always sendable. */
#define VT_ENC_COALESCE_CAP  (128u * 1024u)

struct AxlConsoleVtEnc {
    /* Late-join model: an internal screen fed from our own emitted VT, so
       snapshot() can serialize the current screen for a newly-connected client. */
    AxlConsoleScreen  *screen;

    /* Config (copied). */
    AxlConsoleVtSinkFn sink;
    void              *user;

    /* Live-stream coalescing (config.coalesce). When on, emit() accumulates into
       outbuf instead of calling sink per op; axl_console_vt_enc_flush drains it as
       one sink call. outbuf.buf NULL when off or on init failure (emit falls back
       to the direct sink). */
    bool               coalesce;
    AxlConsoleVtBuf    outbuf;

    /* Cursor-dedup state: the last position EMITTED as an escape (or 0,0 after a
       clear). Encoder-only — the producer separately maintains the true console
       cursor in the Mode it owns. Not advanced from text output (that heuristic
       drifts on line-wrap and would falsely suppress a needed reposition). */
    int32_t            cur_row;   /* -1 = unknown */
    int32_t            cur_col;
};

// ---------------------------------------------------------------------------
// Sink helpers
// ---------------------------------------------------------------------------

static void
emit(AxlConsoleVtEnc *e, const char *bytes, size_t len)
{
    if (len == 0) {
        return;
    }
    if (e->sink != NULL) {
        if (e->coalesce) {
            axl_console_vt_buf_emit(&e->outbuf, bytes, len);
        } else {
            e->sink(bytes, len, e->user);
        }
    }
    /* Tee into the late-join model so snapshot() serializes the current screen.
       The model re-parses our own VT — cheap at console volumes, and it keeps
       alt-screen / cursor state in sync automatically (those escapes flow here). */
    if (e->screen != NULL) {
        axl_console_screen_feed(e->screen, (const uint8_t *)bytes, len);
    }
}

static void
emit_cstr(AxlConsoleVtEnc *e, const char *s)
{
    emit(e, s, axl_strlen(s));
}

// ---------------------------------------------------------------------------
// The VT encoder — an AxlConsoleOps consumer. Turns structured console
// operations into the xterm/VT byte stream handed to the caller's sink.
// ---------------------------------------------------------------------------

static void
vt_clear_screen(void *user)
{
    AxlConsoleVtEnc *e = (AxlConsoleVtEnc *)user;
    emit_cstr(e, "\x1b[2J\x1b[H");
    e->cur_row = 0;   /* the terminal cursor is home; dedup tracks from here */
    e->cur_col = 0;
}

static void
vt_set_cursor(void *user, int32_t row, int32_t col)
{
    AxlConsoleVtEnc *e = (AxlConsoleVtEnc *)user;
    /* Dedup: full-screen apps re-position to the same cell to blink the cursor;
       suppress the redundant escape flood. */
    if (row == e->cur_row && col == e->cur_col) {
        return;
    }
    char buf[24];
    int  n = axl_snprintf(buf, sizeof(buf), "\x1b[%u;%uH",
                          (unsigned)(row + 1), (unsigned)(col + 1));
    if (n > 0) {
        emit(e, buf, (size_t)n);
    }
    e->cur_row = row;
    e->cur_col = col;
}

static void
vt_output_text(void *user, const char *utf8, size_t len)
{
    emit((AxlConsoleVtEnc *)user, utf8, len);
}

/* UEFI-indexed pen -> ANSI SGR "ESC[0;fg;bgm". fg 0..15, bg 0..7. This is the old
   vt_set_attr body, unchanged, so the golden VT stream stays byte-identical. */
static void
vt_emit_indexed_sgr(AxlConsoleVtEnc *e, uint8_t fg, uint8_t bg)
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
        emit(e, buf, (size_t)n);   /* emit() feeds the model even when sink is NULL */
    }
}

/* Full SGR for a general pen (default / indexed / truecolour + style bits), via the
   encoder shared with axl-console-screen. Reachable only from axl-vterm — the tap
   always produces a both-indexed pen and takes the fast path in vt_set_pen. */
static void
vt_emit_full_sgr(AxlConsoleVtEnc *e, const AxlConsolePen *pen)
{
    char   buf[64];
    size_t n = axl_console_pen_to_sgr(buf, sizeof(buf), pen);
    emit(e, buf, n);
}

/* The pen snapshot -> SGR. The tap only ever produces DEFAULT or INDEXED colours;
   emit exactly the bytes the previous vt_set_attr(fg, bg) emitted for the indexed
   case, so the golden stream does not move. The general path is reachable only from
   axl-vterm. */
static void
vt_set_pen(void *user, const AxlConsolePen *pen)
{
    AxlConsoleVtEnc *e = (AxlConsoleVtEnc *)user;
    if (pen->fg.kind == AXL_CONSOLE_COLOR_INDEXED &&
        pen->bg.kind == AXL_CONSOLE_COLOR_INDEXED) {
        vt_emit_indexed_sgr(e, pen->fg.idx, pen->bg.idx);
        return;
    }
    vt_emit_full_sgr(e, pen);
}

static void
vt_emit_dectcem(AxlConsoleVtEnc *e, bool visible)
{
    emit_cstr(e, visible ? "\x1b[?25h" : "\x1b[?25l");
}

/* SetMode has no VT representation in this encoder (the remote terminal's size
   is driven by the consumer's resize, not the guest's mode change). */
static void
vt_set_mode(void *user, uint32_t mode)
{
    (void)user;
    (void)mode;
}

/* The producer reshaped. Keep the late-join screen model at the producer's
   RESOLVED geometry -- a snapshot repaints into this extent, so a stale value
   would repaint a joining client at the old size. Nothing goes on the wire: the
   remote terminal's own size is the consumer's business, not the guest's. */
static void
vt_resize(void *user, uint32_t cols, uint32_t rows)
{
    axl_console_vt_enc_set_size((AxlConsoleVtEnc *)user, cols, rows);
}

static void
vt_emit_alt_screen(AxlConsoleVtEnc *e, bool enter)
{
    emit_cstr(e, enter ? "\x1b[?1049h" : "\x1b[?1049l");
}

/* One dispatcher for every terminal property the VT wire can carry. Accept-and-
   ignore anything else: the wire has no representation for the other props today. */
static int
vt_set_term_prop(void *user, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    AxlConsoleVtEnc *e = (AxlConsoleVtEnc *)user;
    switch (prop) {
    case AXL_CONSOLE_PROP_CURSOR_VISIBLE:
        vt_emit_dectcem(e, val->u.boolean);
        return 1;
    case AXL_CONSOLE_PROP_ALT_SCREEN:
        vt_emit_alt_screen(e, val->u.boolean);
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
    .resize        = vt_resize,
    .set_term_prop = vt_set_term_prop,
    /* set_cell_rule: this encoder re-encodes to a VT wire and never rasterizes, so
       cell width is the far-end terminal's problem. Deliberately unbound. */
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlConsoleVtEnc *
axl_console_vt_enc_new(const AxlConsoleVtEncConfig *cfg)
{
    if (cfg == NULL || cfg->sink == NULL) {
        return NULL;
    }
    AxlConsoleVtEnc *e = axl_calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->sink    = cfg->sink;
    e->user    = cfg->user;
    e->cur_row = -1;
    e->cur_col = -1;
    e->screen  = axl_console_screen_new(cfg->rows != 0 ? cfg->rows : VT_ENC_DEFAULT_ROWS,
                                        cfg->cols != 0 ? cfg->cols : VT_ENC_DEFAULT_COLS);
    if (e->screen == NULL) {
        axl_free(e);
        return NULL;
    }
    /* Opt-in live-stream coalescing. guard=true: emit() appends from the
       producer's TPL while a resident consumer flushes from a raised-TPL tick.
       On init failure the buffer stays NULL and emit() falls back to the direct
       sink (uncoalesced but correct), so coalesce is left off. */
    if (cfg->coalesce) {
        e->coalesce = axl_console_vt_buf_init(&e->outbuf, e->sink, e->user,
                                              VT_ENC_COALESCE_CAP, /*guard=*/true);
    }
    return e;
}

void
axl_console_vt_enc_free(AxlConsoleVtEnc *e)
{
    if (e == NULL) {
        return;
    }
    /* Deliver any last buffered bytes before teardown, then release the buffers. */
    axl_console_vt_buf_flush(&e->outbuf);
    axl_console_vt_buf_dispose(&e->outbuf);
    axl_console_screen_free(e->screen);
    axl_free(e);
}

const AxlConsoleOps *
axl_console_vt_enc_ops(AxlConsoleVtEnc *e, void **user)
{
    if (e == NULL) {
        return NULL;
    }
    if (user != NULL) {
        *user = e;
    }
    return &vt_ops;
}

int
axl_console_vt_enc_snapshot(AxlConsoleVtEnc *e, AxlConsoleScreenSink sink, void *user)
{
    if (e == NULL || sink == NULL) {
        return AXL_ERR;
    }
    return axl_console_screen_snapshot(e->screen, sink, user);
}

void
axl_console_vt_enc_flush(AxlConsoleVtEnc *e)
{
    if (e == NULL) {
        return;
    }
    /* NULL-safe / no-op when coalesce is off (outbuf.buf is NULL). */
    axl_console_vt_buf_flush(&e->outbuf);
}

void
axl_console_vt_enc_set_size(AxlConsoleVtEnc *e, uint32_t cols, uint32_t rows)
{
    /* Both axes required: axl_console_screen_resize takes a whole geometry, and a
       partial resize would silently pin the other axis to a stale value. Producers
       resolve a configured 0 to the physical size before calling here. */
    if (e != NULL && cols != 0 && rows != 0) {
        /* On failure (OOM) the model keeps its OLD extent while the producer moves
           to the new one, so a late-join snapshot repaints at the wrong size. The
           call cannot report that upward (this is void by contract), so say it
           here rather than leave a silent visual bug. */
        if (axl_console_screen_resize(e->screen, rows, cols) != AXL_OK) {
            axl_warning("console vt-enc: screen model resize to %ux%u failed; "
                        "late-join snapshots will repaint at the old size",
                        cols, rows);
        }
    }
}

void
axl_console_vt_enc_reset(AxlConsoleVtEnc *e)
{
    if (e != NULL) {
        e->cur_row = -1;   /* forget the emitted-cursor dedup baseline */
        e->cur_col = -1;
    }
}

// ---------------------------------------------------------------------------
// Internal seam for axl-console-mirror (no public header). The mirror is
// tap+encoder and must keep exposing the screen model to its own test seam.
// ---------------------------------------------------------------------------

AxlConsoleScreen *
_axl_console_vt_enc_screen(AxlConsoleVtEnc *e)
{
    return (e != NULL) ? e->screen : NULL;
}
