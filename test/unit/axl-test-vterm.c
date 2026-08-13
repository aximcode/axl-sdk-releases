/** @file axl-test-vterm.c
    Infra proof for the vendored libvterm (deps/libvterm), Layer 2 only.

    This does NOT test an AXL public API — `axl-vterm` does not exist yet. It
    proves the vendored library actually *runs* inside a freestanding UEFI
    image on every arch we ship: that our patched allocator hands back zeroed
    memory, that `snprintf`/`vsnprintf` really route into AXL's format engine
    when libvterm formats a query reply, and that binding
    `VTermStateCallbacks` (without Layer 3's `screen.c`) yields the positioned
    op stream `AxlConsoleOps` will be widened to carry.

    Linking is not the interesting part; the Makefile already proves that. What
    these assertions pin is *runtime* behaviour that a link check cannot see.

    See deps/libvterm/README.md for the eight local patches. `test_decscusr_query`
    below is the regression guard for patch 6.
**/

#include "axl-test.h"

#include <axl/axl-str.h>
#include <axl/axl-vterm.h>
#include <axl/axl-console-ops.h>
#include <axl/axl-console-screen.h>
#include <axl/axl-console-vt-enc.h>
#include <vterm.h>

// ---------------------------------------------------------------------------
// A minimal Layer-2 sink: records the ops libvterm emits.
// ---------------------------------------------------------------------------

#define MAX_GLYPHS 64

typedef struct {
    // putglyph
    struct {
        uint32_t cp;      // first codepoint of the glyph
        int      width;
        int      row;
        int      col;
    } glyphs[MAX_GLYPHS];
    int      nglyphs;

    // settermprop, narrowed to ALTSCREEN: DECSET/DECRST 1049 also runs
    // savecursor(), which emits CURSORVISIBLE/CURSORBLINK/CURSORSHAPE after
    // the ALTSCREEN prop -- so "the last prop" is not the one under test.
    int      n_altscreen;
    int      last_altscreen;

    // erase
    int      nerase;
    VTermRect last_erase;

    // movecursor
    VTermPos last_cursor;

    // setpenattr
    int      npenattr;
    VTermAttr last_attr;
    int      last_attr_fg_idx;   // valid when last_attr == VTERM_ATTR_FOREGROUND
} Sink;

static int
sink_putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
    Sink *s = user;
    if (s->nglyphs < MAX_GLYPHS) {
        s->glyphs[s->nglyphs].cp    = info->chars[0];
        s->glyphs[s->nglyphs].width = info->width;
        s->glyphs[s->nglyphs].row   = pos.row;
        s->glyphs[s->nglyphs].col   = pos.col;
        s->nglyphs++;
    }
    return 1;
}

static int
sink_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    Sink *s = user;
    (void)oldpos;
    (void)visible;
    s->last_cursor = pos;
    return 1;
}

static int
sink_erase(VTermRect rect, int selective, void *user)
{
    Sink *s = user;
    (void)selective;
    s->nerase++;
    s->last_erase = rect;
    return 1;
}

static int
sink_setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
    Sink *s = user;
    s->npenattr++;
    s->last_attr = attr;
    if (attr == VTERM_ATTR_FOREGROUND && VTERM_COLOR_IS_INDEXED(&val->color)) {
        s->last_attr_fg_idx = val->color.indexed.idx;
    }
    return 1;
}

static int
sink_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    Sink *s = user;
    if (prop == VTERM_PROP_ALTSCREEN) {
        s->n_altscreen++;
        s->last_altscreen = val->boolean;
    }
    return 1;
}

/* scrollrect deliberately absent (NULL): that is what makes libvterm
 * decompose a scroll into moverect + erase via vterm_scroll_rect(). */
static const VTermStateCallbacks sink_cbs = {
    .putglyph    = sink_putglyph,
    .movecursor  = sink_movecursor,
    .erase       = sink_erase,
    .setpenattr  = sink_setpenattr,
    .settermprop = sink_settermprop,
};

/* `reset` mirrors the caller's choice, because it is load-bearing:
 * vterm_obtain_state() does NOT reset, so until vterm_state_reset() runs (or
 * the byte stream carries RIS / DECSTR) the state's mode fields hold whatever
 * the zeroed allocation left. Nearly all consumers reset; the reset-less path
 * is what test_decscusr_query_before_reset exercises. */
static VTerm *
vt_open_ex(Sink *sink, int rows, int cols, bool reset)
{
    VTerm *vt = vterm_new(rows, cols);
    if (vt == NULL) {
        return NULL;
    }
    vterm_set_utf8(vt, 1);

    VTermState *state = vterm_obtain_state(vt);
    vterm_state_set_callbacks(state, &sink_cbs, sink);
    if (reset) {
        vterm_state_reset(state, 1);
    }
    return vt;
}

static VTerm *
vt_open(Sink *sink, int rows, int cols)
{
    return vt_open_ex(sink, rows, cols, true);
}

static void
vt_feed(VTerm *vt, const char *s)
{
    vterm_input_write(vt, s, axl_strlen(s));
}

// ---------------------------------------------------------------------------
// The allocator patch: vterm_new() must work without libc malloc, and must
// hand back zeroed memory (upstream did malloc + memset(0); we use axl_calloc).
// ---------------------------------------------------------------------------

static void
test_vterm_new_uses_axl_heap(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);

    test_check(vt != NULL, "vterm: vterm_new() allocates via the AXL heap");
    if (vt == NULL) {
        return;
    }

    int rows = 0, cols = 0;
    vterm_get_size(vt, &rows, &cols);
    test_check(rows == 25, "vterm: constructed with 25 rows");
    test_check(cols == 80, "vterm: constructed with 80 cols");

    vterm_free(vt);
}

// ---------------------------------------------------------------------------
// Layer 2 emits positioned, width-bearing glyphs.
// ---------------------------------------------------------------------------

static void
test_putglyph_positions_and_width(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: putglyph fixture allocated");
        return;
    }

    vt_feed(vt, "Hi");

    test_check(sink.nglyphs == 2, "vterm: 'Hi' emits exactly 2 putglyph ops");
    test_check(sink.glyphs[0].cp == 'H', "vterm: glyph 0 is 'H'");
    test_check(sink.glyphs[0].row == 0 && sink.glyphs[0].col == 0,
               "vterm: glyph 0 lands at (0,0)");
    test_check(sink.glyphs[0].width == 1, "vterm: 'H' is one cell wide");
    test_check(sink.glyphs[1].cp == 'i', "vterm: glyph 1 is 'i'");
    test_check(sink.glyphs[1].row == 0 && sink.glyphs[1].col == 1,
               "vterm: glyph 1 lands at (0,1)");

    vterm_free(vt);
}

/* The width field is the whole reason a VT producer can carry glyph width
 * where SIMPLE_TEXT_OUTPUT cannot: a CJK codepoint occupies two cells. This
 * also proves the vendored fullwidth.inc table is linked and correct. */
static void
test_putglyph_reports_double_width(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: wide-glyph fixture allocated");
        return;
    }

    vt_feed(vt, "\xe4\xb8\xad");   /* U+4E2D, CJK ideograph */

    test_check(sink.nglyphs == 1, "vterm: one CJK codepoint emits 1 putglyph");
    test_check(sink.glyphs[0].cp == 0x4E2D, "vterm: decodes UTF-8 to U+4E2D");
    test_check(sink.glyphs[0].width == 2, "vterm: U+4E2D reports width 2");

    vterm_free(vt);
}

// ---------------------------------------------------------------------------
// CSI parsing reaches the state layer: erase, pen, cursor, termprop.
// ---------------------------------------------------------------------------

static void
test_csi_erase_and_cursor(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: CSI fixture allocated");
        return;
    }

    vt_feed(vt, "\x1b[2J");        /* ED 2: erase whole screen */
    test_check(sink.nerase >= 1, "vterm: CSI 2J emits erase");
    test_check(sink.last_erase.start_row == 0 && sink.last_erase.end_row == 25,
               "vterm: CSI 2J erases all 25 rows");
    test_check(sink.last_erase.start_col == 0 && sink.last_erase.end_col == 80,
               "vterm: CSI 2J erases all 80 cols");

    vt_feed(vt, "\x1b[5;10H");     /* CUP row 5 col 10, 1-based on the wire */
    test_check(sink.last_cursor.row == 4 && sink.last_cursor.col == 9,
               "vterm: CSI 5;10H moves cursor to 0-based (4,9)");

    vterm_free(vt);
}

