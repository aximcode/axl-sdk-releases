/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream.c
    Core stream operations, console backend, and printf family.
**/

#include <stddef.h>
#include <stdarg.h>
#include "../backend/axl-backend.h"
#include <axl/axl-mem.h>
#include <axl/axl-string.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include "axl-stream-internal.h"
#include <axl/axl-format.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("io");

typedef struct {
    AxlStream  *stream;
    int        count;
    int        error;
} FprintfCtx;

// ---------------------------------------------------------------------------
// Stream allocation
// ---------------------------------------------------------------------------

AxlStream *
axl_stream_new(void)
{
    return axl_new(AxlStream);
}

// ---------------------------------------------------------------------------
// Console backend
// ---------------------------------------------------------------------------

/**
 * Expand bare \n to \r\n in a UCS-2 string for UEFI ConOut.
 * Returns a new allocation, or NULL on failure.
 */
static unsigned short *
console_expand_newlines(const unsigned short *src)
{
    size_t extra = 0;
    size_t src_len = 0;
    for (size_t i = 0; src[i] != 0; i++) {
        if (src[i] == '\n' && (i == 0 || src[i - 1] != '\r')) {
            extra++;
        }
        src_len++;
    }

    if (extra == 0) {
        return NULL;
    }
    unsigned short *out = axl_malloc((src_len + extra + 1) * sizeof(unsigned short));
    if (out == NULL) {
        axl_warning(
            "console_expand_newlines: OOM allocating %zu UCS-2 chars",
            src_len + extra + 1
            );
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; src[i] != 0; i++) {
        if (src[i] == '\n' && (i == 0 || src[i - 1] != '\r')) {
            out[j++] = '\r';
        }
        out[j++] = src[i];
    }
    out[j] = 0;
    return out;
}

static axl_ssize_t
console_write(void *ctx, const void *data, size_t count)
{
    unsigned short *wide;
    char *tmp;

    (void)ctx;

    if (count == 0) {
        return 0;
    }

    /* Need NUL-terminated string for axl_utf8_to_ucs2 */
    tmp = (char *)axl_malloc(count + 1);
    if (tmp == NULL) {
        axl_warning(
            "console_write: OOM allocating %zu-byte temp buffer",
            count + 1
            );
        return -1;
    }
    axl_memcpy(tmp, data, count);
    tmp[count] = '\0';

    wide = axl_utf8_to_ucs2(tmp);
    axl_free(tmp);
    if (wide == NULL) {
        return -1;
    }

    /* UEFI ConOut expects \r\n — expand bare \n */
    unsigned short *expanded = console_expand_newlines(wide);
    if (expanded != NULL) {
        axl_free(wide);
        wide = expanded;
    }

    axl_backend_console_write(wide);
    axl_free(wide);
    return (axl_ssize_t)count;
}

static AxlStream mStdout = {
    .ctx    = NULL,
    .read   = NULL,
    .write  = console_write,
    .pread  = NULL,
    .pwrite = NULL,
    .close  = NULL,
};

static AxlStream mStderr = {
    .ctx    = NULL,
    .read   = NULL,
    .write  = console_write,
    .pread  = NULL,
    .pwrite = NULL,
    .close  = NULL,
};

/**
 * Standard-input read. Backed by EFI_SHELL_PARAMETERS_PROTOCOL.StdIn
 * — for `tool1 | tool2` invocations that's the captured LHS output;
 * for non-redirected launches it may be the keyboard or simply NULL.
 *
 * Returns the byte count actually read (may be < count on EOF or
 * line-buffered console modes), or -1 on error. A NULL stdin handle
 * is treated as "no input connected" and surfaces as 0 (EOF).
 */
static axl_ssize_t
console_read(void *ctx, void *buf, size_t count)
{
    (void)ctx;
    if (buf == NULL || count == 0) {
        return 0;
    }
    AxlFileHandle h = axl_backend_shell_stdin();
    if (h == NULL) {
        /* No shell-params published — no piped input available.
           Surface as EOF rather than blocking on a keyboard the
           caller probably didn't expect. */
        return 0;
    }
    size_t n = count;
    if (axl_backend_file_read(h, &n, buf) != 0) {
        return -1;
    }
    return (axl_ssize_t)n;
}

