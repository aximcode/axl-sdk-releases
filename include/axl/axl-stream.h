/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-stream.h:
 *
 * AxlStream — the byte-stream abstraction. Modeled on POSIX
 * `<stdio.h>`'s `FILE *`: a polymorphic handle that backs onto a
 * file, a memory buffer, the console, or any other byte sink/source
 * via an internal vtable. All public functions that take or return
 * `AxlStream *` live here.
 *
 * Three-layer API shape:
 *   1. Simple helpers (GLib-style): axl_print, axl_printerr.
 *   2. Stream I/O (POSIX-style): axl_fopen, axl_fread, axl_fprintf,
 *      axl_fgets, axl_readline, etc.
 *   3. Low-level: axl_read, axl_write, axl_pread, axl_pwrite.
 *
 * Filesystem operations (read-whole-file, dir walk, volume
 * enumerate, stat) live in `<axl/axl-fs.h>` — they're path-based,
 * not stream-based. AxlStream only knows about bytes.
 *
 * All strings are UTF-8. Paths in axl_fopen are converted to UCS-2
 * internally. Per-stream wire encoding (UCS-2 LE/BE/ASCII) is
 * available via @ref axl_stream_set_encoding.
 */

#ifndef AXL_STREAM_H
#define AXL_STREAM_H

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlStream AxlStream;
typedef long long axl_ssize_t;

// ---------------------------------------------------------------------------
// Standard streams (call axl_stream_init before use)
// ---------------------------------------------------------------------------

extern AxlStream *axl_stdout;
extern AxlStream *axl_stderr;
extern AxlStream *axl_stdin;

/**
 * **axl_stdout_raw** — sibling of axl_stdout for **binary output**.
 * Writes via `EFI_SHELL_PARAMETERS_PROTOCOL.StdOut->WriteFile`
 * directly, bypassing the UTF-8→UCS-2 conversion that axl_stdout
 * (and axl_print / axl_fprintf) does for console output. Use when a
 * tool needs bytes to traverse a pipe intact (dumping a RAM-disk
 * image, SPD blob, etc.).
 *
 * Symmetric with axl_stdin (which is also raw bytes); axl_stdout
 * remains the text-output path.
 *
 * `axl_write(axl_stdout_raw, ...)` returns -1 if the shell-params
 * protocol isn't published — there's no sensible console fallback
 * for binary bytes (the firmware console mangles non-CHAR16 input).
 * Tools that opt in should print a clear error in that case.
 */
extern AxlStream *axl_stdout_raw;

/**
 * @brief Initialize the standard stream globals.
 *
 * Sets up axl_stdout, axl_stderr, axl_stdin, and axl_stdout_raw.
 * Call once at startup (before any axl_print/axl_fprintf or
 * axl_read on axl_stdin). Invoked automatically by axl_runtime_init
 * — most consumers don't call it directly.
 *
 * **axl_stdin** is backed by `EFI_SHELL_PARAMETERS_PROTOCOL.StdIn`
 * when the shell publishes it (the typical case for shell-launched
 * apps including the right-hand side of a `|` pipe). Reading from
 * axl_stdin then consumes the bytes the shell captured from the
 * left-hand side of the pipe.
 *
 * If the shell-params protocol isn't published on this image
 * (cross-volume launches, BDS contexts, non-Shell-2.0 launches),
 * axl_stdin reads return EOF (0 bytes). Callers that need to
 * detect "stdin not connected" can issue a single zero-length
 * read and check the result.
 */
void
axl_stream_init(void);

