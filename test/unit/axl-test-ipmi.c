/** @file axl-test-ipmi.c
    Unit tests for AxlIpmi.

    Tests the typed-command wrappers and the session callback by
    wiring a canned responder into `axl_ipmi_session_new_with_callback`.
    No real BMC is required — every call hits the test callback,
    which echoes a pre-staged response tuned to the specific command.

    Hardware-level behavior (actual KCS state machine, SSIF multi-
    part framing, vendor-protocol dispatch) is covered by the manual
    integration script in test/integration/test-ipmi.sh.
**/

#include "axl-test.h"
#include <axl/axl-ipmi.h>

// ---------------------------------------------------------------------------
// Canned-response harness
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  netfn;             ///< expected NetFn (0 = don't check)
    uint8_t  cmd;               ///< expected Cmd
    uint8_t  resp[64];
    size_t   resp_len;
    size_t   call_count;        ///< updated by the shim
    uint8_t  last_req[32];      ///< captured for assertion
    size_t   last_req_len;
    uint8_t  last_netfn;
    uint8_t  last_cmd;
} Canned;

static int
canned_send_raw(void *user_data,
                uint8_t netfn, uint8_t cmd,
                const uint8_t *req, size_t req_len,
                uint8_t *resp, size_t *resp_len)
{
    Canned *c = (Canned *)user_data;
    c->call_count++;
    c->last_netfn = netfn;
    c->last_cmd   = cmd;

    c->last_req_len = req_len > sizeof(c->last_req)
                      ? sizeof(c->last_req) : req_len;
    for (size_t i = 0; i < c->last_req_len; i++) {
        c->last_req[i] = req[i];
    }

    size_t n = c->resp_len;
    if (n > *resp_len) {
        n = *resp_len;
    }
    for (size_t i = 0; i < n; i++) {
        resp[i] = c->resp[i];
    }
    *resp_len = n;
    return 0;
}

// ---------------------------------------------------------------------------
// Session + raw API basics
// ---------------------------------------------------------------------------

static void
test_session_callback(void)
{
    Canned c = { .resp = {0x00}, .resp_len = 1 };

    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);
    test_check(s != NULL, "session_new_with_callback: returns session");
    test_check(axl_ipmi_session_transport(s) == AXL_IPMI_TRANSPORT_KCS,
               "session_transport: reports caller's label");

    uint8_t resp[4];
    size_t  resp_len = sizeof(resp);
    int rc = axl_ipmi_raw(s, 0x06, 0x01, NULL, 0, resp, &resp_len);
    test_check(rc == 0, "axl_ipmi_raw: forwards to callback");
    test_check(c.call_count == 1, "axl_ipmi_raw: exactly one dispatch");
    test_check(c.last_netfn == 0x06 && c.last_cmd == 0x01,
               "axl_ipmi_raw: netfn + cmd reach the callback");
    test_check(resp_len == 1 && resp[0] == 0x00,
               "axl_ipmi_raw: canned response reaches caller");
}

static void
test_session_callback_rejects_null(void)
{
    AxlIpmiSession *s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_UNKNOWN, NULL, NULL);
    test_check(s == NULL,
               "session_new_with_callback: NULL callback returns NULL");
}

// ---------------------------------------------------------------------------
// Typed wrappers — happy path
// ---------------------------------------------------------------------------

static void
test_get_device_id(void)
{
    Canned c = {
        .netfn = 0x06, .cmd = 0x01,
        .resp = {
            0x00,               // CC
            0x20,               // DeviceId
            0x01,               // DeviceRevision
            0x02, 0x34,         // firmware major.minor
            0x02,               // IPMI version (2.0)
            0xBF,               // DeviceSupport
            0x01, 0x00, 0x00,   // Manufacturer 0x000001 (IBM test value)
            0xAB, 0xCD,         // ProductId
            0x00, 0x00, 0x00, 0x00,  // Aux firmware rev
        },
        .resp_len = 16,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_EDKII, canned_send_raw, &c);

    AxlIpmiDeviceId d;
    int rc = axl_ipmi_get_device_id(s, &d);
    test_check(rc == 0, "get_device_id: ok");
    test_check(d.device_id == 0x20, "get_device_id: device id");
    test_check(d.firmware_major == 0x02 && d.firmware_minor == 0x34,
               "get_device_id: firmware revision (raw)");
    test_check(d.firmware_minor_decoded == 34,
               "get_device_id: firmware minor BCD decode (0x34 → 34)");
    test_check(d.manufacturer_id == 0x000001,
               "get_device_id: manufacturer id little-endian");
    test_check(d.product_id == 0xCDAB,
               "get_device_id: product id little-endian");
    test_check(d.aux_firmware_rev == 0,
               "get_device_id: aux firmware rev zero");
}