static void
test_sgr_sets_indexed_foreground(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: SGR fixture allocated");
        return;
    }

    sink.last_attr_fg_idx = -1;
    vt_feed(vt, "\x1b[31m");       /* SGR 31: foreground red (index 1) */

    test_check(sink.npenattr >= 1, "vterm: SGR 31 emits setpenattr");
    test_check(sink.last_attr == VTERM_ATTR_FOREGROUND,
               "vterm: SGR 31 sets VTERM_ATTR_FOREGROUND");
    test_check(sink.last_attr_fg_idx == 1,
               "vterm: SGR 31 yields indexed colour 1");

    vterm_free(vt);
}

static void
test_altscreen_is_a_termprop(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: altscreen fixture allocated");
        return;
    }

    /* Layer 2 reports the alternate screen as a termprop, not a callback.
     * Without screen.c there is no alt buffer -- only the notification. */
    sink.n_altscreen = 0;
    vt_feed(vt, "\x1b[?1049h");
    test_check(sink.n_altscreen == 1,
               "vterm: DECSET 1049 emits exactly one ALTSCREEN termprop");
    test_check(sink.last_altscreen == 1, "vterm: DECSET 1049 enters alt screen");

    vt_feed(vt, "\x1b[?1049l");
    test_check(sink.n_altscreen == 2,
               "vterm: DECRST 1049 emits a second ALTSCREEN termprop");
    test_check(sink.last_altscreen == 0, "vterm: DECRST 1049 leaves alt screen");

    vterm_free(vt);
}

// ---------------------------------------------------------------------------
// Patch 8 (SCOSC / SCORC): CSI s / CSI u save & restore the cursor.
//
// xterm binds CSI s to SCOSC (save cursor) when left/right-margin mode is off,
// and to DECSLRM (set margins) when it is on.  Upstream libvterm binds CSI s
// unconditionally to DECSLRM and has no CSI u handler at all, so the ANSI.SYS
// save/restore pair -- widely emitted by shell prompts and TUIs -- was lost.
// ---------------------------------------------------------------------------

static void
test_scosc_scorc_saves_and_restores_cursor(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: SCOSC/SCORC fixture allocated");
        return;
    }

    vt_feed(vt, "\x1b" "[4;5H");   /* CUP to 0-based (3,4) */
    test_check(sink.last_cursor.row == 3 && sink.last_cursor.col == 4,
               "vterm: cursor parked at (3,4) before save");

    vt_feed(vt, "\x1b" "[s");      /* SCOSC (margin mode off): save cursor */
    vt_feed(vt, "\x1b" "[1;1H");   /* move away to home */
    test_check(sink.last_cursor.row == 0 && sink.last_cursor.col == 0,
               "vterm: cursor moved to home after save");

    vt_feed(vt, "\x1b" "[u");      /* SCORC: restore the saved cursor */
    test_check(sink.last_cursor.row == 3 && sink.last_cursor.col == 4,
               "vterm: CSI u restores the cursor saved by CSI s");

    vterm_free(vt);
}

static void
test_scosc_under_leftright_margin_is_decslrm(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: DECSLRM fixture allocated");
        return;
    }

    vt_feed(vt, "\x1b" "[3;3H");   /* CUP to (2,2) */
    vt_feed(vt, "\x1b" "[s");      /* SCOSC (margins off): save (2,2) */

    vt_feed(vt, "\x1b" "[?69h");   /* DECLRMM: enable left/right margin mode */
    vt_feed(vt, "\x1b" "[6;6H");   /* CUP to (5,5) */
    vt_feed(vt, "\x1b" "[s");      /* now DECSLRM: sets margins, must NOT save */

    vt_feed(vt, "\x1b" "[8;8H");   /* CUP to (7,7) */
    vt_feed(vt, "\x1b" "[u");      /* SCORC: restore -> (2,2), not (5,5) */
    test_check(sink.last_cursor.row == 2 && sink.last_cursor.col == 2,
               "vterm: CSI s under DECLRMM is DECSLRM, so the save slot is unchanged");

    vterm_free(vt);
}

// ---------------------------------------------------------------------------
// Patch 6 regression: the DECSCUSR query reply.
//
// vterm_state_new() zeroes mode.cursor_shape and vterm_obtain_state() does not
// reset, so before vterm_state_reset() the shape is 0 while the enum starts at
// VTERM_PROP_CURSORSHAPE_BLOCK == 1. Upstream's switch then matched no arm and
// transmitted an uninitialized stack int. Patch 6 seeds it with 2.
//
// Pin the exact reply bytes -- a substring check would happily pass on leaked
// garbage. Formatting the reply runs through libvterm's snprintf, which our
// compat shim maps to axl_snprintf, so these double as proof of that mapping.
// ---------------------------------------------------------------------------

/* DCS $ q SP q ST -- "what is the current cursor style?" */
#define DECSCUSR_QUERY "\x1bP$q q\x1b\\"

static void
decscusr_reply(VTerm *vt, char *buf, size_t size)
{
    vt_feed(vt, DECSCUSR_QUERY);
    size_t n = vterm_output_read(vt, buf, size - 1);
    buf[n] = '\0';
}

/* The vulnerable window: a state that has never been reset. This is the only
 * configuration in which patch 6's seed is observable, so it is the only
 * assertion that actually guards the fix. */
static void
test_decscusr_query_before_reset(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open_ex(&sink, 25, 80, false);
    if (vt == NULL) {
        test_check(false, "vterm: DECSCUSR pre-reset fixture allocated");
        return;
    }

    char buf[64];
    decscusr_reply(vt, buf, sizeof(buf));

    /* shape 0 (unset) hits no arm -> seeded 2; blink 0 (unset) -> no decrement.
     * DCS 1 $ r 2 SP q ST == "steady block". Without patch 6 this byte is
     * whatever was on the stack. */
    test_check(axl_strcmp(buf, "\x1bP1$r2 q\x1b\\") == 0,
               "vterm: DECSCUSR query before reset replies steady block (not stack garbage)");

    vterm_free(vt);
}

/* After a reset the shape is BLOCK and blink is on, so the reply decrements to
 * 1. Guards the seed from being "right for the wrong reason". */
static void
test_decscusr_query_after_reset(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: DECSCUSR post-reset fixture allocated");
        return;
    }

    char buf[64];
    decscusr_reply(vt, buf, sizeof(buf));

    test_check(axl_strcmp(buf, "\x1bP1$r1 q\x1b\\") == 0,
               "vterm: DECSCUSR query after reset replies blinking block");

    vterm_free(vt);
}

/* Once the shape IS set, the reply must track it -- otherwise a hardcoded
 * constant would pass the tests above for the wrong reason. */
static void
test_decscusr_query_tracks_shape(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 25, 80);
    if (vt == NULL) {
        test_check(false, "vterm: DECSCUSR shape fixture allocated");
        return;
    }

    vt_feed(vt, "\x1b[4 q");            /* DECSCUSR 4: steady underline */

    char buf[64];
    decscusr_reply(vt, buf, sizeof(buf));

    test_check(axl_strcmp(buf, "\x1bP1$r4 q\x1b\\") == 0,
               "vterm: DECSCUSR query reports steady underline once set");

    vterm_free(vt);
}

// ---------------------------------------------------------------------------
// The int-return "handled?" protocol: a consumer with no scrollrect gets the
// scroll decomposed into ordinary damage. This is the fallback the widened
// AxlConsoleOps must preserve, so pin it against the vendored vterm.c.
// ---------------------------------------------------------------------------

static void
test_declined_scrollrect_decomposes_to_erase(void)
{
    Sink   sink = {0};
    VTerm *vt   = vt_open(&sink, 4, 10);
    if (vt == NULL) {
        test_check(false, "vterm: scroll fixture allocated");
        return;
    }

    /* Park on the last row, then LF to force a scroll. sink_cbs has no
     * scrollrect, so vterm_scroll_rect() must fall back to moverect+erase;
     * moverect is absent too, leaving erase of the vacated last row. */
    vt_feed(vt, "\x1b[4;1H");
    sink.nerase = 0;
    vt_feed(vt, "\n");

    test_check(sink.nerase >= 1,
               "vterm: a declined scrollrect decomposes into erase damage");
    test_check(sink.last_erase.start_row == 3 && sink.last_erase.end_row == 4,
               "vterm: the scroll erases the vacated last row");

    vterm_free(vt);
}

// ===========================================================================
// axl-vterm: the second AxlConsoleOps producer, adapting libvterm Layer 2.
//
// A probe records the ops the adapter emits — as counters, the last snapshot of
// each op, and a '|'-separated transcript for order-sensitive assertions.
// ===========================================================================

