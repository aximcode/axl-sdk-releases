/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-scsi.h:
 *
 * SCSI / SAS device identity, geometry, and health over the firmware's
 * EFI_EXT_SCSI_PASS_THRU_PROTOCOL. A Platform Access module and the SCSI arm
 * of the storage-access family (AxlNvme / AxlAta / AxlScsi); see
 * docs/AXL-Storage-Design.md. It enumerates the SCSI logical units the
 * firmware exposes (one per target/LUN under each SCSI channel) and reports
 * each one's INQUIRY identity, READ CAPACITY geometry, and LOG SENSE health
 * — the device view that complements AxlBlock's logical block geometry.
 *
 * A SCSI device is addressed by (Target, LUN) under a controller, where the
 * Target is the 16-byte EFI_EXT_SCSI_PASS_THRU target id. As with AxlAta, the
 * unit of enumeration is an opaque per-device handle, AxlScsiDev, walked with
 * axl_scsi_next() — the consumer never juggles the (controller, target, lun)
 * triple.
 *
 * Scope is read-and-health: INQUIRY (standard data + the Unit Serial Number
 * VPD page), READ CAPACITY (16), and LOG SENSE of the Informational
 * Exceptions page (overall pass/fail) plus the Temperature page. Two things
 * are intentionally NOT in the typed surface and are reached through the raw
 * axl_scsi_passthru() escape hatch (which carries any CDB):
 *   - **Self-test** (SEND DIAGNOSTIC + the self-test results log) — a device
 *     write with a divergent progress model, not emulated by common virtual
 *     SCSI HBAs.
 *   - **The stable device designator** (INQUIRY VPD page 0x83 / NAA WWN) — a
 *     planned follow-on; the vendor/product/serial triple identifies a device
 *     for display but is not globally unique.
 *
 * @code
 * AxlScsiDev *dev = NULL;
 * while ((dev = axl_scsi_next(dev)) != NULL) {
 *     AxlScsiInquiry inq;
 *     AxlScsiHealth  h;
 *     if (axl_scsi_inquiry(dev, &inq) == AXL_OK) {
 *         axl_scsi_health(dev, &h);   // best-effort; not all devices log it
 *         axl_printf("%s %s %s: %s\n", inq.vendor, inq.product, inq.serial,
 *                    h.healthy ? "OK" : "FAILING");
 *     }
 * }
 * @endcode
 */

#ifndef AXL_SCSI_H
#define AXL_SCSI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Length of a SCSI target identifier (EFI_EXT_SCSI_PASS_THRU TARGET_MAX_BYTES).
#define AXL_SCSI_TARGET_LEN  16

/// Peripheral device types (INQUIRY byte 0, bits 0..4) — the common ones.
#define AXL_SCSI_DEVTYPE_DISK      0x00  ///< direct-access block device (disk)
#define AXL_SCSI_DEVTYPE_TAPE      0x01  ///< sequential-access (tape)
#define AXL_SCSI_DEVTYPE_CDROM     0x05  ///< CD/DVD
#define AXL_SCSI_DEVTYPE_SES       0x0D  ///< SCSI enclosure services
#define AXL_SCSI_DEVTYPE_RBC       0x0E  ///< reduced-block-command device
#define AXL_SCSI_DEVTYPE_UNKNOWN   0x1F  ///< unknown / no device type

/**
 * @brief A SCSI logical unit (one target/LUN under a controller). Opaque and
 * owned by the enumeration; valid for the image lifetime, never freed by the
 * caller.
 */
typedef struct AxlScsiDev AxlScsiDev;

/**
 * @brief Iterate SCSI logical units across every Ext SCSI pass-thru controller.
 *
 * Cursor-style: pass NULL for the first device, then pass each returned handle
 * back for the next; returns NULL at the end (including when there are no SCSI
 * devices). The device set is snapshotted on the first call and cached for the
 * image lifetime (the AxlBlock / AxlAta model); position is recovered from the
 * handle you pass back, so passing NULL restarts and independent walks do not
 * interfere. The handle is owned by the module — do not free.
 *
 * @return next SCSI device, or NULL at end of enumeration.
 */
