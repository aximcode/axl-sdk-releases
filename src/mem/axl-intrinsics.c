/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-intrinsics.c
    Compiler-required memory intrinsics for freestanding UEFI builds.

    Clang may generate implicit calls to memcpy, memset, memmove for
    struct copies, array initialization, and other operations. In
    freestanding builds there is no libc to provide them, so we
    define them here. String functions (strlen, strcmp, etc.) are in
    src/data/axl-str-compat.c where they can call AXL equivalents.
**/

#include <stddef.h>
#include <stdint.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void *
memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *
memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
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

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
