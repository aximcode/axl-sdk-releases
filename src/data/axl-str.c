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

size_t
axl_strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (n < maxlen && s[n]) {
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
        unsigned char ca = (unsigned char)axl_tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)axl_tolower((unsigned char)*b);
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
axl_memmem(
    const void  *haystack,
    size_t       haystack_len,
    const void  *needle,
    size_t       needle_len
    )
{
    if (haystack == NULL || needle == NULL
        || needle_len == 0 || needle_len > haystack_len) {
        return NULL;
    }
    const unsigned char *hay = (const unsigned char *)haystack;
    const unsigned char *nd  = (const unsigned char *)needle;
    /* First byte is the cheap discriminator; only when it matches
       do we compare the rest. */
    size_t last = haystack_len - needle_len;
    for (size_t i = 0; i <= last; i++) {
        if (hay[i] != nd[0]) {
            continue;
        }
        size_t j = 1;
        while (j < needle_len && hay[i + j] == nd[j]) {
            j++;
        }
        if (j == needle_len) {
            /* Cast away const at the API boundary — matches the
               GNU memmem signature, which lets callers find a
               match in either const or mutable storage. */
            return (void *)(hay + i);
        }
    }
    return NULL;
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
// Hex parsing
// ---------------------------------------------------------------------------

int
axl_hex_parse_u64(
    const char *s,
    size_t      max_chars,
    uint64_t   *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    uint64_t v = 0;
    size_t   i = 0;
    while (i < max_chars) {
        int d = axl_hex_nibble((unsigned char)s[i]);
        if (d < 0) {
            break;
        }
        /* Detect overflow: shifting left 4 must not lose top nibble. */
        if ((v >> 60) != 0) {
            return -1;
        }
        v = (v << 4) | (uint64_t)d;
        i++;
    }
    if (i == 0) {
        return -1;
    }
    *out = v;
    return (int)i;
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
// Human-readable byte formatting (IEC binary units)
// ---------------------------------------------------------------------------

int
axl_format_bytes(uint64_t value, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return -1;
    }
    static const struct {
        uint64_t    divisor;
        const char *suffix;
    } units[] = {
        { 1ULL << 40, "TiB" },
        { 1ULL << 30, "GiB" },
        { 1ULL << 20, "MiB" },
        { 1ULL << 10, "KiB" },
    };

    /* Pick the largest unit that divides @a value evenly. */
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (value >= units[i].divisor && (value % units[i].divisor) == 0) {
            return axl_snprintf(buf, buf_size, "%llu %s",
                                (unsigned long long)(value / units[i].divisor),
                                units[i].suffix);
        }
    }
    /* Non-even values fall back to the largest unit whose floor is
       non-zero, with the raw byte count appended for transparency. */
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (value >= units[i].divisor) {
            return axl_snprintf(buf, buf_size, "%llu %s (%llu B)",
                                (unsigned long long)(value / units[i].divisor),
                                units[i].suffix,
                                (unsigned long long)value);
        }
    }
    return axl_snprintf(buf, buf_size, "%llu B",
                        (unsigned long long)value);
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

// ---------------------------------------------------------------------------
// Boyer-Moore-Horspool substring search
//
// Below the BMH_THRESHOLD, the per-search cost of building the skip
// table dominates the win — fall back to naive byte-walking. Above
// it, BMH gives sub-linear average performance and is what every
// serious strstr (glibc memmem, musl twoway-fallback, BSD libc)
// uses.
//
// One skip table covers the whole alphabet (256 bytes). Default
// shift is the pattern length; each pattern byte (except the last)
// shrinks the shift to "distance from end". Later occurrences of a
// byte in the pattern overwrite earlier — the correct rule for
// Horspool's variant.
// ---------------------------------------------------------------------------

#define BMH_THRESHOLD  4u