AxlScsiDev *
axl_scsi_next(
    AxlScsiDev *prev   ///< previous device, or NULL to start
);

/**
 * @brief Report a device's SCSI target id and LUN — the address that
 * identifies it under its controller (e.g. for display).
 *
 * @p target receives the AXL_SCSI_TARGET_LEN-byte target identifier; pass NULL
 * if only the LUN is wanted.
 *
 * @return AXL_OK on success; AXL_ERR on NULL @p dev.
 */
int
axl_scsi_get_address(
    const AxlScsiDev *dev,                        ///< device from axl_scsi_next
    uint8_t           target[AXL_SCSI_TARGET_LEN],///< [out] 16-byte target id, or NULL
    uint64_t         *lun                         ///< [out] logical unit number, or NULL
);

// ===================================================================
// INQUIRY (identity)
// ===================================================================

/**
 * @brief Decoded SCSI INQUIRY identity.
 *
 * The vendor / product / revision strings are the standard INQUIRY data
 * fields (T10 Vendor Identification, Product Identification, Product Revision
 * Level), space-trimmed and NUL-terminated. `serial` is the Unit Serial
 * Number VPD page (0x80) when the device supports it, else "". `device_type`
 * is the peripheral device type (0 = direct-access block device / disk,
 * 5 = CD/DVD, 1 = tape, ...). `removable` is the RMB bit.
 */
typedef struct {
    char    vendor[9];     ///< T10 vendor id (bytes 8..15), trimmed
    char    product[17];   ///< product id (bytes 16..31), trimmed
    char    revision[5];   ///< product revision (bytes 32..35), trimmed
    char    serial[21];    ///< Unit Serial Number (VPD page 0x80), or ""
    uint8_t device_type;   ///< peripheral device type (byte 0, bits 0..4)
    bool    removable;     ///< removable medium bit (byte 1, bit 7)
} AxlScsiInquiry;

/**
 * @brief Issue INQUIRY (standard data + the Unit Serial Number VPD page) and
 * decode it.
 *
 * The serial read is best-effort: a device that does not support VPD page 0x80
 * still returns AXL_OK with `serial == ""`.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args or a command failure.
 */
AXL_WARN_UNUSED int
axl_scsi_inquiry(
    AxlScsiDev     *dev,   ///< device from axl_scsi_next
    AxlScsiInquiry *out    ///< [out] populated on success
);

// ===================================================================
// READ CAPACITY (geometry)
// ===================================================================

/**
 * @brief Decoded SCSI READ CAPACITY (16) geometry.
 *
 * `size_blocks` is the number of logical blocks (returned last-LBA + 1);
 * `block_size` is the logical block length in bytes; `capacity_bytes` is the
 * product (a convenience).
 */
typedef struct {
    uint64_t size_blocks;     ///< logical block count (last LBA + 1)
    uint32_t block_size;      ///< logical block length in bytes
    uint64_t capacity_bytes;  ///< size_blocks * block_size (convenience)
} AxlScsiCapacity;

/**
 * @brief Issue READ CAPACITY (16) and decode the geometry.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args or a command failure.
 */
AXL_WARN_UNUSED int
axl_scsi_read_capacity(
    AxlScsiDev      *dev,   ///< device from axl_scsi_next
    AxlScsiCapacity *out    ///< [out] populated on success
);

// ===================================================================
// Health (LOG SENSE — Informational Exceptions)
// ===================================================================

/**
 * @brief Decoded SCSI health from the Informational Exceptions log page.
 *
 * `healthy` is the overall pass/fail verdict: false when the device reports an
 * informational-exception additional sense code (`asc != 0`) — the SCSI
 * equivalent of a tripped SMART threshold. `asc` / `ascq` carry that sense
 * code (e.g. 0x5D/0x10 = hardware failure predicted). `temperature_c` is the
 * most-recent temperature in C, or INT32_MIN when unavailable (matching
 * AxlAtaSmart / AxlSmartHealth's sentinel): the IE log parameter carries an
 * optional temperature byte, but many devices omit it, so axl_scsi_health
 * falls back to the dedicated Temperature log page (0x0D) — see there.
 *
 * Counters AxlSmartHealth rolls up from other sources are intentionally NOT
 * here: a SCSI grown-defect / media-error count and power-on hours live in
 * separate log pages (defect list, Start-Stop Cycle Counter) outside this
 * Phase-3 read-and-health scope. The AxlSmart rollup reports them as
 * UINT64_MAX for SCSI devices (mirroring how AxlAtaSmart documents
 * `percent_used == 0xFF`); a consumer that needs them reads those pages via
 * axl_scsi_passthru.
 */
