/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-nvme.h
 *
 * NVM Express device identity and health (SMART) over the firmware's
 * EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL. A Platform Access module (like
 * AxlPci / AxlUsb / AxlBlock): it enumerates the NVMe controllers the
 * firmware exposes and reports each one's Identify data, namespaces, and
 * SMART/Health log — the device view that complements AxlBlock's logical
 * block geometry.
 *
 * Scope is read-and-health: Identify (Controller + Namespace), the
 * SMART/Health log (Get Log Page 0x02), and the one active but
 * non-destructive command, Device Self-test. Arbitrary admin commands go
 * through axl_nvme_admin_passthru(); the typed surface ships no
 * data-destroying command (Format NVM, Sanitize, firmware download) — a
 * caller wanting one assembles it through the raw entry point and owns the
 * consequences.
 *
 * @code
 * // smartctl-style health sweep over every NVMe controller.
 * AxlHandle ctrl = NULL;
 * while ((ctrl = axl_nvme_next(ctrl)) != NULL) {
 *     AxlNvmeController c;
 *     AxlNvmeSmart      s;
 *     if (axl_nvme_identify_controller(ctrl, &c) == AXL_OK
 *         && axl_nvme_smart(ctrl, &s) == AXL_OK) {
 *         axl_printf("%s %s: %s, %d C, %u%% used\n",
 *                    c.model, c.serial, s.healthy ? "OK" : "FAILING",
 *                    s.temperature_c, s.percent_used);
 *     }
 * }
 * @endcode
 *
 * Device-path text needs no new API: the controller AxlHandle resolves
 * through `axl_handle_get_protocol(ctrl, "device-path", ...)` +
 * `axl_device_path_to_text()` (both in `<axl/axl-sys.h>`), the same way
 * AxlBlock correlates a device with its block handle.
 *
 * A controller is the unit of enumeration because the pass-thru protocol,
 * the Identify Controller data, and the SMART log are all controller-wide
 * (the SMART log is read with NSID 0xFFFFFFFF). Namespaces — the
 * addressable capacities, usually exactly one per SSD — are walked within
 * a controller with axl_nvme_namespace_next().
 */

#ifndef AXL_NVME_H
#define AXL_NVME_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>   /* AxlHandle */

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Enumeration
// ===================================================================

/**
 * @brief Iterate NVMe controllers (handles publishing NVMe pass-thru).
 *
 * Cursor-style enumeration matching axl_block_next: pass NULL for the
 * first controller, then pass each returned handle back for the next;
 * returns NULL at the end (including when there are no NVMe controllers).
 *
 * The handle set is snapshotted on the first call and cached for the image
 * lifetime; position is recovered from the handle you pass back, not a
 * hidden cursor, so passing NULL restarts and independent walks do not
 * interfere. The handle is firmware-owned (do not free) and is valid for
 * every axl_nvme_* call below and for
 * `axl_handle_get_protocol(ctrl, "device-path", ...)`.
 *
 * @return next NVMe controller handle, or NULL at end of enumeration.
 */
AxlHandle
axl_nvme_next(
    AxlHandle prev   ///< previous controller handle, or NULL to start
);

/**
 * @brief Advance to the next active namespace id on a controller.
 *
 * NVMe namespace ids are 1-based; pass 0 to get the first active
 * namespace, then pass each returned id back for the next. Returns 0 when
 * there are no more (so 0 is never itself a valid namespace id). Walks via
 * the controller's firmware iterator (GetNextNamespace), so a return of 0
 * means the active-namespace list is exhausted — or, indistinguishably,
 * that it could not be read; either way there is nothing more to walk.
 *
 * @return next active namespace id, or 0 at end of enumeration.
 */
uint32_t
axl_nvme_namespace_next(
    AxlHandle ctrl,        ///< controller handle from axl_nvme_next
    uint32_t  prev_nsid    ///< previous namespace id, or 0 to start
);

// ===================================================================
// Identify
// ===================================================================

/**
 * @brief Identify Controller data, decoded.
 *
 * The model/serial/firmware strings are the ASCII Identify fields with
 * trailing spaces trimmed and NUL-terminated. `nvme_version` is the
 * controller's reported NVMe spec version as BCD (e.g. 0x00010400 for
 * 1.4.0), or 0 if the controller predates the version field.
 */
