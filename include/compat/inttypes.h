/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Minimal inttypes.h for freestanding UEFI — format macros only */
#ifndef AXL_COMPAT_INTTYPES_H
#define AXL_COMPAT_INTTYPES_H

#include <stdint.h>

#define PRId32  "d"
#define PRId64  "lld"
#define PRIu32  "u"
#define PRIu64  "llu"
#define PRIx32  "x"
#define PRIx64  "llx"
#define PRIX32  "X"
#define PRIX64  "llX"

#endif
