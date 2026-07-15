/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-screen.c
    Server-side terminal screen model + self-contained snapshot serializer.
    See axl-console-screen.h and docs/AXL-console-screen-snapshot-handoff.md.

    The model owns a primary and an alternate cell grid, driven by @ref axl_vterm
    (Layer 2) into the currently-active grid — swapping on the guest's alt-screen
    property just as a real terminal does, so the primary survives a full-screen
    app. `snapshot()` walks the active grid and serializes it back to a
    self-contained VT repaint. It is the inverse of `axl-console-mirror`'s encoder,
    and shares its pen->SGR / cursor-address vocabulary.
**/

#include <axl/axl-console-screen.h>
#include <axl/axl-console-ops.h>
#include <axl/axl-vterm.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "../util/axl-console-vt.h"   /* shared pen->SGR encoder (DRY with the mirror) */

#include <stdint.h>

#define SCREEN_CELL_MAX  7   /* bytes of UTF-8 per cell: a base glyph + combining marks */

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    char          utf8[SCREEN_CELL_MAX];   /* not NUL-terminated; @len bytes valid */
    uint8_t       len;                     /* 0 = blank */
    bool          cont;                    /* right half of a double-width glyph */
    AxlConsolePen pen;                     /* full graphic rendition */
} ScreenCell;

struct AxlConsoleScreen {
    AxlVterm     *vt;              /* the Layer-2 parser driving the grid */
    uint32_t      rows;
    uint32_t      cols;

    ScreenCell   *primary;        /* rows*cols, row-major */
    ScreenCell   *alternate;      /* rows*cols, row-major */
    ScreenCell   *active;         /* == primary or alternate */

    int32_t       cur_row;
    int32_t       cur_col;
    bool          cursor_visible;
    bool          alt;            /* alternate screen active */
    bool          reverse;        /* whole-screen reverse video (DECSCNM) */
    bool          width_resolved; /* set_cell_rule reported width-resolved */
    AxlConsolePen pen;            /* current pen (the next glyph's rendition) */
};

typedef struct {
    AxlConsoleScreenSink sink;
    void                *user;
} Emitter;

// ---------------------------------------------------------------------------
// Pen helpers
// ---------------------------------------------------------------------------

static bool
color_eq(const AxlConsoleColor *a, const AxlConsoleColor *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind == AXL_CONSOLE_COLOR_INDEXED) {
        return a->idx == b->idx;
    }
    if (a->kind == AXL_CONSOLE_COLOR_RGB) {
        return a->r == b->r && a->g == b->g && a->b == b->b;
    }
    return true;   /* DEFAULT carries no payload */
}

static bool
pen_eq(const AxlConsolePen *a, const AxlConsolePen *b)
{
    return color_eq(&a->fg, &b->fg) && color_eq(&a->bg, &b->bg)
        && a->underline == b->underline && a->bold == b->bold
        && a->italic == b->italic && a->blink == b->blink
        && a->reverse == b->reverse && a->conceal == b->conceal
        && a->strike == b->strike;
}

/* The pen an EMPTY cell carries. A blank has no glyph, so its foreground and text
   attributes are invisible and meaningless — only the background survives. Storing
   the full pen on an erased cell (e.g. a foreground colour still active when the
   screen was cleared) would make it a non-default, and therefore spuriously
   repainted, cell. */
static AxlConsolePen
blank_pen(const AxlConsolePen *cur)
{
    AxlConsolePen p = {0};   /* default fg + no attributes */
    p.bg = cur->bg;
    return p;
}

static bool
pen_is_default(const AxlConsolePen *p)
{
    return p->fg.kind == AXL_CONSOLE_COLOR_DEFAULT
        && p->bg.kind == AXL_CONSOLE_COLOR_DEFAULT
        && p->underline == AXL_CONSOLE_UNDERLINE_OFF
        && !p->bold && !p->italic && !p->blink && !p->reverse
        && !p->conceal && !p->strike;
}

// ---------------------------------------------------------------------------
// Grid ops — an AxlConsoleOps consumer driven by the internal axl_vterm.
// ---------------------------------------------------------------------------

static ScreenCell *
cell_at(AxlConsoleScreen *s, int32_t row, int32_t col)
{
    return &s->active[(uint32_t)row * s->cols + (uint32_t)col];
}