static AxlStream mStdin = {
    .ctx    = NULL,
    .read   = console_read,
    .write  = NULL,
    .pread  = NULL,
    .pwrite = NULL,
    .close  = NULL,
};

/**
 * Raw-bytes write to the shell's StdOut handle, bypassing the
 * UTF-8→UCS-2 console_write conversion. For binary output (a tool
 * dumping a RAM-disk image, captured SPD blob, etc.) where the
 * caller wants bytes to traverse a pipe intact.
 *
 * Returns -1 if the shell-params protocol isn't published — there's
 * no sensible fallback for binary bytes (the firmware console
 * would mangle them via CHAR16 conversion, which is exactly what
 * this path exists to avoid).
 */
static axl_ssize_t
console_write_raw(void *ctx, const void *data, size_t count)
{
    (void)ctx;
    if (data == NULL || count == 0) {
        return 0;
    }
    AxlFileHandle h = axl_backend_shell_stdout();
    if (h == NULL) {
        return -1;
    }
    size_t n = count;
    if (axl_backend_file_write(h, &n, data) != 0) {
        return -1;
    }
    return (axl_ssize_t)n;
}

static AxlStream mStdoutRaw = {
    .ctx    = NULL,
    .read   = NULL,
    .write  = console_write_raw,
    .pread  = NULL,
    .pwrite = NULL,
    .close  = NULL,
};

AxlStream *axl_stdout     = NULL;
AxlStream *axl_stderr     = NULL;
AxlStream *axl_stdin      = NULL;
AxlStream *axl_stdout_raw = NULL;

void
axl_stream_init(void)
{
    axl_stdout     = &mStdout;
    axl_stderr     = &mStderr;
    axl_stdin      = &mStdin;
    axl_stdout_raw = &mStdoutRaw;
}

// ---------------------------------------------------------------------------
// Per-stream encoding (transcoders for the byte-I/O primitives)
// ---------------------------------------------------------------------------

int
axl_stream_set_encoding(AxlStream *s, AxlEncoding enc)
{
    if (s == NULL) {
        return -1;
    }
    if (enc != AXL_ENC_UTF8 && enc != AXL_ENC_UCS2_LE
        && enc != AXL_ENC_UCS2_BE && enc != AXL_ENC_ASCII) {
        return -1;
    }
    /* Reset transcode buffers — pending bytes describe partial state
       that is no longer meaningful under the new encoding, and the
       passthrough path doesn't drain them, so leaving them would
       silently orphan caller bytes (or replay stale ones). */
    s->in_pending_n  = 0;
    s->out_pending_n = 0;
    s->encoding      = enc;
    return 0;
}

AxlEncoding
axl_stream_get_encoding(AxlStream *s)
{
    if (s == NULL) {
        return AXL_ENC_UTF8;
    }
    return s->encoding;
}

/* Encode a BMP code point (0..0xFFFF) as UTF-8 into @p out. Surrogate
   halves transcode as their 3-byte BMP shape (intentionally
   permissive — see header doc). Returns 1, 2, or 3. */
static size_t
encode_utf8_bmp(uint16_t code, uint8_t out[3])
{
    if (code < 0x80) {
        out[0] = (uint8_t)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (uint8_t)(0xC0 | (code >> 6));
        out[1] = (uint8_t)(0x80 | (code & 0x3F));
        return 2;
    }
    out[0] = (uint8_t)(0xE0 | (code >> 12));
    out[1] = (uint8_t)(0x80 | ((code >> 6) & 0x3F));
    out[2] = (uint8_t)(0x80 |  (code        & 0x3F));
    return 3;
}

/* Pull exactly @p n wire bytes from the backend, looping on short
   reads. Returns the count obtained (may be < n on EOF). On backend
   error returns -1 with no bytes copied. */
