/** @file ipmi_fuzz.c
    libFuzzer entry point for the AxlIpmi response parsers.

    Every typed command in `src/ipmi/axl-ipmi-cmd.c` builds a
    request, hands it to `axl_ipmi_raw()`, and decodes the reply.
    This harness overrides `axl_ipmi_raw()` at link time so it
    returns the fuzzer's byte string as the response; that lets us
    drive the decoders with hostile bytes without standing up a
    session, transport, or backend.

    Coverage includes every command wrapper plus the three string-
    table format helpers. Typical finds would be out-of-bounds
    reads on truncated responses or integer-overflow on length
    fields — the kind of bug a malformed BMC reply could expose.
**/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <axl/axl-ipmi.h>

// ---------------------------------------------------------------------------
// Shimmed axl_ipmi_raw: copy fuzzer input into the caller's response.
//
// The real implementation lives in src/ipmi/axl-ipmi.c, which we
// deliberately don't compile in this target. Providing the symbol
// here at link time lets us exercise axl-ipmi-cmd.c (which calls
// axl_ipmi_raw) in isolation.
// ---------------------------------------------------------------------------

static const uint8_t  *g_fuzz_data;
static size_t          g_fuzz_size;
static size_t          g_fuzz_offset;

int
axl_ipmi_raw(AxlIpmiSession *session,
             uint8_t netfn, uint8_t cmd,
             const uint8_t *req, size_t req_len,
             uint8_t *resp, size_t *resp_len)
{
    (void)session; (void)netfn; (void)cmd; (void)req; (void)req_len;

    //
    // Advance a cursor across the fuzzer input so each axl_ipmi_raw
    // call sees a different slice. Matters for the multi-call
    // paths like axl_ipmi_sdr_get (3 calls per record) — with a
    // static slice every call got identical bytes and state-
    // dependent reassembly bugs were unreachable.
    //
    if (g_fuzz_size == 0) {
        *resp_len = 0;
        return 0;
    }
    if (g_fuzz_offset >= g_fuzz_size) {
        g_fuzz_offset = 0;   // wrap so long command sequences still get bytes
    }

    size_t remain = g_fuzz_size - g_fuzz_offset;
    size_t n = remain < *resp_len ? remain : *resp_len;
    memcpy(resp, g_fuzz_data + g_fuzz_offset, n);
    *resp_len = n;
    g_fuzz_offset += n;
    return 0;
}

// ---------------------------------------------------------------------------
// Fuzz entry
// ---------------------------------------------------------------------------

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    g_fuzz_data = data;
    g_fuzz_size = size;
    g_fuzz_offset = 0;

    //
    // AxlIpmiSession is opaque; the command wrappers only pass it
    // through to axl_ipmi_raw() (which this TU overrides) and
    // NULL-check it. Any non-NULL pointer satisfies the contract.
    //
    AxlIpmiSession *session = (AxlIpmiSession *)(uintptr_t)1;

    //
    // Exercise every typed wrapper. Return value is ignored —
    // we only care that nothing crashes or leaks under ASan.
    //
    AxlIpmiDeviceId d;
    axl_ipmi_get_device_id(session, &d);

    AxlIpmiChassisStatus st;
    axl_ipmi_get_chassis_status(session, &st);

    axl_ipmi_chassis_control(session, AXL_IPMI_CHASSIS_POWER_UP);

    AxlIpmiSelInfo si;
    axl_ipmi_sel_info(session, &si);

    AxlIpmiSelEntry se;
    axl_ipmi_sel_get_entry(session, 0x0000, &se);

    AxlIpmiSdrInfo sd;
    axl_ipmi_sdr_info(session, &sd);

    uint8_t  sdr_buf[128];
    size_t   sdr_len = sizeof(sdr_buf);
    uint16_t next_id;
    axl_ipmi_sdr_get(session, 0x0000, &next_id, sdr_buf, &sdr_len);

    AxlIpmiSensorReading r;
    axl_ipmi_get_sensor_reading(session, 0x10, &r);

    AxlIpmiFruInfo fi;
    axl_ipmi_fru_info(session, 0, &fi);

    uint8_t  fru_buf[64];
    size_t   fru_len = sizeof(fru_buf);
    axl_ipmi_fru_read(session, 0, 0, fru_buf, &fru_len);

    //
    // Format helpers: static string-table lookups. Use the first
    // byte of the fuzzer input to index — any uint8_t input must
    // return a non-NULL string without faulting.
    //
    uint8_t idx = size > 0 ? data[0] : 0;
    const char *s1 = axl_ipmi_completion_code_string(idx);
    const char *s2 = axl_ipmi_sensor_type_string(idx);
    const char *s3 = axl_ipmi_entity_id_string(idx);
    (void)s1; (void)s2; (void)s3;

    return 0;
}