static const char *
bmh_search(const char *haystack, size_t h_len,
           const char *needle,   size_t n_len)
{
    /* Skip table: how far to advance the window when the byte at
       its tail mismatches. Use size_t to avoid overflow on long
       patterns; 256 entries × 8 bytes = 2KB on x64 stack — fine. */
    size_t skip[256];
    for (int i = 0; i < 256; i++) {
        skip[i] = n_len;
    }
    for (size_t k = 0; k + 1 < n_len; k++) {
        skip[(unsigned char)needle[k]] = n_len - 1 - k;
    }

    size_t i = 0;
    const size_t last_n = n_len - 1;
    while (i + n_len <= h_len) {
        /* Compare from end of pattern backward — the byte that
           drives the shift is examined first. */
        if (haystack[i + last_n] == needle[last_n]) {
            size_t k = last_n;
            while (k > 0 && haystack[i + k - 1] == needle[k - 1]) {
                k--;
            }
            if (k == 0) {
                return haystack + i;
            }
        }
        i += skip[(unsigned char)haystack[i + last_n]];
    }
    return NULL;
}

/* Case-insensitive Horspool variant. The skip table is built over
   the lowercase-folded pattern, and every haystack byte is folded
   on the fly during comparison and shift lookup. */
static const char *
bmh_isearch(const char *haystack, size_t h_len,
            const char *needle,   size_t n_len)
{
    size_t skip[256];
    for (int i = 0; i < 256; i++) {
        skip[i] = n_len;
    }
    for (size_t k = 0; k + 1 < n_len; k++) {
        unsigned char nc = (unsigned char)axl_tolower((unsigned char)needle[k]);
        skip[nc] = n_len - 1 - k;
    }

    size_t i = 0;
    const size_t last_n = n_len - 1;
    while (i + n_len <= h_len) {
        unsigned char hc = (unsigned char)axl_tolower((unsigned char)haystack[i + last_n]);
        unsigned char nc = (unsigned char)axl_tolower((unsigned char)needle[last_n]);
        if (hc == nc) {
            size_t k = last_n;
            while (k > 0) {
                unsigned char a = (unsigned char)axl_tolower((unsigned char)haystack[i + k - 1]);
                unsigned char b = (unsigned char)axl_tolower((unsigned char)needle[k - 1]);
                if (a != b) break;
                k--;
            }
            if (k == 0) {
                return haystack + i;
            }
        }
        i += skip[(unsigned char)axl_tolower((unsigned char)haystack[i + last_n])];
    }
    return NULL;
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

    /* Long-needle path: Boyer-Moore-Horspool — sub-linear average,
       what glibc/musl/BSD libc all use. The skip-table setup costs
       ~256 stores, so it's only a win once the inner loop saves
       enough comparisons to amortize it. */
    if (n_len >= BMH_THRESHOLD) {
        return (char *)bmh_search(haystack, h_len, needle, n_len);
    }

    /* Short-needle path: naive scan with first-byte fast-path. */
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
axl_strcasestr(const char *haystack, const char *needle)
{
    return axl_strcasestr_len(haystack, -1, needle);
}

char *
axl_strcasestr_len(
    const char  *haystack,
    long long    haystack_len,
    const char  *needle
    )
{
    if (haystack == NULL || needle == NULL) {
        return NULL;
    }

    size_t n_len = axl_strlen(needle);
    if (n_len == 0) {
        return (char *)haystack;
    }

    size_t h_len = (haystack_len < 0)
                 ? axl_strlen(haystack)
                 : (size_t)haystack_len;

    if (n_len > h_len) {
        return NULL;
    }

    /* Long-needle path: case-insensitive Horspool. */
    if (n_len >= BMH_THRESHOLD) {
        return (char *)bmh_isearch(haystack, h_len, needle, n_len);
    }

    /* Short-needle path: naive scan with per-byte case fold. */
    for (size_t i = 0; i <= h_len - n_len; i++) {
        size_t k = 0;
        while (k < n_len) {
            unsigned char ch = (unsigned char)axl_tolower((unsigned char)haystack[i + k]);
            unsigned char cn = (unsigned char)axl_tolower((unsigned char)needle[k]);
            if (ch != cn) break;
            k++;
        }
        if (k == n_len) {
            return (char *)(haystack + i);
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
        unsigned char c1 = (unsigned char)axl_tolower((unsigned char)s1[i]);
        unsigned char c2 = (unsigned char)axl_tolower((unsigned char)s2[i]);

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
// UCS-2 -> UTF-8 (caller buffer)
// ---------------------------------------------------------------------------

size_t
axl_ucs2_to_utf8_buf(
    const unsigned short *src,
    char                 *dst,
    size_t                dst_size
    )
{
    if (dst == NULL || dst_size == 0) {
        return 0;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    size_t   i = 0;
    /* Reserve 1 byte for NUL. Each char takes 1-3 bytes; we only
       commit if the whole sequence fits. */
    for (const unsigned short *p = src; *p != 0; p++) {
        uint16_t ch = (uint16_t)*p;
        size_t   need;
        if (ch < 0x80) {
            need = 1;
        } else if (ch < 0x800) {
            need = 2;
        } else {
            need = 3;
        }
        if (i + need + 1 > dst_size) {
            break;
        }
        if (ch < 0x80) {
            dst[i++] = (char)ch;
        } else if (ch < 0x800) {
            dst[i++] = (char)(0xC0 | (ch >> 6));
            dst[i++] = (char)(0x80 | (ch & 0x3F));
        } else {
            dst[i++] = (char)(0xE0 | (ch >> 12));
            dst[i++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            dst[i++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    dst[i] = '\0';
    return i;
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
        return AXL_ERR;
    }

    in_len = axl_strlen(b64);
    if (in_len == 0) {
        *out = axl_malloc(1);
        *out_len = 0;
        return (*out != NULL) ? AXL_OK : AXL_ERR;
    }

    if ((in_len % 4) != 0) {
        return AXL_ERR;
    }

    pad = 0;
    if (b64[in_len - 1] == '=') { pad++; }
    if (b64[in_len - 2] == '=') { pad++; }

    raw_len = (in_len / 4) * 3 - pad;
    raw = (uint8_t *)axl_malloc(raw_len + 1);
    if (raw == NULL) {
        axl_warning("base64_decode allocation failed");
        return AXL_ERR;
    }

    j = 0;
    for (i = 0; i < in_len; i += 4) {
        a = ((uint8_t)b64[i]   < 128) ? b64_dec[(uint8_t)b64[i]]   : 0xFF;
        b = ((uint8_t)b64[i+1] < 128) ? b64_dec[(uint8_t)b64[i+1]] : 0xFF;
        c = (b64[i+2] != '=' && (uint8_t)b64[i+2] < 128) ? b64_dec[(uint8_t)b64[i+2]] : 0xFF;
        d = (b64[i+3] != '=' && (uint8_t)b64[i+3] < 128) ? b64_dec[(uint8_t)b64[i+3]] : 0xFF;

        if (a == 0xFF || b == 0xFF) {
            axl_free(raw);
            return AXL_ERR;
        }

        raw[j++] = (uint8_t)((a << 2) | (b >> 4));
        if (j < raw_len) {
            if (c == 0xFF && b64[i+2] != '=') { axl_free(raw); return AXL_ERR; }
            raw[j++] = (uint8_t)((b << 4) | (c >> 2));
        }
        if (j < raw_len) {
            if (d == 0xFF && b64[i+3] != '=') { axl_free(raw); return AXL_ERR; }
            raw[j++] = (uint8_t)((c << 6) | d);
        }
    }

    *out = raw;
    *out_len = (size_t)raw_len;
    return AXL_OK;
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
        return AXL_ERR;
    }
    if (base != 0 && (base < 2 || base > 36)) {
        return AXL_ERR;
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
        return AXL_ERR;
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
        return AXL_ERR;
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
            return AXL_ERR;
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
            return AXL_ERR;
        }
    } else {
        *endptr = p;
    }
    *out = val;
    return AXL_OK;
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
    if (axl_str_to_u64(nptr, base, &v, endptr) != AXL_OK) {
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
        return AXL_ERR;
    }
    *(uint32_t *)out = (uint32_t)v;
    return AXL_OK;
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
        return AXL_ERR;
    }
    *(uint16_t *)out = (uint16_t)v;
    return AXL_OK;
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
        return AXL_ERR;
    }
    *(uint8_t *)out = (uint8_t)v;
    return AXL_OK;
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
        return AXL_ERR;
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
    if (axl_str_to_u64(p, base, &v, endptr) != AXL_OK) {
        if (endptr != NULL) {
            *endptr = nptr;
        }
        return AXL_ERR;
    }

    if (negative) {
        if (v > (uint64_t)INT64_MAX + 1u) {
            if (endptr != NULL) {
                *endptr = nptr;
            }
            return AXL_ERR;
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
            return AXL_ERR;
        }
        *out = (int64_t)v;
    }
    return AXL_OK;
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
        return AXL_ERR;
    }
    *(int32_t *)out = (int32_t)v;
    return AXL_OK;
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
        return AXL_ERR;
    }
    *(int16_t *)out = (int16_t)v;
    return AXL_OK;
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
        return AXL_ERR;
    }
    *(int8_t *)out = (int8_t)v;
    return AXL_OK;
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
    if (s == NULL || out == NULL) { return AXL_ERR; }

    /* Dogfooded with AxlStrReader. Grammar:
     *   value := u64
     *          | u64 '+' u64       (no whitespace, no sign chars)
     */
    AxlStrReader r;
    axl_str_reader_init(&r, s);

    uint64_t base_val = 0;
    if (!axl_str_reader_take_u64(&r, 0, &base_val)) { return AXL_ERR; }

    /* No offset: input must be fully consumed. */
    if (axl_str_reader_eof(&r)) {
        *out = base_val;
        return AXL_OK;
    }

    /* Otherwise must be '+' followed by an unsigned literal with no
     * sign chars or whitespace. Sticky-ok handles the chain. */
    if (!axl_str_reader_consume_char(&r, '+')) { return AXL_ERR; }
    char next = axl_str_reader_peek(&r);
    if (next == '+' || next == '-' || next == ' ' || next == '\t' || next == '\0') {
        return AXL_ERR;
    }

    uint64_t off_val = 0;
    if (!axl_str_reader_take_u64(&r, 0, &off_val)) { return AXL_ERR; }
    if (!axl_str_reader_eof(&r))                   { return AXL_ERR; }

    /* Sum, with overflow check. */
    if (off_val > UINT64_MAX - base_val) { return AXL_ERR; }
    *out = base_val + off_val;
    return AXL_OK;
}

// ===========================================================================
// AxlStrReader — cursor-based string parser
// ===========================================================================

void
axl_str_reader_init(
    AxlStrReader  *r,
    const char    *s
    )
{
    if (r == NULL) { return; }
    if (s == NULL) {
        r->p = r->end = NULL;
    } else {
        r->p = s;
        r->end = s + axl_strlen(s);
    }
    r->ok = true;
}

void
axl_str_reader_init_n(
    AxlStrReader  *r,
    const char    *s,
    size_t         n
    )
{
    if (r == NULL) { return; }
    if (s == NULL) {
        r->p = r->end = NULL;
        /* NULL pointer with non-zero length is a programming error.
         * Mark not-ok so subsequent ops short-circuit cleanly. */
        r->ok = (n == 0);
        return;
    }
    r->p = s;
    r->end = s + n;
    r->ok = true;
}

bool
axl_str_reader_eof(
    const AxlStrReader *r
    )
{
    if (r == NULL) { return true; }
    return r->p >= r->end;
}

size_t
axl_str_reader_remaining(
    const AxlStrReader *r
    )
{
    if (r == NULL || r->p >= r->end) { return 0; }
    return (size_t)(r->end - r->p);
}

char
axl_str_reader_peek(
    const AxlStrReader *r
    )
{
    if (r == NULL || !r->ok || r->p >= r->end) { return '\0'; }
    return *r->p;
}

bool
axl_str_reader_skip_ws(
    AxlStrReader *r
    )
{
    if (r == NULL || !r->ok) { return false; }
    while (r->p < r->end) {
        char c = *r->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n'
            || c == '\f' || c == '\v')
        {
            r->p++;
        } else {
            break;
        }
    }
    return true;
}