#define PROBE_TEXT_MAX  256
#define PROBE_TRANS_MAX 512

typedef struct {
    // set_cell_rule
    int                rule_calls;
    AxlConsoleCellRule rule;

    // output_text (last run recorded, NUL-terminated)
    int    text_calls;
    char   text[PROBE_TEXT_MAX];

    // set_cursor
    int    cursor_calls;

    // set_pen (last snapshot)
    int           pen_calls;
    AxlConsolePen pen;

    // erase / moverect / scrollrect
    int            erase_calls;
    int            moverect_calls;
    AxlConsoleRect last_moverect_dest;
    AxlConsoleRect last_moverect_src;
    int            scrollrect_calls;

    // set_term_prop, narrowed to ALT_SCREEN
    int    alt_prop_calls;

    // clear_scrollback (CSI 3J -> libvterm sb_clear)
    int    clear_sb_calls;

    // order-sensitive op transcript, e.g. "text:ab|cursor:4,0|text:cd|"
    char   transcript[PROBE_TRANS_MAX];
} VtermProbe;

static void
probe_cat(VtermProbe *p, const char *s)
{
    size_t cur = axl_strlen(p->transcript);
    size_t add = axl_strlen(s);
    if (cur + add < sizeof p->transcript) {
        axl_memcpy(p->transcript + cur, s, add + 1);
    }
}

static void
probe_set_cell_rule(void *u, AxlConsoleCellRule rule)
{
    VtermProbe *p = u;
    p->rule_calls++;
    p->rule = rule;
}

static void
probe_output_text(void *u, const char *utf8, size_t len)
{
    VtermProbe *p = u;
    p->text_calls++;

    size_t n = len < PROBE_TEXT_MAX - 1 ? len : PROBE_TEXT_MAX - 1;
    axl_memcpy(p->text, utf8, n);
    p->text[n] = '\0';

    probe_cat(p, "text:");
    probe_cat(p, p->text);
    probe_cat(p, "|");
}

static void
probe_set_cursor(void *u, int32_t row, int32_t col)
{
    VtermProbe *p = u;
    p->cursor_calls++;

    char seg[32];
    axl_snprintf(seg, sizeof seg, "cursor:%d,%d|", (int)row, (int)col);
    probe_cat(p, seg);
}

static void
probe_set_pen(void *u, const AxlConsolePen *pen)
{
    VtermProbe *p = u;
    p->pen_calls++;
    p->pen = *pen;
    probe_cat(p, "pen|");   /* in the transcript so pen-before-paint ordering is pinned */
}

static void
probe_erase(void *u, AxlConsoleRect rect, bool selective)
{
    VtermProbe *p = u;
    (void)rect;
    (void)selective;
    p->erase_calls++;
}

static void
probe_moverect(void *u, AxlConsoleRect dest, AxlConsoleRect src)
{
    VtermProbe *p = u;
    p->moverect_calls++;
    p->last_moverect_dest = dest;
    p->last_moverect_src  = src;
}

static int
probe_scrollrect_decline(void *u, AxlConsoleRect rect, int32_t downward, int32_t rightward)
{
    VtermProbe *p = u;
    (void)rect;
    (void)downward;
    (void)rightward;
    p->scrollrect_calls++;
    return 0;   // decline: force libvterm to decompose into moverect + erase
}

static int
probe_scrollrect_accept(void *u, AxlConsoleRect rect, int32_t downward, int32_t rightward)
{
    VtermProbe *p = u;
    (void)rect;
    (void)downward;
    (void)rightward;
    p->scrollrect_calls++;
    return 1;   // accept: libvterm must NOT decompose
}

static int
probe_set_term_prop(void *u, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    VtermProbe *p = u;
    (void)val;
    if (prop == AXL_CONSOLE_PROP_ALT_SCREEN) {
        p->alt_prop_calls++;
    }
    return 1;
}

static int
probe_set_term_prop_reject_alt(void *u, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    VtermProbe *p = u;
    (void)val;
    if (prop == AXL_CONSOLE_PROP_ALT_SCREEN) {
        p->alt_prop_calls++;
        return 0;   // reject: libvterm must NOT latch the alt-screen state
    }
    return 1;
}

static void
probe_clear_scrollback(void *u)
{
    VtermProbe *p = u;
    p->clear_sb_calls++;
    probe_cat(p, "clearsb|");
}

/* The common sink: text/cursor/pen + erase, no blit ops (scrolls decompose). */
static const AxlConsoleOps probe_ops = {
    .set_cell_rule    = probe_set_cell_rule,
    .set_cursor       = probe_set_cursor,
    .output_text      = probe_output_text,
    .set_pen          = probe_set_pen,
    .erase            = probe_erase,
    .set_term_prop    = probe_set_term_prop,
    .clear_scrollback = probe_clear_scrollback,
};

/* Binds scrollrect (accepting) plus moverect + erase, so a suppressed
 * decomposition is observable as moverect_calls == 0. */
static const AxlConsoleOps probe_ops_blit = {
    .set_cell_rule = probe_set_cell_rule,
    .set_cursor    = probe_set_cursor,
    .output_text   = probe_output_text,
    .erase         = probe_erase,
    .moverect      = probe_moverect,
    .scrollrect    = probe_scrollrect_accept,
};

/* Binds scrollrect (declining) plus erase + moverect, so the decline decomposes
 * into an observable moverect + erase. */
static const AxlConsoleOps probe_ops_no_blit = {
    .set_cell_rule = probe_set_cell_rule,
    .set_cursor    = probe_set_cursor,
    .output_text   = probe_output_text,
    .erase         = probe_erase,
    .moverect      = probe_moverect,
    .scrollrect    = probe_scrollrect_decline,
};

/* Rejects the alt-screen property, to pin that our int return reaches
 * libvterm's settermprop gate. erase is bound because an ACCEPTED
 * ALTSCREEN=true erases the full screen (state.c:2256); binding it lets the
 * test observe that a REJECTED one never does. */
static const AxlConsoleOps probe_ops_reject_alt = {
    .set_cell_rule = probe_set_cell_rule,
    .set_cursor    = probe_set_cursor,
    .output_text   = probe_output_text,
    .erase         = probe_erase,
    .set_term_prop = probe_set_term_prop_reject_alt,
};

static void
test_vterm_reports_width_resolved_cell_rule(void)
{
    VtermProbe p = {0};
    AxlVterm *v = axl_vterm_new(25, 80, &probe_ops, &p);
    test_check(v != NULL, "new ok");
    test_check(p.rule_calls == 1, "set_cell_rule reported once");
    test_check(p.rule == AXL_CONSOLE_CELLS_WIDTH_RESOLVED, "axl-vterm is width-resolved");
    axl_vterm_free(v);
}

static void
test_vterm_char_width_zero_for_combining(void)
{
    test_check(axl_vterm_char_width('A')      == 1, "ASCII is one cell");
    test_check(axl_vterm_char_width(0x4E00)   == 2, "CJK is two cells");
    test_check(axl_vterm_char_width(0x0301)   == 0, "combining acute is zero cells");
    test_check(axl_vterm_char_width(0x200B)   == 0, "zero-width space is zero cells");
}

static void
test_vterm_coalesces_glyphs_into_one_run(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);
    axl_vterm_feed(v, "hello", 5);
    axl_vterm_flush(v);   /* if the adapter buffers, the API must expose a flush */

    test_check(p.text_calls == 1, "five glyphs coalesced into one output_text");
    test_check(axl_strcmp(p.text, "hello") == 0, "run bytes exact");
    axl_vterm_free(v);
}

static void
test_vterm_cursor_jump_flushes_the_run(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);
    axl_vterm_feed(v, "ab" "\x1b" "[5;1Hcd", 10);
    axl_vterm_flush(v);

    test_check(p.text_calls == 2, "the jump split the runs");
    test_check(axl_strcmp(p.transcript,
                          "text:ab|cursor:4,0|text:cd|") == 0, "exact op transcript");
    axl_vterm_free(v);
}

/* The adapter re-encodes libvterm's decoded codepoints back to UTF-8, and what
 * it re-encodes is NOT guaranteed to be a Unicode scalar value: libvterm's
 * decoder folds surrogates to U+FFFD but lets an over-long-range 4/5/6-byte
 * form through with its raw value (encoding.c only range-checks for overlongs).
 * "\xF4\x90\x80\x80" decodes to U+110000 -- one past the last scalar value --
 * and reaches putglyph. Re-encoding it verbatim would put ill-formed UTF-8 on
 * the wire for every downstream consumer of output_text. The contract is
 * U+FFFD, matching what libvterm itself substitutes for the malformed input it
 * DOES catch. */
