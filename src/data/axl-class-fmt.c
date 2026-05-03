/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-class-fmt.c
    Shared FMT-shape switch for class-triplet output.

    Lifted verbatim from the previous duplicated implementations in
    `axl-pci.c` (axl_pci_class_string_fmt) and `axl-usb-class.c`
    (axl_usb_class_string_fmt). Tier-lookup happens in the caller
    (overlay-then-builtin for PCI; builtin-only for USB); this
    function does only output assembly.
**/

#include "axl-class-fmt.h"

#include <axl/axl-str.h>

int
axl_class_string_fmt_resolve(
    const char  *base_str,
    const char  *sub_str,
    const char  *prog_str,
    const char  *numeric,
    AxlClassFmt  fmt,
    char        *buf,
    size_t       buflen
    )
{
    if (buf == NULL || buflen == 0 || numeric == NULL) {
        return -1;
    }

    switch (fmt) {
    case AXL_CLASS_FMT_BASE:
        if (base_str == NULL) {
            return axl_snprintf(buf, buflen, "%s", numeric);
        }
        return axl_snprintf(buf, buflen, "%s", base_str);

    case AXL_CLASS_FMT_SUBCLASS:
        /* Prefer subclass; coarsen to base; then numeric. */
        if (sub_str != NULL) {
            return axl_snprintf(buf, buflen, "%s", sub_str);
        }
        if (base_str != NULL) {
            return axl_snprintf(buf, buflen, "%s", base_str);
        }
        return axl_snprintf(buf, buflen, "%s", numeric);

    case AXL_CLASS_FMT_FULL:
        if (base_str == NULL) {
            return axl_snprintf(buf, buflen, "%s", numeric);
        }
        if (sub_str == NULL) {
            return axl_snprintf(buf, buflen, "%s", base_str);
        }
        if (prog_str == NULL) {
            return axl_snprintf(buf, buflen, "%s / %s",
                                base_str, sub_str);
        }
        return axl_snprintf(buf, buflen, "%s / %s / %s",
                            base_str, sub_str, prog_str);

    default:
        /* Unknown fmt enum (e.g. caller cast a bogus int).
           Explicit default keeps the contract inside the switch. */
        return -1;
    }
}
