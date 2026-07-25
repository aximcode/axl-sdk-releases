/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-emit.c
    Shared SIMPLE_TEXT_OUTPUT -> AxlConsoleOps translation engine. See
    axl-console-emit.h. The logic here was factored out of axl-console-tap.c
    verbatim so the tap and the take-over device emit identical ops.
**/

#include "axl-console-emit.h"

// ---------------------------------------------------------------------------
// Structured-op emit helpers. Only the ops a SIMPLE_TEXT_OUTPUT source can
// observe are produced: set_cell_rule, clear_screen, set_cursor, output_text,
// set_pen, set_mode, set_term_prop. The rect/scroll/bell ops are for axl-vterm.
// ---------------------------------------------------------------------------

/* SetAttribute(attr) is already a whole-pen snapshot, so build and emit a pen.
   The UEFI attribute splits into an indexed foreground (0..15) and background
   (0..7); a SIMPLE_TEXT_OUTPUT source never produces default/truecolor or style
   bits. */
static void
emit_pen(AxlConsoleEmit *e, uint32_t fg, uint32_t bg)
{
    if (e->ops == NULL || e->ops->set_pen == NULL) {
        return;
    }
    AxlConsolePen pen = {
        .fg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = (uint8_t)fg },
        .bg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = (uint8_t)bg },
    };
    e->ops->set_pen(e->user, &pen);
}

/* Emit a boolean terminal property. Returns the consumer's accept/reject (1 when
   set_term_prop is unbound). Callers that assert their own state ignore it. */
static int
emit_prop_bool(AxlConsoleEmit *e, AxlConsoleProp prop, bool v)
{
    if (e->ops == NULL || e->ops->set_term_prop == NULL) {
        return 1;
    }
    AxlConsoleValue val = { .kind = AXL_CONSOLE_VALUE_BOOL, .u.boolean = v };
    return e->ops->set_term_prop(e->user, prop, &val);
}

// ---------------------------------------------------------------------------
// Text dispatch
// ---------------------------------------------------------------------------

/* Substitute the characters the firmware itself refuses to pass through.
   EDK2's TerminalConOut.c (MdeModulePkg/Universal/Console/TerminalDxe) accepts only
   printable 0x20..0x7F plus the EFI control chars {NUL, BS, TAB, LF, CR}, and
   replaces everything else -- ESC included -- with '?'. A literal ESC is emitted
   ONLY when that driver is writing its OWN control string
   (TerminalDevice->OutputEscChar), never on behalf of an application. So a UEFI app
   cannot push raw VT through OutputString, and neither may we: forwarding ESC would
   diverge from firmware semantics AND hand any app that prints a user-controlled
   string an escape-injection vector into the consumer's terminal (a stray
   "\x1b[2J" would clear a remote xterm, or drive our own grid).

   We keep >= 0x20 rather than <= 0x7F: the console is UCS-2 and we speak UTF-8, so
   non-ASCII BMP text (box drawing, CJK) is legitimate here where TerminalDxe's
   ASCII-only wire could not carry it. Only the C0 control range is filtered. */
static CHAR16
sanitize_char(CHAR16 c)
{
    if (c >= 0x20) {
        return c;                       /* printable ASCII + all non-ASCII BMP */
    }
    if (c == 0x08 || c == 0x09 || c == 0x0A || c == 0x0D) {
        return c;                       /* BS, TAB, LF, CR */
    }
    return (CHAR16)'?';
}

/* Emit a UCS-2 string as UTF-8, chunked, no truncation. BMP only -- the UEFI
   console is UCS-2 (no surrogate pairs); a lone surrogate code unit would be
   emitted as its 3-byte form, which the console never produces in practice. */
static void
emit_output_text(AxlConsoleEmit *e, const CHAR16 *s)
{
    if (e->ops == NULL || e->ops->output_text == NULL || s == NULL) {
        return;
    }
    char   buf[256];
    size_t n = 0;
    for (; *s != 0; s++) {
        unsigned c = (unsigned)sanitize_char(*s);
        char     tmp[3];
        size_t   tn;
        if (c < 0x80) {
            tmp[0] = (char)c;
            tn = 1;
        } else if (c < 0x800) {
            tmp[0] = (char)(0xC0 | (c >> 6));
            tmp[1] = (char)(0x80 | (c & 0x3F));
            tn = 2;
        } else {
            tmp[0] = (char)(0xE0 | (c >> 12));
            tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (c & 0x3F));
            tn = 3;
        }
        if (n + tn > sizeof(buf)) {
            e->ops->output_text(e->user, buf, n);
            n = 0;
        }
        for (size_t i = 0; i < tn; i++) {
            buf[n++] = tmp[i];
        }
    }
    if (n > 0) {
        e->ops->output_text(e->user, buf, n);
    }
}