static void
test_vterm_out_of_range_codepoint_becomes_replacement(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);
    axl_vterm_feed(v, "\xF4\x90\x80\x80", 4);   /* U+110000: above U+10FFFF */
    axl_vterm_flush(v);

    test_check(p.text_calls == 1, "over-range codepoint still emits one run");
    test_check(axl_strcmp(p.text, "\xEF\xBF\xBD") == 0,
               "U+110000 is re-encoded as U+FFFD, not as ill-formed UTF-8");
    axl_vterm_free(v);
}

/* Same defect through libvterm's 5-byte legacy form, whose value cannot fit a
 * 4-byte sequence at all: U+200000 would have written a 0xF8 lead byte plus
 * three continuation bytes -- not merely out of range, but structurally
 * impossible UTF-8. */
static void
test_vterm_five_byte_form_becomes_replacement(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);
    axl_vterm_feed(v, "\xF8\x88\x80\x80\x80", 5);   /* U+200000 */
    axl_vterm_flush(v);

    test_check(p.text_calls == 1, "5-byte legacy form still emits one run");
    test_check(axl_strcmp(p.text, "\xEF\xBF\xBD") == 0,
               "U+200000 is re-encoded as U+FFFD");
    axl_vterm_free(v);
}

/* The guard above must not over-reject: a legitimate astral-plane codepoint is
 * still a 4-byte sequence and must round-trip byte for byte. */
static void
test_vterm_astral_codepoint_round_trips(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);
    axl_vterm_feed(v, "\xF0\x9F\x98\x80", 4);   /* U+1F600 GRINNING FACE */
    axl_vterm_flush(v);

    test_check(p.text_calls == 1, "astral codepoint emits one run");
    test_check(axl_strcmp(p.text, "\xF0\x9F\x98\x80") == 0,
               "U+1F600 round-trips byte for byte");
    axl_vterm_free(v);
}

/* U+10FFFF is the LAST scalar value -- the boundary the over-range guard must
 * sit above, not on. Paired with the U+110000 case this pins the exact edge. */
static void
test_vterm_last_scalar_value_round_trips(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);
    axl_vterm_feed(v, "\xF4\x8F\xBF\xBF", 4);   /* U+10FFFF */
    axl_vterm_flush(v);

    test_check(p.text_calls == 1, "U+10FFFF emits one run");
    test_check(axl_strcmp(p.text, "\xF4\x8F\xBF\xBF") == 0,
               "U+10FFFF round-trips byte for byte");
    axl_vterm_free(v);
}

static void
test_vterm_sgr_truecolor_becomes_rgb_pen(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);
    axl_vterm_feed(v, "\x1b" "[38;2;10;20;30mX", 17);
    axl_vterm_flush(v);

    test_check(p.pen.fg.kind == AXL_CONSOLE_COLOR_RGB, "fg is rgb");
    test_check(p.pen.fg.r == 10 && p.pen.fg.g == 20 && p.pen.fg.b == 30, "rgb exact");
    test_check(axl_strcmp(p.transcript, "pen|text:X|") == 0,
               "set_pen latched before the text run it colours");
    axl_vterm_free(v);
}

static void
test_vterm_declined_scrollrect_decomposes_to_moverect_and_erase(void)
{
    /* probe_ops_no_blit binds scrollrect (returning 0) and erase + moverect. */
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(3, 4, &probe_ops_no_blit, &p);
    axl_vterm_feed(v, "\x1b" "[3;1H\n", 7);   /* force a scroll at the last row */
    axl_vterm_flush(v);

    test_check(p.scrollrect_calls == 1, "scrollrect offered once");
    test_check(p.moverect_calls  == 1, "declined scroll decomposed to a moverect");
    test_check(p.erase_calls     >= 1, "and an erase of the vacated rows");
    /* Pin the (dest, src) order: content moves UP, so dest is the higher rows. */
    test_check(p.last_moverect_dest.start_row == 0, "dest starts at row 0");
    test_check(p.last_moverect_src.start_row  == 1, "src starts at row 1");
    axl_vterm_free(v);
}

static void
test_vterm_accepted_scrollrect_suppresses_decomposition(void)
{
    /* probe_ops_blit binds scrollrect returning 1. */
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(3, 4, &probe_ops_blit, &p);
    axl_vterm_feed(v, "\x1b" "[3;1H\n", 7);
    axl_vterm_flush(v);

    test_check(p.scrollrect_calls == 1, "scrollrect offered once");
    test_check(p.moverect_calls  == 0, "accepted scroll did NOT decompose");
    axl_vterm_free(v);
}

static void
test_vterm_rejected_altscreen_prop_is_not_stored(void)
{
    /* probe_ops_reject_alt returns 0 from set_term_prop for ALT_SCREEN. */
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops_reject_alt, &p);
    axl_vterm_feed(v, "\x1b" "[?1049h", 8);
    axl_vterm_feed(v, "\x1b" "[?1049l", 8);
    /* Both alt-screen transitions reach the consumer, so the prop is mapped and
     * forwarded (not swallowed) even when the consumer rejects it. The rejection
     * itself IS observable through the ops surface: libvterm only stores the new
     * ALTSCREEN value if the callback accepted it (state.c:2223), and an ACCEPTED
     * ALTSCREEN=true erases the full screen rect (state.c:2256) while a REJECTED
     * one returns early (state.c:2227) and never erases. probe_ops_reject_alt
     * binds erase, so a 0 that fails to propagate would show up as erase_calls > 0.
     * adapter_settermprop returns ops->set_term_prop's value verbatim. */
    test_check(p.alt_prop_calls == 2, "both alt-screen transitions were offered");
    test_check(p.erase_calls == 0,
               "rejected alt-screen enter never cleared the screen -- our 0 reached "
               "libvterm's settermprop gate");
    axl_vterm_free(v);
}

static void
test_vterm_csi_3j_clears_scrollback(void)
{
    /* CSI 3J (ED, param 3) is xterm's "erase scrollback"; libvterm surfaces it as
     * the Layer-2 sb_clear callback, which the adapter maps to clear_scrollback.
     * It is the ONLY scrollback callback Layer 2 exposes (sb_pushline is Layer 3),
     * so a consumer owning a scrollback ring needs this to honour CSI 3J. */
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);

    axl_vterm_feed(v, "\x1b" "[2J", 4);   /* ED 2 (whole screen) must NOT clear sb */
    axl_vterm_flush(v);
    test_check(p.clear_sb_calls == 0, "CSI 2J does not clear the scrollback");

    axl_vterm_feed(v, "\x1b" "[3J", 4);   /* ED 3 (scrollback) -> sb_clear */
    axl_vterm_flush(v);
    test_check(p.clear_sb_calls == 1, "CSI 3J drives clear_scrollback exactly once");

    axl_vterm_free(v);
}

static void
test_vterm_csi_argcount_overflow_is_bounded(void)
{
    /* AXL patch [7/8]: a CSI with more than CSI_ARGS_MAX (16) arguments must not
     * write past args[] into the adjacent callbacks pointer.  Before the clamp,
     * the OOB write corrupted the parser and the sequence + trailing glyph were
     * lost; after it the write stays in bounds and the glyph after the CSI still
     * prints.  (test_vterm_csi_keeps_first_16_args pins the stronger property
     * that the first 16 args are kept and the rest dropped, per DEC/xterm.)
     * Untrusted-input hardening (serial / SOL). */
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);

    const char *seq = "\x1b" "[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20mZ";
    axl_vterm_feed(v, seq, axl_strlen(seq));
    axl_vterm_flush(v);

    test_check(axl_strcmp(p.text, "Z") == 0,
               "CSI with >16 args is bounded; the trailing glyph still prints");
    axl_vterm_free(v);
}

static void
test_vterm_csi_keeps_first_16_args(void)
{
    /* AXL patch [7/8]: the DEC/xterm rule for an over-long CSI is to keep the
     * first CSI_ARGS_MAX (16) parameters and silently drop the rest (Paul
     * Williams' parser; libtsm does the same). This SGR carries 17 params: 15
     * resets, then 31 (fg red = index 1) as arg 16, then 32 (fg green = index 2)
     * as arg 17 -- which must be dropped. So the pen's foreground must be red.
     * Before the digit/separator guards, arg 17 collapsed onto slot 15 and the
     * foreground came out green (last-wins), which this pins against. */
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);

    const char *seq =
        "\x1b" "[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;31;32mZ";
    axl_vterm_feed(v, seq, axl_strlen(seq));
    axl_vterm_flush(v);

    test_check(p.pen.fg.kind == AXL_CONSOLE_COLOR_INDEXED,
               "keep-first-16: arg 16 (SGR 31) set an indexed foreground");
    test_check(p.pen.fg.idx == 1,
               "keep-first-16: foreground is red (arg 16), not green (arg 17 dropped)");
    axl_vterm_free(v);
}

