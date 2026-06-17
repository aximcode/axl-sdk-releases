/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-ata.h:
 *
 * ATA/SATA device identity and health (SMART) over the firmware's
 * EFI_ATA_PASS_THRU_PROTOCOL. A Platform Access module and the ATA arm of
 * the storage-access family (AxlNvme / AxlAta / AxlScsi); see
 * docs/AXL-Storage-Design.md. It enumerates the ATA devices the firmware
 * exposes (one per controller port / port-multiplier port) and reports
 * each one's IDENTIFY DEVICE data and SMART health — the device view that
 * complements AxlBlock's logical block geometry.
 *
 * Unlike NVMe (where the controller is the unit and the SMART log is
 * controller-wide), ATA SMART is per-device, and a device is addressed by
 * (port, port-multiplier port) under a controller. So the unit of
 * enumeration is an opaque per-device handle, AxlAtaDev, walked with
 * axl_ata_next() — the consumer never juggles the (controller, port,
 * pmport) triple.
 *
 * Scope is read-and-health: IDENTIFY DEVICE, SMART (READ DATA + READ
 * THRESHOLDS for the attribute table and the overall pass/fail verdict),
 * and SMART EXECUTE OFF-LINE IMMEDIATE (the one active, non-destructive
 * self-test). Arbitrary task-file commands go through axl_ata_passthru();
 * the typed surface ships no data-destroying command (SECURITY ERASE,
 * firmware download) — assemble those through the raw entry point.
 *
 * @code
 * AxlAtaDev *dev = NULL;
 * while ((dev = axl_ata_next(dev)) != NULL) {
 *     AxlAtaIdentify id;
 *     AxlAtaSmart    s;
 *     if (axl_ata_identify(dev, &id) == AXL_OK
 *         && axl_ata_smart(dev, &s) == AXL_OK) {
 *         axl_printf("%s %s: %s, %d C\n", id.model, id.serial,
 *                    s.healthy ? "OK" : "FAILING", s.temperature_c);
 *     }
 * }
 * @endcode
 */

#ifndef AXL_ATA_H
#define AXL_ATA_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An ATA device (a disk on one controller port / PMP). Opaque and
 * owned by the enumeration; valid for the image lifetime, never freed by
 * the caller.
 */
typedef struct AxlAtaDev AxlAtaDev;

/**
 * @brief Iterate ATA/SATA devices across every ATA pass-thru controller.
 *
 * Cursor-style: pass NULL for the first device, then pass each returned
 * handle back for the next; returns NULL at the end (including when there
 * are no ATA devices). The device set is snapshotted on the first call and
 * cached for the image lifetime (the AxlBlock model); position is recovered
 * from the handle you pass back, so passing NULL restarts and independent
 * walks do not interfere. The handle is owned by the module — do not free.
 *
 * @return next ATA device, or NULL at end of enumeration.
 */
AxlAtaDev *
axl_ata_next(
    AxlAtaDev *prev   ///< previous device, or NULL to start
);

/**
 * @brief Report a device's controller port and port-multiplier port — the
 * address that identifies it under its controller (e.g. for display).
 *
 * @return AXL_OK on success; AXL_ERR on NULL args.
 */
int
axl_ata_get_address(
    const AxlAtaDev *dev,      ///< device from axl_ata_next
    uint16_t        *port,     ///< [out] controller port, or NULL
    uint16_t        *pmport    ///< [out] port-multiplier port, or NULL
);

// ===================================================================
// IDENTIFY DEVICE
// ===================================================================

/**
 * @brief Decoded ATA IDENTIFY DEVICE data.
 *
 * The model/serial/firmware strings are the ATA identity fields with the
 * spec's per-word byte-swap undone, trailing spaces trimmed, and
 * NUL-terminated. `size_blocks` is the user-addressable sector count
 * (48-bit when supported, else 28-bit); `capacity_bytes` is
 * `size_blocks * block_size`.
 */
typedef struct {
    char     model[41];        ///< Model Number (words 27..46), de-swapped + trimmed
    char     serial[21];       ///< Serial Number (words 10..19)
    char     firmware[9];      ///< Firmware Revision (words 23..26)
    uint64_t size_blocks;      ///< user-addressable logical blocks (LBA count)
    uint32_t block_size;       ///< logical sector size in bytes (usually 512)
    uint64_t capacity_bytes;   ///< size_blocks * block_size (convenience)
    bool     smart_supported;  ///< IDENTIFY word 82 bit 0 (SMART feature set)
} AxlAtaIdentify;

/**
 * @brief Issue IDENTIFY DEVICE and decode it.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args or a command failure.
 */
