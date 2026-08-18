/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxrt-alloc.c
    The heap AXL hands to newlib's allocator, and nothing else.

    §2-DECISION of `docs/AXL-Libc-Substrate-Design.md`: TWO allocators, split
    by NAME. newlib owns the whole C vocabulary — `malloc`, `free`, `realloc`,
    `calloc`, `memalign` and the reentrant `_r` family — and runs its own
    dlmalloc. `axl_malloc`/`axl_free` stay a separate allocator over
    `AllocatePool`. Nothing crosses between them, because the namespaces are
    disjoint.

    THAT DISJOINTNESS IS THE SAFETY ARGUMENT, and it is worth stating because a
    half-split is genuinely dangerous. This file used to bridge the PLAIN names
    onto `axl_malloc` while newlib's internals kept using the `_r` family — so
    `strdup()` allocated from one allocator and `free()` released to the other,
    and the only thing preventing corruption was that `sbrk` returned -1 and
    newlib's allocator could never obtain a byte. Two allocators are safe when
    the split is total; they are a live corruption when it is partial.

    What AXL still owes newlib is the one thing dlmalloc needs from any
    platform: a region it can extend. That is `sbrk` below.

    SEPARATE OBJECT from the frame-table lifecycle on purpose. Archive members
    are all-or-nothing, and keeping the `__eh_frame_start` reference in a
    different member means a link can take this one WITHOUT opting into the
    exceptions linker script. Merged, `--no-undefined` fires on
    `__eh_frame_start` before `--gc-sections` can collect the unreferenced
    function holding it. Same reason `src/runtime/axl-cxxabi.c` is its own
    object.

    THE COST, ACCEPTED KNOWINGLY (§2-DECISION): `operator new` reaches
    `malloc`, which is now newlib's, so C++ and third-party allocations no
    longer appear in AXL's leak gate. That gate found the libstdc++
    emergency-pool leak (`a0234245`). It still covers every `axl_malloc`; it no
    longer covers the C/C++ world.
**/

#include <malloc.h>
#include <reent.h>

#include <axl.h>

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// sbrk — the heap newlib's dlmalloc grows into
// ---------------------------------------------------------------------------
//
// §2-DECISION of docs/AXL-Libc-Substrate-Design.md: TWO allocators, split by
// NAME. newlib owns the whole C vocabulary (malloc/free/realloc/calloc and the
// _r family) and runs its own dlmalloc; `axl_malloc` stays a separate
// allocator over AllocatePool. Nothing crosses, because the namespaces are
// disjoint -- which is what makes two allocators safe here where the earlier
// half-split was not.
//
// So this file no longer bridges any allocator entry point. It supplies the
// one thing dlmalloc actually needs from a platform: a region it can extend.
//
// WHY PAGES RATHER THAN AllocatePool. dlmalloc wants a break it can push
// outward, and asks in multiples it chooses. axl_alloc_pages maps straight
// onto that, and keeping the heap out of the pool allocator is the point of
// the split -- AXL's own allocations stay individually visible to the firmware
// and to the leak gate, while newlib's churn happens inside a region the
// firmware sees once.
//
// THE BREAK IS ONE ASCENDING RANGE, AND THAT IS NOT NEGOTIABLE.
//
// This file used to take a fresh axl_alloc_pages chunk whenever the current
// one ran out, on the reasoning that "dlmalloc handles a non-contiguous
// MORECORE by starting a new segment". BOTH HALVES OF THAT WERE WRONG, and
// the result was a heap that failed every allocation of 1 MiB or more and
// then wedged so that even a 256 KiB request returned NULL:
//
//   - UEFI satisfies AllocatePages DOWNWARD from high memory, so each fresh
//     chunk landed BELOW the previous one. Measured, both arches:
//     0x1de1f000, then 0x1dd1f000, then 0x1dc1f000. That is not merely
//     non-contiguous -- a break that moves BACKWARDS is not a break. newlib's
//     allocator sizes its top as `brk + size - old_end`, which then goes
//     negative and wraps; mallinfo reported 46,645,422,825,533,952 free bytes.
//   - the "multi-chunk assertion in test-libc-alloc-qemu.sh" did not exist.
//     The largest allocation any fixture made was realloc(16 -> 4096), so the
//     path was never executed, let alone asserted.
//
// So the region is obtained ONCE and extended IN PLACE. axl_alloc_pages_at
// asks the firmware for the pages immediately following the ones we already
// own; if it can give them out the range stays contiguous and ascending by
// construction, and if it cannot the answer is a clean -1 that dlmalloc reads
// as ordinary OOM. Nothing here can ever hand back a descending address.
//
// PLACEMENT IS WHAT MAKES THE EXTENSION POSSIBLE, and this is the part that
// is not obvious. Taking the arena from axl_alloc_pages -- i.e. letting the
// firmware choose -- puts it at the TOP of memory, inside the stripe the
// firmware has been carving its own allocations out of; measured on OVMF x64,
// the pages above such a region are occupied every single time, so the first
// extension fails and the arena's initial size becomes a hard cap. Asking
// axl_mem_largest_free_run where the big untouched run is, and taking its LOW
// end, leaves the whole rest of that run above us to grow into: 423 MiB on
// x64 and 326 MiB on aa64, against a 16 MiB initial commit.
//
// PI's GCD looks like the right tool and is not. AllocateMemorySpace has an
// explicit EfiGcdAllocateAnySearchBottomUp, but the DXE Core claims all of
// SystemMemory for itself at init, so GCD reports ZERO unallocated
// SystemMemory on both arches and the call returns EFI_NOT_FOUND. Measured,
// not assumed.
//
// WHY PAGES RATHER THAN AllocatePool. dlmalloc wants a break it can push
// outward, and asks in multiples it chooses. Page allocation maps straight
// onto that, and keeping the heap out of the pool allocator is the point of
// the split -- AXL's own allocations stay individually visible to the firmware
// and to the leak gate, while newlib's churn happens inside a region the
// firmware sees once.

