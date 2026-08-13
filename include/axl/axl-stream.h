/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream.h
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
 * directly, bypassing the UTF-8→UCS-2 transcode that axl_stdout
 * (and axl_print / axl_fprintf) applies. Use for **non-text** payloads
 * that must survive byte-for-byte (dumping a RAM-disk image, an SPD
 * blob, etc.) — the transcode would corrupt arbitrary bytes.
 *
 * NOTE: axl_stdout itself now pipes and redirects correctly. When
 * stdout is a `|` pipe or a `>`/`>a` redirect, axl_stdout writes its
 * transcoded UCS-2 to the same StdOut handle (the shell wires it for a
 * pipe but does not swap gST->ConOut), so `tool | other`, `tool > f`
 * and `tool >a f` all carry a tool's TEXT output. axl_stdout_raw is
 * therefore only needed for BINARY output, not merely to pipe.
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
 * axl_read on axl_stdin). The CRT0 that `AXL_APP` installs calls it,
 * as does axl_driver_init() for a driver image — most consumers
 * don't call it directly.
 *
 * **It RESETS the five, it does not merely publish them.** They are
 * statically allocated (see axl_fclose()), so every mutable field a
 * caller leaves on one — an encoding, a tee, a buffering mode, a
 * sticky `err`, half-transcoded bytes — otherwise outlives that
 * caller for the life of the image. Each is put back to the ground
 * state axl_fclose() leaves a static in: unbuffered, `AXL_ENC_UTF8`,
 * no tee, not interactive, `eof`/`err` clear. Pending buffered
 * output is **drained**, not dropped.
 *
 * That makes a second call meaningful rather than a no-op: it is the
 * supported way to put the standard streams back after code that
 * configured one of them did not restore it. What it discards is
 * **configuration** — so re-apply any tee or buffering afterwards,
 * and do not call it as a defensive "make sure the streams are up"
 * in the middle of a run. On the first call it changes nothing: the
 * statics start in the ground state already.
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
 *
 * This drains the AXL-side buffer ONLY — it does not invoke the sink's own
 * flush, so closing a file stream is NOT the durability point that
 * axl_fflush is. A caller that must know the bytes reached the volume
 * calls axl_fflush and checks it, then closes.
 *
 * THE FIVE STANDARD STREAMS SURVIVE THIS. axl_stdout, axl_stderr,
 * axl_stdin, axl_stdout_raw and axl_stderr_raw are statically allocated, so
 * closing one drains it and resets it but does NOT destroy it — the pointer
 * stays valid and the stream stays usable. Generic code handed an arbitrary
 * AxlStream can therefore close it unconditionally without special-casing
 * the standard streams first.
 *
 * "Resets it" means back to how axl_stream_init left it: unbuffered,
 * AXL_ENC_UTF8, no tee, not interactive, eof/err clear. Anything you
 * configured on it (axl_stream_set_buffering, _set_encoding, _set_stdout_tee,
 * _set_interactive) is gone afterwards, and must be re-applied — dropping the
 * tee in particular is what stops the stream outliving a tee you close next.
 */
void
axl_fclose(
    AxlStream *s  ///< stream, or NULL
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlStream, axl_fclose)
#endif

/**
 * @brief Read @a size * @a count bytes from a stream. Like C's fread().
 *
 * LOOPS until the request is satisfied, exactly as fread() does. A backend
 * is free to transfer less than asked for (see AxlStreamOps) — a socket or
 * ring buffer routinely does — so a single read is not enough, and this
 * keeps reading until the item count is met, the stream ends, or the backend
 * errors. A caller therefore never has to tell a short backend read apart
 * from end-of-input, because only the latter can shorten the return value.
 *
 * Returns the number of COMPLETE items read, which is less than @a count
 * only at end of input or on error. A partial trailing item is not counted,
 * though its bytes ARE in @a buf — again matching C. To tell the two short
 * cases apart, ask axl_feof() / axl_ferror(); the bytes already read are
 * kept either way.
 *
 * Returns 0 if @a size or @a count is 0, or if `size * count` would
 * overflow — the product is the byte count handed to the backend, and a
 * wrapped one bears no relation to @a buf, so the request is refused rather
 * than serviced at the wrong size.
 *
 * On `axl_stdin` in interactive mode reads are line-cooked and blocking
 * (see axl_read() and axl_stream_init()), not raw keystrokes. Note what the
 * loop means there, and it is the same in C: this blocks until @a count
 * items have been typed. A filter that should act on each line as it arrives
 * wants axl_read() or axl_readline().
 */