typedef struct {
    bool    healthy;        ///< no informational exception reported (asc == 0)
    int32_t temperature_c;  ///< current temperature in C, or INT32_MIN
    uint8_t asc;            ///< IE additional sense code (0 = no exception)
    uint8_t ascq;           ///< IE additional sense code qualifier
} AxlScsiHealth;

/**
 * @brief Read SCSI health: LOG SENSE of the Informational Exceptions page
 * (0x2F) for the pass/fail verdict, plus the Temperature page (0x0D) for the
 * current temperature when the IE page omits it.
 *
 * Best-effort by nature: a device that does not implement the IE log page
 * fails (AXL_ERR). A device that implements it but is healthy returns AXL_OK
 * with `healthy == true` and `asc == 0`. The Temperature page read is itself
 * best-effort — `temperature_c` stays INT32_MIN if neither page reports it,
 * and a missing 0x0D page never fails the call.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args or an IE-page command failure.
 */
AXL_WARN_UNUSED int
axl_scsi_health(
    AxlScsiDev    *dev,   ///< device from axl_scsi_next
    AxlScsiHealth *out    ///< [out] populated on success
);

// ===================================================================
// Raw-buffer decoders (pure; the readers above delegate to these)
// ===================================================================
//
// As in AxlNvme / AxlAta: each typed reader is `pass-thru read the buffer`
// plus one of these pure decoders. They are public for decoding a captured
// INQUIRY / READ CAPACITY / LOG SENSE blob without a device, and they are the
// hardware-free unit-test seam. Each rejects a buffer shorter than the
// structure it needs (AXL_ERR).
//
// SCSI parameter data is BIG-ENDIAN (unlike NVMe's little-endian); these
// decoders consume the on-the-wire bytes directly. The decoders that fill an
// AxlScsiInquiry / AxlScsiHealth populate only the fields documented for their
// page (e.g. axl_scsi_decode_inquiry leaves `serial` untouched) — a caller
// decoding captured blobs should zero the output struct first, then call each
// relevant decoder.

/**
 * @brief Decode standard INQUIRY data (vendor / product / revision / type).
 *
 * Fills everything in AxlScsiInquiry except `serial` (which comes from the VPD
 * page and is left untouched). The standard INQUIRY data is at least 36 bytes.
 *
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_scsi_decode_inquiry(
    const uint8_t  *data,   ///< standard INQUIRY data bytes
    size_t          len,    ///< buffer length (>= 36)
    AxlScsiInquiry *out     ///< [out] populated on success (serial untouched)
);

/**
 * @brief Extract the unit serial number from a VPD page 0x80 buffer.
 *
 * Copies the page's product-serial-number field (trimmed, NUL-terminated) into
 * @p out. A zero-length page yields "".
 *
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_scsi_decode_serial(
    const uint8_t *vpd80,    ///< VPD page 0x80 bytes (4-byte header + serial)
    size_t         len,      ///< buffer length (>= 4)
    char          *out,      ///< [out] serial string (truncated to @p out_size)
    size_t         out_size  ///< capacity of @p out (21 matches AxlScsiInquiry.serial; longer SAS serials are truncated)
);

/**
 * @brief Decode a READ CAPACITY (16) parameter data buffer (>= 16 bytes).
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_scsi_decode_capacity(
    const uint8_t   *data,   ///< READ CAPACITY (16) parameter bytes
    size_t           len,    ///< buffer length (>= 16)
    AxlScsiCapacity *out     ///< [out] populated on success
);

/**
 * @brief Decode health from an Informational Exceptions log page (0x2F).
 *
 * @p data is the LOG SENSE parameter data: a 4-byte log-page header followed
 * by the IE log parameter (the IE ASC/ASCQ, then an optional temperature
 * byte). Sets `healthy`/`asc`/`ascq` from the sense code, and
 * `temperature_c` from the IE temperature byte when the parameter is long
 * enough to include it, else INT32_MIN (the caller may then fill it from the
 * Temperature page via axl_scsi_decode_temperature). A buffer too short to
 * contain the IE parameter is rejected.
 *
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_scsi_decode_health(
    const uint8_t *data,   ///< IE log page bytes (LOG SENSE of page 0x2F)
    size_t         len,    ///< buffer length
    AxlScsiHealth *out     ///< [out] populated on success
);

/**
 * @brief Decode the current temperature from a Temperature log page (0x0D).
 *
 * @p data is the LOG SENSE parameter data for page 0x0D: a 4-byte log-page
 * header followed by the Temperature parameter (0x0000), whose byte 5 is the
 * current temperature in degrees Celsius. A reading of 0xFF means "not
 * available" and is decoded as INT32_MIN.
 *
 * @return AXL_OK on success (out set to the temperature or INT32_MIN); AXL_ERR
 *     on a short buffer or NULL args.
 */
