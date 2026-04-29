/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-str.c
    String utilities: basic string/mem ops, strlcpy/strlcat,
    UTF-8/UCS-2, base64, number parsing, snprintf.
**/

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-format.h>
AXL_LOG_DOMAIN("str");

typedef struct {
    char   *buf;
    size_t  size;
    size_t  written;
} SnprintfCtx;

// ---------------------------------------------------------------------------
// Basic string/memory operations (freestanding)
// ---------------------------------------------------------------------------

size_t
axl_strlen(const char *s)
{
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

int
axl_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int
axl_strncmp(const char *a, const char *b, size_t n)
{
    if (n == 0) {
        return 0;
    }
    while (n > 1 && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/**
 * @brief Case-insensitive ASCII string comparison.
 *
 * @return <0, 0, or >0.
 */
int
axl_strcasecmp(
    const char  *a,  ///< first string
    const char  *b   ///< second string
    )
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void *
axl_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (dst == NULL || src == NULL) {
        return dst;
    }
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *
axl_memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char val = (unsigned char)c;

    for (size_t i = 0; i < n; i++) {
        d[i] = val;
    }
    return dst;
}

int
axl_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;

    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

void *
axl_memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s || d >= s + n) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// axl_snprintf — format into fixed buffer via axl_vformat
// ---------------------------------------------------------------------------

static void
snprintf_writer(const char *data, size_t len, void *arg)
{
    SnprintfCtx *ctx = arg;
    for (size_t i = 0; i < len; i++) {
        if (ctx->written < ctx->size - 1) {
            ctx->buf[ctx->written] = data[i];
        }
        ctx->written++;
    }
}

int
axl_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    SnprintfCtx ctx;

    if (buf == NULL || size == 0) {
        return 0;
    }

    ctx.buf = buf;
    ctx.size = size;
    ctx.written = 0;

    va_start(args, fmt);
    axl_vformat(snprintf_writer, &ctx, fmt, args);
    va_end(args);

    if (ctx.written < size) {
        buf[ctx.written] = '\0';
    } else {
        buf[size - 1] = '\0';
    }

    return (int)ctx.written;
}

// ---------------------------------------------------------------------------
// Safe string copy/concat
// ---------------------------------------------------------------------------

