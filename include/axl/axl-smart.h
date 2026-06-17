/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-smart.h:
 *
 * Normalized cross-transport storage health — the synthesis layer over
 * AxlNvme / AxlAta / AxlScsi, and Phase 4 of the storage-access family (see
 * docs/AXL-Storage-Design.md). It answers the one question a "is my disk OK?"
 * tool actually asks, regardless of how the disk attaches: enumerate every
 * storage device the firmware exposes — NVMe controllers, ATA/SATA devices,
 * SCSI logical units — and report each one's health and identity in a single
 * uniform AxlSmartHealth record. This is `smartctl` for UEFI: the `smart` tool
 * is a thin renderer over it.
 *
 * The union walk (axl_storage_next / AxlStorageDev) iterates all three
 * transports back-to-back so a consumer never special-cases them. Per-field
 * normalization is honest: a field a transport cannot supply is set to a
 * documented sentinel (never guessed), so call sites test the sentinel rather
 * than juggle a presence mask.
 *
 * Scope is read-and-health only (the storage family's non-goals — block
 * read/write, partitioning, RAID, destructive commands — stay out). For
 * transport-specific detail beyond the normalized struct (NVMe namespaces, the
 * ATA attribute table, raw CDBs) drop to AxlNvme / AxlAta / AxlScsi directly.
 *
 * @code
 * AxlStorageDev *dev = NULL;
 * while ((dev = axl_storage_next(dev)) != NULL) {
 *     AxlSmartHealth h;
 *     if (axl_smart_health(dev, &h) == AXL_OK) {
 *         axl_printf("%s %s: %s\n", h.model, h.serial,
 *                    h.healthy ? "OK" : "FAILING");
 *     }
 * }
 * @endcode
 */

#ifndef AXL_SMART_H
#define AXL_SMART_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-nvme.h>
#include <axl/axl-ata.h>
#include <axl/axl-scsi.h>

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

/// Normalized critical-warning bits in AxlSmartHealth::warning_flags.
/// warning_flags has NVMe-grain detail only: NVMe maps its Critical Warning
/// byte onto these bits. ATA and SCSI expose just an overall verdict (no
/// per-condition decode), so they set AXL_SMART_WARN_RELIABILITY iff
/// `!healthy` and nothing else — in particular AXL_SMART_WARN_TEMPERATURE is
/// NOT derived from `temperature_c`, so a consumer must not read the
/// temperature bit cross-transport.
#define AXL_SMART_WARN_SPARE        0x01u  ///< available spare below threshold
#define AXL_SMART_WARN_TEMPERATURE  0x02u  ///< temperature threshold exceeded
#define AXL_SMART_WARN_RELIABILITY  0x04u  ///< media reliability degraded
#define AXL_SMART_WARN_READ_ONLY    0x08u  ///< media placed in read-only mode
#define AXL_SMART_WARN_BACKUP       0x10u  ///< volatile-memory backup failed

/**
 * @brief One storage device's normalized identity + health.
 *
 * `healthy` and `warning_flags` are always present — every transport reports an
 * overall pass/fail. Every other numeric field uses a documented per-field
 * sentinel when the transport cannot supply it, never a guessed value:
 * `temperature_c == INT32_MIN`,
 * `power_on_hours` / `power_cycles` / `media_errors` / `data_units_*` `==
 * UINT64_MAX`, `percent_used == 0xFF`. (These match AxlAtaSmart /
 * AxlNvmeSmart's sentinels, so a value carried across is carried unchanged.)
 */
