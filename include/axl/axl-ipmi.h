/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-ipmi.h:
 *
 * Local BMC access via IPMI transports (KCS, SSIF, EDKII vendor
 * protocol, Dell vendor protocol). Auto-detects the best available
 * transport using SMBIOS Type 38 plus firmware protocol lookup.
 *
 * Public surface is the raw-command entry point; typed command
 * wrappers (get_device_id, chassis_status, sdr_*, sel_*, fru_*,
 * etc.) are added incrementally as consumers need them.
 */

#ifndef AXL_IPMI_H
#define AXL_IPMI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/**
 * AxlIpmiSession:
 *
 * Opaque handle for a selected IPMI transport. Created via
 * axl_ipmi_session_new() and freed with axl_ipmi_session_free().
 */
typedef struct AxlIpmiSession AxlIpmiSession;

/**
 * @brief Transport kind currently driving a session.
 */
typedef enum {
    AXL_IPMI_TRANSPORT_UNKNOWN = 0,  ///< No transport available
    AXL_IPMI_TRANSPORT_KCS,          ///< Keyboard Controller Style (x86 I/O ports)
    AXL_IPMI_TRANSPORT_SSIF,         ///< SMBus System Interface (I2C)
    AXL_IPMI_TRANSPORT_EDKII,        ///< EDKII IPMI_PROTOCOL (firmware provides)
    AXL_IPMI_TRANSPORT_DELL          ///< Dell EFI_IPMI_TRANSPORT
} AxlIpmiTransport;

/**
 * @brief Open an IPMI session against the local BMC.
 *
 * Auto-detects the best available transport in priority order:
 *
 *   1. EDKII IPMI_PROTOCOL (gIpmiProtocolGuid)
 *   2. Dell EFI_IPMI_TRANSPORT (gDellIpmiProtocolGuid)
 *   3. SMBIOS Type 38 — KCS on x86 (I/O at 0x0CA2/0x0CA3 by default)
 *      or SSIF on ARM (slave 0x20 by default)
 *
 * Returns NULL within microseconds when no live transport responds
 * (including the case where SMBIOS Type 38 advertises a KCS interface
 * but no BMC is actually present). Use `axl_ipmi_probe()` for a
 * deeper snapshot of what the firmware exposes when this returns NULL.
 *
 * @return session handle, or NULL if no transport is available.
 */
AxlIpmiSession *
axl_ipmi_session_new(void);

/**
 * @brief Free an IPMI session. NULL-safe.
 */
void
axl_ipmi_session_free(
    AxlIpmiSession  *session  ///< session to free (NULL-safe)
    );

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlIpmiSession, axl_ipmi_session_free)
#endif

/**
 * @brief Return the transport kind this session is using.
 */
AxlIpmiTransport
axl_ipmi_session_transport(
    const AxlIpmiSession  *session  ///< session (NULL returns UNKNOWN)
    );

/**
 * @brief Return the IPMI completion code from the most recent typed
 *     command call on any session.
 *
 * Lets callers distinguish an IPMI-level failure (non-zero CC from
 * the BMC) from a transport-level failure (no response, timeout,
 * LocateProtocol fail) when a typed wrapper returns -1. On a
 * successful call the completion code is 0x00.
 *
 * UEFI is single-threaded so the tracked CC is process-global; the
 * session parameter is accepted for API-shape consistency but the
 * value does not vary per-session in this implementation.
 *
 * @return last-observed completion code, or 0x00 if no typed command
 *     has been invoked yet.
 */
uint8_t
axl_ipmi_session_last_cc(
    const AxlIpmiSession  *session  ///< session (accepted but ignored)
    );

// ---------------------------------------------------------------------------
// Raw command
// ---------------------------------------------------------------------------

/**
 * @brief Send a raw IPMI command and receive its response.
 *
 * Every typed wrapper is built on top of this. @a netfn is
 * unshifted (0x06 for App, 0x00 for Chassis, etc.). The response
 * buffer receives the completion code as its first byte.
 *
 * @a resp_len is in/out: on entry holds the buffer capacity; on
 * success receives the number of bytes actually written.
 *
 * @return AXL_OK on success (transport completed the transaction;
 *     check @a resp[0] for the IPMI completion code). -1 on
 *     transport error or invalid arguments.
 */