/**
 * @brief Tee subsequent writes to @p extra alongside the console.
 *
 * After this call, every byte written to @ref axl_stdout via
 * `axl_print`, `axl_printf`, `axl_fprintf(axl_stdout, ...)`, or
 * direct `axl_write(axl_stdout, ...)` is also written to @p extra.
 * Pass NULL to clear an active tee. Multiple calls replace the
 * previous tee — there is no chain (the @p extra stream's own
 * `tee` field is intentionally ignored, so an accidental loop
 * just double-writes once instead of recursing).
 *
 * Lifetime: the caller owns @p extra and is responsible for
 * closing it. axl-sdk does not close the tee on exit. The typical
 * idiom for a `-o:<file>` log option is:
 *
 * @code
 * AxlStream *log = axl_fopen(path, "a");
 * axl_stream_set_stdout_tee(log);
 * axl_atexit(close_log_stream, log);
 * @endcode
 *
 * The tee target is written with the same UTF-8 caller bytes
 * passed to `axl_write` — NOT the post-transcode wire bytes the
 * source produced. If the tee target itself has a non-UTF-8
 * encoding set via @ref axl_stream_set_encoding, the tee
 * transcodes those caller bytes through its own encoding on the
 * way out (so a UTF-8 source teeing to a UCS-2-LE log file
 * produces a UCS-2-LE log file, not a UTF-8 one).
 *
 * If the primary write fails (returns -1), the tee still fires
 * with the caller bytes — log-on-best-effort. A broken primary
 * console must not cost you the log.
 *
 * @return 0 on success, -1 if axl_stdout isn't initialized.
 */
int
axl_stream_set_stdout_tee(
    AxlStream *extra   ///< stream to tee to (NULL clears)
);

/**
 * @brief Tee subsequent writes to @p extra alongside the stderr console.
 *
 * Symmetric to @ref axl_stream_set_stdout_tee but for axl_stderr.
 *
 * @return 0 on success, -1 if axl_stderr isn't initialized.
 */
int
axl_stream_set_stderr_tee(
    AxlStream *extra   ///< stream to tee to (NULL clears)
);

// ---------------------------------------------------------------------------
// Layer 1: Simple console helpers (GLib-style)
// ---------------------------------------------------------------------------

/**
 * @brief Print to stdout. Like g_print().
 *
 * @return number of bytes written, or -1 on error.
 */
int
axl_print(
    const char *fmt,  ///< printf-style format string
    ...
) __attribute__((format(printf, 1, 2)));

/**
 * @brief Alias for axl_print. Matches the design-doc name.
 */
#define axl_printf axl_print

/**
 * @brief Print to stderr. Like g_printerr().
 *
 * @return number of bytes written, or -1 on error.
 */
int
axl_printerr(
    const char *fmt,  ///< printf-style format string
    ...
) __attribute__((format(printf, 1, 2)));

// ---------------------------------------------------------------------------
// Layer 2: Stream I/O (POSIX fopen-style)
// ---------------------------------------------------------------------------

/**
 * @brief Open a file stream.
 *
 * Path is converted to UCS-2 internally.
 *
 * @return stream, or NULL on error. Close with axl_fclose().
 */
AxlStream *
axl_fopen(
    const char *path,  ///< file path (UTF-8, e.g. "fs0:/data.txt")
    const char *mode   ///< "r" (read), "w" (write/create), "a" (append)
);

/**
 * @brief Close a stream and free resources. NULL-safe.
 */
void
axl_fclose(
    AxlStream *s  ///< stream, or NULL
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlStream, axl_fclose)
#endif

/**
 * @brief Read size*count bytes from stream.
 *
 * Returns number of complete items read (may be less than count at
 * EOF or on error). Returns 0 on both EOF and error — use axl_read()
 * if you need to distinguish them (-1 = error, 0 = EOF).
 */
size_t
axl_fread(
    void      *buf,    ///< destination buffer
    size_t     size,   ///< item size in bytes
    size_t     count,  ///< number of items
    AxlStream *s       ///< stream
);

/**
 * @brief Write size*count bytes to stream.
 *
 * Returns number of complete items written.
 */
size_t
axl_fwrite(
    const void *buf,    ///< source buffer
    size_t      size,   ///< item size in bytes
    size_t      count,  ///< number of items
    AxlStream  *s       ///< stream
);

/**
 * @brief Write formatted text to a stream.
 *
 * @return number of bytes written, or -1 on error.
 */
int
axl_fprintf(
    AxlStream  *s,    ///< stream
    const char *fmt,  ///< printf-style format string
    ...
) __attribute__((format(printf, 2, 3)));

/**
 * @brief Read one line (up to and including '\\n').
 *
 * **Unbounded** — grows the internal buffer until '\\n' or EOF.
 * For arbitrary input (untrusted files, network streams, output
 * piped from another tool) prefer @ref axl_readline_max so a
 * single oversized line cannot exhaust memory.
 *
 * Caller frees with axl_free(). Returns NULL at EOF or on error.
 */
char *
axl_readline(
    AxlStream *s  ///< stream
);

