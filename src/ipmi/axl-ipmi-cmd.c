/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-cmd.c
    Typed IPMI command wrappers.

    Each wrapper builds the request bytes, fires axl_ipmi_raw(),
    verifies the completion code, and decodes the response into the
    public AxlIpmi* struct. NetFn values are taken from IPMI v2.0
    Table 5-1.
**/

//
// cmd wrappers only need the public AxlIpmi types and the allocator
// + logger — not the transport vtable. Keeping this TU free of
// axl-ipmi-internal.h (and therefore of backend includes) also means
// the fuzz harness can compile it standalone against a libc shim.
//
#include <axl/axl-ipmi.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("ipmi-cmd");

// ---------------------------------------------------------------------------
// NetFn + Cmd constants (IPMI v2.0 Chapters 20-35)
// ---------------------------------------------------------------------------

#define IPMI_NETFN_APP      0x06
#define IPMI_NETFN_CHASSIS  0x00
#define IPMI_NETFN_SENSOR   0x04
#define IPMI_NETFN_STORAGE  0x0A

// App
#define CMD_GET_DEVICE_ID         0x01

// App (continued)
#define CMD_BMC_COLD_RESET        0x02
#define CMD_BMC_WARM_RESET        0x03

// Chassis
#define CMD_GET_CHASSIS_STATUS    0x01
#define CMD_CHASSIS_CONTROL       0x02

// Sensor
#define CMD_GET_SENSOR_READING    0x2D

// Storage
#define CMD_GET_SEL_INFO              0x40
#define CMD_GET_SEL_ENTRY             0x43
#define CMD_GET_SDR_INFO              0x20
#define CMD_RESERVE_SDR_REPOSITORY    0x22
#define CMD_GET_SDR                   0x23
#define CMD_GET_FRU_INFO              0x10
#define CMD_READ_FRU_DATA             0x11

// Completion code: reservation cancelled / invalid.
#define IPMI_CC_RESERVATION_INVALID   0xC5

#define IPMI_CC_OK  0x00

//
// Tracks the most recent completion code across all typed wrappers.
// UEFI is single-threaded so a file-static is sufficient; if we ever
// grow a multi-thread model this becomes per-session state.
//
static uint8_t  g_last_cc = 0x00;

uint8_t
axl_ipmi_session_last_cc(const AxlIpmiSession *session)
{
    (void)session;
    return g_last_cc;
}

// ---------------------------------------------------------------------------
// Little-endian byte extractors (IPMI responses are LE on the wire)
// ---------------------------------------------------------------------------

