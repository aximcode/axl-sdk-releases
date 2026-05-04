/** @file axl-test-smbus.c
    Unit tests for AxlSmbus.

    Installs a capturing mock EFI_I2C_MASTER_PROTOCOL (and separately
    EFI_SMBUS_HC_PROTOCOL) via gBS->InstallProtocolInterface so
    axl_smbus_new() locks onto the mock. Every axl_smbus_read_block /
    _write_block call is captured; tests assert against the exact byte
    sequence AxlSmbus would put on the wire.

    These tests are the automated regression for B1 (code review 78859fa):
    the SMBus block-transfer byte-count prefix MUST appear at buf[1] on
    writes, and the response count byte MUST be stripped on reads. The
    I2C framing path in axl-smbus-i2c.c has no other automated coverage
    — mock-callback IPMI tests inject above the transport layer and
    skip this code entirely.
**/

#include "axl-test.h"
#include <axl/axl-smbus.h>

//
// <uefi/axl-uefi.h> is the public UEFI-types umbrella; it declares gBS
// (via axl-uefi-extra.h) and provides the UEFI structs/GUIDs this test
// needs for gBS->InstallProtocolInterface. Never include backend-internal
// headers from tests.
//
#include <uefi/axl-uefi.h>

// ---------------------------------------------------------------------------
// Capturing mock state (file scope: UEFI is single-process)
// ---------------------------------------------------------------------------

#define MOCK_BUF_CAP  64

typedef struct {
    // Captured request
    size_t    call_count;
    UINTN     captured_slave;
    UINTN     captured_op_count;
    uint32_t  captured_flags[2];
    uint8_t   captured_buf[2][MOCK_BUF_CAP];
    size_t    captured_len[2];

    // Response to deliver (used when op[last].Flags & I2C_FLAG_READ)
    uint8_t   response[MOCK_BUF_CAP];
    size_t    response_len;

    // Injected fault for StartRequest's return
    EFI_STATUS inject_status;
} MockCapture;

static MockCapture g_cap;