static void
test_vterm_sgr_arg_walk_bounded_at_16(void)
{
    /* AXL patch [7/8]: SGR consumers walk args[] past the current index -- the
     * underline sub-parameter (`CSI 4:<n>`), the end-of-code MORE skip, and the
     * `38`/`48` alternative-palette selector all read args[argi+1] or advance
     * argi. With a full 16-arg list the last in-count slot is args[15], so an
     * unbounded walk reads args[CSI_ARGS_MAX] == args[16] -- one past the array,
     * onto the adjacent callbacks pointer. These are OOB *reads* (undefined
     * behaviour, not deterministically observable without a sanitizer), so this
     * is functional coverage: each adversarial 16-arg CSI must be consumed
     * without wedging the parser, and a normal SGR + glyph fed afterwards must
     * still render. A MORE flag survives to slot 15 only because arg 16 is kept
     * (see test_vterm_csi_keeps_first_16_args), which is what exposed the walk. */
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(25, 80, &probe_ops, &p);

    /* arg 16 (slot 15) is an underline (4) carrying a MORE flag with no in-count
     * sub-parameter; and a 38 alternative-palette selector as the last arg.
     * Feed by axl_strlen -- a hardcoded length that undercounts would drop the
     * terminating 'm', leaving the CSI unterminated so setpen never runs and the
     * boundary is never exercised. */
    const char *more_at_16 = "\x1b" "[;;;;;;;;;;;;;;;4:m";
    const char *pal38_at_16 = "\x1b" "[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;38m";
    axl_vterm_feed(v, more_at_16, axl_strlen(more_at_16));
    axl_vterm_feed(v, pal38_at_16, axl_strlen(pal38_at_16));
    axl_vterm_flush(v);

    /* The parser must be healthy: a plain red glyph still renders correctly. */
    const char *red_r = "\x1b" "[31mR";
    axl_vterm_feed(v, red_r, axl_strlen(red_r));
    axl_vterm_flush(v);
    test_check(axl_strcmp(p.text, "R") == 0, "parser survived the 16-arg walks");
    test_check(p.pen.fg.kind == AXL_CONSOLE_COLOR_INDEXED && p.pen.fg.idx == 1,
               "a normal SGR after the adversarial CSIs still sets fg red");
    axl_vterm_free(v);
}

static void
test_vterm_set_size_validates_and_flows(void)
{
    VtermProbe p = {0};
    AxlVterm *v = NULL;
    v = axl_vterm_new(3, 4, &probe_ops, &p);

    /* Argument validation is deterministic and observable. */
    test_check(axl_vterm_set_size(NULL, 6, 4) == AXL_ERR, "NULL handle rejected");
    test_check(axl_vterm_set_size(v, 0, 4) == AXL_ERR, "zero rows rejected");
    test_check(axl_vterm_set_size(v, 6, -1) == AXL_ERR, "negative cols rejected");
    test_check(axl_vterm_set_size(v, 6, 4) == AXL_OK, "grow to 6 rows ok");

    /* Functional coverage of the grow path: text lands on a row that only exists
     * after the grow, and the terminal survives a full RIS. This exercises the
     * lineinfo realloc in adapter_resize (a Layer-2 consumer owns that realloc;
     * libvterm's unbound fallback is dead code). NOTE: the underlying defect the
     * realloc fixes is an out-of-bounds heap access, which is undefined behaviour
     * that this freestanding harness cannot deterministically observe without a
     * sanitizer — so these assertions are functional coverage, not a strict
     * fail-on-revert guard for the realloc. */
    axl_vterm_feed(v, "\x1b" "[6;1Hlow", 9);   /* CUP row 6, then "low" */
    axl_vterm_flush(v);
    test_check(axl_strcmp(p.text, "low") == 0, "text on the grown row is intact");

    axl_vterm_feed(v, "\x1b" "cAB", 4);         /* RIS, then "AB" at home */
    axl_vterm_flush(v);
    test_check(axl_strcmp(p.text, "AB") == 0, "reset after grow leaves a live terminal");

    axl_vterm_free(v);
}

// ===========================================================================
// axl-console-screen: the server-side screen model + snapshot serializer.
//
// Test seams (no public header) read the grid a round-trip asserts against.
// ===========================================================================

extern bool _axl_console_screen_test_cell(const AxlConsoleScreen *s,
        uint32_t row, uint32_t col, char *utf8_out, AxlConsolePen *pen_out);
extern void _axl_console_screen_test_cursor(const AxlConsoleScreen *s,
        uint32_t *row, uint32_t *col, bool *visible);
extern bool _axl_console_screen_test_alt(const AxlConsoleScreen *s);
extern bool _axl_console_screen_test_reverse(const AxlConsoleScreen *s);
extern void _axl_console_screen_test_geometry(const AxlConsoleScreen *s,
        uint32_t *rows, uint32_t *cols);

typedef struct {
    char   buf[16384];
    size_t len;
    size_t calls;   /* number of sink invocations — the "frame count" coalescing cuts */
} ScreenCap;

static void
screen_cap_sink(const char *bytes, size_t len, void *user)
{
    ScreenCap *c = user;
    c->calls++;
    if (c->len + len < sizeof c->buf) {
        axl_memcpy(c->buf + c->len, bytes, len);
        c->len += len;
        c->buf[c->len] = '\0';
    }
}

static void
screen_feed_str(AxlConsoleScreen *s, const char *str)
{
    axl_console_screen_feed(s, (const uint8_t *)str, axl_strlen(str));
}

static bool
screen_pen_eq(const AxlConsolePen *a, const AxlConsolePen *b)
{
    if (a->fg.kind != b->fg.kind || a->bg.kind != b->bg.kind) {
        return false;
    }
    if (a->fg.kind == AXL_CONSOLE_COLOR_INDEXED && a->fg.idx != b->fg.idx) {
        return false;
    }
    if (a->fg.kind == AXL_CONSOLE_COLOR_RGB
        && (a->fg.r != b->fg.r || a->fg.g != b->fg.g || a->fg.b != b->fg.b)) {
        return false;
    }
    if (a->bg.kind == AXL_CONSOLE_COLOR_INDEXED && a->bg.idx != b->bg.idx) {
        return false;
    }
    if (a->bg.kind == AXL_CONSOLE_COLOR_RGB
        && (a->bg.r != b->bg.r || a->bg.g != b->bg.g || a->bg.b != b->bg.b)) {
        return false;
    }
    return a->underline == b->underline && a->bold == b->bold
        && a->italic == b->italic && a->blink == b->blink
        && a->reverse == b->reverse && a->conceal == b->conceal
        && a->strike == b->strike;
}

/* Every visible cell (glyph + pen), the cursor, and the alt/reverse state of the
   two screens' ACTIVE grids match — the round-trip acceptance predicate. */
static bool
screens_identical(const AxlConsoleScreen *a, const AxlConsoleScreen *b,
                  uint32_t rows, uint32_t cols)
{
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            char ga[8] = {0}, gb[8] = {0};
            AxlConsolePen pa = {0}, pb = {0};
            bool oka = _axl_console_screen_test_cell(a, r, c, ga, &pa);
            bool okb = _axl_console_screen_test_cell(b, r, c, gb, &pb);
            if (oka != okb || axl_strcmp(ga, gb) != 0 || !screen_pen_eq(&pa, &pb)) {
                return false;
            }
        }
    }
    uint32_t ar, ac, br, bc;
    bool av, bv;
    _axl_console_screen_test_cursor(a, &ar, &ac, &av);
    _axl_console_screen_test_cursor(b, &br, &bc, &bv);
    return ar == br && ac == bc && av == bv
        && _axl_console_screen_test_alt(a) == _axl_console_screen_test_alt(b)
        && _axl_console_screen_test_reverse(a) == _axl_console_screen_test_reverse(b);
}

