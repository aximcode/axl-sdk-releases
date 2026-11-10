/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-child.c — children for test-heap-release-qemu.sh, one source,
 * two images:
 *
 *   heap-rel-c.efi     calls newlib malloc/free, as any C program may. That
 *                      takes the C heap's arena, which must go back to the
 *                      firmware when the image exits.
 *   heap-rel-leak.efi  (-DHEAP_REL_LEAK) the CONTROL: takes 64 pages and
 *                      never frees them, so the observer must report exactly
 *                      64 per launch. A sampler that reports 0 here cannot
 *                      see a leak, and its 0 for the others means nothing.
 */

#include <stdlib.h>

#include <axl.h>

int
main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;

#ifdef HEAP_REL_LEAK
    uint64_t phys;

    return axl_alloc_pages(64, &phys) == AXL_OK ? 0 : 1;
#else
    /* volatile: the allocation must happen, not be folded away. */
    char *volatile p = malloc(100);

    if (p == NULL) {
        return 1;
    }
    p[0] = 1;
    free(p);
    return 0;
#endif
}