/**
 * @brief Read one line with a memory cap.
 *
 * Like @ref axl_readline but bounded: stops appending after
 * @p max_bytes-1 bytes have been buffered. The returned string is
 * always NUL-terminated.
 *
 * **Truncation semantics**: when the cap is hit before a `\\n`, the
 * remaining bytes of that logical line are silently consumed from
 * the stream up to (and including) the next `\\n` or EOF. The next
 * `axl_readline_max` call therefore reads the next *logical* line —
 * line counting stays meaningful even when individual lines are
 * truncated.
 *
 * Detect truncation via "last char != `\\n`": a complete line ends
 * in `\\n`; a truncated one doesn't (and the byte count equals
 * `max_bytes-1`).
 *
 * Caller frees with axl_free(). Returns NULL at EOF (no bytes
 * read) or on error.
 */
char *
axl_readline_max(
    AxlStream *s,         ///< stream
    size_t     max_bytes  ///< maximum heap-buffered bytes (incl. trailing NUL)
);

/**
 * @brief Stateful line reader for chunk-buffered streams.
 *
 * Iterate line-by-line over a stream using a caller-supplied
 * working buffer. Constant memory regardless of input size; the
 * line slice on each `next` call points into the working buffer
 * and is invalidated by the next call.
 *
 * @code
 * AxlLineReader r;
 * char buf[64 * 1024];
 * axl_line_reader_init(&r, stream, buf, sizeof(buf));
 *
 * const char *line;
 * size_t      len;
 * bool        truncated;
 * while (axl_line_reader_next(&r, &line, &len, &truncated)) {
 *     // use line[0..len) — invalidated by next call
 * }
 * @endcode
 *
 * Lines longer than `buf_size-1` fire one `next` call with
 * `truncated == true` carrying the prefix; the rest of the
 * logical line is consumed before the next call. The buffer
 * doubles as the maximum line length.
 *
 * Fields are internal — callers must not touch them. Stack-
 * allocate the struct and pass &reader to the API. No teardown
 * function is needed; ownership of the working buffer stays with
 * the caller.
 */
typedef struct {
    /* internal — do not access directly */
    AxlStream *_stream;
    char      *_buf;
    size_t     _buf_size;
    size_t     _fill;
    size_t     _last_consumed;
    bool       _discard;
    bool       _eof;
    bool       _err;
} AxlLineReader;

/**
 * @brief Initialize a line reader.
 *
 * @param r         caller-allocated reader struct
 * @param s         source stream (caller-owned)
 * @param buf       working buffer (caller-owned, must outlive the reader)
 * @param buf_size  buffer size; must be ≥ 2 and is the max line length
 */
void
axl_line_reader_init(
    AxlLineReader *r,
    AxlStream     *s,
    char          *buf,
    size_t         buf_size
);

/**
 * @brief Read the next line from the stream.
 *
 * On a successful return, @p *line points into the reader's
 * working buffer (valid until the next `_next` call) and @p *len
 * is the byte count, **excluding** any trailing `\n` (a `\r` from
 * a CRLF pair is left for the caller to strip if desired). On
 * truncation, @p *truncated is set to true and the rest of the
 * logical line has already been drained from the stream — line
 * counts in the caller stay consistent.
 *
 * @return true if a line was read, false at EOF or on backend
 *     read error. Use @ref axl_line_reader_error to distinguish.
 */
bool
axl_line_reader_next(
    AxlLineReader  *r,
    const char    **line,
    size_t         *len,
    bool           *truncated
);

/**
 * @brief True if the most recent read returned a backend error.
 *
 * Distinguishes EOF (false return + this returns false) from a
 * real read failure (false return + this returns true).
 */
bool
axl_line_reader_error(
    const AxlLineReader *r
);

/**
 * @brief Per-line callback for @ref axl_walk_lines.
 *
 * @p line points into the caller's working buffer — valid only
 * for the duration of this callback invocation. @p len excludes
 * the trailing `\\n`; a `\\r` immediately before is left in @p line
 * for callers that want to keep CRLF context (most strip it).
 *
 * @p truncated is true when the logical line exceeded the working
 * buffer; in that case @p line holds the leading @p len bytes and
 * the rest of the line was discarded from the stream before this
 * callback fired.
 *
 * Return 0 to continue, any non-zero value to stop iteration —
 * the value is propagated back from `axl_walk_lines`.
 */