static void
test_screen_new_validates_and_null_safety(void)
{
    test_check(axl_console_screen_new(0, 80) == NULL, "screen: zero rows rejected");
    test_check(axl_console_screen_new(25, 0) == NULL, "screen: zero cols rejected");

    AxlConsoleScreen *s = axl_console_screen_new(25, 80);
    test_check(s != NULL, "screen: valid new ok");

    /* NULL-safety of the remaining entry points. */
    axl_console_screen_feed(NULL, (const uint8_t *)"x", 1);   /* no crash */
    axl_console_screen_feed(s, NULL, 1);                      /* no crash */

    ScreenCap cap = {0};
    test_check(axl_console_screen_snapshot(NULL, screen_cap_sink, &cap) == AXL_ERR,
               "screen: snapshot NULL handle rejected");
    test_check(axl_console_screen_snapshot(s, NULL, &cap) == AXL_ERR,
               "screen: snapshot NULL sink rejected");
    test_check(cap.len == 0, "screen: rejected snapshot wrote nothing to the sink");

    test_check(axl_console_screen_resize(NULL, 5, 5) == AXL_ERR, "screen: resize NULL rejected");
    test_check(axl_console_screen_resize(s, 0, 5) == AXL_ERR, "screen: resize zero rows rejected");
    test_check(axl_console_screen_resize(s, 5, 0) == AXL_ERR, "screen: resize zero cols rejected");

    axl_console_screen_free(s);
    axl_console_screen_free(NULL);   /* NULL-safe */
}

static void
test_screen_feed_places_text(void)
{
    AxlConsoleScreen *s = NULL;
    s = axl_console_screen_new(3, 10);

    screen_feed_str(s, "Hi");

    char g[8] = {0};
    test_check(_axl_console_screen_test_cell(s, 0, 0, g, NULL) && axl_strcmp(g, "H") == 0,
               "screen: cell (0,0) is 'H'");
    test_check(_axl_console_screen_test_cell(s, 0, 1, g, NULL) && axl_strcmp(g, "i") == 0,
               "screen: cell (0,1) is 'i'");
    uint32_t r = 99, c = 99;
    _axl_console_screen_test_cursor(s, &r, &c, NULL);
    test_check(r == 0 && c == 2, "screen: cursor advanced to (0,2)");

    axl_console_screen_free(s);
}

/* Pin the EXACT self-contained VT repaint of a tiny screen. Substrings would let
   a format regression slip through, so this is axl_strcmp-exact per the
   output-format rule. */
static void
test_screen_snapshot_exact_vt(void)
{
    AxlConsoleScreen *s = NULL;
    s = axl_console_screen_new(2, 4);
    screen_feed_str(s, "\x1b[31mAB");   /* red fg, "AB" at (0,0),(0,1); cursor (0,2) */

    ScreenCap cap = {0};
    test_check(axl_console_screen_snapshot(s, screen_cap_sink, &cap) == AXL_OK,
               "screen: snapshot returns AXL_OK");
    test_check(axl_strcmp(cap.buf,
            "\x1b[2J\x1b[m\x1b[1;1H\x1b[0;38;5;1;49mAB\x1b[?25h\x1b[1;3H") == 0,
            "screen: exact self-contained VT repaint of a 2x4 red-AB screen");

    axl_console_screen_free(s);
}

/* A mostly-empty screen must coalesce to a handful of bytes, NOT ~2KB of spaces. */
static void
test_screen_snapshot_blank_is_coalesced(void)
{
    AxlConsoleScreen *s = NULL;
    s = axl_console_screen_new(25, 80);

    ScreenCap cap = {0};
    axl_console_screen_snapshot(s, screen_cap_sink, &cap);

    test_check(cap.len > 0, "screen: blank snapshot is non-empty (still a clear + cursor)");
    test_check(cap.len < 200,
               "screen: blank 80x25 snapshot is coalesced small, not ~2KB of spaces");

    axl_console_screen_free(s);
}

/* The SoftBMC acceptance: feed known ANSI (clear + colored runs + CUP) -> snapshot
   -> feed into a SECOND fresh screen -> identical grids (cells + pen + cursor). */
static void
test_screen_snapshot_roundtrip_primary(void)
{
    AxlConsoleScreen *a = NULL;
    a = axl_console_screen_new(5, 20);
    /* clear+home; red "RED" at (0,0); CUP to (1,4); blue-bg (keeps red fg) "sky". */
    screen_feed_str(a, "\x1b[2J\x1b[H\x1b[31mRED\x1b[2;5H\x1b[44msky");

    ScreenCap cap = {0};
    test_check(axl_console_screen_snapshot(a, screen_cap_sink, &cap) == AXL_OK,
               "screen: snapshot ok");

    AxlConsoleScreen *b = NULL;
    b = axl_console_screen_new(5, 20);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);

    char g[8] = {0};
    AxlConsolePen p = {0};
    test_check(_axl_console_screen_test_cell(b, 0, 0, g, &p) && axl_strcmp(g, "R") == 0,
               "screen: round-trip B (0,0) is 'R'");
    test_check(p.fg.kind == AXL_CONSOLE_COLOR_INDEXED && p.fg.idx == 1,
               "screen: round-trip B (0,0) foreground is red");
    test_check(_axl_console_screen_test_cell(b, 1, 4, g, &p) && axl_strcmp(g, "s") == 0,
               "screen: round-trip B (1,4) is 's'");
    test_check(p.bg.kind == AXL_CONSOLE_COLOR_INDEXED && p.bg.idx == 4,
               "screen: round-trip B (1,4) background is blue");

    test_check(screens_identical(a, b, 5, 20),
               "screen: round-trip A and B active grids are identical");

    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

/* Snapshot while the ALTERNATE screen is active repaints the alt content and
   re-enters alt; the round-trip restores it. */
static void
test_screen_snapshot_roundtrip_alt(void)
{
    AxlConsoleScreen *a = NULL;
    a = axl_console_screen_new(5, 20);
    screen_feed_str(a, "\x1b[2J\x1b[H\x1b[33mprimary");   /* primary content */
    screen_feed_str(a, "\x1b[?1049h\x1b[2;2Halt");        /* enter alt, draw "alt" */
    test_check(_axl_console_screen_test_alt(a), "screen: A is on the alternate screen");

    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = NULL;
    b = axl_console_screen_new(5, 20);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);

    test_check(_axl_console_screen_test_alt(b), "screen: round-trip B enters the alternate screen");
    char g[8] = {0};
    test_check(_axl_console_screen_test_cell(b, 1, 1, g, NULL) && axl_strcmp(g, "a") == 0,
               "screen: round-trip B shows the alt content at (1,1)");
    test_check(screens_identical(a, b, 5, 20),
               "screen: alt round-trip active grids identical");

    /* The in-alt snapshot also carried the PRIMARY underneath: leaving the alt
       screen on B (as the guest's DECRST 1049 would) reveals it, not a blank. */
    axl_console_screen_feed(b, (const uint8_t *)"\x1b[?1049l", 8);
    test_check(!_axl_console_screen_test_alt(b), "screen: B leaves the alternate screen");
    test_check(_axl_console_screen_test_cell(b, 0, 0, g, NULL) && axl_strcmp(g, "p") == 0,
               "screen: in-alt snapshot carried the primary (revealed on DECRST 1049)");

    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

/* The dual-grid faithfulness test: after the guest ENTERS then LEAVES the alt
   screen, the primary content is intact and the snapshot repaints THAT — not the
   stale alt buffer. A single-grid model fails this. */
static void
test_screen_snapshot_roundtrip_out_of_alt(void)
{
    AxlConsoleScreen *a = NULL;
    a = axl_console_screen_new(5, 20);
    screen_feed_str(a, "\x1b[2J\x1b[HSHELL");        /* primary "SHELL" at row 0 */
    screen_feed_str(a, "\x1b[?1049h\x1b[2;2HTUI");   /* enter alt, draw "TUI" */
    screen_feed_str(a, "\x1b[?1049l");               /* LEAVE alt -> primary restored */

    test_check(!_axl_console_screen_test_alt(a), "screen: A back on the primary screen");
    char g[8] = {0};
    test_check(_axl_console_screen_test_cell(a, 0, 0, g, NULL) && axl_strcmp(g, "S") == 0,
               "screen: A primary 'SHELL' survived the alt-screen round-trip");

    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = NULL;
    b = axl_console_screen_new(5, 20);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);

    test_check(!_axl_console_screen_test_alt(b), "screen: round-trip B on the primary screen");
    test_check(_axl_console_screen_test_cell(b, 0, 0, g, NULL) && axl_strcmp(g, "S") == 0,
               "screen: round-trip B shows the restored primary, not the alt buffer");
    test_check(_axl_console_screen_test_cell(b, 1, 1, g, NULL) && g[0] == '\0',
               "screen: round-trip B has no leftover 'TUI' alt content");

    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