size_t
axl_fread(
    void      *buf,    ///< destination buffer
    size_t     size,   ///< item size in bytes
    size_t     count,  ///< number of items
    AxlStream *s       ///< stream
);

/**
 * @brief Write @a size * @a count bytes to a stream. Like C's fwrite().
 *
 * LOOPS until every byte is accepted, for the reason axl_fread() does: a
 * backend may accept less than offered, and a caller cannot act on a short
 * item count it has no way to interpret.
 *
 * Returns the number of COMPLETE items written. On an UNBUFFERED stream
 * (AXL_STREAM_BUF_NONE, the default) two things can shorten it, and
 * axl_ferror() is what tells them apart:
 *
 *   - The backend FAILED (-1). axl_ferror() is true.
 *   - The backend accepted NOTHING (0) — a legal "try again later" per
 *     AxlStreamOps, not an error, so axl_ferror() stays false. The loop
 *     stops at the first such zero rather than retrying: nothing can drain
 *     the sink while this call holds the CPU, so a retry here would only
 *     spin. Retry at your own cadence from the returned item count.
 *
 * A partially written trailing item is not counted, but its accepted bytes
 * have still gone to the sink — as in C, the item count alone cannot express
 * that, so a caller resuming a partial write on a byte-exact sink should use
 * `size == 1`.
 *
 * UNDER LINE / FULL BUFFERING THE RETURN IS NOT A RESUME POINT. A buffered
 * write copies the bytes into the stream's buffer before any sink call, so a
 * stalled sink surfaces as -1 (with axl_ferror() true — the "try again" case
 * above does not arise) while the bytes stay QUEUED for the next flush.
 * Re-sending from the returned item count would duplicate them. On a
 * buffered stream, treat a short return as "ask axl_fflush what happened",
 * not as a byte offset.
 *
 * Returns 0 if @a size or @a count is 0, or if `size * count` would
 * overflow (see axl_fread()).
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
) AXL_CB_NOEXCEPT;

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
 * On a file stream opened for writing this reaches the firmware's real
 * flush primitive, so AXL_OK means the bytes are on the volume — this is
 * the durability point for a caller that must not report success before
 * the data is safe. A stream opened read-only, or one whose sink has no
 * flush of its own (a buffer stream), is a no-op success.
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
 * — axl_fflush drains the buffer AND pushes the sink; axl_fclose drains the
 * buffer and frees it but never calls the sink's flush, so it is not an
 * equivalent. (Nothing closes axl_stdout / axl_stderr for you on the way
 * out, so code that buffers them owns the final axl_fflush.)
 *
 * Under LINE / FULL a successful write returns the byte count **accepted
 * into the buffer**, which does not mean the sink took them — a sink error
 * surfaces at the later axl_fflush (or via axl_ferror), not at the
 * buffered write. Do not treat a buffered write's return as durability,
 * and do not expect axl_fclose to report one: on a file stream the
 * firmware close cannot ( EFI_FILE_PROTOCOL.Close is specified to return
 * only EFI_SUCCESS ). Check axl_fflush.
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
 * **THE FILTER RULE** — read this before setting an encoding on a stream that
 * something else is reading or writing.
 *
 * **A filter that moves opaque bytes THROUGH another stream requires that
 * stream to be at AXL_ENC_UTF8.** Not because the bytes are text, but because
 * any other setting means that stream is transforming them, and a filter's
 * bytes are not characters. Setting an encoding here therefore reaches beyond
 * this stream:
 *
 *   - It **poisons a live axl_text_stream_wrap() wrapper** over @p s — the
 *     source would decode and the wrapper's classifier would then be handed
 *     decoded text. The wrapper detects this and fails its reads; see there.
 *   - It **corrupts a compressing filter** attached to @p s
 *     (axl_compress_writer / axl_compress_reader): DEFLATE bytes are binary,
 *     and a transcode over them is destruction. Those constructors refuse a
 *     @p s that is not at AXL_ENC_UTF8 for that reason.
 *
 * This call cannot see any of that and still returns AXL_OK. Set the encoding
 * before you attach a filter, or not at all — and set it on the stream your
 * own code READS FROM, which for a filter is the filter, not its peer.
 *
 * Setting an encoding on a **text wrapper** is the mirror mistake and is not
 * refused either: it overwrites the verdict the classifier reached at
 * construction, which is the one thing that wrapper exists to produce.
 *
 * On one of the five **standard streams** the setting is additionally
 * long-lived: they are statically allocated, so it outlives the code that
 * made it and, in a resident driver, every later dispatch. Restore it
 * yourself, or call axl_stream_init() to put all five back to ground.
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
 * It does READ from it, though, both here and on every read of the wrapper,
 * so @p src's own sticky flags track what the wrapper did to it: on return
 * axl_feof(src) / axl_ferror(src) reflect the classification read. Whether
 * EOF is reached during classification depends on the source's length against
 * an internal probe window, so do not treat either flag as a promise — a
 * short source is typically already at EOF once wrapped.
 *
 * **@p src must present its bytes undecoded**, which is the default for
 * every stream and what axl_stream_get_encoding() reports unless something
 * set otherwise. Two sources qualify, and anything else is refused with NULL:
 *
 *   - a stream at **AXL_ENC_UTF8**, where axl_read() is the wire read; and
 *   - **another text wrapper**, whose output is UTF-8 by construction
 *     whatever its own classifier settled on. Without this second case,
 *     whether a text stream could be wrapped again would depend on the bytes
 *     in the file underneath it. Over such a source the new wrapper is a
 *     plain PASSTHROUGH — it does not classify, because the bytes it would
 *     be classifying are already decoded and re-classifying them is the
 *     double decode wearing a different hat: decoded UCS-2 whose content
 *     holds U+0000 characters alternates ASCII with NUL bytes, which is
 *     exactly what the headerless sniff below fires on.
 *
 * A source that has had an encoding set on it is already transforming its
 * bytes — decoding them (UCS-2) or destroying them (AXL_ENC_ASCII replaces
 * every high byte with `?`) — and the classifier must see the wire. **One
 * decoder per byte stream**, and wrapping declares that it is the wrapper's.
 * @see axl_stream_set_encoding for the general form of this rule.
 *
 * The refusal is **inert**: @p src is not read, modified, or advanced, and no
 * allocation is made. It applies uniformly, including to an interactive
 * source — the short-circuit below decides how a wrapper behaves, not whether
 * one may exist.
 *
 * The remedy is to lend the source, not to reconfigure it permanently — @p src
 * is not owned here, so leaving a setting changed behind you is a side effect
 * on someone else's object:
 *
 * @code
 * AxlEncoding saved = axl_stream_get_encoding(src);
 * axl_stream_set_encoding(src, AXL_ENC_UTF8);
 * AxlStream *txt = axl_text_stream_wrap(src);
 * // ... read txt ...
 * axl_fclose(txt);                              // BEFORE the restore
 * axl_stream_set_encoding(src, saved);
 * @endcode
 *
 * The close must come first because the rule holds for the wrapper's whole
 * LIFETIME, not only at construction: while a wrapper is alive, setting an
 * encoding on its source makes the wrapper fail. axl_ferror() on the wrapper
 * becomes true, and its reads return -1 once the wrapper's own decoded
 * leftovers — at most 3 bytes held back from a previous call — have drained.
 * The check is live rather than latched, so restoring the source to
 * AXL_ENC_UTF8 makes the wrapper usable again. That -1 is **not
 * distinguishable** from an I/O error, so do not branch on it; it is a
 * programming error reported the only way the stream contract allows.
 *
 * If you want the source's own setting honoured, you do not want a classifier
 * at all: read the source directly. On the SAME stream this function and
 * axl_stream_set_encoding are alternatives, not layers.
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
 * @return wrapper stream (free with axl_fclose), or NULL on OOM, on a NULL or
 *     write-only @p src, or on a @p src that is already decoding (see above).
 *     A refusal is logged at debug level, because the cause is often a
 *     setting made by unrelated code on a shared stream.
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
 * **`axl_stdin` is a process global**, so this can fail for a reason that has
 * nothing to do with the caller: any code in the image may set an encoding on
 * it, and axl_text_stream_wrap() refuses a source that is already decoding
 * (see there). Code that sets an encoding on `axl_stdin` should read
 * `axl_stdin` directly rather than wrap it, and should restore the previous
 * value: `axl_stdin` is a `.data` object, so a setting left behind outlives
 * the code that made it, and in a resident driver that is every later
 * dispatch. Nothing clears it on its own — axl_stream_init() and
 * axl_fclose() both put a static back to its ground state, but neither runs
 * per dispatch, so a driver that wants a clean slate has to ask for one.
 *
 * @return a text-decoding read stream over stdin (free with axl_fclose), or
 *     NULL on allocation failure, if `axl_stdin` is not at AXL_ENC_UTF8, or
 *     before axl_stream_init() has published `axl_stdin` at all.
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
 *
 * @a s must be a stream from axl_bufopen(). Any other stream — a file
 * stream, a text wrapper, a consumer backend from
 * axl_stream_open_custom() — is REFUSED, not reinterpreted: the
 * accessor checks the stream's own vtable, so passing the wrong kind is
 * a NULL return rather than silent garbage. On a refusal @a size is left
 * untouched, exactly as for a NULL @a s.
 *
 * @return the buffer's bytes, or NULL if @a s is NULL or is not a buffer
 *     stream.
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
 *
 * Refuses a non-buffer stream on the same terms as axl_bufdata(): NULL
 * return, @a size untouched, and — because this one WRITES to the
 * context — the stream it refused is left entirely unmodified.
 *
 * @return the buffer's bytes, now owned by the caller, or NULL if @a s is
 *     NULL or is not a buffer stream.
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
 * Sets the sticky error flag (axl_ferror()) if the backend fails, as
 * axl_read() does. Deliberately asymmetric in one respect: reading 0 bytes
 * here does NOT set axl_feof(), because positional I/O neither consults nor
 * moves the stream position — "there was nothing at that offset" says
 * nothing about whether the stream is at its end, and a following axl_read()
 * must not inherit an EOF that this call invented. "Not supported" (no
 * backend `pread`) returns -1 without touching the flag, matching how
 * axl_read() treats a read-less stream.
 *
 * @return bytes read, 0 if nothing is there, -1 on error or if not supported.
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
 * Sets the sticky error flag (axl_ferror()) if the backend fails, as
 * axl_write() does. A short count is not an error and sets nothing.
 * @see axl_pread for the flag rules in full.
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

// ---------------------------------------------------------------------------
// Custom backends (fopencookie-shape)
// ---------------------------------------------------------------------------

/// Version of the AxlStreamOps contract. Bump only if a slot's MEANING
/// changes; purely additive growth is carried by @a struct_size instead.
#define AXL_STREAM_OPS_VERSION  1u

/**
 * @brief Backend operations for axl_stream_open_custom().
 *
 * A backend supplies RAW BYTE MOVEMENT ONLY. Everything AxlStream layers on
 * top -- UTF-8 to wire transcode, LINE/FULL buffering, the stdout tee, the
 * interactive hint, and the sticky eof/err flags -- sits ABOVE these
 * operations and is inherited for free. So a socket, a ring buffer, a
 * compressed file, or a deliberately-failing test double all plug in here as
 * first-class citizens rather than a second tier.
 *
 * Three consequences to know before writing one:
 *
 *   - The bytes an operation sees are always WIRE-side. On write the
 *     transcode has already happened; on read it happens after you return. A
 *     backend never needs to know the stream's encoding.
 *   - When the encoding is AXL_ENC_UTF8 (the default) and buffering is
 *     AXL_STREAM_BUF_NONE (also the default), one axl_write() is one call to
 *     @a write. Neither default is guaranteed: LINE/FULL buffering coalesces,
 *     and a non-UTF-8 encoding issues one call PER CODE POINT. Never assume a
 *     one-to-one mapping.
 *   - @a flush is NOT called before @a close, so a backend holding bytes of
 *     its own must push them from @a close.
 *
 * ZERO-INITIALIZE and fill in only what the backend supports. A NULL slot
 * means "unsupported", but what a caller then observes differs per slot and
 * is worth knowing exactly:
 *
 *   | NULL slot           | what the caller sees                    |
 *   |---------------------|-----------------------------------------|
 *   | read/write          | -1 from axl_read / axl_write            |
 *   | pread/pwrite        | -1 from axl_pread / axl_pwrite          |
 *   | seek                | AXL_ERR from axl_fseek                  |
 *   | tell                | -1 from axl_ftell                       |
 *   | flush               | **AXL_OK** -- nothing to push, not an error |
 *   | close               | no-op                                   |
 *
 * @code
 * AxlStreamOps ops = AXL_STREAM_OPS_INIT;
 * ops.write = my_sink_write;
 * ops.close = my_sink_close;
 * AxlStream *s = axl_stream_open_custom(sink, &ops, "my-sink");
 * @endcode
 *
 * THE RETURN CONTRACT, which every caller of the stream relies on:
 *
 *   - @a read / @a write / @a pread / @a pwrite return the number of bytes
 *     transferred. A count LESS than requested is legal and is NOT an error.
 *     0 from @a read means end of input.
 *   - @a seek and @a flush return 0 on success; @a tell returns the current
 *     offset; all three return -1 on error.
 *   - -1 is the only defined failure value. Every other negative return is
 *     RESERVED and a backend must not produce one today. Dispatch treats all
 *     negatives as errors but propagates the value UNCHANGED, so a future
 *     non-blocking backend can be given a distinct "try again later" code
 *     without it having to be encoded as a lie -- and a caller written today
 *     against `< 0` keeps working.
 *
 * Short transfers are handled ABOVE the backend, so they cost a backend
 * author nothing: axl_fread() and axl_fwrite() loop until the item count is
 * satisfied (a short read only ever shortens their return at real end of
 * input), and a buffered flush retains whatever the sink did not take.
 * Return what you moved and let the layer above do the rest.
 *
 * The struct is COPIED by axl_stream_open_custom(), so it may live on the
 * stack and be reused or freed the moment that call returns.
 *
 * WRITING A FILTER -- a backend whose context is another AxlStream? Move
 * bytes through the peer with the ordinary public axl_read() / axl_write(),
 * and require the peer at AXL_ENC_UTF8, where those ARE the wire calls.
 * There is deliberately no "read below a stream's decode" call: a filter's
 * bytes are not characters, so a peer that is transcoding is a peer that is
 * corrupting them, and the answer is to refuse it rather than to reach past
 * it. axl_text_stream_wrap() and the axl_compress_* filters are built on
 * exactly this and reach past nothing. @see axl_stream_set_encoding
 */
typedef struct {
    /// sizeof(AxlStreamOps) -- set via AXL_STREAM_OPS_INIT.
    uint32_t struct_size;
    /// AXL_STREAM_OPS_VERSION -- set via AXL_STREAM_OPS_INIT.
    uint32_t version;
    /// Read up to @a count bytes. Bytes read, 0 at end of input, -1 on error.
    axl_ssize_t (*read)(void *ctx, void *buf, size_t count) AXL_CB_NOEXCEPT;
    /// Write up to @a count bytes. Bytes written (may be short), -1 on error.
    axl_ssize_t (*write)(void *ctx, const void *buf, size_t count) AXL_CB_NOEXCEPT;
    /// Positional read; must not disturb the sequential position. Optional.
    axl_ssize_t (*pread)(void *ctx, void *buf, size_t count, size_t offset) AXL_CB_NOEXCEPT;
    /// Positional write; must not disturb the sequential position. Optional.
    axl_ssize_t (*pwrite)(void *ctx, const void *buf, size_t count, size_t offset) AXL_CB_NOEXCEPT;
    /// Reposition; @a whence is AXL_SEEK_SET/_CUR/_END. 0 ok, -1 on error. Optional.
    int (*seek)(void *ctx, int64_t offset, int whence) AXL_CB_NOEXCEPT;
    /// Current offset, or -1 if unknown. Optional, and independent of @a seek.
    int64_t (*tell)(void *ctx) AXL_CB_NOEXCEPT;
    /// Push backend-held bytes onward. 0 ok, -1 on error. Optional.
    int (*flush)(void *ctx) AXL_CB_NOEXCEPT;
    /// Release @a ctx. Called exactly once, by axl_fclose(), after the drain.
    void (*close)(void *ctx) AXL_CB_NOEXCEPT;
} AxlStreamOps;

