/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-string.c
    Wide-string (UCS-2) utilities.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "../backend/axl-backend.h"
#include <axl/axl-str.h>

// ===========================================================================
// UCS-2 primitive operations
// ===========================================================================

size_t
axl_wcslen(const unsigned short *s)
{
    if (s == NULL) {
        return 0;
    }
    return axl_backend_wcslen(s);
}

int
axl_wcscmp(const unsigned short *a, const unsigned short *b)
{
    if (a == b) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    return axl_backend_wcscmp(a, b);
}

void
axl_wcscpy(unsigned short *dst, const unsigned short *src, size_t dst_count)
{
    if (dst == NULL || src == NULL || dst_count == 0) {
        return;
    }
    axl_backend_wcscpy(dst, src, dst_count);
}

