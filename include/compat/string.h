/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Minimal string.h for freestanding UEFI builds.
   Functions are implemented in src/mem/axl-intrinsics.c. */
#ifndef AXL_COMPAT_STRING_H
#define AXL_COMPAT_STRING_H

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

#endif
