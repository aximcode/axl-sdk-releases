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
    its cursor, which would otherwise flood the wire with escapes). The dedup is
    only as good as the tracked cursor, so the encoder follows the TERMINAL's
    cursor through every op, text included, and keeps the terminal in step with a
    UEFI console at the right margin (see vt_output_text).

    The op bodies are the mirror's, moved verbatim — the emitted byte stream is
    load-bearing (goldens, and every already-deployed consumer), so the refactor is
    a move, not a rewrite. See docs/AXL-Console-Mirror-Design.md.
**/

#include <axl/axl-console-vt-enc.h>
#include <axl/axl-console-screen.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-vterm.h>        /* axl_vterm_char_width: the terminal's cell width */
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

    /* The far terminal's cursor after everything emitted so far: set by a CUP or
       a clear, advanced by text (vt_output_text). A move to it is suppressed.
       -1 = unknown, after which the next move is always emitted. */
    int32_t            cur_row;
    int32_t            cur_col;

    /* The console's geometry: where text wraps and where a line feed scrolls. */
    uint32_t           cols;
    uint32_t           rows;

    /* The producer's cell rule (set_cell_rule): false until told otherwise,
       which is AxlConsoleOps's default, ONE_PER_CODEPOINT. */
    bool               width_resolved;

    /* The last SGR emitted: the pen the console is writing with. A repaint from
       the screen model does not always end on it (an SGR with no text after it
       yet ends the repaint on the default pen), so a repaint -- late join, a 1049
       switch -- re-emits it last. */
    char               sgr[64];
    size_t             sgr_len;
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
    e->cur_row = 0;   /* the terminal cursor is home; tracking starts from here */
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
vt_cursor_unknown(AxlConsoleVtEnc *e)
{
    e->cur_row = -1;
    e->cur_col = -1;
}

/* One UTF-8 sequence at @p s (at most @p avail bytes; @p s[0] >= 0xC0).
   Well-formed: its length, with the codepoint in *cp. Not well-formed -- a
   lead byte no codepoint has (0xC0, 0xC1, 0xF5..0xFF), an overlong form, an
   encoded surrogate, a value past U+10FFFF, or cut short: 0. Either way
   *span is how many bytes it occupies, the lead and the continuation bytes
   that follow it (up to the length the lead announces), which is what a
   malformed one is replaced as. Bounded by @p avail -- output_text's text is
   a counted run, not a NUL-terminated string. */
static size_t
utf8_seq(const char *s, size_t avail, uint32_t *cp, size_t *span)
{
    static const uint32_t min[5] = { 0, 0, 0x80u, 0x800u, 0x10000u };
    uint8_t  b  = (uint8_t)s[0];
    size_t   n  = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
    uint32_t v  = b & (n == 4 ? 0x07u : n == 3 ? 0x0Fu : 0x1Fu);
    size_t   k  = 1;

    for (; k < n && k < avail; k++) {
        uint8_t t = (uint8_t)s[k];
        if ((t & 0xC0) != 0x80) {
            break;
        }
        v = (v << 6) | (t & 0x3Fu);
    }
    *span = k;
    /* 0xF5..0xF7 decode past U+10FFFF on their own; 0xF8..0xFF would decode as
       0xF0..0xF7 under the 4-byte mask, so they are refused by name. */
    if (b >= 0xF8 || k < n || v < min[n] || v > 0x10FFFFu
        || (v >= 0xD800u && v <= 0xDFFFu)) {
        return 0;
    }
    *cp = v;
    return n;
}