static void
mock_reset(void)
{
    for (size_t i = 0; i < sizeof(g_cap); i++) {
        ((uint8_t *)&g_cap)[i] = 0;
    }
    g_cap.inject_status = EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Mock EFI_I2C_MASTER_PROTOCOL
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
mock_i2c_start_request(const EFI_I2C_MASTER_PROTOCOL  *This,
                       UINTN                           SlaveAddress,
                       EFI_I2C_REQUEST_PACKET         *RequestPacket,
                       EFI_EVENT                       Event,
                       EFI_STATUS                     *I2cStatus)
{
    (void)This;
    (void)Event;
    (void)I2cStatus;

    g_cap.call_count++;
    g_cap.captured_slave     = SlaveAddress;
    g_cap.captured_op_count  = RequestPacket->OperationCount;

    //
    // Capture up to two operations. AxlSmbus forms exactly one
    // (block write) or two (block read) ops; larger shapes get
    // silently truncated here and the test assertion on
    // captured_op_count will fail loudly.
    //
    size_t captured = RequestPacket->OperationCount;
    if (captured > 2) captured = 2;
    for (size_t i = 0; i < captured; i++) {
        EFI_I2C_OPERATION *op = &RequestPacket->Operation[i];
        g_cap.captured_flags[i] = op->Flags;

        size_t copy = op->LengthInBytes;
        if (copy > MOCK_BUF_CAP) copy = MOCK_BUF_CAP;

        if (op->Flags & I2C_FLAG_READ) {
            //
            // Read op: device delivers data. Fill caller's buffer
            // from g_cap.response (which includes the SMBus count
            // byte at offset 0). Also capture the outbound length
            // so the test can verify how much AxlSmbus asked for.
            //
            g_cap.captured_len[i] = op->LengthInBytes;
            size_t fill = g_cap.response_len;
            if (fill > copy) fill = copy;
            for (size_t j = 0; j < fill; j++) {
                op->Buffer[j] = g_cap.response[j];
            }
        } else {
            //
            // Write op: capture the exact bytes AxlSmbus put on the
            // wire. This is where B1's byte-count prefix must appear.
            //
            g_cap.captured_len[i] = copy;
            for (size_t j = 0; j < copy; j++) {
                g_cap.captured_buf[i][j] = op->Buffer[j];
            }
        }
    }

    return g_cap.inject_status;
}

//
// Minimal mock protocol — only StartRequest is exercised by
// AxlSmbus. SetBusFrequency and Reset remain NULL; the opener
// explicitly tolerates that.
//
static EFI_I2C_MASTER_PROTOCOL g_mock_i2c = {
    .SetBusFrequency           = NULL,
    .Reset                     = NULL,
    .StartRequest              = mock_i2c_start_request,
    .I2cControllerCapabilities = NULL,
};

// ---------------------------------------------------------------------------
// Mock EFI_SMBUS_HC_PROTOCOL (for transport-selection test only)
// ---------------------------------------------------------------------------

static bool g_hc_called;

static EFI_STATUS EFIAPI
mock_hc_execute(const EFI_SMBUS_HC_PROTOCOL  *This,
                EFI_SMBUS_DEVICE_ADDRESS      SlaveAddress,
                EFI_SMBUS_DEVICE_COMMAND      Command,
                EFI_SMBUS_OPERATION           Operation,
                BOOLEAN                       PecCheck,
                UINTN                        *Length,
                VOID                         *Buffer)
{
    (void)This;
    (void)SlaveAddress;
    (void)Command;
    (void)Operation;
    (void)PecCheck;
    (void)Length;
    (void)Buffer;
    g_hc_called = true;
    return EFI_SUCCESS;
}

static EFI_SMBUS_HC_PROTOCOL g_mock_hc = {
    .Execute   = mock_hc_execute,
    .ArpDevice = NULL,
    .GetArpMap = NULL,
    .Notify    = NULL,
};

// ---------------------------------------------------------------------------
// Protocol install / uninstall helpers
// ---------------------------------------------------------------------------

static EFI_HANDLE g_i2c_handle = NULL;
static EFI_HANDLE g_hc_handle  = NULL;

static bool
install_mock_i2c(void)
{
    EFI_GUID guid = EFI_I2C_MASTER_PROTOCOL_GUID;
    EFI_STATUS s = gBS->InstallProtocolInterface(
        &g_i2c_handle, &guid, EFI_NATIVE_INTERFACE, &g_mock_i2c);
    return !EFI_ERROR(s);
}

static void
uninstall_mock_i2c(void)
{
    if (g_i2c_handle == NULL) {
        return;
    }
    EFI_GUID guid = EFI_I2C_MASTER_PROTOCOL_GUID;
    gBS->UninstallProtocolInterface(g_i2c_handle, &guid, &g_mock_i2c);
    g_i2c_handle = NULL;
}

static bool
install_mock_hc(void)
{
    EFI_GUID guid = gEfiSmbusHcProtocolGuid;
    EFI_STATUS s = gBS->InstallProtocolInterface(
        &g_hc_handle, &guid, EFI_NATIVE_INTERFACE, &g_mock_hc);
    return !EFI_ERROR(s);
}

static void
uninstall_mock_hc(void)
{
    if (g_hc_handle == NULL) {
        return;
    }
    EFI_GUID guid = gEfiSmbusHcProtocolGuid;
    gBS->UninstallProtocolInterface(g_hc_handle, &guid, &g_mock_hc);
    g_hc_handle = NULL;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static bool
bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

/* Transport selection ---------------------------------------------------- */

//
// Tests below assume `install_mock_i2c()` has already run once from
// test_smbus_main(); they only mutate capture state via mock_reset().
// With only I2C present, axl_smbus_new() must pick
// AXL_SMBUS_TRANSPORT_I2C. The HC-preferred test does the one
// temporary HC install/uninstall we need.
//
static void
test_transport_selection_i2c_only(void)
{
    mock_reset();

    AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();
    test_check(s != NULL, "axl_smbus_new: opens with mock I2C present");
    test_check(axl_smbus_transport(s) == AXL_SMBUS_TRANSPORT_I2C,
               "axl_smbus_transport: reports I2C when only I2C installed");
}

static void
test_transport_selection_hc_preferred(void)
{
    mock_reset();
    g_hc_called = false;

    //
    // Temporarily install HC on top of the already-present I2C so we
    // can verify HC wins. Uninstalled at the end so downstream tests
    // continue to see I2C-only and exercise the B1 framing path.
    //
    test_check(install_mock_hc(),  "install_mock_hc: succeeds");

    AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();
    test_check(s != NULL, "axl_smbus_new: opens with both mocks present");
    test_check(axl_smbus_transport(s) == AXL_SMBUS_TRANSPORT_HC,
               "axl_smbus_transport: HC wins over I2C");

    //
    // Issue a block write and verify the HC path is taken — the I2C
    // mock's StartRequest counter must NOT advance, and the HC
    // mock's flag MUST fire.
    //
    uint8_t payload = 0xAB;
    int rc = axl_smbus_write_block(s, 0x10, 0x02, &payload, 1);
    test_check(rc == AXL_OK, "axl_smbus_write_block (HC): succeeds");
    test_check(g_hc_called,
               "axl_smbus_write_block (HC): dispatches via EFI_SMBUS_HC_PROTOCOL");
    test_check(g_cap.call_count == 0,
               "axl_smbus_write_block (HC): does NOT call I2C mock");

    uninstall_mock_hc();
}

/* B1 regression: block-write framing ------------------------------------- */

//
// This is the direct regression for commit 78859fa's write-side
// fix. AxlSmbus must emit one write operation with exactly
// [cmd][byte_count][payload...] — the byte_count MUST appear at
// byte 1. A revert of 78859fa would produce [cmd][payload...]
// (no count) and this test would fail loudly on the captured_buf
// comparison.
//
static void
test_block_write_byte_count_prefix(void)
{
    mock_reset();

    AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();
    test_check(s != NULL, "axl_smbus_new: I2C path ready");

    const uint8_t payload[2] = { 0x18, 0x01 }; /* NetFn App << 2 | cmd Get Device ID */
    int rc = axl_smbus_write_block(s, /*slave=*/0x10, /*command=*/0x02,
                                   payload, sizeof(payload));
    test_check(rc == AXL_OK, "axl_smbus_write_block: returns 0");
    test_check(g_cap.call_count == 1, "block_write: exactly one I2C dispatch");
    test_check(g_cap.captured_slave == 0x10,
               "block_write: slave address passed through");
    test_check(g_cap.captured_op_count == 1,
               "block_write: emits exactly one I2C operation");
    test_check((g_cap.captured_flags[0] & I2C_FLAG_READ) == 0,
               "block_write: op flags mark a write (no I2C_FLAG_READ)");
    test_check(g_cap.captured_len[0] == 4,
               "block_write: wire length = 1 cmd + 1 count + 2 payload");

    const uint8_t expected[4] = { 0x02, 0x02, 0x18, 0x01 };
    test_check(bytes_equal(g_cap.captured_buf[0], expected, 4),
               "B1 regression: wire format is [cmd][count][payload...]");
}

/* B1 regression: block-read count-byte strip ----------------------------- */

//
// This is the direct regression for commit 78859fa's read-side
// fix. The mock hands back [count=12, payload...]; AxlSmbus must
// strip the count byte and return the payload cleanly with
// *len = 12. A revert of 78859fa would either return the count
// byte as part of the payload or leave len wrong.
//
static void
test_block_read_strips_count_byte(void)
{
    mock_reset();

    //
    // SMBus block-read response: [count=0x0C, payload bytes 0x01..0x0C].
    //
    g_cap.response_len = 13;
    g_cap.response[0]  = 0x0C;
    for (size_t i = 0; i < 12; i++) {
        g_cap.response[i + 1] = (uint8_t)(i + 1);
    }

    AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();
    test_check(s != NULL, "axl_smbus_new: I2C path ready (read)");

    uint8_t out[32];
    size_t  out_len = sizeof(out);
    int rc = axl_smbus_read_block(s, /*slave=*/0x10, /*command=*/0x03,
                                  out, &out_len);
    test_check(rc == AXL_OK, "axl_smbus_read_block: returns 0");
    test_check(g_cap.call_count == 1, "block_read: exactly one I2C dispatch");
    test_check(g_cap.captured_op_count == 2,
               "block_read: emits exactly two I2C operations");
    test_check(g_cap.captured_flags[0] == 0,
               "block_read: op[0] is a write (the cmd byte)");
    test_check(g_cap.captured_len[0] == 1 &&
               g_cap.captured_buf[0][0] == 0x03,
               "block_read: op[0] writes the SMBus command byte");
    test_check((g_cap.captured_flags[1] & I2C_FLAG_READ) != 0,
               "block_read: op[1] is a read");

    test_check(out_len == 12,
               "B1 regression: count byte stripped (len = payload size)");
    for (size_t i = 0; i < out_len; i++) {
        if (out[i] != (uint8_t)(i + 1)) {
            test_fail("B1 regression: payload shifted by count byte");
            return;
        }
    }
    test_pass("B1 regression: payload matches expected bytes 0x01..0x0C");
}

/* Length overflow rejection --------------------------------------------- */

static void
test_write_rejects_oversized(void)
{
    mock_reset();

    AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();

    uint8_t big[AXL_SMBUS_BLOCK_MAX + 1];
    int rc = axl_smbus_write_block(s, 0x10, 0x02, big, sizeof(big));
    test_check(rc == AXL_ERR,
               "axl_smbus_write_block: rejects len > AXL_SMBUS_BLOCK_MAX");
    test_check(g_cap.call_count == 0,
               "axl_smbus_write_block: no dispatch on overflow");
}

/* Short-response clamp --------------------------------------------------- */

static void
test_read_clamps_to_caller_capacity(void)
{
    mock_reset();

    //
    // Device falsely reports 20 payload bytes but only delivers 10
    // (classic firmware bug). AxlSmbus must clamp to the caller's
    // buffer capacity, not to the count byte.
    //
    g_cap.response[0] = 0x14;          // count = 20 (bogus)
    g_cap.response_len = 11;            // reality: 10 payload bytes
    for (size_t i = 0; i < 10; i++) {
        g_cap.response[i + 1] = 0xAA;
    }

    AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();

    uint8_t out[8];                    // smaller than the claimed count
    size_t  out_len = sizeof(out);
    int rc = axl_smbus_read_block(s, 0x10, 0x03, out, &out_len);
    test_check(rc == AXL_OK, "axl_smbus_read_block (short/bogus): returns 0");
    test_check(out_len == sizeof(out),
               "axl_smbus_read_block: clamps to caller's capacity");
}

/* StartRequest error propagation --------------------------------------- */

static void
test_write_propagates_error(void)
{
    mock_reset();

    g_cap.inject_status = EFI_DEVICE_ERROR;

    AXL_AUTOPTR(AxlSmbus) s = axl_smbus_new();

    uint8_t p = 0x55;
    int rc = axl_smbus_write_block(s, 0x10, 0x02, &p, 1);
    test_check(rc == AXL_ERR,
               "axl_smbus_write_block: returns -1 on StartRequest error");
}

/* NULL guards ----------------------------------------------------------- */

static void
test_null_safety(void)
{
    uint8_t buf[4] = { 0 };
    size_t  len = sizeof(buf);

    //
    // NULL session: the module must return -1 without crashing.
    // (The error path is cheap to test and is the typical way a
    // consumer misuses the API.)
    //
    test_check(axl_smbus_read_block(NULL, 0x10, 0x03, buf, &len) == AXL_ERR,
               "axl_smbus_read_block: rejects NULL session");
    test_check(axl_smbus_write_block(NULL, 0x10, 0x02, buf, 1) == AXL_ERR,
               "axl_smbus_write_block: rejects NULL session");

    test_check(axl_smbus_transport(NULL) == AXL_SMBUS_TRANSPORT_UNKNOWN,
               "axl_smbus_transport: NULL session → UNKNOWN");

    axl_smbus_free(NULL);
    test_pass("axl_smbus_free: NULL-safe");
}

/* transport_string formatting ------------------------------------------ */

static void
test_transport_string(void)
{
    test_check(axl_smbus_transport_string(AXL_SMBUS_TRANSPORT_HC) != NULL,
               "transport_string HC: not NULL");
    test_check(axl_smbus_transport_string(AXL_SMBUS_TRANSPORT_I2C) != NULL,
               "transport_string I2C: not NULL");
    test_check(axl_smbus_transport_string(AXL_SMBUS_TRANSPORT_UNKNOWN) != NULL,
               "transport_string UNKNOWN: not NULL");
    //
    // Verify HC and I2C strings are distinct so debug log lines are
    // useful.
    //
    const char *a = axl_smbus_transport_string(AXL_SMBUS_TRANSPORT_HC);
    const char *b = axl_smbus_transport_string(AXL_SMBUS_TRANSPORT_I2C);
    test_check(axl_strcmp(a, b) != 0,
               "transport_string: HC and I2C produce different strings");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_smbus_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlSmbus");

    //
    // API-shape tests that don't touch the backend run first with no
    // mock protocol installed.
    //
    test_null_safety();
    test_transport_string();

    //
    // Everything below needs the I2C Master mock in the protocol DB.
    // Install once, reset capture state between tests, uninstall once.
    // This deliberately minimizes churn on the UEFI protocol database
    // — each InstallProtocolInterface/UninstallProtocolInterface pair
    // fires protocol-registration notifications that can reactivate
    // driver-binding dispatch, and we saw evidence on AARCH64 TCG
    // that repeated churn occasionally interacts badly with the
    // network stack binaries that run later in the suite.
    //
    test_check(install_mock_i2c(), "install_mock_i2c (suite-level): succeeds");

    test_transport_selection_i2c_only();
    test_transport_selection_hc_preferred();
    test_block_write_byte_count_prefix();
    test_block_read_strips_count_byte();
    test_write_rejects_oversized();
    test_read_clamps_to_caller_capacity();
    test_write_propagates_error();

    uninstall_mock_i2c();

    return test_print_results();
}

AXL_APP(test_smbus_main)
