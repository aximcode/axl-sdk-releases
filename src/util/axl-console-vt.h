/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-vt.h
    Internal: shared VT wire-format encoders for the two console serializers.

    `axl-console-mirror` (structured ops -> VT, live) and `axl-console-screen`
    (grid snapshot -> VT, repaint) both turn an @ref AxlConsolePen into the same
    SGR escape. This is that one encoder, so the byte format cannot drift between
    them. Not a public header — internal to the console modules.
**/

#ifndef AXL_CONSOLE_VT_H
#define AXL_CONSOLE_VT_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-console-ops.h>

typedef struct AxlString AxlString;   /* fwd; <axl/axl-string.h> in the .c */

/**
 * @brief Encode a pen snapshot as a full SGR escape "ESC[0;<fg>;<bg>;<styles>m".
 *
 * Standard SGR vocabulary only: 39/49 default, 38;5;N/48;5;N indexed,
 * 38;2;R;G;B / 48;2;R;G;B truecolour, plus 1/3/4/5/7/8/9 for the style bits. The
 * leading 0 resets, so the escape is absolute — it fully replaces any prior pen.
 *
 * @param buf receives the escape (needs ~64 bytes for the widest pen).
 * @param cap size of @p buf.
 * @param pen the rendition to encode.
 * @return the number of bytes written (never more than @p cap - 1), or 0 if
 *     @p buf is NULL or @p cap is 0.
 */
size_t
axl_console_pen_to_sgr(char *buf, size_t cap, const AxlConsolePen *pen);

// ---------------------------------------------------------------------------
// AxlConsoleVtBuf — a buffering VT sink shared by the two serializers.
//
// The live encoder (structured ops -> VT) and the snapshot serializer (grid ->
// VT) both used to call their sink once per op / per cell — a keystroke echo is
// >= 3 ops and a cleared 80x25 repaint is ~2000 cells, so a redraw fanned out as
// hundreds of one-byte frames and could starve a slow WS consumer. This buffers
// appended bytes and delivers them to the sink in few large calls: the live path
// flushes on demand (consumer loop tick), the snapshot auto-flushes per chunk.
// ---------------------------------------------------------------------------

/** Sink for coalesced VT bytes. Binary-compatible with @c AxlConsoleVtSinkFn and
    @c AxlConsoleScreenSink — all three are @c void(const char *, size_t, void *). */
typedef void (*AxlConsoleVtEmitFn)(const char *bytes, size_t len, void *user);

/** A buffering VT sink. Fields are private; use the functions below. */
typedef struct {
    AxlConsoleVtEmitFn sink;    ///< downstream sink (borrowed)
    void              *user;    ///< sink context
    AxlString         *buf;     ///< accumulation (NULL => emit passes straight through)
    AxlString         *spare;   ///< idle double-buffer, swapped in under the guard
    size_t             chunk;   ///< auto-flush threshold in bytes; 0 = flush only on demand
    bool               guard;   ///< raise TPL around the buffer swap (persistent buffers)
} AxlConsoleVtBuf;

/**
 * @brief Initialize @p b to accumulate into @p sink.
 *
 * @param chunk auto-flush once the pending buffer reaches this many bytes; 0 = flush
 *     only on @ref axl_console_vt_buf_flush (the live, consumer-driven case).
 * @param guard true to raise TPL around the buffer swap — required when a persistent
 *     buffer is appended from a foreground writer (@c TPL_APPLICATION) and flushed
 *     from a driver-tick notify (@c TPL_CALLBACK); false for a single-threaded,
 *     one-call context (a snapshot).
 * @return true on success; false on allocation failure — in which case @p b is left
 *     with @c buf NULL and @ref axl_console_vt_buf_emit passes straight to @p sink
 *     (correct, just uncoalesced), so a caller may ignore the return.
 */
bool
axl_console_vt_buf_init(AxlConsoleVtBuf *b, AxlConsoleVtEmitFn sink, void *user,
                        size_t chunk, bool guard);

/** Append @p len bytes: accumulate when buffered (auto-flushing past @c chunk), else
    call @c sink directly. NULL/0-safe. */
void
axl_console_vt_buf_emit(AxlConsoleVtBuf *b, const char *bytes, size_t len);

/** Deliver everything buffered as ONE @c sink call, then empty the buffer. No-op when
    empty or unbuffered. NULL-safe. */
void
axl_console_vt_buf_flush(AxlConsoleVtBuf *b);

/** Release both buffers. Does NOT flush — call @ref axl_console_vt_buf_flush first if
    pending bytes matter. NULL-safe; leaves @p b zeroed. */
void
axl_console_vt_buf_dispose(AxlConsoleVtBuf *b);

#endif /* AXL_CONSOLE_VT_H */
