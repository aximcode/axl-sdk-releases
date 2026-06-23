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
#include <axl/axl-driver-info.h>

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
// Buffer-load identity + app-dir-restricted load: safe-negative validation.
//
// Only the errors AXL's own validation produces BEFORE any firmware call are
// asserted here (NULL / 0 / a name that could escape the app directory, and a
// well-formed-but-absent sibling). A real LoadImage of a bogus blob is NOT
// exercised — on UEFI that faults rather than returning cleanly
// (feedback_uefi_firmware_test_hazards). The positive round-trip + the exact
// synthesized device-path string live in the QEMU integration test
// (test-driver-identity-qemu.sh).
// ---------------------------------------------------------------------------

static void
test_buffer_identity_and_sibling(void)
{
    AxlDriverHandle h     = (AxlDriverHandle)0x1;  // sentinel: must be cleared on reject
    unsigned char   blob[4] = { 'M', 'Z', 0, 0 };

    // axl_driver_load_buffer_with_image_info argument validation.
    test_check(axl_driver_load_buffer_with_image_info(NULL, 4, NULL, &h) == AXL_ERR,
               "load_buffer_with_image_info: NULL buffer returns AXL_ERR");
    test_check(axl_driver_load_buffer_with_image_info(blob, 0, NULL, &h) == AXL_ERR,
               "load_buffer_with_image_info: zero length returns AXL_ERR");
    test_check(axl_driver_load_buffer_with_image_info(blob, 4, NULL, NULL) == AXL_ERR,
               "load_buffer_with_image_info: NULL out_handle returns AXL_ERR");

    // axl_driver_load_sibling argument validation.
    h = (AxlDriverHandle)0x1;
    test_check(axl_driver_load_sibling(NULL, &h) == AXL_ERR,
               "load_sibling: NULL file_name returns AXL_ERR");
    test_check(h == NULL,
               "load_sibling: rejected call clears *out_handle");
    test_check(axl_driver_load_sibling("doDriver.efi", NULL) == AXL_ERR,
               "load_sibling: NULL out_handle returns AXL_ERR");

    // Path-escape guard: a bare basename only — no separator or drive prefix.
    test_check(axl_driver_load_sibling("sub/doDriver.efi", &h) == AXL_INVALID,
               "load_sibling: '/' in name returns AXL_INVALID");
    test_check(axl_driver_load_sibling("sub\\doDriver.efi", &h) == AXL_INVALID,
               "load_sibling: backslash in name returns AXL_INVALID");
    test_check(axl_driver_load_sibling("fs0:doDriver.efi", &h) == AXL_INVALID,
               "load_sibling: ':' in name returns AXL_INVALID");

    // A well-formed bare name not present in the app's own directory:
    // resolution succeeds, the file lookup does not. (The test EFI's
    // directory has no 'definitely-not-here.efi'.)
    test_check(axl_driver_load_sibling("definitely-not-here.efi", &h) == AXL_NOT_FOUND,
               "load_sibling: well-formed missing sibling returns AXL_NOT_FOUND");
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

// ---------------------------------------------------------------------------
// AxlDriverInfo (P1) — the `drivers` view: enumerate loaded DriverBinding
// drivers with names, version, #managed, network classification. Read-only;
// exercised against the live OVMF/AAVMF driver set (dozens of DriverBinding
// drivers, including the firmware network stack).
// ---------------------------------------------------------------------------

static AxlDriverInfo g_drv_list[256];

static void
test_driver_info(void)
{
    /* NULL count is rejected. */
    test_check(axl_driver_list_loaded(NULL, 0, NULL) == AXL_ERR,
               "driver_info: list_loaded(NULL count) returns AXL_ERR");

    /* Count-only mode (out == NULL): the platform has DriverBinding drivers. */
    size_t total = 0;
    test_check(axl_driver_list_loaded(NULL, 0, &total) == AXL_OK,
               "driver_info: count-only returns AXL_OK");
    test_check(total > 0,
               "driver_info: at least one DriverBinding driver is loaded");

    /* Populate. */
    size_t n = 0;
    test_check(axl_driver_list_loaded(g_drv_list, 256, &n) == AXL_OK,
               "driver_info: populate returns AXL_OK");
    test_check(n == total,
               "driver_info: populated count matches the count-only total");

    size_t cap_written = (n < 256) ? n : 256;
    bool   any_name = false, any_handle = true, any_network = false;
    uint32_t total_managed = 0;
    for (size_t i = 0; i < cap_written; i++) {
        if (g_drv_list[i].name[0] != '\0') {
            any_name = true;
        }
        if (g_drv_list[i].handle == NULL) {
            any_handle = false;
        }
        if (g_drv_list[i].is_network) {
            any_network = true;
        }
        total_managed += g_drv_list[i].num_devices;
    }
    test_check(any_handle,
               "driver_info: every entry carries a DriverBinding handle");
    test_check(any_name,
               "driver_info: at least one driver has a ComponentName2 name");
    test_check(total_managed > 0,
               "driver_info: drivers manage at least one controller total");
    test_check(any_network,
               "driver_info: at least one network driver is classified");

    /* Truncation contract: cap smaller than the total still reports the full
       count and writes only `cap` entries. */
    size_t n2 = 0;
    test_check(axl_driver_list_loaded(g_drv_list, 1, &n2) == AXL_OK,
               "driver_info: truncated call returns AXL_OK");
    test_check(n2 == total,
               "driver_info: count is the full total even when cap < total");
}

// ---------------------------------------------------------------------------
// AxlDriverInfo (P1a/P2/P3/P5) — name a handle, PCI<->handle, bound query,
// targeted bind. Exercised against the live PCI topology.
// ---------------------------------------------------------------------------

static void
test_driver_discovery(void)
{
    char nbuf[128];

    /* P1a — handle name. */
    test_check(axl_handle_name(NULL, nbuf, sizeof(nbuf)) == AXL_ERR,
               "handle_name: NULL handle returns AXL_ERR");
    size_t ndrv = 0;
    axl_driver_list_loaded(g_drv_list, 256, &ndrv);
    bool named = false;
    for (size_t i = 0; i < ndrv && i < 256; i++) {
        if (axl_handle_name(g_drv_list[i].handle, nbuf, sizeof(nbuf)) == AXL_OK
            && nbuf[0] != '\0') {
            named = true;
            break;
        }
    }
    test_check(named, "handle_name: names a loaded driver handle");

    /* P2/P5 — a bogus PCI address resolves to nothing. */
    AxlPciAddr bogus = { .seg = 0xFFFF, .bus = 0xFF, .dev = 0x1F, .func = 7 };
    AxlHandle  bh    = (AxlHandle)(uintptr_t)0x1;
    test_check(axl_pci_to_handle(bogus, &bh) == AXL_ERR && bh == NULL,
               "pci_to_handle: bogus address returns AXL_ERR + clears out");
    test_check(axl_pci_to_handle(bogus, NULL) == AXL_ERR,
               "pci_to_handle: NULL out returns AXL_ERR");
    bool bd = true;
    test_check(axl_pci_driver_bound(bogus, &bd, NULL, 0) == AXL_ERR,
               "pci_driver_bound: bogus address returns AXL_ERR");
    test_check(axl_pci_driver_bound(bogus, NULL, NULL, 0) == AXL_ERR,
               "pci_driver_bound: NULL bound returns AXL_ERR");

    /* P3 — NULL controller guard. */
    test_check(axl_driver_bind(NULL, NULL) == AXL_ERR,
               "driver_bind: NULL controller returns AXL_ERR");

    /* P2/P5 — a real PCI function resolves to its controller handle. */
    AxlPciAddr *p    = NULL;
    AxlHandle   ctrl = NULL;
    AxlPciAddr  ctrl_addr = {0};
    while ((p = axl_pci_next(p)) != NULL) {
        if (axl_pci_to_handle(*p, &ctrl) == AXL_OK) {
            ctrl_addr = *p;
            break;
        }
    }
    test_check(ctrl != NULL,
               "pci_to_handle: a real PCI function resolves to a handle");
    if (ctrl != NULL) {
        /* P2 — the bound query succeeds. */
        bool bound = false;
        char dn[64];
        test_check(axl_pci_driver_bound(ctrl_addr, &bound, dn, sizeof(dn))
                       == AXL_OK,
                   "pci_driver_bound: real device query returns AXL_OK");

        /* P1a — the resolved controller also names (device-path fallback when
           it carries no ComponentName2). */
        test_check(axl_handle_name(ctrl, nbuf, sizeof(nbuf)) == AXL_OK
                       && nbuf[0] != '\0',
                   "handle_name: names a controller handle (device-path)");
    }

    /* P3 — find a PCI controller that IS bound (the PCI bus driver manages
       bridges, so at least one exists on both arches) and re-bind it:
       already-connected => verified-managed => AXL_OK. This exercises the
       success path without depending on connecting an unbound device. */
    AxlPciAddr *q = NULL;
    AxlHandle   bound_ctrl = NULL;
    while ((q = axl_pci_next(q)) != NULL) {
        AxlHandle h = NULL;
        bool      b = false;
        if (axl_pci_to_handle(*q, &h) == AXL_OK
            && axl_pci_driver_bound(*q, &b, NULL, 0) == AXL_OK && b) {
            bound_ctrl = h;
            break;
        }
    }
    test_check(bound_ctrl != NULL,
               "pci_driver_bound: at least one bound PCI controller exists");
    if (bound_ctrl != NULL) {
        test_check(axl_driver_bind(bound_ctrl, NULL) == AXL_OK,
                   "driver_bind: re-connect an already-bound controller is AXL_OK");
    }

    /* P4 — the network base-class constant, and raw SNP/NII handle
       enumeration by name (the name table now carries "nii"). Count may be 0
       on a NIC-less platform; the point is the name resolves (AXL_OK). */
    test_check(AXL_PCI_CLASS_NETWORK == 0x02,
               "p4: AXL_PCI_CLASS_NETWORK is the network base class");
    void  **snp_h = NULL;
    size_t  snp_n = 0;
    test_check(axl_protocol_enumerate("simple-network", &snp_h, &snp_n) == AXL_OK,
               "p4: enumerate(\"simple-network\") resolves the SNP name");
    axl_free(snp_h);
    void  **nii_h = NULL;
    size_t  nii_n = 0;
    test_check(axl_protocol_enumerate("nii", &nii_h, &nii_n) == AXL_OK,
               "p4: enumerate(\"nii\") resolves the NII name");
    axl_free(nii_h);
}

// ---------------------------------------------------------------------------
// AxlDriverInfo (P1b/P1c) — generic handle + protocol enumeration: the Devices
// tab and the per-NIC protocol-stack view. axl_handle_list,
// axl_handle_protocols, axl_net_protocol_name, axl_handle_drivers,
// axl_handle_children. Exercised against the live OVMF/AAVMF handle database
// (with the network stack connected — the unit boot runs connect -r + DHCP).
// ---------------------------------------------------------------------------

/* Canonical UEFI protocol GUIDs (the test owns these literals so it can both
   filter by them and round-trip them through axl_net_protocol_name). */
static const AxlGuid GUID_DEVICE_PATH =
    AXL_GUID(0x09576e91, 0x6d3f, 0x11d2,
             0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
static const AxlGuid GUID_SIMPLE_NETWORK =
    AXL_GUID(0xA19832B9, 0xAC25, 0x11D3,
             0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D);
static const AxlGuid GUID_IP4CONFIG2 =
    AXL_GUID(0x5b446ed1, 0xe30b, 0x4faa,
             0x87, 0x1a, 0x36, 0x54, 0xec, 0xa3, 0x60, 0x80);
/* A GUID no protocol uses — for the unknown-GUID name path. */
static const AxlGuid GUID_BOGUS =
    AXL_GUID(0xdeadbeef, 0x1234, 0x5678,
             0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x44);

static AxlHandle g_henum[512];
static AxlGuid   g_penum[64];

/* Append the recognized net-protocol names installed on @p h to @p line
   (comma-separated), so the diagnostic shows where each stack layer lives. */
static void
append_net_names(AxlHandle h, char *line, size_t cap)
{
    size_t np = 0;
    if (axl_handle_protocols(h, g_penum, 64, &np) != AXL_OK) {
        return;
    }
    for (size_t i = 0; i < np && i < 64; i++) {
        char nm[24];
        if (axl_net_protocol_name(&g_penum[i], nm, sizeof(nm)) != AXL_OK) {
            continue;
        }
        size_t used = axl_strlen(line);
        if (used > 0 && used + 1 < cap) {
            line[used++] = ',';
            line[used]   = '\0';
        }
        axl_strlcpy(line + axl_strlen(line), nm, cap - axl_strlen(line));
    }
}

/* Diagnostic: where does the IP4 networking stack land — on the SNP controller
   handle itself, or on its child handles? Determines whether the per-NIC stack
   view must walk children. Prints P1C_STACK lines so the answer is readable
   from the serial log. Not an assertion (the exact set is platform-dependent). */
static void
probe_net_stack_location(void)
{
    size_t nsnp = 0;
    if (axl_handle_list(&GUID_SIMPLE_NETWORK, g_henum, 512, &nsnp) != AXL_OK
        || nsnp == 0) {
        axl_printf("P1C_STACK: no SNP handle\n");
        return;
    }
    AxlHandle snp = g_henum[0];

    /* Net protocols directly on the SNP/NIC controller handle. */
    char on_ctrl[256] = "";
    append_net_names(snp, on_ctrl, sizeof(on_ctrl));

    /* Net protocols across the SNP handle's children. */
    AxlHandle kids[64];
    size_t    nkids = 0;
    char      on_kids[256] = "";
    axl_handle_children(snp, kids, 64, &nkids);
    for (size_t k = 0; k < nkids && k < 64; k++) {
        append_net_names(kids[k], on_kids, sizeof(on_kids));
    }

    axl_printf("P1C_STACK: snp_handles=%zu nchildren=%zu\n", nsnp, nkids);
    axl_printf("P1C_STACK: on_controller=[%s]\n", on_ctrl);
    axl_printf("P1C_STACK: on_children=[%s]\n", on_kids);
}

static void
test_handle_enum(void)
{
    // --- axl_handle_list ----------------------------------------------------
    test_check(axl_handle_list(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_list: NULL count returns AXL_ERR");

    size_t all = 0;
    test_check(axl_handle_list(NULL, NULL, 0, &all) == AXL_OK,
               "handle_list: count-only (all handles) returns AXL_OK");
    test_check(all > 0, "handle_list: the handle database is non-empty");

    size_t n = 0;
    test_check(axl_handle_list(NULL, g_henum, 512, &n) == AXL_OK && n == all,
               "handle_list: populate count matches count-only total");
    bool all_nonnull = true;
    for (size_t i = 0; i < n && i < 512; i++) {
        if (g_henum[i] == NULL) {
            all_nonnull = false;
        }
    }
    test_check(all_nonnull, "handle_list: every enumerated handle is non-NULL");

    size_t n1 = 0;
    test_check(axl_handle_list(NULL, g_henum, 1, &n1) == AXL_OK && n1 == all,
               "handle_list: count is full total even when cap < total");

    size_t ndp = 0;
    test_check(axl_handle_list(&GUID_DEVICE_PATH, NULL, 0, &ndp) == AXL_OK,
               "handle_list: by-GUID (DevicePath) count returns AXL_OK");
    test_check(ndp > 0 && ndp <= all,
               "handle_list: DevicePath subset is non-empty and <= all handles");

    // --- axl_handle_protocols ----------------------------------------------
    test_check(axl_handle_protocols(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_protocols: NULL handle returns AXL_ERR");

    size_t ndp_h = 0;
    axl_handle_list(&GUID_DEVICE_PATH, g_henum, 512, &ndp_h);
    test_check(ndp_h > 0, "handle_protocols: have a DevicePath handle to inspect");
    if (ndp_h > 0) {
        size_t np = 0;
        test_check(axl_handle_protocols(g_henum[0], NULL, 0, &np) == AXL_OK
                       && np > 0,
                   "handle_protocols: count-only returns AXL_OK with >0 protocols");
        size_t np2 = 0;
        test_check(axl_handle_protocols(g_henum[0], g_penum, 64, &np2) == AXL_OK
                       && np2 == np,
                   "handle_protocols: populate count matches count-only total");
        bool has_dp = false;
        for (size_t i = 0; i < np2 && i < 64; i++) {
            if (axl_guid_cmp(&g_penum[i], &GUID_DEVICE_PATH)) {
                has_dp = true;
            }
        }
        test_check(has_dp,
                   "handle_protocols: a DevicePath handle lists DevicePath (reverse-consistent)");
    }

    // --- axl_net_protocol_name (exact strings) -----------------------------
    char nm[32];
    test_check(axl_net_protocol_name(NULL, nm, sizeof(nm)) == AXL_ERR,
               "net_protocol_name: NULL guid returns AXL_ERR");
    test_check(axl_net_protocol_name(&GUID_SIMPLE_NETWORK, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "SimpleNetwork") == 0,
               "net_protocol_name: SNP GUID -> \"SimpleNetwork\"");
    test_check(axl_net_protocol_name(&GUID_IP4CONFIG2, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "Ip4Config2") == 0,
               "net_protocol_name: Ip4Config2 GUID -> \"Ip4Config2\"");
    test_check(axl_net_protocol_name(&GUID_DEVICE_PATH, nm, sizeof(nm)) == AXL_ERR,
               "net_protocol_name: non-net GUID (DevicePath) returns AXL_ERR");

    // --- axl_handle_drivers -------------------------------------------------
    test_check(axl_handle_drivers(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_drivers: NULL controller returns AXL_ERR");

    /* A bound PCI controller (the PCI bus driver manages bridges on both
       arches) has at least one managing driver. */
    AxlPciAddr *q          = NULL;
    AxlHandle   bound_ctrl = NULL;
    while ((q = axl_pci_next(q)) != NULL) {
        AxlHandle h = NULL;
        bool      b = false;
        if (axl_pci_to_handle(*q, &h) == AXL_OK
            && axl_pci_driver_bound(*q, &b, NULL, 0) == AXL_OK && b) {
            bound_ctrl = h;
            break;
        }
    }
    test_check(bound_ctrl != NULL, "handle_drivers: a bound PCI controller exists");
    if (bound_ctrl != NULL) {
        size_t nd = 0;
        test_check(axl_handle_drivers(bound_ctrl, NULL, 0, &nd) == AXL_OK && nd >= 1,
                   "handle_drivers: a bound controller has >= 1 managing driver");
        AxlHandle drv[8] = { NULL };
        size_t    nd2    = 0;
        test_check(axl_handle_drivers(bound_ctrl, drv, 8, &nd2) == AXL_OK && nd2 == nd,
                   "handle_drivers: populate count matches count-only total");
        test_check(nd2 == 0 || drv[0] != NULL,
                   "handle_drivers: managing driver handle is non-NULL");
    }

    // --- axl_handle_children ------------------------------------------------
    test_check(axl_handle_children(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_children: NULL controller returns AXL_ERR");

    /* Some controller has children (a PCI root bridge / bus always produces
       child controllers). Scan for the first one. */
    size_t    nall          = 0;
    AxlHandle parent         = NULL;
    size_t    parent_nkids   = 0;
    axl_handle_list(NULL, g_henum, 512, &nall);
    size_t scan = (nall < 512) ? nall : 512;
    for (size_t i = 0; i < scan; i++) {
        size_t nk = 0;
        if (axl_handle_children(g_henum[i], NULL, 0, &nk) == AXL_OK && nk > 0) {
            parent       = g_henum[i];
            parent_nkids = nk;
            break;
        }
    }
    test_check(parent != NULL && parent_nkids > 0,
               "handle_children: a controller with child controllers exists");
    if (parent != NULL) {
        AxlHandle kids[64] = { NULL };
        size_t    nk2      = 0;
        test_check(axl_handle_children(parent, kids, 64, &nk2) == AXL_OK,
                   "handle_children: populate returns AXL_OK");
        bool kids_nonnull = (nk2 > 0);
        for (size_t i = 0; i < nk2 && i < 64; i++) {
            if (kids[i] == NULL) {
                kids_nonnull = false;
            }
        }
        test_check(kids_nonnull, "handle_children: every child handle is non-NULL");
    }

    // --- axl_protocol_guid_name (general; exact strings) -------------------
    test_check(axl_protocol_guid_name(NULL, nm, sizeof(nm)) == AXL_ERR,
               "protocol_guid_name: NULL guid returns AXL_ERR");
    /* Delegates to the net table first — a net GUID still resolves. */
    test_check(axl_protocol_guid_name(&GUID_SIMPLE_NETWORK, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "SimpleNetwork") == 0,
               "protocol_guid_name: net GUID resolves via the general entry");
    /* A non-net GUID the net-only function rejects now resolves. */
    test_check(axl_net_protocol_name(&GUID_DEVICE_PATH, nm, sizeof(nm)) == AXL_ERR,
               "protocol_guid_name: DevicePath is non-net (net_protocol_name rejects)");
    test_check(axl_protocol_guid_name(&GUID_DEVICE_PATH, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "DevicePath") == 0,
               "protocol_guid_name: DevicePath GUID -> \"DevicePath\"");
    test_check(axl_protocol_guid_name(&GUID_BOGUS, nm, sizeof(nm)) == AXL_ERR,
               "protocol_guid_name: unknown GUID returns AXL_ERR");

    // --- axl_handle_parents -------------------------------------------------
    test_check(axl_handle_parents(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_parents: NULL controller returns AXL_ERR");
    /* Round-trip vs axl_handle_children: a child of @p parent must report
       @p parent among its parents. Reuses the controller-with-children found
       above. */
    if (parent != NULL) {
        AxlHandle kids[64] = { NULL };
        size_t    nk       = 0;
        if (axl_handle_children(parent, kids, 64, &nk) == AXL_OK && nk > 0) {
            AxlHandle parents[16] = { NULL };
            size_t    npar        = 0;
            test_check(axl_handle_parents(kids[0], parents, 16, &npar) == AXL_OK
                           && npar >= 1,
                       "handle_parents: a child controller has >= 1 parent");
            bool found = false;
            for (size_t i = 0; i < npar && i < 16; i++) {
                if (parents[i] == parent) {
                    found = true;
                }
            }
            test_check(found,
                       "handle_parents: child's parents include the original parent (round-trip)");
        }
    }

    /* Diagnostic for the handoff question (not an assertion): where the IP4
       stack lands relative to the SNP handle. */
    probe_net_stack_location();
}

int
test_driver_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_print_header("AxlDriver");

    test_install_fresh_handle();
    test_install_null_safety();
    test_buffer_identity_and_sibling();
    test_driver_binding();
    test_disconnect_handle_contract();
    test_driver_info();
    test_driver_discovery();
    test_handle_enum();

    return test_print_results();
}

AXL_APP(test_driver_main)
