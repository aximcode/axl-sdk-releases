/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-mem-impl.h:
 *
 * Implementation details for axl_malloc/free macros.
 * Do NOT include this directly — include axl/axl-mem.h instead.
 */

#ifndef AXL_MEM_IMPL_H
#define AXL_MEM_IMPL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *axl_malloc_impl(size_t size, const char *file, size_t line);
void  *axl_calloc_impl(size_t count, size_t size, const char *file, size_t line);
void  *axl_realloc_impl(void *ptr, size_t size, const char *file, size_t line);
void   axl_free_impl(void *ptr);
char  *axl_strdup_impl(const char *str, const char *file, size_t line);
void  *axl_memdup_impl(const void *src, size_t size, const char *file, size_t line);

#ifdef __cplusplus
}
#endif

#endif /* AXL_MEM_IMPL_H */