/**
 * Static initializer for AxlStreamOps. Always start from this.
 *
 * Stamps @a struct_size and @a version and NULLs every operation, so each
 * reads as unsupported until assigned. Deliberately POSITIONAL rather than
 * designated: GCC suppresses -Wmissing-field-initializers for designated
 * initializers in C only, so the designated form warns in every C++ standard
 * once a consumer adds -Wextra. Matches AXL_QUEUE_INIT.
 */
#define AXL_STREAM_OPS_INIT                                  \
    { sizeof(AxlStreamOps), AXL_STREAM_OPS_VERSION,          \
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }

/**
 * @brief Build a stream from caller-supplied backend operations.
 *
 * The stream behaves like any other: axl_fprintf, axl_readline, the encoding
 * and buffering setters, and axl_fclose all work on it unchanged.
 *
 * OWNERSHIP. @a ctx belongs to the backend -- axl_fclose() calls
 * `ops->close(ctx)` exactly once, after draining buffered output, and
 * releasing @a ctx is that callback's job. **If this function returns NULL,
 * @a close is NOT called and @a ctx remains the caller's to release.** The
 * AxlStream itself belongs to AXL. @a ops and @a name are both copied.
 *
 * VERSIONING. AXL copies `min(ops->struct_size, sizeof(AxlStreamOps))`, which
 * survives a skew in both directions: an older caller against a newer library
 * leaves the newer slots NULL (merely "unsupported", always safe), and a newer
 * caller against an older library has its extra slots ignored rather than read
 * out of bounds. Note @a struct_size cannot protect the read of itself -- the
 * struct is unconditionally assumed to be at least large enough to contain
 * @a struct_size and @a version, which is inherent to the pattern. A
 * @a struct_size of 0, or smaller than the first published version of this
 * struct, is rejected; so is a @a version this library does not know.
 *
 * At least one of @a read or @a write must be present; a stream that can do
 * neither is a programming error, not a degenerate case worth supporting.
 *
 * @return the stream, or NULL if @a ops is NULL, @a struct_size or @a version
 *     is invalid, neither @a read nor @a write is set, or allocation fails.
 */