static void
screen_set_cell_rule(void *user, AxlConsoleCellRule rule)
{
    AxlConsoleScreen *s = user;
    s->width_resolved = (rule == AXL_CONSOLE_CELLS_WIDTH_RESOLVED);
}

static void
screen_set_cursor(void *user, int32_t row, int32_t col)
{
    AxlConsoleScreen *s = user;
    s->cur_row = (row < 0) ? 0
               : ((uint32_t)row >= s->rows ? (int32_t)s->rows - 1 : row);
    s->cur_col = (col < 0) ? 0
               : ((uint32_t)col >= s->cols ? (int32_t)s->cols - 1 : col);
}

static void
screen_set_pen(void *user, const AxlConsolePen *pen)
{
    AxlConsoleScreen *s = user;
    s->pen = *pen;
}

static void
screen_output_text(void *user, const char *utf8, size_t len)
{
    AxlConsoleScreen *s = user;
    for (size_t i = 0; i < len; ) {
        unsigned char b = (unsigned char)utf8[i];
        uint32_t cp;
        size_t   nb;
        if (b < 0x80) {
            cp = b; nb = 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(utf8[i + 1] & 0x3F);
            nb = 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(utf8[i + 1] & 0x3F) << 6)
               | (uint32_t)(utf8[i + 2] & 0x3F);
            nb = 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < len) {
            cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(utf8[i + 1] & 0x3F) << 12)
               | ((uint32_t)(utf8[i + 2] & 0x3F) << 6) | (uint32_t)(utf8[i + 3] & 0x3F);
            nb = 4;
        } else {
            i += 1;   /* invalid lead byte: skip it */
            continue;
        }

        int w = s->width_resolved ? axl_vterm_char_width(cp) : 1;
        if (w == 0) {
            /* Combining / zero-width: merge into the preceding cell (its base if the
               previous column is a wide glyph's continuation). */
            if (s->cur_col > 0) {
                int32_t pc = s->cur_col - 1;
                if ((uint32_t)pc >= s->cols) {
                    pc = (int32_t)s->cols - 1;   /* cursor parked past the margin */
                }
                if (cell_at(s, s->cur_row, pc)->cont && pc > 0) {
                    pc -= 1;
                }
                ScreenCell *prev = cell_at(s, s->cur_row, pc);
                if ((size_t)prev->len + nb <= SCREEN_CELL_MAX) {
                    axl_memcpy(prev->utf8 + prev->len, utf8 + i, nb);
                    prev->len = (uint8_t)(prev->len + nb);
                }
            }
            i += nb;
            continue;
        }

        if ((uint32_t)s->cur_col >= s->cols) {
            i += nb;   /* no room; a coalesced run never spans a row, so this is a guard */
            continue;
        }
        ScreenCell *cell = cell_at(s, s->cur_row, s->cur_col);
        axl_memcpy(cell->utf8, utf8 + i, nb);
        cell->len  = (uint8_t)nb;
        cell->cont = false;
        cell->pen  = s->pen;
        if (w == 2 && (uint32_t)(s->cur_col + 1) < s->cols) {
            ScreenCell *rhs = cell_at(s, s->cur_row, s->cur_col + 1);
            rhs->len  = 0;
            rhs->cont = true;
            rhs->pen  = s->pen;
        }
        s->cur_col += w;
        i += nb;
    }
}

static void
screen_erase(void *user, AxlConsoleRect rect, bool selective)
{
    AxlConsoleScreen *s = user;
    (void)selective;   /* no DECSCA protection tracked */
    for (int32_t r = rect.start_row; r < rect.end_row; r++) {
        if (r < 0 || (uint32_t)r >= s->rows) {
            continue;
        }
        for (int32_t c = rect.start_col; c < rect.end_col; c++) {
            if (c < 0 || (uint32_t)c >= s->cols) {
                continue;
            }
            ScreenCell *cell = cell_at(s, r, c);
            cell->len  = 0;
            cell->cont = false;
            cell->pen  = blank_pen(&s->pen);   /* blank keeps only the background */
        }
    }
}

