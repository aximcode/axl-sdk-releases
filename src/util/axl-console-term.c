/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-term.c
    Local interactive terminal — the on-screen AxlConsoleOps sink. See
    axl-console-term.h and AXL-Console-Terminal-Design.md. The cell model + op
    translation graduate the proven console-device-smoke.c grid (which was itself the
    cv2 spike's grid); this file makes it a reusable heap object with scrollback,
    selection, rendering to the GOP or an offscreen buffer, and reflow.
**/

#include <axl/axl-console-term.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <axl/axl-string.h>
#include <axl/axl-str.h>     /* axl_utf8_encode — the one codepoint->UTF-8 encoder */
#include <axl/axl-clipboard.h>
#include <axl/axl-cursor.h>  /* dogfood the shared software mouse-cursor overlay */
#include <uefi/axl-uefi.h>   /* handle_hotkey reads the opaque key as EFI_KEY_DATA */

AXL_LOG_DOMAIN("conterm");

#define TERM_CELL_MAX       AXL_UTF8_MAX_LEN   /* bytes of UTF-8 per cell */
#define TERM_DEFAULT_COLS   80
#define TERM_DEFAULT_ROWS   25
#define TERM_DEFAULT_SCROLLBACK 1000   /* history rows when cfg is 0 */

#define TERM_SCAN_PAGE_UP   0x09   /* EFI SCAN_PAGE_UP */
#define TERM_SCAN_PAGE_DOWN 0x0A   /* EFI SCAN_PAGE_DOWN */

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    char    utf8[TERM_CELL_MAX];   /* not NUL-terminated; @len bytes valid */
    uint8_t len;                   /* 0 = blank */
    uint8_t fg;                    /* palette index 0..15 */
    uint8_t bg;                    /* palette index 0..7  */
} TermCell;

/* Last-rendered appearance of a cell, so render() blits only cells that actually
   changed. Writing to the GOP framebuffer is cheap on bare metal but expensive under
   a dirty-tracked display (each write faults into the hypervisor), so re-blitting a
   whole 160-cell row for a one-cell change pegged a core under VNC. This shadow makes
   the pixel writes proportional to the change. Captures everything that affects the
   drawn pixels: the glyph, the EFFECTIVE colours (post selection-invert) and whether
   the cursor caret sits on the cell. @valid=0 forces a (re)blit. */
typedef struct {
    char    utf8[TERM_CELL_MAX];
    uint8_t len;
    uint8_t fg;      /* effective foreground (after selection invert) */
    uint8_t bg;      /* effective background */
    uint8_t caret;   /* 1 = the cursor underline is drawn on this cell */
    uint8_t valid;   /* 0 = never rendered / invalidated -> force a blit */
} ShadowCell;

struct AxlConsoleTerm {
    uint32_t        cols;
    uint32_t        rows;
    TermCell       *screen;        /* rows*cols, row-major */
    bool           *dirty_row;     /* rows; a row needs re-blit */
    ShadowCell     *shadow;        /* rows*cols; last-rendered appearance (damage) */

    /* Scrollback: a ring of rows that scrolled off the top of the live screen. */
    TermCell       *history;       /* scrollback_rows*cols, row-major ring */
    uint32_t        scrollback_rows;
    uint32_t        hist_head;     /* next write slot */
    uint32_t        hist_fill;     /* rows stored (<= scrollback_rows) */
    uint32_t        scroll_off;    /* viewport offset: 0 = live, N = N rows back */

    /* Selection, in VIEWPORT cell coordinates (resolved through scroll_off at copy). */
    bool            sel_active;
    uint32_t        sel_a_row, sel_a_col;   /* anchor */
    uint32_t        sel_b_row, sel_b_col;   /* free end (drag-to) */

    int32_t         cur_row;
    int32_t         cur_col;
    uint8_t         pen_fg;
    uint8_t         pen_bg;
    bool            cursor_visible;

    const AxlFont  *font;
    uint16_t        cell_w;        /* cached font cell metrics (== font->cell_width) */
    uint16_t        cell_h;
    AxlGfxPixel     palette[16];

    /* Render target + bounds (from the config). target NULL => the GOP screen. */
    AxlGfxBuffer   *target;
    uint32_t        bx, by;        /* render origin within the target */
    uint32_t        bw, bh;        /* render extent (0 = to the target/GOP edge) */
    void          (*on_zoom)(void *user, int32_t delta);
    void           *cb_user;

    /* Software mouse-cursor overlay (cfg.mouse_cursor). NULL when disabled or when the
       cursor could not be created (OOM / save-under with no GOP) -- every cursor call
       is NULL-safe, so the terminal renders fine without it. Bound to t->target: a
       NULL target => save-under against the GOP; a buffer => scene-fold into it. */
    AxlCursor      *cursor;

    AxlConsoleOps   ops;           /* the sink handed to a producer */
};

