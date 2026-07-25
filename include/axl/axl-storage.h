/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-storage.h:
 *
 * Cross-transport storage device enumeration + identity — the device-walk
 * layer of the storage-access family (see docs/AXL-Storage-Design.md). It
 * iterates every storage device the firmware exposes — NVMe controllers,
 * ATA/SATA devices, SCSI logical units — behind one opaque handle, so a
 * consumer never special-cases the transport. Each module's own enumeration
 * (axl_nvme_next / axl_ata_next / axl_scsi_next) is walked back-to-back.
 *
 * This header answers "which devices are there, and where does each attach?"
 * For each device's normalized health + identity record, layer
 * <axl/axl-smart.h> on top (axl_smart_health takes the AxlStorageDev this
 * header hands out). Scope is read-and-identify only (the storage family's
 * non-goals — block read/write, partitioning, RAID, destructive commands —
 * stay out).
 *
 * @code
 * AxlStorageDev *dev = NULL;
 * while ((dev = axl_storage_next(dev)) != NULL) {
 *     AxlStorageTransport t;
 *     axl_storage_get_transport(dev, &t);
 *     // ... e.g. axl_smart_health(dev, &h) for its health
 * }
 * @endcode
 */

#ifndef AXL_STORAGE_H
#define AXL_STORAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which storage transport a device attaches through.
 */
typedef enum {
    AXL_STORAGE_NVME = 0,  /**< NVMe controller (AxlNvme). */
    AXL_STORAGE_ATA  = 1,  /**< ATA/SATA device (AxlAta). */
    AXL_STORAGE_SCSI = 2   /**< SCSI logical unit (AxlScsi). */
} AxlStorageTransport;

// ===================================================================
// Union device walk
// ===================================================================

/**
 * @brief A storage device on any transport. Opaque and owned by the
 * enumeration; valid for the image lifetime, never freed by the caller.
 */
typedef struct AxlStorageDev AxlStorageDev;

/**
 * @brief Iterate every storage device across all transports.
 *
 * Cursor-style: pass NULL for the first device, then pass each returned handle
 * back for the next; returns NULL at the end. The walk visits NVMe controllers,
 * then ATA/SATA devices, then SCSI logical units, delegating to each module's
 * own enumeration (axl_nvme_next / axl_ata_next / axl_scsi_next). The set is
 * snapshotted on the first call and cached for the image lifetime; position is
 * recovered from the handle you pass back, so passing NULL restarts and
 * independent walks do not interfere. The handle is owned by the module — do
 * not free.
 *
 * @return next storage device, or NULL at end of enumeration.
 */
AxlStorageDev *
axl_storage_next(
    AxlStorageDev *prev   ///< previous device, or NULL to start
);

/**
 * @brief Report which transport a device attaches through (e.g. for display
 * before reading its health).
 *
 * @return AXL_OK on success; AXL_ERR on NULL @p dev.
 */
int
axl_storage_get_transport(
    const AxlStorageDev *dev,        ///< device from axl_storage_next
    AxlStorageTransport *out         ///< [out] transport
);

/**
 * @brief Write a device's transport-native location string — a stable key for
 * telling two otherwise-identical devices apart.
 *
 * The model/serial triple does not uniquely identify a device (two identical
 * disks, or virtualized drives with a blank serial, collide). This writes a
 * location that does, in the transport's native addressing:
 *   - NVMe: the controller's UEFI device-path text (e.g.
 *     "PciRoot(0x0)/Pci(0x4,0x0)/NVMe(...)") — which also correlates with the
 *     EFI_BLOCK_IO device path AxlBlock exposes;
 *   - ATA: "ata <port>.<pmp>" (the controller port / port-multiplier port);
 *   - SCSI: "scsi <target>:<lun>".
 * The string is heterogeneous by transport (a full device path only for NVMe);
 * pair it with `transport` when parsing. NUL-terminated, truncated to
 * @p out_size.
 *
 * @return AXL_OK on success (location written); AXL_ERR on NULL args, zero
 *     @p out_size, or when no location could be resolved (@p out set to "").
 */
int
axl_storage_get_location(
    const AxlStorageDev *dev,        ///< device from axl_storage_next
    char                *out,        ///< [out] location string
    size_t               out_size    ///< capacity of @p out in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_STORAGE_H */