int
axl_scsi_decode_temperature(
    const uint8_t *data,   ///< Temperature log page bytes (LOG SENSE of page 0x0D)
    size_t         len,    ///< buffer length
    int32_t       *out     ///< [out] temperature in C, or INT32_MIN
);

// ===================================================================
// Raw CDB pass-through (escape hatch)
// ===================================================================

/**
 * @brief Data-transfer direction for a raw CDB.
 */
typedef enum {
    AXL_SCSI_NO_DATA  = 0,  /**< no data transfer. */
    AXL_SCSI_DATA_IN  = 1,  /**< data-in (device -> host). */
    AXL_SCSI_DATA_OUT = 2   /**< data-out (host -> device). */
} AxlScsiDir;

/**
 * @brief Submit a raw SCSI command descriptor block (the escape hatch).
 *
 * The typed readers are built on this. @p cdb / @p cdb_len carry the CDB (6,
 * 10, 12, or 16 bytes). @p data / @p data_len carry the transfer buffer per
 * @p dir (NULL/0 for AXL_SCSI_NO_DATA; for a data-in command the buffer is
 * written by the firmware and must be writable). @p data_len is the
 * allocation length; @p data_transferred (when non-NULL) receives how many
 * bytes were actually transferred — necessary for the variable-length reads
 * (INQUIRY allocation vs returned length, LOG SENSE / VPD page lengths) the
 * typed readers do. It is only meaningful on AXL_OK.
 *
 * On a CHECK CONDITION the device's sense data is written to @p sense (up to
 * @p *sense_len bytes) and @p *sense_len is set to the length returned; on
 * any other status @p *sense_len is set to 0. @p sense and @p sense_len must
 * both be non-NULL or both NULL (pass both NULL to discard sense). No safety
 * policy is applied — a destructive CDB assembled here (FORMAT UNIT, WRITE,
 * ...) will be issued.
 *
 * @return AXL_OK if the adapter completed the command and the target returned
 *     GOOD status; AXL_ERR on bad args, transport failure, or any non-GOOD
 *     target status (a CHECK CONDITION's sense data is still reported via
 *     @p sense).
 */
int
axl_scsi_passthru(
    AxlScsiDev    *dev,              ///< device from axl_scsi_next
    const uint8_t *cdb,             ///< command descriptor block
    size_t         cdb_len,         ///< CDB length (6/10/12/16)
    AxlScsiDir     dir,             ///< data-transfer direction
    void          *data,            ///< transfer buffer (NULL iff NO_DATA)
    size_t         data_len,        ///< transfer allocation length in bytes
    size_t        *data_transferred,///< [out] bytes actually transferred, or NULL
    uint8_t       *sense,           ///< [out] sense data on CHECK CONDITION, or NULL
    size_t        *sense_len        ///< [in,out] sense capacity / length, or NULL
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SCSI_H */