static axl_ssize_t
pull_wire(AxlStream *s, uint8_t *out, size_t n)
{
    size_t got = 0;
    while (got < n) {
        axl_ssize_t r = s->read(s->ctx, out + got, n - got);
        if (r < 0) return -1;
        if (r == 0) break;
        got += (size_t)r;
    }
    return (axl_ssize_t)got;
}

/* Drain the in_pending UTF-8 leftover into the caller's buffer.
   Returns bytes emitted. */
static size_t
drain_in_pending(AxlStream *s, uint8_t *out, size_t cap)
{
    size_t emitted = 0;
    while (s->in_pending_n > 0 && emitted < cap) {
        out[emitted++] = s->in_pending[0];
        for (size_t i = 0; i + 1 < s->in_pending_n; i++) {
            s->in_pending[i] = s->in_pending[i + 1];
        }
        s->in_pending_n--;
    }
    return emitted;
}

/* Read with transcoding (wire encoding → UTF-8). */
static axl_ssize_t
read_transcode(AxlStream *s, void *buf, size_t count)
{
    uint8_t *out     = (uint8_t *)buf;
    size_t   emitted = 0;

    /* Always drain leftover UTF-8 first. */
    emitted += drain_in_pending(s, out + emitted, count - emitted);
    if (emitted >= count) {
        return (axl_ssize_t)emitted;
    }

    if (s->encoding == AXL_ENC_ASCII) {
        while (emitted < count) {
            uint8_t      b;
            axl_ssize_t  got = pull_wire(s, &b, 1);
            if (got < 0) {
                s->err = true;
                return emitted ? (axl_ssize_t)emitted : -1;
            }
            if (got == 0) break;       /* EOF */
            out[emitted++] = (b < 0x80) ? b : '?';
        }
        return (axl_ssize_t)emitted;
    }

    /* UCS-2 LE/BE → UTF-8 BMP transcode. */
    while (emitted < count) {
        uint8_t      pair[2];
        axl_ssize_t  got = pull_wire(s, pair, 2);
        if (got < 0) {
            s->err = true;
            return emitted ? (axl_ssize_t)emitted : -1;
        }
        if (got < 2) {
            if (got == 1) {
                axl_debug("orphan UCS-2 byte at end of stream");
            }
            break;                     /* EOF */
        }
        uint16_t code = (s->encoding == AXL_ENC_UCS2_LE)
                      ? (uint16_t)(pair[0] | ((uint16_t)pair[1] << 8))
                      : (uint16_t)(((uint16_t)pair[0] << 8) | pair[1]);

        uint8_t enc[3];
        size_t  enc_n = encode_utf8_bmp(code, enc);

        size_t  fits  = (count - emitted < enc_n) ? (count - emitted) : enc_n;
        for (size_t i = 0; i < fits; i++) {
            out[emitted++] = enc[i];
        }
        for (size_t i = fits; i < enc_n; i++) {
            s->in_pending[s->in_pending_n++] = enc[i];
        }
    }
    return (axl_ssize_t)emitted;
}

/* How many UTF-8 bytes a given lead implies (returns 1..4). */
static size_t
utf8_lead_len(uint8_t lead)
{
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;   /* invalid lead → Latin-1 fallback */
}

/* Emit one wire char for @p code in the stream's encoding. Sets
   s->err and returns -1 on backend write error. */
static int
emit_wire(AxlStream *s, uint32_t code)
{
    if (s->encoding == AXL_ENC_ASCII) {
        uint8_t b = (code < 0x80) ? (uint8_t)code : (uint8_t)'?';
        if (s->write(s->ctx, &b, 1) < 0) {
            s->err = true;
            return -1;
        }
        return 0;
    }

    /* UCS-2: codepoints above U+FFFF replaced with '?' (no surrogate
       pair encoding — UCS-2 is BMP-only by design). */
    uint16_t cu = (code <= 0xFFFFu) ? (uint16_t)code : (uint16_t)'?';
    uint8_t  wire[2];
    if (s->encoding == AXL_ENC_UCS2_LE) {
        wire[0] = (uint8_t)(cu & 0xFFu);
        wire[1] = (uint8_t)(cu >> 8);
    } else {
        wire[0] = (uint8_t)(cu >> 8);
        wire[1] = (uint8_t)(cu & 0xFFu);
    }
    if (s->write(s->ctx, wire, 2) < 0) {
        s->err = true;
        return -1;
    }
    return 0;
}