static void
test_screen_snapshot_carries_reverse_video(void)
{
    AxlConsoleScreen *a = NULL;
    a = axl_console_screen_new(3, 5);
    screen_feed_str(a, "\x1b[?5h");   /* DECSCNM: whole-screen reverse video ON */
    test_check(_axl_console_screen_test_reverse(a), "screen: A has reverse video on");

    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = NULL;
    b = axl_console_screen_new(3, 5);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);

    test_check(_axl_console_screen_test_reverse(b),
               "screen: round-trip B carries the reverse-video (DECSCNM) state");

    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

/* A double-width (CJK) glyph occupies two columns: the base cell plus a
   continuation. The snapshot must not emit a stray space for the continuation, and
   the cursor must land two columns on. */
static void
test_screen_snapshot_roundtrip_wide(void)
{
    AxlConsoleScreen *a = NULL;
    a = axl_console_screen_new(3, 8);
    screen_feed_str(a, "\xe4\xb8\xad");   /* U+4E2D, a two-cell CJK ideograph */

    uint32_t cr = 99, cc = 99;
    _axl_console_screen_test_cursor(a, &cr, &cc, NULL);
    test_check(cr == 0 && cc == 2, "screen: a double-width glyph advances the cursor by two");

    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = NULL;
    b = axl_console_screen_new(3, 8);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);

    char g[8] = {0};
    test_check(_axl_console_screen_test_cell(b, 0, 0, g, NULL) && axl_strcmp(g, "\xe4\xb8\xad") == 0,
               "screen: round-trip B (0,0) is the CJK ideograph");
    _axl_console_screen_test_cursor(b, &cr, &cc, NULL);
    test_check(cr == 0 && cc == 2, "screen: round-trip B cursor lands two columns on");
    test_check(screens_identical(a, b, 3, 8),
               "screen: wide-glyph round-trip active grids identical");

    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

/* A wide glyph parked on the last column (autowrap off) followed by a combining
   mark in the same run must not index past the row — a guest-reachable heap OOB
   before the pending-margin clamp. Functional coverage: the model survives and a
   later glyph still renders. */
static void
test_screen_wide_glyph_at_margin_is_bounded(void)
{
    AxlConsoleScreen *s = NULL;
    s = axl_console_screen_new(2, 4);
    screen_feed_str(s, "\x1b[?7l");               /* DECAWM off: no autowrap */
    screen_feed_str(s, "\x1b[2;4H");              /* park at the last row, last column */
    screen_feed_str(s, "\xe4\xb8\xad\xcc\x81");   /* U+4E2D (wide) + U+0301 (combining) */
    screen_feed_str(s, "\x1b[1;1HX");             /* the parser must still be healthy */

    char g[8] = {0};
    test_check(_axl_console_screen_test_cell(s, 0, 0, g, NULL) && axl_strcmp(g, "X") == 0,
               "screen: survives a wide glyph + combining mark parked at the margin");

    axl_console_screen_free(s);
}

static void
test_screen_snapshot_rep_run_exact(void)
{
    /* A run of >= REP_RUN_MIN+1 identical glyphs collapses to one glyph + REP
       (CSI n b), not one write per cell. Exact bytes (bucket B). */
    AxlConsoleScreen *s = axl_console_screen_new(1, 10);
    screen_feed_str(s, "\x1b[2J\x1b[Hxxxxxxxx\x1b[H");   /* 8 x's, cursor home */

    ScreenCap cap = {0};
    axl_console_screen_snapshot(s, screen_cap_sink, &cap);
    test_check(axl_strcmp(cap.buf,
                          "\x1b[2J\x1b[m\x1b[1;1Hx\x1b[7b\x1b[?25h\x1b[1;1H") == 0,
               "screen: run of 8 glyphs -> 'x' + REP 7 (CSI 7 b)");
    axl_console_screen_free(s);
}

static void
test_screen_snapshot_rep_run_roundtrip(void)
{
    /* REP round-trips exactly: fed back through a fresh model, the run of glyphs
       reproduces the same cells. */
    AxlConsoleScreen *a = axl_console_screen_new(3, 20);
    screen_feed_str(a, "\x1b[2J\x1b[H\x1b[32mgggggggggggg\x1b[2;3Hhi");   /* 12 g's + "hi" */
    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = axl_console_screen_new(3, 20);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);
    test_check(screens_identical(a, b, 3, 20),
               "screen: glyph-run snapshot (REP) round-trips cell-exact");
    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

static void
test_screen_snapshot_ech_blank_run_roundtrip(void)
{
    /* A blank run with a non-default background collapses to ECH + CUF, which
       round-trips to *blank* cells (len 0) — not space glyphs. */
    AxlConsoleScreen *a = axl_console_screen_new(2, 20);
    screen_feed_str(a, "\x1b[44m\x1b[2J\x1b[H");   /* blue bg, clear -> 40 blank bg cells */
    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = axl_console_screen_new(2, 20);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);
    test_check(screens_identical(a, b, 2, 20),
               "screen: blank-bg-run snapshot (ECH+CUF) round-trips to blank cells");

    /* The cell (0,0) is a blank (len 0) carrying the background, not a space. */
    char g[8] = {0};
    _axl_console_screen_test_cell(b, 0, 0, g, NULL);
    test_check(g[0] == '\0',
               "screen: ECH replay leaves a blank cell, not a ' ' glyph");
    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

static void
test_screen_snapshot_rep_margin_phantom(void)
{
    /* Regression: a REP'd glyph run ending at cols-2, with an emittable cell at
       the last column, must not phantom-wrap that cell to the next row. */
    AxlConsoleScreen *a = axl_console_screen_new(1, 10);
    screen_feed_str(a, "\x1b[2J\x1b[Hxxxxxxxxxy");   /* 9 x (cols 0-8), y at col 9 */
    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = axl_console_screen_new(1, 10);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);
    test_check(screens_identical(a, b, 1, 10),
               "screen: REP run ending at cols-2 keeps the following cell on-row");
    char g[8] = {0};
    _axl_console_screen_test_cell(b, 0, 9, g, NULL);
    test_check(axl_strcmp(g, "y") == 0,
               "screen: cell after a margin REP run lands at the last column");
    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

static void
test_screen_snapshot_box_border_roundtrip(void)
{
    /* The canonical full-width box border: a horizontal run that ends one short of
       the right corner — the exact REP phantom trigger this feature must survive. */
    AxlConsoleScreen *a = axl_console_screen_new(3, 20);
    screen_feed_str(a, "\x1b[2J\x1b[H+------------------+");   /* + 18x- + (cols 0..19) */
    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);

    AxlConsoleScreen *b = axl_console_screen_new(3, 20);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);
    test_check(screens_identical(a, b, 3, 20),
               "screen: full-width box border round-trips (no REP phantom wrap)");
    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

static void
test_screen_snapshot_multichunk(void)
{
    /* Force the buffering auto-flush (SNAPSHOT_CHUNK, 4 KB): a large screen of
       varied (non-run) content serializes to > one chunk, so the snapshot delivers
       several sink calls yet still round-trips exactly. */
    AxlConsoleScreen *a = axl_console_screen_new(60, 100);
    screen_feed_str(a, "\x1b[2J\x1b[H");
    for (int r = 0; r < 60; r++) {
        char cup[16];
        axl_snprintf(cup, sizeof(cup), "\x1b[%d;1H", r + 1);
        screen_feed_str(a, cup);
        char line[101];
        for (int c = 0; c < 100; c++) {
            /* Alternate glyphs so no run reaches REP_RUN_MIN — keeps bytes high. */
            line[c] = (char)(((r + c) & 1) ? ('a' + ((r + c) % 26))
                                           : ('A' + ((r * 3 + c) % 26)));
        }
        line[100] = '\0';
        screen_feed_str(a, line);
    }
    ScreenCap cap = {0};
    axl_console_screen_snapshot(a, screen_cap_sink, &cap);
    test_check(cap.len > 4096, "screen: large varied snapshot exceeds one 4 KB chunk");
    test_check(cap.calls >= 2,
               "screen: multi-chunk snapshot delivers several sink calls (auto-flush)");

    AxlConsoleScreen *b = axl_console_screen_new(60, 100);
    axl_console_screen_feed(b, (const uint8_t *)cap.buf, cap.len);
    test_check(screens_identical(a, b, 60, 100),
               "screen: multi-chunk snapshot round-trips exactly");
    axl_console_screen_free(a);
    axl_console_screen_free(b);
}