int
axl_ipmi_raw(
    AxlIpmiSession  *session,  ///< session returned by axl_ipmi_session_new
    uint8_t          netfn,    ///< network function (unshifted)
    uint8_t          cmd,      ///< command byte
    const uint8_t   *req,      ///< request data bytes (may be NULL if req_len == 0)
    size_t           req_len,  ///< request data length in bytes
    uint8_t         *resp,     ///< (out) response bytes; resp[0] is completion code
    size_t          *resp_len  ///< in/out: buffer size -> actual bytes written
    );

// ---------------------------------------------------------------------------
// Caller-provided transport (testing + future transports)
// ---------------------------------------------------------------------------

/**
 * Send-raw callback. Same shape as axl_ipmi_raw() minus the session
 * argument; the caller's @a user_data is passed through instead.
 */
typedef int (*AxlIpmiSendRaw)(
    void            *user_data,
    uint8_t          netfn,
    uint8_t          cmd,
    const uint8_t   *req,
    size_t           req_len,
    uint8_t         *resp,
    size_t          *resp_len
    );

/**
 * @brief Open an IPMI session backed by a caller-supplied transport
 *     callback.
 *
 * Useful for unit tests (inject canned responses) and for pluggable
 * out-of-process transports (IPMI-over-LAN, test rigs, etc.) that
 * aren't baked into the auto-detect chain.
 *
 * @return session handle, or NULL on allocation failure.
 */
AxlIpmiSession *
axl_ipmi_session_new_with_callback(
    AxlIpmiTransport  transport_kind,  ///< label reported by axl_ipmi_session_transport
    AxlIpmiSendRaw    send_raw,        ///< callback invoked for each command
    void             *user_data        ///< passed through to @a send_raw
    );

// ---------------------------------------------------------------------------
// Typed command wrappers
// ---------------------------------------------------------------------------

/**
 * Decoded Get Device ID response (App 0x01).
 */
typedef struct {
    uint8_t   device_id;
    uint8_t   device_revision;        ///< bits 0-3 revision, bit 7 = SDR support
    uint8_t   firmware_major;         ///< bits 0-6 major rev; bit 7 = device busy
    uint8_t   firmware_minor;         ///< raw BCD minor rev byte (e.g. 0x42 = .42)
    uint8_t   firmware_minor_decoded; ///< BCD-decoded minor rev (e.g. 42)
    uint8_t   ipmi_version;           ///< BCD: bits 3:0 major, bits 7:4 minor
    uint8_t   device_support;         ///< bitmask of functions supported
    uint32_t  manufacturer_id;        ///< 24-bit IANA manufacturer ID
    uint16_t  product_id;
    uint32_t  aux_firmware_rev;       ///< 0 if not provided
} AxlIpmiDeviceId;

int axl_ipmi_get_device_id(
    AxlIpmiSession   *session,
    AxlIpmiDeviceId  *out
    );

/**
 * Decoded Get Chassis Status response (Chassis 0x01).
 */
typedef struct {
    uint8_t  current_power_state;
    uint8_t  last_power_event;
    uint8_t  misc_state;
    uint8_t  front_panel_caps;        ///< 0 if not reported
} AxlIpmiChassisStatus;

int axl_ipmi_get_chassis_status(
    AxlIpmiSession        *session,
    AxlIpmiChassisStatus  *out
    );

/**
 * Chassis Control action (Chassis 0x02).
 */
typedef enum {
    AXL_IPMI_CHASSIS_POWER_DOWN        = 0x0,
    AXL_IPMI_CHASSIS_POWER_UP          = 0x1,
    AXL_IPMI_CHASSIS_POWER_CYCLE       = 0x2,
    AXL_IPMI_CHASSIS_HARD_RESET        = 0x3,
    AXL_IPMI_CHASSIS_PULSE_DIAG        = 0x4,
    AXL_IPMI_CHASSIS_SOFT_SHUTDOWN     = 0x5,
} AxlIpmiChassisAction;

int axl_ipmi_chassis_control(
    AxlIpmiSession         *session,
    AxlIpmiChassisAction    action
    );

/**
 * @brief Send IPMI Chassis Identify (Chassis 0x04).
 *
 * Flashes the front-panel chassis identify LED. The wire format
 * mirrors IPMI 2.0 §28.5:
 *   - With @p force_on false the request body is a single byte
 *     (the interval). @p interval_sec = 0 stops an in-progress
 *     identify; 1-255 sets the LED on for that many seconds.
 *   - With @p force_on true a second byte with bit 0 set is
 *     appended, telling the BMC to keep the LED on indefinitely.
 *     Some BMCs do not implement force-on and reply CC 0xC1.
 *
 * The spec-omitted-byte case (no interval byte at all) is not
 * exposed by this wrapper — callers that want the BMC default
 * pass interval_sec = 15 explicitly.
 *
 * @return AXL_OK on CC 0x00, AXL_ERR on transport error or non-zero CC.
 *     Last CC observable via axl_ipmi_session_last_cc().
 */