static void
screen_moverect(void *user, AxlConsoleRect dest, AxlConsoleRect src)
{
    AxlConsoleScreen *s = user;
    int32_t h = dest.end_row - dest.start_row;
    int32_t w = dest.end_col - dest.start_col;
    if (h <= 0 || w <= 0) {
        return;
    }
    /* Copy in the direction that keeps an overlapping source intact. */
    bool row_desc = dest.start_row > src.start_row;
    bool col_desc = dest.start_col > src.start_col;
    for (int32_t dr = 0; dr < h; dr++) {
        int32_t rr = row_desc ? h - 1 - dr : dr;
        int32_t drow = dest.start_row + rr;
        int32_t srow = src.start_row + rr;
        if (drow < 0 || (uint32_t)drow >= s->rows || srow < 0 || (uint32_t)srow >= s->rows) {
            continue;
        }
        for (int32_t dc = 0; dc < w; dc++) {
            int32_t cc = col_desc ? w - 1 - dc : dc;
            int32_t dcol = dest.start_col + cc;
            int32_t scol = src.start_col + cc;
            if (dcol < 0 || (uint32_t)dcol >= s->cols || scol < 0 || (uint32_t)scol >= s->cols) {
                continue;
            }
            s->active[(uint32_t)drow * s->cols + (uint32_t)dcol] =
                s->active[(uint32_t)srow * s->cols + (uint32_t)scol];
        }
    }
}

static int
screen_set_term_prop(void *user, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    AxlConsoleScreen *s = user;
    switch (prop) {
    case AXL_CONSOLE_PROP_CURSOR_VISIBLE:
        if (val->kind == AXL_CONSOLE_VALUE_BOOL) {
            s->cursor_visible = val->u.boolean;
        }
        return 1;
    case AXL_CONSOLE_PROP_ALT_SCREEN:
        if (val->kind == AXL_CONSOLE_VALUE_BOOL) {
            s->alt    = val->u.boolean;
            s->active = s->alt ? s->alternate : s->primary;
            /* On enter, libvterm follows with a full erase of the now-active
               (alternate) grid; on leave, no erase, so the primary is intact. */
        }
        return 1;
    case AXL_CONSOLE_PROP_REVERSE:
        if (val->kind == AXL_CONSOLE_VALUE_BOOL) {
            s->reverse = val->u.boolean;
        }
        return 1;
    default:
        return 1;   /* accept everything, including props we do not carry */
    }
}

static const AxlConsoleOps screen_ops = {
    .set_cell_rule = screen_set_cell_rule,
    .set_cursor    = screen_set_cursor,
    .set_pen       = screen_set_pen,
    .output_text   = screen_output_text,
    .erase         = screen_erase,
    .moverect      = screen_moverect,
    .set_term_prop = screen_set_term_prop,
    /* scrollrect unbound => a scroll decomposes into moverect + erase (both bound).
       clear_screen / clear_scrollback: axl-vterm never calls the former, and this
       model keeps no scrollback, so CSI 3J is a no-op. */
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int
axl_console_screen_new(AxlConsoleScreen **out, uint32_t rows, uint32_t cols)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL || rows == 0 || cols == 0 || rows > INT32_MAX || cols > INT32_MAX) {
        return AXL_ERR;
    }

    AxlConsoleScreen *s = axl_calloc(1, sizeof(*s));
    if (s == NULL) {
        return AXL_ERR;
    }
    s->rows = rows;
    s->cols = cols;
    s->cursor_visible = true;
    s->primary   = axl_calloc((size_t)rows * cols, sizeof(ScreenCell));
    s->alternate = axl_calloc((size_t)rows * cols, sizeof(ScreenCell));
    if (s->primary == NULL || s->alternate == NULL) {
        axl_free(s->primary);
        axl_free(s->alternate);
        axl_free(s);
        return AXL_ERR;
    }
    s->active = s->primary;

    /* Binds the grid ops and fires set_cell_rule before returning. */
    if (axl_vterm_new(&s->vt, (int32_t)rows, (int32_t)cols, &screen_ops, s) != AXL_OK) {
        axl_free(s->primary);
        axl_free(s->alternate);
        axl_free(s);
        return AXL_ERR;
    }

    *out = s;
    return AXL_OK;
}

void
axl_console_screen_free(AxlConsoleScreen *s)
{
    if (s == NULL) {
        return;
    }
    axl_vterm_free(s->vt);
    axl_free(s->primary);
    axl_free(s->alternate);
    axl_free(s);
}

void
axl_console_screen_feed(AxlConsoleScreen *s, const uint8_t *bytes, size_t len)
{
    if (s == NULL || bytes == NULL) {
        return;
    }
    axl_vterm_feed(s->vt, (const char *)bytes, len);
    axl_vterm_flush(s->vt);   /* keep the grid current so snapshot() stays const */
}

