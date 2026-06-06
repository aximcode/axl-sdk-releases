/** @file axl-test-driver.c
    Unit tests for the AXL driver-authoring surface (axl-driver.h).

    Phase 1 covers the protocol-publishing primitive
    axl_protocol_install / axl_protocol_uninstall: the AXL-typed wrappers
    a Type-A resident driver uses to publish a protocol interface without
    a gBS-> drop-down. Tests install a sentinel interface under a private
    test GUID, then confirm via the firmware's own LocateProtocol that it
    really landed in the protocol database (and is gone after uninstall).

    AxlGuid is binary-compatible with EFI_GUID and AxlHandle is
    EFI_HANDLE, so the test casts between them to cross-check the AXL
    surface against the raw firmware view.
**/

#include "axl-test.h"
#include <axl/axl-driver.h>

//
// <uefi/axl-uefi.h> declares gBS and the EFI types used to cross-check
// the install against the firmware's own protocol database. Never
// include backend-internal headers from tests.
//
#include <uefi/axl-uefi.h>

// ---------------------------------------------------------------------------
// Private test GUIDs + sentinel interfaces (file scope: UEFI is single-process)
// ---------------------------------------------------------------------------

static const AxlGuid TEST_PROTO_GUID = {
    0xa1b2c3d4, 0xe5f6, 0x4789, { 0x8a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61 }
};
static const AxlGuid TEST_PROTO_GUID2 = {
    0xa1b2c3d4, 0xe5f6, 0x4789, { 0x8a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x62 }
};

typedef struct {
    uint32_t magic;
} TestIface;

static TestIface g_iface  = { 0xCAFEF00D };
static TestIface g_iface2 = { 0xBADC0FFE };

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void
test_install_fresh_handle(void)
{
    AxlHandle h = NULL;   /* fresh-handle path */
    int r = axl_protocol_install(&TEST_PROTO_GUID, &g_iface, &h);
    test_check(r == AXL_OK, "install: fresh handle returns AXL_OK");
    test_check(h != NULL, "install: fresh handle was allocated (non-NULL)");

    /* Cross-check against the firmware's own view. */
    void      *found = NULL;
    EFI_STATUS s = gBS->LocateProtocol((EFI_GUID *)&TEST_PROTO_GUID, NULL, &found);
    test_check(!EFI_ERROR(s) && found == &g_iface,
               "install: protocol locatable and returns the installed interface");

    /* Install a SECOND protocol on the SAME handle — handle unchanged. */
    AxlHandle h2 = h;
    int r2 = axl_protocol_install(&TEST_PROTO_GUID2, &g_iface2, &h2);
    test_check(r2 == AXL_OK && h2 == h,
               "install: second protocol on existing handle keeps the handle");

    /* Uninstall the first; it must no longer be locatable. */
    int u = axl_protocol_uninstall(h, &TEST_PROTO_GUID, &g_iface);
    test_check(u == AXL_OK, "uninstall: returns AXL_OK");
    found = NULL;
    s = gBS->LocateProtocol((EFI_GUID *)&TEST_PROTO_GUID, NULL, &found);
    test_check(EFI_ERROR(s), "uninstall: protocol no longer locatable");

    /* Clean up the second. */
    test_check(axl_protocol_uninstall(h, &TEST_PROTO_GUID2, &g_iface2) == AXL_OK,
               "uninstall: second protocol returns AXL_OK");
}

