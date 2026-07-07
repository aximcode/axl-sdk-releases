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
 * available via axl_stream_set_encoding.
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
 * **axl_stderr_raw** — binary sibling of axl_stderr. Writes via the shell
 * StdErr handle (EFI_SHELL_PARAMETERS_PROTOCOL.StdErr), bypassing the
 * UTF-8→UCS-2 console conversion, so a tool can emit raw diagnostic bytes
 * under `2>`. Returns -1 when no shell StdErr handle is available.
 */
extern AxlStream *axl_stderr_raw;

/**
 * @brief Initialize the standard stream globals.
 *
 * Sets up axl_stdout, axl_stderr, axl_stdin, axl_stdout_raw, and
 * axl_stderr_raw.
 * Call once at startup (before any axl_print/axl_fprintf or
 * axl_read on axl_stdin). Invoked automatically by axl_runtime_init
 * — most consumers don't call it directly.
 *
 * **axl_stdin** is backed by `EFI_SHELL_PARAMETERS_PROTOCOL.StdIn`
 * when the shell publishes it (the typical case for shell-launched
 * apps including the right-hand side of a `|` pipe). When StdIn is
 * **redirected** — `cmd | tool`, `tool < file` — reading axl_stdin
 * consumes those captured bytes, byte-for-byte.
 *
 * When StdIn is **interactive** (the command was typed with no
 * redirection, so StdIn is the console itself), axl_stdin switches to
 * canonical console line editing: a read blocks until the user presses
 * Enter, then delivers the echoed, Backspace-edited line (with a
 * trailing `\n`). This mirrors POSIX tty-vs-pipe behavior and is what
 * lets `axl_readline` / `axl_stdin_text` serve an interactive prompt
 * without the caller distinguishing the two cases (see
 * axl_stdin_is_interactive()). To read raw console keystrokes with no
 * line assembly, use axl_console_read_key() instead.
 *
 * If the shell-params protocol isn't published on this image
 * (cross-volume launches, BDS contexts, non-Shell-2.0 launches),
 * axl_stdin reads return EOF (0 bytes) — it does not fall back to the
 * console, so a context that never expected a keyboard won't block.
 * Callers that need to detect "stdin not connected" can issue a single
 * zero-length read and check the result.
 */
void
axl_stream_init(void);

/**
 * @brief Is the shell's StdIn an interactive console rather than a
 *        redirected file/pipe?
 *
 * True when a command was typed with no input redirection, so
 * `EFI_SHELL_PARAMETERS_PROTOCOL.StdIn` is the console (not a
 * byte-addressable file). False when StdIn is redirected (`| tool`,
 * `tool < file`) — those cases carry a real file handle. Also false
 * when no shell StdIn handle is published at all (BDS / non-shell
 * contexts): there is nothing to read interactively through the shell.
 *
 * This is the predicate behind axl_stdin's automatic console-line-edit
 * fallback (see axl_stream_init()). Consumers rarely need it —
 * axl_readline already routes correctly — but it is useful for deciding
 * whether to draw a prompt, or to switch to a hidden (password) read via
 * axl_console_readline_ex(), which only makes sense interactively.
 *
 * **"interactive console" does NOT mean "a human is present."** An
 * unattended `startup.nsh` at boot has StdIn = the console (so this
 * returns true) but nobody at the keyboard — a bare `axl_stdin` /
 * `axl_readline` read there blocks until the outer boot timeout instead
 * of returning EOF. In an automated script, either redirect the input
 * (`tool < in`) or gate the read on this predicate and skip it when it
 * would block (read-if-piped-else-EOF).
 *
 * @return true if StdIn is an interactive console; false if redirected
 *     or not connected.
 */
bool
axl_stdin_is_interactive(void);

/**
 * @brief Tee subsequent writes to @p extra alongside the console.
 *
 * After this call, every byte written to axl_stdout via
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
 * encoding set via axl_stream_set_encoding, the tee
 * transcodes those caller bytes through its own encoding on the
 * way out (so a UTF-8 source teeing to a UCS-2-LE log file
 * produces a UCS-2-LE log file, not a UTF-8 one).
 *
 * If the primary write fails (returns -1), the tee still fires
 * with the caller bytes — log-on-best-effort. A broken primary
 * console must not cost you the log.
 *
 * @return AXL_OK on success, AXL_ERR if axl_stdout isn't initialized.
 */