typedef int (*AxlLineFn)(
    const char  *line,
    size_t       len,
    bool         truncated,
    void        *user
);

/**
 * @brief Callback wrapper around @ref AxlLineReader.
 *
 * Convenience for callers that prefer callback dispatch over
 * iterator-style `while (next(...))` loops. Equivalent to:
 *
 * @code
 * AxlLineReader r;
 * axl_line_reader_init(&r, s, buf, buf_size);
 * while (axl_line_reader_next(&r, &line, &len, &truncated)) {
 *     int rc = fn(line, len, truncated, user);
 *     if (rc != 0) return rc;
 * }
 * return axl_line_reader_error(&r) ? -1 : 0;
 * @endcode
 *
 * Most callers should prefer the reader-struct form for its
 * normal-scope local variables and standard control flow. Use
 * this wrapper only when the per-line work is genuinely
 * stateless or when the dispatch shape simplifies a generic API.
 *
 * @return 0 on full traversal to EOF, the callback's non-zero
 *     return if it stopped, or -1 on backend read error or
 *     invalid arguments.
 */
int
axl_walk_lines(
    AxlStream  *s,         ///< stream
    char       *buf,       ///< working buffer (caller-owned)
    size_t      buf_size,  ///< buffer capacity (>= 2; doubles as max line length)
    AxlLineFn   fn,        ///< per-line callback
    void       *user       ///< opaque user pointer for the callback
);

// ---------------------------------------------------------------------------
// Stream positioning
// ---------------------------------------------------------------------------

#define AXL_SEEK_SET  0  ///< seek from beginning
#define AXL_SEEK_CUR  1  ///< seek from current position
#define AXL_SEEK_END  2  ///< seek from end of file

/**
 * @brief Set the stream position.
 *
 * @return 0 on success, -1 on error or if not supported.
 */
int
axl_fseek(
    AxlStream *s,      ///< stream
    int64_t    offset,  ///< byte offset (may be negative for CUR/END)
    int        whence   ///< AXL_SEEK_SET, AXL_SEEK_CUR, or AXL_SEEK_END
);

/**
 * @brief Get the current stream position.
 *
 * @return position in bytes, or -1 on error.
 */
int64_t
axl_ftell(
    AxlStream *s  ///< stream
);

/**
 * @brief Check if the stream has reached end-of-file.
 *
 * Set when read returns 0 bytes. Cleared by axl_fseek.
 *
 * @return true if at EOF.
 */
bool
axl_feof(
    AxlStream *s  ///< stream
);

/**
 * @brief Flush pending writes to the underlying file. NULL-safe.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_fflush(
    AxlStream *s  ///< stream
);

// ---------------------------------------------------------------------------
// Per-stream encoding (caller-side UTF-8 ↔ wire-side encoding)
// ---------------------------------------------------------------------------

/**
 * Wire-side encoding for a stream. The caller always works in UTF-8
 * — `axl_read` returns UTF-8, `axl_write` accepts UTF-8 — and
 * `axl_stream_set_encoding` declares what's on the wire underneath.
 *
 * Default is @ref AXL_ENC_UTF8, which is a passthrough (no
 * transcoding) — existing behavior is unchanged for any stream
 * the consumer doesn't explicitly configure.
 *
 * Transcoding is **permissive** — bad input never produces an error:
 *   - Invalid UTF-8 in writes → encoded byte-for-byte as Latin-1.
 *   - Invalid wire bytes / surrogate halves in reads → transcoded
 *     in their BMP shape (U+D800..U+DFFF round-trip as a 3-byte
 *     UTF-8 sequence; lone wire byte at end of stream is dropped).
 *   - Codepoints above U+FFFF on encode to UCS-2/ASCII → replaced
 *     with `?`.
 */
typedef enum {
    AXL_ENC_UTF8 = 0,   ///< default — passthrough, no transcoding
    AXL_ENC_UCS2_LE,    ///< native UEFI; what the shell pipes use
    AXL_ENC_UCS2_BE,    ///< network-byte-order UCS-2; rare
    AXL_ENC_ASCII,      ///< 7-bit; high bytes replaced with `?`
} AxlEncoding;

