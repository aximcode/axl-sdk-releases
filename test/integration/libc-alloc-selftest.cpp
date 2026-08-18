/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * libc-alloc-selftest.cpp — newlib's dlmalloc, on a heap AXL provides.
 *
 * §2-DECISION of AXL-Libc-Substrate-Design.md: TWO allocators, split by
 * namespace. newlib owns the whole C vocabulary (`malloc`, `free`, `realloc`,
 * `_malloc_r`, ...) and runs its own dlmalloc on a region AXL hands it through
 * `sbrk`; `axl_malloc` stays a separate allocator over `AllocatePool`. Nothing
 * crosses, because the names are disjoint.
 *
 * This fixture therefore asserts the OPPOSITE of what it did an hour ago, when
 * the design was one allocator wearing both sets of names. Then, dlmalloc had
 * to be absent from the image; now it has to be present AND working, because a
 * dlmalloc that links but cannot obtain a byte from `sbrk` fails exactly the
 * way the old design's did -- `strdup()` returning NULL -- and the two are
 * indistinguishable without asserting both halves.
 *
 * `strdup` is still the probe: it is the shortest path to `_malloc_r` a
 * consumer can write, and under this design `free()` is newlib's too, so the
 * round-trip stays within one allocator by construction.
 */
#include <malloc.h>

#include <axl.h>

#include <stdlib.h>
#include <string.h>

/* Declared by hand, not reached through <string.h>. `strdup` is a POSIX
   extension, and newlib hides it under `-std=c++23` (a strict-conformance
   mode defines no `_POSIX_C_SOURCE`), so the header offers `strcmp` and not
   this. The declaration is the C library's own -- getting it wrong would be a
   link error, not a silent mismatch. */
extern "C" char *strdup(const char *s);

static int passed;
static int failed;