int
axl_stream_set_stdout_tee(
    AxlStream *extra   ///< stream to tee to (NULL clears)
);

/**
 * @brief Tee subsequent writes to @p extra alongside the stderr console.
 *
 * Symmetric to axl_stream_set_stdout_tee but for axl_stderr.
 *
 * @return AXL_OK on success, AXL_ERR if axl_stderr isn't initialized.
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
 *
 * If @p s has output buffering (axl_stream_set_buffering), any buffered
 * bytes are flushed to the sink first, then the buffer is freed. A sink
 * error during that final flush is not reported through this void return —
 * call axl_fflush explicitly beforehand if you need to observe it.
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
 *
 * On `axl_stdin` in interactive mode reads are line-cooked and blocking
 * (see axl_read() and axl_stream_init()), not raw keystrokes.
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
 * piped from another tool) prefer axl_readline_max so a
 * single oversized line cannot exhaust memory.
 *
 * Caller frees with axl_free(). Returns NULL at EOF or on error.
 *
 * On `axl_stdin` in interactive mode this blocks on the console and
 * returns line-buffered, echoed input — the automatic prompt-vs-pipe
 * routing (see axl_stream_init(), axl_stdin_is_interactive()).
 */
char *
axl_readline(
    AxlStream *s  ///< stream
);

/**
 * @brief Read one line with a memory cap.
 *
 * Like axl_readline but bounded: stops appending after
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
 *     read error. Use axl_line_reader_error to distinguish.
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
 * @brief Per-line callback for axl_walk_lines.
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
 * @brief Callback wrapper around AxlLineReader.
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
 * @return AXL_OK on success, AXL_ERR on error or if not supported.
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
 * @brief Flush pending writes to the sink. NULL-safe.
 *
 * If @p s has output buffering (axl_stream_set_buffering), this drains the
 * buffered bytes through the stream's write path — including any UTF-8 →
 * UCS-2 transcode and tee — to the sink. Note a stream with a non-UTF-8
 * encoding may still retain an *incomplete* trailing multi-byte unit in the
 * transcoder until the completing bytes arrive; flush emits every complete
 * unit but cannot invent the missing tail. It also invokes the underlying
 * sink flush (e.g. a file backend's firmware cache flush) when present.
 *
 * @return AXL_OK on success, AXL_ERR on a sink write/flush error.
 */
int
axl_fflush(
    AxlStream *s  ///< stream
);

// ---------------------------------------------------------------------------
// Output buffering (stdio setvbuf family)
// ---------------------------------------------------------------------------

/// Default buffer capacity used when axl_stream_set_buffering is given
/// size 0 (and by axl_setlinebuf / axl_setbuf). Sized to hold a typical
/// console line plus headroom without a per-line reallocation.
#define AXL_STREAM_BUF_DEFAULT_SIZE  1024u

/**
 * @brief Output buffering mode for a stream — mirrors C stdio.
 *
 * Selects how axl_write / axl_print* / axl_fwrite coalesce writes before
 * they reach the sink. This is an **output** policy only: as in C stdio,
 * line buffering is defined for output, and AXL performs no input
 * read-ahead, so the mode never affects reads.
 */
typedef enum {
    AXL_STREAM_BUF_NONE = 0,  ///< unbuffered — every write goes straight to
                              ///< the sink (stdio `_IONBF`). The AXL default.
    AXL_STREAM_BUF_LINE,      ///< line-buffered — on each write, everything
                              ///< through the last '\n' is flushed; a partial
                              ///< trailing line is retained. If the buffer
                              ///< fills before any '\n', the WHOLE buffer is
                              ///< flushed (as stdio `_IOLBF` does) so writes
                              ///< never stall for a newline that won't come.
    AXL_STREAM_BUF_FULL,      ///< fully buffered — bytes are held until the
                              ///< buffer fills, then flushed as a block
                              ///< (stdio `_IOFBF`).
} AxlStreamBuffering;

