/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxrt-eh.c
    Frame-table lifecycle for the EXCEPTIONS build.

    Its own object, apart from the allocator bridge, because `__eh_frame_start`
    below comes from the exceptions linker script and archive members are
    all-or-nothing: merged with the bridge, every C++ link would drag this
    reference and fail under `--no-undefined`. See axl-cxxrt-alloc.c.
**/

#include <axl.h>

#include "axl-cxxrt.h"

// ---------------------------------------------------------------------------
// Toolchain entry points
// ---------------------------------------------------------------------------

/* Emitted by the exceptions linker script (elf_*_efi_eh.lds). A link without
   that script fails loudly here rather than registering garbage: all three
   build paths pass --no-undefined, and __register_frame dereferences its
   argument immediately, so a silently-zero symbol would read address 0. */
extern char __eh_frame_start[];

/* libgcc's, hand-declared because there is no header to include: <unwind.h>
   does NOT declare these -- they live in libgcc's uninstalled
   unwind-dw2-fde.h. Signatures match libgcc's exactly (void *, not
   const void *), so the prototypes cannot disagree across TUs. */
void __register_frame(void *begin);
void *__deregister_frame(void *begin);

/* __gnu_cxx::__freeres(), by its mangled name so a C file can reach it. The
   Itanium C++ ABI mangling is identical on x86-64 and AArch64, and this is
   the spelling `nm` reports for both toolchains' eh_alloc.o.

   It frees libsupc++'s emergency exception pool, which a static-init
   constructor malloc'd. glibc reaches it from its valgrind-clean shutdown
   path; UEFI has no equivalent, so nothing called it and the pool leaked. */
void axl_cxxrt__freeres(void) __asm__("_ZN9__gnu_cxx9__freeresEv");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

/* Guards BOTH directions, and this is a fault-avoidance measure rather than
   tidiness. __deregister_frame on a table that was never registered walks
   into gcc_assert(ob) inside libgcc, which is `ud2` on x64 and `bl abort` on
   aa64 -- and a #UD with CR2=0 under UEFI reads exactly like the AVX fault
   this project has documented lore about, so the symptom actively misleads.
   Teardown running on a path init did not take is ordinary (an early
   axl_exit, a failed init), so it must be safe, not merely discouraged. */
static bool mRegistered;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/* axl_atexit takes a void(void *); the lifecycle API takes no argument
   because nothing needs one. One trampoline is cheaper than widening the
   public shape of axl_cxxrt_fini to suit one caller. */
static void
cxxrt_fini_trampoline(
    void *unused
    )
{
    (void)unused;
    axl_cxxrt_fini();
}

void
axl_cxxrt_init(
    void
    )
{
    if (mRegistered) {
        return;
    }
    __register_frame(__eh_frame_start);
    mRegistered = true;

    /* REGISTERING TEARDOWN HERE IS WHAT MAKES IT HAPPEN AT ALL. Nothing else
       calls axl_cxxrt_fini: it is reachable from no other symbol, so
       --gc-sections collects it and the frame table, libgcc's registration
       object and the 13912-byte sorted FDE table it builds on the first
       unwind, and libsupc++'s emergency pool all survive to teardown. That is
       the leak this file's docstring measured down to zero -- and an
       AXL_MEM_DEBUG build fails the leak gate on it.
       atexit is LIFO, so registering FIRST means running LAST, which is the
       ordering axl_cxxrt_fini documents: after every other atexit handler (a
       destructor may throw, which needs the table still registered) and
       before the leak report. */
    (void)axl_atexit(cxxrt_fini_trampoline, NULL);
}

/**
 * Both halves are needed and neither is optional: measured on aa64, the
 * baseline leaked 3 allocations / 15112 bytes, __freeres alone left 2 /
 * 13960, and both together leave ZERO.
 *
 * The larger two were never libstdc++ init state -- they are libgcc's own
 * bookkeeping for the table WE registered: a registration object (48 B) plus
 * the sorted FDE lookup table search_object() builds on the first unwind
 * (13912 B). So this is symmetry, not a workaround: whoever calls
 * __register_frame owes a __deregister_frame.
 */
void
axl_cxxrt_fini(
    void
    )
{
    if (!mRegistered) {
        return;
    }
    mRegistered = false;

    axl_cxxrt__freeres();
    (void)__deregister_frame(__eh_frame_start);
}
