/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* axl-vterm — the second producer behind the AxlConsoleOps contract.
 *
 * Adapts vendored libvterm's Layer 2 (VTermStateCallbacks) onto AxlConsoleOps,
 * so a consumer that binds one vtable renders both a UEFI console (via
 * axl-console-tap) and a real VT/xterm byte stream (here). Two jobs bridge the
 * shape mismatch between libvterm's Layer 2 and the contract:
 *
 *   - Coalescing. libvterm's putglyph is a POSITIONED SINGLE GLYPH; AxlConsoleOps
 *     output_text is a cursor-relative run. We buffer consecutive glyphs at
 *     advancing positions and flush on a position jump, a pen change, or any other
 *     op. A logical cursor is tracked so a movecursor to where a run already left
 *     the cursor emits no redundant set_cursor.
 *
 *   - Pen accumulation. libvterm hands attributes incrementally (initpen +
 *     setpenattr); AxlConsolePen is a snapshot. We accumulate here and emit set_pen
 *     lazily, so a burst of setpenattr collapses into one set_pen before the paint.
 *
 * Two callback returns are load-bearing and propagate exactly (see AxlConsoleOps):
 * scrollrect (a decline lets libvterm decompose into moverect + erase) and
 * settermprop (its return gates whether libvterm latches the property, notably
 * alt-screen). The rest return 1 unconditionally — their returns are decoration
 * inside libvterm.
 */

#include <axl/axl-vterm.h>

#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <vterm.h>

/* Coalescing buffer for one output_text run. A run never spans rows, so this
 * bounds at one line's UTF-8; when a line would overflow it we flush and continue
 * seamlessly (the next glyph lands where the run left the cursor, so no set_cursor
 * is emitted). */
#define AXL_VTERM_RUN_MAX 1024

/* Worst-case UTF-8 for one cell: a base plus up to five combining marks
 * (VTERM_MAX_CHARS_PER_CELL == 6), each up to 4 bytes. */
#define AXL_VTERM_GLYPH_MAX (VTERM_MAX_CHARS_PER_CELL * 4)

struct AxlVterm {
    VTerm               *vt;
    VTermState          *state;
    const AxlConsoleOps *ops;
    void                *user;
    int                  rows;   /* current row count, for lineinfo realloc on resize */

    /* Glyph coalescing. */
    char     run[AXL_VTERM_RUN_MAX];
    size_t   run_len;
    VTermPos run_end;        /* where the next glyph must land to extend the run */
    bool     run_active;
    VTermPos cursor;         /* the consumer's logical cursor (post-run / post-move) */

    /* Pen accumulation. */
    AxlConsolePen pen;
    bool          pen_dirty;
};

/* Vendored libvterm's own width authority (INTERNAL visibility; resolves at
 * static-link time within the image). axl_vterm_char_width wraps it so producer
 * and consumer share ONE table — never a second wcwidth here. */
int vterm_unicode_width(uint32_t codepoint);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void *
vterm_alloc(size_t size, void *allocdata)
{
    (void)allocdata;
    /* libvterm requires zeroed memory from its allocator. */
    return axl_calloc(1, size);
}

static void
vterm_free_cb(void *ptr, void *allocdata)
{
    (void)allocdata;
    axl_free(ptr);
}

/* Non-const because vterm_new_with_allocator() takes a non-const pointer (it only
 * reads the two function pointers; it never writes through it). */
static VTermAllocatorFunctions vterm_allocator = {
    .malloc = vterm_alloc,
    .free   = vterm_free_cb,
};

static size_t
encode_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static AxlConsoleRect
rect_of(VTermRect r)
{
    AxlConsoleRect out = {
        .start_row = r.start_row,
        .end_row   = r.end_row,
        .start_col = r.start_col,
        .end_col   = r.end_col,
    };
    return out;
}

static AxlConsoleColor
color_of(const VTermColor *c)
{
    AxlConsoleColor out = {0};
    /* The default bits are orthogonal to the RGB/indexed type bit, so test them
     * FIRST. */
    if (VTERM_COLOR_IS_DEFAULT_FG(c) || VTERM_COLOR_IS_DEFAULT_BG(c)) {
        out.kind = AXL_CONSOLE_COLOR_DEFAULT;
    } else if (VTERM_COLOR_IS_INDEXED(c)) {
        out.kind = AXL_CONSOLE_COLOR_INDEXED;
        out.idx  = c->indexed.idx;
    } else {
        out.kind = AXL_CONSOLE_COLOR_RGB;
        out.r    = c->rgb.red;
        out.g    = c->rgb.green;
        out.b    = c->rgb.blue;
    }
    return out;
}