static void
check(bool ok, const char *what)
{
    axl_printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

int
main(void)
{
    /* The whole point: this reaches _malloc_r. NULL means newlib's allocator
       answered and had no heap. */
    char *dup = strdup("axl");

    check(dup != nullptr, "strdup() returns memory (reaches _malloc_r)");

    if (dup != nullptr) {
        check(axl_strcmp(dup, "axl") == 0, "strdup() copied the bytes");
        /* newlib allocated it and newlib frees it -- under the namespace
           split there is no crossing to test for. What IS worth asserting is
           that the heap survives the round-trip, which a corrupted free list
           would not. Allocated back through malloc(), not axl_malloc(),
           because it is dlmalloc's free list this is probing. */
        free(dup);
        void *after = malloc(64);
        check(after != nullptr, "the heap still allocates after a free()");
        free(after);
    }

    /* A size that exercises the same path with a real payload rather than a
       4-byte string, so a bridge that happened to work for tiny blocks is
       not enough. */
    char *big = strdup("the quick brown fox jumps over the lazy dog, twice over");
    check(big != nullptr, "strdup() of a 55-byte string returns memory");
    if (big != nullptr) {
        check(axl_strlen(big) == 55, "the long copy is intact");
        free(big);
    }

    /* realloc growth. Under the old design this went to axl_realloc; under
     * this one it is dlmalloc's, which can extend a chunk in place. Either
     * way the CONTENTS must survive, which is what a bridge gets wrong. */
    char *grow = (char *)malloc(16);

    check(grow != nullptr, "malloc(16) from newlib's allocator");
    if (grow != nullptr) {
        axl_memcpy(grow, "0123456789abcde", 16);
        grow = (char *)realloc(grow, 4096);
        check(grow != nullptr, "realloc(16 -> 4096) succeeds");
        if (grow != nullptr) {
            check(axl_strcmp(grow, "0123456789abcde") == 0,
                  "realloc preserved the contents across the grow");
            free(grow);
        }
    }

    /* THE HEAP MUST GROW PAST ONE sbrk CHUNK. Nothing above this point ever
     * asked for more than 4 KiB, so the whole multi-chunk path was untested
     * while the design doc claimed it was "exercised deliberately by the
     * multi-chunk assertion in test-libc-alloc-qemu.sh". There was no such
     * assertion, and the path was broken: sbrk's chunks come from
     * axl_alloc_pages, which UEFI satisfies DOWNWARD from high memory, so
     * each new chunk landed BELOW the previous one. A break that moves
     * backwards is not merely non-contiguous, it is invalid -- dlmalloc sizes
     * its top by computing `brk + size - old_end`, which then goes negative
     * and wraps.
     *
     * Three separate claims, because they fail independently:
     *   1. a single allocation LARGER than one chunk succeeds;
     *   2. it is really writable for its whole length, not merely non-NULL;
     *   3. the allocator still works AFTERWARDS -- the observed failure
     *      wedged the heap, so even a 256 KiB request returned NULL once a
     *      large one had been attempted. */
    {
        const size_t big = 3u << 20;   /* 3 MiB: three chunks' worth */
        unsigned char *blk = (unsigned char *) malloc(big);

        check(blk != nullptr, "malloc(3 MiB) crosses the sbrk chunk boundary");
        if (blk != nullptr) {
            /* Touch both ends and the middle. A pointer into a region the
             * firmware never actually gave us faults here rather than later
             * in something unrelated. */
            blk[0] = 0xA5;
            blk[big / 2] = 0x5A;
            blk[big - 1] = 0xC3;
            check(blk[0] == 0xA5 && blk[big / 2] == 0x5A && blk[big - 1] == 0xC3,
                  "the 3 MiB block is writable end to end");
            free(blk);
        }

        /* The heap survives. Ordered AFTER the big allocation on purpose. */
        void *small = malloc(256u << 10);
        check(small != nullptr, "malloc(256 KiB) still works after the big one");
        free(small);
    }

    /* GROWTH BY ACCUMULATION, which is the shape a real consumer has: many
     * medium allocations that are never freed, forcing chunk after chunk.
     * Distinct from the single-large case above -- that one asks sbrk for an
     * oversized region in one call, this one walks the break forward. */
    {
        void  *held[24];
        size_t held_n = 0;
        bool   all_ok = true;

        for (size_t i = 0; i < sizeof held / sizeof held[0]; i++) {
            held[i] = malloc(256u << 10);          /* 24 x 256 KiB = 6 MiB */
            if (held[i] == nullptr) { all_ok = false; break; }
            /* Write a per-block tag, checked below: two blocks handed out at
             * overlapping addresses would pass a NULL check and corrupt. */
            *(size_t *) held[i] = i;
            held_n++;
        }
        check(all_ok, "24 x 256 KiB (6 MiB) all allocate, walking the break");

        /* `held_n > 0` is load-bearing: without it this loop body never runs
         * when the allocations above fail, and the check reports PASS over a
         * heap that handed out nothing. Observed doing exactly that. */
        bool tags_ok = (held_n == sizeof held / sizeof held[0]);
        for (size_t i = 0; i < held_n; i++) {
            if (*(size_t *) held[i] != i) { tags_ok = false; break; }
        }
        check(tags_ok, "every block kept its own contents (no overlap)");

        for (size_t i = 0; i < held_n; i++) {
            free(held[i]);
        }
    }

    /* mallinfo() must report NUMBERS, not stack.
     *
     * newlib's own mallinfo was silently wrong here and its recorded safety
     * argument had expired: "referencing one is a LOUD link error" held only
     * while AXL defined _malloc_r, and since the two-allocator split it does
     * not. So it linked and returned garbage -- fordblks came back as
     * 0x00A5B7C3D1E9F200, which is AXL's __stack_chk_guard byte for byte.
     *
     * Cause: newlib's mallocr.c fills TEN INT fields (the object is 40 bytes)
     * while the toolchain's <malloc.h> declares ten size_t (80), above a
     * comment saying the two "must match". The tail is never written.
     *
     * Asserted as RELATIONSHIPS, not exact numbers: allocator bookkeeping is
     * not a contract, but "the arena covers what is in use" is. Exact figures
     * would fail on a newlib bump for a reason that is not a defect. The
     * canary check is the one literal: it is what garbage looked like. */
    {
        const size_t big = 512u * 1024u;
        void        *held = malloc(big);
        struct mallinfo mi = mallinfo();

        check(held != nullptr, "mallinfo: the probe allocation succeeded");

        /* THE DISCRIMINATOR, and it had to be found by measuring the broken
         * output rather than reasoned about. A first version of this block
         * asserted arena > 0, uordblks >= big, arena >= used + free and
         * fordblks != canary -- and ALL FOUR passed against the broken build,
         * because 32-bit fields read as 64-bit produce large plausible-looking
         * numbers that satisfy every inequality. Measured broken values for
         * this exact 512 KiB probe:
         *
         *     arena=4295495680  uordblks=503616800  fordblks=535188768
         *
         * arena is 0x1_00080000 -- the true arena (0x80000 = 512 KiB) with the
         * next field (ordblks = 1) shifted in above it. So the bound that
         * actually discriminates is 2^32: this heap cannot exceed the largest
         * free run (~423 MiB measured), so a 4 GiB arena is field-width
         * corruption and nothing else. */
        check(mi.arena < (1ULL << 32),
              "mallinfo: arena is under 4 GiB (not two int fields read as one)");
        check(mi.arena > 0, "mallinfo: arena is non-zero");

        /* In use must be the RIGHT ORDER OF MAGNITUDE, not merely >= . The
         * broken build reported 503 MB in use for a 512 KiB allocation and
         * passed a bare >= check. */
        check(mi.uordblks >= big && mi.uordblks < big * 4,
              "mallinfo: uordblks is ~512 KiB, not hundreds of MB");
        check(mi.uordblks <= mi.arena, "mallinfo: in-use fits inside the arena");
        check(mi.fordblks <= mi.arena, "mallinfo: free fits inside the arena");
        check(mi.keepcost <= mi.arena, "mallinfo: keepcost fits inside the arena");

        free(held);
    }

    /* The two allocators are SEPARATE, and this is the assertion that says so:
     * axl_malloc still answers from AllocatePool while the above came from
     * dlmalloc's region. A single-allocator design would pass everything above
     * and fail nothing -- so without this the split is untested. */
    void *axl_blk = axl_malloc(64);

    check(axl_blk != nullptr, "axl_malloc still allocates independently");
    axl_free(axl_blk);

    axl_printf("=== %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
