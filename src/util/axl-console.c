/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console.c
    Interactive console input wrapper around the backend's ConIn
    primitives. See @ref axl-console.h for the public contract.
**/

#include <axl/axl-console.h>
#include <axl/axl-mem.h>   /* axl_malloc / axl_realloc / axl_free */
#include <axl/axl-str.h>   /* axl_ucs2_to_utf8 */
#include "../backend/axl-backend.h"

/* 100ns ticks per millisecond — matches EFI's TIMER_PERIODIC unit. */
#define HUNDRED_NS_PER_MS  10000ULL

/* Echo strings written to ConOut during interactive line editing. Explicit
   UCS-2 arrays (not L"..."): the console-write backend takes unsigned short *,
   and this sidesteps any wchar_t width assumption. */
static const unsigned short kReadlineEraseCell[] = { 0x08, 0x20, 0x08, 0 }; /* \b \b */
static const unsigned short kReadlineNewline[]   = { 0x0D, 0x0A, 0 };       /* CRLF   */

/* Create and arm a one-shot relative timer of @p timeout_ms milliseconds.
   Returns the event (close with axl_backend_event_close), or NULL on failure.
   Only for a finite, non-zero timeout — callers handle the 0 (non-blocking)
   and UINT64_MAX (block-forever) cases without a timer. Shared by
   axl_console_read_key and axl_console_readline_ex. */
static AxlEventHandle
make_relative_timer(
    uint64_t  timeout_ms
    )
{
    AxlEventHandle timer = NULL;
    if (axl_backend_event_create_timer(&timer) != AXL_OK) {
        return NULL;
    }
    /* Cap the multiplication to avoid overflow when the caller passes a
       near-UINT64_MAX timeout; UEFI's interval field is uint64, so anything
       that overflows the * 10000 multiply is well past any reasonable wait. */
    uint64_t interval_100ns = (timeout_ms > UINT64_MAX / HUNDRED_NS_PER_MS)
                                  ? UINT64_MAX
                                  : timeout_ms * HUNDRED_NS_PER_MS;
    if (axl_backend_event_set_timer(timer, AXL_TIMER_RELATIVE,
                                    interval_100ns) != AXL_OK) {
        axl_backend_event_close(timer);
        return NULL;
    }
    return timer;
}

int
axl_console_read_key(
    uint64_t   timeout_ms,
    AxlKey    *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }

    AxlEventHandle key_evt = axl_backend_console_wait_for_key();
    if (key_evt == NULL) {
        /* No console (driver app, raw firmware ctx, BDS hand-off
           that hasn't published ConIn yet). Caller can fall back
           to whatever degraded UI they want. */
        return AXL_ERR;
    }

    if (timeout_ms == 0) {
        /* Non-blocking: poll the event without waiting. */
        if (axl_backend_event_check(key_evt) != 0) {
            return AXL_ERR;
        }
    } else if (timeout_ms == UINT64_MAX) {
        /* Block forever on the key event alone. */
        size_t fired = 0;
        if (axl_backend_event_wait(1, &key_evt, &fired) != AXL_OK) {
            return AXL_ERR;
        }
    } else {
        /* Bounded wait: union of {key event, timer}. The timer event
           is closed unconditionally on return so a slow key path
           doesn't leak it. */
        AxlEventHandle timer = make_relative_timer(timeout_ms);
        if (timer == NULL) {
            return AXL_ERR;
        }
        AxlEventHandle events[2];
        events[0] = key_evt;
        events[1] = timer;
        size_t fired = 0;
        int rc = axl_backend_event_wait(2, events, &fired);
        axl_backend_event_close(timer);
        if (rc != AXL_OK) {
            return AXL_ERR;
        }
        if (fired == 1) {
            /* Timer beat the key — timeout. */
            return AXL_ERR;
        }
    }

    /* Key is available. Read it via the backend (non-blocking;
       returns -1 only if the firmware lost it between event-fire
       and read, which shouldn't happen but mirrors the backend
       contract). */
    out->scan_code    = 0;
    out->unicode_char = 0;
    return axl_backend_console_read_key(&out->scan_code,
                                        &out->unicode_char);
}

void
axl_console_flush_input(
    void
    )
{
    /* Drain ConIn until ReadKeyStroke reports nothing left. The
       backend returns -1 when the queue is empty, so this loop
       terminates after the firmware has handed back every buffered
       keystroke. NULL-safe: backend's ConIn check inside
       read_key short-circuits when the protocol isn't published. */
    uint16_t scan = 0;
    uint16_t uni  = 0;
    while (axl_backend_console_read_key(&scan, &uni) == AXL_OK) {
        /* discard */
    }
}