int axl_ipmi_chassis_identify(
    AxlIpmiSession  *session,
    uint8_t          interval_sec,
    bool             force_on
    );

/**
 * @brief Send a BMC Cold Reset (App 0x02).
 *
 * Full BMC reboot. The BMC is unresponsive to further IPMI for
 * 20–30 seconds afterward; callers should plan a retry delay.
 *
 * @return AXL_OK on success.
 */
int axl_ipmi_bmc_cold_reset(AxlIpmiSession *session);

/**
 * @brief Send a BMC Warm Reset (App 0x03).
 *
 * Resets BMC state without full reboot. Not all BMCs implement this
 * — expect CC 0xC1 "Invalid command" on some platforms.
 *
 * @return AXL_OK on success.
 */
int axl_ipmi_bmc_warm_reset(AxlIpmiSession *session);

/**
 * Decoded Get SEL Info response (Storage 0x40).
 */
typedef struct {
    uint8_t   version;
    uint16_t  entries;
    uint16_t  free_space_bytes;
    uint32_t  most_recent_addition;   ///< seconds since 1970-01-01 UTC
    uint32_t  most_recent_erase;
    uint8_t   op_support;             ///< reservation / delete / etc. bitmask
} AxlIpmiSelInfo;

int axl_ipmi_sel_info(
    AxlIpmiSession   *session,
    AxlIpmiSelInfo   *out
    );

/**
 * One SEL entry (Storage 0x43).
 *
 * The `record` field is filled with the raw 16 bytes per IPMI spec
 * Table 32-1; decoding the specific event type is up to the caller
 * (or the format helpers that land in a later phase).
 */
typedef struct {
    uint16_t  record_id;              ///< this entry's ID
    uint16_t  next_record_id;         ///< 0xFFFF marks last entry
    uint8_t   record[16];             ///< raw entry bytes
} AxlIpmiSelEntry;

int axl_ipmi_sel_get_entry(
    AxlIpmiSession    *session,
    uint16_t           record_id,     ///< 0x0000 = first, 0xFFFF = last
    AxlIpmiSelEntry   *out
    );

/**
 * Decoded Get SDR Repository Info response (Storage 0x20).
 */
typedef struct {
    uint8_t   version;
    uint16_t  record_count;
    uint16_t  free_space_bytes;
    uint32_t  most_recent_addition;
    uint32_t  most_recent_erase;
    uint8_t   op_support;
} AxlIpmiSdrInfo;

int axl_ipmi_sdr_info(
    AxlIpmiSession   *session,
    AxlIpmiSdrInfo   *out
    );

/**
 * @brief Fetch one SDR record's bytes.
 *
 * @a len is in/out: buffer capacity in, actual record size out. The
 * typed SDR layouts (full sensor / compact sensor / entity / etc.)
 * are defined in the IPMI spec; callers decode the returned bytes
 * based on the SDR header's record type.
 *
 * @return AXL_OK on success; @a *next_record_id receives the record id
 *     to pass on the next call (0xFFFF after the last record).
 */
int axl_ipmi_sdr_get(
    AxlIpmiSession    *session,
    uint16_t           record_id,
    uint16_t          *next_record_id,
    uint8_t           *buf,
    size_t            *len
    );

/**
 * Decoded Get Sensor Reading response (Sensor 0x2D).
 *
 * Raw reading conversion into engineering units requires the sensor's
 * SDR record (for m, b, exponent); callers that want scaled values
 * look up the SDR once and compute `(reading * m + b) * 10^exp` per
 * IPMI spec 36.3.
 */
typedef struct {
    uint8_t  reading;                 ///< raw linear value
    uint8_t  event_status;            ///< bit 7: scanning disabled
    uint8_t  reading_fields;          ///< discrete reading bits (optional)
    uint8_t  threshold_fields;        ///< threshold status (optional)
} AxlIpmiSensorReading;

int axl_ipmi_get_sensor_reading(
    AxlIpmiSession        *session,
    uint8_t                sensor_num,
    AxlIpmiSensorReading  *out
    );

