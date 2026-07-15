/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-vt.c
    Internal: shared VT wire-format encoders. See axl-console-vt.h.
**/

#include "axl-console-vt.h"
#include <axl/axl-str.h>

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