size_t
axl_strlcpy(char *dst, const char *src, size_t dst_size)
{
    size_t src_len = 0;
    size_t i;

    /* Count source length */
    while (src[src_len] != '\0') {
        src_len++;
    }

    if (dst_size == 0) {
        return src_len;
    }

    /* Copy up to dst_size - 1 chars */
    for (i = 0; i < src_len && i < dst_size - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';

    return src_len;
}

size_t
axl_strlcat(char *dst, const char *src, size_t dst_size)
{
    size_t dst_len = 0;
    size_t src_len = 0;
    size_t i;

    /* Find end of dst */
    while (dst_len < dst_size && dst[dst_len] != '\0') {
        dst_len++;
    }

    /* Count source length */
    while (src[src_len] != '\0') {
        src_len++;
    }

    if (dst_len >= dst_size) {
        return dst_size + src_len;
    }

    /* Append up to remaining space */
    for (i = 0; i < src_len && dst_len + i < dst_size - 1; i++) {
        dst[dst_len + i] = src[i];
    }
    dst[dst_len + i] = '\0';

    return dst_len + src_len;
}

// ---------------------------------------------------------------------------
// String duplication
// ---------------------------------------------------------------------------

char *
axl_strndup(const char *s, size_t n)
{
    if (s == NULL) {
        return NULL;
    }

    size_t len = 0;
    while (len < n && s[len] != '\0') {
        len++;
    }

    char *dup = axl_malloc(len + 1);
    if (dup == NULL) {
        axl_warning("strndup allocation failed");
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        dup[i] = s[i];
    }
    dup[len] = '\0';

    return dup;
}

// ---------------------------------------------------------------------------
// String splitting, joining, trimming
// ---------------------------------------------------------------------------

char **
axl_strsplit(const char *str, char delimiter)
{
    size_t  count;
    size_t  i;
    size_t  start;
    size_t  len;
    char  **result;

    if (str == NULL) {
        return NULL;
    }

    /* Count segments */
    count = 1;
    for (i = 0; str[i] != '\0'; i++) {
        if (str[i] == delimiter) {
            count++;
        }
    }

    result = axl_malloc((count + 1) * sizeof(char *));
    if (result == NULL) {
        axl_error("failed to allocate strsplit array");
        return NULL;
    }

    /* Split */
    i = 0;
    start = 0;
    count = 0;
    while (str[i] != '\0') {
        if (str[i] == delimiter) {
            len = i - start;
            result[count] = axl_strndup(str + start, len);
            if (result[count] == NULL) {
                axl_strfreev(result);
                return NULL;
            }
            count++;
            start = i + 1;
        }
        i++;
    }

    /* Last segment */
    result[count] = axl_strndup(str + start, i - start);
    if (result[count] == NULL) {
        axl_strfreev(result);
        return NULL;
    }
    count++;
    result[count] = NULL;

    return result;
}

void
axl_strfreev(char **arr)
{
    if (arr == NULL) {
        return;
    }

    for (size_t i = 0; arr[i] != NULL; i++) {
        axl_free(arr[i]);
    }
    axl_free(arr);
}

char *
axl_strjoin(const char *separator, const char **arr)
{
    size_t  total;
    size_t  sep_len;
    size_t  count;
    size_t  i;
    char   *result;
    size_t  pos;

    if (arr == NULL) {
        return NULL;
    }

    sep_len = (separator != NULL) ? axl_strlen(separator) : 0;

    /* Calculate total length */
    total = 0;
    count = 0;
    for (i = 0; arr[i] != NULL; i++) {
        if (count > 0) {
            total += sep_len;
        }
        total += axl_strlen(arr[i]);
        count++;
    }

    result = axl_malloc(total + 1);
    if (result == NULL) {
        axl_error("failed to allocate strjoin result");
        return NULL;
    }

    /* Build string */
    pos = 0;
    for (i = 0; arr[i] != NULL; i++) {
        if (i > 0 && sep_len > 0) {
            axl_memcpy(result + pos, separator, sep_len);
            pos += sep_len;
        }
        size_t slen = axl_strlen(arr[i]);
        axl_memcpy(result + pos, arr[i], slen);
        pos += slen;
    }
    result[pos] = '\0';

    return result;
}

char *
axl_strstrip(char *str)
{
    size_t start;
    size_t end;
    size_t len;

    if (str == NULL) {
        return NULL;
    }

    /* Find start (skip leading whitespace) */
    start = 0;
    while (str[start] == ' ' || str[start] == '\t' ||
           str[start] == '\r' || str[start] == '\n') {
        start++;
    }

    if (str[start] == '\0') {
        str[0] = '\0';
        return str;
    }

    /* Find end (skip trailing whitespace) */
    end = axl_strlen(str);
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' ||
                           str[end - 1] == '\r' || str[end - 1] == '\n')) {
        end--;
    }

    /* Shift content if needed */
    len = end - start;
    if (start > 0) {
        for (size_t i = 0; i < len; i++) {
            str[i] = str[start + i];
        }
    }
    str[len] = '\0';

    return str;
}

// ---------------------------------------------------------------------------
// String searching
// ---------------------------------------------------------------------------