static void
test_install_null_safety(void)
{
    AxlHandle h = NULL;
    test_check(axl_protocol_install(&TEST_PROTO_GUID, &g_iface, NULL) == AXL_ERR,
               "install: NULL handle pointer returns AXL_ERR");
    test_check(axl_protocol_install(NULL, &g_iface, &h) == AXL_ERR,
               "install: NULL guid returns AXL_ERR");
    test_check(axl_protocol_install(&TEST_PROTO_GUID, NULL, &h) == AXL_ERR,
               "install: NULL iface returns AXL_ERR");
    test_check(h == NULL, "install: rejected calls leave the handle untouched");

    test_check(axl_protocol_uninstall(NULL, &TEST_PROTO_GUID, &g_iface) == AXL_ERR,
               "uninstall: NULL handle returns AXL_ERR");
    test_check(axl_protocol_uninstall(&h, NULL, &g_iface) == AXL_ERR,
               "uninstall: NULL guid returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// Type-B Driver Model binding — a synthetic "bus" protocol on a test-created
// controller, driven by the firmware's real ConnectController / Disconnect.
// ---------------------------------------------------------------------------

static const AxlGuid SYNTH_BIND_GUID = {
    0xb1c2d3e4, 0xf5a6, 0x47b8, { 0x9c, 0x0d, 0x2e, 0x3f, 0x40, 0x51, 0x62, 0x73 }
};
static TestIface g_synth_iface = { 0x5151D135 };

static int       g_bind_supported = 0;
static int       g_bind_start     = 0;
static int       g_bind_stop      = 0;
static AxlHandle g_bind_ctrl      = NULL;
static void     *g_bind_iface     = NULL;

static bool bind_supported(AxlHandle ctrl, void *ctx)
{
    (void)ctrl; (void)ctx; g_bind_supported++; return true;
}
static int bind_start(AxlHandle ctrl, void *iface, void *ctx)
{
    (void)ctx; g_bind_start++; g_bind_ctrl = ctrl; g_bind_iface = iface;
    return AXL_OK;
}
static int bind_stop(AxlHandle ctrl, void *ctx)
{
    (void)ctrl; (void)ctx; g_bind_stop++; return AXL_OK;
}

static void
test_driver_binding(void)
{
    /* A synthetic controller: a fresh handle carrying SYNTH_BIND_GUID. */
    AxlHandle controller = NULL;
    test_check(axl_protocol_install(&SYNTH_BIND_GUID, &g_synth_iface,
                                    &controller) == AXL_OK && controller != NULL,
               "binding: synthetic controller created");

    /* Install a Type-B binding that manages SYNTH_BIND_GUID controllers. */
    AxlDriverBinding db = {
        .name      = "axl-test-binding",
        .binds     = &SYNTH_BIND_GUID,
        .supported = bind_supported,
        .start     = bind_start,
        .stop      = bind_stop,
        .ctx       = NULL,
    };
    test_check(axl_driver_binding_install(&db) == AXL_OK, "binding: install OK");

    /* v1 is one binding per image handle: a second install is rejected
       (the firmware refuses a duplicate EFI_DRIVER_BINDING_PROTOCOL). */
    test_check(axl_driver_binding_install(&db) == AXL_ERR,
               "binding: second install on the image handle rejected (v1)");

    /* The firmware's ConnectController drives Supported -> Start. */
    EFI_STATUS s = gBS->ConnectController((EFI_HANDLE)controller, NULL, NULL, TRUE);
    test_check(!EFI_ERROR(s), "binding: ConnectController succeeded");
    test_check(g_bind_supported >= 1, "binding: Supported was consulted");
    test_check(g_bind_start == 1, "binding: Start fired exactly once");
    test_check(g_bind_ctrl == controller, "binding: Start got the controller handle");
    test_check(g_bind_iface == &g_synth_iface, "binding: Start got the bound interface");

    /* axl_driver_disconnect_handle drives Stop and releases the BY_DRIVER
       open (the AXL wrapper over the firmware's DisconnectController). */
    test_check(axl_driver_disconnect_handle(controller) == AXL_OK,
               "binding: axl_driver_disconnect_handle succeeded");
    test_check(g_bind_stop == 1, "binding: Stop fired on disconnect");

    /* Re-connect / re-disconnect: Start/Stop fire again. */
    s = gBS->ConnectController((EFI_HANDLE)controller, NULL, NULL, TRUE);
    test_check(!EFI_ERROR(s) && g_bind_start == 2,
               "binding: re-ConnectController fires Start again");
    test_check(axl_driver_disconnect_handle(controller) == AXL_OK,
               "binding: re-disconnect via handle succeeded");
    test_check(g_bind_stop == 2, "binding: re-Disconnect fires Stop again");

    /* Clean up the synthetic controller. The uninstall must succeed — if Stop
       failed to CloseProtocol the BY_DRIVER open, this would return AXL_ERR
       (the proto still open), catching that regression. */
    test_check(axl_protocol_uninstall(controller, &SYNTH_BIND_GUID,
                                      &g_synth_iface) == AXL_OK,
               "binding: synthetic controller uninstalled cleanly after Stop");

    /* Explicit teardown — the path a Type-B *driver* uses from its unload
       callback (the axl_atexit hook only fires at app exit, not driver
       unload). Proven to really remove the binding by a fresh install
       succeeding again afterward, then a final uninstall to leave clean. */
    test_check(axl_driver_binding_uninstall() == AXL_OK,
               "binding: explicit uninstall succeeds");
    test_check(axl_driver_binding_install(&db) == AXL_OK,
               "binding: re-install after uninstall succeeds (image slot freed)");
    test_check(axl_driver_binding_uninstall() == AXL_OK,
               "binding: second uninstall cleans up");
    test_check(axl_driver_binding_uninstall() == AXL_ERR,
               "binding: uninstall with nothing installed returns AXL_ERR");
}

// axl_driver_disconnect_handle's argument + NOT_FOUND contract, independent
// of a live binding (the bound-driver Stop path is covered by
// test_driver_binding above).
static void
test_disconnect_handle_contract(void)
{
    test_check(axl_driver_disconnect_handle(NULL) == AXL_ERR,
               "disconnect_handle: NULL handle returns AXL_ERR");

    /* A handle with no driver managing it disconnects cleanly (the firmware
       returns EFI_SUCCESS for an unmanaged controller; the wrapper also maps
       EFI_NOT_FOUND to AXL_OK as a defensive symmetry with connect_handle). */
    AxlHandle h = NULL;
    test_check(axl_protocol_install(&TEST_PROTO_GUID, &g_iface, &h) == AXL_OK,
               "disconnect_handle: scratch handle created");
    test_check(axl_driver_disconnect_handle(h) == AXL_OK,
               "disconnect_handle: nothing bound returns AXL_OK");
    test_check(axl_protocol_uninstall(h, &TEST_PROTO_GUID, &g_iface) == AXL_OK,
               "disconnect_handle: scratch handle cleaned up");
}

int
test_driver_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_print_header("AxlDriver");

    test_install_fresh_handle();
    test_install_null_safety();
    test_driver_binding();
    test_disconnect_handle_contract();

    return test_print_results();
}

AXL_APP(test_driver_main)