typedef struct {
    AxlStorageTransport transport;     ///< which transport this device attaches through
    char     model[64];                ///< NVMe Model Number / ATA Model / SCSI "vendor product" (joined; 64 sizes the SCSI join)
    char     serial[32];               ///< serial number
    char     firmware[16];             ///< firmware / product revision

    bool     healthy;                  ///< overall pass/fail — the headline answer
    uint32_t warning_flags;            ///< normalized AXL_SMART_WARN_* bits (0 = none)

    int32_t  temperature_c;            ///< current temperature in C, or INT32_MIN
    uint64_t power_on_hours;           ///< power-on hours, or UINT64_MAX
    uint64_t power_cycles;             ///< power cycles, or UINT64_MAX
    uint8_t  percent_used;             ///< SSD endurance used 0..255 (NVMe Percentage Used; ATA/SCSI always 0xFF), 0xFF = unknown
    uint64_t media_errors;             ///< NVMe media errors / ATA reallocated sectors / SCSI IE count, or UINT64_MAX
    uint64_t data_units_read;          ///< 1000*512-byte units read (NVMe), or UINT64_MAX
    uint64_t data_units_written;       ///< 1000*512-byte units written (NVMe), or UINT64_MAX
} AxlSmartHealth;

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

/**
 * @brief Read a device's normalized identity + health.
 *
 * Dispatches by transport: issues the transport's identity + health reads
 * (Identify Controller + SMART log for NVMe; IDENTIFY DEVICE + SMART for ATA;
 * INQUIRY + LOG SENSE for SCSI) and normalizes the result into @p out. @p out
 * is fully populated — every absent field carries its sentinel.
 *
 * On AXL_ERR @p out is not modified.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args or when the device's health
 *     could not be read (e.g. an ATA device that fails SMART, or a SCSI LUN
 *     with no IE log — the per-transport reader's failure is surfaced).
 */
int
axl_smart_health(
    AxlStorageDev  *dev,   ///< device from axl_storage_next
    AxlSmartHealth *out    ///< [out] populated on success
);

// ===================================================================
// Pure normalizers (the per-transport mappings; the test seam)
// ===================================================================
//
// axl_smart_health is `read the transport's structs` plus one of these pure
// mappings. They are public so a consumer that already has the per-transport
// decoded structs (e.g. from a captured fixture) can normalize without a
// device, and they are the hardware-free unit-test seam. Each fully populates
// @p out (sentinels for fields the transport lacks) and never fails on a valid
// struct.

/**
 * @brief Normalize NVMe Identify Controller + SMART into AxlSmartHealth.
 *
 * Carries model/serial/firmware, the health verdict, temperature, power-on
 * hours, power cycles, percent-used, media errors, and data-units read/written
 * (NVMe supplies them all). warning_flags maps the Critical Warning bits.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args.
 */
int
axl_smart_from_nvme(
    const AxlNvmeController *id,   ///< Identify Controller (model/serial/fw)
    const AxlNvmeSmart      *s,    ///< SMART/Health log
    AxlSmartHealth          *out   ///< [out] normalized record
);

/**
 * @brief Normalize ATA IDENTIFY DEVICE + SMART into AxlSmartHealth.
 *
 * media_errors carries the reallocated-sector count; data_units_* are
 * UINT64_MAX (ATA has no universal data-units attribute); warning_flags is
 * AXL_SMART_WARN_RELIABILITY when unhealthy, else 0.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args.
 */
int
axl_smart_from_ata(
    const AxlAtaIdentify *id,   ///< IDENTIFY DEVICE (model/serial/fw)
    const AxlAtaSmart    *s,    ///< decoded SMART health
    AxlSmartHealth       *out   ///< [out] normalized record
);

/**
 * @brief Normalize SCSI INQUIRY + LOG SENSE health into AxlSmartHealth.
 *
 * model is "vendor product" (the two INQUIRY fields joined); firmware is the
 * product revision. power_on_hours / power_cycles / media_errors / data_units_*
 * are UINT64_MAX and percent_used is 0xFF (Phase-3 SCSI health does not supply
 * them — see axl-scsi.h); warning_flags is AXL_SMART_WARN_RELIABILITY when
 * unhealthy, else 0.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args.
 */
int
axl_smart_from_scsi(
    const AxlScsiInquiry *inq,   ///< standard INQUIRY + serial
    const AxlScsiHealth  *h,     ///< decoded IE-page health
    AxlSmartHealth       *out    ///< [out] normalized record
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SMART_H */
