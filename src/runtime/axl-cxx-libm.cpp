/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxx-libm.cpp
    The libm symbols a hosted container header reaches, routed to AxlMath.

    `std::unordered_map` sizes its bucket array with
    `_Prime_rehash_policy::_M_bkt_for_elements`, which is inline in
    `<bits/hashtable_policy.h>` and reads:

        `__builtin_ceil(__n / (double)_M_max_load_factor)`

    On AArch64 that compiles to a single `frintp` — the rounding mode
    is base ISA, so nothing is called and nothing is needed here. On
    x86-64 rounding a double needs SSE4.1's `roundsd`, which is above
    the `-march=x86-64` baseline the SDK targets, so gcc emits a call
    to `ceil` instead. That asymmetry is why an AArch64-only
    measurement concluded the whole libc footprint was `memcpy`,
    `memmove`, `memset`, `memcmp` and `strlen`: the sixth symbol only
    exists on the other arch.

    Linking glibc's `libm.a` to satisfy it is not an option — IFUNC
    resolvers, `errno` through TLS, and AVX above our baseline, none of
    which survive a UEFI image. AxlMath already exports `axl_ceil` with
    the same semantics and is already tested, so this forwards rather
    than growing a second implementation.

    This TU deliberately includes NO header that declares `ceil`.
    glibc spells it `__THROW` (i.e. `noexcept` under C++) and newlib
    does not, so a definition written to match one toolchain fails to
    compile against the other. Declaring it here, once, sidesteps the
    disagreement — the exception specification is not part of a
    C-linkage symbol's identity, so the link is unaffected either way.

    Lives in `libaxl-cxx.a` rather than `libaxl.a` for the same reason
    `abort` does: a pure-C consumer links `libaxl.a` alone and must not
    acquire a `ceil` it never asked for.
**/

#include <axl.h>

extern "C" double ceil(double x);

extern "C" double
ceil(double x)
{
    return axl_ceil(x);
}
