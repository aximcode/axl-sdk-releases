/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-intrinsics.c
    Compiler-required memory intrinsics, for a link that carries NO libc
    at all.

    This is the `AXL_TOOLCHAIN=host` archive
    (docs/AXL-Host-Toolchain-Design.md §5.3): compiled into the SEPARATE
    `libaxl-standin.a`, linked only when `host` mode builds a link with no
    newlib on it. gcc and clang generate implicit calls to memcpy, memset
    and memmove for struct copies, array initialization, and other
    operations, whether or not the source ever names them -- something has
    to define them, and under `host` there is no other C library on the
    link to do it. String functions (strlen, strcmp, etc.) are in
    src/data/axl-str-compat.c, where they can call AXL equivalents.

    @par Why every definition here is weak

    Under `AXL_TOOLCHAIN=host` this archive is the ONLY provider on the
    link, so weakness buys nothing at compile time today. It is insurance
    against this archive ever being linked beside a real libc -- see
    @par History for why that insurance is not theoretical.
    `scripts/check-libc-overlap.py`'s `check_standin()` asserts every
    symbol here stays weak rather than excluding these names from the
    gate, since an exclusion would stop it seeing a regression.

    @par History

    These files, and this same weak-by-default reasoning, used to also
    cover the `AXL_TOOLCHAIN=axl` link, back when a C-only link there was
    `libaxl.a` and nothing else. There, weak was load-bearing, not just
    insurance: `libgcc(unwind-dw2-fde.o)` referencing `memcpy` once pulled
    this object in beside `libc(memset.o)`, which newlib's stdio had
    already pulled in the same link -- strong definitions on both sides
    would have collided, an uncaught `throw 42;` failed to link on exactly
    that shape. Which archive's copy actually ran depended on which
    reference libgcc's unwinder happened to resolve first, not on
    intent -- weak only bought coexistence, never precedence.

    `6ec731d3` (P3) deleted these files from the `axl` link once newlib
    joined every in-tree build there, reasoning "libc.a is on every link
    now, so a second provider would only compete on scan order." That
    reasoning is still correct for `AXL_TOOLCHAIN=axl` and does NOT apply
    here: `host` has no newlib on the link at all, so there is no
    scan-order competitor to lose to, and restoring these files -- in a
    SEPARATE archive from `libaxl.a`, so P3's tree is otherwise untouched
    -- is what completes a `host` link instead.

    `scripts/check-libc-overlap.py` also enforces the CONVERSE, on
    `libaxl.a` under `AXL_TOOLCHAIN=axl`: symbols AXL must define
    STRONGLY because newlib's are inert under UEFI.
**/

#include <stddef.h>
#include <stdint.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* Weak: see the file docs. No <axl/...> include for the macro -- this file is
   deliberately dependency-free (the compiler can emit a call to memset from
   inside AxlMemLib itself), so it spells the attribute rather than sharing
   axl-str-compat.c's. */
#define AXL_LIBC_FALLBACK __attribute__((weak))

AXL_LIBC_FALLBACK void *
memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

AXL_LIBC_FALLBACK void *
memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

AXL_LIBC_FALLBACK void *
memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s || d >= s + n) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