/**
 * @brief Set the output buffering mode for @p s.
 *
 * Controls how writes to @p s coalesce before hitting the sink. The buffer
 * lives in axl_write, which axl_print / axl_printf / axl_fprintf and
 * axl_fwrite all funnel through (the printf family formats into a scratch
 * buffer then issues one axl_write), so every text-output entry point is
 * coalesced uniformly. Buffering happens on the **raw byte stream before**
 * any UTF-8 → UCS-2 transcode (axl_stream_set_encoding), so a UCS-2 console
 * still coalesces correctly, and a tee (axl_stream_set_stdout_tee) sees the
 * same bytes at flush time.
 *
 * **AXL streams are AXL_STREAM_BUF_NONE by default.** Unlike C stdio, AXL
 * does NOT auto-select line/full buffering from tty-ness: a UEFI app can
 * exit through a crt0 path that runs no atexit hook, so buffered output
 * could be silently lost. Opt in explicitly, and **flush before you exit**
 * — axl_fflush drains the buffer, and axl_fclose flushes then frees it.
 * (Because axl_stdout / axl_stderr are process globals that are never
 * fclosed, code that buffers them owns the final axl_fflush.)
 *
 * Under LINE / FULL a successful write returns the byte count **accepted
 * into the buffer**, which does not mean the sink took them — a sink error
 * surfaces at the later axl_fflush / axl_fclose and via axl_ferror, not at
 * the buffered write. Do not treat a buffered write's return as durability.
 *
 * Switching mode first flushes any bytes already buffered under the old
 * mode. A single write larger than the buffer first flushes any pending
 * bytes (preserving order), then writes directly — buffering never
 * truncates, splits, or reorders an over-size write.
 *
 * @param s     stream (NULL-safe — returns AXL_ERR).
 * @param mode  one of AxlStreamBuffering.
 * @param size  buffer capacity in bytes for LINE / FULL; 0 selects
 *              AXL_STREAM_BUF_DEFAULT_SIZE. Ignored for NONE.
 * @return AXL_OK on success; AXL_ERR on a NULL stream, a stream with no
 *     write side, or an allocation failure (on alloc failure the stream
 *     is left unbuffered — writes still go through, just uncoalesced).
 */
int
axl_stream_set_buffering(
    AxlStream          *s,     ///< stream to configure
    AxlStreamBuffering  mode,  ///< buffering mode
    size_t              size   ///< buffer size (0 = default; ignored for NONE)
);

/**
 * @brief Current output buffering mode of @p s.
 *
 * @return the mode set by axl_stream_set_buffering, or AXL_STREAM_BUF_NONE
 *     for an unconfigured or NULL stream.
 */
AxlStreamBuffering
axl_stream_get_buffering(
    AxlStream *s  ///< stream (NULL-safe)
);

/**
 * @brief stdio `setvbuf()` shim over AxlStream.
 *
 * Thin wrapper over axl_stream_set_buffering for muscle memory. Like C
 * `setvbuf` you would normally call it **before the first write**, though
 * the underlying axl_stream_set_buffering also supports a mid-stream switch
 * (it flushes the old buffer first).
 *
 * **The @p buf argument is ignored — pass NULL.** AXL always owns the
 * buffer: a caller-supplied buffer whose lifetime must outlive the stream
 * is a use-after-free hazard in RAII / UEFI code, so the borrow-a-buffer
 * half of the C API is deliberately not honored (a debug build asserts
 * @p buf is NULL to catch a mistaken hand-off). @p mode is the AXL enum,
 * not the C `_IOFBF` / `_IOLBF` / `_IONBF` macros.
 *
 * @return AXL_OK on success, AXL_ERR on failure (see
 *     axl_stream_set_buffering).
 */
int
axl_setvbuf(
    AxlStream          *s,     ///< stream
    char               *buf,   ///< IGNORED — pass NULL (AXL owns the buffer)
    AxlStreamBuffering  mode,  ///< buffering mode (AXL enum)
    size_t              size   ///< buffer size (0 = default)
);

/**
 * @brief stdio `setlinebuf()` shim — line-buffer @p s with a default-size
 *     buffer. Equivalent to axl_stream_set_buffering(s, AXL_STREAM_BUF_LINE, 0).
 *     Void like C: on allocation failure the stream is left unbuffered.
 */
void
axl_setlinebuf(
    AxlStream *s  ///< stream (NULL-safe)
);

/**
 * @brief stdio `setbuf()` shim.
 *
 * Matches C `setbuf`'s `buf ? _IOFBF : _IONBF` selection: a non-NULL
 * @p buf requests full buffering (default size), NULL requests
 * unbuffered. **The @p buf pointer itself is ignored** (AXL owns the
 * buffer) — only NULL-vs-non-NULL is consulted. Void like C: on
 * allocation failure the stream is left unbuffered.
 */
