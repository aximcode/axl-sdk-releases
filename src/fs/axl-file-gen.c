/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-file-gen.c
    The in-image file-generation registry. See axl-file-gen.h for the
    keying rationale, the bounds and the cross-image ceiling.

    Deliberately dependency-free — no allocator, no logging, no string
    module. It is called from AxlLogLib's file handler, which cannot use
    axl_malloc (circular dependency), and from the stream write vtable,
    where a bump must not be able to fail.
**/

#include "axl-file-gen.h"

/* One counter per slot. BSS; zero-initialised, which is a perfectly good
   starting generation (a reader records whatever it sees at open). */
static uint32_t mGen[AXL_FILE_GEN_SLOTS];

uint32_t
axl_file_gen_key(const char *path)
{
    if (path == NULL) {
        return 0;
    }

    size_t end = 0;
    while (path[end] != '\0') {
        end++;
    }
    /* Trailing separators are not part of the name: "fs0:\dir\" and
       "fs0:\dir" must key the same. */
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
        end--;
    }
    /* Back up to the start of the final component. ':' terminates too, so
       "fs0:file" yields "file" the way "fs0:\file" does. */
    size_t start = end;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\'
           && path[start - 1] != ':') {
        start--;
    }

    /* FNV-1a over the ASCII-case-folded component. Folding only A-Z is
       exactly FAT's rule for the 8.3/LFN names these paths carry; a
       case-sensitive provider that does distinguish them merely shares a
       slot, which over-invalidates and stays safe. */
    uint32_t h = 2166136261u;
    for (size_t i = start; i < end; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        h ^= (uint32_t)c;
        h *= 16777619u;
    }
    return h;
}

uint32_t
axl_file_gen_read(uint32_t key)
{
    return mGen[key & (AXL_FILE_GEN_SLOTS - 1u)];
}

void
axl_file_gen_bump_key(uint32_t key)
{
    mGen[key & (AXL_FILE_GEN_SLOTS - 1u)]++;
}

void
axl_file_gen_bump(const char *path)
{
    if (path == NULL) {
        return;
    }
    axl_file_gen_bump_key(axl_file_gen_key(path));
}