// ===================================================================
// Interactive line input (axl_console_readline / _ex)
// ===================================================================

/* Feed one keystroke into the growing UCS-2 line buffer.
   @return 1 when Enter ends the line, 0 to keep reading, -1 on OOM. */
static int
readline_feed(
    unsigned short **buf,      ///< [in,out] growing UCS-2 buffer (NULL until first char)
    size_t          *len,      ///< [in,out] characters accumulated
    size_t          *cap,      ///< [in,out] buffer capacity in characters
    size_t           max_len,  ///< character cap (0 = unbounded)
    bool             echo,     ///< echo printable chars + Backspace erase
    unsigned short   uc        ///< the keystroke's UCS-2 char (0 for special keys)
    )
{
    if (uc == 0x0D || uc == 0x0A) {          /* Enter (CR or LF) ends the line. */
        axl_backend_console_write(kReadlineNewline);   /* echoed even when hidden */
        return 1;
    }
    if (uc == 0x08) {                        /* Backspace erases the last char. */
        if (*len > 0) {
            (*len)--;
            if (echo) {
                axl_backend_console_write(kReadlineEraseCell);
            }
        }
        return 0;
    }
    if (uc < 0x20 || uc == 0x7F) {
        /* Control code, DEL, or a special key (uc == 0, scan code set):
           ignored — there is no mid-line cursor editing. */
        return 0;
    }
    if (max_len != 0 && *len >= max_len) {
        return 0;                            /* past the cap: silently dropped */
    }
    if (*len + 2 > *cap) {                    /* room for the char + a NUL */
        size_t new_cap = (*cap == 0) ? 32 : *cap * 2;
        unsigned short *nb = axl_realloc(*buf, new_cap * sizeof(unsigned short));
        if (nb == NULL) {
            return -1;
        }
        *buf = nb;
        *cap = new_cap;
    }
    (*buf)[(*len)++] = uc;
    if (echo) {
        unsigned short one[2] = { uc, 0 };
        axl_backend_console_write(one);
    }
    return 0;
}

int
axl_console_readline(
    uint64_t   timeout_ms,
    char     **out_line
    )
{
    return axl_console_readline_ex(timeout_ms, 0, true, out_line);
}

int
axl_console_readline_ex(
    uint64_t   timeout_ms,
    size_t     max_len,
    bool       echo,
    char     **out_line
    )
{
    if (out_line == NULL) {
        return AXL_ERR;
    }
    *out_line = NULL;

    AxlEventHandle key_evt = axl_backend_console_wait_for_key();
    if (key_evt == NULL) {
        return AXL_ERR;   /* no console (ConIn unavailable) */
    }
    /* Wake on Ctrl-C too, when the shell publishes its break event. */
    AxlEventHandle break_evt = axl_backend_shell_break_event();

    /* One whole-line deadline timer (finite timeout only): set once and
       shared across every keystroke wait, so the budget spans the line. */
    AxlEventHandle timer      = NULL;
    bool           have_timer = (timeout_ms != 0 && timeout_ms != UINT64_MAX);
    if (have_timer) {
        timer = make_relative_timer(timeout_ms);
        if (timer == NULL) {
            return AXL_ERR;
        }
    }

    unsigned short *buf = NULL;
    size_t          len = 0;
    size_t          cap = 0;
    int             rc  = AXL_ERR;   /* default: aborted (timeout/break/OOM/EOF) */

    for (;;) {
        uint16_t sc = 0;
        uint16_t uc = 0;

        if (timeout_ms == 0) {
            /* Non-blocking: consume only already-buffered keys. Break
               (Ctrl-C) checked first; a drained queue with no Enter ends
               the loop with rc == AXL_ERR (partial input discarded). */
            if (break_evt != NULL && axl_backend_event_check(break_evt) == 0) {
                break;
            }
            if (axl_backend_console_read_key(&sc, &uc) != AXL_OK) {
                break;
            }
        } else {
            /* Blocking: wait on the union of {key, [timer], [break]}. */
            AxlEventHandle evs[3];
            size_t         n         = 0;
            size_t         idx_timer = (size_t)-1;
            size_t         idx_break = (size_t)-1;
            evs[n++] = key_evt;
            if (have_timer) {
                idx_timer = n;
                evs[n++]  = timer;
            }
            if (break_evt != NULL) {
                idx_break = n;
                evs[n++]  = break_evt;
            }
            size_t fired = 0;
            if (axl_backend_event_wait(n, evs, &fired) != AXL_OK) {
                break;
            }
            if (fired == idx_timer || fired == idx_break) {
                break;   /* whole-line deadline hit, or Ctrl-C */
            }
            if (axl_backend_console_read_key(&sc, &uc) != AXL_OK) {
                continue;   /* woke but the key was lost; keep waiting */
            }
        }

        int fed = readline_feed(&buf, &len, &cap, max_len, echo, uc);
        if (fed < 0) {
            break;              /* OOM — rc stays AXL_ERR */
        }
        if (fed == 1) {
            rc = AXL_OK;        /* Enter — line complete */
            break;
        }
    }

    if (timer != NULL) {
        axl_backend_event_close(timer);
    }

    if (rc == AXL_OK) {
        if (buf == NULL) {
            /* Immediate Enter: hand back a heap "" (never NULL on AXL_OK). */
            char *empty = axl_malloc(1);
            if (empty == NULL) {
                rc = AXL_ERR;
            } else {
                empty[0] = '\0';
                *out_line = empty;
            }
        } else {
            buf[len] = 0;   /* room reserved by readline_feed (len + 2 <= cap) */
            char *utf8 = axl_ucs2_to_utf8((const unsigned short *)buf);
            if (utf8 == NULL) {
                rc = AXL_ERR;
            } else {
                *out_line = utf8;
            }
        }
    }

    axl_free(buf);
    return rc;
}

