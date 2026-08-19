/**
 * binding-driver.c — canonical UEFI Driver Model (Type-B) driver in AXL.
 *
 * A Type-B driver publishes EFI_DRIVER_BINDING_PROTOCOL and is bound to
 * controllers by the firmware's ConnectController (Supported -> Start ->
 * Stop). That is what a NIC, storage, or bus driver is. The fiddly EFI
 * mechanics — the EFIAPI thunks, the OpenProtocol(BY_DRIVER) / CloseProtocol
 * ownership bookkeeping, building and installing the binding +
 * EFI_COMPONENT_NAME2_PROTOCOL — are what `AxlDriverBinding` manages for you.
 * You write three callbacks in pure AXL C (`AxlHandle`, no `EFI_HANDLE`, no
 * EFIAPI, no OpenProtocol dance). This whole file includes NO <uefi/...>
 * header.
 *
 * A real Type-B driver binds to devices the firmware already enumerated (a
 * PCI function, a USB interface). To keep this example self-contained and
 * runnable on any machine — no specific emulated device required — it plays
 * BOTH halves of the relationship:
 *
 *   1. the "device": a synthetic `WidgetController` protocol published on a
 *      fresh handle, standing in for a controller the firmware enumerated;
 *   2. the driver: an `AxlDriverBinding` that manages WidgetController
 *      controllers.
 *
 * In production these two halves live in separate images and the firmware's
 * ConnectController drives the binding. Here the entry point drives it
 * itself (axl_driver_connect_handle / axl_driver_disconnect_handle) so simply
 * loading the driver walks the full Supported -> Start -> Stop lifecycle and
 * prints what happened. The binding stays resident afterward, so the shell's
 * `connect` / `disconnect` can re-drive it against the synthetic controller.
 *
 * Build:  axl-cc --type driver binding-driver.c -o binding-driver.efi
 *         (in-tree: `make binding-driver`)
 * Load:   load binding-driver.efi
 * Drivers listing (shows the Component Name 2 name):  drivers
 * Re-drive: connect <controller-handle> / disconnect <controller-handle>
 * Unload: find the image handle in `drivers` (or `dh -p LoadedImage`), then
 *         `unload <handle>` — the shell's unload takes a handle, not a name.
 */

#include <axl.h>

AXL_LOG_DOMAIN("binding-drv");

// ---------------------------------------------------------------------------
// The synthetic "widget bus" controller protocol.
//
// This is an ordinary author-defined protocol: an AxlGuid plus a plain
// struct. It stands in for whatever real protocol a controller exposes (e.g.
// EFI_PCI_IO_PROTOCOL). The binding below opens it BY_DRIVER to claim the
// controller; `start` receives this struct as its bound `iface`.
// ---------------------------------------------------------------------------

static const AxlGuid gWidgetGuid =
    AXL_GUID(0x7a9d1c30, 0x4e62, 0x4b18,
             0x9f, 0x2a, 0x10, 0x33, 0x55, 0x77, 0x99, 0xbb);

typedef struct {
    uint32_t    revision;
    const char *model;
} WidgetController;

static WidgetController  mWidget = { .revision = 2, .model = "AXL-Widget-9000" };
static AxlHandle         mController;   // the synthetic controller handle

// ---------------------------------------------------------------------------
// The three binding callbacks — pure AXL C, no EFI types.
// ---------------------------------------------------------------------------

// Optional gate, called during Supported AFTER AXL has confirmed `binds` is
// present on the controller and openable BY_DRIVER. Return true to manage it.
//
// Keep it side-effect-free: the firmware runs Supported as a pure query
// against many controllers. Real drivers filter here (by device class, vendor
// ID, etc.). Note the gate gets only the controller handle, not the bound
// interface — AXL hands that to `start`; to inspect the device in Supported a
// driver opens its protocol GET_PROTOCOL itself. This example manages any
// widget, so it just reports that it was consulted.
static bool
widget_supported(AxlHandle controller, void *ctx)
{
    (void)controller;
    (void)ctx;
    axl_info("Supported: a widget controller is present and bindable");
    return true;
}

// Start managing the controller. AXL has already opened `binds` BY_DRIVER
// (claiming ownership) and hands us the bound interface as `iface` — the one
// unavoidable raw touch is casting it to the real protocol type; operating it
// is the driver's job.
static int
widget_start(AxlHandle controller, void *iface, void *ctx)
{
    (void)ctx;
    const WidgetController *w = iface;   // the bound protocol interface
    axl_info("Start: bound controller %p - model=%s rev=%u",
             controller, w->model, (unsigned)w->revision);
    // A real driver would initialise the device and publish child protocols
    // here. Return AXL_OK to keep the binding; any other value makes AXL roll
    // back the open (CloseProtocol) and report failure to ConnectController.
    return AXL_OK;
}