/* THE INITIAL COMMIT IS SMALL BECAUSE GROWTH IS RELIABLE, and that ordering
   is the whole point. An earlier version committed 16 MiB up front -- correct
   when growth was impossible, because it was all you would ever get, and
   strictly wrong now: it made any program that mallocs sixteen BYTES commit
   sixteen MEGABYTES. With placement working, 1 MiB up front reaches the same
   ceiling and wastes nothing.

   A program that never calls malloc still pays NOTHING: hello.c does not
   reference this file and is byte-identical with or without it on the link
   line (47,247 bytes, measured across the change). */
#define AXL_SBRK_ARENA_PAGES  256   /* 1 MiB initial commit */

/* GEOMETRIC. A fixed 1 MiB step needs ~400 AllocateAddress calls to consume
   the 423 MiB run measured on OVMF x64; doubling needs ~9. Capped so a heap
   that has grown large stops over-reserving on each step -- past 16 MiB the
   call count is already negligible and the waste is not. */
#define AXL_SBRK_GROW_MIN     256   /* 1 MiB first step */
#define AXL_SBRK_GROW_MAX     4096  /* 16 MiB step ceiling */

/* Total pages the C heap may ever own, 0 = whatever the run allows. Read once
   from AXL_LIBC_HEAP_MAX (in MiB) -- the knob a deployment actually wants is
   a CAP, for a driver coexisting with other consumers; the initial size stops
   mattering once growth is cheap. Deliberately NOT a percentage of RAM: total
   RAM is not the constraint, the largest contiguous FREE run is, and the two
   diverge sharply (512 MB of RAM, a 423 MiB best run). */
#define AXL_SBRK_LIMIT_ENV    "AXL_LIBC_HEAP_MAX"

static uint8_t *mBrkBase;   /* start of the arena -- set ONCE, never moves */
static uint8_t *mBrkCur;    /* next byte to hand out */
static uint8_t *mBrkEnd;    /* end of the region owned so far */
static size_t   mGrowPages = AXL_SBRK_GROW_MIN;  /* doubles, capped */
static size_t   mLimitPages;                     /* 0 = no ceiling */
static bool     mLimitRead;                      /* env consulted already */

/* Read the cap once, before a single byte has been handed out.
 *
 * axl_getenv allocates through axl_malloc (AllocatePool), NOT through the C
 * allocator this file feeds, so it cannot recurse into sbrk. The guard is set
 * FIRST anyway: if some future getenv path did reach malloc, the nested call
 * sees mLimitRead already true and proceeds uncapped rather than deadlocking
 * on a half-initialised limit. */