/* Advance the owned Mode's cursor across emitted text, mirroring the reference
   driver: BS steps left, CR homes the column, LF advances the row, any other
   printable advances the column with autowrap. The row clamps at the last line
   (the console scrolls rather than growing). Control chars don't move. */
static void
track_cursor(AxlConsoleEmit *e, const CHAR16 *s, uint32_t cols, uint32_t rows)
{
    if (s == NULL) {
        return;
    }
    int32_t col = (int32_t)e->mode->CursorColumn;
    int32_t row = (int32_t)e->mode->CursorRow;
    for (; *s != 0; s++) {
        /* Track what the console actually shows: a filtered control char became a
           printable '?', so it advances the column like one. */
        CHAR16 c = sanitize_char(*s);
        if (c == L'\b') {
            if (col > 0) { col--; }
        } else if (c == L'\r') {
            col = 0;
        } else if (c == L'\n') {
            row++;
        } else if (c >= L' ') {
            col++;
            if (cols > 0 && (uint32_t)col >= cols) {   /* autowrap */
                col = 0;
                row++;
            }
        }
        if (rows > 0 && (uint32_t)row >= rows) {       /* scrolled: pin to last line */
            row = (int32_t)rows - 1;
        }
    }
    e->mode->CursorColumn = col;
    e->mode->CursorRow    = row;
}

/* Auto-leave: a newline in the output is linear shell flow. A full-screen app
   addresses the screen with set_cursor and never emits '\n' (verified against
   `edit`), so a newline both leaves the alt-screen and closes the post-clear window
   in which a backward cursor jump would be read as a full-screen repaint (see
   track_cursor's enter). No-op unless auto_alt_screen is on. */
static void
track_flow(AxlConsoleEmit *e, const CHAR16 *s)
{
    if (!e->auto_alt_screen || s == NULL) {
        return;
    }
    bool newline = false;
    for (; *s != 0; s++) {
        if (*s == L'\n') {
            newline = true;
            break;
        }
    }
    if (newline) {
        if (e->alt_active) {
            axl_console_emit_leave_alt_screen(e);
        }
        e->saw_clear = false;   /* linear flow closes the post-clear detect window */
    }
}

// ---------------------------------------------------------------------------
// Alt-screen (DECSET/DECRST 1049). Asserted BY the producer -- SIMPLE_TEXT_OUTPUT
// has no way to express it, so it's inferred (explicit API, or the
// auto_alt_screen heuristic) rather than received from the protocol.
// ---------------------------------------------------------------------------

/* The producer asserts its OWN alt-screen state (alt_active), so it IGNORES
   set_term_prop's return -- unlike libvterm, which stores the property only if
   the consumer accepts it. A SIMPLE_TEXT_OUTPUT producer has no grid to keep in
   sync with a consumer. */
void
axl_console_emit_enter_alt_screen(AxlConsoleEmit *e)
{
    if (e != NULL && !e->alt_active) {
        e->alt_active = true;
        (void)emit_prop_bool(e, AXL_CONSOLE_PROP_ALT_SCREEN, true);
    }
}

void
axl_console_emit_leave_alt_screen(AxlConsoleEmit *e)
{
    if (e != NULL && e->alt_active) {
        e->alt_active = false;
        (void)emit_prop_bool(e, AXL_CONSOLE_PROP_ALT_SCREEN, false);
    }
}

bool
axl_console_emit_in_alt_screen(const AxlConsoleEmit *e)
{
    return e != NULL && e->alt_active;
}

// ---------------------------------------------------------------------------
// Public engine surface
// ---------------------------------------------------------------------------

void
axl_console_emit_init(AxlConsoleEmit *e, const AxlConsoleOps *ops, void *user,
                      SIMPLE_TEXT_OUTPUT_MODE *mode, bool auto_alt_screen)
{
    e->ops             = ops;
    e->user            = user;
    e->mode            = mode;
    e->auto_alt_screen = auto_alt_screen;
    e->alt_active      = false;
    e->saw_clear       = false;
}

