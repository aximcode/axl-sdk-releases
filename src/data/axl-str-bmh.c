/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-str-bmh.c
    Boyer-Moore-Horspool substring search backing
    `axl_strstr` / `axl_strstr_len` / `axl_strrstr` /
    `axl_strrstr_len` / `axl_strcasestr` / `axl_strcasestr_len`.

    Below `BMH_THRESHOLD` the per-search cost of building the
    skip table dominates the win, so the public wrappers fall
    back to a naive byte-walk in that range.

    Split out of axl-str.c per docs/Style-Cleanup-Plan.md Pass C.
**/

#include <axl/axl-mem.h>
#include <axl/axl-str.h>

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
axl_strstr(const char *haystack, const char *needle)
{
    return axl_strstr_len(haystack, -1, needle);
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