static void
sbrk_read_limit(void)
{
    char *v;

    if (mLimitRead) {
        return;
    }
    mLimitRead = true;

    v = axl_getenv(AXL_SBRK_LIMIT_ENV);
    if (v == NULL) {
        return;
    }
    /* MiB in, pages out. A value of 0 or an unparsable one leaves the heap
       uncapped, which is the documented default -- refusing to start over a
       malformed tuning knob would be a worse failure than ignoring it. */
    uint64_t mib = 0;
    if (axl_str_to_u64(v, 0, &mib, NULL) == AXL_OK && mib > 0) {
        mLimitPages = (size_t)(mib * 256u);   /* 256 pages per MiB */
    }
    axl_free(v);
}

#if defined(AXL_NEWLIB_MALLINFO_INT)
/* GATED ON THE BUILD-TIME PROBE, and that gate is the whole correctness story
   across arches. The Makefile reads the SIZE of __malloc_current_mallinfo out
   of the toolchain's libc.a: 40 bytes is ten ints (broken against the header),
   80 is ten size_t (correct). Measured -- the pinned x86_64-elf build is 40 and
   ARM's is 80, so this is a property of the TOOLCHAIN BUILD and not of the
   arch. Hard-coding the int layout was wrong on aa64 and the aa64 suite caught
   it; hard-coding the other would be wrong on x64. Where newlib is already
   right, AXL defines nothing and newlib's own mstats.o is used unchanged.

   This block therefore disappears the moment the x64 toolchain is rebuilt with
   matching types, which is the correct end state. */

// ---------------------------------------------------------------------------
// mallinfo and friends -- newlib's are BROKEN here, so AXL owns them
// ---------------------------------------------------------------------------
//
// THE BUG IS THE TOOLCHAIN'S, and it is silent. newlib's mallocr.c fills a
// `struct mallinfo` of ten INT fields -- __malloc_current_mallinfo is 40 bytes,
// read straight off the archive symbol table -- while the same toolchain's
// <malloc.h> declares ten `size_t` (80 bytes), above a comment reading "This
// version of struct mallinfo must match the one in libc/stdlib/mallocr.c".
// It does not. A caller therefore reads pairs of 32-bit fields as single
// 64-bit ones and 40 bytes of untouched stack after them. Measured, for a
// 512 KiB allocation:
//
//     arena=4295495680  uordblks=503616800  fordblks=535188768
//
// arena is 0x1_00080000: the true value (0x80000 = 512 KiB) with ordblks (1)
// shifted in above it. Before the heap grew large enough to disturb the stack,
// fordblks came back as 0x00A5B7C3D1E9F200 -- AXL's __stack_chk_guard, byte for
// byte, which is what finally identified it as uninitialised memory.
//
// WHY THIS WAS NOT CAUGHT EARLIER. It used to be a LOUD link error: pulling
// mallinfo dragged mallocr.o, which multiply-defined _malloc_r against AXL's.
// §2-DECISION handed the whole C allocator to newlib, so AXL defines _malloc_r
// no longer, and the loud failure became a quiet wrong answer.
//
// ALL FIVE SYMBOLS, because a member is all-or-nothing. libc_a-mstats.o
// defines mallinfo, malloc_stats, mallopt, mstats and _mstats_r; defining only
// mallinfo would leave a consumer who calls malloc_stats pulling that member
// and multiply-defining mallinfo against this one. Owning MOST of a member is
// the bug -- the same trap P3 hit with stack_protector.o.
//
// The data comes from symbols OUTSIDE that member, which is what makes the
// displacement work: __malloc_update_mallinfo lives in mallinfor.o and
// __malloc_current_mallinfo in mallocr.o, both global.

/* newlib's ACTUAL layout: ten ints, matching the 40-byte object. Declared
   here rather than trusted from <malloc.h>, which is the header that is
   wrong. */
struct axl_newlib_mallinfo {
    int arena, ordblks, smblks, hblks, hblkhd;
    int usmblks, fsmblks, uordblks, fordblks, keepcost;
};

extern void axl_newlib_update_mallinfo(void)
    __asm__("__malloc_update_mallinfo");
extern struct axl_newlib_mallinfo axl_newlib_current_mallinfo
    __asm__("__malloc_current_mallinfo");

/**
 * @brief Allocator statistics, with the field widths corrected.
 *
 * Same numbers newlib computes; read through a declaration that matches how
 * they were written, then widened into the `size_t` struct <malloc.h>
 * declares and every caller expects.
 */