bool
axl_str_reader_consume_char(
    AxlStrReader  *r,
    char           c
    )
{
    if (r == NULL || !r->ok) { return false; }
    if (r->p >= r->end || *r->p != c) {
        r->ok = false;
        return false;
    }
    r->p++;
    return true;
}

bool
axl_str_reader_consume_str(
    AxlStrReader  *r,
    const char    *literal
    )
{
    if (r == NULL || !r->ok) { return false; }
    if (literal == NULL || *literal == '\0') { return true; }
    size_t lit_len = axl_strlen(literal);
    if ((size_t)(r->end - r->p) < lit_len) {
        r->ok = false;
        return false;
    }
    for (size_t i = 0; i < lit_len; i++) {
        if (r->p[i] != literal[i]) {
            r->ok = false;
            return false;
        }
    }
    r->p += lit_len;
    return true;
}

bool
axl_str_reader_take_until(
    AxlStrReader   *r,
    char            delim,
    const char    **out,
    size_t         *out_len
    )
{
    if (r == NULL || !r->ok) { return false; }
    const char *start = r->p;
    while (r->p < r->end && *r->p != delim) {
        r->p++;
    }
    if (r->p >= r->end) {
        /* Delim not found before EOF — leave cursor at start, mark not ok. */
        r->p = start;
        r->ok = false;
        return false;
    }
    if (out != NULL)     { *out     = start; }
    if (out_len != NULL) { *out_len = (size_t)(r->p - start); }
    r->p++;   /* consume the delimiter */
    return true;
}

