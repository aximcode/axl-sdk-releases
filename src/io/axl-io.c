/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-io.c
    Core stream operations, console backend, and printf family.
**/

#include <stddef.h>
#include <stdarg.h>
#include "../backend/axl-backend.h"
#include <axl/axl-mem.h>
#include <axl/axl-string.h>
#include <axl/axl-str.h>
#include <axl/axl-io.h>
#include "axl-io-internal.h"
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

AxlStream *axl_stdout = NULL;
AxlStream *axl_stderr = NULL;

void
axl_io_init(void)
{
    axl_stdout = &mStdout;
    axl_stderr = &mStderr;
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
    n = s->read(s->ctx, buf, count);
    if (n == 0) {
        s->eof = true;
    }
    return n;
}

axl_ssize_t
axl_write(AxlStream *s, const void *buf, size_t count)
{
    if (s == NULL || s->write == NULL) {
        return -1;
    }
    return s->write(s->ctx, buf, count);
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
    AxlString *b;
    char c;
    axl_ssize_t n;

    if (s == NULL || s->read == NULL) {
        return NULL;
    }

    b = axl_string_new(NULL);
    if (b == NULL) {
        return NULL;
    }

    for (;;) {
        n = s->read(s->ctx, &c, 1);
        if (n <= 0) {
            /* EOF or error */
            if (axl_string_len(b) == 0) {
                axl_string_free(b);
                return NULL;
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

// ---------------------------------------------------------------------------
// Stream positioning
// ---------------------------------------------------------------------------

int
axl_fseek(AxlStream *s, int64_t offset, int whence)
{
    if (s == NULL || s->seek == NULL) {
        return -1;
    }
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
axl_fprintf(AxlStream *s, const char *fmt, ...)
{
    va_list args;
    FprintfCtx fc;

    if (s == NULL || fmt == NULL) {
        return -1;
    }

    fc.stream = s;
    fc.count  = 0;
    fc.error  = 0;

    va_start(args, fmt);
    axl_vformat(fprintf_write, &fc, fmt, args);
    va_end(args);

    return fc.error ? -1 : fc.count;
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

// ---------------------------------------------------------------------------
// Layer 1: file helpers (delegate to AxlIOFile.c)
// ---------------------------------------------------------------------------

int
axl_file_get_contents(const char *path, void **buf, size_t *len)
{
    return axl_file_get_contents_internal(path, buf, len);
}

int
axl_file_set_contents(const char *path, const void *buf, size_t len)
{
    return axl_file_set_contents_internal(path, buf, len);
}

// ---------------------------------------------------------------------------
// Stream constructors (delegate to backends)
// ---------------------------------------------------------------------------

AxlStream *
axl_fopen(const char *path, const char *mode)
{
    return axl_fopen_internal(path, mode);
}

AxlStream *
axl_bufopen(void)
{
    return axl_bufopen_internal();
}
