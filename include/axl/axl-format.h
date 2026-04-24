/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-format.h:
 *
 * Callback-driven printf engine. Format text directly into any sink
 * (buffer, network socket, file, hash) without intermediate allocation.
 *
 * This is the engine behind axl_printf, axl_snprintf, axl_asprintf,
 * and axl_string_append_printf. Exposed for consumers who need
 * custom formatting targets.
 *
 * Supports: %d %i %u %x %X %s %c %p %%
 * Length modifiers: l ll z
 * Flags: 0 - + (space)
 * Width: N or *
 * Precision: .N or .*
 */

#ifndef AXL_FORMAT_H
#define AXL_FORMAT_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AxlWriteFunc:
 *
 * Callback invoked by the format engine to emit output. May be called
 * multiple times per format call (once per literal segment and once
 * per formatted argument).
 */
typedef void (*AxlWriteFunc)(
    const char *data,  ///< output bytes (not NUL-terminated)
    size_t      len,   ///< number of bytes
    void       *ctx    ///< caller-provided context
);

/**
 * @brief Format with va_list into a write callback.
 *
 * The engine calls @p write_fn one or more times with formatted
 * output segments. No memory is allocated — all formatting uses
 * a small stack buffer.
 *
 * @code
 * void my_write(const char *data, size_t len, void *ctx) {
 *     axl_tcp_send((AxlTcp *)ctx, data, len, 0);
 * }
 *
 * va_list args;
 * va_start(args, fmt);
 * axl_vformat(my_write, sock, fmt, args);
 * va_end(args);
 * @endcode
 */
void
axl_vformat(
    AxlWriteFunc  write_fn,  ///< output callback
    void         *ctx,       ///< passed to write_fn
    const char   *fmt,       ///< printf-style format string
    va_list       args       ///< format arguments
);

/**
 * @brief Format into a write callback (variadic wrapper).
 *
 * Convenience wrapper around axl_vformat.
 *
 * @code
 * void buf_write(const char *data, size_t len, void *ctx) {
 *     // append to a custom buffer
 * }
 * axl_format(buf_write, &my_buf, "count=%d name=%s", 42, "AXL");
 * @endcode
 */
void
axl_format(
    AxlWriteFunc  write_fn,  ///< output callback
    void         *ctx,       ///< passed to write_fn
    const char   *fmt,       ///< printf-style format string
    ...                      ///< format arguments
) __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#endif /* AXL_FORMAT_H */