bool
axl_str_reader_take_while(
    AxlStrReader   *r,
    bool          (*pred)(char),
    const char    **out,
    size_t         *out_len
    )
{
    if (r == NULL || !r->ok || pred == NULL) {
        if (r != NULL && pred == NULL) { r->ok = false; }
        return false;
    }
    const char *start = r->p;
    while (r->p < r->end && pred(*r->p)) {
        r->p++;
    }
    if (out != NULL)     { *out     = start; }
    if (out_len != NULL) { *out_len = (size_t)(r->p - start); }
    return true;
}

bool
axl_str_reader_take_u64(
    AxlStrReader  *r,
    int            base,
    uint64_t      *out
    )
{
    if (r == NULL || !r->ok || out == NULL) {
        if (r != NULL) { r->ok = false; }
        return false;
    }
    if (r->p >= r->end) {
        r->ok = false;
        return false;
    }
    /* axl_str_to_u64 expects a NUL-terminated input. The reader's input
     * may not be (slice into a larger buffer), but `endptr` lets us walk
     * up to but not past `end`. We rely on `endptr` to tell us where
     * parsing stopped, then advance the cursor accordingly. To handle
     * the non-NUL-terminated case safely, we copy a digit run into a
     * stack buffer first.
     *
     * Buffer size 80: comfortably above the longest u64 literal in
     * any supported base. 64 binary digits + "0b" prefix would be 66
     * but we don't accept binary prefixes; the practical worst case
     * is a base-2 explicit-base parse of 64 raw digits. Decimal
     * overflows at 20, octal at 22, hex at 16 (+2 for the 0x prefix).
     *
     * Zero-init via designated initializer so the static analyzer can
     * prove every byte is defined — axl_str_to_u64 will stop at the
     * first NUL anyway, but the analyzer doesn't know that. */
    char     tmp[80] = {0};
    size_t   max = sizeof(tmp) - 1;
    size_t   avail = (size_t)(r->end - r->p);
    if (avail > max) { avail = max; }

    /* Copy candidate digit run, including optional 0x prefix and any
     * leading sign axl_str_to_u64 tolerates. The function itself
     * stops at the first non-digit, so over-copying is harmless. */
    for (size_t i = 0; i < avail; i++) {
        tmp[i] = r->p[i];
    }
    tmp[avail] = '\0';

    uint64_t     v;
    const char  *endptr = NULL;
    if (axl_str_to_u64(tmp, base, &v, &endptr) != AXL_OK || endptr == tmp) {
        r->ok = false;
        return false;
    }
    /* endptr - tmp = bytes consumed; advance the reader by the same. */
    size_t consumed = (size_t)(endptr - tmp);
    r->p += consumed;
    *out = v;
    return true;
}