/* Emit the accumulated pen if it changed since the last set_pen. Called before any
 * op that paints, so a burst of setpenattr collapses into one set_pen. */
static void
emit_pen(AxlVterm *v)
{
    if (v->pen_dirty) {
        if (v->ops->set_pen) {
            v->ops->set_pen(v->user, &v->pen);
        }
        v->pen_dirty = false;
    }
}

/* Flush the buffered run. output_text advanced the terminal cursor to run_end, so
 * the logical cursor follows even when the run is empty or output_text is unbound. */
static void
flush_run(AxlVterm *v)
{
    if (!v->run_active) {
        return;
    }
    if (v->run_len > 0 && v->ops->output_text) {
        v->ops->output_text(v->user, v->run, v->run_len);
    }
    v->cursor     = v->run_end;
    v->run_active = false;
    v->run_len    = 0;
}

/* Begin a run at pos: reposition the consumer's cursor if needed, latch the pen. */
static void
begin_run(AxlVterm *v, VTermPos pos)
{
    if (pos.row != v->cursor.row || pos.col != v->cursor.col) {
        if (v->ops->set_cursor) {
            v->ops->set_cursor(v->user, pos.row, pos.col);
        }
        v->cursor = pos;
    }
    emit_pen(v);
    v->run_active = true;
    v->run_len    = 0;
    v->run_end    = pos;
}

// ---------------------------------------------------------------------------
// VTermStateCallbacks adapters
// ---------------------------------------------------------------------------

static int
adapter_putglyph(VTermGlyphInfo *info, VTermPos pos, void *u)
{
    AxlVterm *v = u;

    char   glyph[AXL_VTERM_GLYPH_MAX];
    size_t g = 0;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && info->chars[i]; i++) {
        g += encode_utf8(info->chars[i], glyph + g);
    }

    bool extend = v->run_active &&
                  pos.row == v->run_end.row && pos.col == v->run_end.col;
    if (!extend) {
        flush_run(v);
        begin_run(v, pos);
    }
    if (v->run_len + g > AXL_VTERM_RUN_MAX) {
        flush_run(v);
        begin_run(v, pos);
    }

    axl_memcpy(v->run + v->run_len, glyph, g);
    v->run_len += g;
    v->run_end.col = pos.col + info->width;
    return 1;
}

static int
adapter_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *u)
{
    AxlVterm *v = u;
    (void)oldpos;
    (void)visible;

    flush_run(v);
    if (pos.row != v->cursor.row || pos.col != v->cursor.col) {
        if (v->ops->set_cursor) {
            v->ops->set_cursor(v->user, pos.row, pos.col);
        }
        v->cursor = pos;
    }
    return 1;
}

static int
adapter_scrollrect(VTermRect rect, int downward, int rightward, void *u)
{
    AxlVterm *v = u;
    flush_run(v);
    emit_pen(v);
    /* Propagate the consumer's answer verbatim: a 0 lets libvterm decompose the
     * scroll into moverect + erase via vterm_scroll_rect(). Unbound == decline. */
    if (v->ops->scrollrect) {
        return v->ops->scrollrect(v->user, rect_of(rect), downward, rightward);
    }
    return 0;
}

static int
adapter_moverect(VTermRect dest, VTermRect src, void *u)
{
    AxlVterm *v = u;
    /* Reached only from vterm_scroll_rect()'s decomposition, which already ran
     * after adapter_scrollrect flushed; flush again defensively. Argument order is
     * (dest, src) — matching vterm.c's call site, not vterm.h's mis-named
     * prototype. */
    flush_run(v);
    if (v->ops->moverect) {
        v->ops->moverect(v->user, rect_of(dest), rect_of(src));
    }
    return 1;
}

static int
adapter_erase(VTermRect rect, int selective, void *u)
{
    AxlVterm *v = u;
    flush_run(v);
    emit_pen(v);   /* erase paints the current pen's background */
    if (v->ops->erase) {
        v->ops->erase(v->user, rect_of(rect), selective != 0);
    }
    return 1;
}

static int
adapter_initpen(void *u)
{
    AxlVterm *v = u;
    flush_run(v);
    axl_memset(&v->pen, 0, sizeof v->pen);   /* zeroed == default fg/bg, no attrs */
    v->pen_dirty = true;
    return 1;
}

