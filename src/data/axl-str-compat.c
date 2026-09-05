/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-str-compat.c
    Standard C string names, for a link that carries NO libc at all.

    This is the `AXL_TOOLCHAIN=host` archive
    (docs/AXL-Host-Toolchain-Design.md §5.3): compiled into the SEPARATE
    `libaxl-standin.a`, linked only when `host` mode builds a link with no
    newlib on it. Third-party code still calls the standard names -- the
    vendored libvterm (`state.o`, `vterm.o`, `parser.o`) and lzma
    (`LzmaEnc.o`, `LzmaDec.o`, `LzFind.o`), plus a few AXL files that reach
    for `memcpy`/`memset` directly on bulk buffers -- and under `host`
    there is no other C library on the link to define them, so
    `--gc-sections` aside, the link fails without this file. `http-server`
    is the shipped source that §5.3 names as the case that fails without it.

    Memory intrinsics (memcpy, memset, memmove) stay in
    src/mem/axl-intrinsics.c because the compiler generates implicit calls
    to them and they can't depend on AxlDataLib.

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
    `libaxl.a` and NOTHING else -- no `libc.a` on the line at all -- so on
    that link libaxl WAS the libc. Weak was load-bearing even then: adding
    the C++ terminate-handler object perturbed the archive scan order and
    produced five `multiple definition` errors on a one-line program,
    which strong definitions would not have survived.

    `6ec731d3` (P3) deleted these files from the `axl` link once newlib
    joined every in-tree build there, reasoning "libc.a is on every link
    now, so a second provider would only compete on scan order." That
    reasoning is still correct for `AXL_TOOLCHAIN=axl` and does NOT apply
    here: `host` has no newlib on the link at all, so there is no
    scan-order competitor to lose to, and restoring these files -- in a
    SEPARATE archive from `libaxl.a`, so P3's tree is otherwise untouched
    -- is what completes a `host` link instead.

    Not everything libaxl and newlib both define is weak, on the
    `AXL_TOOLCHAIN=axl` link: `__cxa_atexit` and the `__stack_chk_*` pair
    stay strong there, because newlib's are structurally inert under UEFI.
    `scripts/check-libc-overlap.py` owns that reasoning for `libaxl.a`.
**/

#include <axl/axl-str.h>
#include <stddef.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* Weak: see the file docs. Spelled on every definition rather than hidden in a
   macro, because `check-libc-overlap.py` reads the BINDING out of the built
   archive -- a macro that silently expanded to nothing would leave the source
   looking correct and the gate is what would catch it, which is the right way
   round but a slow way to learn. */
#define AXL_LIBC_FALLBACK __attribute__((weak))

AXL_LIBC_FALLBACK size_t strlen(const char *s) { return axl_strlen(s); }
AXL_LIBC_FALLBACK int    strcmp(const char *a, const char *b) { return axl_strcmp(a, b); }
AXL_LIBC_FALLBACK int    strncmp(const char *a, const char *b, size_t n) { return axl_strncmp(a, b, n); }
AXL_LIBC_FALLBACK int    memcmp(const void *a, const void *b, size_t n) { return axl_memcmp(a, b, n); }
/* Not one of the four intrinsics gcc assumes freestanding, but libstdc++'s
   char_traits<char>::find calls it out of line -- so every std::string_view
   search (and therefore axl::string's, which forwards to it) needs this. */
AXL_LIBC_FALLBACK void  *memchr(const void *s, int c, size_t n) { return axl_memchr(s, c, n); }
AXL_LIBC_FALLBACK char  *strchr(const char *s, int c) { return axl_strchr(s, c); }
AXL_LIBC_FALLBACK char  *strstr(const char *h, const char *n) { return axl_strstr(h, n); }
AXL_LIBC_FALLBACK char  *strncpy(char *d, const char *s, size_t n) { return axl_strncpy(d, s, n); }

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