AxlStream *
axl_stream_open_custom(
    void               *ctx,   ///< backend state, passed to every operation
    const AxlStreamOps *ops,   ///< operations; copied, caller may free after
    const char         *name   ///< label for diagnostics (copied; NULL OK)
);

/**
 * @brief Recover a custom backend's context from the stream built over it.
 *
 * The half of the custom-backend contract axl_stream_open_custom() leaves out.
 * A backend author who wants a STREAM-KEYED accessor --
 * `my_sink_dropped(AxlStream *s)`, the shape axl_bufdata() has -- needs the
 * context back from a stream they were HANDED, not from a handle they kept.
 * Without this the constructor has to return two things and every caller has
 * to carry both.
 *
 * PROVE WHAT YOU EXPECT. @a ops is not decoration: the context comes back only
 * if @a s was opened with a matching set of operations. Any other stream is a
 * NULL return.
 *
 * @warning There is deliberately no one-argument form, at any spelling. A bare
 *     `axl_stream_ctx(s)` would hand a *file* stream's private context to code
 *     about to cast it to something else -- which is not hypothetical:
 *     axl_bufdata() shipped with exactly that hole, and reinterpreting a
 *     16-byte context as a 32-byte one and zeroing it wrote past the end of
 *     the allocation and took the running image down with no diagnostic.
 *     Inside the library that was fixable afterwards with a vtable check; a
 *     consumer cannot see the struct, so for them there would be no fix.
 *
 * WHY THE OPERATIONS ARE A SUFFICIENT TOKEN. All eight slots are compared,
 * the NULL ones included, so matching SHAPE is never enough -- a stream with a
 * read, a write, a seek and a tell is refused unless they are the same four
 * functions, and a stream carrying an operation your @a ops leaves NULL is
 * refused too. The identifying material is the set of non-NULL addresses, and
 * ONE is enough when it is a function only your backend can name: a single
 * `write` that is `static` in your translation unit makes the set unforgeable
 * from outside it.
 *
 * A match means "this stream was opened with MY operations", which is what an
 * accessor needs before it casts -- **provided you keep one AxlStreamOps per
 * context TYPE.** If one ops block serves two different context layouts, a
 * match cannot tell them apart and the cast is yours to get wrong; give each
 * layout its own operations. A match also does not mean "this is THAT stream":
 * every stream from one backend matches, which is right, since they all carry
 * a context of the same type. Telling individual streams apart is the
 * context's own job. And it is a type-confusion guard, not an access-control
 * boundary -- any code that can NAME your operations can assemble a matching
 * ops block, so keep them file-static if that matters to you.
 *
 * BUILT-IN STREAMS ARE OUT OF REACH for exactly that reason, and it is worth
 * being precise about which reason: every operation the file, buffer, text,
 * compress and console backends use is file-static, so no consumer can name
 * one in an AxlStreamOps. Being built-in is NOT itself the test -- axl_bufopen
 * goes through this same public constructor and is protected by the same
 * file-static rule any consumer gets. There is no spelling of this call that
 * reaches a FileCtx.
 *
 * BUILD THE OPS IN ONE PLACE and pass that one object to both calls.
 *
 * @code
 * // ONE definition, shared by the constructor and every accessor.
 * static AxlStreamOps
 * my_sink_ops(void)
 * {
 *     AxlStreamOps ops = AXL_STREAM_OPS_INIT;
 *
 *     ops.write = my_sink_write;
 *     ops.close = my_sink_close;
 *     return ops;
 * }
 *
 * AxlStream *
 * my_sink_open(MySink *sink)
 * {
 *     AxlStreamOps ops = my_sink_ops();
 *
 *     return axl_stream_open_custom(sink, &ops, "my-sink");
 * }
 *
 * size_t
 * my_sink_dropped(const AxlStream *s)
 * {
 *     AxlStreamOps  ops  = my_sink_ops();
 *     MySink       *sink = (MySink *)axl_stream_ctx(s, &ops);
 *
 *     // NULL is "not one of mine" OR "mine, with a NULL context". Use an
 *     // out-param if your accessor must tell those apart.
 *     return (sink != NULL) ? sink->dropped : 0;
 * }
 * @endcode
 *
 * Spelling the assignments out a second time inside the accessor is the
 * tempting shape and the wrong one. The two copies drift -- someone adds
 * `ops.flush` to the constructor and not to the accessor -- and then the match
 * fails forever, the accessor answers "not mine" forever, and NOTHING reports
 * it: no compiler error, no diagnostic, just a plausible default. Hence one
 * definition, called twice.
 *
 * WHAT IS NOT REACHABLE. This asks about the stream you were handed, not about
 * anything underneath it. A stream that WRAPS yours -- axl_text_stream_wrap()
 * is the public one -- carries the WRAPPER's operations and holds your stream,
 * not your context, in its own ctx, so it is refused. That is correct rather
 * than a limitation: reaching through would hand you a context the wrapper
 * does not own. Recover what you need from the stream you opened, before
 * handing it to a wrapper. There is no unwrap.
 *
 * VERSION SKEW resolves exactly as axl_stream_open_custom() resolves it,
 * because both go through the same normalisation:
 * `min(ops->struct_size, sizeof(AxlStreamOps))` bytes' worth of slots are
 * compared, and any slot this library knows but @a ops is too small to carry
 * must additionally be NULL on the stream -- otherwise an older accessor would
 * match a stream a newer sibling opened with an extra operation. A caller that
 * opens and queries with the same struct always matches itself, on whichever
 * side of a header bump it sits. Read "the same struct" strictly: the same
 * VALUES, not merely the same header version.
 *
 * OWNERSHIP is unchanged. This hands back the same pointer
 * axl_stream_open_custom() was given and transfers nothing; it stays valid
 * until axl_fclose() calls @a close. One consequence worth stating: a backend
 * opened with a NULL context is indistinguishable from a refusal. That costs
 * nothing, because a backend with no context has nothing to recover.
 *
 * @return the context @a s was opened with, or NULL if @a s or @a ops is NULL,
 *     @a ops has an invalid @a struct_size or @a version, @a ops sets neither
 *     @a read nor @a write (refused on the same terms as
 *     axl_stream_open_custom(), so an empty AXL_STREAM_OPS_INIT can never be
 *     used as a probe), the operations do not match, or @a s was opened with a
 *     NULL context.
 */