/**
 * @brief Set the wire-side encoding for a stream.
 *
 * Applies to the byte-I/O primitives — @ref axl_read, @ref axl_write,
 * @ref axl_fread, @ref axl_fwrite, @ref axl_readline, @ref axl_fgets.
 * Does **not** affect @ref axl_print / @ref axl_printf / @ref
 * axl_printerr — those go through the console_write path which
 * does its own UTF-8→UCS-2 conversion.
 *
 * Switching encoding mid-stream **discards** any partial multi-byte
 * sequence that was being buffered under the previous encoding.
 * That avoids silently splicing stale partial bytes onto the new
 * encoding's byte stream. Likewise, @ref axl_fseek discards
 * transcode buffers — they describe state at the pre-seek position.
 *
 * @return 0 on success, -1 if @p s is NULL or @p enc is out of range.
 */
int
axl_stream_set_encoding(
    AxlStream  *s,    ///< stream
    AxlEncoding enc   ///< wire-side encoding
);

/**
 * @brief Get the current wire-side encoding for a stream.
 *
 * @return current encoding (defaults to @ref AXL_ENC_UTF8).
 */
AxlEncoding
axl_stream_get_encoding(
    AxlStream *s   ///< stream
);

// ---------------------------------------------------------------------------
// POSIX-shape conveniences
// ---------------------------------------------------------------------------

/**
 * @brief Read up to @p size-1 bytes from @p stream into @p buf,
 *        stopping at end-of-line or EOF, and NUL-terminate.
 *
 * Like POSIX `fgets()`: reads at most one less than @p size bytes,
 * stopping after the first newline (which is included in @p buf),
 * at EOF, or on error. The buffer is always NUL-terminated when a
 * non-NULL return is delivered.
 *
 * @return @p buf on success, NULL at EOF (with no bytes read) or
 *     on error (use @ref axl_ferror to distinguish).
 */
char *
axl_fgets(
    char      *buf,    ///< destination buffer (must be at least @p size bytes)
    int        size,   ///< buffer size in bytes (incl. NUL)
    AxlStream *stream  ///< source stream
);

/**
 * @brief Write formatted text to a stream (va_list variant).
 *
 * Like POSIX `vfprintf()`. The @ref axl_fprintf entry point wraps
 * this for the variadic case.
 *
 * @return number of bytes written, or -1 on error.
 */
int
axl_vfprintf(
    AxlStream  *stream,  ///< stream
    const char *fmt,     ///< printf-style format string
    va_list     ap       ///< argument list
);

/**
 * @brief Test the sticky error indicator on a stream.
 *
 * Set by any backend read/write/seek error. Mirror of POSIX
 * `ferror()`. Cleared by @ref axl_clearerr.
 *
 * @return true if an error has been signaled on @p stream.
 */
bool
axl_ferror(
    AxlStream *stream  ///< stream
);

/**
 * @brief Clear both the EOF and error indicators on @p stream.
 *
 * Mirror of POSIX `clearerr()`.
 */
void
axl_clearerr(
    AxlStream *stream  ///< stream
);

// ---------------------------------------------------------------------------
// Text-decoding stream wrapper
// ---------------------------------------------------------------------------