/* Text, with the far terminal's cursor followed through it.

   One cell per codepoint (the SIMPLE_TEXT_OUTPUT producers report
   CELLS_ONE_PER_CODEPOINT); CR homes the column, LF moves down and scrolls on the
   last row, BS steps left and stops at column 0 -- the rules the producer's own
   Mode cursor follows (axl-console-emit.c track_cursor), which is what the Shell
   positions against.

   The right margin is a UEFI console's: the cell in the last column wraps AT
   ONCE (EDK2 GraphicsConsole outputs CR LF right there, and TerminalDxe's TtyTerm
   does the same to a serial terminal). A VT terminal instead defers the wrap until
   the next printable, and a move clears the deferral -- so a console that has
   already scrolled and a terminal that has not would disagree on every row after.
   Writing CR LF here keeps the two in step, and it is also what makes a terminal
   WIDER than the console wrap at the console's edge rather than its own.

   A TAB is one cell on a UEFI console (GraphicsConsole draws it as a glyph; no
   tab stops), so it goes out as one space. Any other control byte moves a
   terminal by rules the console does not model, so it makes the position
   unknown rather than wrong.

   How many cells a codepoint takes is the producer's cell rule. Under
   ONE_PER_CODEPOINT (the device and the tap: a UEFI console) every codepoint
   is ONE cell: GraphicsConsole advances a column per character unless the
   string switches to wide (WIDE_CHAR), which the Shell never does. A codepoint
   a terminal draws in two cells (CJK) or in none (a combining mark) would
   slide the terminal's grid off the console's, so it goes out as one '?' --
   what TerminalDxe's own non-UTF-8 terminals are sent for a character they
   cannot show. Under WIDTH_RESOLVED (axl-vterm) the producer has laid its
   glyphs out by axl_vterm_char_width already: the glyph goes out as it is and
   the tracked cursor moves by that width. A C1 control is a control, not a
   glyph, under either rule: it goes out as one '?', never raw (U+009B is an
   8-bit CSI to a terminal that honours C1). */
static void
vt_output_text(void *user, const char *utf8, size_t len)
{
    AxlConsoleVtEnc *e    = (AxlConsoleVtEnc *)user;
    size_t           from = 0;   /* start of the run not yet emitted */
    size_t           seq_end = 0;   /* end of the well-formed sequence in progress */
    for (size_t i = 0; i < len; i++) {
        uint8_t  c     = (uint8_t)utf8[i];
        uint32_t cells = 1;   /* what this codepoint moves the terminal by */
        if (i < seq_end) {
            continue;   /* a continuation byte of a codepoint already counted */
        }
        if (c >= 0x80 && c < 0xC0) {
            /* A continuation byte with no lead: one '?', one cell. */
            emit(e, utf8 + from, i - from);
            emit_cstr(e, "?");
            from = i + 1;
            c    = '?';
        } else if (c == '\t') {   /* tracked or not, the terminal gets a space */
            emit(e, utf8 + from, i - from);
            emit_cstr(e, " ");
            from = i + 1;
        } else if (c >= 0xC0) {
            uint32_t cp   = 0;
            size_t   span = 1;
            size_t   n    = utf8_seq(utf8 + i, len - i, &cp, &span);
            int      w    = n > 0 ? axl_vterm_char_width(cp) : 1;
            bool     c1   = cp >= 0x80 && cp <= 0x9F;
            if (n == 0 || c1 || (!e->width_resolved && w != 1)) {
                /* Not well-formed, a C1 control, or not one cell on this
                   producer's console: one '?' for the whole sequence. */
                emit(e, utf8 + from, i - from);
                emit_cstr(e, "?");
                from = i + span;
                i += span - 1;   /* tracked below as the one cell it is */
            } else {
                seq_end = i + n;
                if (e->width_resolved) {
                    cells = (uint32_t)w;
                }
            }
        }
        if (e->cur_row < 0) {
            continue;   /* unknown stays unknown until a CUP or a clear */
        }
        if (c == '\r') {
            e->cur_col = 0;
        } else if (c == '\n') {
            if ((uint32_t)e->cur_row + 1 < e->rows) {
                e->cur_row++;
            }
        } else if (c == '\b') {
            if (e->cur_col > 0) {
                e->cur_col--;
            }
        } else if (c == 0) {
            /* NUL: a terminal ignores it; nothing moves */
        } else if (c != '\t' && (c < 0x20 || c == 0x7F)) {
            vt_cursor_unknown(e);
        } else if ((c & 0xC0) != 0x80) {   /* a codepoint's first byte (or the TAB) */
            if (cells == 2 && e->cur_col > 0 && (uint32_t)e->cur_col + 2 > e->cols
                && (uint32_t)e->cur_col < e->cols) {
                /* A wide glyph that does not fit wraps before it (xterm,
                   libvterm); the cell it could not use stays blank. */
                emit(e, utf8 + from, i - from);
                emit_cstr(e, "\r\n");
                from       = i;
                e->cur_col = 0;
                if ((uint32_t)e->cur_row + 1 < e->rows) {
                    e->cur_row++;
                }
            }
            e->cur_col += (int32_t)cells;
            if ((uint32_t)e->cur_col >= e->cols) {
                /* Emit through the end of this codepoint, then the wrap. */
                size_t end = seq_end > i ? seq_end : i + 1;
                emit(e, utf8 + from, end - from);
                emit_cstr(e, "\r\n");
                from       = end;
                i          = end - 1;
                e->cur_col = 0;
                if ((uint32_t)e->cur_row + 1 < e->rows) {
                    e->cur_row++;
                }
            }
        }
    }
    emit(e, utf8 + from, len - from);
}