void *
axl_stream_ctx(
    const AxlStream    *s,    ///< stream to recover the context from
    const AxlStreamOps *ops   ///< operations @a s must have been opened with
);

/**
 * @brief The stream's diagnostic label.
 *
 * Whatever was passed to axl_stream_open_custom(), or the built-in name for a
 * stream AXL constructed. The built-in set is closed, so a test may assert on
 * it exactly: "file", "buffer", "text", "compress", "stdout", "stderr",
 * "stdin", "stdout-raw", "stderr-raw".
 *
 * @return the name, never NULL -- "" when the stream has none.
 */
const char *
axl_stream_name(
    const AxlStream *s  ///< stream (NULL yields "")
);

/**
 * @brief Whether the stream supports sequential reads.
 *
 * Generic code that accepts any AxlStream needs to ask before it calls,
 * rather than discovering -1 and having to guess whether that meant
 * "unsupported" or "failed". These queries also pin properties that were
 * previously only incidentally true -- axl_stdout having no read operation,
 * for instance, is correct by design but nothing promised it, so a test
 * asserting it was resting on an accident.
 *
 * @return true if axl_read() can succeed on this stream.
 */
bool
axl_stream_can_read(
    const AxlStream *s  ///< stream (NULL yields false)
);

/// @return true if axl_write() can succeed on this stream.
bool
axl_stream_can_write(
    const AxlStream *s  ///< stream (NULL yields false)
);