void
axl_setbuf(
    AxlStream *s,   ///< stream (NULL-safe)
    char      *buf  ///< NULL = unbuffered; non-NULL = full-buffered (ptr ignored)
);

// ---------------------------------------------------------------------------
// Interactive / no-EOF source marking (line-discipline layer)
// ---------------------------------------------------------------------------

/**
 * @brief Mark @p s as an interactive / no-EOF source (or clear the mark).
 *
 * Interactive sources — a live console, an interactive socket REPL —
 * deliver input one line at a time and never signal EOF while the peer is
 * connected. Any code that reads them must treat a short read as a
 * COMPLETE result and must not loop to fill a buffer, or it blocks forever
 * waiting for bytes the user has not typed yet.
 *
 * The one place in AXL that would otherwise over-read such a source is
 * axl_text_stream_wrap's construction-time encoding sniff (it reads a
 * probe window to detect a BOM / headerless UCS-2). When the wrapped
 * source is flagged interactive, that sniff is **skipped entirely** —
 * interactive input is already UTF-8 and line-cooked, so there is no BOM
 * or UCS-2 to classify — and the wrapper becomes a plain passthrough that
 * returns one line per read.
 *
 * This is the **line-discipline** axis (C's `termios`/ICANON layer), which
 * is orthogonal to buffering (axl_stream_set_buffering, C's `setvbuf`):
 * one governs whether reads over-consume, the other how writes coalesce.
 *
 * `axl_stdin` reports interactivity **dynamically** via
 * axl_stdin_is_interactive() (per console handle) and does not need this
 * flag set — axl_text_stream_wrap consults that predicate for it. Set this
 * on a caller-owned no-EOF byte stream you intend to wrap as text.
 *
 * The text wrapper returned by axl_text_stream_wrap **inherits** the source's
 * effective interactive mark, so testing the wrapper (or wrapping it again)
 * reports interactive too — not just the original source handle.
 */
void
axl_stream_set_interactive(
    AxlStream *s,           ///< stream to mark (NULL-safe no-op)
    bool       interactive  ///< true = interactive/no-EOF; false = normal
);

/**
 * @brief Whether @p s carries the interactive / no-EOF mark.
 *
 * Reflects the last axl_stream_set_interactive on @p s. This is the
 * per-stream FLAG only; it does **not** consult axl_stdin_is_interactive()
 * for the stdin console (that verdict is dynamic and handle-specific — use
 * axl_stdin_is_interactive() directly for stdin).
 *
 * @return true if flagged interactive; false otherwise (including NULL).
 */
bool
axl_stream_get_interactive(
    AxlStream *s  ///< stream (NULL-safe)
);

// ---------------------------------------------------------------------------
// Per-stream encoding (caller-side UTF-8 ↔ wire-side encoding)
// ---------------------------------------------------------------------------

/**
 * Wire-side encoding for a stream. The caller always works in UTF-8
 * — `axl_read` returns UTF-8, `axl_write` accepts UTF-8 — and
 * `axl_stream_set_encoding` declares what's on the wire underneath.
 *
 * Default is AXL_ENC_UTF8, which is a passthrough (no
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
 * @brief Sniff a file's text encoding from a leading byte sample.
 *
 * Recognizes a UTF-8 BOM (EF BB BF), UTF-16 LE BOM (FF FE), and UTF-16
 * BE BOM (FE FF); failing a BOM, applies a light BOM-less heuristic
 * (interleaved NUL bytes ⇒ UCS-2 LE/BE) and otherwise reports UTF-8.
 * @p out_has_bom (optional) is set true when a BOM was present, so a
 * caller can round-trip it on save.
 *
 * @return the detected encoding (UCS-2 variants for UTF-16; UTF-8 is
 *     the default).
 */
AxlEncoding
axl_detect_encoding(
    const void *prefix,        ///< leading bytes of the file
    size_t      len,           ///< number of bytes available
    bool       *out_has_bom    ///< [out, optional] BOM present
);