static bool
is_ident_start(char c)
{
    return axl_isalpha((unsigned char)c) || c == '_';
}

static bool
is_ident_cont(char c)
{
    return is_ident_start(c) || axl_isdigit((unsigned char)c);
}

bool
axl_str_reader_take_ident(
    AxlStrReader   *r,
    const char    **out,
    size_t         *out_len
    )
{
    if (r == NULL || !r->ok) { return false; }
    if (r->p >= r->end || !is_ident_start(*r->p)) {
        r->ok = false;
        return false;
    }
    const char *start = r->p;
    r->p++;
    while (r->p < r->end && is_ident_cont(*r->p)) {
        r->p++;
    }
    if (out != NULL)     { *out     = start; }
    if (out_len != NULL) { *out_len = (size_t)(r->p - start); }
    return true;
}

// ===========================================================================
// axl_sscanf / axl_vsscanf
//
// Dogfoods the cursor parser above. Supports a useful subset of C99
// sscanf — see the header for the conversion list.
// ===========================================================================

/* Width modifiers parsed from a conversion specifier. */
typedef enum {
    SCAN_LEN_DEFAULT,    /* int / unsigned int / etc. */
    SCAN_LEN_CHAR,       /* %hh */
    SCAN_LEN_SHORT,      /* %h  */
    SCAN_LEN_LONG,       /* %l  */
    SCAN_LEN_LONG_LONG,  /* %ll */
    SCAN_LEN_SIZE_T,     /* %z  */
    SCAN_LEN_INTMAX,     /* %j  */
} ScanLen;