/* Write with transcoding (UTF-8 → wire encoding). Returns the count
   of input bytes accepted (== @p count if no backend error; bytes
   may be flushed to the wire OR carried as out_pending for the next
   call). On backend error returns the partial count, or -1 if no
   input bytes were accepted. */
static axl_ssize_t
write_transcode(AxlStream *s, const void *buf, size_t count)
{
    const uint8_t *in = (const uint8_t *)buf;
    size_t         i  = 0;

    while (i < count) {
        /* Lead byte comes from out_pending if any, else from input. */
        uint8_t lead = (s->out_pending_n > 0) ? s->out_pending[0] : in[i];
        size_t  needed = utf8_lead_len(lead);

        /* Gather the sequence: pending first, then input. */
        uint8_t seq[4];
        size_t  seq_n        = 0;
        size_t  from_pending = 0;
        size_t  from_input   = 0;
        while (from_pending < s->out_pending_n && seq_n < needed) {
            seq[seq_n++] = s->out_pending[from_pending++];
        }
        while (i + from_input < count && seq_n < needed) {
            seq[seq_n++] = in[i + from_input++];
        }

        if (seq_n < needed) {
            /* Not enough bytes yet — buffer everything we have and
               wait for the next call. The from_pending bytes were
               already in out_pending (no shift needed since they
               cover exactly the front), so we just append the
               from_input bytes after them. */
            for (size_t k = 0; k < from_input; k++) {
                s->out_pending[s->out_pending_n++] = in[i + k];
            }
            i += from_input;
            return (axl_ssize_t)i;
        }

        /* Validate continuation bytes for multi-byte sequences. If
           any continuation is bad, fall back to Latin-1 on the lead
           byte alone and rescan from the next byte. */
        bool latin1 = (needed == 1 && lead >= 0x80);
        if (!latin1 && needed > 1) {
            for (size_t k = 1; k < needed; k++) {
                if ((seq[k] & 0xC0) != 0x80) {
                    latin1 = true;
                    break;
                }
            }
        }

        size_t consume_n;       /* bytes of seq we'll account for */
        uint32_t code;
        if (latin1) {
            consume_n = 1;
            code      = lead;
        } else {
            consume_n = needed;
            if (needed == 1) {
                code = seq[0];
            } else if (needed == 2) {
                code = ((uint32_t)(seq[0] & 0x1F) << 6)
                     |  (uint32_t)(seq[1] & 0x3F);
            } else if (needed == 3) {
                code = ((uint32_t)(seq[0] & 0x0F) << 12)
                     | ((uint32_t)(seq[1] & 0x3F) << 6)
                     |  (uint32_t)(seq[2] & 0x3F);
            } else {
                code = ((uint32_t)(seq[0] & 0x07) << 18)
                     | ((uint32_t)(seq[1] & 0x3F) << 12)
                     | ((uint32_t)(seq[2] & 0x3F) << 6)
                     |  (uint32_t)(seq[3] & 0x3F);
            }
        }

        if (emit_wire(s, code) < 0) {
            return i ? (axl_ssize_t)i : -1;
        }

        /* Account for consume_n bytes split across pending/input.
           Pending always comes first. */
        size_t pending_used = (consume_n < from_pending) ? consume_n : from_pending;
        size_t input_used   = consume_n - pending_used;
        if (pending_used > 0) {
            for (size_t k = 0; k + pending_used < s->out_pending_n; k++) {
                s->out_pending[k] = s->out_pending[k + pending_used];
            }
            s->out_pending_n -= pending_used;
        }
        i += input_used;
    }

    return (axl_ssize_t)i;
}

// ---------------------------------------------------------------------------
// Layer 3: Low-level read/write/pread/pwrite
// ---------------------------------------------------------------------------