/* The UEFI 16-colour console palette, in GOP order (same as the cv2 spike / smoke). */
static const AxlGfxPixel TERM_PALETTE[16] = {
    AXL_GFX_RGB(0x00, 0x00, 0x00), AXL_GFX_RGB(0x00, 0x00, 0xA8),
    AXL_GFX_RGB(0x00, 0xA8, 0x00), AXL_GFX_RGB(0x00, 0xA8, 0xA8),
    AXL_GFX_RGB(0xA8, 0x00, 0x00), AXL_GFX_RGB(0xA8, 0x00, 0xA8),
    AXL_GFX_RGB(0xA8, 0x54, 0x00), AXL_GFX_RGB(0xA8, 0xA8, 0xA8),
    AXL_GFX_RGB(0x54, 0x54, 0x54), AXL_GFX_RGB(0x54, 0x54, 0xFF),
    AXL_GFX_RGB(0x54, 0xFF, 0x54), AXL_GFX_RGB(0x54, 0xFF, 0xFF),
    AXL_GFX_RGB(0xFF, 0x54, 0x54), AXL_GFX_RGB(0xFF, 0x54, 0xFF),
    AXL_GFX_RGB(0xFF, 0xFF, 0x54), AXL_GFX_RGB(0xFF, 0xFF, 0xFF),
};

// ---------------------------------------------------------------------------
// Cell grid model (heap-object port of the smoke's grid)
// ---------------------------------------------------------------------------

static TermCell *
cell_at(AxlConsoleTerm *t, int32_t row, int32_t col)
{
    return &t->screen[(uint32_t)row * t->cols + (uint32_t)col];
}

static void
mark_dirty(AxlConsoleTerm *t, int32_t row)
{
    if (row >= 0 && (uint32_t)row < t->rows) {
        t->dirty_row[row] = true;
    }
}

static void
mark_all_dirty(AxlConsoleTerm *t)
{
    for (uint32_t r = 0; r < t->rows; r++) {
        t->dirty_row[r] = true;
    }
}

/* Drop the per-cell damage shadow so the next render re-blits every visible cell.
   Needed when the PIXELS change while the cell CONTENT does not -- a new font, new
   render bounds, or a new palette -- since the shadow compares content, not pixels.
   (A selection or scroll change alters the effective cell appearance, which the
   normal per-cell compare already catches, so those don't need this.) */
static void
shadow_invalidate(AxlConsoleTerm *t)
{
    if (t->shadow != NULL) {
        for (uint32_t i = 0; i < t->rows * t->cols; i++) {
            t->shadow[i].valid = 0;
        }
    }
}

static void
cell_blank(AxlConsoleTerm *t, TermCell *c)
{
    c->len = 0;
    c->fg  = t->pen_fg;
    c->bg  = t->pen_bg;
}

static void
grid_clear(AxlConsoleTerm *t)
{
    for (uint32_t r = 0; r < t->rows; r++) {
        for (uint32_t c = 0; c < t->cols; c++) {
            cell_blank(t, cell_at(t, (int32_t)r, (int32_t)c));
        }
        t->dirty_row[r] = true;
    }
    t->cur_row = 0;
    t->cur_col = 0;
}

static void
grid_scroll(AxlConsoleTerm *t)
{
    for (uint32_t r = 0; r + 1 < t->rows; r++) {
        for (uint32_t c = 0; c < t->cols; c++) {
            *cell_at(t, (int32_t)r, (int32_t)c) = *cell_at(t, (int32_t)(r + 1), (int32_t)c);
        }
        t->dirty_row[r] = true;
    }
    for (uint32_t c = 0; c < t->cols; c++) {
        cell_blank(t, cell_at(t, (int32_t)(t->rows - 1), (int32_t)c));
    }
    t->dirty_row[t->rows - 1] = true;
}

/* Copy the top live row (about to be lost to grid_scroll) into the history ring. */
static void
push_history(AxlConsoleTerm *t)
{
    if (t->history == NULL || t->scrollback_rows == 0) {
        return;
    }
    TermCell *dst = &t->history[(size_t)t->hist_head * t->cols];
    for (uint32_t c = 0; c < t->cols; c++) {
        dst[c] = *cell_at(t, 0, (int32_t)c);
    }
    t->hist_head = (t->hist_head + 1) % t->scrollback_rows;
    if (t->hist_fill < t->scrollback_rows) {
        t->hist_fill++;
    }
}

static void
grid_newline(AxlConsoleTerm *t)
{
    t->cur_row++;
    if ((uint32_t)t->cur_row >= t->rows) {
        push_history(t);   /* row 0 is about to be lost -> scrollback */
        grid_scroll(t);
        t->cur_row = (int32_t)t->rows - 1;
    }
}

static void
grid_put_cp(AxlConsoleTerm *t, uint32_t cp)
{
    if (cp == '\n') {
        grid_newline(t);
        if ((uint32_t)t->cur_col >= t->cols) { t->cur_col = (int32_t)t->cols - 1; }
        return;
    }
    if (cp == '\r') { t->cur_col = 0; return; }
    if (cp == '\b') { if (t->cur_col > 0) { t->cur_col--; } return; }
    if (cp == '\t') {
        t->cur_col = (t->cur_col + 8) & ~7;
        if ((uint32_t)t->cur_col >= t->cols) { t->cur_col = 0; grid_newline(t); }
        return;
    }
    if (cp < 0x20) {
        return;
    }
    if ((uint32_t)t->cur_col >= t->cols) { t->cur_col = 0; grid_newline(t); }

    /* A cell's bytes go straight to the glyph renderer AND to the clipboard, so
       a codepoint with no UTF-8 spelling -- a lone surrogate, which term_output_text
       will happily decode out of a WTF-8 producer -- must not be re-encoded in its
       3-byte shape. axl_utf8_encode refuses it; substitute U+FFFD rather than blank
       the cell, so the glyph run stays aligned with the cursor arithmetic below and
       the reader sees the same replacement character every other AXL decoder emits.
       cell->utf8 is AXL_UTF8_MAX_LEN, so a 0 return means only "unencodable". */
    TermCell *cell = cell_at(t, t->cur_row, t->cur_col);
    size_t    n    = axl_utf8_encode(cp, cell->utf8, sizeof(cell->utf8));
    if (n == 0) {
        n = axl_utf8_encode(0xFFFD, cell->utf8, sizeof(cell->utf8));
    }
    cell->len = (uint8_t)n;
    cell->fg = t->pen_fg;
    cell->bg = t->pen_bg;
    mark_dirty(t, t->cur_row);
    t->cur_col++;
}