static void
test_get_chassis_status(void)
{
    Canned c = {
        .netfn = 0x00, .cmd = 0x01,
        .resp = {
            0x00,       // CC
            0x21,       // current_power_state: on + last-on=command
            0x00,       // last_power_event
            0x40,       // misc_state: front panel lockout
        },
        .resp_len = 4,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiChassisStatus st;
    test_check(axl_ipmi_get_chassis_status(s, &st) == 0,
               "get_chassis_status: ok");
    test_check(st.current_power_state == 0x21,
               "get_chassis_status: current power state");
    test_check(st.misc_state == 0x40,
               "get_chassis_status: misc state");
}

static void
test_chassis_control(void)
{
    Canned c = { .resp = {0x00}, .resp_len = 1 };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    test_check(axl_ipmi_chassis_control(s, AXL_IPMI_CHASSIS_POWER_UP) == 0,
               "chassis_control: power-up dispatch");
    test_check(c.last_netfn == 0x00 && c.last_cmd == 0x02,
               "chassis_control: uses correct netfn/cmd");
    test_check(c.last_req_len == 1 && c.last_req[0] == 0x01,
               "chassis_control: carries action byte");
}

static void
test_sel_info(void)
{
    Canned c = {
        .resp = {
            0x00,                    // CC
            0x51,                    // SEL version (2.0 compliant)
            0x0A, 0x00,              // Entries = 10
            0x00, 0x04,              // Free space = 1024 bytes
            0x78, 0x56, 0x34, 0x12,  // Most recent addition timestamp
            0x00, 0x00, 0x00, 0x00,  // Most recent erase
            0x0B,                    // op_support
        },
        .resp_len = 15,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiSelInfo info;
    test_check(axl_ipmi_sel_info(s, &info) == 0, "sel_info: ok");
    test_check(info.version == 0x51, "sel_info: version");
    test_check(info.entries == 10, "sel_info: entry count");
    test_check(info.free_space_bytes == 0x0400,
               "sel_info: free space little-endian");
    test_check(info.most_recent_addition == 0x12345678,
               "sel_info: timestamp little-endian");
}

static void
test_sel_get_entry(void)
{
    Canned c = {
        .resp = {
            0x00,                // CC
            0x02, 0x00,          // NextRecordId = 2
            0x01, 0x00,          // RecordId = 1 (first 2 bytes of entry)
            0x02,                // RecordType
            0x78, 0x56, 0x34, 0x12,  // Timestamp
            0x20, 0x00,          // GeneratorId
            0x04,                // EvMRev
            0x02,                // SensorType
            0x30,                // SensorNumber
            0x01, 0xFF, 0xFF,    // EventType/Direction, ED1, ED2
            0xFF,                // ED3
        },
        .resp_len = 19,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiSelEntry e;
    test_check(axl_ipmi_sel_get_entry(s, 1, &e) == 0,
               "sel_get_entry: ok");
    test_check(e.next_record_id == 2,
               "sel_get_entry: next record id");
    test_check(e.record_id == 1,
               "sel_get_entry: record id");
    test_check(e.record[2] == 0x02,
               "sel_get_entry: RecordType decoded correctly");
}

static void
test_sdr_info(void)
{
    Canned c = {
        .resp = {
            0x00,
            0x51,                   // version
            0x40, 0x00,             // 64 records
            0x00, 0x10,             // 4 KiB free
            0x00, 0x00, 0x00, 0x00, // addition
            0x00, 0x00, 0x00, 0x00, // erase
            0x0F,
        },
        .resp_len = 15,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiSdrInfo info;
    test_check(axl_ipmi_sdr_info(s, &info) == 0, "sdr_info: ok");
    test_check(info.record_count == 64, "sdr_info: record count");
    test_check(info.free_space_bytes == 0x1000, "sdr_info: free space");
}

static void
test_sensor_reading(void)
{
    Canned c = {
        .resp = {
            0x00,       // CC
            0x7D,       // Sensor reading: 125 (raw)
            0xC0,       // Scanning enabled, event messages enabled
            0x00,       // reading fields
        },
        .resp_len = 4,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiSensorReading r;
    test_check(axl_ipmi_get_sensor_reading(s, 0x10, &r) == 0,
               "sensor_reading: ok");
    test_check(r.reading == 0x7D, "sensor_reading: raw value");
    test_check(r.event_status == 0xC0, "sensor_reading: event status");
    test_check(c.last_req_len == 1 && c.last_req[0] == 0x10,
               "sensor_reading: sensor number in request");
}

static void
test_fru_info(void)
{
    Canned c = {
        .resp = {
            0x00,          // CC
            0x00, 0x01,    // 256 bytes
            0x00,          // byte-addressed
        },
        .resp_len = 4,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiFruInfo fi;
    test_check(axl_ipmi_fru_info(s, 0x00, &fi) == 0, "fru_info: ok");
    test_check(fi.size_bytes == 256, "fru_info: size");
    test_check(fi.word_access == false, "fru_info: byte access");
}

static void
test_fru_read(void)
{
    Canned c = {
        .resp = {
            0x00,                // CC
            0x08,                // bytes read = 8
            0x01, 0x00, 0x00, 0x01, 0x07, 0x00, 0x00, 0xC2,
        },
        .resp_len = 10,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    uint8_t buf[16];
    size_t  got = sizeof(buf);
    int rc = axl_ipmi_fru_read(s, 0x00, 0, buf, &got);
    test_check(rc == 0, "fru_read: ok");
    test_check(got == 8, "fru_read: bytes read");
    test_check(buf[0] == 0x01, "fru_read: first byte");
}

// ---------------------------------------------------------------------------
// Negative paths
// ---------------------------------------------------------------------------

static void
test_bmc_cold_reset(void)
{
    Canned c = { .resp = {0x00}, .resp_len = 1 };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    test_check(axl_ipmi_bmc_cold_reset(s) == 0, "bmc_cold_reset: ok");
    test_check(c.last_netfn == 0x06 && c.last_cmd == 0x02,
               "bmc_cold_reset: App 0x02");
}

static void
test_bmc_warm_reset(void)
{
    Canned c = { .resp = {0x00}, .resp_len = 1 };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    test_check(axl_ipmi_bmc_warm_reset(s) == 0, "bmc_warm_reset: ok");
    test_check(c.last_netfn == 0x06 && c.last_cmd == 0x03,
               "bmc_warm_reset: App 0x03");
}

//
// sdr_get chunked reads — the canned harness returns the same payload
// to every call. We seed a 30-byte record (5 header + 25 body) and
// confirm:
//   1. The first request asks for 5 bytes (SDR header).
//   2. Subsequent calls at offset 5, 28 fetch the body.
//   3. The reassembled buffer carries all 30 bytes.
//
typedef struct {
    size_t   call_count;
    uint8_t  last_offset;
    uint8_t  last_bytes;
    uint8_t  header[5];
    uint8_t  body[25];
} SdrHarness;

static int
sdr_harness_send_raw(void *user_data,
                     uint8_t netfn, uint8_t cmd,
                     const uint8_t *req, size_t req_len,
                     uint8_t *resp, size_t *resp_len)
{
    SdrHarness *h = (SdrHarness *)user_data;
    h->call_count++;
    if (netfn != 0x0A || cmd != 0x23 || req_len < 6) {
        *resp_len = 0;
        return 0;
    }
    h->last_offset = req[4];
    h->last_bytes  = req[5];

    //
    // Reply: [CC=0, NextRecordId_lo, NextRecordId_hi, data...]
    // Next record id = 0xFFFF (last entry).
    //
    const uint8_t *src;
    size_t src_len;
    if (h->last_offset == 0) {
        src = h->header;
        src_len = 5;
    } else {
        size_t off_into_body = h->last_offset - 5;
        if (off_into_body >= sizeof(h->body)) {
            *resp_len = 3;  // CC + NextId with no body
            if (*resp_len > 0) { resp[0] = 0x00; resp[1] = 0xFF; resp[2] = 0xFF; }
            return 0;
        }
        src = h->body + off_into_body;
        src_len = sizeof(h->body) - off_into_body;
    }

    size_t give = h->last_bytes < src_len ? h->last_bytes : src_len;
    if (give + 3 > *resp_len) {
        give = *resp_len - 3;
    }
    resp[0] = 0x00;
    resp[1] = 0xFF;
    resp[2] = 0xFF;
    for (size_t i = 0; i < give; i++) {
        resp[3 + i] = src[i];
    }
    *resp_len = 3 + give;
    return 0;
}

static void
test_sdr_chunked_read(void)
{
    SdrHarness h = {
        //
        // Header byte 4 = body length = 25. Total record = 30 bytes.
        // That's larger than SDR_READ_CHUNK (23), so the chunked
        // strategy must issue 3 Get SDR calls: header(5), body(23),
        // body(2).
        //
        .header = { 0x01, 0x00, 0x51, 0x01, 25 },
    };
    for (size_t i = 0; i < sizeof(h.body); i++) {
        h.body[i] = (uint8_t)(0x40 + i);
    }

    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_SSIF, sdr_harness_send_raw, &h);

    uint8_t  rec[64];
    size_t   rec_len = sizeof(rec);
    uint16_t next;
    test_check(axl_ipmi_sdr_get(s, 0x0001, &next, rec, &rec_len) == 0,
               "sdr_get: chunked read ok");
    test_check(rec_len == 30,
               "sdr_get: reassembled length = header + body");
    test_check(h.call_count == 3,
               "sdr_get: three Get SDR calls (hdr + 23 + 2)");
    test_check(rec[0] == 0x01 && rec[4] == 25,
               "sdr_get: header bytes preserved");
    test_check(rec[5] == 0x40 && rec[29] == (0x40 + 24),
               "sdr_get: body bytes reassembled in order");
    test_check(next == 0xFFFF,
               "sdr_get: next_record_id from header phase");
}

static void
test_cc_failure(void)
{
    Canned c = {
        .resp = {0xC1},      // "Invalid command" completion code
        .resp_len = 1,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiDeviceId d;
    test_check(axl_ipmi_get_device_id(s, &d) == -1,
               "get_device_id: non-zero CC returns -1");
}

//
// SDR reservation-retry scenario. Harness drives a BMC that rejects
// unreserved partial reads:
//   call 1: Get SDR (rsv=0)          -> CC=0xC5
//   call 2: Reserve SDR Repository   -> CC=0 + rsv_id=0xBEEF
//   call 3: Get SDR (rsv=0xBEEF,off=0,5)  -> header
//   call 4: Get SDR (rsv=0xBEEF,off=5,N)  -> body
//
typedef struct {
    size_t   call_count;
    uint8_t  header[5];
    uint8_t  body[3];
} SdrReserveHarness;

static int
sdr_reserve_harness(void *user_data,
                    uint8_t netfn, uint8_t cmd,
                    const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t *resp_len)
{
    SdrReserveHarness *h = (SdrReserveHarness *)user_data;
    h->call_count++;

    //
    // Call 1: Get SDR with rsv=0 — reject.
    //
    if (h->call_count == 1 && netfn == 0x0A && cmd == 0x23 && req_len >= 2
        && req[0] == 0x00 && req[1] == 0x00)
    {
        resp[0] = 0xC5;
        *resp_len = 1;
        return 0;
    }
    //
    // Call 2: Reserve SDR Repository — grant 0xBEEF.
    //
    if (h->call_count == 2 && netfn == 0x0A && cmd == 0x22) {
        resp[0] = 0x00;
        resp[1] = 0xEF;
        resp[2] = 0xBE;
        *resp_len = 3;
        return 0;
    }
    //
    // Call 3 / 4: Get SDR with rsv=0xBEEF — honor.
    //
    if (netfn == 0x0A && cmd == 0x23 && req_len >= 6
        && req[0] == 0xEF && req[1] == 0xBE)
    {
        uint8_t  offset = req[4];
        uint8_t  ask    = req[5];
        resp[0] = 0x00;            // CC
        resp[1] = 0xFF;            // NextRecordId lo
        resp[2] = 0xFF;            // NextRecordId hi
        const uint8_t *src;
        size_t         src_len;
        if (offset == 0) {
            src = h->header; src_len = sizeof(h->header);
        } else if (offset == 5) {
            src = h->body;   src_len = sizeof(h->body);
        } else {
            *resp_len = 3;
            return 0;
        }
        size_t give = ask < src_len ? ask : src_len;
        for (size_t i = 0; i < give; i++) {
            resp[3 + i] = src[i];
        }
        *resp_len = 3 + give;
        return 0;
    }
    *resp_len = 0;
    return 0;
}

static void
test_sdr_reservation_retry(void)
{
    SdrReserveHarness h = {
        .header = { 0x01, 0x00, 0x51, 0x01, 3 },   // body_len = 3
        .body   = { 0xAA, 0xBB, 0xCC },
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_SSIF, sdr_reserve_harness, &h);

    uint8_t  rec[32];
    size_t   rec_len = sizeof(rec);
    uint16_t next;
    test_check(axl_ipmi_sdr_get(s, 0x0001, &next, rec, &rec_len) == 0,
               "sdr_reserve: succeeds after CC=0xC5 triggers Reserve SDR");
    test_check(h.call_count == 4,
               "sdr_reserve: four IPMI calls (reject + reserve + hdr + body)");
    test_check(rec_len == 8,
               "sdr_reserve: reassembled record length");
    test_check(rec[5] == 0xAA && rec[7] == 0xCC,
               "sdr_reserve: body bytes came through post-reservation");
}

static void
test_last_cc(void)
{
    //
    // On a success path, last_cc is 0x00. After a non-zero CC
    // response, the accessor exposes it so callers can distinguish
    // transport errors from BMC-level refusals.
    //
    //
    // get_chassis_status needs 3 data bytes after the CC (power state,
    // last power event, misc state).
    //
    Canned ok = { .resp = {0x00, 0x01, 0x00, 0x00}, .resp_len = 4 };
    AXL_AUTOPTR(AxlIpmiSession) s_ok = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &ok);
    AxlIpmiChassisStatus st;
    test_check(axl_ipmi_get_chassis_status(s_ok, &st) == 0,
               "last_cc: success path returns 0");
    test_check(axl_ipmi_session_last_cc(s_ok) == 0x00,
               "last_cc: records 0x00 on success");

    Canned bad = { .resp = {0xC1}, .resp_len = 1 };
    AXL_AUTOPTR(AxlIpmiSession) s_bad = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &bad);
    AxlIpmiDeviceId d;
    test_check(axl_ipmi_get_device_id(s_bad, &d) == -1,
               "last_cc: CC=0xC1 surfaces as -1");
    test_check(axl_ipmi_session_last_cc(s_bad) == 0xC1,
               "last_cc: records 0xC1 from failed command");
}

//
// Real-hardware exercise: relies on a working IPMI transport being
// available at runtime. Under test-axl.sh (plain QEMU, no BMC
// simulator) axl_ipmi_session_new() returns NULL and we soft-skip
// — so this test is a no-op in the default ratchet.
//
// Under test-ipmi-qemu.sh QEMU gets `-device ipmi-bmc-sim -device
// isa-ipmi-kcs,ioport=0xca2`. The session autodetects via the
// default KCS ports (they match the BMC sim attachment), and the
// simulator replies to Get Device ID with CC=0x00 + plausible
// bytes. Any breakage in the KCS wire framing (e.g. spurious
// trailing zero on zero-body commands) would cause the simulator
// to return CC=0xC7 "Request data length invalid" — this is
// exactly B2 from the code review.
//
// Both the typed wrapper and axl_ipmi_raw are exercised so the
// two paths (send_and_check through cmd.c vs. direct FSM) are
// both regression-covered.
//
static void
test_real_hw(void)
{
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new();
    if (s == NULL) {
        axl_printf("SKIP: real_hw: no IPMI transport available\n");
        return;
    }
    axl_printf("INFO: real_hw: transport=%d\n",
               (int)axl_ipmi_session_transport(s));

    AxlIpmiDeviceId d;
    int rc = axl_ipmi_get_device_id(s, &d);
    test_check(rc == 0,
               "real_hw: Get Device ID succeeds against live BMC");
    if (rc == 0) {
        test_check(d.manufacturer_id != 0 || d.product_id != 0 || d.device_id != 0,
                   "real_hw: response has plausible content");
    }

    //
    // Raw-path regression: Get Device ID is a zero-body command,
    // i.e. the path that B2 broke by appending a spurious trailing
    // zero byte after WRITE_END. A strict BMC (and the QEMU
    // simulator is strict) returns CC=0xC7 in that case.
    //
    uint8_t resp[16];
    size_t  resp_len = sizeof(resp);
    rc = axl_ipmi_raw(s, /*netfn=*/0x06, /*cmd=*/0x01,
                      /*req=*/NULL, /*req_len=*/0,
                      resp, &resp_len);
    test_check(rc == 0,
               "real_hw: raw Get Device ID (zero-body) succeeds");
    if (rc == 0) {
        test_check(resp_len >= 1 && resp[0] == 0x00,
                   "real_hw: raw CC=0x00 (catches KCS wire-framing bugs)");
        test_check(resp_len >= 12,
                   "real_hw: raw response has the full Get Device ID payload");
    }
}

static void
test_truncated_response(void)
{
    Canned c = {
        .resp = {0x00, 0x20},    // CC + 1 byte, not enough for get_device_id
        .resp_len = 2,
    };
    AXL_AUTOPTR(AxlIpmiSession) s = axl_ipmi_session_new_with_callback(
        AXL_IPMI_TRANSPORT_KCS, canned_send_raw, &c);

    AxlIpmiDeviceId d;
    test_check(axl_ipmi_get_device_id(s, &d) == -1,
               "get_device_id: truncated response returns -1");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_ipmi_main(int argc, char **argv)
{
    //
    // The hardware exercise only runs when argv[1]=="hw". Plain
    // test-axl.sh invokes us without args and skips the hardware
    // path; test-ipmi-qemu.sh passes "hw" after attaching the
    // BMC simulator. Guarding on a CLI arg instead of
    // axl_ipmi_session_new() returning NULL matters because the
    // default-KCS fallback "opens" successfully even without a
    // BMC behind it (the I/O port read returns 0xFF but the x86
    // `inb` instruction itself always succeeds), so the library
    // doesn't have a clean way to detect "no real hardware" from
    // inside the session constructor.
    //
    bool run_hw = (argc > 1 && axl_strcmp(argv[1], "hw") == 0);

    test_print_header("AxlIpmi");

    test_session_callback();
    test_session_callback_rejects_null();

    test_get_device_id();
    test_get_chassis_status();
    test_chassis_control();
    test_sel_info();
    test_sel_get_entry();
    test_sdr_info();
    test_sensor_reading();
    test_fru_info();
    test_fru_read();

    test_bmc_cold_reset();
    test_bmc_warm_reset();
    test_sdr_chunked_read();
    test_sdr_reservation_retry();

    test_last_cc();
    test_cc_failure();
    test_truncated_response();

    //
    // Real-hardware test: only when explicitly requested (e.g. via
    // test-ipmi-qemu.sh which attaches the BMC simulator).
    //
    if (run_hw) {
        test_real_hw();
    }

    return test_print_results();
}

AXL_APP(test_ipmi_main)
