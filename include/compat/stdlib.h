/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Minimal stdlib.h for freestanding UEFI */
#ifndef AXL_COMPAT_STDLIB_H
#define AXL_COMPAT_STDLIB_H

#include <stddef.h>

void *calloc(size_t n, size_t size);
void  free(void *ptr);

#endif