// ---------------------------------------------------------------------------
// AxlConsoleOps sink
// ---------------------------------------------------------------------------

static void
term_output_text(void *user, const char *utf8, size_t len)
{
    AxlConsoleTerm *t = user;
    for (size_t i = 0; i < len; ) {
        unsigned char b = (unsigned char)utf8[i];
        uint32_t cp;
        if (b < 0x80) { cp = b; i += 1; }
        else if ((b & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(utf8[i + 1] & 0x3F); i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(utf8[i + 1] & 0x3F) << 6)
               | (uint32_t)(utf8[i + 2] & 0x3F); i += 3;
        } else { i += 1; continue; }
        grid_put_cp(t, cp);
    }
}

static void
term_set_cursor(void *user, int32_t row, int32_t col)
{
    AxlConsoleTerm *t = user;
    mark_dirty(t, t->cur_row);
    t->cur_row = (row < 0) ? 0 : ((uint32_t)row >= t->rows ? (int32_t)t->rows - 1 : row);
    t->cur_col = (col < 0) ? 0 : ((uint32_t)col >= t->cols ? (int32_t)t->cols - 1 : col);
    mark_dirty(t, t->cur_row);
}

static void
term_set_pen(void *user, const AxlConsolePen *pen)
{
    AxlConsoleTerm *t = user;
    /* A producer only ever emits INDEXED (UEFI nibble) fg/bg. */
    if (pen->fg.kind == AXL_CONSOLE_COLOR_INDEXED) { t->pen_fg = pen->fg.idx & 0x0F; }
    if (pen->bg.kind == AXL_CONSOLE_COLOR_INDEXED) { t->pen_bg = pen->bg.idx & 0x07; }
}

static void
term_clear_screen(void *user)
{
    grid_clear(user);
}

static int
term_set_term_prop(void *user, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    AxlConsoleTerm *t = user;
    if (prop == AXL_CONSOLE_PROP_CURSOR_VISIBLE && val->kind == AXL_CONSOLE_VALUE_BOOL) {
        t->cursor_visible = val->u.boolean;
        mark_dirty(t, t->cur_row);
    }
    return 1;   /* accept everything, including props we ignore */
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AxlConsoleTerm *
axl_console_term_new(const AxlConsoleTermConfig *cfg)
{
    AxlConsoleTermConfig zero = {0};
    if (cfg == NULL) { cfg = &zero; }

    AxlConsoleTerm *t = axl_calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }

    t->font = (cfg->font != NULL) ? cfg->font : axl_gfx_default_font();
    t->cell_w = t->font->cell_width;
    t->cell_h = t->font->cell_height;
    t->target = cfg->target;
    t->bx = cfg->x;
    t->by = cfg->y;
    t->bw = cfg->w;
    t->bh = cfg->h;
    t->on_zoom = cfg->on_zoom;
    t->cb_user = cfg->cb_user;

    /* Geometry: explicit cols/rows win; a 0 axis is auto-derived from the render
       extent (or the target buffer / GOP edge, minus the origin) and the font's cell
       size, falling back to the 80x25 default when there is no measurable surface. */
    t->cols = cfg->cols;
    t->rows = cfg->rows;
    if (t->cols == 0 || t->rows == 0) {
        uint32_t avail_w = 0, avail_h = 0;
        if (cfg->target != NULL) {
            axl_gfx_buffer_get_info(cfg->target, &avail_w, &avail_h);
        } else {
            AxlGfxInfo gi;
            if (axl_gfx_get_info(&gi) == AXL_OK) { avail_w = gi.width; avail_h = gi.height; }
        }
        uint32_t ext_w = (cfg->w != 0) ? cfg->w : (avail_w > cfg->x ? avail_w - cfg->x : 0);
        uint32_t ext_h = (cfg->h != 0) ? cfg->h : (avail_h > cfg->y ? avail_h - cfg->y : 0);
        if (t->cols == 0 && t->cell_w != 0) { t->cols = ext_w / t->cell_w; }
        if (t->rows == 0 && t->cell_h != 0) { t->rows = ext_h / t->cell_h; }
    }
    if (t->cols == 0) { t->cols = TERM_DEFAULT_COLS; }
    if (t->rows == 0) { t->rows = TERM_DEFAULT_ROWS; }

    t->pen_fg = 7;   /* light gray on black */
    t->pen_bg = 0;
    t->cursor_visible = true;

    for (uint32_t i = 0; i < 16; i++) {
        t->palette[i] = (cfg->palette != NULL) ? cfg->palette[i] : TERM_PALETTE[i];
    }

    t->scrollback_rows = (cfg->scrollback_rows != 0) ? cfg->scrollback_rows
                                                     : TERM_DEFAULT_SCROLLBACK;

    t->screen    = axl_calloc((size_t)t->rows * t->cols, sizeof(TermCell));
    t->dirty_row = axl_calloc(t->rows, sizeof(bool));
    t->shadow    = axl_calloc((size_t)t->rows * t->cols, sizeof(ShadowCell));
    t->history   = axl_calloc((size_t)t->scrollback_rows * t->cols, sizeof(TermCell));
    if (t->screen == NULL || t->dirty_row == NULL || t->shadow == NULL
        || t->history == NULL) {
        axl_free(t->screen);
        axl_free(t->dirty_row);
        axl_free(t->shadow);
        axl_free(t->history);
        axl_free(t);
        return NULL;
    }
    /* Cells start blank with the current pen (calloc already zeroed len/fg/bg to 0,
       which is blank + fg 0; set fg to the default pen so a first render matches). */
    for (uint32_t i = 0; i < t->rows * t->cols; i++) {
        t->screen[i].fg = t->pen_fg;
        t->screen[i].bg = t->pen_bg;
    }

    t->ops.output_text   = term_output_text;
    t->ops.set_cursor    = term_set_cursor;
    t->ops.set_pen       = term_set_pen;
    t->ops.clear_screen  = term_clear_screen;
    t->ops.set_term_prop = term_set_term_prop;

    /* Optional software mouse cursor, bound to the same target the cells render into
       (NULL => save-under against the GOP). NULL-tolerant: a failure to create it (OOM,
       or save-under with no GOP) just leaves the terminal cursor-less. */
    if (cfg->mouse_cursor) {
        t->cursor = axl_cursor_new(t->target);
    }

    return t;
}