/* Store helpers are macros (rather than functions taking `va_list *`)
 * because `va_list` is an array type on x86_64 — passing &ap doesn't
 * decay the way the caller expects, and clang flags the pointer
 * mismatch. Macros keep `va_arg(ap, ...)` in the caller's scope. */
#define SCAN_STORE_UNSIGNED(v, len, ap) do {                                  \
    uint64_t _val = (uint64_t)(v);                                            \
    switch (len) {                                                            \
        case SCAN_LEN_CHAR:      *va_arg(ap, unsigned char *)      = (unsigned char)_val;  break;  \
        case SCAN_LEN_SHORT:     *va_arg(ap, unsigned short *)     = (unsigned short)_val; break; \
        case SCAN_LEN_LONG:      *va_arg(ap, unsigned long *)      = (unsigned long)_val;  break; \
        case SCAN_LEN_LONG_LONG: *va_arg(ap, unsigned long long *) = (unsigned long long)_val; break; \
        case SCAN_LEN_SIZE_T:    *va_arg(ap, size_t *)             = (size_t)_val;         break; \
        case SCAN_LEN_INTMAX:    *va_arg(ap, uint64_t *)           = _val;                 break; \
        case SCAN_LEN_DEFAULT:   *va_arg(ap, unsigned int *)       = (unsigned int)_val;   break; \
    }                                                                         \
} while (0)

#define SCAN_STORE_SIGNED(v, len, ap) do {                                    \
    int64_t _val = (int64_t)(v);                                              \
    switch (len) {                                                            \
        case SCAN_LEN_CHAR:      *va_arg(ap, signed char *)      = (signed char)_val;      break; \
        case SCAN_LEN_SHORT:     *va_arg(ap, short *)            = (short)_val;            break; \
        case SCAN_LEN_LONG:      *va_arg(ap, long *)             = (long)_val;             break; \
        case SCAN_LEN_LONG_LONG: *va_arg(ap, long long *)        = (long long)_val;        break; \
        case SCAN_LEN_SIZE_T:    *va_arg(ap, size_t *)           = (size_t)_val;           break; \
        case SCAN_LEN_INTMAX:    *va_arg(ap, int64_t *)          = _val;                   break; \
        case SCAN_LEN_DEFAULT:   *va_arg(ap, int *)              = (int)_val;              break; \
    }                                                                         \
} while (0)

/* Match a single character class spec '[...]'. @a fmt points at the
 * char AFTER '['. On success advances @a *fmt to the char AFTER ']'.
 * On malformed spec, returns NULL fmt unchanged (caller treats as
 * format error). The returned 256-byte table[c] is true if c is in
 * the set.
 *
 * Supports: leading '^' for negation, ']' as first char to include
 * literal ']', and ranges 'a-z'. */