/// @return true if axl_fseek() can succeed. Independent of
///     axl_stream_can_tell -- a ring buffer can report a position it cannot
///     seek to.
bool
axl_stream_can_seek(
    const AxlStream *s  ///< stream (NULL yields false)
);

/// @return true if axl_ftell() can succeed. @see axl_stream_can_seek
bool
axl_stream_can_tell(
    const AxlStream *s  ///< stream (NULL yields false)
);

/// @return true if axl_pread() can succeed on this stream.
bool
axl_stream_can_pread(
    const AxlStream *s  ///< stream (NULL yields false)
);

/// @return true if axl_pwrite() can succeed. Queried separately from
///     axl_stream_can_pread because a read-only mapping has one and not the
///     other.
bool
axl_stream_can_pwrite(
    const AxlStream *s  ///< stream (NULL yields false)
);

/**
 * @brief Make the Nth subsequent BACKEND write fail.
 *
 * A custom backend can already fail on demand, so why a global switch too?
 * Because the two reach different code. A custom backend only helps where the
 * test CONSTRUCTS the stream; a function that opens its own file, or operates
 * on an AxlStream handed to it, is unreachable that way. These hooks work on
 * EVERY stream including the built-ins, which is what makes the error paths of
 * the file backend -- and the handlers above it -- testable at all.
 *
 * The counter ticks once per call into the backend's write operation, NOT
 * once per axl_write(). The two differ in three ways that will otherwise
 * surprise you:
 *
 *   - Under AXL_STREAM_BUF_LINE / _FULL, an axl_write() that only fills the
 *     buffer reaches no backend and consumes no tick; the armed failure fires
 *     at the eventual flush (axl_fflush, axl_fclose, or buffer-full).
 *   - When the encoding is not AXL_ENC_UTF8, the transcode issues one backend
 *     write PER CODE POINT, so one axl_write() of an n-character string
 *     consumes n ticks. This is about axl_stream_set_encoding, NOT about the
 *     console: axl_stdout and axl_stderr are UTF-8 streams whose UCS-2
 *     conversion happens inside their backend, below the injection point, so
 *     a console write costs one tick however long the string is.
 *   - Writes to a stdout/stderr tee never consume a tick; only the primary
 *     sink is injected. axl_pwrite() is a different operation and is never
 *     injected here.
 *
 * If the armed operation never reaches the backend the counter STAYS ARMED
 * and will fire on a later, unrelated write -- disarm with 0 in teardown.
 *
 * Not reentrancy-safe: a callback that writes at raised TPL can consume the
 * armed tick. Arm and observe within one non-reentrant sequence.
 *
 * @code
 * axl_stream_fail_next_write(1);
 * test_check(axl_fprintf(s, "x") < 0, "the sink error is reported");
 * @endcode
 */
