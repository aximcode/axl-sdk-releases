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

void
_axl_cxxabi_run_init_array(void)
{
    for (void (**fn)(void) = __init_array_start; fn < __init_array_end; ++fn) {
        (*fn)();
    }
}