typedef struct {
    char     model[41];          ///< Model Number (Identify bytes 24..63), trimmed
    char     serial[21];         ///< Serial Number (bytes 4..23), trimmed
    char     firmware[9];        ///< Firmware Revision (bytes 64..71), trimmed
    uint16_t pci_vid;            ///< PCI Vendor ID
    uint16_t pci_ssvid;          ///< PCI Subsystem Vendor ID
    uint32_t nvme_version;       ///< NVMe spec version (BCD), or 0 if unreported
    uint32_t namespace_count;    ///< Number of Namespaces (NN)
} AxlNvmeController;

/**
 * @brief Read and decode the Identify Controller data structure.
 *
 * @return AXL_OK on success; AXL_ERR if @p ctrl does not publish NVMe
 *     pass-thru, @p out is NULL, or the admin command fails.
 */
int
axl_nvme_identify_controller(
    AxlHandle          ctrl,   ///< controller handle from axl_nvme_next
    AxlNvmeController *out     ///< [out] populated on success
);

/**
 * @brief Identify Namespace data, decoded.
 *
 * `capacity_bytes` is `size_blocks * block_size`. A namespace that exists
 * but is not active reports `size_blocks == 0`.
 */
typedef struct {
    uint32_t nsid;               ///< namespace id this describes
    uint32_t block_size;         ///< logical block (LBA) size in bytes, per the active LBA format
    uint64_t size_blocks;        ///< namespace size in logical blocks (NSZE)
    uint64_t capacity_bytes;     ///< size_blocks * block_size (convenience)
} AxlNvmeNamespace;

/**
 * @brief Read and decode Identify Namespace for @p nsid.
 *
 * @return AXL_OK on success; AXL_ERR on a bad handle, NULL @p out, an
 *     inactive/unknown @p nsid, or admin-command failure.
 */
int
axl_nvme_identify_namespace(
    AxlHandle         ctrl,    ///< controller handle from axl_nvme_next
    uint32_t          nsid,    ///< namespace id (from axl_nvme_namespace_next)
    AxlNvmeNamespace *out      ///< [out] populated on success
);

// ===================================================================
// SMART / Health (Get Log Page 0x02)
// ===================================================================

/**
 * @brief Decoded NVMe SMART / Health Information log (controller-wide).
 *
 * Counters the spec stores as 128-bit values are returned as 64-bit; they
 * saturate at UINT64_MAX rather than wrapping (a drive does not reach 2^64
 * in any of these in practice). `data_units_read` / `_written` are in the
 * spec's units of 1000 * 512 bytes (the unit AxlSmartHealth normalizes to,
 * so AxlSmart copies them straight through).
 *
 * `healthy` is the headline: true iff `critical_warning == 0`. The
 * individual `warn_*` bools decode the Critical Warning byte for a caller
 * that wants the specific condition.
 *
 * Absent-field sentinels (matching AxlSmartHealth, so AxlSmart needs no
 * per-transport remapping): a field the controller does not report reads
 * as `temperature_c == INT32_MIN` (the Composite Temperature is optional;
 * a 0-Kelvin readout is treated as unreported, not -273 C),
 * `percent_used == 0xFF`, and any 64-bit counter `== UINT64_MAX`.
 * `healthy`, `critical_warning`, and the `warn_*` bools are always present.
 */
typedef struct {
    bool     healthy;                  ///< critical_warning == 0 (overall pass/fail)
    uint8_t  critical_warning;         ///< raw Critical Warning bitfield (byte 0)
    bool     warn_spare_low;           ///< available spare below threshold
    bool     warn_temperature;         ///< temperature above/below a threshold
    bool     warn_reliability;         ///< media subsystem degraded (reliability)
    bool     warn_read_only;           ///< media placed in read-only mode
    bool     warn_volatile_backup;     ///< volatile-memory backup device failed

    int32_t  temperature_c;            ///< Composite Temperature in Celsius
    uint8_t  available_spare;          ///< available spare as a percentage (0..100)
    uint8_t  available_spare_threshold;///< spare threshold percentage
    uint8_t  percent_used;          ///< endurance used, 0..255 (>100 is allowed)

    uint64_t data_units_read;          ///< 1000*512-byte units read from the media
    uint64_t data_units_written;       ///< 1000*512-byte units written to the media
    uint64_t host_read_commands;       ///< host read commands completed
    uint64_t host_write_commands;      ///< host write commands completed
    uint64_t power_cycles;             ///< power cycles
    uint64_t power_on_hours;           ///< power-on hours
    uint64_t unsafe_shutdowns;         ///< unsafe shutdowns
    uint64_t media_errors;             ///< media and data-integrity errors
    uint64_t error_log_entries;        ///< number of error-information log entries
} AxlNvmeSmart;