axl_ssize_t
axl_read(AxlStream *s, void *buf, size_t count)
{
    axl_ssize_t n;

    if (s == NULL || s->read == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (s->encoding != AXL_ENC_UTF8) {
        n = read_transcode(s, buf, count);
    } else {
        n = s->read(s->ctx, buf, count);
    }
    if (n == 0) {
        s->eof = true;
    } else if (n < 0) {
        s->err = true;
    }
    return n;
}

axl_ssize_t
axl_write(AxlStream *s, const void *buf, size_t count)
{
    axl_ssize_t n;

    if (s == NULL || s->write == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (s->encoding != AXL_ENC_UTF8) {
        return write_transcode(s, buf, count);
    }
    n = s->write(s->ctx, buf, count);
    if (n < 0) {
        s->err = true;
    }
    return n;
}

axl_ssize_t
axl_pread(AxlStream *s, void *buf, size_t count, size_t offset)
{
    if (s == NULL || s->pread == NULL) {
        return -1;
    }
    return s->pread(s->ctx, buf, count, offset);
}

axl_ssize_t
axl_pwrite(AxlStream *s, const void *buf, size_t count, size_t offset)
{
    if (s == NULL || s->pwrite == NULL) {
        return -1;
    }
    return s->pwrite(s->ctx, buf, count, offset);
}

// ---------------------------------------------------------------------------
// Layer 2: fread/fwrite/fclose/fprintf/readline
// ---------------------------------------------------------------------------

size_t
axl_fread(void *buf, size_t size, size_t count, AxlStream *s)
{
    axl_ssize_t n;

    if (size == 0 || count == 0) {
        return 0;
    }

    n = axl_read(s, buf, size * count);
    if (n <= 0) {
        return 0;
    }
    return (size_t)n / size;
}

size_t
axl_fwrite(const void *buf, size_t size, size_t count, AxlStream *s)
{
    axl_ssize_t n;

    if (size == 0 || count == 0) {
        return 0;
    }

    n = axl_write(s, buf, size * count);
    if (n <= 0) {
        return 0;
    }
    return (size_t)n / size;
}

void
axl_fclose(AxlStream *s)
{
    if (s == NULL) {
        return;
    }
    if (s->close != NULL) {
        s->close(s->ctx);
    }
    axl_free(s);
}

char *
axl_readline(AxlStream *s)
{
    /* Unbounded — delegates to the bounded helper with SIZE_MAX so
       a single allocation strategy lives in one place. */
    return axl_readline_max(s, (size_t)-1);
}

// ---------------------------------------------------------------------------
// AxlLineReader — chunked line iterator. Stateful so callers can use
// normal control flow (while + break) instead of callback indirection.
// axl_walk_lines is now a thin wrapper around this.
// ---------------------------------------------------------------------------

void
axl_line_reader_init(
    AxlLineReader *r,
    AxlStream     *s,
    char          *buf,
    size_t         buf_size
    )
{
    if (r == NULL) return;
    r->_stream         = s;
    r->_buf            = buf;
    r->_buf_size       = buf_size;
    r->_fill           = 0;
    r->_last_consumed  = 0;
    r->_discard        = false;
    r->_eof            = false;
    r->_err            = false;
}

bool
axl_line_reader_error(const AxlLineReader *r)
{
    return r != NULL && r->_err;
}

bool
axl_line_reader_next(
    AxlLineReader  *r,
    const char    **line,
    size_t         *len,
    bool           *truncated
    )
{
    if (r == NULL || line == NULL || len == NULL
        || r->_stream == NULL || r->_buf == NULL || r->_buf_size < 2) {
        return false;
    }

    /* Shift out the slice returned on the previous call so its
       bytes don't reappear. */
    if (r->_last_consumed > 0) {
        if (r->_last_consumed < r->_fill) {
            axl_memmove(r->_buf, r->_buf + r->_last_consumed,
                        r->_fill - r->_last_consumed);
            r->_fill -= r->_last_consumed;
        } else {
            r->_fill = 0;
        }
        r->_last_consumed = 0;
    }

    for (;;) {
        /* Discard mode: drain bytes up to (and including) the next
           '\n'. After draining, fall through to the line scan. */
        if (r->_discard) {
            size_t i = 0;
            while (i < r->_fill && r->_buf[i] != '\n') i++;
            if (i < r->_fill) {
                size_t consumed = i + 1;
                if (consumed < r->_fill) {
                    axl_memmove(r->_buf, r->_buf + consumed,
                                r->_fill - consumed);
                    r->_fill -= consumed;
                } else {
                    r->_fill = 0;
                }
                r->_discard = false;
            } else {
                r->_fill = 0;  /* still inside long line — refill */
            }
        }

        if (!r->_discard) {
            /* Look for '\n' in the current fill. */
            size_t i = 0;
            while (i < r->_fill && r->_buf[i] != '\n') i++;
            if (i < r->_fill) {
                *line = r->_buf;
                *len  = i;
                if (truncated) *truncated = false;
                r->_last_consumed = i + 1;  /* shift on next call */
                return true;
            }
            if (r->_fill == r->_buf_size) {
                /* Buffer full, no '\n' — emit prefix, enter discard
                   mode for the rest of this logical line. */
                *line = r->_buf;
                *len  = r->_fill;
                if (truncated) *truncated = true;
                r->_last_consumed = r->_fill;
                r->_discard = true;
                return true;
            }
        }

        if (r->_eof || r->_err) {
            /* Upstream done — flush any tail not terminated by '\n'. */
            if (!r->_discard && r->_fill > 0) {
                *line = r->_buf;
                *len  = r->_fill;
                if (truncated) *truncated = false;
                r->_last_consumed = r->_fill;
                return true;
            }
            return false;
        }

        /* Refill. */
        axl_ssize_t got = axl_read(r->_stream, r->_buf + r->_fill,
                                   r->_buf_size - r->_fill);
        if (got < 0) {
            r->_err = true;
            continue;  /* re-evaluate as a "no more data" iteration */
        }
        if (got == 0) {
            r->_eof = true;
            continue;
        }
        r->_fill += (size_t)got;
    }
}

int
axl_walk_lines(
    AxlStream  *s,
    char       *buf,
    size_t      buf_size,
    AxlLineFn   fn,
    void       *user
    )
{
    if (s == NULL || buf == NULL || buf_size < 2 || fn == NULL) {
        return -1;
    }

    AxlLineReader r;
    axl_line_reader_init(&r, s, buf, buf_size);

    const char *line;
    size_t      len;
    bool        truncated;
    while (axl_line_reader_next(&r, &line, &len, &truncated)) {
        int rc = fn(line, len, truncated, user);
        if (rc != 0) return rc;
    }
    return axl_line_reader_error(&r) ? -1 : 0;
}

char *
axl_readline_max(AxlStream *s, size_t max_bytes)
{
    AxlString *b;
    char c;
    axl_ssize_t n;

    if (s == NULL || s->read == NULL || max_bytes < 2) {
        /* max_bytes must hold at least one payload byte plus the
           trailing NUL — anything smaller is a programmer error. */
        return NULL;
    }

    b = axl_string_new(NULL);
    if (b == NULL) {
        return NULL;
    }

    /* Cap on the payload (excluding the implicit trailing NUL). */
    const size_t payload_cap = max_bytes - 1;

    for (;;) {
        n = axl_read(s, &c, 1);
        if (n <= 0) {
            /* EOF or error — axl_read sets eof/err sticky bits. */
            if (axl_string_len(b) == 0) {
                axl_string_free(b);
                return NULL;
            }
            break;
        }

        if (axl_string_len(b) >= payload_cap) {
            /* Cap hit before a '\n'. Drain the rest of this logical
               line from the stream so the next axl_readline_max call
               starts at the next line — line counting stays correct
               and the heap doesn't grow further. */
            if (c != '\n') {
                while (axl_read(s, &c, 1) > 0 && c != '\n') {
                    /* discard */
                }
            }
            break;
        }

        axl_string_append_c(b, c);
        if (c == '\n') {
            break;
        }
    }

    char *line = axl_string_steal(b);
    axl_string_free(b);
    return line;
}

char *
axl_fgets(char *buf, int size, AxlStream *s)
{
    if (buf == NULL || size <= 1 || s == NULL || s->read == NULL) {
        return NULL;
    }

    int total = 0;
    while (total < size - 1) {
        char        c;
        axl_ssize_t n = axl_read(s, &c, 1);
        if (n < 0) {
            /* err sticky bit set by axl_read */
            if (total == 0) return NULL;
            break;
        }
        if (n == 0) {
            /* EOF */
            if (total == 0) return NULL;
            break;
        }
        buf[total++] = c;
        if (c == '\n') {
            break;
        }
    }
    buf[total] = '\0';
    return buf;
}

bool
axl_ferror(AxlStream *s)
{
    if (s == NULL) {
        return false;
    }
    return s->err;
}

void
axl_clearerr(AxlStream *s)
{
    if (s == NULL) {
        return;
    }
    s->eof = false;
    s->err = false;
}

// ---------------------------------------------------------------------------
// Stream positioning
// ---------------------------------------------------------------------------

int
axl_fseek(AxlStream *s, int64_t offset, int whence)
{
    if (s == NULL || s->seek == NULL) {
        return -1;
    }
    /* Discard buffered transcode state — those bytes describe a
       partial sequence at the OLD position; splicing them onto the
       post-seek byte stream would corrupt the next read/write. */
    s->in_pending_n  = 0;
    s->out_pending_n = 0;
    s->eof = false;
    return s->seek(s->ctx, offset, whence);
}

int64_t
axl_ftell(AxlStream *s)
{
    if (s == NULL || s->tell == NULL) {
        return -1;
    }
    return s->tell(s->ctx);
}

bool
axl_feof(AxlStream *s)
{
    if (s == NULL) {
        return true;
    }
    return s->eof;
}

int
axl_fflush(AxlStream *s)
{
    if (s == NULL) {
        return 0;
    }
    if (s->flush == NULL) {
        return 0;
    }
    return s->flush(s->ctx);
}

// ---------------------------------------------------------------------------
// Printf family
// ---------------------------------------------------------------------------

static void
fprintf_write(const char *data, size_t len, void *ctx)
{
    FprintfCtx *fc = (FprintfCtx *)ctx;
    axl_ssize_t n;

    if (fc->error) {
        return;
    }

    n = axl_write(fc->stream, data, len);
    if (n < 0) {
        fc->error = 1;
    } else {
        fc->count += (int)n;
    }
}

int
axl_vfprintf(AxlStream *s, const char *fmt, va_list ap)
{
    FprintfCtx fc;

    if (s == NULL || fmt == NULL) {
        return -1;
    }

    fc.stream = s;
    fc.count  = 0;
    fc.error  = 0;

    axl_vformat(fprintf_write, &fc, fmt, ap);

    return fc.error ? -1 : fc.count;
}

int
axl_fprintf(AxlStream *s, const char *fmt, ...)
{
    va_list args;
    int     ret;

    va_start(args, fmt);
    ret = axl_vfprintf(s, fmt, args);
    va_end(args);
    return ret;
}

int
axl_print(const char *fmt, ...)
{
    va_list args;
    FprintfCtx fc;

    if (axl_stdout == NULL || fmt == NULL) {
        return -1;
    }

    fc.stream = axl_stdout;
    fc.count  = 0;
    fc.error  = 0;

    va_start(args, fmt);
    axl_vformat(fprintf_write, &fc, fmt, args);
    va_end(args);

    return fc.error ? -1 : fc.count;
}

int
axl_printerr(const char *fmt, ...)
{
    va_list args;
    FprintfCtx fc;

    if (axl_stderr == NULL || fmt == NULL) {
        return -1;
    }

    fc.stream = axl_stderr;
    fc.count  = 0;
    fc.error  = 0;

    va_start(args, fmt);
    axl_vformat(fprintf_write, &fc, fmt, args);
    va_end(args);

    return fc.error ? -1 : fc.count;
}

/* axl_fopen lives in axl-stream-file.c (the file backend).
   axl_bufopen lives in axl-stream-buf.c (the buffer backend).
   axl_file_get_contents / axl_file_set_contents live in
   src/fs/axl-fs.c (filesystem operations). */
