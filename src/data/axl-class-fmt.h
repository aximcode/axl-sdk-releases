/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-class-fmt.h
    Internal helper: format a (base, sub, prog) class triplet given
    pre-resolved tier name strings.

    Hoisted from `axl-pci.c` and `axl-usb-class.c`, which carried
    line-for-line copies of the same FMT_FULL / FMT_SUBCLASS /
    FMT_BASE switch over an enum + the same "omit unknown tiers,
    fall back to numeric on wholly unknown" fallback chain.

    The lookup chain (overlay then compiled-in for PCI; compiled-in
    only for USB) stays in each module — that's the genuinely
    module-specific concern. The output assembly is shared.

    Internal-only: consumers reach for `axl_pci_class_string_fmt` /
    `axl_usb_class_string_fmt` through the public API.
**/

#ifndef AXL_CLASS_FMT_H
#define AXL_CLASS_FMT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AXL_CLASS_FMT_FULL     = 0,  ///< "<base> / <sub> / <prog>" (omit unknown tiers)
    AXL_CLASS_FMT_SUBCLASS = 1,  ///< "<sub>" (coarsen to <base> if unknown)
    AXL_CLASS_FMT_BASE     = 2,  ///< "<base>" (coarsen to numeric if unknown)
} AxlClassFmt;

/* Caller has already done the per-tier lookups (consulting an
   overlay sidecar then compiled-in tables, or whatever scheme fits
   the module) and supplies the resolved strings — NULL for tiers
   the lookup did not find. @p numeric is the fallback string used
   when @p base_str is NULL or @p fmt requires a tier nobody
   resolved; the caller pre-formats it (axl_snprintf shape).

   Returns axl_snprintf shape: byte count written excluding NUL on
   success, -1 on bad arguments (NULL buf, zero buflen, unknown
   @p fmt). */
int axl_class_string_fmt_resolve(
    const char  *base_str,    ///< base-tier name, or NULL if unknown
    const char  *sub_str,     ///< sub-tier name, or NULL if unknown
    const char  *prog_str,    ///< prog-tier name, or NULL if unknown
    const char  *numeric,     ///< pre-formatted numeric fallback (e.g. "Class 060000")
    AxlClassFmt  fmt,         ///< output shape
    char        *buf,         ///< destination buffer
    size_t       buflen       ///< capacity of @p buf
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CLASS_FMT_H */