/**
 * @brief Read and decode the controller-wide SMART/Health log (LID 0x02).
 *
 * The log is read with NSID 0xFFFFFFFF, so it is controller-wide: every
 * namespace on @p ctrl shares this one health record (NVMe has no
 * mandatory per-namespace SMART). AxlSmart attaches the same record to
 * each namespace device on the controller.
 *
 * @return AXL_OK on success; AXL_ERR on a bad handle, NULL @p out, or
 *     admin-command failure.
 */
int
axl_nvme_smart(
    AxlHandle     ctrl,   ///< controller handle from axl_nvme_next
    AxlNvmeSmart *out     ///< [out] populated on success
);

// ===================================================================
// Device Self-test (active, non-destructive)
// ===================================================================

/**
 * @brief Device Self-test operation.
 */
/* These are AXL enum values, not the wire STC codes; the implementation
   maps them (ABORT -> STC 0xF, SHORT -> 0x1, EXTENDED -> 0x2). */
typedef enum {
    AXL_NVME_SELF_TEST_ABORT    = 0,  /**< abort a running self-test. */
    AXL_NVME_SELF_TEST_SHORT    = 1,  /**< short self-test. */
    AXL_NVME_SELF_TEST_EXTENDED = 2   /**< extended self-test. */
} AxlNvmeSelfTest;

/**
 * @brief Start (or abort) a Device Self-test.
 *
 * Issues Device Self-test (admin opcode 0x14) over all namespaces
 * (NSID 0xFFFFFFFF). The operation runs in the device's background; poll
 * axl_nvme_self_test_result() for progress and outcome. It is
 * non-destructive — the device exercises itself without touching host
 * data — but it IS a device write, so it is never issued implicitly by a
 * health read.
 *
 * @return AXL_OK if the self-test was accepted (or aborted); AXL_ERR on a
 *     bad handle, an unsupported operation, or admin-command failure.
 */
int
axl_nvme_self_test_start(
    AxlHandle       ctrl,   ///< controller handle from axl_nvme_next
    AxlNvmeSelfTest kind    ///< which self-test, or AXL_NVME_SELF_TEST_ABORT
);

/**
 * @brief Result of the most recent / running Device Self-test.
 *
 * `passed`/`result_code` always describe the most recent *completed*
 * self-test (the newest entry in the Device Self-test log), independent of
 * whether one is running now. `result_code` is the NVMe Self-test Result
 * code: 0 = completed without error, non-zero = a specific failure/abort.
 * If no self-test has ever completed, `result_code == 0x0F` (the NVMe
 * "entry unused" value) and `passed == false`.
 *
 * `percent_complete` is meaningful only when `in_progress` is true; it
 * reads 0 otherwise.
 */
typedef struct {
    bool    in_progress;       ///< a self-test is currently running
    uint8_t percent_complete;  ///< 0..100 when in_progress; 0 otherwise
    bool    passed;            ///< most recent completed self-test passed (result_code == 0)
    uint8_t result_code;       ///< NVMe Self-test Result code of the last completed run; 0x0F if none
} AxlNvmeSelfTestResult;

/**
 * @brief Read the Device Self-test log (LID 0x06) and decode the status.
 *
 * @return AXL_OK on success; AXL_ERR on a bad handle, NULL @p out, or
 *     admin-command failure (including a controller that does not support
 *     Device Self-test).
 */
int
axl_nvme_self_test_result(
    AxlHandle              ctrl,   ///< controller handle from axl_nvme_next
    AxlNvmeSelfTestResult *out     ///< [out] populated on success
);

// ===================================================================
// Raw-buffer decoders (pure; the handle readers delegate to these)
// ===================================================================
//
// Each typed reader above is `pass-thru read the buffer` + one of these
// pure decode functions. The decoders are public in their own right: a
// caller holding a raw Identify / SMART / self-test buffer it obtained
// some other way (a captured fixture blob, a buffer carried over Redfish)
// can decode it without a device. They are also the hardware-free unit-
// test seam — exercised against committed real-device blobs.
//
// Each takes the raw little-endian NVMe data structure and its length;
// a buffer shorter than the structure requires is rejected (AXL_ERR).

