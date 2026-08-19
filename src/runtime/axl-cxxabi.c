/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxabi.c
    Minimal Itanium C++ ABI symbols needed for freestanding C++ in
    UEFI: `__dso_handle`, `__cxa_atexit`, and a hook that walks
    `.init_array` to fire global constructors.

    What a PURE-C image pays is the walker below and nothing else.
    Both entry paths reference `_axl_cxxabi_run_init_array`
    unconditionally, so archive selection pulls this object into every
    image; `--gc-sections` then drops the rest, and `nm` on a
    C-only app confirms it — the walker is present, `__dso_handle` and
    `__cxa_atexit` are not.  An image with even one C++ source keeps
    those two as well, via compiler-generated references.

    `_axl_cxxabi_run_init_array` is called from BOTH entry paths, in
    each case after the rest of the runtime is up so ctors may freely
    call `axl_printf`, `axl_malloc`, etc:

      app     `_axl_init`        (src/runtime/axl-runtime.c)
      driver  `axl_driver_init`  (src/util/axl-driver.c)

    The driver half was missing for the whole life of the C++ layer,
    and failed silently — a driver image carried constructors nothing
    ever ran, and .bss zeroing made every unconstructed global read as
    a plausible 0 rather than fault.

    Destructors registered by `__cxa_atexit` go through `axl_atexit`,
    which means C++ static dtors and C atexit callbacks share LIFO
    ordering — during `_axl_cleanup` for an app, and during
    `axl_driver_cleanup` on a driver's unload path.  Note that
    `axl_atexit` refuses registration while its table is NULL, so
    whichever entry path runs the constructors must have called
    `_axl_atexit_init` FIRST or every destructor is silently dropped.
**/

#include <stddef.h>

#include <axl/axl-atexit.h>

#include "axl-cxxabi-internal.h"

// ---------------------------------------------------------------------------
// Itanium C++ ABI symbols
// ---------------------------------------------------------------------------

/* Cookie passed to compiler-generated __cxa_atexit calls so the dtor
 * machinery can group destructors by owning DSO.  Single-DSO model:
 * the address itself is the only thing that matters. */
void *__dso_handle = &__dso_handle;

/* Registers a destructor; routes through axl_atexit so C++ static
 * destructors and C atexit callbacks share LIFO order at cleanup. */
int
__cxa_atexit(
    void (*fn)(void *),
    void  *arg,
    void  *dso
    )
{
    (void)dso;
    return axl_atexit((AxlAtexitFn)fn, arg) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// .init_array walker
// ---------------------------------------------------------------------------

/* Bounds emitted by the linker scripts (elf_x86_64_efi.lds,
 * elf_aarch64_efi.lds).  When no C++ source contributes a static
 * initializer the array is empty and the bounds are equal. */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

/* The exceptions build's frame-table registration (src/cxxrt/axl-cxxrt-eh.c).
 *
 * WEAK, and called from HERE rather than from _axl_init, for two reasons that
 * both come down to what a link pulls in:
 *
 *   - A pure-C image never references any symbol in this object, so libaxl.a's
 *     archive selection skips it entirely -- which means the call below costs
 *     such an image nothing at all, not even a null check. Putting it in
 *     _axl_init would put it in EVERY image. That is the §U2 byte-identity
 *     constraint: a C image must be unchanged whether or not the SDK supports
 *     exceptions.
 *   - A C++ image WITHOUT exceptions links no libaxl-cxxrt.a, so the weak
 *     reference resolves to 0 and is skipped. Only an exceptions link defines
 *     it. `ld --no-undefined` permits an undefined WEAK reference, which is
 *     what makes the same object serve both.
 *
 * Ordering is not incidental: it must run BEFORE the constructors below,
 * because a global constructor may throw and the unwinder needs the table.
 */
extern void axl_cxxrt_init(void) __attribute__((weak));

void
_axl_cxxabi_run_init_array(void)
{
    if (axl_cxxrt_init != NULL) {
        axl_cxxrt_init();
    }
    for (void (**fn)(void) = __init_array_start; fn < __init_array_end; ++fn) {
        (*fn)();
    }
}