// ===================================================================
// Text-console modes (SimpleTextOutput QueryMode / SetMode) — the
// graphics-free peer of the AxlGfx display-mode API. Thin policy over the
// backend's raw protocol access: bounds + signed-field guards, and the
// inventory-walking find / max helpers that skip QueryMode failures.
// ===================================================================

uint32_t
axl_console_text_mode_count(
    void
    )
{
    return axl_backend_console_text_mode_count();
}

int
axl_console_text_query_mode(
    uint32_t             index,
    AxlConsoleTextMode  *out
    )
{
    if (out == NULL || index >= axl_console_text_mode_count()) {
        return AXL_ERR;
    }
    uint32_t cols = 0, rows = 0;
    if (axl_backend_console_text_query_mode(index, &cols, &rows) != AXL_OK) {
        return AXL_ERR;
    }
    out->index   = index;
    out->columns = cols;
    out->rows    = rows;
    return AXL_OK;
}

int
axl_console_text_current_mode(
    uint32_t  *out_index
    )
{
    if (out_index == NULL) {
        return AXL_ERR;
    }
    int cur = axl_backend_console_text_current_mode();
    /* -1 == no mode set; also reject a value the firmware reports outside
       the enumerable range (malformed). */
    if (cur < 0 || (uint32_t)cur >= axl_console_text_mode_count()) {
        return AXL_ERR;
    }
    *out_index = (uint32_t)cur;
    return AXL_OK;
}

int
axl_console_text_find_mode(
    uint32_t   columns,
    uint32_t   rows,
    uint32_t  *out_index
    )
{
    if (out_index == NULL) {
        return AXL_ERR;
    }
    uint32_t n = axl_console_text_mode_count();
    for (uint32_t i = 0; i < n; i++) {
        AxlConsoleTextMode m;
        /* Skip modes whose QueryMode fails (a legal optional-mode reject). */
        if (axl_console_text_query_mode(i, &m) == AXL_OK
            && m.columns == columns && m.rows == rows) {
            *out_index = i;   /* lowest-numbered match (ascending walk) */
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

int
axl_console_text_max_mode(
    AxlConsoleTextMode  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    uint32_t           n     = axl_console_text_mode_count();
    bool               found = false;
    AxlConsoleTextMode best  = { 0, 0, 0 };
    for (uint32_t i = 0; i < n; i++) {
        AxlConsoleTextMode m;
        if (axl_console_text_query_mode(i, &m) != AXL_OK
            || m.columns == 0 || m.rows == 0) {
            continue;   /* skip QueryMode failures and degenerate geometry */
        }
        uint64_t area      = (uint64_t)m.columns * m.rows;
        uint64_t best_area = (uint64_t)best.columns * best.rows;
        /* Greatest area; ties -> more columns; full ties -> lowest index
           (strict comparisons + ascending walk keep the first such mode). */
        if (!found || area > best_area
            || (area == best_area && m.columns > best.columns)) {
            best  = m;
            found = true;
        }
    }
    if (!found) {
        return AXL_ERR;
    }
    *out = best;
    return AXL_OK;
}

int
axl_console_text_set_mode(
    uint32_t  index
    )
{
    /* Range-check before the firmware call (count == 0 -> always rejects). */
    if (index >= axl_console_text_mode_count()) {
        return AXL_ERR;
    }
    return axl_backend_console_text_set_mode(index);
}