// Stop managing the controller (DisconnectController / driver unload). Tear
// down what `start` built; AXL closes `binds` afterward.
static int
widget_stop(AxlHandle controller, void *ctx)
{
    (void)ctx;
    axl_info("Stop: released controller %p", controller);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Forward declarations the AXL_DRIVER macro expects.
// ---------------------------------------------------------------------------

static int binding_main(AxlHandle image, AxlSystemTable *st);
static int binding_unload(AxlHandle image);

AXL_DRIVER(binding_main, binding_unload)

// ---------------------------------------------------------------------------
// DriverEntry body
// ---------------------------------------------------------------------------

static int
binding_main(
    AxlHandle        image,
    AxlSystemTable  *st
    )
{
    (void)image;
    (void)st;

    // 1. Install the Driver Model binding on our own image handle. AXL builds
    //    EFI_DRIVER_BINDING_PROTOCOL + EFI_COMPONENT_NAME2_PROTOCOL with the
    //    managed thunks and copies the descriptor (so this stack value is
    //    fine; `name` and `binds` are borrowed and must outlive the driver —
    //    here they are static/literal). binding_unload uninstalls it
    //    explicitly -- the axl_atexit safety net does now run on driver
    //    unload, but only an explicit uninstall can disconnect first and
    //    report a failure.
    AxlDriverBinding db = {
        .name      = "AXL Widget Driver",
        .binds     = &gWidgetGuid,
        .supported = widget_supported,
        .start     = widget_start,
        .stop      = widget_stop,
        .ctx       = NULL,
    };
    if (axl_driver_binding_install(&db) != AXL_OK) {
        axl_error("axl_driver_binding_install failed");
        return AXL_ERR;
    }
    axl_info("binding installed (shell `drivers` lists \"%s\")", db.name);

    // 2. Synthesise a controller for the binding to manage: publish the
    //    widget protocol on a fresh handle. In a real driver the firmware
    //    would already have enumerated this controller (a PCI/USB device).
    mController = NULL;
    if (axl_protocol_install(&gWidgetGuid, &mWidget, &mController) != AXL_OK) {
        axl_error("failed to publish synthetic widget controller");
        // Entry returns non-zero -> the firmware unloads us via
        // CoreUnloadAndCloseImage, which does NOT call our unload. AXL_DRIVER
        // runs axl_driver_cleanup() on this path, which drains axl_atexit and
        // so removes the binding through its safety-net hook. Nothing to do
        // here -- but note the hook cannot disconnect a bound controller, so
        // a driver that may have connected one must unwind before returning.
        return AXL_ERR;
    }

    // 3. Drive the lifecycle the way the firmware would: connect (Supported
    //    -> Start), then disconnect (Stop). axl_driver_connect_handle and its
    //    symmetric axl_driver_disconnect_handle are the AXL wrappers over the
    //    firmware's Connect/DisconnectController.
    axl_info("--- connecting controller (firmware would do this) ---");
    if (axl_driver_connect_handle(mController) != AXL_OK) {
        axl_error("connect failed");
    }
    axl_info("--- disconnecting controller ---");
    if (axl_driver_disconnect_handle(mController) != AXL_OK) {
        axl_error("disconnect failed");
    }

    axl_info("ready - binding resident; `connect`/`disconnect` the "
             "controller to re-drive it");
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// DriverUnload body
//
// Driver unload DOES drain axl_atexit now (axl_driver_cleanup), so the
// binding's safety-net hook would eventually remove it -- but a Type-B driver
// uninstalls explicitly here anyway, because the hook runs after this callback
// and cannot disconnect a still-bound controller or report the failure.
// Leaving it installed would dangle EFI_DRIVER_BINDING_PROTOCOL on the freed
// image handle and crash the next `connect`/`drivers`. Order matters:
// disconnect the controller first
// (so the binding's Stop releases its BY_DRIVER open), then uninstall the
// binding (now unreferenced), then tear down the synthetic controller we own.
// ---------------------------------------------------------------------------

static int
binding_unload(
    AxlHandle  image
    )
{
    (void)image;

    // Release the controller if it is still bound (the firmware's post-load
    // connect re-binds it; the shell may also have). Harmless (AXL_OK) if not.
    if (mController != NULL) {
        axl_driver_disconnect_handle(mController);
    }

    if (axl_driver_binding_uninstall() != AXL_OK) {
        axl_error("axl_driver_binding_uninstall failed");
    }

    if (mController != NULL) {
        axl_protocol_uninstall(mController, &gWidgetGuid, &mWidget);
        mController = NULL;
    }

    axl_info("unloaded - checking for leaks");
    axl_mem_dump_leaks();
    return AXL_OK;
}
