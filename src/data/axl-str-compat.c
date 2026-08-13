/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-str-compat.c
    Standard C function aliases for freestanding UEFI builds.

    Third-party libraries (e.g. mbedTLS) call standard C names.
    These are thin wrappers around AXL functions. Memory intrinsics
    (memcpy, memset, memmove) stay in src/mem/axl-intrinsics.c
    because the compiler generates implicit calls to them and they
    can't depend on AxlDataLib.
**/

#include <axl/axl-str.h>
#include <stddef.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

size_t strlen(const char *s) { return axl_strlen(s); }
int    strcmp(const char *a, const char *b) { return axl_strcmp(a, b); }
int    strncmp(const char *a, const char *b, size_t n) { return axl_strncmp(a, b, n); }
int    memcmp(const void *a, const void *b, size_t n) { return axl_memcmp(a, b, n); }
/* Not one of the four intrinsics gcc assumes freestanding, but libstdc++'s
   char_traits<char>::find calls it out of line -- so every std::string_view
   search (and therefore axl::string's, which forwards to it) needs this. */
void  *memchr(const void *s, int c, size_t n) { return axl_memchr(s, c, n); }
char  *strchr(const char *s, int c) { return axl_strchr(s, c); }
char  *strstr(const char *h, const char *n) { return axl_strstr(h, n); }
char  *strncpy(char *d, const char *s, size_t n) { return axl_strncpy(d, s, n); }

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