char *
axl_strchr(const char *s, int c)
{
    if (s == NULL) {
        return NULL;
    }
    for (; *s != '\0'; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *
axl_strstr(const char *haystack, const char *needle)
{
    return axl_strstr_len(haystack, -1, needle);
}

char *
axl_strncpy(char *dst, const char *src, size_t n)
{
    size_t i;

    if (dst == NULL) {
        return NULL;
    }
    if (src == NULL) {
        for (i = 0; i < n; i++) {
            dst[i] = '\0';
        }
        return dst;
    }
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *
axl_strstr_len(const char *haystack, long long haystack_len, const char *needle)
{
    size_t h_len;
    size_t n_len;
    size_t i;

    if (haystack == NULL || needle == NULL) {
        return NULL;
    }

    n_len = axl_strlen(needle);
    if (n_len == 0) {
        return (char *)haystack;
    }

    if (haystack_len < 0) {
        h_len = axl_strlen(haystack);
    } else {
        h_len = (size_t)haystack_len;
    }

    if (n_len > h_len) {
        return NULL;
    }

    for (i = 0; i <= h_len - n_len; i++) {
        if (haystack[i] == needle[0] &&
            axl_strncmp(haystack + i, needle, n_len) == 0) {
            return (char *)(haystack + i);
        }
    }

    return NULL;
}

char *
axl_strrstr(const char *haystack, const char *needle)
{
    size_t h_len;
    size_t n_len;
    size_t i;

    if (haystack == NULL || needle == NULL) {
        return NULL;
    }

    n_len = axl_strlen(needle);
    if (n_len == 0) {
        return (char *)(haystack + axl_strlen(haystack));
    }

    h_len = axl_strlen(haystack);
    if (n_len > h_len) {
        return NULL;
    }

    for (i = h_len - n_len; ; i--) {
        if (haystack[i] == needle[0] &&
            axl_strncmp(haystack + i, needle, n_len) == 0) {
            return (char *)(haystack + i);
        }
        if (i == 0) {
            break;
        }
    }

    return NULL;
}

char *
axl_strrstr_len(const char *haystack, long long haystack_len, const char *needle)
{
    size_t h_len;
    size_t n_len;
    size_t i;

    if (haystack == NULL || needle == NULL) {
        return NULL;
    }

    n_len = axl_strlen(needle);
    if (n_len == 0) {
        return (char *)haystack;
    }

    if (haystack_len < 0) {
        h_len = axl_strlen(haystack);
    } else {
        h_len = (size_t)haystack_len;
    }

    if (n_len > h_len) {
        return NULL;
    }

    for (i = h_len - n_len; ; i--) {
        if (haystack[i] == needle[0] &&
            axl_strncmp(haystack + i, needle, n_len) == 0) {
            return (char *)(haystack + i);
        }
        if (i == 0) {
            break;
        }
    }

    return NULL;
}

char *
axl_strcasestr(
    const char  *haystack,
    const char  *needle
    )
{
    if (haystack == NULL || needle == NULL) {
        return NULL;
    }
    if (*needle == '\0') {
        return (char *)haystack;
    }

    for (; *haystack != '\0'; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h != '\0' && *n != '\0') {
            unsigned char ch = (unsigned char)*h;
            unsigned char cn = (unsigned char)*n;
            if (ch >= 'A' && ch <= 'Z') {
                ch += 'a' - 'A';
            }
            if (cn >= 'A' && cn <= 'Z') {
                cn += 'a' - 'A';
            }
            if (ch != cn) {
                break;
            }
            h++;
            n++;
        }
        if (*n == '\0') {
            return (char *)haystack;
        }
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// String testing
// ---------------------------------------------------------------------------

/**
 * @brief Recursive glob matcher.
 */
static bool
fnmatch_impl(
    const char  *p,
    const char  *s
    )
{
    while (*p != '\0') {
        if (*p == '*') {
            p++;
            /* skip consecutive stars */
            while (*p == '*') {
                p++;
            }
            if (*p == '\0') {
                return true;
            }
            for (; *s != '\0'; s++) {
                if (fnmatch_impl(p, s)) {
                    return true;
                }
            }
            return fnmatch_impl(p, s);
        }

        if (*s == '\0') {
            return false;
        }

        if (*p == '?') {
            p++;
            s++;
            continue;
        }

        if (*p == '[') {
            bool invert = false;
            bool matched = false;
            p++;
            if (*p == '!' || *p == '^') {
                invert = true;
                p++;
            }
            while (*p != '\0' && *p != ']') {
                char lo = *p;
                p++;
                if (*p == '-' && *(p + 1) != '\0' && *(p + 1) != ']') {
                    p++;
                    char hi = *p;
                    p++;
                    if (*s >= lo && *s <= hi) {
                        matched = true;
                    }
                } else {
                    if (*s == lo) {
                        matched = true;
                    }
                }
            }
            if (*p == ']') {
                p++;
            }
            if (matched == invert) {
                return false;
            }
            s++;
            continue;
        }

        if (*p != *s) {
            return false;
        }
        p++;
        s++;
    }

    return *s == '\0';
}

bool
axl_fnmatch(
    const char  *pattern,
    const char  *string
    )
{
    if (pattern == NULL || string == NULL) {
        return false;
    }
    return fnmatch_impl(pattern, string);
}

bool
axl_str_has_prefix(const char *str, const char *prefix)
{
    if (str == NULL || prefix == NULL) {
        return false;
    }

    while (*prefix != '\0') {
        if (*str != *prefix) {
            return false;
        }
        str++;
        prefix++;
    }

    return true;
}

bool
axl_str_has_suffix(const char *str, const char *suffix)
{
    size_t str_len;
    size_t suf_len;

    if (str == NULL || suffix == NULL) {
        return false;
    }

    str_len = axl_strlen(str);
    suf_len = axl_strlen(suffix);

    if (suf_len > str_len) {
        return false;
    }

    return axl_strncmp(str + str_len - suf_len, suffix, suf_len) == 0;
}

bool
axl_str_is_ascii(const char *str)
{
    if (str == NULL) {
        return false;
    }

    while (*str != '\0') {
        if ((unsigned char)*str > 0x7F) {
            return false;
        }
        str++;
    }

    return true;
}

int
axl_strcmp0(const char *str1, const char *str2)
{
    if (str1 == NULL && str2 == NULL) {
        return 0;
    }
    if (str1 == NULL) {
        return -1;
    }
    if (str2 == NULL) {
        return 1;
    }
    return axl_strcmp(str1, str2);
}

bool
axl_str_equal(const void *v1, const void *v2)
{
    return axl_strcmp((const char *)v1, (const char *)v2) == 0;
}

size_t
axl_str_hash(const void *key)
{
#if defined(MDE_CPU_X64) || defined(MDE_CPU_AARCH64)
    #define AXL_FNV_OFFSET  14695981039346656037ULL
    #define AXL_FNV_PRIME   1099511628211ULL
#else
    #define AXL_FNV_OFFSET  2166136261u
    #define AXL_FNV_PRIME   16777619u
#endif
    const char *s = (const char *)key;
    size_t hash = AXL_FNV_OFFSET;

    while (*s != '\0') {
        hash ^= (size_t)(unsigned char)*s;
        hash *= AXL_FNV_PRIME;
        s++;
    }

    return hash;
#undef AXL_FNV_OFFSET
#undef AXL_FNV_PRIME
}

int
axl_strncasecmp(const char *s1, const char *s2, size_t n)
{
    size_t i;

    if (s1 == NULL && s2 == NULL) {
        return 0;
    }
    if (s1 == NULL) {
        return -1;
    }
    if (s2 == NULL) {
        return 1;
    }

    for (i = 0; i < n; i++) {
        unsigned char c1 = (unsigned char)s1[i];
        unsigned char c2 = (unsigned char)s2[i];

        if (c1 >= 'A' && c1 <= 'Z') {
            c1 += 'a' - 'A';
        }
        if (c2 >= 'A' && c2 <= 'Z') {
            c2 += 'a' - 'A';
        }

        if (c1 != c2) {
            return (int)c1 - (int)c2;
        }
        if (c1 == '\0') {
            return 0;
        }
    }

    return 0;
}

bool
axl_strv_contains(const char *const *strv, const char *str)
{
    if (strv == NULL || str == NULL) {
        return false;
    }

    while (*strv != NULL) {
        if (axl_strcmp(*strv, str) == 0) {
            return true;
        }
        strv++;
    }

    return false;
}

bool
axl_strv_equal(const char *const *strv1, const char *const *strv2)
{
    if (strv1 == NULL || strv2 == NULL) {
        return false;
    }

    while (*strv1 != NULL && *strv2 != NULL) {
        if (axl_strcmp(*strv1, *strv2) != 0) {
            return false;
        }
        strv1++;
        strv2++;
    }

    return *strv1 == NULL && *strv2 == NULL;
}

// ---------------------------------------------------------------------------
// UTF-8 -> UCS-2
// ---------------------------------------------------------------------------

unsigned short *
axl_utf8_to_ucs2(const char *s)
{
    const uint8_t  *in;
    size_t          count;
    size_t          i;
    unsigned short *out;
    uint32_t        cp;

    if (s == NULL) {
        return NULL;
    }

    // Helper macro: check if byte is a valid continuation byte (10xxxxxx)
    #define IS_CONT(b) (((b) & 0xC0) == 0x80)

    // First pass: count output UCS-2 values
    in = (const uint8_t *)s;
    count = 0;
    while (*in != 0) {
        if ((*in & 0x80) == 0) {
            in += 1;
        } else if ((*in & 0xE0) == 0xC0 && IS_CONT(in[1])) {
            in += 2;
        } else if ((*in & 0xF0) == 0xE0 && IS_CONT(in[1]) && IS_CONT(in[2])) {
            in += 3;
        } else if ((*in & 0xF8) == 0xF0 && IS_CONT(in[1]) && IS_CONT(in[2]) && IS_CONT(in[3])) {
            in += 4;
            continue; // 4-byte(above BMP) — skip
        } else {
            in += 1; // invalid byte — skip
            continue;
        }
        count++;
    }

    out = (unsigned short *)axl_malloc((count + 1) * sizeof (unsigned short));
    if (out == NULL) {
        axl_warning("utf8_to_ucs2 allocation failed");
        return NULL;
    }

    // Second pass: decode
    in = (const uint8_t *)s;
    i = 0;
    while (*in != 0) {
        if ((*in & 0x80) == 0) {
            out[i++] = (unsigned short)*in;
            in += 1;
        } else if ((*in & 0xE0) == 0xC0 && IS_CONT(in[1])) {
            cp = ((uint32_t)(in[0] & 0x1F) << 6) |
                 ((uint32_t)(in[1] & 0x3F));
            out[i++] = (unsigned short)cp;
            in += 2;
        } else if ((*in & 0xF0) == 0xE0 && IS_CONT(in[1]) && IS_CONT(in[2])) {
            cp = ((uint32_t)(in[0] & 0x0F) << 12) |
                 ((uint32_t)(in[1] & 0x3F) << 6) |
                 ((uint32_t)(in[2] & 0x3F));
            out[i++] = (unsigned short)cp;
            in += 3;
        } else if ((*in & 0xF8) == 0xF0 && IS_CONT(in[1]) && IS_CONT(in[2]) && IS_CONT(in[3])) {
            in += 4; // above BMP — skip
        } else {
            in += 1; // invalid byte — skip
        }
    }
    out[i] = 0;

    #undef IS_CONT

    return out;
}

size_t
axl_utf8_to_ucs2_buf(const char *src, unsigned short *dst, size_t dst_count)
{
    size_t i = 0;

    if (src == NULL || dst == NULL || dst_count == 0) {
        return 0;
    }

    while (src[i] != '\0' && i + 1 < dst_count) {
        dst[i] = (unsigned short)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

// ---------------------------------------------------------------------------
// UCS-2 -> UTF-8
// ---------------------------------------------------------------------------

char *
axl_ucs2_to_utf8(const unsigned short *s)
{
    const unsigned short  *p;
    size_t                 bytes;
    char                  *out;
    size_t                 i;
    uint16_t               ch;

    if (s == NULL) {
        return NULL;
    }

    // First pass: count output bytes
    bytes = 0;
    for (p = s; *p != 0; p++) {
        ch = (uint16_t)*p;
        if (ch < 0x80) {
            bytes += 1;
        } else if (ch < 0x800) {
            bytes += 2;
        } else {
            bytes += 3;
        }
    }

    out = (char *)axl_malloc(bytes + 1);
    if (out == NULL) {
        axl_warning("ucs2_to_utf8 allocation failed");
        return NULL;
    }

    // Second pass: encode
    i = 0;
    for (p = s; *p != 0; p++) {
        ch = (uint16_t)*p;
        if (ch < 0x80) {
            out[i++] = (char)ch;
        } else if (ch < 0x800) {
            out[i++] = (char)(0xC0 | (ch >> 6));
            out[i++] = (char)(0x80 | (ch & 0x3F));
        } else {
            out[i++] = (char)(0xE0 | (ch >> 12));
            out[i++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            out[i++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[i] = '\0';

    return out;
}

// ---------------------------------------------------------------------------
// Base64 (RFC 4648)
// ---------------------------------------------------------------------------

static const char b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t b64_dec[128] = {
    /* 0x00-0x0F */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x10-0x1F */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x20-0x2F */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  62,0xFF,0xFF,0xFF,  63,
    /* 0x30-0x3F */   52,  53,  54,  55,  56,  57,  58,  59,  60,  61,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x40-0x4F */ 0xFF,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
    /* 0x50-0x5F */   15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* 0x60-0x6F */ 0xFF,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    /* 0x70-0x7F */   41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,0xFF,0xFF,0xFF,0xFF,0xFF
};

char *
axl_base64_encode(const void *data, size_t len)
{
    const uint8_t *in;
    size_t         out_len;
    char          *out;
    size_t         i;
    size_t         j;
    uint32_t       trip;

    if (data == NULL || len == 0) {
        out = (char *)axl_malloc(1);
        if (out != NULL) {
            out[0] = '\0';
        }
        return out;
    }

    in = (const uint8_t *)data;
    out_len = 4 * ((len + 2) / 3);
    out = (char *)axl_malloc(out_len + 1);
    if (out == NULL) {
        axl_warning("base64_encode allocation failed");
        return NULL;
    }

    j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        trip = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[j++] = b64_enc[(trip >> 18) & 0x3F];
        out[j++] = b64_enc[(trip >> 12) & 0x3F];
        out[j++] = b64_enc[(trip >>  6) & 0x3F];
        out[j++] = b64_enc[(trip      ) & 0x3F];
    }

    if (i < len) {
        trip = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            trip |= (uint32_t)in[i+1] << 8;
        }
        out[j++] = b64_enc[(trip >> 18) & 0x3F];
        out[j++] = b64_enc[(trip >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_enc[(trip >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }

    out[j] = '\0';
    return out;
}

int
axl_base64_decode(const char *b64, void **out, size_t *out_len)
{
    size_t   in_len;
    size_t   pad;
    size_t   raw_len;
    uint8_t *raw;
    size_t   i;
    size_t   j;
    uint8_t  a, b, c, d;

    if (b64 == NULL || out == NULL || out_len == NULL) {
        return -1;
    }

    in_len = axl_strlen(b64);
    if (in_len == 0) {
        *out = axl_malloc(1);
        *out_len = 0;
        return (*out != NULL) ? 0 : -1;
    }

    if ((in_len % 4) != 0) {
        return -1;
    }

    pad = 0;
    if (b64[in_len - 1] == '=') { pad++; }
    if (b64[in_len - 2] == '=') { pad++; }

    raw_len = (in_len / 4) * 3 - pad;
    raw = (uint8_t *)axl_malloc(raw_len + 1);
    if (raw == NULL) {
        axl_warning("base64_decode allocation failed");
        return -1;
    }

    j = 0;
    for (i = 0; i < in_len; i += 4) {
        a = ((uint8_t)b64[i]   < 128) ? b64_dec[(uint8_t)b64[i]]   : 0xFF;
        b = ((uint8_t)b64[i+1] < 128) ? b64_dec[(uint8_t)b64[i+1]] : 0xFF;
        c = (b64[i+2] != '=' && (uint8_t)b64[i+2] < 128) ? b64_dec[(uint8_t)b64[i+2]] : 0xFF;
        d = (b64[i+3] != '=' && (uint8_t)b64[i+3] < 128) ? b64_dec[(uint8_t)b64[i+3]] : 0xFF;

        if (a == 0xFF || b == 0xFF) {
            axl_free(raw);
            return -1;
        }

        raw[j++] = (uint8_t)((a << 2) | (b >> 4));
        if (j < raw_len) {
            if (c == 0xFF && b64[i+2] != '=') { axl_free(raw); return -1; }
            raw[j++] = (uint8_t)((b << 4) | (c >> 2));
        }
        if (j < raw_len) {
            if (d == 0xFF && b64[i+3] != '=') { axl_free(raw); return -1; }
            raw[j++] = (uint8_t)((c << 6) | d);
        }
    }

    *out = raw;
    *out_len = (size_t)raw_len;
    return 0;
}

// ---------------------------------------------------------------------------
// Number parsing
// ---------------------------------------------------------------------------

/* Map an ASCII digit char to its numeric value for the given base.
 * Returns -1 if the char isn't a valid digit for that base. */
static int
digit_value(
    char c,
    int  base
    )
{
    int v;
    if (c >= '0' && c <= '9') {
        v = c - '0';
    } else if (c >= 'a' && c <= 'z') {
        v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
        v = c - 'A' + 10;
    } else {
        return -1;
    }
    return (v < base) ? v : -1;
}

int
axl_str_to_u64(
    const char  *nptr,
    int          base,
    uint64_t    *out,
    const char **endptr
    )
{
    if (endptr != NULL) {
        *endptr = nptr;
    }
    if (nptr == NULL || out == NULL) {
        return -1;
    }
    if (base != 0 && (base < 2 || base > 36)) {
        return -1;
    }

    const char *p = nptr;

    /* Leading whitespace (matches strtoul + the legacy axl_strtou64). */
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    /* Optional '+' sign. '-' is rejected for unsigned. */
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        return -1;
    }

    /* "0x"/"0X" prefix: required for base 0 to switch to hex,
     * tolerated when base is explicitly 16. We deliberately do NOT
     * decode leading "0" as octal — surprising and rarely useful. */
    if ((base == 0 || base == 16) && p[0] == '0'
        && (p[1] == 'x' || p[1] == 'X')
        && digit_value(p[2], 16) >= 0)
    {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = 10;
    }

    /* Need at least one valid digit. */
    if (digit_value(*p, base) < 0) {
        return -1;
    }

    /* Accumulate, checking overflow on each step. */
    uint64_t val = 0;
    const uint64_t cutoff = UINT64_MAX / (uint64_t)base;
    const int      cutlim = (int)(UINT64_MAX % (uint64_t)base);

    while (true) {
        int d = digit_value(*p, base);
        if (d < 0) {
            break;
        }
        if (val > cutoff || (val == cutoff && d > cutlim)) {
            return -1;
        }
        val = val * (uint64_t)base + (uint64_t)d;
        p++;
    }

    /* Strict mode: with no endptr, the entire input must be consumed.
     * Callers that want partial parsing (tokenization) pass endptr.
     * This is the whole point of the new API — silent partial parses
     * are the bug axl_strtou64 had. */
    if (endptr == NULL) {
        if (*p != '\0') {
            return -1;
        }
    } else {
        *endptr = p;
    }
    *out = val;
    return 0;
}

/* Shared narrow-unsigned helper: parse via u64, range-check against
 * the caller-supplied ceiling, restore endptr to nptr on overflow. */
static int
str_to_unarrow(
    const char  *nptr,
    int          base,
    uint64_t     ceiling,
    uint64_t    *out,
    const char **endptr
    )
{
    uint64_t v;
    if (out == NULL) {
        return -1;
    }
    if (axl_str_to_u64(nptr, base, &v, endptr) != 0) {
        return -1;
    }
    if (v > ceiling) {
        if (endptr != NULL) {
            *endptr = nptr;
        }
        return -1;
    }
    *out = v;
    return 0;
}

int
axl_str_to_u32(
    const char  *nptr,
    int          base,
    uint32_t    *out,
    const char **endptr
    )
{
    uint64_t v;
    if (str_to_unarrow(nptr, base, UINT32_MAX, &v, endptr) != 0) {
        return -1;
    }
    *(uint32_t *)out = (uint32_t)v;
    return 0;
}

int
axl_str_to_u16(
    const char  *nptr,
    int          base,
    uint16_t    *out,
    const char **endptr
    )
{
    uint64_t v;
    if (str_to_unarrow(nptr, base, UINT16_MAX, &v, endptr) != 0) {
        return -1;
    }
    *(uint16_t *)out = (uint16_t)v;
    return 0;
}

int
axl_str_to_u8(
    const char  *nptr,
    int          base,
    uint8_t     *out,
    const char **endptr
    )
{
    uint64_t v;
    if (str_to_unarrow(nptr, base, UINT8_MAX, &v, endptr) != 0) {
        return -1;
    }
    *(uint8_t *)out = (uint8_t)v;
    return 0;
}

int
axl_str_to_s64(
    const char  *nptr,
    int          base,
    int64_t     *out,
    const char **endptr
    )
{
    if (endptr != NULL) {
        *endptr = nptr;
    }
    if (nptr == NULL || out == NULL) {
        return -1;
    }

    const char *p = nptr;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    bool negative = false;
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        negative = true;
        p++;
    }

    /* Parse the magnitude as unsigned. We bound by the range of the
     * negative side (|INT64_MIN|) since it's larger than INT64_MAX.
     * If u64 advances endptr past the magnitude and we then reject for
     * range, restore endptr to nptr — contract is "endptr untouched
     * past nptr on error" and overflow is an error. */
    uint64_t v;
    if (axl_str_to_u64(p, base, &v, endptr) != 0) {
        if (endptr != NULL) {
            *endptr = nptr;
        }
        return -1;
    }

    if (negative) {
        if (v > (uint64_t)INT64_MAX + 1u) {
            if (endptr != NULL) {
                *endptr = nptr;
            }
            return -1;
        }
        /* -INT64_MIN is undefined; build it without negating. */
        *out = (v == (uint64_t)INT64_MAX + 1u)
            ? INT64_MIN
            : -(int64_t)v;
    } else {
        if (v > (uint64_t)INT64_MAX) {
            if (endptr != NULL) {
                *endptr = nptr;
            }
            return -1;
        }
        *out = (int64_t)v;
    }
    return 0;
}

/* Shared narrow-signed helper: parse via s64, range-check against
 * [floor, ceiling], restore endptr to nptr on overflow. */
static int
str_to_snarrow(
    const char  *nptr,
    int          base,
    int64_t      floor,
    int64_t      ceiling,
    int64_t     *out,
    const char **endptr
    )
{
    int64_t v;
    if (out == NULL) {
        return -1;
    }
    if (axl_str_to_s64(nptr, base, &v, endptr) != 0) {
        return -1;
    }
    if (v < floor || v > ceiling) {
        if (endptr != NULL) {
            *endptr = nptr;
        }
        return -1;
    }
    *out = v;
    return 0;
}

int
axl_str_to_s32(
    const char  *nptr,
    int          base,
    int32_t     *out,
    const char **endptr
    )
{
    int64_t v;
    if (str_to_snarrow(nptr, base, INT32_MIN, INT32_MAX, &v, endptr) != 0) {
        return -1;
    }
    *(int32_t *)out = (int32_t)v;
    return 0;
}

int
axl_str_to_s16(
    const char  *nptr,
    int          base,
    int16_t     *out,
    const char **endptr
    )
{
    int64_t v;
    if (str_to_snarrow(nptr, base, INT16_MIN, INT16_MAX, &v, endptr) != 0) {
        return -1;
    }
    *(int16_t *)out = (int16_t)v;
    return 0;
}

int
axl_str_to_s8(
    const char  *nptr,
    int          base,
    int8_t      *out,
    const char **endptr
    )
{
    int64_t v;
    if (str_to_snarrow(nptr, base, INT8_MIN, INT8_MAX, &v, endptr) != 0) {
        return -1;
    }
    *(int8_t *)out = (int8_t)v;
    return 0;
}

uint64_t
axl_strtou64(const char *s)
{
    /* Legacy: best-effort, no error reporting, accepts partial parses
     * and silently wraps on overflow. New code should use
     * axl_str_to_u64. Implemented in-place rather than via the new
     * function because the new function rejects partial parses
     * ("123abc" → -1) and overflow, which would change behavior for
     * callers that rely on the lax semantics. */
    uint64_t val;

    if (s == NULL) {
        return 0;
    }

    while (*s == ' ' || *s == '\t') {
        s++;
    }

    val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            char c = *s;
            if (c >= '0' && c <= '9') {
                val = (val << 4) | (uint64_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                val = (val << 4) | (uint64_t)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                val = (val << 4) | (uint64_t)(c - 'A' + 10);
            } else {
                break;
            }
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (uint64_t)(*s - '0');
            s++;
        }
    }

    return (uint64_t)val;
}

int
axl_strtou64_with_offset(
    const char  *s,
    uint64_t    *out
    )
{
    if (s == NULL || out == NULL) {
        return -1;
    }

    /* Parse the base value. Auto-detect 0x prefix via base=0. The
     * endptr tells us where parsing stopped — either '\0' (no
     * offset) or '+' (offset follows). */
    uint64_t     base_val = 0;
    const char  *endptr   = NULL;
    if (axl_str_to_u64(s, 0, &base_val, &endptr) != 0) {
        return -1;
    }

    /* No offset: input must be fully consumed. */
    if (*endptr == '\0') {
        *out = base_val;
        return 0;
    }

    /* Anything other than '+' immediately after the base value is a
     * parse error. Reject whitespace around the '+' too — a
     * legitimate value never has spaces in the middle, and being
     * strict here catches typos that would otherwise silently
     * produce a wrong result. */
    if (*endptr != '+') {
        return -1;
    }
    const char *off_str = endptr + 1;
    if (*off_str == '\0' || *off_str == ' ' || *off_str == '\t') {
        return -1;
    }
    /* axl_str_to_u64 itself accepts a leading '+' (and rejects '-')
     * — strip those out here so "0x100++0x10" and "0x100+-0x10"
     * are parse errors instead of slipping through to a positive
     * accept on the inner call. The "+offset" syntax has exactly
     * one '+' separator and no signs on either side. */
    if (*off_str == '+' || *off_str == '-') {
        return -1;
    }

    uint64_t     off_val = 0;
    const char  *off_end = NULL;
    if (axl_str_to_u64(off_str, 0, &off_val, &off_end) != 0) {
        return -1;
    }
    if (*off_end != '\0') {
        return -1;   /* trailing garbage after the offset */
    }

    /* Sum, with overflow check. */
    if (off_val > UINT64_MAX - base_val) {
        return -1;
    }
    *out = base_val + off_val;
    return 0;
}