/* Emit an SGR and remember it as the console's pen (see AxlConsoleVtEnc.sgr). */
static void
vt_emit_sgr(AxlConsoleVtEnc *e, const char *sgr, size_t len)
{
    if (len < sizeof(e->sgr)) {
        axl_memcpy(e->sgr, sgr, len);
        e->sgr_len = len;
    }
    emit(e, sgr, len);   /* emit() feeds the model even when sink is NULL */
}

/* A UEFI colour index (EFI_BLACK..EFI_WHITE) -> the ANSI colour index that means
   the same colour. UEFI orders blue before red and is BGR-ish; ANSI orders red
   first. 8..15 are the bright set in both: EFI_YELLOW (14) = EFI_BROWN |
   EFI_BRIGHT is ANSI 11, bright yellow. */
static const uint8_t uefi_to_ansi[16] = {
    0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15
};

/* UEFI-indexed pen -> SGR, through the ONE pen encoder the repaint also uses
   (axl_console_pen_to_sgr): the 16 colours go out as 30-37/90-97 and 40-47, so
   a cell reaches a late joiner in the SGR the live stream used. EDK2's
   TerminalDxe writes bright colours as "1;3x" instead; that is bright only on a
   terminal that draws bold as bright, which VTE-based terminals, for one, do not
   by default -- so EFI_YELLOW read as brown. 90-97 is bright everywhere. */
static void
vt_emit_indexed_sgr(AxlConsoleVtEnc *e, uint8_t fg, uint8_t bg)
{
    AxlConsolePen pen = {
        .fg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = uefi_to_ansi[fg & 0x0F] },
        .bg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = uefi_to_ansi[bg & 0x07] },
    };
    char   buf[64];
    size_t n = axl_console_pen_to_sgr(buf, sizeof(buf), &pen);
    vt_emit_sgr(e, buf, n);
}

/* Full SGR for a general pen (default / indexed / truecolour + style bits), via the
   encoder shared with axl-console-screen. Reachable only from axl-vterm — the tap
   always produces a both-indexed pen and takes the fast path in vt_set_pen. */
static void
vt_emit_full_sgr(AxlConsoleVtEnc *e, const AxlConsolePen *pen)
{
    char   buf[64];
    size_t n = axl_console_pen_to_sgr(buf, sizeof(buf), pen);
    vt_emit_sgr(e, buf, n);
}