static void
test_screen_snapshot_frame_count_coalesced(void)
{
    /* The frame-count win: a fully-painted 80x25 used to emit ~one sink call per
       cell (~2000). Buffered into SNAPSHOT_CHUNK pieces it is a small, bounded
       number of calls. */
    AxlConsoleScreen *s = axl_console_screen_new(25, 80);
    screen_feed_str(s, "\x1b[2J\x1b[H");
    for (int r = 0; r < 25; r++) {
        char line[81];
        for (int c = 0; c < 80; c++) {
            line[c] = (char)('A' + ((r + c) % 26));
        }
        line[80] = '\0';
        char cup[16];
        axl_snprintf(cup, sizeof(cup), "\x1b[%d;1H", r + 1);
        screen_feed_str(s, cup);
        screen_feed_str(s, line);
    }
    ScreenCap cap = {0};
    axl_console_screen_snapshot(s, screen_cap_sink, &cap);
    test_check(cap.len > 1000, "screen: full 80x25 snapshot is substantial");
    test_check(cap.calls <= 8,
               "screen: full-screen snapshot coalesces to a handful of sink calls "
               "(<= 8), not one per cell");
    axl_console_screen_free(s);
}

// ---------------------------------------------------------------------------

/* Counts sink invocations and concatenates bytes — the live-encoder equivalent
   of ScreenCap, for the coalesce/flush assertions. */
typedef struct {
    char   buf[4096];
    size_t len;
    size_t calls;
} EncCap;

static void
enc_cap_sink(const char *bytes, size_t len, void *user)
{
    EncCap *c = user;
    c->calls++;
    if (c->len + len < sizeof c->buf) {
        axl_memcpy(c->buf + c->len, bytes, len);
        c->len += len;
        c->buf[c->len] = '\0';
    }
}

static void
test_vt_enc_coalesce_buffers_until_flush(void)
{
    EncCap cap = {0};
    AxlConsoleVtEncConfig cfg = {
        .sink = enc_cap_sink, .user = &cap, .cols = 80, .rows = 25, .coalesce = true
    };
    AxlConsoleVtEnc *e = axl_console_vt_enc_new(&cfg);
    test_check(e != NULL, "vt-enc: coalesce encoder created");

    void                *u  = NULL;
    const AxlConsoleOps *ops = axl_console_vt_enc_ops(e, &u);
    /* A keystroke echo's worth of ops: clear + cursor + text = 3 ops. */
    ops->clear_screen(u);
    ops->set_cursor(u, 2, 5);
    ops->output_text(u, "hi", 2);
    test_check(cap.calls == 0,
               "vt-enc: coalesce buffers — no sink call before flush");

    axl_console_vt_enc_flush(e);
    test_check(cap.calls == 1,
               "vt-enc: flush delivers the whole turn as ONE sink call");
    /* clear_screen -> ESC[2J ESC[H ; set_cursor(2,5) -> ESC[3;6H ; "hi". */
    test_check(axl_strcmp(cap.buf, "\x1b[2J\x1b[H\x1b[3;6Hhi") == 0,
               "vt-enc: flushed bytes are the concatenated turn, in order");

    /* A second flush with nothing buffered is a no-op (no extra call). */
    axl_console_vt_enc_flush(e);
    test_check(cap.calls == 1, "vt-enc: empty flush is a no-op");
    axl_console_vt_enc_free(e);
}

static void
test_vt_enc_no_coalesce_emits_per_op(void)
{
    EncCap cap = {0};
    AxlConsoleVtEncConfig cfg = {
        .sink = enc_cap_sink, .user = &cap, .cols = 80, .rows = 25, .coalesce = false
    };
    AxlConsoleVtEnc *e = axl_console_vt_enc_new(&cfg);
    void                *u  = NULL;
    const AxlConsoleOps *ops = axl_console_vt_enc_ops(e, &u);
    ops->clear_screen(u);
    ops->output_text(u, "hi", 2);
    test_check(cap.calls >= 2,
               "vt-enc: default (no coalesce) still emits per op");
    axl_console_vt_enc_flush(e);   /* NULL-safe / no-op when not coalescing */
    test_check(cap.calls >= 2, "vt-enc: flush is a no-op without coalesce");
    axl_console_vt_enc_free(e);
}

// ---------------------------------------------------------------------------

static void
test_screen_resize(void)
{
    AxlConsoleScreen *s = NULL;
    s = axl_console_screen_new(3, 10);
    screen_feed_str(s, "\x1b[2J\x1b[Habc");

    test_check(axl_console_screen_resize(s, 5, 20) == AXL_OK, "screen: grow resize ok");
    uint32_t rows = 0, cols = 0;
    _axl_console_screen_test_geometry(s, &rows, &cols);
    test_check(rows == 5 && cols == 20, "screen: geometry updated after resize");

    char g[8] = {0};
    test_check(_axl_console_screen_test_cell(s, 0, 0, g, NULL) && axl_strcmp(g, "a") == 0,
               "screen: resize preserves the top-left region ('a' at (0,0))");

    /* Shrink below the cursor: geometry updates and the cursor clamps in-bounds. */
    screen_feed_str(s, "\x1b[5;20H");   /* cursor to (4,19) */
    test_check(axl_console_screen_resize(s, 2, 4) == AXL_OK, "screen: shrink resize ok");
    _axl_console_screen_test_geometry(s, &rows, &cols);
    test_check(rows == 2 && cols == 4, "screen: geometry updated after shrink");
    uint32_t cr = 99, cc = 99;
    _axl_console_screen_test_cursor(s, &cr, &cc, NULL);
    test_check(cr == 1 && cc == 3, "screen: cursor clamped into the shrunk bounds");
    test_check(_axl_console_screen_test_cell(s, 0, 0, g, NULL) && axl_strcmp(g, "a") == 0,
               "screen: shrink preserves the top-left region");

    axl_console_screen_free(s);
}

// ---------------------------------------------------------------------------


static int
test_vterm_main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;
    /* The header is not decoration: the harness brackets each binary between
       this line and its Results footer, and a binary without one is invisible
       to both the stalled-binary detector and the per-binary leak verdict
       check (test_check_leaks, test/integration/common-test.sh). */
    test_print_header("AxlVterm");

    test_vterm_new_uses_axl_heap();
    test_putglyph_positions_and_width();
    test_putglyph_reports_double_width();
    test_csi_erase_and_cursor();
    test_sgr_sets_indexed_foreground();
    test_altscreen_is_a_termprop();
    test_scosc_scorc_saves_and_restores_cursor();
    test_scosc_under_leftright_margin_is_decslrm();
    test_decscusr_query_before_reset();
    test_decscusr_query_after_reset();
    test_decscusr_query_tracks_shape();
    test_declined_scrollrect_decomposes_to_erase();

    test_vterm_reports_width_resolved_cell_rule();
    test_vterm_char_width_zero_for_combining();
    test_vterm_coalesces_glyphs_into_one_run();
    test_vterm_cursor_jump_flushes_the_run();
    test_vterm_out_of_range_codepoint_becomes_replacement();
    test_vterm_five_byte_form_becomes_replacement();
    test_vterm_astral_codepoint_round_trips();
    test_vterm_last_scalar_value_round_trips();
    test_vterm_sgr_truecolor_becomes_rgb_pen();
    test_vterm_declined_scrollrect_decomposes_to_moverect_and_erase();
    test_vterm_accepted_scrollrect_suppresses_decomposition();
    test_vterm_rejected_altscreen_prop_is_not_stored();
    test_vterm_csi_3j_clears_scrollback();
    test_vterm_csi_argcount_overflow_is_bounded();
    test_vterm_csi_keeps_first_16_args();
    test_vterm_sgr_arg_walk_bounded_at_16();
    test_vterm_set_size_validates_and_flows();

    test_screen_new_validates_and_null_safety();
    test_screen_feed_places_text();
    test_screen_snapshot_exact_vt();
    test_screen_snapshot_blank_is_coalesced();
    test_screen_snapshot_roundtrip_primary();
    test_screen_snapshot_roundtrip_alt();
    test_screen_snapshot_roundtrip_out_of_alt();
    test_screen_snapshot_carries_reverse_video();
    test_screen_snapshot_roundtrip_wide();
    test_screen_wide_glyph_at_margin_is_bounded();
    test_screen_snapshot_rep_run_exact();
    test_screen_snapshot_rep_run_roundtrip();
    test_screen_snapshot_rep_margin_phantom();
    test_screen_snapshot_box_border_roundtrip();
    test_screen_snapshot_multichunk();
    test_screen_snapshot_ech_blank_run_roundtrip();
    test_screen_snapshot_frame_count_coalesced();
    test_vt_enc_coalesce_buffers_until_flush();
    test_vt_enc_no_coalesce_emits_per_op();
    test_screen_resize();

    return test_print_results();
}

AXL_APP(test_vterm_main)
