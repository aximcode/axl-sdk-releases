/* SPDX-License-Identifier: MIT */
/* Copyright 2026 AximCode */

/** @file axl-vterm-compat.h
    Freestanding compatibility shim for the vendored libvterm sources.

    libvterm targets a hosted C environment. This header supplies the handful
    of C-library names its Layer-2 sources reference, mapping them onto AXL's
    own implementations. It is included from the top of `vterm_internal.h`,
    which every Layer-2 translation unit includes first, so the mappings are
    in scope before any `<stdio.h>` / `<stdlib.h>` declaration is seen.

    Resolved elsewhere, for free (listed so the audit is complete):
      - `memcpy` / `memmove` / `memset`  -> src/mem/axl-intrinsics.c
      - `strncmp` / `strncpy`            -> src/data/axl-str-compat.c

    Mapped here:
      - `snprintf` / `vsnprintf`         -> axl_snprintf / axl_vsnprintf
      - `abs`                            -> __builtin_abs

    Eliminated by patch (see deps/libvterm/README.md):
      - `malloc` / `free`                -> axl_malloc / axl_free
      - `exit` / `fprintf` / `stderr`    -> vterm_check_version() gutted
      - `vterm_screen_free`              -> Layer 3 (screen.c) is not vendored
**/

#ifndef AXL_VTERM_COMPAT_H
#define AXL_VTERM_COMPAT_H

#include <axl/axl-debug.h>
#include <axl/axl-str.h>

/* libvterm formats DSR / DA / SGR query replies with these. AXL's printf
 * engine implements the specifiers it uses (%d, %s, %c) with C99 return
 * semantics, which is what vterm_push_output_sprintf() relies on to size
 * its output. */
#define snprintf  axl_snprintf
#define vsnprintf axl_vsnprintf

/* state.c and vterm.c take |delta| of a row/column difference. */
#define abs(x) __builtin_abs(x)

#endif /* AXL_VTERM_COMPAT_H */
