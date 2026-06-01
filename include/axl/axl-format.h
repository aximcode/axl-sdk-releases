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

/**
 * Minimum @p bufsz for axl_dtoa: the shortest decimal representation
 * of any IEEE-754 double needs at most 17 significant digits; +1 for
 * the NUL terminator axl_dtoa writes.
 */
#define AXL_DTOA_BUF_MIN  18

/**
 * @brief Shortest round-trippable decimal digits of a double (Grisu2).
 *
 * Converts the finite double @p value to the *shortest* string of
 * decimal digits that, when read back, reproduces @p value exactly
 * (round-trip). This is the engine behind %f / %e / %g and the
 * primitive a consumer needs to serialize a double without losing
 * precision.
 *
 * Output is split into three pieces so the caller can render any C
 * float format from one conversion:
 *   - @p buf receives the significant digits as ASCII '0'..'9', with
 *     no sign, no decimal point, and no exponent. NUL-terminated.
 *   - @p out_decpt receives the position of the decimal point measured
 *     in digits from the start of @p buf: the value's magnitude is
 *     `0.<digits> x 10^(*out_decpt)` ... equivalently
 *     `<digits-as-integer> x 10^(*out_decpt - ndigits)`. So *out_decpt
 *     is the count of digits that belong to the left of the decimal
 *     point (it may be <= 0 or > ndigits). Examples: 1.5 -> "15",
 *     decpt 1; 0.001 -> "1", decpt -2; 100.0 -> "1", decpt 3.
 *   - @p out_neg receives true for a negative @p value (including
 *     -0.0), false otherwise.
 *
 * Zero yields "0" with @p *out_decpt == 1. The result is the
 * canonical shortest form: trailing zeros are not emitted (100.0 is
 * "1" with decpt 3, not "100").
 *
 * @p value MUST be finite. NaN and +/-infinity are NOT handled here
 * (callers detect them first — `v != v` for NaN, `|v| > DBL_MAX` for
 * infinity); passing one returns 0.
 *
 * No allocation, no libm, no libc. Uses a ~1.3KB cached-powers table.
 *
 * @return number of digits written to @p buf (>= 1), or 0 on error
 *         (@p buf NULL, @p bufsz < AXL_DTOA_BUF_MIN, or @p value
 *         non-finite). @p out_decpt / @p out_neg may be NULL to skip.
 */
int
axl_dtoa(
    double   value,       ///< finite value to convert
    char    *buf,         ///< [out] digit buffer (>= AXL_DTOA_BUF_MIN bytes)
    size_t   bufsz,       ///< size of @p buf
    int     *out_decpt,   ///< [out] decimal-point position (NULL OK)
    int     *out_neg      ///< [out] 1 if negative (NULL OK)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_FORMAT_H */