struct mallinfo
mallinfo(
    void
    )
{
    struct mallinfo out;
    const struct axl_newlib_mallinfo *in = &axl_newlib_current_mallinfo;

    axl_newlib_update_mallinfo();

    /* Field by field, not a memcpy: the widening IS the fix. Each is signed
       in newlib and unsigned here, so a negative would wrap to something
       enormous -- clamped rather than propagated, because a caller printing
       18 quintillion bytes free is the failure this function exists to end. */
    #define AXL_MI(f)  out.f = (in->f < 0) ? 0u : (size_t)in->f
    AXL_MI(arena);   AXL_MI(ordblks); AXL_MI(smblks);   AXL_MI(hblks);
    AXL_MI(hblkhd);  AXL_MI(usmblks); AXL_MI(fsmblks);  AXL_MI(uordblks);
    AXL_MI(fordblks); AXL_MI(keepcost);
    #undef AXL_MI

    return out;
}

/* The rest of libc_a-mstats.o, forwarded to the reentrant forms it would have
   called. Present so that member is never pulled -- see the note above.
   Signatures come from <malloc.h> and <reent.h>; declaring them by hand here
   was a compile error, which is the headers doing their job. */

void
malloc_stats(
    void
    )
{
    _malloc_stats_r(_REENT);
}

int
mallopt(
    int parameter,
    int value
    )
{
    return _mallopt_r(_REENT, parameter, value);
}

/**
 * @brief Legacy BSD allocator report.
 *
 * newlib's version prints through `fiprintf` to a `stderr` that firmware does
 * not wire up, so it emitted nothing. This one goes through `axl_printf`,
 * which reaches the console -- and it is built on the corrected mallinfo
 * above, so the numbers are the right ones.
 */
void
_mstats_r(
    struct _reent *reent,
    char          *label
    )
{
    struct mallinfo mi = mallinfo();

    (void)reent;
    axl_printf("%s: in use %lu bytes, free %lu bytes, arena %lu bytes\n",
               (label != NULL) ? label : "malloc",
               (unsigned long)mi.uordblks, (unsigned long)mi.fordblks,
               (unsigned long)mi.arena);
}

void
mstats(
    char *label
    )
{
    _mstats_r(_REENT, label);
}

#endif /* AXL_NEWLIB_MALLINFO_INT */

/**
 * @brief Extend the break by @a increment bytes.
 *
 * Returns the PREVIOUS break on success and (void *)-1 on failure, which is
 * the contract dlmalloc checks. `sbrk(0)` reports the current break without
 * allocating, which is how dlmalloc discovers its base.
 *
 * A negative increment walks back inside the current chunk only. Pages are
 * never returned to the firmware here: dlmalloc calls this to trim, and
 * handing a page back that it still believes it owns would be a use-after-free
 * with the firmware as the other party.
 */