static int
adapter_setpenattr(VTermAttr attr, VTermValue *val, void *u)
{
    AxlVterm *v = u;
    flush_run(v);            /* the pen changed: the old run keeps the old pen */
    switch (attr) {
    case VTERM_ATTR_BOLD:       v->pen.bold      = !!val->boolean; break;
    case VTERM_ATTR_ITALIC:     v->pen.italic    = !!val->boolean; break;
    case VTERM_ATTR_BLINK:      v->pen.blink     = !!val->boolean; break;
    case VTERM_ATTR_REVERSE:    v->pen.reverse   = !!val->boolean; break;
    case VTERM_ATTR_CONCEAL:    v->pen.conceal   = !!val->boolean; break;
    case VTERM_ATTR_STRIKE:     v->pen.strike    = !!val->boolean; break;
    case VTERM_ATTR_UNDERLINE:  v->pen.underline = (uint8_t)val->number; break;
    case VTERM_ATTR_FOREGROUND: v->pen.fg = color_of(&val->color); break;
    case VTERM_ATTR_BACKGROUND: v->pen.bg = color_of(&val->color); break;
    default: break;              /* FONT, SMALL, BASELINE: not carried */
    }
    v->pen_dirty = true;
    return 1;
}

/* Map a libvterm VTermProp onto AxlConsoleProp + AxlConsoleValue. Returns false for
 * a property the contract does not carry (the caller then accepts it, so libvterm
 * is never blocked on our account). */
static bool
map_prop(VTermProp prop, const VTermValue *val,
         AxlConsoleProp *out_prop, AxlConsoleValue *out_val)
{
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: *out_prop = AXL_CONSOLE_PROP_CURSOR_VISIBLE; break;
    case VTERM_PROP_CURSORBLINK:   *out_prop = AXL_CONSOLE_PROP_CURSOR_BLINK;   break;
    case VTERM_PROP_ALTSCREEN:     *out_prop = AXL_CONSOLE_PROP_ALT_SCREEN;     break;
    case VTERM_PROP_TITLE:         *out_prop = AXL_CONSOLE_PROP_TITLE;          break;
    case VTERM_PROP_ICONNAME:      *out_prop = AXL_CONSOLE_PROP_ICON_NAME;      break;
    case VTERM_PROP_REVERSE:       *out_prop = AXL_CONSOLE_PROP_REVERSE;        break;
    case VTERM_PROP_CURSORSHAPE:   *out_prop = AXL_CONSOLE_PROP_CURSOR_SHAPE;   break;
    case VTERM_PROP_MOUSE:         *out_prop = AXL_CONSOLE_PROP_MOUSE;          break;
    case VTERM_PROP_FOCUSREPORT:   *out_prop = AXL_CONSOLE_PROP_FOCUS_REPORT;   break;
    default: return false;
    }

    axl_memset(out_val, 0, sizeof *out_val);
    switch (vterm_get_prop_type(prop)) {
    case VTERM_VALUETYPE_BOOL:
        out_val->kind      = AXL_CONSOLE_VALUE_BOOL;
        out_val->u.boolean = val->boolean != 0;
        break;
    case VTERM_VALUETYPE_INT:
        out_val->kind     = AXL_CONSOLE_VALUE_NUMBER;
        out_val->u.number = val->number;
        break;
    case VTERM_VALUETYPE_STRING:
        out_val->kind = AXL_CONSOLE_VALUE_STRING;
        out_val->u.string.str     = val->string.str;
        out_val->u.string.len     = val->string.len;
        out_val->u.string.initial = val->string.initial;
        out_val->u.string.final   = val->string.final;
        break;
    default:
        return false;
    }
    return true;
}

static int
adapter_settermprop(VTermProp prop, VTermValue *val, void *u)
{
    AxlVterm *v = u;
    AxlConsoleProp  aprop;
    AxlConsoleValue aval;

    if (!map_prop(prop, val, &aprop, &aval)) {
        return 1;   /* a prop we don't carry: accept, so libvterm proceeds */
    }
    flush_run(v);
    /* Propagate the consumer's answer verbatim: libvterm only latches the property
     * if the callback said it was happy (especially for alt-screen). */
    if (v->ops->set_term_prop) {
        return v->ops->set_term_prop(v->user, aprop, &aval);
    }
    return 1;
}

static int
adapter_bell(void *u)
{
    AxlVterm *v = u;
    flush_run(v);
    if (v->ops->bell) {
        v->ops->bell(v->user);
    }
    return 1;
}

/* CSI 3J (erase-scrollback). Layer 2's only scrollback callback: libvterm keeps
 * no history itself, so this just tells the consumer to drop its ring. Returning
 * non-zero marks the command handled (state.c:1130); returning 0 when unbound
 * lets libvterm fall through harmlessly. */
static int
adapter_sb_clear(void *u)
{
    AxlVterm *v = u;
    flush_run(v);
    if (v->ops->clear_scrollback) {
        v->ops->clear_scrollback(v->user);
        return 1;
    }
    return 0;
}