// ---------------------------------------------------------------------------
// Snapshot serializer — walk the active grid, emit a self-contained VT repaint.
// ---------------------------------------------------------------------------

static void
emit(Emitter *e, const char *bytes, size_t len)
{
    if (len > 0) {
        e->sink(bytes, len, e->user);
    }
}

static void
emit_cstr(Emitter *e, const char *str)
{
    emit(e, str, axl_strlen(str));
}

/* The pen snapshot -> a full SGR escape, via the encoder shared with the mirror. */
static void
emit_sgr(Emitter *e, const AxlConsolePen *pen)
{
    char   buf[64];
    size_t n = axl_console_pen_to_sgr(buf, sizeof(buf), pen);
    emit(e, buf, n);
}

static void
emit_cup(Emitter *e, uint32_t row, uint32_t col)
{
    char buf[24];
    int  n = axl_snprintf(buf, sizeof(buf), "\x1b[%u;%uH",
                          (unsigned)(row + 1), (unsigned)(col + 1));
    if (n > 0) {
        emit(e, buf, (size_t)n);
    }
}

/* A cell is emittable when it carries a glyph or a non-default pen. A wide glyph's
   continuation half is never emitted on its own — the base glyph re-advances the
   cursor by two on replay. */
static bool
cell_emittable(const ScreenCell *cell)
{
    return !cell->cont && (cell->len > 0 || !pen_is_default(&cell->pen));
}

/* Repaint one grid: a coalesced clear+cell walk into @p e, tracking the last
   emitted pen in @p *cur so a run of same-pen cells collapses under one SGR. */
static void
emit_grid(Emitter *e, const AxlConsoleScreen *s, const ScreenCell *grid, AxlConsolePen *cur)
{
    for (uint32_t r = 0; r < s->rows; r++) {
        int32_t first = -1, last = -1;
        for (uint32_t c = 0; c < s->cols; c++) {
            if (cell_emittable(&grid[r * s->cols + c])) {
                if (first < 0) {
                    first = (int32_t)c;
                }
                last = (int32_t)c;
            }
        }
        if (first < 0) {
            continue;   /* fully-blank row: emit nothing */
        }
        emit_cup(e, r, (uint32_t)first);
        uint32_t c = (uint32_t)first;
        while ((int32_t)c <= last) {
            const ScreenCell *cell = &grid[r * s->cols + c];
            if (cell->cont) {
                c++;
                continue;
            }
            if (!pen_eq(&cell->pen, cur)) {
                emit_sgr(e, &cell->pen);
                *cur = cell->pen;
            }
            if (cell->len > 0) {
                emit(e, cell->utf8, cell->len);
            } else {
                emit_cstr(e, " ");   /* a non-default-pen blank paints its background */
            }
            /* Skip a wide glyph's continuation half. */
            if (c + 1 < s->cols && grid[r * s->cols + c + 1].cont) {
                c += 2;
            } else {
                c++;
            }
        }
    }
}