int
axl_ata_identify(
    AxlAtaDev      *dev,   ///< device from axl_ata_next
    AxlAtaIdentify *out    ///< [out] populated on success
);

// ===================================================================
// SMART / health
// ===================================================================

/**
 * @brief Decoded ATA SMART health.
 *
 * `healthy` is the overall pass/fail: false when any pre-fail attribute's
 * normalized value has fallen to or below its threshold (the smartctl
 * verdict). The numeric fields come from the well-known universal
 * attributes; an attribute the device does not report uses the documented
 * sentinel rather than a guess (matching AxlSmartHealth):
 * `temperature_c == INT32_MIN` and any 64-bit counter `== UINT64_MAX`.
 *
 * `percent_used` is **always 0xFF (unknown) for ATA**: there is no
 * universal SSD-endurance attribute — the candidate ids (231, 233, 177)
 * mean different things on different vendors (231 is even *temperature* on
 * some drives), so reporting a number would be a guess. A consumer that
 * knows its drive can read the raw attributes via axl_ata_passthru.
 */
typedef struct {
    bool     healthy;             ///< no pre-fail attribute at/below threshold
    int32_t  temperature_c;       ///< attribute 194 (or 190), or INT32_MIN
    uint64_t power_on_hours;      ///< attribute 9, or UINT64_MAX
    uint64_t power_cycles;        ///< attribute 12, or UINT64_MAX
    uint64_t reallocated_sectors; ///< attribute 5 raw, or UINT64_MAX
    uint8_t  percent_used;        ///< always 0xFF for ATA (no universal attr)
} AxlAtaSmart;

/**
 * @brief Read SMART (READ DATA + READ THRESHOLDS) and decode health.
 *
 * Fails (AXL_ERR) if the device does not support the SMART feature set
 * (see AxlAtaIdentify::smart_supported) or a command fails.
 *
 * @return AXL_OK on success; AXL_ERR otherwise.
 */
int
axl_ata_smart(
    AxlAtaDev   *dev,   ///< device from axl_ata_next
    AxlAtaSmart *out    ///< [out] populated on success
);

// ===================================================================
// Self-test (active, non-destructive)
// ===================================================================

/**
 * @brief SMART self-test routine (the EXECUTE OFF-LINE IMMEDIATE subcommand).
 */
typedef enum {
    AXL_ATA_SELF_TEST_ABORT    = 0,  /**< abort a running off-line test. */
    AXL_ATA_SELF_TEST_SHORT    = 1,  /**< short off-line self-test. */
    AXL_ATA_SELF_TEST_EXTENDED = 2   /**< extended off-line self-test. */
} AxlAtaSelfTest;

/**
 * @brief Start (or abort) a SMART off-line self-test.
 *
 * Non-destructive — the device exercises itself without touching host
 * data — but a device write, so never implied by a health read. Progress
 * and outcome are read with axl_ata_self_test_result().
 *
 * @return AXL_OK if accepted; AXL_ERR on NULL/unsupported/command failure.
 */
int
axl_ata_self_test_start(
    AxlAtaDev     *dev,    ///< device from axl_ata_next
    AxlAtaSelfTest kind    ///< which self-test, or AXL_ATA_SELF_TEST_ABORT
);

/**
 * @brief Result of the running / most recent self-test.
 *
 * From the SMART READ DATA self-test execution-status byte: `passed`
 * reflects the most recent completed run (`result_code == 0`),
 * `percent_complete` is meaningful only while `in_progress` (0 otherwise).
 */
typedef struct {
    bool    in_progress;       ///< a self-test is currently running
    uint8_t percent_complete;  ///< 0..100 while in_progress; 0 otherwise
    bool    passed;            ///< most recent completed self-test passed
    uint8_t result_code;       ///< ATA self-test execution status code (0 = ok)
} AxlAtaSelfTestResult;

/**
 * @brief Read the self-test execution status.
 *
 * @return AXL_OK on success; AXL_ERR on NULL args or command failure.
 */
int
axl_ata_self_test_result(
    AxlAtaDev            *dev,   ///< device from axl_ata_next
    AxlAtaSelfTestResult *out    ///< [out] populated on success
);

// ===================================================================
// Raw-buffer decoders (pure; the readers above delegate to these)
// ===================================================================
//
// As in AxlNvme: each typed reader is `pass-thru read the buffer(s)` plus
// one of these pure decoders. They are public for decoding a captured
// IDENTIFY / SMART blob without a device, and they are the hardware-free
// unit-test seam. Each rejects a buffer shorter than the structure
// (AXL_ERR). ATA IDENTIFY / SMART data structures are 512 bytes.