/**
 * @brief Wrap a raw byte stream as a UTF-8 text stream.
 *
 * Source encoding is classified at construction time. In priority:
 *
 * 1. **BOM**:
 *    - `FF FE`    → UTF-16 LE; BOM consumed, body transcoded to UTF-8
 *    - `FE FF`    → UTF-16 BE; BOM consumed, body transcoded to UTF-8
 *    - `EF BB BF` → UTF-8 BOM; consumed, body returned verbatim
 * 2. **Headerless UCS-2 sniff** (≥16 bytes available, no BOM): if
 *    every odd-position byte is 0x00 the source is treated as
 *    UCS-2 LE; if every even-position byte is 0x00, UCS-2 BE. The
 *    sniffed bytes are non-consuming and re-emerge on the first
 *    read. This catches UEFI shells that write UCS-2 LE without a
 *    BOM (`some-cmd > out.txt`). UTF-8 ASCII text never matches
 *    (no NULs anywhere); the remaining false-positive risk is
 *    binary content with NULs at every alternate byte — wrap such
 *    streams only if they're known to be text.
 * 3. **Otherwise** → passthrough (raw bytes returned as-is).
 *
 * Transcoding is incremental and bounded-memory regardless of source
 * size — wrap a multi-MB pipe and read line by line. Returned reads
 * are valid UTF-8 even when the caller's buffer cuts mid-character
 * (the wrapper holds back partial transcoded sequences for the next
 * call).
 *
 * Useful for shell-pipe consumers in UEFI: the shell wraps text
 * output as UCS-2 LE, so wrapping `axl_stdin` gives every text-
 * oriented tool transparent UTF-8 input regardless of whether the
 * upstream is a built-in (UCS-2), an AXL tool that wrote via
 * `axl_print` (UCS-2 after console conversion), or a binary tool
 * that wrote raw UTF-8 (passthrough).
 *
 * The wrapper does **not** take ownership of @p src — the caller is
 * responsible for closing both eventually.
 *
 * **Surrogate-half caveat.** Codepoints above U+FFFF are encoded in
 * UTF-16 as a pair of surrogate code units (U+D800-U+DFFF). This
 * wrapper transcodes each code unit independently as a 3-byte
 * UTF-8 sequence rather than combining the pair into the proper
 * UTF-8 4-byte form. The output is **not strictly valid UTF-8** for
 * codepoints > U+FFFF: lone-surrogate sequences will be rejected by
 * strict UTF-8 validators (axl_utf8_validate, JSON encoders,
 * MultiByteToWideChar-style decoders). Lenient consumers (grep,
 * substring search, console display) tolerate it. Almost all
 * real-world UEFI Shell content is BMP-only ASCII / Latin-1 /
 * common scripts, so this hasn't bitten in practice; if a real
 * consumer needs proper SMP support, a follow-up can add the
 * surrogate-pair combiner.
 *
 * @return wrapper stream (free with @ref axl_fclose), or NULL on
 *     OOM or NULL @p src.
 */
AxlStream *
axl_text_stream_wrap(
    AxlStream *src   ///< source byte stream (caller-owned)
);

// ---------------------------------------------------------------------------
// Buffer streams (in-memory, auto-growing)
// ---------------------------------------------------------------------------

/**
 * @brief Create an in-memory buffer stream.
 *
 * Supports read, write, pread, pwrite.
 *
 * @return stream, or NULL on allocation failure.
 */
AxlStream *
axl_bufopen(void);

/**
 * @brief Peek at buffer contents without consuming.
 *
 * The returned pointer is owned by the stream and invalidated
 * by writes or close.
 */
const void *
axl_bufdata(
    AxlStream *s,     ///< buffer stream
    size_t    *size   ///< (out, optional): buffer size
);

/**
 * @brief Transfer ownership of buffer to caller.
 *
 * Stream becomes empty. Caller frees with axl_free().
 */
void *
axl_bufsteal(
    AxlStream *s,     ///< buffer stream
    size_t    *size   ///< (out, optional): buffer size
);

// ---------------------------------------------------------------------------
// Layer 3: Low-level read/write/pread/pwrite
// ---------------------------------------------------------------------------

/**
 * @brief Read up to @a count bytes from stream at current position.
 *
 * @return bytes read, 0 at EOF, -1 on error.
 */
axl_ssize_t
axl_read(
    AxlStream *s,      ///< stream
    void      *buf,    ///< destination buffer
    size_t     count   ///< max bytes to read
);

/**
 * @brief Write @a count bytes to stream at current position.
 *
 * @return bytes written, -1 on error.
 */
axl_ssize_t
axl_write(
    AxlStream  *s,     ///< stream
    const void *buf,   ///< source buffer
    size_t      count  ///< bytes to write
);

/**
 * @brief Read up to @a count bytes at @a offset without changing stream position.
 *
 * @return bytes read, -1 on error or if not supported.
 */
axl_ssize_t
axl_pread(
    AxlStream *s,       ///< stream
    void      *buf,     ///< destination buffer
    size_t     count,   ///< max bytes to read
    size_t     offset   ///< byte offset to read from
);

/**
 * @brief Write @a count bytes at @a offset without changing stream position.
 *
 * @return bytes written, -1 on error or if not supported.
 */
axl_ssize_t
axl_pwrite(
    AxlStream  *s,       ///< stream
    const void *buf,     ///< source buffer
    size_t      count,   ///< bytes to write
    size_t      offset   ///< byte offset to write at
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_STREAM_H */
