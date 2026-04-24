/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-string.c
    Mutable auto-growing string builder(GLib GString equivalent).
**/

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-format.h>
AXL_LOG_DOMAIN("str");

#define DEFAULT_CAPACITY  64

// ---------------------------------------------------------------------------
// Internal struct
// ---------------------------------------------------------------------------

struct AxlString {
    char    *buf;
    size_t   len;
    size_t   alloc;
    bool     error;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static
bool
grow(
    AxlString  *b,
    size_t      need
    )
{
    size_t  new_alloc;
    char   *new_buf;

    if (b->len + need + 1 <= b->alloc) {
        return true;
    }

    new_alloc = b->alloc;
    while (new_alloc < b->len + need + 1) {
        new_alloc *= 2;
    }

    new_buf = (char *)axl_realloc(b->buf, new_alloc);
    if (new_buf == NULL) {
        axl_warning("strbuf grow failed");
        return false;
    }

    b->buf   = new_buf;
    b->alloc = new_alloc;
    return true;
}

static void
strbuf_write(const char *data, size_t len, void *ctx)
{
    AxlString *b = (AxlString *)ctx;

    if (!grow(b, len)) {
        b->error = true;
        return;
    }

    axl_memcpy(b->buf + b->len, data, len);
    b->len += len;
    b->buf[b->len] = '\0';
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlString *
axl_string_new(const char *init)
{
    AxlString *b = axl_string_new_size(DEFAULT_CAPACITY);
    if (b != NULL && init != NULL) {
        strbuf_write(init, axl_strlen(init), b);
    }
    return b;
}

AxlString *
axl_string_new_size(size_t reserve)
{
    AxlString  *b;

    if (reserve < 16) {
        reserve = 16;
    }

    b = axl_new(AxlString);
    if (b == NULL) {
        axl_warning("strbuf allocation failed");
        return NULL;
    }

    b->buf = (char *)axl_malloc(reserve);
    if (b->buf == NULL) {
        axl_warning("strbuf buffer allocation failed");
        axl_free(b);
        return NULL;
    }

    b->buf[0] = '\0';
    b->len    = 0;
    b->alloc  = reserve;

    return b;
}

int
axl_string_append(AxlString *b, const char *s)
{
    size_t  len;

    if (b == NULL || s == NULL) {
        return 0;
    }

    len = axl_strlen(s);
    if (len == 0) {
        return 0;
    }

    if (!grow(b, len)) {
        return -1;
    }

    axl_memcpy(b->buf + b->len, s, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return 0;
}

int
axl_string_append_len(AxlString *b, const char *data, size_t len)
{
    if (b == NULL || data == NULL || len == 0) {
        return 0;
    }

    if (!grow(b, len)) {
        return -1;
    }

    axl_memcpy(b->buf + b->len, data, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return 0;
}

int
axl_string_append_printf(AxlString *b, const char *fmt, ...)
{
    va_list args;

    if (b == NULL || fmt == NULL) {
        return 0;
    }

    b->error = false;
    va_start(args, fmt);
    axl_vformat(strbuf_write, b, fmt, args);
    va_end(args);
    return b->error ? -1 : 0;
}

int
axl_string_append_c(AxlString *b, char c)
{
    if (b == NULL) {
        return 0;
    }

    if (!grow(b, 1)) {
        return -1;
    }

    b->buf[b->len] = c;
    b->len++;
    b->buf[b->len] = '\0';
    return 0;
}

int
axl_string_prepend(AxlString *b, const char *s)
{
    size_t  slen;
    size_t  i;

    if (b == NULL || s == NULL) {
        return b == NULL ? -1 : 0;
    }

    slen = axl_strlen(s);
    if (slen == 0) {
        return 0;
    }

    if (!grow(b, slen)) {
        return -1;
    }

    // Shift existing content right (byte-by-byte, end to start)
    for (i = b->len; i != (size_t)-1; i--) {
        b->buf[i + slen] = b->buf[i];
    }

    axl_memcpy(b->buf, s, slen);
    b->len += slen;
    return 0;
}

int
axl_string_prepend_len(AxlString *b, const char *s, size_t len)
{
    size_t  i;

    if (b == NULL) {
        return -1;
    }
    if (s == NULL || len == 0) {
        return 0;
    }

    if (!grow(b, len)) {
        return -1;
    }

    // Shift existing content right
    for (i = b->len; i != (size_t)-1; i--) {
        b->buf[i + len] = b->buf[i];
    }

    axl_memcpy(b->buf, s, len);
    b->len += len;
    return 0;
}

int
axl_string_prepend_c(AxlString *b, char c)
{
    size_t  i;

    if (b == NULL) {
        return -1;
    }

    if (!grow(b, 1)) {
        return -1;
    }

    // Shift existing content right by 1
    for (i = b->len; i != (size_t)-1; i--) {
        b->buf[i + 1] = b->buf[i];
    }

    b->buf[0] = c;
    b->len++;
    return 0;
}

int
axl_string_insert(AxlString *b, size_t pos, const char *s)
{
    size_t  slen;
    size_t  i;

    if (b == NULL) {
        return -1;
    }
    if (s == NULL) {
        return 0;
    }

    slen = axl_strlen(s);
    if (slen == 0) {
        return 0;
    }

    // If pos >= len, just append
    if (pos >= b->len) {
        return axl_string_append(b, s);
    }

    if (!grow(b, slen)) {
        return -1;
    }

    // Shift content at pos right by slen (end to start)
    for (i = b->len; i >= pos && i != (size_t)-1; i--) {
        b->buf[i + slen] = b->buf[i];
    }

    axl_memcpy(b->buf + pos, s, slen);
    b->len += slen;
    return 0;
}

int
axl_string_erase(AxlString *b, size_t pos, size_t len)
{
    size_t  i;
    size_t  tail;

    if (b == NULL) {
        return -1;
    }

    if (pos >= b->len) {
        return 0;
    }

    // Clamp len to available bytes
    if (pos + len > b->len) {
        len = b->len - pos;
    }

    // Shift left: copy from pos+len to pos
    tail = b->len - pos - len;
    for (i = 0; i < tail; i++) {
        b->buf[pos + i] = b->buf[pos + len + i];
    }

    b->len -= len;
    b->buf[b->len] = '\0';
    return 0;
}

int
axl_string_truncate(AxlString *b, size_t len)
{
    if (b == NULL) {
        return -1;
    }

    if (len >= b->len) {
        return 0;
    }

    b->len = len;
    b->buf[b->len] = '\0';
    return 0;
}

int
axl_string_overwrite(AxlString *b, size_t pos, const char *s)
{
    size_t  slen;
    size_t  end;

    if (b == NULL) {
        return -1;
    }
    if (s == NULL) {
        return 0;
    }

    slen = axl_strlen(s);
    if (slen == 0) {
        return 0;
    }

    end = pos + slen;
    if (end > b->len) {
        if (!grow(b, end - b->len)) {
            return -1;
        }
        b->len = end;
        b->buf[b->len] = '\0';
    }

    axl_memcpy(b->buf + pos, s, slen);
    return 0;
}

const char *
axl_string_str(AxlString *b)
{
    if (b == NULL || b->buf == NULL) {
        return "";
    }
    return (const char *)b->buf;
}

size_t
axl_string_len(AxlString *b)
{
    if (b == NULL) {
        return 0;
    }
    return b->len;
}

char *
axl_string_steal(AxlString *b)
{
    char  *s;

    if (b == NULL || b->buf == NULL || b->len == 0) {
        return NULL;
    }

    s = b->buf;
    b->buf   = NULL;
    b->len   = 0;
    b->alloc = 0;

    return s;
}

void
axl_string_clear(AxlString *b)
{
    if (b == NULL) {
        return;
    }

    b->len = 0;
    if (b->buf != NULL) {
        b->buf[0] = '\0';
    }
}

void
axl_string_free(AxlString *b)
{
    if (b == NULL) {
        return;
    }

    axl_free(b->buf);
    axl_free(b);
}

char *
axl_asprintf(const char *fmt, ...)
{
    AxlString  *b;
    va_list    args;
    char       *result;

    if (fmt == NULL) {
        return NULL;
    }

    b = axl_string_new(NULL);
    if (b == NULL) {
        return NULL;
    }

    va_start(args, fmt);
    axl_vformat(strbuf_write, b, fmt, args);
    va_end(args);

    result = axl_string_steal(b);
    axl_string_free(b);
    return result;
}
