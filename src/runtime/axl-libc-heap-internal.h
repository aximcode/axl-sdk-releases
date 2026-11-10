/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-libc-heap-internal.h
    Giving newlib's heap back to the firmware when an image ends.
    Not a public header.

    newlib's dlmalloc grows into a region `_sbrk` takes with AllocatePages
    (src/cxxrt/axl-cxxrt-alloc.c). UEFI does NOT reclaim an image's page
    allocations when it unloads, so without this every launch of an image that
    had called malloc -- every C++ image among them, since libsupc++'s
    emergency-pool constructor mallocs before main -- left its 1 MiB arena
    behind. Measured: 256 pages per launch on both arches, from 442c442f (the
    two-allocator split, v4.2.0) until this hook existed.

    WEAK, and a pointer rather than a function, so an image that never
    mallocs pays a load and a null test and nothing else: `_sbrk` arms the
    hook only when it first takes the arena, and an image whose link has no
    `_sbrk` at all (the host toolchain carries no newlib) resolves the
    reference to null.

    THE CALL MUST BE THE LAST THING TEARDOWN DOES. Anything after it that
    reaches malloc or free -- an atexit handler, a C++ destructor,
    `__gnu_cxx::__freeres`, a destroy callback the registry sweep runs --
    would write into pages the firmware has taken back. Callers:

      app      _axl_cleanup          (src/runtime/axl-runtime.c), last --
                                      not for a DRIVER image, which reaches
                                      it via axl_exit and may stay resident,
                                      UNLESS that exit unloads it (an EFI
                                      error inside an AXL macro's entry)
      driver   axl_driver_cleanup    (src/util/axl-driver.c), last
      service  the unload stub       (src/service/axl-service.c), after its
                                      own registry sweep and leak report
      minimal  _AxlEntry             (src/crt0/axl-crt0-minimal.c), last
      exit     _axl_cleanup_finish_exit, axl_exit's step before gBS->Exit:
                                      again, for a NESTED exit from inside
                                      the atexit drain, whose _axl_cleanup
                                      never reaches its own last step
**/

#ifndef AXL_LIBC_HEAP_INTERNAL_H
#define AXL_LIBC_HEAP_INTERNAL_H

#include <stddef.h>

/** Set by `_sbrk` once it holds an arena; cleared once that is released. */
extern void (*_axl_libc_heap_release_hook)(void) __attribute__((weak));

/** Release newlib's arena, if this image has one. Idempotent. */
static inline void
_axl_libc_heap_release(
    void
    )
{
    if (&_axl_libc_heap_release_hook != NULL
        && _axl_libc_heap_release_hook != NULL) {
        _axl_libc_heap_release_hook();
    }
}

#endif /* AXL_LIBC_HEAP_INTERNAL_H */