static inline uint16_t
rd16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t
rd24_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline uint32_t
rd32_le(const uint8_t *p)
{
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Shared path: send a command and verify the completion code.
 * Returns the *payload* length (response bytes after the CC) via
 * @a out_payload_len, or -1 on transport/CC error.
 */
static int
send_and_check(AxlIpmiSession *session,
               uint8_t netfn, uint8_t cmd,
               const uint8_t *req, size_t req_len,
               uint8_t *resp, size_t resp_cap,
               size_t *out_payload_len)
{
    if (session == NULL || resp == NULL || out_payload_len == NULL) {
        return -1;
    }
    size_t resp_len = resp_cap;
    int rc = axl_ipmi_raw(session, netfn, cmd,
                          req, req_len, resp, &resp_len);
    if (rc != 0) {
        return -1;
    }
    if (resp_len < 1) {
        return -1;
    }
    g_last_cc = resp[0];
    if (resp[0] != IPMI_CC_OK) {
        axl_debug("IPMI cmd %02x/%02x CC=0x%02x", netfn, cmd, resp[0]);
        return -1;
    }
    *out_payload_len = resp_len - 1;
    return 0;
}

// ---------------------------------------------------------------------------
// App: Get Device ID (0x06 / 0x01)
// ---------------------------------------------------------------------------

int
axl_ipmi_get_device_id(AxlIpmiSession *session, AxlIpmiDeviceId *out)
{
    if (out == NULL) {
        return -1;
    }
    uint8_t  resp[16];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_APP, CMD_GET_DEVICE_ID,
                       NULL, 0, resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    //
    // Response body (after CC) — spec 20.1 Table 20-2:
    //   [0] DeviceId
    //   [1] DeviceRevision
    //   [2] FirmwareMajor
    //   [3] FirmwareMinor (BCD)
    //   [4] IpmiVersion (BCD)
    //   [5] DeviceSupport
    //   [6..8] ManufacturerId (24-bit LE)
    //   [9..10] ProductId (16-bit LE)
    //   [11..14] AuxFirmwareRev (optional, may be absent)
    //
    if (payload < 11) {
        return -1;
    }
    const uint8_t *p = &resp[1];
    out->device_id        = p[0];
    out->device_revision  = p[1];
    out->firmware_major   = p[2];
    out->firmware_minor   = p[3];
    out->ipmi_version     = p[4];
    out->device_support   = p[5];
    out->manufacturer_id  = rd24_le(&p[6]);
    out->product_id       = rd16_le(&p[9]);
    if (payload >= 15) {
        out->aux_firmware_rev = rd32_le(&p[11]);
    } else {
        out->aux_firmware_rev = 0;
        if (payload > 11) {
            axl_debug("Get Device ID: partial aux firmware rev (%zu bytes), "
                      "returning 0", payload - 11);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Chassis: Get Status (0x00 / 0x01), Control (0x00 / 0x02)
// ---------------------------------------------------------------------------

int
axl_ipmi_get_chassis_status(AxlIpmiSession *session,
                            AxlIpmiChassisStatus *out)
{
    if (out == NULL) {
        return -1;
    }
    uint8_t  resp[5];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_CHASSIS, CMD_GET_CHASSIS_STATUS,
                       NULL, 0, resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    if (payload < 3) {
        return -1;
    }
    const uint8_t *p = &resp[1];
    out->current_power_state = p[0];
    out->last_power_event    = p[1];
    out->misc_state          = p[2];
    out->front_panel_caps    = (payload >= 4) ? p[3] : 0;
    return 0;
}

int
axl_ipmi_bmc_cold_reset(AxlIpmiSession *session)
{
    uint8_t resp[1];
    size_t  payload;
    return send_and_check(session, IPMI_NETFN_APP, CMD_BMC_COLD_RESET,
                          NULL, 0, resp, sizeof(resp), &payload);
}

int
axl_ipmi_bmc_warm_reset(AxlIpmiSession *session)
{
    uint8_t resp[1];
    size_t  payload;
    return send_and_check(session, IPMI_NETFN_APP, CMD_BMC_WARM_RESET,
                          NULL, 0, resp, sizeof(resp), &payload);
}

int
axl_ipmi_chassis_control(AxlIpmiSession *session,
                         AxlIpmiChassisAction action)
{
    uint8_t  req = (uint8_t)action;
    uint8_t  resp[1];
    size_t   payload;
    return send_and_check(session, IPMI_NETFN_CHASSIS, CMD_CHASSIS_CONTROL,
                          &req, 1, resp, sizeof(resp), &payload);
}

// ---------------------------------------------------------------------------
// Storage: Get SEL Info (0x0A / 0x40), Get SEL Entry (0x0A / 0x43)
// ---------------------------------------------------------------------------

int
axl_ipmi_sel_info(AxlIpmiSession *session, AxlIpmiSelInfo *out)
{
    if (out == NULL) {
        return -1;
    }
    uint8_t  resp[15];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_STORAGE, CMD_GET_SEL_INFO,
                       NULL, 0, resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    //
    // Spec 31.2 Table 31-1 response body (after CC):
    //   [0] SEL version
    //   [1..2] Entries LE
    //   [3..4] FreeSpace bytes LE
    //   [5..8] Most recent addition timestamp
    //   [9..12] Most recent erase timestamp
    //   [13] Operation support bitmask
    //
    if (payload < 14) {
        return -1;
    }
    const uint8_t *p = &resp[1];
    out->version              = p[0];
    out->entries              = rd16_le(&p[1]);
    out->free_space_bytes     = rd16_le(&p[3]);
    out->most_recent_addition = rd32_le(&p[5]);
    out->most_recent_erase    = rd32_le(&p[9]);
    out->op_support           = p[13];
    return 0;
}

int
axl_ipmi_sel_get_entry(AxlIpmiSession *session,
                       uint16_t record_id,
                       AxlIpmiSelEntry *out)
{
    if (out == NULL) {
        return -1;
    }
    //
    // Get SEL Entry request (Spec 31.5 Table 31-5):
    //   [0..1] ReservationId LE (0 if unused)
    //   [2..3] RecordId LE
    //   [4]    Offset into record (0 for whole)
    //   [5]    Bytes to read (0xFF = entire record)
    //
    uint8_t req[6] = {
        0x00, 0x00,
        (uint8_t)(record_id & 0xFF), (uint8_t)((record_id >> 8) & 0xFF),
        0x00, 0xFF,
    };

    uint8_t  resp[1 + 2 + 16];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_STORAGE, CMD_GET_SEL_ENTRY,
                       req, sizeof(req), resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    if (payload < 2 + 16) {
        return -1;
    }
    const uint8_t *p = &resp[1];
    out->next_record_id = rd16_le(&p[0]);
    out->record_id      = rd16_le(&p[2]);     // first two bytes of entry
    for (size_t i = 0; i < 16; i++) {
        out->record[i] = p[2 + i];
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Storage: Get SDR Info (0x0A / 0x20), Get SDR (0x0A / 0x23)
// ---------------------------------------------------------------------------

int
axl_ipmi_sdr_info(AxlIpmiSession *session, AxlIpmiSdrInfo *out)
{
    if (out == NULL) {
        return -1;
    }
    uint8_t  resp[15];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_STORAGE, CMD_GET_SDR_INFO,
                       NULL, 0, resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    if (payload < 14) {
        return -1;
    }
    const uint8_t *p = &resp[1];
    out->version              = p[0];
    out->record_count         = rd16_le(&p[1]);
    out->free_space_bytes     = rd16_le(&p[3]);
    out->most_recent_addition = rd32_le(&p[5]);
    out->most_recent_erase    = rd32_le(&p[9]);
    out->op_support           = p[13];
    return 0;
}

/**
 * Internal: one Get SDR call with explicit offset + byte count.
 *
 * Returns 0 on success and fills @a *next_record_id (from CC+1, CC+2)
 * plus @a *chunk_len with the number of record bytes written into
 * @a out (starting at out[0] — the NextRecordId header is stripped).
 */
static int
sdr_get_partial(AxlIpmiSession *session,
                uint16_t reservation_id,
                uint16_t record_id,
                uint8_t  offset,
                uint8_t  bytes_to_read,
                uint16_t *next_record_id,
                uint8_t  *out,
                size_t   *chunk_len)
{
    uint8_t req[6] = {
        (uint8_t)(reservation_id & 0xFF),
        (uint8_t)((reservation_id >> 8) & 0xFF),
        (uint8_t)(record_id & 0xFF),
        (uint8_t)((record_id >> 8) & 0xFF),
        offset,
        bytes_to_read,
    };

    //
    // Response is CC + NextRecordId(2) + up to @a bytes_to_read
    // payload bytes. Stage on the stack — 32 bytes is enough since
    // we never ask the BMC for more than SDR_READ_CHUNK at a time.
    //
    uint8_t  resp[3 + 32];
    size_t   payload;
    int rc = send_and_check(session, IPMI_NETFN_STORAGE, CMD_GET_SDR,
                            req, sizeof(req),
                            resp, sizeof(resp), &payload);
    if (rc != 0) {
        return -1;
    }
    if (payload < 2) {
        return -1;
    }
    *next_record_id = rd16_le(&resp[1]);

    size_t body = payload - 2;
    if (body > *chunk_len) {
        body = *chunk_len;
    }
    for (size_t i = 0; i < body; i++) {
        out[i] = resp[3 + i];
    }
    *chunk_len = body;
    return 0;
}

//
// Internal: Reserve SDR Repository (Storage 0x22).
// Returns a reservation ID for use with partial Get SDR calls on
// BMCs that require it.
//
static int
sdr_reserve(AxlIpmiSession *session, uint16_t *out_rsv_id)
{
    if (out_rsv_id == NULL) {
        return -1;
    }
    uint8_t  resp[3];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_STORAGE,
                       CMD_RESERVE_SDR_REPOSITORY,
                       NULL, 0, resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    if (payload < 2) {
        return -1;
    }
    *out_rsv_id = rd16_le(&resp[1]);
    return 0;
}

int
axl_ipmi_sdr_get(AxlIpmiSession *session,
                 uint16_t record_id,
                 uint16_t *next_record_id,
                 uint8_t *buf,
                 size_t *len)
{
    if (next_record_id == NULL || buf == NULL || len == NULL) {
        return -1;
    }
    size_t cap = *len;
    if (cap < 5) {
        return -1;
    }

    //
    // Chunked read strategy: matches Linux ipmitool's
    // ipmi_sdr_get_record(). Full-record reads (BytesToRead=0xFF)
    // produce 60+ byte IPMI responses that force multi-part SSIF
    // reads, which hang on the Nvidia UEFI I2C driver + Dell iDRAC
    // combination (uefi-ipmitool commit 8c6acdb documented this).
    // Header-first + 23-byte body chunks keeps every response in a
    // single SSIF block.
    //
    // We optimistically try reservation_id=0 first — most BMCs don't
    // require a reservation for partial reads. If the first chunk
    // comes back with CC=0xC5 (reservation cancelled / invalid), we
    // call Reserve SDR Repository to obtain a valid ID and retry.
    // The same reservation is used for every subsequent body chunk
    // of this record.
    //
    #define SDR_HEADER_BYTES  5
    #define SDR_READ_CHUNK    23

    uint16_t rsv_id = 0;
    size_t   header_chunk = SDR_HEADER_BYTES;
    int      rc = sdr_get_partial(session, rsv_id, record_id, 0,
                                  SDR_HEADER_BYTES, next_record_id,
                                  buf, &header_chunk);
    if (rc != 0 && g_last_cc == IPMI_CC_RESERVATION_INVALID) {
        //
        // BMC requires a reservation. Grab one and retry.
        //
        if (sdr_reserve(session, &rsv_id) != 0) {
            return -1;
        }
        header_chunk = SDR_HEADER_BYTES;
        rc = sdr_get_partial(session, rsv_id, record_id, 0,
                             SDR_HEADER_BYTES, next_record_id,
                             buf, &header_chunk);
    }
    if (rc != 0) {
        return -1;
    }
    if (header_chunk < SDR_HEADER_BYTES) {
        return -1;
    }

    //
    // Header byte 4 is the record body length (excludes the 5 header
    // bytes). Zero-body records happen on some BMCs; treat them as
    // header-only and return.
    //
    uint8_t body_len = buf[4];
    size_t  want     = SDR_HEADER_BYTES + body_len;
    if (want > cap) {
        want = cap;
    }

    size_t offset = SDR_HEADER_BYTES;
    while (offset < want) {
        size_t  remaining = want - offset;
        uint8_t ask = (uint8_t)(remaining > SDR_READ_CHUNK
                                ? SDR_READ_CHUNK : remaining);

        size_t   got = ask;
        uint16_t ign;  // NextRecordId is identical on every partial read
        if (sdr_get_partial(session, rsv_id, record_id,
                            (uint8_t)offset, ask,
                            &ign, &buf[offset], &got) != 0)
        {
            return -1;
        }
        if (got == 0) {
            //
            // BMC returned zero — avoid a tight loop. Warn so the
            // short-read case is visible in logs; callers that care
            // about completeness can also compare *len against the
            // header's body_len byte (buf[4]).
            //
            axl_warning("Get SDR id=0x%04x: short body at offset %zu "
                        "(asked %u, got 0)",
                        (unsigned)record_id, offset, (unsigned)ask);
            break;
        }
        offset += got;
    }

    *len = offset;
    return 0;
    #undef SDR_HEADER_BYTES
    #undef SDR_READ_CHUNK
}

// ---------------------------------------------------------------------------
// Sensor: Get Sensor Reading (0x04 / 0x2D)
// ---------------------------------------------------------------------------

int
axl_ipmi_get_sensor_reading(AxlIpmiSession *session,
                            uint8_t sensor_num,
                            AxlIpmiSensorReading *out)
{
    if (out == NULL) {
        return -1;
    }
    uint8_t  resp[5];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_SENSOR, CMD_GET_SENSOR_READING,
                       &sensor_num, 1, resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    if (payload < 2) {
        return -1;
    }
    const uint8_t *p = &resp[1];
    out->reading           = p[0];
    out->event_status      = p[1];
    out->reading_fields    = (payload >= 3) ? p[2] : 0;
    out->threshold_fields  = (payload >= 4) ? p[3] : 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Storage: FRU Inventory Area Info (0x0A / 0x10), Read FRU Data (0x0A / 0x11)
// ---------------------------------------------------------------------------

int
axl_ipmi_fru_info(AxlIpmiSession *session,
                  uint8_t fru_id,
                  AxlIpmiFruInfo *out)
{
    if (out == NULL) {
        return -1;
    }
    uint8_t  req = fru_id;
    uint8_t  resp[4];
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_STORAGE, CMD_GET_FRU_INFO,
                       &req, 1, resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    //
    // Spec 34.1 Table 34-1 response body (after CC):
    //   [0..1] FRU size LE
    //   [2]    Access type: bit 0 set = word-addressable
    //
    if (payload < 3) {
        return -1;
    }
    const uint8_t *p = &resp[1];
    out->size_bytes  = rd16_le(&p[0]);
    out->word_access = (p[2] & 0x01) != 0;
    return 0;
}

int
axl_ipmi_fru_read(AxlIpmiSession *session,
                  uint8_t fru_id,
                  uint16_t offset,
                  uint8_t *buf,
                  size_t *len)
{
    if (buf == NULL || len == NULL) {
        return -1;
    }
    if (*len == 0) {
        return -1;
    }
    //
    // IPMI "Read FRU Data" request tops out at 255 bytes per call
    // (the count field is UINT8). Clamp the caller's request to the
    // wire limit; callers that want more loop on offset.
    //
    size_t cap = (*len > 0xFF) ? 0xFF : *len;

    uint8_t req[4] = {
        fru_id,
        (uint8_t)(offset & 0xFF),
        (uint8_t)((offset >> 8) & 0xFF),
        (uint8_t)cap,
    };

    uint8_t  resp[1 + 1 + 0xFF];   // CC + count + up to 255 bytes
    size_t   payload;
    if (send_and_check(session, IPMI_NETFN_STORAGE, CMD_READ_FRU_DATA,
                       req, sizeof(req), resp, sizeof(resp), &payload) != 0)
    {
        return -1;
    }
    //
    // Spec 34.2: response body [0] = count read, [1..] = data bytes.
    //
    if (payload < 1) {
        return -1;
    }
    size_t got = resp[1];
    if (got > payload - 1) {
        got = payload - 1;
    }
    if (got > *len) {
        got = *len;
    }
    for (size_t i = 0; i < got; i++) {
        buf[i] = resp[2 + i];
    }
    *len = got;
    return 0;
}