/**
 * @brief Decode a 512-byte IDENTIFY DEVICE data structure.
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_ata_decode_identify(
    const uint8_t  *id,    ///< IDENTIFY DEVICE bytes
    size_t          len,   ///< buffer length (>= 512)
    AxlAtaIdentify *out    ///< [out] populated on success
);

/**
 * @brief Decode SMART health from the attribute and threshold tables.
 *
 * @p data is SMART READ DATA (feature 0xD0) and @p thresholds is SMART
 * READ THRESHOLDS (0xD1), each 512 bytes. `healthy` is computed by
 * comparing each pre-fail attribute's normalized value against its
 * threshold; the numeric fields are read from the universal attributes.
 *
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_ata_decode_smart(
    const uint8_t *data,        ///< SMART READ DATA bytes (>= 512)
    const uint8_t *thresholds,  ///< SMART READ THRESHOLDS bytes (>= 512)
    size_t         len,         ///< length of each buffer (>= 512)
    AxlAtaSmart   *out          ///< [out] populated on success
);

/**
 * @brief Decode the self-test execution status from SMART READ DATA.
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_ata_decode_self_test(
    const uint8_t        *data,   ///< SMART READ DATA bytes (>= 512)
    size_t                len,    ///< buffer length (>= 512)
    AxlAtaSelfTestResult *out     ///< [out] populated on success
);

// ===================================================================
// Raw task-file pass-through (escape hatch)
// ===================================================================

/**
 * @brief An ATA command task file (the registers a caller programs).
 *
 * The `*_exp` fields are the 48-bit LBA "previous content" extensions;
 * leave them 0 for 28-bit commands. The device/head register selects the
 * addressing mode and device.
 */
typedef struct {
    uint8_t command;           ///< Command register (e.g. 0xEC IDENTIFY)
    uint8_t features;          ///< Features register
    uint8_t sector_count;      ///< Sector Count
    uint8_t lba_low;           ///< LBA Low (Sector Number)
    uint8_t lba_mid;           ///< LBA Mid (Cylinder Low)
    uint8_t lba_high;          ///< LBA High (Cylinder High)
    uint8_t device;            ///< Device/Head
    uint8_t features_exp;      ///< Features (exp, 48-bit)
    uint8_t sector_count_exp;  ///< Sector Count (exp)
    uint8_t lba_low_exp;       ///< LBA Low (exp)
    uint8_t lba_mid_exp;       ///< LBA Mid (exp)
    uint8_t lba_high_exp;      ///< LBA High (exp)
} AxlAtaCmd;

/**
 * @brief The task file a command returns (status + output registers).
 *
 * For SMART RETURN STATUS the verdict is in `lba_mid`/`lba_high`
 * (0x4F/0xC2 = passing, 0xF4/0x2C = a threshold exceeded).
 */
typedef struct {
    uint8_t status;        ///< Status register
    uint8_t error;         ///< Error register
    uint8_t sector_count;  ///< Sector Count
    uint8_t lba_low;       ///< LBA Low
    uint8_t lba_mid;       ///< LBA Mid
    uint8_t lba_high;      ///< LBA High
    uint8_t device;        ///< Device/Head
} AxlAtaResult;

/**
 * @brief Data-transfer protocol for a raw task-file command.
 */
typedef enum {
    AXL_ATA_NO_DATA  = 0,  /**< no data transfer. */
    AXL_ATA_PIO_IN   = 1,  /**< PIO data-in (device -> host). */
    AXL_ATA_PIO_OUT  = 2   /**< PIO data-out (host -> device). */
} AxlAtaProtocol;

/**
 * @brief Submit a raw ATA task-file command (the escape hatch).
 *
 * The typed readers are built on this. @p data / @p data_len carry the
 * transfer buffer per @p proto (NULL/0 for AXL_ATA_NO_DATA; for a data-in
 * command the buffer is written by the firmware and must be writable). The
 * returned task file is written to @p out_result when non-NULL. No safety
 * policy is applied — a destructive command assembled here will be issued.
 *
 * @return AXL_OK if the command completed without an ATA error (the Status
 *     register's ERR bit clear); AXL_ERR on bad args, transport failure, or
 *     a device error (the task file is still reported via @p out_result).
 */
int
axl_ata_passthru(
    AxlAtaDev       *dev,         ///< device from axl_ata_next
    const AxlAtaCmd *cmd,         ///< command task file
    AxlAtaProtocol   proto,       ///< data-transfer protocol
    void            *data,        ///< transfer buffer (NULL iff NO_DATA)
    size_t           data_len,    ///< transfer length in bytes
    AxlAtaResult    *out_result   ///< [out] returned task file, or NULL
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ATA_H */