/**
 * Decoded FRU Inventory Area Info response (Storage 0x10).
 */
typedef struct {
    uint16_t  size_bytes;
    bool      word_access;            ///< true: offsets are word-addressed
} AxlIpmiFruInfo;

int axl_ipmi_fru_info(
    AxlIpmiSession   *session,
    uint8_t           fru_id,
    AxlIpmiFruInfo   *out
    );

/**
 * @brief Read a chunk of FRU data.
 *
 * FRU areas are typically hundreds to a few thousand bytes; callers
 * loop this helper with a 16-byte chunk size (SSIF sweet spot).
 *
 * @a len is in/out: buffer capacity on entry, bytes actually read
 * on return.
 *
 * @return AXL_OK on success.
 */
int axl_ipmi_fru_read(
    AxlIpmiSession  *session,
    uint8_t          fru_id,
    uint16_t         offset,
    uint8_t         *buf,
    size_t          *len
    );

// ---------------------------------------------------------------------------
// Formatting helpers (string table lookups; no allocation)
// ---------------------------------------------------------------------------

/**
 * @brief Human-readable name for an IPMI completion code.
 *
 * Covers the standard codes from IPMI v2.0 Table 5-2. Unknown codes
 * fall through to a generic "device-specific / OEM" label.
 *
 * @return static string; never NULL.
 */
const char *
axl_ipmi_completion_code_string(
    uint8_t  cc   ///< completion code byte (resp[0])
    );

/**
 * @brief Human-readable name for an IPMI sensor type.
 *
 * Covers type codes 0x01-0x2C from IPMI v2.0 Table 42-3. Unknown
 * codes return "Unknown".
 *
 * @return static string; never NULL.
 */
const char *
axl_ipmi_sensor_type_string(
    uint8_t  sensor_type
    );

// ---------------------------------------------------------------------------
// Probe — diagnostic snapshot for troubleshooting
// ---------------------------------------------------------------------------

/**
 * Result of `axl_ipmi_probe()`.
 *
 * Describes which IPMI-adjacent firmware protocols are present, what
 * SMBIOS Type 38 says about the BMC interface, and how many I2C
 * Master handles are available. Intended for diagnostics when
 * `axl_ipmi_session_new()` can't find a working transport — lets a
 * tool print a summary of "what the firmware exposes" without
 * needing direct access to UEFI protocol lookup.
 */
typedef struct {
    // Known IPMI transport protocol presence (LocateProtocol).
    bool  edkii_ipmi_protocol;        ///< gIpmiProtocolGuid
    bool  dell_ipmi_transport;        ///< gDellIpmiProtocolGuid
    bool  ami_dxe_ipmi_transport;     ///< AMI IpmiPkg DXE variant
    bool  ami_smm_ipmi_transport;     ///< AMI IpmiPkg SMM variant
    bool  intel_sm_ipmi_transport;    ///< Intel ServerManagementPkg
    bool  mu_ipmi_transport2;         ///< Microsoft Project Mu

    // Supporting infrastructure
    bool  smbus_hc_protocol;          ///< EFI_SMBUS_HC_PROTOCOL
    bool  i2c_master_protocol;        ///< EFI_I2C_MASTER_PROTOCOL (any)

    // SMBIOS Type 38 (IPMI Device Information)
    bool     smbios_type38_present;
    uint8_t  smbios_interface_type;   ///< 1=KCS 2=SMIC 3=BT 4=SSIF
    uint8_t  smbios_i2c_slave;        ///< 8-bit wire address from SMBIOS
    uint64_t smbios_base_address;     ///< KCS: low bit = I/O vs memory

    // Handle counts
    size_t   i2c_master_handle_count;
} AxlIpmiProbe;

/**
 * @brief Populate @a out with a snapshot of IPMI-related firmware
 *     state. Always succeeds (fields are zero-filled when the
 *     corresponding feature isn't present).
 *
 * @return AXL_OK on success, AXL_ERR only on NULL argument.
 */
int
axl_ipmi_probe(
    AxlIpmiProbe  *out
    );

/**
 * @brief Human-readable name for an IPMI entity ID.
 *
 * Covers the common entity IDs from IPMI v2.0 Table 43-13.
 *
 * @return static string; never NULL.
 */
const char *
axl_ipmi_entity_id_string(
    uint8_t  entity_id
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_IPMI_H */
