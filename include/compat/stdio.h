/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Minimal stdio.h for freestanding UEFI */
#ifndef AXL_COMPAT_STDIO_H
#define AXL_COMPAT_STDIO_H

#include <stddef.h>
#include <stdarg.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef void FILE;

int snprintf(char *buf, size_t size, const char *fmt, ...);
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);

#define stderr ((FILE *)0)
#define stdout ((FILE *)0)

#ifdef __cplusplus
}
#endif

#endif
