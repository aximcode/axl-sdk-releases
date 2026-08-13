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
#include <axl/axl-math.h>
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

void *
axl_memchr(const void *s, int c, size_t n)
{
    const unsigned char *p      = (const unsigned char *)s;
    unsigned char        target = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == target) {
            return (void *)(p + i);
        }
    }
    return NULL;
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
// axl_vsnprintf / axl_snprintf — format into fixed buffer via axl_vformat
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
axl_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    SnprintfCtx ctx;

    if (buf == NULL || size == 0) {
        return 0;
    }

    ctx.buf = buf;
    ctx.size = size;
    ctx.written = 0;

    axl_vformat(snprintf_writer, &ctx, fmt, args);

    if (ctx.written < size) {
        buf[ctx.written] = '\0';
    } else {
        buf[size - 1] = '\0';
    }

    return (int)ctx.written;
}

int
axl_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int     n;

    va_start(args, fmt);
    n = axl_vsnprintf(buf, size, fmt, args);
    va_end(args);

    return n;
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
axl_strjoinv(const char *separator, size_t count, const char *const argv[])
{
    if (count > 0 && argv == NULL) {
        return NULL;
    }

    size_t sep_len = (separator != NULL) ? axl_strlen(separator) : 0;

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            total += sep_len;
        }
        total += axl_strlen(argv[i]);
    }

    char *result = axl_malloc(total + 1);
    if (result == NULL) {
        axl_error("failed to allocate strjoin result");
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && sep_len > 0) {
            axl_memcpy(result + pos, separator, sep_len);
            pos += sep_len;
        }
        size_t slen = axl_strlen(argv[i]);
        axl_memcpy(result + pos, argv[i], slen);
        pos += slen;
    }
    result[pos] = '\0';
    return result;
}

char *
axl_strjoin(const char *separator, const char **arr)
{
    if (arr == NULL) {
        return NULL;
    }
    size_t count = 0;
    while (arr[count] != NULL) {
        count++;
    }
    return axl_strjoinv(separator, count, arr);
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
// UTF-8 iterator
// ---------------------------------------------------------------------------

size_t
axl_utf8_decode(
    const char  *s,
    uint32_t    *out_codepoint
    )
{
    if (s == NULL || out_codepoint == NULL) {
        return 0;
    }

    const uint8_t b0 = (uint8_t)s[0];
    if (b0 == 0) {
        return 0;
    }

    #define UTF8_IS_CONT(b) (((b) & 0xC0) == 0x80)

    /* 1-byte: 0xxxxxxx (U+0000..U+007F) */
    if (b0 < 0x80) {
        *out_codepoint = b0;
        return 1;
    }

    /* 2-byte: 110xxxxx 10xxxxxx (U+0080..U+07FF) */
    if ((b0 & 0xE0) == 0xC0) {
        uint8_t b1 = (uint8_t)s[1];
        if (!UTF8_IS_CONT(b1)) {
            *out_codepoint = 0xFFFD;
            return 1;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        /* Reject overlong encodings (codepoints < 0x80 encoded in 2 bytes). */
        if (cp < 0x80) {
            *out_codepoint = 0xFFFD;
            return 1;
        }
        *out_codepoint = cp;
        return 2;
    }

    /* 3-byte: 1110xxxx 10xxxxxx 10xxxxxx (U+0800..U+FFFF, except surrogates) */
    if ((b0 & 0xF0) == 0xE0) {
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)(b1 ? s[2] : 0);
        if (!UTF8_IS_CONT(b1) || !UTF8_IS_CONT(b2)) {
            *out_codepoint = 0xFFFD;
            return 1;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                      ((uint32_t)(b1 & 0x3F) << 6)  |
                      ((uint32_t)(b2 & 0x3F));
        /* Reject overlong + UTF-16 surrogates (D800..DFFF must never appear). */
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            *out_codepoint = 0xFFFD;
            return 1;
        }
        *out_codepoint = cp;
        return 3;
    }

    /* 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx (U+10000..U+10FFFF) */
    if ((b0 & 0xF8) == 0xF0) {
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)(b1 ? s[2] : 0);
        uint8_t b3 = (uint8_t)(b2 ? s[3] : 0);
        if (!UTF8_IS_CONT(b1) || !UTF8_IS_CONT(b2) || !UTF8_IS_CONT(b3)) {
            *out_codepoint = 0xFFFD;
            return 1;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) |
                      ((uint32_t)(b1 & 0x3F) << 12) |
                      ((uint32_t)(b2 & 0x3F) << 6)  |
                      ((uint32_t)(b3 & 0x3F));
        /* Reject overlong + out-of-range (Unicode max is U+10FFFF). */
        if (cp < 0x10000 || cp > 0x10FFFF) {
            *out_codepoint = 0xFFFD;
            return 1;
        }
        *out_codepoint = cp;
        return 4;
    }

    /* Invalid lead byte (0xF8..0xFF, or orphan continuation 0x80..0xBF). */
    *out_codepoint = 0xFFFD;
    return 1;

    #undef UTF8_IS_CONT
}