/* Reallocate one per-row lineinfo buffer to new_rows, preserving min(old,new)
 * rows and zeroing the rest. A bound resize callback owns this: libvterm's own
 * unbound-callback fallback is dead code (it compares rows against the value it
 * just assigned), and Layer 3's screen.c reallocates here exactly this way. */
static void
resize_lineinfo(AxlVterm *v, VTermStateFields *fields, int idx, int new_rows)
{
    VTermLineInfo *old = fields->lineinfos[idx];
    if (!old) {
        return;   /* the altscreen buffer may be absent */
    }

    VTermLineInfo *fresh = axl_calloc((size_t)new_rows, sizeof *fresh);
    if (!fresh) {
        return;   /* best effort under OOM: keep the old buffer */
    }
    int keep = v->rows < new_rows ? v->rows : new_rows;
    for (int row = 0; row < keep; row++) {
        fresh[row] = old[row];
    }

    axl_free(old);
    fields->lineinfos[idx] = fresh;
}

static int
adapter_resize(int rows, int cols, VTermStateFields *fields, void *u)
{
    AxlVterm *v = u;
    (void)cols;
    flush_run(v);
    /* We bound resize, so libvterm hands us total control of fields->lineinfos and
     * copies our pointers straight back. v->rows is the OLD count (state->rows was
     * already advanced before this dispatch). */
    if (rows != v->rows) {
        resize_lineinfo(v, fields, 0, rows);
        resize_lineinfo(v, fields, 1, rows);
    }
    v->rows = rows;
    return 1;
}

static const VTermStateCallbacks adapter_cbs = {
    .putglyph    = adapter_putglyph,
    .movecursor  = adapter_movecursor,
    .scrollrect  = adapter_scrollrect,
    .moverect    = adapter_moverect,
    .erase       = adapter_erase,
    .initpen     = adapter_initpen,
    .setpenattr  = adapter_setpenattr,
    .settermprop = adapter_settermprop,
    .bell        = adapter_bell,
    .resize      = adapter_resize,
    .sb_clear    = adapter_sb_clear,
    /* setlineinfo, premove: deliberately NULL. */
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlVterm *
axl_vterm_new(int32_t rows, int32_t cols,
              const AxlConsoleOps *ops, void *user)
{
    if (!ops || rows <= 0 || cols <= 0) {
        return NULL;
    }

    AxlVterm *v = axl_new(AxlVterm);   /* zeroed: run inactive, pen default+clean */
    if (!v) {
        return NULL;
    }
    v->ops  = ops;
    v->user = user;
    v->rows = rows;

    v->vt = vterm_new_with_allocator(rows, cols, &vterm_allocator, NULL);
    if (!v->vt) {
        axl_free(v);
        return NULL;
    }
    vterm_set_utf8(v->vt, 1);
    v->state = vterm_obtain_state(v->vt);

    /* Reset BEFORE binding callbacks so the reset's own mode init emits no ops; the
     * consumer's grid starts fresh (cursor 0,0, default pen), matching our state. */
    vterm_state_reset(v->state, 1);
    vterm_state_set_callbacks(v->state, &adapter_cbs, v);

    /* Binding callbacks fires initpen (state.c:2185), which marked the pen dirty.
     * The consumer's grid already starts at the default pen (see axl_vterm_new's
     * docs), so swallow that one emission: the first set_pen then fires only on a
     * real SGR, never a redundant default snapshot before the first run. */
    v->pen_dirty = false;

    /* The one op emitted at construction: report our cell-boundary rule. */
    if (ops->set_cell_rule) {
        ops->set_cell_rule(user, AXL_CONSOLE_CELLS_WIDTH_RESOLVED);
    }

    return v;
}

void
axl_vterm_free(AxlVterm *v)
{
    if (!v) {
        return;
    }
    if (v->vt) {
        vterm_free(v->vt);
    }
    axl_free(v);
}

void
axl_vterm_feed(AxlVterm *v, const char *bytes, size_t len)
{
    if (!v || !bytes) {
        return;
    }
    vterm_input_write(v->vt, bytes, len);
}

void
axl_vterm_flush(AxlVterm *v)
{
    if (!v) {
        return;
    }
    flush_run(v);
}

int
axl_vterm_set_size(AxlVterm *v, int32_t rows, int32_t cols)
{
    if (!v || rows <= 0 || cols <= 0) {
        return AXL_ERR;
    }
    flush_run(v);
    vterm_set_size(v->vt, rows, cols);
    return AXL_OK;
}

int
axl_vterm_char_width(uint32_t codepoint)
{
    int w = vterm_unicode_width(codepoint);
    /* vterm_unicode_width returns -1 for C0/C1 controls; the contract is total and
     * treats them as zero-width. Combining marks and ZWSP already return 0. */
    return w < 0 ? 0 : w;
}