int
axl_console_screen_snapshot(const AxlConsoleScreen *s, AxlConsoleScreenSink sink, void *user)
{
    if (s == NULL || sink == NULL) {
        return AXL_ERR;
    }
    Emitter e = { sink, user };
    AxlConsolePen cur = {0};   /* tracks the last emitted pen (default after ESC[m) */

    if (s->reverse) {
        emit_cstr(&e, "\x1b[?5h");
    }
    emit_cstr(&e, "\x1b[2J");   /* clear the primary screen to the default background */
    emit_cstr(&e, "\x1b[m");    /* baseline: default pen */
    emit_grid(&e, s, s->primary, &cur);

    /* When the alternate screen is active, the primary was just repainted and is
       saved by DECSET 1049 (which the joiner's DECRST 1049 restores when the guest
       exits the full-screen app); then repaint the alternate on top. */
    if (s->alt) {
        emit_cstr(&e, "\x1b[?1049h");   /* save primary, switch to (and clear) the alt */
        emit_cstr(&e, "\x1b[2J");       /* deterministic clear of the alternate buffer */
        emit_cstr(&e, "\x1b[m");
        cur = (AxlConsolePen){0};
        emit_grid(&e, s, s->alternate, &cur);
    }

    /* Restore the model's current pen so the late joiner's next write matches. */
    if (!pen_eq(&cur, &s->pen)) {
        emit_sgr(&e, &s->pen);
    }
    emit_cstr(&e, s->cursor_visible ? "\x1b[?25h" : "\x1b[?25l");
    emit_cup(&e, (uint32_t)s->cur_row, (uint32_t)s->cur_col);

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

static void
copy_overlap(ScreenCell *dst, const ScreenCell *src,
             uint32_t dst_cols, uint32_t src_cols,
             uint32_t copy_rows, uint32_t copy_cols)
{
    for (uint32_t r = 0; r < copy_rows; r++) {
        for (uint32_t c = 0; c < copy_cols; c++) {
            dst[r * dst_cols + c] = src[r * src_cols + c];
        }
    }
}

int
axl_console_screen_resize(AxlConsoleScreen *s, uint32_t rows, uint32_t cols)
{
    if (s == NULL || rows == 0 || cols == 0 || rows > INT32_MAX || cols > INT32_MAX) {
        return AXL_ERR;
    }
    if (rows == s->rows && cols == s->cols) {
        return AXL_OK;
    }

    ScreenCell *np = axl_calloc((size_t)rows * cols, sizeof(ScreenCell));
    ScreenCell *na = axl_calloc((size_t)rows * cols, sizeof(ScreenCell));
    if (np == NULL || na == NULL) {
        axl_free(np);
        axl_free(na);
        return AXL_ERR;
    }

    /* Resize the parser first; if it fails, leave the model wholly unchanged. */
    if (axl_vterm_set_size(s->vt, (int32_t)rows, (int32_t)cols) != AXL_OK) {
        axl_free(np);
        axl_free(na);
        return AXL_ERR;
    }

    uint32_t copy_rows = (rows < s->rows) ? rows : s->rows;
    uint32_t copy_cols = (cols < s->cols) ? cols : s->cols;
    copy_overlap(np, s->primary,   cols, s->cols, copy_rows, copy_cols);
    copy_overlap(na, s->alternate, cols, s->cols, copy_rows, copy_cols);

    bool active_is_primary = (s->active == s->primary);
    axl_free(s->primary);
    axl_free(s->alternate);
    s->primary   = np;
    s->alternate = na;
    s->active    = active_is_primary ? np : na;
    s->rows = rows;
    s->cols = cols;
    if ((uint32_t)s->cur_row >= rows) {
        s->cur_row = (int32_t)rows - 1;
    }
    if ((uint32_t)s->cur_col >= cols) {
        s->cur_col = (int32_t)cols - 1;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Test seams (no public header). Read the active grid + state a headless unit
// test asserts the snapshot round-trip against.
// ---------------------------------------------------------------------------

bool
_axl_console_screen_test_cell(const AxlConsoleScreen *s, uint32_t row, uint32_t col,
                              char *utf8_out, AxlConsolePen *pen_out)
{
    if (s == NULL || row >= s->rows || col >= s->cols) {
        return false;
    }
    const ScreenCell *cell = &s->active[row * s->cols + col];
    if (utf8_out != NULL) {
        for (uint8_t i = 0; i < cell->len; i++) {
            utf8_out[i] = cell->utf8[i];
        }
        utf8_out[cell->len] = '\0';
    }
    if (pen_out != NULL) {
        *pen_out = cell->pen;
    }
    return true;
}

void
_axl_console_screen_test_cursor(const AxlConsoleScreen *s,
                                uint32_t *row, uint32_t *col, bool *visible)
{
    if (s == NULL) {
        return;
    }
    if (row != NULL)     { *row = (uint32_t)s->cur_row; }
    if (col != NULL)     { *col = (uint32_t)s->cur_col; }
    if (visible != NULL) { *visible = s->cursor_visible; }
}

bool
_axl_console_screen_test_alt(const AxlConsoleScreen *s)
{
    return s != NULL && s->alt;
}

bool
_axl_console_screen_test_reverse(const AxlConsoleScreen *s)
{
    return s != NULL && s->reverse;
}

void
_axl_console_screen_test_geometry(const AxlConsoleScreen *s, uint32_t *rows, uint32_t *cols)
{
    if (s == NULL) {
        return;
    }
    if (rows != NULL) { *rows = s->rows; }
    if (cols != NULL) { *cols = s->cols; }
}
