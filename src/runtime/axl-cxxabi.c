/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxabi.c
    Minimal Itanium C++ ABI symbols needed for freestanding C++ in
    UEFI: `__dso_handle`, `__cxa_atexit`, and a hook that walks
    `.init_array` to fire global constructors.

    Pure-C apps pay zero cost — none of these symbols are referenced
    by C code, so libaxl.a's archive selection skips this object
    entirely.  Apps that include even one C++ source pull this object
    in via compiler-generated references to `__dso_handle` /
    `__cxa_atexit`.

    `_axl_cxxabi_run_init_array` is called from `_axl_init` after the
    rest of the runtime is up, so ctors may freely call `axl_printf`,
    `axl_malloc`, etc.  Destructors registered by `__cxa_atexit` go
    through `axl_atexit`, which means C++ static dtors and C atexit
    callbacks share LIFO ordering during `_axl_cleanup`.
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