/**
 * @brief Set the wire-side encoding for a stream.
 *
 * Applies to the byte-I/O primitives — axl_read, axl_write,
 * axl_fread, axl_fwrite, axl_readline, axl_fgets.
 * Does **not** affect axl_print / axl_printf / axl_printerr — those go through the console_write path which
 * does its own UTF-8→UCS-2 conversion.
 *
 * Switching encoding mid-stream **discards** any partial multi-byte
 * sequence that was being buffered under the previous encoding.
 * That avoids silently splicing stale partial bytes onto the new
 * encoding's byte stream. Likewise, axl_fseek discards
 * transcode buffers — they describe state at the pre-seek position.
 *
 * @return AXL_OK on success, AXL_ERR if @p s is NULL or @p enc is out of range.
 */
int
axl_stream_set_encoding(
    AxlStream  *s,    ///< stream
    AxlEncoding enc   ///< wire-side encoding
);

/**
 * @brief Get the current wire-side encoding for a stream.
 *
 * @return current encoding (defaults to AXL_ENC_UTF8).
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
 *     on error (use axl_ferror to distinguish).
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
 * Like POSIX `vfprintf()`. The axl_fprintf entry point wraps
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
 * `ferror()`. Cleared by axl_clearerr.
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
 * **Interactive sources are short-circuited.** When @p src is an
 * interactive / no-EOF source — `axl_stdin` on an interactive console
 * (axl_stdin_is_interactive()), or any stream flagged via
 * axl_stream_set_interactive() — the input is already UTF-8, line-cooked,
 * and never returns EOF, so there is no BOM or UCS-2 to detect. The
 * classifier probe is skipped entirely and the wrapper is a plain
 * passthrough returning one line per read. (Probing it would block until
 * a full probe window's worth of bytes arrived — a hang at the console.)
 * Redirected / piped stdin is not interactive and is classified normally.
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
 * @return wrapper stream (free with axl_fclose), or NULL on
 *     OOM or NULL @p src.
 */
AxlStream *
axl_text_stream_wrap(
    AxlStream *src   ///< source byte stream (caller-owned)
);

/**
 * @brief A fresh text-decoding view of standard input.
 *
 * Convenience for `axl_text_stream_wrap(axl_stdin)`: the returned stream
 * transparently decodes the UEFI shell's UCS-2 pipe output (and BOM'd
 * UTF-16 / UTF-8) to UTF-8, so `axl_readline()` on it reads piped text
 * regardless of the shell's `|` encoding — no `|a` needed. `<`
 * redirection passes through unchanged. **Interactive** input arrives
 * line-buffered and already UTF-8 (see axl_stream_init() and
 * axl_stdin_is_interactive()), so it needs no decoding but is cooked,
 * not raw. For raw redirected/piped bytes, read `axl_stdin` directly;
 * for raw console keystrokes (no line assembly), use axl_console_read_key()
 * — reading `axl_stdin` at an interactive console is line-cooked, not raw.
 *
 * **The caller owns the returned stream** — close it with axl_fclose.
 * A NEW wrapper is returned on every call (it is **not** cached): the
 * wrapper buffers read-ahead bytes and a one-time encoding sniff, so a
 * resident shared-driver must create a fresh one per dispatch and must
 * not hold one across launcher invocations (a stale wrapper would
 * replay a previous invocation's buffered input).
 *
 * **Construction classifies the source encoding.** For redirected or
 * piped stdin (`<`, `|`) it eager-reads a probe to detect a BOM or
 * headerless UCS-2, so construction blocks until that input arrives.
 * For **interactive** stdin there is nothing to classify (already
 * UTF-8, no BOM, and the console never signals EOF), so NO eager read
 * happens: construction returns immediately and the *first* read blocks
 * until the user enters one line (Enter), then returns that line. Either
 * way, create it at the point you are ready to read, not speculatively.
 *
 * @return a text-decoding read stream over stdin (free with axl_fclose),
 *     or NULL on allocation failure. (axl_text_stream_wrap also returns
 *     NULL for a write-only/NULL source, but axl_stdin is always
 *     readable, so that path cannot trigger here.)
 */
AxlStream *
axl_stdin_text(void);

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
 * On `axl_stdin` in interactive mode this is not raw: it blocks on the
 * console and returns line-cooked, echoed bytes (a full line plus `\n`
 * per Enter) — see axl_stream_init(). Use axl_console_read_key()
 * for raw keystrokes.
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