size_t
axl_utf8_encode(
    uint32_t   codepoint,
    char      *dst,
    size_t     dst_size
    )
{
    size_t need;

    /* A UTF-16 surrogate is not a Unicode scalar value and nothing above
       U+10FFFF has an encoding at all. Both are refused rather than emitted
       as the CESU-8 / WTF-8 and 5-byte forms the ladder below would happily
       produce — axl_utf8_decode rejects exactly those byte sequences, so
       emitting one would break the round-trip this call is the other half of. */
    if ((codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
        return 0;
    }

    need = (codepoint < 0x80)    ? 1 :
           (codepoint < 0x800)   ? 2 :
           (codepoint < 0x10000) ? 3 : 4;

    if (dst == NULL) {
        return need;        /* sizing pass — dst_size is not consulted */
    }
    if (dst_size < need) {
        /* All or nothing. A partial sequence is ill-formed UTF-8, and the
           caller cannot tell it from a complete one after the fact. */
        return 0;
    }

    switch (need) {
    case 1:
        dst[0] = (char)codepoint;
        break;
    case 2:
        dst[0] = (char)(0xC0 | (codepoint >> 6));
        dst[1] = (char)(0x80 | (codepoint & 0x3F));
        break;
    case 3:
        dst[0] = (char)(0xE0 | (codepoint >> 12));
        dst[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (codepoint & 0x3F));
        break;
    default:
        dst[0] = (char)(0xF0 | (codepoint >> 18));
        dst[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (codepoint & 0x3F));
        break;
    }
    return need;
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
    /* Real UTF-8 → UCS-2 (BMP) decoder — mirrors axl_utf8_to_ucs2's
       allocating cousin. The earlier `(unsigned char)src[i]` cast was
       a Latin-1 shortcut that silently corrupted any non-ASCII name
       it touched (e.g., "résumé.txt" → "r\xC3\xA9sum\xC3\xA9.txt"
       smeared across CHAR16 cells). The fs-provider thunks and any
       code-page-aware filename consumer need the real thing. */
    if (src == NULL || dst == NULL || dst_count == 0) {
        return 0;
    }

    #define IS_CONT(b) (((b) & 0xC0) == 0x80)

    const uint8_t *in = (const uint8_t *)src;
    size_t         out_i = 0;

    /* Always reserve room for the terminating NUL. */
    while (*in != 0 && out_i + 1 < dst_count) {
        uint32_t cp;
        size_t   advance;

        if ((*in & 0x80) == 0) {
            cp = *in;
            advance = 1;
        } else if ((*in & 0xE0) == 0xC0 && IS_CONT(in[1])) {
            cp = ((uint32_t)(in[0] & 0x1F) << 6) |
                 ((uint32_t)(in[1] & 0x3F));
            advance = 2;
        } else if ((*in & 0xF0) == 0xE0 && IS_CONT(in[1]) && IS_CONT(in[2])) {
            cp = ((uint32_t)(in[0] & 0x0F) << 12) |
                 ((uint32_t)(in[1] & 0x3F) << 6) |
                 ((uint32_t)(in[2] & 0x3F));
            advance = 3;
        } else if ((*in & 0xF8) == 0xF0 && IS_CONT(in[1]) &&
                   IS_CONT(in[2]) && IS_CONT(in[3])) {
            /* Above-BMP. UCS-2 can't represent it; skip silently to
               match the allocating cousin's behavior. */
            in += 4;
            continue;
        } else {
            /* Invalid byte — skip, match allocating cousin. */
            in += 1;
            continue;
        }

        dst[out_i++] = (unsigned short)cp;
        in += advance;
    }
    dst[out_i] = 0;

    #undef IS_CONT

    return out_i;
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

size_t
axl_utf16_to_utf8(const uint16_t *src, size_t count, char *dst, size_t dst_size)
{
    if (src == NULL) {
        return 0;
    }
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t cp = src[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {                 /* high surrogate */
            if (i + 1 < count && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (src[i + 1] - 0xDC00u);
                i++;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {          /* lone low surrogate */
            cp = 0xFFFD;
        }
        size_t need = (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
        if (dst != NULL) {
            if (out + need > dst_size) {
                break;                                      /* clean boundary */
            }
            if (cp < 0x80) {
                dst[out++] = (char)cp;
            } else if (cp < 0x800) {
                dst[out++] = (char)(0xC0 | (cp >> 6));
                dst[out++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                dst[out++] = (char)(0xE0 | (cp >> 12));
                dst[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[out++] = (char)(0x80 | (cp & 0x3F));
            } else {
                dst[out++] = (char)(0xF0 | (cp >> 18));
                dst[out++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                dst[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[out++] = (char)(0x80 | (cp & 0x3F));
            }
        } else {
            out += need;
        }
    }
    return out;
}

size_t
axl_utf8_to_utf16(const char *src, size_t len, uint16_t *dst, size_t dst_count)
{
    if (src == NULL) {
        return 0;
    }
    const uint8_t *s = (const uint8_t *)src;
    size_t out = 0, i = 0;
    while (i < len) {
        uint8_t  b0 = s[i];
        uint32_t cp;
        size_t   n;
        if (b0 < 0x80) {
            cp = b0;
            n = 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < len && (s[i + 1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(b0 & 0x1F) << 6) | (s[i + 1] & 0x3F);
            n = 2;
            if (cp < 0x80) { cp = 0xFFFD; }
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < len
                   && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6)
                 | (s[i + 2] & 0x3F);
            n = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) { cp = 0xFFFD; }
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < len
                   && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80
                   && (s[i + 3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12)
                 | ((uint32_t)(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            n = 4;
            if (cp < 0x10000 || cp > 0x10FFFF) { cp = 0xFFFD; }
        } else {
            cp = 0xFFFD;
            n = 1;
        }
        if (cp >= 0x10000) {
            uint16_t hi = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
            if (dst != NULL) {
                if (out + 2 > dst_count) { break; }
                dst[out++] = hi;
                dst[out++] = lo;
            } else {
                out += 2;
            }
        } else {
            if (dst != NULL) {
                if (out + 1 > dst_count) { break; }
                dst[out++] = (uint16_t)cp;
            } else {
                out += 1;
            }
        }
        i += n;
    }
    return out;
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

// ---------------------------------------------------------------------------
// Fixed-buffer emit -- shared by all four *_to_str renderers
// ---------------------------------------------------------------------------

/* Copy the @a src_len characters at @a src into @a buf, writing at most
 * @a bufsz - 1 of them and always NUL-terminating. @a bufsz must be >= 1.
 *
 * This is axl_vsnprintf's convention, which the four public docstrings
 * commit to: the return is the length the WHOLE rendering would have
 * had, so `ret >= bufsz` is the caller's truncation test. One helper for
 * all four keeps that promise literally identical across them.
 *
 * @return @a src_len -- the full length, however little of it fit. */
static size_t
emit_result(
    char       *buf,
    size_t      bufsz,
    const char *src,
    size_t      src_len
    )
{
    size_t n = (src_len < bufsz - 1) ? src_len : bufsz - 1;

    axl_memcpy(buf, src, n);
    buf[n] = '\0';
    return src_len;
}

// ---------------------------------------------------------------------------
// Integer -> string (round-trip pair for axl_str_to_u64 / axl_str_to_s64)
// ---------------------------------------------------------------------------

/* Render @a value in @a base BACKWARDS into @a out, filling down from
 * out[cap - 1] and writing no NUL; the text is out[return .. cap).
 * Filling backwards is the natural direction (digits come out least
 * significant first) and lets axl_s64_to_str prepend its sign without a
 * second pass. @a base must be 2..36. @a cap must be at least 64 for
 * the DIGITS -- base 2, the widest spelling, is exactly 64 digits for a
 * 64-bit value -- and at least 65 for a caller that prepends a sign,
 * because axl_s64_to_str does that with `out[--pos]` on the returned
 * index: at cap == 64 with INT64_MIN in base 2 that index is 0 and the
 * decrement wraps to SIZE_MAX. Callers pass AXL_U64_STR_MAX (65) and
 * AXL_S64_STR_MAX (66), so both hold with a byte to spare; this comment
 * is the only guard, so keep it true if a third caller appears.
 *
 * Deliberately not shared with format_uint() in axl-format.c: that one
 * lives in the zero-dependency AxlFormatLib (which exists to break the
 * Log -> Data cycle), has a 16-entry digit table because %x/%u are its
 * only bases, and does not NUL-terminate.
 *
 * @return index of the first character written. */
static size_t
u64_digits_rev(
    uint64_t  value,
    int       base,
    char     *out,
    size_t    cap
    )
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    size_t pos = cap;

    /* do/while so 0 renders as "0" rather than nothing. */
    do {
        out[--pos] = digits[value % (uint64_t)base];
        value /= (uint64_t)base;
    } while (value != 0);

    return pos;
}

size_t
axl_u64_to_str(
    uint64_t  value,
    int       base,
    char     *buf,
    size_t    bufsz
    )
{
    char   tmp[AXL_U64_STR_MAX];
    size_t pos;

    if (buf == NULL || bufsz == 0) {
        return 0;
    }
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return 0;
    }

    pos = u64_digits_rev(value, base, tmp, sizeof(tmp));
    return emit_result(buf, bufsz, tmp + pos, sizeof(tmp) - pos);
}

size_t
axl_s64_to_str(
    int64_t   value,
    int       base,
    char     *buf,
    size_t    bufsz
    )
{
    char     tmp[AXL_S64_STR_MAX];
    size_t   pos;
    uint64_t mag;

    if (buf == NULL || bufsz == 0) {
        return 0;
    }
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return 0;
    }

    if (value < 0) {
        /* Two steps so INT64_MIN survives: -(value + 1) is in range for
         * every negative int64_t, and the missing unit is added back
         * after the conversion to unsigned. Negating INT64_MIN as a
         * signed value is undefined. */
        mag = (uint64_t)(-(value + 1)) + 1u;
    } else {
        mag = (uint64_t)value;
    }

    pos = u64_digits_rev(mag, base, tmp, sizeof(tmp));
    if (value < 0) {
        tmp[--pos] = '-';
    }
    return emit_result(buf, bufsz, tmp + pos, sizeof(tmp) - pos);
}

// ---------------------------------------------------------------------------
// Double / float -> string (round-trip pair for axl_str_to_double)
// ---------------------------------------------------------------------------

/* Assemble @a digits (@a ndig significant digits, decimal point at
 * @a decpt per the axl_dtoa convention, negative if @a neg) into @a out
 * following the %g-style rule: exponential when the decimal exponent is
 * < -4 or >= 17, fixed otherwise. @a out must have room for the
 * AXL_DOUBLE_STR_MAX worst case; the caller owns NUL-termination.
 *
 * @return number of bytes written to @a out. */
static size_t
assemble_decimal(
    char       *out,
    const char *digits,
    int         ndig,
    int         decpt,
    int         neg
    )
{
    size_t pos = 0;
    int    exp = decpt - 1;
    int    i;

    if (neg) {
        out[pos++] = '-';
    }

    if (exp < -4 || exp >= 17) {
        /* Scientific: d[.ddd...]e+NN, minimum 2 exponent digits. */
        int  aexp = (exp < 0) ? -exp : exp;
        char edig[3];
        int  elen;

        out[pos++] = digits[0];
        if (ndig > 1) {
            out[pos++] = '.';
            axl_memcpy(out + pos, digits + 1, (size_t)(ndig - 1));
            pos += (size_t)(ndig - 1);
        }
        out[pos++] = 'e';
        out[pos++] = (exp < 0) ? '-' : '+';

        if (aexp >= 100) {
            edig[0] = (char)('0' + aexp / 100);
            edig[1] = (char)('0' + (aexp / 10) % 10);
            edig[2] = (char)('0' + aexp % 10);
            elen = 3;
        } else {
            edig[0] = (char)('0' + aexp / 10);
            edig[1] = (char)('0' + aexp % 10);
            elen = 2;
        }
        axl_memcpy(out + pos, edig, (size_t)elen);
        pos += (size_t)elen;
    } else if (decpt <= 0) {
        /* 0.00...digits, -decpt leading fractional zeros. */
        out[pos++] = '0';
        out[pos++] = '.';
        for (i = 0; i < -decpt; i++) {
            out[pos++] = '0';
        }
        axl_memcpy(out + pos, digits, (size_t)ndig);
        pos += (size_t)ndig;
    } else if (decpt < ndig) {
        /* digits split by the point: decpt whole digits, then the rest. */
        axl_memcpy(out + pos, digits, (size_t)decpt);
        pos += (size_t)decpt;
        out[pos++] = '.';
        axl_memcpy(out + pos, digits + decpt, (size_t)(ndig - decpt));
        pos += (size_t)(ndig - decpt);
    } else {
        /* Integer: all digits, then trailing zeros, no decimal point. */
        axl_memcpy(out + pos, digits, (size_t)ndig);
        pos += (size_t)ndig;
        for (i = 0; i < decpt - ndig; i++) {
            out[pos++] = '0';
        }
    }

    return pos;
}

/* The shared numeric engine behind both axl_double_to_str and the
 * non-finite path of axl_float_to_str: nan/inf handling + axl_dtoa +
 * assemble_decimal. @a out must have room for AXL_DOUBLE_STR_MAX.
 *
 * @return number of bytes written to @a out. */
static size_t
double_to_str_core(
    double  value,
    char   *out
    )
{
    char digits[AXL_DTOA_BUF_MIN];
    int  decpt;
    int  neg;
    int  ndig;

    if (axl_isnan(value)) {
        axl_memcpy(out, "nan", 3);
        return 3;
    }

    if (axl_isinf(value)) {
        size_t pos = 0;
        if (value < 0.0) { out[pos++] = '-'; }
        axl_memcpy(out + pos, "inf", 3);
        return pos + 3;
    }

    decpt = 1;
    neg = 0;
    ndig = axl_dtoa(value, digits, sizeof(digits), &decpt, &neg);
    if (ndig <= 0) {
        digits[0] = '0';
        digits[1] = '\0';
        ndig = 1;
        decpt = 1;
        neg = 0;
    }

    return assemble_decimal(out, digits, ndig, decpt, neg);
}

size_t
axl_double_to_str(
    double  value,
    char   *buf,
    size_t  bufsz
    )
{
    char tmp[AXL_DOUBLE_STR_MAX];
    size_t len;

    if (buf == NULL || bufsz == 0) {
        return 0;
    }

    len = double_to_str_core(value, tmp);
    return emit_result(buf, bufsz, tmp, len);
}

/* Round the @a *ndig significant digits in @a d (decimal point at
 * @a *decpt, per the axl_dtoa convention) to @a want significant digits,
 * round-half-to-even, then strip any trailing zeros the rounding
 * created. @a want must be in [1, *ndig). Updates @a *ndig / @a *decpt
 * in place; @a d must have room for one extra digit (a carry out of the
 * leading digit, e.g. 999 -> 1000). */
static void
round_sig_digits(
    char *d,
    int  *ndig,
    int  *decpt,
    int   want
    )
{
    int  orig_ndig = *ndig;
    bool round_up;
    char next = d[want];
    int  n, i;

    if (next > '5') {
        round_up = true;
    } else if (next < '5') {
        round_up = false;
    } else {
        bool exactly_half = true;
        for (i = want + 1; i < orig_ndig; i++) {
            if (d[i] != '0') { exactly_half = false; break; }
        }
        round_up = exactly_half ? (((d[want - 1] - '0') % 2) != 0) : true;
    }

    n = want;
    if (round_up) {
        i = n - 1;
        while (i >= 0 && d[i] == '9') {
            d[i] = '0';
            i--;
        }
        if (i >= 0) {
            d[i]++;
        } else {
            /* Carried out of the leading digit: shift right, prepend '1'. */
            int j;
            for (j = n; j > 0; j--) { d[j] = d[j - 1]; }
            d[0] = '1';
            n++;
            (*decpt)++;
        }
    }

    /* Strip trailing zeros the rounding produced (canonical shortest form). */
    while (n > 1 && d[n - 1] == '0') {
        n--;
    }
    d[n] = '\0';
    *ndig = n;
}

size_t
axl_float_to_str(
    float   value,
    char   *buf,
    size_t  bufsz
    )
{
    double d = (double)value;
    char   tmp[AXL_DOUBLE_STR_MAX];
    size_t len;

    if (buf == NULL || bufsz == 0) {
        return 0;
    }

    if (axl_isnan(d) || axl_isinf(d)) {
        /* Non-finite spellings are width-independent, so the double
         * engine's answer is already the float's. Assign rather than
         * return so both paths leave through the single emit below --
         * one exit, one truncation convention. */
        len = double_to_str_core(d, tmp);
    } else {
        /* Shortest text that round-trips through axl_str_to_double + a
         * (float) cast: search increasing significant-digit counts,
         * correctly rounded at each length, and stop at the first that
         * reproduces @a value bit-for-bit. 9 (FLT_DECIMAL_DIG) always
         * suffices for IEEE-754 binary32, so the loop is bounded there
         * even when the double's own shortest form needs more. */
        char digits[AXL_DTOA_BUF_MIN];
        int  decpt = 1;
        int  neg = 0;
        int  ndig = axl_dtoa(d, digits, sizeof(digits), &decpt, &neg);
        char best[AXL_DTOA_BUF_MIN];
        int  best_ndig, best_decpt;
        int  want;

        if (ndig <= 0) {
            digits[0] = '0';
            digits[1] = '\0';
            ndig = 1;
            decpt = 1;
            neg = 0;
        }

        best_ndig = ndig;
        best_decpt = decpt;
        axl_memcpy(best, digits, (size_t)ndig + 1);

        for (want = 1; want < ndig && want <= 9; want++) {
            char   cand[AXL_DTOA_BUF_MIN];
            int    cand_ndig = ndig;
            int    cand_decpt = decpt;
            char   probe[AXL_DOUBLE_STR_MAX];
            size_t probe_len;
            double back;

            axl_memcpy(cand, digits, (size_t)ndig + 1);
            round_sig_digits(cand, &cand_ndig, &cand_decpt, want);

            probe_len = assemble_decimal(probe, cand, cand_ndig, cand_decpt, neg);
            probe[probe_len] = '\0';

            if (axl_str_to_double(probe, &back, NULL) == AXL_OK
                && (float)back == value)
            {
                best_ndig = cand_ndig;
                best_decpt = cand_decpt;
                axl_memcpy(best, cand, (size_t)cand_ndig + 1);
                break;
            }
        }

        len = assemble_decimal(tmp, best, best_ndig, best_decpt, neg);
    }

    return emit_result(buf, bufsz, tmp, len);
}