void
axl_console_term_free(AxlConsoleTerm *t)
{
    if (t == NULL) {
        return;
    }
    axl_cursor_free(t->cursor);
    axl_free(t->screen);
    axl_free(t->dirty_row);
    axl_free(t->shadow);
    axl_free(t->history);
    axl_free(t);
}

void
axl_console_term_scroll(AxlConsoleTerm *t, int32_t delta_rows)
{
    if (t == NULL) {
        return;
    }
    int64_t off = (int64_t)t->scroll_off + delta_rows;
    if (off < 0) { off = 0; }
    if (off > (int64_t)t->hist_fill) { off = (int64_t)t->hist_fill; }
    if ((uint32_t)off != t->scroll_off) {
        t->scroll_off = (uint32_t)off;
        mark_all_dirty(t);   /* the whole viewport moved */
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

/* The cell shown at viewport row @a vrow, col @a col. The viewport is a rows-high
   window over the combined [history .. live screen] buffer whose bottom edge sits
   scroll_off rows above the live bottom; rows above the live screen come from the
   history ring (oldest first). */
static const TermCell *
viewport_cell(AxlConsoleTerm *t, uint32_t vrow, uint32_t col)
{
    int64_t combined = (int64_t)t->hist_fill - (int64_t)t->scroll_off + (int64_t)vrow;
    if (combined < (int64_t)t->hist_fill) {
        uint32_t rows_back = t->hist_fill - (uint32_t)combined;   /* 1 = most recent */
        uint32_t idx = (t->hist_head + t->scrollback_rows - rows_back) % t->scrollback_rows;
        return &t->history[(size_t)idx * t->cols + col];
    }
    return cell_at(t, (int32_t)(combined - (int64_t)t->hist_fill), (int32_t)col);
}

/* Normalize the selection endpoints into reading order: (*sr,*sc) <= (*er,*ec). */
static void
selection_bounds(const AxlConsoleTerm *t,
                 uint32_t *sr, uint32_t *sc, uint32_t *er, uint32_t *ec)
{
    bool a_first = t->sel_a_row < t->sel_b_row
                || (t->sel_a_row == t->sel_b_row && t->sel_a_col <= t->sel_b_col);
    *sr = a_first ? t->sel_a_row : t->sel_b_row;
    *sc = a_first ? t->sel_a_col : t->sel_b_col;
    *er = a_first ? t->sel_b_row : t->sel_a_row;
    *ec = a_first ? t->sel_b_col : t->sel_a_col;
}

/* Is viewport cell (row,col) inside the active selection? Full rows between the
   endpoints are selected edge to edge; the first/last rows start/stop at the col. */
/* True when the selection covers more than its anchor -- a click (anchor == free
   end) selects nothing, so it neither highlights nor copies; only a drag does. */
static bool
selection_nonempty(const AxlConsoleTerm *t)
{
    return t->sel_active
        && !(t->sel_a_row == t->sel_b_row && t->sel_a_col == t->sel_b_col);
}

static bool
cell_selected(const AxlConsoleTerm *t, uint32_t row, uint32_t col)
{
    if (!selection_nonempty(t)) {
        return false;
    }
    uint32_t sr, sc, er, ec;
    selection_bounds(t, &sr, &sc, &er, &ec);
    if (row < sr || row > er) {
        return false;
    }
    uint32_t lo = (row == sr) ? sc : 0;
    uint32_t hi = (row == er) ? ec : (t->cols - 1);
    return col >= lo && col <= hi;
}

/* Take the mouse cursor off the target so the cell blits land on clean pixels, and put
   it back atop them afterward. The two AxlCursor modes bracket oppositely: save-under
   (GOP target) keeps the arrow ON the target between frames, so lift ERASES it and drop
   REDRAWS it; scene-fold (buffer target) keeps the target clean between frames, so drop
   UNFOLDS the previous frame's arrow and lift FOLDS this frame's back in (leaving it in
   the buffer for the host to present). See axl_cursor_lift/_drop. */
static void
cursor_frame_begin(AxlConsoleTerm *t)
{
    if (t->cursor == NULL) {
        return;
    }
    if (t->target == NULL) { axl_cursor_lift(t->cursor); }   /* save-under: erase */
    else                   { axl_cursor_drop(t->cursor); }   /* scene-fold: unfold prev */
}

static void
cursor_frame_end(AxlConsoleTerm *t)
{
    if (t->cursor == NULL) {
        return;
    }
    if (t->target == NULL) { axl_cursor_drop(t->cursor); }   /* save-under: redraw */
    else                   { axl_cursor_lift(t->cursor); }   /* scene-fold: fold in */
}

void
axl_console_term_render(AxlConsoleTerm *t)
{
    if (t == NULL) {
        return;
    }
    /* Decide whether to bracket the mouse cursor this frame. A save-under cursor
       (GOP target) persists on the screen between frames -- set_pointer already
       redrew it -- so an idle frame (no dirty rows) needs no erase+redraw of the
       footprint; only a frame that actually blits cells must lift it out of the way.
       A scene-fold cursor (buffer target) is different: the host presents the buffer
       every frame, so the arrow must be folded back in EVERY frame, idle or not. */
    bool any_dirty = false;
    for (uint32_t r = 0; r < t->rows; r++) {
        if (t->dirty_row[r]) { any_dirty = true; break; }
    }
    bool bracket = (t->target != NULL) || any_dirty;
    if (bracket) {
        cursor_frame_begin(t);   /* clear the cursor footprint before the cell blits */
    }
    AxlGfxBuffer *prev = axl_gfx_get_current_target();
    axl_gfx_target_buffer(t->target);   /* NULL target => the GOP screen */

    /* Confine every draw to the render bounds when an extent is set (0 = to the
       target/GOP edge, i.e. no clip). Keeps a sub-region host (a compositor placing
       the terminal in a slot) from spilling past its rectangle. */
    bool clipped = (t->bw != 0 && t->bh != 0);
    if (clipped) {
        AxlGfxClip clip = { .x = (int32_t)t->bx, .y = (int32_t)t->by,
                            .w = t->bw, .h = t->bh };
        clipped = (axl_gfx_push_clip(clip) == AXL_OK);
    }

    uint32_t cw = t->cell_w;
    uint32_t ch = t->cell_h;
    char     buf[TERM_CELL_MAX + 1];

    /* The cursor caret is folded into the per-cell render (an underline on the cursor
       cell) so it participates in the damage shadow: it is erased when the cursor
       leaves a cell and not redundantly re-blitted every frame. */
    bool caret_row_live = t->cursor_visible && t->scroll_off == 0
        && t->cur_row >= 0 && (uint32_t)t->cur_row < t->rows
        && t->cur_col >= 0 && (uint32_t)t->cur_col < t->cols;

    for (uint32_t r = 0; r < t->rows; r++) {
        if (!t->dirty_row[r]) {
            continue;
        }
        uint32_t y = t->by + r * ch;
        for (uint32_t c = 0; c < t->cols; c++) {
            const TermCell *cell = viewport_cell(t, r, c);
            uint8_t fg = cell->fg;
            uint8_t bg = cell->bg;
            if (cell_selected(t, r, c)) { uint8_t tmp = fg; fg = bg; bg = tmp; }
            uint8_t caret = (caret_row_live && (uint32_t)t->cur_row == r
                             && (uint32_t)t->cur_col == c) ? 1 : 0;

            /* Damage check: skip the blit when this cell already shows exactly this. */
            ShadowCell *sh = &t->shadow[(size_t)r * t->cols + c];
            if (sh->valid && sh->len == cell->len && sh->fg == fg && sh->bg == bg
                && sh->caret == caret) {
                bool same = true;
                for (uint8_t k = 0; k < cell->len; k++) {
                    if (sh->utf8[k] != cell->utf8[k]) { same = false; break; }
                }
                if (same) {
                    continue;
                }
            }

            uint32_t x = t->bx + c * cw;
            axl_gfx_fill_rect(x, y, cw, ch, t->palette[bg]);
            if (cell->len != 0) {
                for (uint8_t k = 0; k < cell->len; k++) { buf[k] = cell->utf8[k]; }
                buf[cell->len] = '\0';
                axl_gfx_draw_text(t->font, x, y, buf, t->palette[fg], 1);
            }
            if (caret) {
                axl_gfx_fill_rect(x, y + ch - 2, cw, 2, t->palette[fg]);
            }

            /* Record what we just drew. */
            for (uint8_t k = 0; k < cell->len; k++) { sh->utf8[k] = cell->utf8[k]; }
            sh->len = cell->len; sh->fg = fg; sh->bg = bg; sh->caret = caret;
            sh->valid = 1;
        }
        t->dirty_row[r] = false;
    }

    if (clipped) {
        axl_gfx_pop_clip();
    }
    axl_gfx_target_buffer(prev);   /* restore the caller's draw target */
    if (bracket) {
        cursor_frame_end(t);       /* re-lay the mouse cursor atop the fresh cells */
    }
}

// ---------------------------------------------------------------------------
// Reflow
// ---------------------------------------------------------------------------

void
axl_console_term_set_font(AxlConsoleTerm *t, const AxlFont *font)
{
    if (t == NULL || font == NULL) {
        return;
    }
    t->font   = font;
    t->cell_w = font->cell_width;
    t->cell_h = font->cell_height;
    shadow_invalidate(t);   /* new glyph metrics -> every cell's pixels change */
    mark_all_dirty(t);
}

void
axl_console_term_resize(AxlConsoleTerm *t, uint32_t cols, uint32_t rows)
{
    if (t == NULL || cols == 0 || rows == 0) {
        return;
    }
    if (cols == t->cols && rows == t->rows) {
        return;
    }

    /* History rows are @a cols wide, so a column change invalidates the ring. Allocate
       EVERY new buffer up front and commit only once they all succeed: an OOM must
       leave the terminal fully consistent -- committing a new @a cols with the old
       (narrower) history would overflow the ring on the next scroll. */
    bool        cols_changed = (cols != t->cols);
    TermCell   *new_screen   = axl_calloc((size_t)rows * cols, sizeof(TermCell));
    bool       *new_dirty    = axl_calloc(rows, sizeof(bool));
    ShadowCell *new_shadow   = axl_calloc((size_t)rows * cols, sizeof(ShadowCell));
    TermCell   *new_hist      = cols_changed
        ? axl_calloc((size_t)t->scrollback_rows * cols, sizeof(TermCell)) : NULL;
    if (new_screen == NULL || new_dirty == NULL || new_shadow == NULL
        || (cols_changed && new_hist == NULL)) {
        axl_free(new_screen);
        axl_free(new_dirty);
        axl_free(new_shadow);
        axl_free(new_hist);
        return;   /* leave the terminal unchanged on OOM */
    }

    /* Blank every new cell in the current pen, then copy the overlapping region. */
    for (uint32_t i = 0; i < rows * cols; i++) {
        new_screen[i].fg = t->pen_fg;
        new_screen[i].bg = t->pen_bg;
    }
    uint32_t copy_rows = (rows < t->rows) ? rows : t->rows;
    uint32_t copy_cols = (cols < t->cols) ? cols : t->cols;
    for (uint32_t r = 0; r < copy_rows; r++) {
        for (uint32_t c = 0; c < copy_cols; c++) {
            new_screen[r * cols + c] = t->screen[r * t->cols + c];
        }
    }

    axl_free(t->screen);
    axl_free(t->dirty_row);
    axl_free(t->shadow);
    t->screen    = new_screen;
    t->dirty_row = new_dirty;
    t->shadow    = new_shadow;   /* fresh (valid=0) -> next render re-blits everything */

    if (cols_changed) {
        axl_free(t->history);
        t->history = new_hist;
        t->hist_head = 0;
        t->hist_fill = 0;
        t->scroll_off = 0;
    }

    t->cols = cols;
    t->rows = rows;
    if ((uint32_t)t->cur_row >= rows) { t->cur_row = (int32_t)rows - 1; }
    if ((uint32_t)t->cur_col >= cols) { t->cur_col = (int32_t)cols - 1; }
    mark_all_dirty(t);
}

void
axl_console_term_set_bounds(AxlConsoleTerm *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (t == NULL) {
        return;
    }
    t->bx = x;
    t->by = y;
    t->bw = w;
    t->bh = h;
    shadow_invalidate(t);   /* new origin -> every cell moves */
    mark_all_dirty(t);
}

void
axl_console_term_set_palette(AxlConsoleTerm *t, const AxlGfxPixel *palette)
{
    if (t == NULL || palette == NULL) {
        return;
    }
    for (uint32_t i = 0; i < 16; i++) {
        t->palette[i] = palette[i];
    }
    shadow_invalidate(t);   /* same indices, different colours -> re-blit */
    mark_all_dirty(t);
}

// ---------------------------------------------------------------------------
// Selection + copy
// ---------------------------------------------------------------------------

void
axl_console_term_selection_start(AxlConsoleTerm *t, uint32_t col, uint32_t row)
{
    if (t == NULL) {
        return;
    }
    t->sel_active = true;
    t->sel_a_row = row;
    t->sel_a_col = col;
    t->sel_b_row = row;
    t->sel_b_col = col;
    mark_all_dirty(t);
}

void
axl_console_term_selection_extend(AxlConsoleTerm *t, uint32_t col, uint32_t row)
{
    if (t == NULL || !t->sel_active) {
        return;
    }
    t->sel_b_row = row;
    t->sel_b_col = col;
    mark_all_dirty(t);
}

void
axl_console_term_selection_clear(AxlConsoleTerm *t)
{
    if (t == NULL || !t->sel_active) {
        return;
    }
    t->sel_active = false;
    mark_all_dirty(t);
}

int
axl_console_term_selection_copy(AxlConsoleTerm *t)
{
    if (t == NULL || !selection_nonempty(t)) {
        return AXL_ERR;   /* no selection, or a click with no drag */
    }
    uint32_t sr, sc, er, ec;
    selection_bounds(t, &sr, &sc, &er, &ec);

    AxlString *b = axl_string_new("");
    if (b == NULL) {
        return AXL_ERR;
    }
    for (uint32_t row = sr; row <= er; row++) {
        uint32_t lo = (row == sr) ? sc : 0;
        uint32_t hi = (row == er) ? ec : (t->cols - 1);
        /* Find the last non-blank cell so trailing blanks can be trimmed. */
        uint32_t last = lo;
        bool     any  = false;
        for (uint32_t col = lo; col <= hi; col++) {
            if (viewport_cell(t, row, col)->len != 0) { last = col; any = true; }
        }
        if (any) {
            for (uint32_t col = lo; col <= last; col++) {
                const TermCell *cell = viewport_cell(t, row, col);
                int ar = (cell->len == 0)
                    ? axl_string_append_c(b, ' ')   /* interior blank preserved */
                    : axl_string_append_len(b, cell->utf8, cell->len);
                if (ar != AXL_OK) {   /* OOM mid-build: don't ship a truncated copy */
                    axl_string_free(b);
                    return AXL_ERR;
                }
            }
        }
        if (row != er && axl_string_append_c(b, '\n') != AXL_OK) {
            axl_string_free(b);
            return AXL_ERR;
        }
    }
    int rc = axl_clipboard_set(axl_string_str(b), axl_string_len(b), "text/plain");
    axl_string_free(b);
    return rc;
}

// ---------------------------------------------------------------------------
// Interaction conveniences
// ---------------------------------------------------------------------------

/* Map a target-space pixel to a viewport cell. false if outside the grid. */
static bool
pixel_to_cell(const AxlConsoleTerm *t, int32_t px, int32_t py,
              uint32_t *col, uint32_t *row)
{
    if (t->cell_w == 0 || t->cell_h == 0
        || px < (int32_t)t->bx || py < (int32_t)t->by) {
        return false;
    }
    /* Reject clicks past the render extent when one is set (0 = to the edge). */
    if ((t->bw != 0 && (uint32_t)px >= t->bx + t->bw)
        || (t->bh != 0 && (uint32_t)py >= t->by + t->bh)) {
        return false;
    }
    uint32_t c = ((uint32_t)px - t->bx) / t->cell_w;
    uint32_t r = ((uint32_t)py - t->by) / t->cell_h;
    if (c >= t->cols || r >= t->rows) {
        return false;
    }
    *col = c;
    *row = r;
    return true;
}

void
axl_console_term_handle_pointer(AxlConsoleTerm *t, const AxlInputEvent *e)
{
    if (t == NULL || e == NULL) {
        return;
    }
    uint32_t col, row;
    switch (e->type) {
    case AXL_INPUT_MOUSE_WHEEL:
        if ((e->modifiers & AXL_INPUT_MOD_CTRL) != 0) {
            if (t->on_zoom != NULL) {
                t->on_zoom(t->cb_user, e->wheel_dy);   /* consumer owns zoom policy */
            }
        } else {
            axl_console_term_scroll(t, e->wheel_dy);    /* +up = back into history */
        }
        break;
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
        if ((e->buttons & AXL_INPUT_BUTTON_LEFT) != 0
            && pixel_to_cell(t, e->x, e->y, &col, &row)) {
            axl_console_term_selection_start(t, col, row);
        }
        break;
    case AXL_INPUT_MOUSE_MOVE:
        /* Track the mouse cursor (no-op if off). Motion re-shows a hidden cursor by
           design: a moving pointer is a present pointer, so hide_pointer stays in
           effect only until the pointer moves again. */
        axl_console_term_set_pointer(t, e->x, e->y);
        if (e->dragging && t->sel_active && pixel_to_cell(t, e->x, e->y, &col, &row)) {
            axl_console_term_selection_extend(t, col, row);
        }
        break;
    case AXL_INPUT_MOUSE_BUTTON_UP:
        /* A click with no drag leaves a zero-length selection (anchor == free end);
           collapse it so a stray click doesn't leave a cell inverted forever. A real
           drag moved the free end away, so it survives. */
        if (t->sel_active && t->sel_a_row == t->sel_b_row && t->sel_a_col == t->sel_b_col) {
            axl_console_term_selection_clear(t);
        }
        break;
    default:
        break;
    }
}

void
axl_console_term_set_pointer(AxlConsoleTerm *t, int32_t px, int32_t py)
{
    if (t == NULL || t->cursor == NULL) {
        return;
    }
    /* The render bracket leaves a scene-fold cursor folded into the buffer between
       frames (so a buffer-presenting host shows it). Unfold before moving so the move
       recomposites from a clean scene; the next render re-folds at the new spot. No-op
       for the save-under (GOP) cursor, which the bracket leaves un-lifted. */
    axl_cursor_drop(t->cursor);
    axl_cursor_move(t->cursor, px, py);
    axl_cursor_show(t->cursor);
}

void
axl_console_term_hide_pointer(AxlConsoleTerm *t)
{
    if (t == NULL) {
        return;
    }
    axl_cursor_hide(t->cursor);   /* NULL-safe */
}

bool
axl_console_term_handle_hotkey(AxlConsoleTerm *t, const void *key)
{
    if (t == NULL || key == NULL) {
        return false;
    }
    const EFI_KEY_DATA *kd = key;
    uint32_t shift = kd->KeyState.KeyShiftState;
    bool has_shift = (shift & (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED)) != 0;
    bool has_ctrl  = (shift & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED)) != 0;
    uint16_t scan = kd->Key.ScanCode;
    uint16_t uni  = kd->Key.UnicodeChar;

    if (has_shift && scan == TERM_SCAN_PAGE_UP) {
        axl_console_term_scroll(t, (int32_t)t->rows);
        return true;
    }
    if (has_shift && scan == TERM_SCAN_PAGE_DOWN) {
        axl_console_term_scroll(t, -(int32_t)t->rows);
        return true;
    }
    /* Ctrl+Shift+C = copy (Ctrl+C alone stays SIGINT for the shell). Firmware maps
       Ctrl+C to UnicodeChar 3; accept the raw letter too. */
    if (has_ctrl && has_shift && (uni == 3 || uni == 'C' || uni == 'c')) {
        (void)axl_console_term_selection_copy(t);
        return true;
    }
    return false;
}

const AxlConsoleOps *
axl_console_term_ops(AxlConsoleTerm *t, void **user)
{
    if (t == NULL) {
        return NULL;
    }
    if (user != NULL) {
        *user = t;
    }
    return &t->ops;
}

// ---------------------------------------------------------------------------
// Test seams (no public header). Read the cell model + cursor a headless unit test
// asserts against, without a GOP.
// ---------------------------------------------------------------------------

bool
_axl_console_term_test_cell(AxlConsoleTerm *t, uint32_t row, uint32_t col,
                            char *utf8_out, uint8_t *fg, uint8_t *bg)
{
    if (t == NULL || row >= t->rows || col >= t->cols) {
        return false;
    }
    TermCell *c = cell_at(t, (int32_t)row, (int32_t)col);
    if (utf8_out != NULL) {
        for (uint8_t i = 0; i < c->len; i++) { utf8_out[i] = c->utf8[i]; }
        utf8_out[c->len] = '\0';
    }
    if (fg != NULL) { *fg = c->fg; }
    if (bg != NULL) { *bg = c->bg; }
    return true;
}

void
_axl_console_term_test_cursor(AxlConsoleTerm *t, uint32_t *row, uint32_t *col)
{
    if (t == NULL) {
        return;
    }
    if (row != NULL) { *row = (uint32_t)t->cur_row; }
    if (col != NULL) { *col = (uint32_t)t->cur_col; }
}

uint32_t
_axl_console_term_test_scroll_off(AxlConsoleTerm *t)
{
    return (t != NULL) ? t->scroll_off : 0;
}

/* Read a scrollback row @a rows_back rows above the live top (1 = the most recently
   scrolled-off line, hist_fill = the oldest). Returns its glyph at @a col. */
bool
_axl_console_term_test_hist_cell(AxlConsoleTerm *t, uint32_t rows_back, uint32_t col,
                                 char *utf8_out)
{
    if (t == NULL || rows_back == 0 || rows_back > t->hist_fill || col >= t->cols) {
        return false;
    }
    uint32_t idx = (t->hist_head + t->scrollback_rows - rows_back) % t->scrollback_rows;
    TermCell *cell = &t->history[(size_t)idx * t->cols + col];
    if (utf8_out != NULL) {
        for (uint8_t i = 0; i < cell->len; i++) { utf8_out[i] = cell->utf8[i]; }
        utf8_out[cell->len] = '\0';
    }
    return true;
}

void
_axl_console_term_test_geometry(AxlConsoleTerm *t,
        uint32_t *cols, uint32_t *rows, uint32_t *cw, uint32_t *ch,
        uint32_t *bx, uint32_t *by, uint32_t *bw, uint32_t *bh)
{
    if (t == NULL) {
        return;
    }
    if (cols != NULL) { *cols = t->cols; }
    if (rows != NULL) { *rows = t->rows; }
    if (cw != NULL)   { *cw = t->cell_w; }
    if (ch != NULL)   { *ch = t->cell_h; }
    if (bx != NULL)   { *bx = t->bx; }
    if (by != NULL)   { *by = t->by; }
    if (bw != NULL)   { *bw = t->bw; }
    if (bh != NULL)   { *bh = t->bh; }
}

AxlGfxPixel
_axl_console_term_test_palette(AxlConsoleTerm *t, uint32_t idx)
{
    AxlGfxPixel zero = {0};
    if (t == NULL || idx >= 16) {
        return zero;
    }
    return t->palette[idx];
}

/* Report the mouse-cursor overlay's position + visibility. No cursor => hidden at
   (0,0), so a mouse_cursor-off terminal reads deterministically. */
void
_axl_console_term_test_pointer(AxlConsoleTerm *t, int32_t *x, int32_t *y, bool *visible)
{
    int32_t cx = 0, cy = 0;
    if (t != NULL && t->cursor != NULL) {
        axl_cursor_position(t->cursor, &cx, &cy);
    }
    if (x != NULL) { *x = cx; }
    if (y != NULL) { *y = cy; }
    if (visible != NULL) { *visible = (t != NULL) && axl_cursor_visible(t->cursor); }
}