void
axl_console_emit_report_cell_rule(AxlConsoleEmit *e)
{
    /* SIMPLE_TEXT_OUTPUT is one-cell-per-codepoint by construction. */
    if (e->ops != NULL && e->ops->set_cell_rule != NULL) {
        e->ops->set_cell_rule(e->user, AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT);
    }
}

void
axl_console_emit_text(AxlConsoleEmit *e, const CHAR16 *s,
                      uint32_t res_cols, uint32_t res_rows)
{
    emit_output_text(e, s);
    track_cursor(e, s, res_cols, res_rows);
    track_flow(e, s);
}

void
axl_console_emit_set_attribute(AxlConsoleEmit *e, uint32_t attribute)
{
    e->mode->Attribute = (INT32)attribute;
    emit_pen(e, (uint32_t)(attribute & 0x0F), (uint32_t)((attribute >> 4) & 0x07));
}

void
axl_console_emit_set_mode(AxlConsoleEmit *e, uint32_t mode_number)
{
    e->mode->Mode         = (INT32)mode_number;
    e->mode->CursorColumn = 0;   /* SetMode clears the screen and homes */
    e->mode->CursorRow    = 0;
    if (e->auto_alt_screen) {
        e->saw_clear = true;     /* a mode change resets the screen, like a clear */
    }
    if (e->ops != NULL && e->ops->set_mode != NULL) {
        e->ops->set_mode(e->user, mode_number);
    }
}

void
axl_console_emit_resize(AxlConsoleEmit *e, uint32_t cols, uint32_t rows)
{
    if (e->ops != NULL && e->ops->resize != NULL) {
        e->ops->resize(e->user, cols, rows);
    }
}

void
axl_console_emit_clear_screen(AxlConsoleEmit *e)
{
    /* A clear alone is NOT a full-screen app -- a shell `cls` clears then prints a
       prompt. Just arm saw_clear; a subsequent BACKWARD cursor jump (a TUI
       repainting) is what enters the alt-screen (see set_cursor). Entering on the
       clear itself latched the alt-screen at the boot ClearScreen and never left. */
    if (e->auto_alt_screen) {
        e->saw_clear = true;
    }
    if (e->ops != NULL && e->ops->clear_screen != NULL) {
        e->ops->clear_screen(e->user);
    }
    e->mode->CursorColumn = 0;
    e->mode->CursorRow    = 0;
}

void
axl_console_emit_set_cursor(AxlConsoleEmit *e, uint32_t column, uint32_t row)
{
    /* A backward row jump after a clear is a full-screen app repainting (a shell
       prompt only moves forward/down); enter the alt-screen so its redraws don't
       dump into the terminal's scrollback. Compare BEFORE updating the tracked row.
       Inferring the alt-screen from SIMPLE_TEXT_OUTPUT is best-effort: `saw_clear`
       stays armed from a clear until the next newline, so a shell `cls` (which shows
       a prompt with no trailing newline) leaves a window in which backward-editing a
       wrapped command line would spuriously enter -- it self-corrects on the next
       newline (leave). It cannot be closed by disarming on the prompt text, because a
       full-screen app's post-clear title text is indistinguishable from a prompt
       until what follows (a backward jump vs. a newline) arrives. */
    if (e->auto_alt_screen && e->saw_clear && !e->alt_active
        && row < (uint32_t)e->mode->CursorRow) {
        axl_console_emit_enter_alt_screen(e);
    }
    /* The cursor has moved even when a caller suppresses a redundant escape, so
       publish it unconditionally. */
    e->mode->CursorColumn = (INT32)column;
    e->mode->CursorRow    = (INT32)row;
    if (e->ops != NULL && e->ops->set_cursor != NULL) {
        e->ops->set_cursor(e->user, (int32_t)row, (int32_t)column);
    }
}

void
axl_console_emit_enable_cursor(AxlConsoleEmit *e, bool visible)
{
    e->mode->CursorVisible = visible ? TRUE : FALSE;
    (void)emit_prop_bool(e, AXL_CONSOLE_PROP_CURSOR_VISIBLE, visible);
}

void
axl_console_emit_home_cursor(AxlConsoleEmit *e)
{
    e->mode->CursorColumn = 0;
    e->mode->CursorRow    = 0;
}

void
axl_console_emit_reset(AxlConsoleEmit *e)
{
    if (e == NULL) {
        return;
    }
    /* Leave the alt-screen cleanly (emits the false prop if we were in it) so the
       next session starts on the normal screen; clear the detect state. */
    axl_console_emit_leave_alt_screen(e);
    e->saw_clear = false;
}