void
axl_stream_fail_next_write(
    size_t n  ///< fail the Nth next backend write (1 = next, 0 = disabled)
);

/**
 * @brief Make the Nth subsequent BACKEND read fail.
 *
 * Same counting rule as axl_stream_fail_next_write: per backend read, not per
 * axl_read(). A non-UTF-8 encoding pulls wire bytes in small chunks, so one
 * axl_read() can consume several ticks. axl_pread() is never injected here.
 */
void
axl_stream_fail_next_read(
    size_t n  ///< fail the Nth next backend read (1 = next, 0 = disabled)
);

/**
 * @brief Make the Nth subsequent BACKEND flush fail.
 *
 * Ticks on the backend flush operation only, after the buffered drain. A
 * backend with a NULL flush never reaches the injection point -- axl_fflush()
 * returns AXL_OK without calling anything -- so the counter stays armed.
 */
void
axl_stream_fail_next_flush(
    size_t n  ///< fail the Nth next backend flush (1 = next, 0 = disabled)
);

/**
 * @brief Make the Nth subsequent BACKEND write accept only @a limit bytes.
 *
 * A SHORT write, not a failed one -- the distinction consumers must handle,
 * and the one with no AxlMem analogue, because a partial allocation is not a
 * thing. Layers above AxlStream read a short count as "the sink is full,
 * bytes were dropped" and a -1 as "the sink is broken"; those are different
 * recoveries, and without this hook the short branch cannot be reached from a
 * test at all.
 *
 * @a limit is clamped against the backend call's own count, and may be 0 --
 * a legal "accepted nothing, try again" result, distinct from an error, and
 * the one that drives the buffered path's retain-the-tail branch.
 *
 * Same counting rule and reentrancy caveat as axl_stream_fail_next_write.
 */
void
axl_stream_short_next_write(
    size_t n,      ///< short the Nth next backend write (1 = next, 0 = disabled)
    size_t limit   ///< bytes to accept from that write
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_STREAM_H */
