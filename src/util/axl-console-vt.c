/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-vt.c
    Internal: shared VT wire-format encoders. See axl-console-vt.h.
**/

#include "axl-console-vt.h"
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include "../backend/axl-backend.h"

/* Advance a build offset by a just-returned axl_snprintf count, clamped so a later
   buf+off / cap-off stays in bounds even if the vocabulary outgrew the buffer. */
static size_t
sgr_advance(size_t off, size_t cap, int n)
{
    if (n < 0) {
        return off;
    }
    off += (size_t)n;
    return (off > cap - 1) ? cap - 1 : off;
}

/* One SGR colour parameter: ;39/;49 default, ;38;5;N/;48;5;N indexed,
   ;38;2;R;G;B / ;48;2;R;G;B truecolour. */
static size_t
sgr_color(char *buf, size_t cap, size_t off, const AxlConsoleColor *c, bool fg)
{
    int n = 0;
    switch (c->kind) {
    case AXL_CONSOLE_COLOR_DEFAULT:
        n = axl_snprintf(buf + off, cap - off, ";%u", fg ? 39u : 49u);
        break;
    case AXL_CONSOLE_COLOR_INDEXED:
        n = axl_snprintf(buf + off, cap - off, ";%u;5;%u", fg ? 38u : 48u,
                         (unsigned)c->idx);
        break;
    case AXL_CONSOLE_COLOR_RGB:
        n = axl_snprintf(buf + off, cap - off, ";%u;2;%u;%u;%u", fg ? 38u : 48u,
                         (unsigned)c->r, (unsigned)c->g, (unsigned)c->b);
        break;
    default:
        break;
    }
    return sgr_advance(off, cap, n);
}

size_t
axl_console_pen_to_sgr(char *buf, size_t cap, const AxlConsolePen *pen)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    int    n   = axl_snprintf(buf, cap, "\x1b[0");   /* reset base */
    off = sgr_advance(off, cap, n);
    off = sgr_color(buf, cap, off, &pen->fg, /*fg=*/true);
    off = sgr_color(buf, cap, off, &pen->bg, /*fg=*/false);

    struct { bool on; const char *code; } styles[] = {
        { pen->bold,                                   ";1" },
        { pen->italic,                                 ";3" },
        { pen->underline != AXL_CONSOLE_UNDERLINE_OFF, ";4" },
        { pen->blink,                                  ";5" },
        { pen->reverse,                                ";7" },
        { pen->conceal,                                ";8" },
        { pen->strike,                                 ";9" },
    };
    for (size_t i = 0; i < sizeof(styles) / sizeof(styles[0]); i++) {
        if (styles[i].on) {
            n   = axl_snprintf(buf + off, cap - off, "%s", styles[i].code);
            off = sgr_advance(off, cap, n);
        }
    }
    n   = axl_snprintf(buf + off, cap - off, "m");
    off = sgr_advance(off, cap, n);
    return off;
}

// ---------------------------------------------------------------------------
// AxlConsoleVtBuf — buffering VT sink (see axl-console-vt.h).
//
// A double buffer keeps the critical section O(1): flush swaps the accumulating
// AxlString for the idle spare under the TPL guard (just two pointer moves), then
// delivers the swapped-out bytes to the sink OUTSIDE the guard — so the sink's own
// work (which for a WS consumer runs an async TLS write) never holds a raised TPL.
// ---------------------------------------------------------------------------

bool
axl_console_vt_buf_init(AxlConsoleVtBuf *b, AxlConsoleVtEmitFn sink, void *user,
                        size_t chunk, bool guard)
{
    b->sink  = sink;
    b->user  = user;
    b->chunk = chunk;
    b->guard = guard;
    b->buf   = axl_string_new(NULL);
    b->spare = axl_string_new(NULL);
    if (b->buf == NULL || b->spare == NULL) {
        /* Degrade to pass-through: emit calls the sink directly. Free whichever
           allocation succeeded so the half-init leaks nothing. */
        axl_string_free(b->buf);
        axl_string_free(b->spare);
        b->buf = b->spare = NULL;
        return false;
    }
    return true;
}

void
axl_console_vt_buf_flush(AxlConsoleVtBuf *b)
{
    if (b == NULL || b->buf == NULL || axl_string_len(b->buf) == 0) {
        return;
    }
    /* Swap accumulate<->spare under the guard: a foreground append cannot run
       between taking the bytes and clearing the buffer. Two pointer moves only. */
    uintptr_t token = b->guard ? axl_backend_enter_critical() : 0;
    AxlString *full = b->buf;
    b->buf   = b->spare;   /* fresh (empty) accumulate buffer */
    b->spare = full;       /* holds the bytes to deliver */
    if (b->guard) {
        axl_backend_leave_critical(token);
    }
    b->sink(axl_string_str(full), axl_string_len(full), b->user);
    axl_string_clear(full);   /* now the empty spare for next time */
}

void
axl_console_vt_buf_emit(AxlConsoleVtBuf *b, const char *bytes, size_t len)
{
    if (b == NULL || bytes == NULL || len == 0) {
        return;
    }
    if (b->buf == NULL) {
        b->sink(bytes, len, b->user);   /* pass-through (init failed) */
        return;
    }
    bool over;
    uintptr_t token = b->guard ? axl_backend_enter_critical() : 0;
    axl_string_append_len(b->buf, bytes, len);
    over = (b->chunk != 0 && axl_string_len(b->buf) >= b->chunk);
    if (b->guard) {
        axl_backend_leave_critical(token);
    }
    /* Flush OUTSIDE the guard (it re-enters for its own O(1) swap): the append
       stays a short critical section and the sink runs unguarded. */
    if (over) {
        axl_console_vt_buf_flush(b);
    }
}

void
axl_console_vt_buf_dispose(AxlConsoleVtBuf *b)
{
    if (b == NULL) {
        return;
    }
    axl_string_free(b->buf);
    axl_string_free(b->spare);
    b->buf = b->spare = NULL;
    b->sink = NULL;
    b->user = NULL;
}
