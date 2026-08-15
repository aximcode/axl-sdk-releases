/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Minimal stdlib.h for freestanding UEFI */
#ifndef AXL_COMPAT_STDLIB_H
#define AXL_COMPAT_STDLIB_H

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

void *calloc(size_t n, size_t size);
void  free(void *ptr);
/* malloc / realloc declarations exist solely so that system headers
 * which forward-reference them (e.g. gcc's <mm_malloc.h> on x86)
 * parse cleanly under our compat-shadowed <stdlib.h>.  We don't
 * provide implementations — modules route allocation through
 * `axl_malloc` / `axl_realloc`. */
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif
