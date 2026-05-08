/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci-internal.h
    Shared between the AxlPci module's split source files
    (axl-pci.c core + axl-pci-cap.c). Not part of the public SDK
    surface — consumers go through `<axl/axl-pci.h>`.
**/

#ifndef AXL_PCI_INTERNAL_H
#define AXL_PCI_INTERNAL_H

#include <axl/axl-pci.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// PCI Configuration Space register offsets (PCI Local Bus Spec §6.1)
// ---------------------------------------------------------------------------

#define PCI_VENDOR_ID_OFFSET   0x00
#define PCI_DEVICE_ID_OFFSET   0x02
#define PCI_STATUS_OFFSET      0x06
#define PCI_HEADER_TYPE_OFFSET 0x0E
#define PCI_CAP_PTR_OFFSET     0x34

#define PCI_HEADER_MULTIFUNC   0x80
#define PCI_STATUS_CAP_LIST    0x10

#define PCIE_FIRST_EXT_CAP     0x100u
#define PCIE_EXT_CAP_END       0xFFFFu  /* cap_id when no caps present */

// ---------------------------------------------------------------------------
// Internal API shared between axl-pci.c (core) and axl-pci-cap.c
// ---------------------------------------------------------------------------

/**
 * @brief Lazy-init the MCFG segment table. Idempotent and
 *     sticky — a single firmware that doesn't publish MCFG won't
 *     be re-probed on every cap-walk call.
 *
 * @return 0 on ready (subsequent reads can hit ECAM), -1 if MCFG
 *     isn't reachable (caller should fall back to legacy CF8/CFC
 *     or skip the operation).
 */
int
axl_pci_ensure_init(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PCI_INTERNAL_H */