static int
scan_parse_charset(const char **fmt, bool table[256])
{
    const char *p = *fmt;
    bool negate = false;
    if (*p == '^') {
        negate = true;
        p++;
    }
    for (size_t i = 0; i < 256; i++) { table[i] = false; }

    /* ']' as first char (after '^' if any) is literal. */
    bool first = true;
    while (*p != '\0' && (first || *p != ']')) {
        first = false;
        unsigned char c = (unsigned char)*p;
        /* Range: x-y where y > '-' and the char after isn't ']'. */
        if (p[1] == '-' && p[2] != ']' && p[2] != '\0' && (unsigned char)p[2] >= c) {
            unsigned char to = (unsigned char)p[2];
            for (unsigned int x = c; x <= to; x++) { table[x] = true; }
            p += 3;
        } else {
            table[c] = true;
            p++;
        }
    }
    if (*p != ']') { return -1; }   /* unterminated set */
    p++;
    if (negate) {
        for (size_t i = 0; i < 256; i++) { table[i] = !table[i]; }
    }
    *fmt = p;
    return 0;
}

int
axl_vsscanf(
    const char  *str,
    const char  *fmt,
    va_list      ap
    )
{
    if (str == NULL || fmt == NULL) { return -1; }
    AxlStrReader r;
    axl_str_reader_init(&r, str);
    int conversions = 0;
    const char *f = fmt;

    while (*f != '\0') {
        char fc = *f;

        /* Whitespace in fmt matches any run of input whitespace. */
        if (fc == ' ' || fc == '\t' || fc == '\n' || fc == '\r') {
            axl_str_reader_skip_ws(&r);
            f++;
            continue;
        }

        /* Literal char in fmt must match input exactly. */
        if (fc != '%') {
            if (r.p >= r.end || *r.p != fc) { return conversions; }
            r.p++;
            f++;
            continue;
        }

        /* '%' — parse a conversion spec. */
        f++;

        /* Optional '*' suppresses assignment. */
        bool suppress = false;
        if (*f == '*') { suppress = true; f++; }

        /* Optional max-width. */
        size_t max_width = 0;
        bool   have_width = false;
        while (*f >= '0' && *f <= '9') {
            max_width = max_width * 10 + (size_t)(*f - '0');
            have_width = true;
            f++;
        }

        /* Length modifier. */
        ScanLen len = SCAN_LEN_DEFAULT;
        if (*f == 'h' && f[1] == 'h')      { len = SCAN_LEN_CHAR;      f += 2; }
        else if (*f == 'h')                { len = SCAN_LEN_SHORT;     f += 1; }
        else if (*f == 'l' && f[1] == 'l') { len = SCAN_LEN_LONG_LONG; f += 2; }
        else if (*f == 'l')                { len = SCAN_LEN_LONG;      f += 1; }
        else if (*f == 'z')                { len = SCAN_LEN_SIZE_T;    f += 1; }
        else if (*f == 'j')                { len = SCAN_LEN_INTMAX;    f += 1; }

        char conv = *f;
        if (conv == '\0') { return -1; }   /* malformed: trailing '%'... */
        f++;

        switch (conv) {
            case '%': {
                /* Literal '%' — must match input. */
                if (r.p >= r.end || *r.p != '%') { return conversions; }
                r.p++;
                break;
            }

            case 'c': {
                /* %c: read N chars (default 1). No leading-ws skip. */
                size_t n = have_width ? max_width : 1;
                if ((size_t)(r.end - r.p) < n) { return conversions; }
                if (!suppress) {
                    char *dst = va_arg(ap, char *);
                    for (size_t i = 0; i < n; i++) { dst[i] = r.p[i]; }
                }
                r.p += n;
                if (!suppress) { conversions++; }
                break;
            }

            case 's': {
                /* %s: skip leading ws, then a run of non-ws. Width is
                 * required for unsuppressed %s so the destination buffer
                 * is bounded — without it, a format-string typo can write
                 * past the caller's buffer. Suppressed (%*s) doesn't write
                 * anywhere, so it's safe without a width. */
                if (!have_width && !suppress) { return -1; }
                axl_str_reader_skip_ws(&r);
                if (r.p >= r.end) { return conversions; }
                const char *start = r.p;
                while (r.p < r.end
                       && (!have_width || (size_t)(r.p - start) < max_width)
                       && *r.p != ' '  && *r.p != '\t' && *r.p != '\n'
                       && *r.p != '\r' && *r.p != '\f' && *r.p != '\v')
                {
                    r.p++;
                }
                size_t n = (size_t)(r.p - start);
                if (n == 0) { return conversions; }
                if (!suppress) {
                    char *dst = va_arg(ap, char *);
                    for (size_t i = 0; i < n; i++) { dst[i] = start[i]; }
                    dst[n] = '\0';
                    conversions++;
                }
                break;
            }

            case '[': {
                /* %[set] / %[^set] — width required. */
                if (!have_width) { return -1; }
                bool table[256];
                if (scan_parse_charset(&f, table) != 0) { return -1; }
                const char *start = r.p;
                while (r.p < r.end
                       && (size_t)(r.p - start) < max_width
                       && table[(unsigned char)*r.p])
                {
                    r.p++;
                }
                size_t n = (size_t)(r.p - start);
                if (n == 0) { return conversions; }
                if (!suppress) {
                    char *dst = va_arg(ap, char *);
                    for (size_t i = 0; i < n; i++) { dst[i] = start[i]; }
                    dst[n] = '\0';
                    conversions++;
                }
                break;
            }

            case 'd': case 'i': case 'u':
            case 'o': case 'x': case 'X': {
                /* Numeric: leading-ws skip, optional sign for d/i,
                 * optional 0x for i/x. Use the cursor-take_u64 helper
                 * (which uses axl_str_to_u64) for the heavy lifting. */
                axl_str_reader_skip_ws(&r);
                if (r.p >= r.end) { return conversions; }
                bool negative = false;
                bool is_signed = (conv == 'd' || conv == 'i');
                if (is_signed && (*r.p == '+' || *r.p == '-')) {
                    negative = (*r.p == '-');
                    r.p++;
                }
                int base;
                switch (conv) {
                    case 'd': base = 10; break;
                    case 'i': base = 0;  break;   /* auto-detect */
                    case 'u': base = 10; break;
                    case 'o': base = 8;  break;
                    case 'x': case 'X': base = 16; break;
                    default:  base = 10; break;   /* unreachable */
                }
                /* Apply max_width by clamping the reader's view. */
                AxlStrReader sub = r;
                if (have_width
                    && (size_t)(sub.end - sub.p) > max_width)
                {
                    sub.end = sub.p + max_width;
                }
                uint64_t v;
                if (!axl_str_reader_take_u64(&sub, base, &v)) {
                    return conversions;
                }
                /* sub started as a copy of r and only moved forward
                 * within the (possibly width-clamped) view. */
                r.p = sub.p;
                if (suppress) { break; }
                if (is_signed) {
                    int64_t sv;
                    if (negative) {
                        /* INT64_MIN is -INT64_MAX-1, which corresponds
                         * to the unsigned magnitude (uint64_t)INT64_MAX+1
                         * = 0x8000000000000000. Computing -(int64_t)v
                         * for that magnitude is signed-int overflow
                         * (UB per C99 §6.5/5) — even though gcc/clang
                         * emit `neg` and yield INT64_MIN today, a future
                         * optimizer is free to assume it can't happen
                         * and UBSan trips. Special-case the boundary
                         * and use unsigned arithmetic for the rest. */
                        if (v > (uint64_t)INT64_MAX + 1) { return conversions; }
                        sv = (v == (uint64_t)INT64_MAX + 1)
                             ? INT64_MIN
                             : -(int64_t)v;
                    } else {
                        if (v > (uint64_t)INT64_MAX) { return conversions; }
                        sv = (int64_t)v;
                    }
                    SCAN_STORE_SIGNED(sv, len, ap);
                } else {
                    SCAN_STORE_UNSIGNED(v, len, ap);
                }
                conversions++;
                break;
            }

            case 'n': {
                /* %n — assigns # bytes consumed so far. Doesn't count
                 * as a conversion. */
                if (!suppress) {
                    int *dst = va_arg(ap, int *);
                    *dst = (int)(r.p - str);
                }
                break;
            }

            default:
                /* Unsupported / malformed conversion. */
                return -1;
        }
    }
    return conversions;
}

int
axl_sscanf(
    const char  *str,
    const char  *fmt,
    ...
    )
{
    va_list ap;
    va_start(ap, fmt);
    int rc = axl_vsscanf(str, fmt, ap);
    va_end(ap);
    return rc;
}
