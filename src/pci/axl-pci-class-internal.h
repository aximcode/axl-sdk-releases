/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci-class-internal.h
    Library-internal accessors for the optional class-name overlay
    singleton. axl-pci.c calls these from its lookup_base/sub/prog
    helpers to consult the loaded overlay before falling back to the
    compiled-in tables. Not part of the public API.
**/

#ifndef AXL_PCI_CLASS_INTERNAL_H
#define AXL_PCI_CLASS_INTERNAL_H

#include <stdint.h>

const char *_axl_pci_class_overlay_base(uint8_t base);
const char *_axl_pci_class_overlay_sub(uint8_t base, uint8_t sub);
const char *_axl_pci_class_overlay_prog(uint8_t base,
                                        uint8_t sub,
                                        uint8_t prog);

#endif /* AXL_PCI_CLASS_INTERNAL_H */