void *
_sbrk(
    intptr_t increment
    )
{
    uint8_t   *prev;
    uint64_t   phys;
    size_t     pages;
    size_t     want;

    if (increment == 0) {
        return mBrkCur;
    }
    if (increment < 0) {
        size_t back = (size_t)(-increment);

        /* Against the CHUNK BASE, not the raw pointer value. An earlier
           version compared the amount to the address itself, which is not a
           bound on anything and would have let a trim walk out of the
           region. */
        if (mBrkCur == NULL || back > (size_t)(mBrkCur - mBrkBase)) {
            return (void *)-1;
        }
        prev = mBrkCur;
        mBrkCur -= back;
        return prev;
    }

    want = (size_t)increment;
    if (mBrkCur != NULL && want <= (size_t)(mBrkEnd - mBrkCur)) {
        prev = mBrkCur;
        mBrkCur += want;
        return prev;
    }

    /* FIRST USE: take the arena. Nothing is committed before this point. */
    if (mBrkCur == NULL) {
        uint64_t run_base  = 0;
        size_t   run_pages = 0;
        bool     placed    = false;

        sbrk_read_limit();

        pages = (want + 4095u) / 4096u;
        if (pages < AXL_SBRK_ARENA_PAGES) {
            pages = AXL_SBRK_ARENA_PAGES;
        }
        if (mLimitPages != 0 && pages > mLimitPages) {
            /* The very first request already exceeds the cap. Trim to it and
               let the allocation fail honestly if that is not enough. */
            pages = mLimitPages;
        }

        /* PLACE it at the bottom of the biggest free run, so the pages above
           are free for later extension. See the header note: this is the
           difference between a growable heap and a hard cap. */
        if (axl_mem_largest_free_run(&run_base, &run_pages) == AXL_OK
            && run_pages >= pages
            && axl_alloc_pages_at(run_base, pages) == AXL_OK) {
            phys   = run_base;
            placed = true;
        }

        /* FALLBACK: let the firmware choose. Reached when the map cannot be
           read, or the run is too small, or somebody took the base between
           the query and the claim -- the query is a snapshot, so losing that
           race is ordinary. A heap that cannot grow still beats no heap.
           AXL_OK, not a bool: axl_alloc_pages returns a STATUS and AXL_OK is
           0, so `if (!axl_alloc_pages(...))` reads success as failure --
           which is exactly what it did once, and every newlib allocation
           returned NULL while axl_malloc carried on working. */
        if (!placed && axl_alloc_pages(pages, &phys) != AXL_OK) {
            return (void *)-1;
        }
        mBrkBase = (uint8_t *)(uintptr_t)phys;
        mBrkCur  = mBrkBase;
        mBrkEnd  = mBrkBase + (pages * 4096u);

        prev = mBrkCur;
        mBrkCur += want;
        return prev;
    }

    /* GROW IN PLACE, never sideways. The shortfall is what we are missing, not
       the whole request -- part of it is already covered by the tail of the
       region we own. */
    {
        size_t shortfall = want - (size_t)(mBrkEnd - mBrkCur);
        size_t owned     = (size_t)(mBrkEnd - mBrkBase) / 4096u;

        pages = (shortfall + 4095u) / 4096u;
        if (pages < mGrowPages) {
            pages = mGrowPages;
        }

        /* The cap bounds the TOTAL owned, so a step is trimmed to fit rather
           than refused outright -- a heap one page under its limit should be
           able to use that page. */
        if (mLimitPages != 0) {
            if (owned >= mLimitPages) {
                return (void *)-1;
            }
            if (owned + pages > mLimitPages) {
                pages = mLimitPages - owned;
            }
            /* Trimmed below what this request needs: refuse, rather than
               extend by a useless amount and fail on the next call anyway. */
            if (pages * 4096u < shortfall) {
                return (void *)-1;
            }
        }

        /* Exactly the pages after ours. A failure here means the firmware has
           given that range to somebody else, which is ordinary -- report OOM
           rather than accepting a disjoint region, because a break that jumps
           is the defect this file exists to document. */
        if (axl_alloc_pages_at((uint64_t)(uintptr_t)mBrkEnd, pages) != AXL_OK) {
            return (void *)-1;
        }
        mBrkEnd += pages * 4096u;

        /* Double for next time. Grown AFTER a success, so a run of refusals
           does not inflate the ask and turn a recoverable shortage into a
           guaranteed one. */
        if (mGrowPages < AXL_SBRK_GROW_MAX) {
            mGrowPages *= 2;
            if (mGrowPages > AXL_SBRK_GROW_MAX) {
                mGrowPages = AXL_SBRK_GROW_MAX;
            }
        }
    }

    prev = mBrkCur;
    mBrkCur += want;
    return prev;
}

/* `sbrk` is a WEAK ALIAS for the implementation above, never a wrapper that
   CALLS it -- and that distinction is load-bearing rather than stylistic.
 *
 * ARM's newlib defines a strong `sbrk` of its own (configure.host gives
 * aarch64 a syscall_dir; x86_64 gets none). Where that one wins, its chain is
 * `sbrk` -> `_sbrk_r` -> `_sbrk`. If `_sbrk` here were a wrapper calling
 * `sbrk` by name, that call would resolve to NEWLIB's and close the loop:
 * _sbrk -> sbrk -> _sbrk_r -> _sbrk, an infinite recursion that blows the
 * stack at the first malloc rather than failing at link. An alias cannot form
 * that cycle, because both names denote the same address.
 *
 * Weak so newlib's plain `sbrk` wins where it exists and ours is used where it
 * does not -- exactly one definition in the image either way, with the
 * underscore form always the implementation. */
void *sbrk(intptr_t increment) __attribute__((weak, alias("_sbrk")));