/**
 * @brief Decode an Identify Controller data structure (>= 4096 bytes).
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_nvme_decode_identify_controller(
    const uint8_t     *id,    ///< Identify Controller bytes
    size_t             len,   ///< buffer length (>= 4096)
    AxlNvmeController *out     ///< [out] populated on success
);

/**
 * @brief Decode an Identify Namespace data structure (>= 4096 bytes).
 *
 * The namespace id is not carried in the structure, so the caller passes
 * the @p nsid the buffer was fetched for; it is copied into @p out.
 *
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_nvme_decode_identify_namespace(
    const uint8_t    *id,     ///< Identify Namespace bytes
    size_t            len,    ///< buffer length (>= 4096)
    uint32_t          nsid,   ///< namespace id this buffer describes
    AxlNvmeNamespace *out     ///< [out] populated on success
);

/**
 * @brief Decode a SMART/Health Information log page (>= 512 bytes).
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_nvme_decode_smart(
    const uint8_t *log,   ///< SMART/Health log bytes (LID 0x02)
    size_t         len,   ///< buffer length (>= 512)
    AxlNvmeSmart  *out    ///< [out] populated on success
);

/**
 * @brief Decode a Device Self-test log page (>= 564 bytes).
 * @return AXL_OK on success; AXL_ERR on a short buffer or NULL args.
 */
int
axl_nvme_decode_self_test_log(
    const uint8_t         *log,   ///< Device Self-test log bytes (LID 0x06)
    size_t                 len,   ///< buffer length (>= 564)
    AxlNvmeSelfTestResult *out    ///< [out] populated on success
);

// ===================================================================
// Raw admin pass-through (escape hatch)
// ===================================================================

/**
 * @brief Direction of the data transfer for a raw admin command.
 */
typedef enum {
    AXL_NVME_NO_DATA  = 0,  /**< no data buffer. */
    AXL_NVME_DATA_IN  = 1,  /**< controller -> host (read; e.g. Identify, Get Log Page). The buffer is written by the firmware and must be writable. */
    AXL_NVME_DATA_OUT = 2   /**< host -> controller (write). */
} AxlNvmeDataDir;

/**
 * @brief An NVMe admin command (the command Dwords a caller controls).
 *
 * Mirrors the admin Submission Queue Entry fields a pass-thru exposes:
 * the opcode, the namespace id, and command Dwords 10..15. The data
 * pointer/length and direction are passed separately to
 * axl_nvme_admin_passthru().
 */
typedef struct {
    uint8_t  opcode;   ///< admin command opcode (CDW0 OPC)
    uint32_t nsid;     ///< namespace id (or 0xFFFFFFFF for controller-wide)
    uint32_t cdw10;    ///< command Dword 10
    uint32_t cdw11;    ///< command Dword 11
    uint32_t cdw12;    ///< command Dword 12
    uint32_t cdw13;    ///< command Dword 13
    uint32_t cdw14;    ///< command Dword 14
    uint32_t cdw15;    ///< command Dword 15
} AxlNvmeAdminCmd;

/**
 * @brief Submit a raw admin command (the escape hatch under the typed API).
 *
 * The typed wrappers above are built on this; reach for it directly only
 * for commands AXL does not type. @p data / @p data_len carry the command's
 * data buffer per @p dir (NULL/0 for AXL_NVME_NO_DATA). The completion
 * Dword 0 is written to @p cqe_dw0 when non-NULL (some commands return a
 * result there), and the NVMe Status Field (the 15-bit SCT||SC from
 * completion Dword 3) is written to @p cqe_status when non-NULL — set on
 * both success (0) and a command-level failure, so a caller assembling a
 * vendor command can see *why* it failed rather than only that it did.
 *
 * Metadata buffers (the MPTR / separate-metadata transfer) are not
 * supported through this entry point; a command needing them must use the
 * pass-thru protocol directly. The Phase-1 typed wrappers (Identify, Get
 * Log Page, Device Self-test) do not use metadata.
 *
 * No safety policy is applied — a destructive opcode assembled here (Format
 * NVM, Sanitize, firmware commit) will be issued. The caller owns that.
 *
 * @return AXL_OK if the command completed with a zero NVMe status; AXL_ERR
 *     on a bad handle, a transport failure, or a non-zero NVMe completion
 *     status (the status is still reported via @p cqe_status).
 */
int
axl_nvme_admin_passthru(
    AxlHandle              ctrl,       ///< controller handle from axl_nvme_next
    const AxlNvmeAdminCmd *cmd,        ///< command to submit
    AxlNvmeDataDir         dir,        ///< data transfer direction
    void                  *data,       ///< data buffer (NULL iff dir == NO_DATA)
    size_t                 data_len,   ///< data buffer length in bytes
    uint32_t              *cqe_dw0,    ///< [out] completion Dword 0, or NULL
    uint16_t              *cqe_status  ///< [out] NVMe Status Field (0 = success), or NULL
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_NVME_H */