/* The pen snapshot -> SGR. A both-indexed pen is a SIMPLE_TEXT_OUTPUT producer's
   UEFI attribute (the tap and the device produce nothing else), so its indices are
   UEFI's and are mapped; any other pen comes from axl-vterm and is already ANSI. */
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
   is driven by the consumer's resize, not the guest's mode change). It does home
   the console's cursor behind the terminal's back, so the position is unknown. */
static void
vt_set_mode(void *user, uint32_t mode)
{
    (void)mode;
    vt_cursor_unknown((AxlConsoleVtEnc *)user);
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

typedef struct {
    AxlString *buf;
    bool       failed;
} VtCapture;

static void
vt_capture_sink(const char *bytes, size_t len, void *user)
{
    VtCapture *c = (VtCapture *)user;
    if (axl_string_append_len(c->buf, bytes, len) != AXL_OK) {
        c->failed = true;
    }
}

/* DECSET/DECRST 1049, carrying the visible screen across the switch.

   The alternate screen exists to keep a full-screen app out of the terminal's
   scrollback. It must not change what the terminal SHOWS: a UEFI console has one
   screen, and the GOP goes on showing whatever was drawn last. A bare switch does
   change it. Entering shows a blank screen -- and a SIMPLE_TEXT_OUTPUT producer
   only recognises a full-screen app after its first paint (a backward cursor jump
   after a clear), so that paint is lost: the editor's title bar. Leaving brings
   back the screen as it was before the app, cursor included, while the console
   shows the app's last frame. So the screen on show is serialized from the model
   BEFORE the switch (the model follows our own bytes) and written after it.

   Out of memory, the switch goes out bare: a wrong screen, not a lost switch. */
static void
vt_emit_alt_screen(AxlConsoleVtEnc *e, bool enter)
{
    VtCapture shown = { .buf = axl_string_new(NULL), .failed = false };
    if (shown.buf != NULL
        && (_axl_console_screen_snapshot_active(e->screen, vt_capture_sink, &shown) != AXL_OK
            || shown.failed)) {
        axl_string_free(shown.buf);
        shown.buf = NULL;
    }
    emit_cstr(e, enter ? "\x1b[?1049h" : "\x1b[?1049l");
    if (shown.buf != NULL) {
        emit(e, axl_string_str(shown.buf), axl_string_len(shown.buf));
        axl_string_free(shown.buf);
        emit(e, e->sgr, e->sgr_len);   /* back to the console's pen */
    }
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

/* The producer's cell rule decides what vt_output_text does with a codepoint
   that is not one terminal cell. */
static void
vt_set_cell_rule(void *user, AxlConsoleCellRule rule)
{
    AxlConsoleVtEnc *e = (AxlConsoleVtEnc *)user;
    e->width_resolved = rule == AXL_CONSOLE_CELLS_WIDTH_RESOLVED;
}

static const AxlConsoleOps vt_ops = {
    .set_cell_rule = vt_set_cell_rule,
    .clear_screen  = vt_clear_screen,
    .set_cursor    = vt_set_cursor,
    .output_text   = vt_output_text,
    .set_pen       = vt_set_pen,
    .set_mode      = vt_set_mode,
    .resize        = vt_resize,
    .set_term_prop = vt_set_term_prop,
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
    e->cols    = cfg->cols != 0 ? cfg->cols : VT_ENC_DEFAULT_COLS;
    e->rows    = cfg->rows != 0 ? cfg->rows : VT_ENC_DEFAULT_ROWS;
    e->screen  = axl_console_screen_new(e->rows, e->cols);
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
    if (axl_console_screen_snapshot(e->screen, sink, user) != AXL_OK) {
        return AXL_ERR;
    }
    if (e->sgr_len > 0) {
        sink(e->sgr, e->sgr_len, user);   /* the console's pen, not the last cell's */
    }
    return AXL_OK;
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
        e->cols = cols;   /* where text wraps from now on */
        e->rows = rows;
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
        vt_cursor_unknown(e);   /* forget the dedup baseline */
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
