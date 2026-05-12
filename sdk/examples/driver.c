/**
 * driver.c — example DXE driver showcasing AXL SDK features.
 *
 * Demonstrates how a DXE driver can be written in pure AXL types,
 * with NO `EFI_*` / `EFIAPI` boilerplate visible at the source
 * level. The `AXL_DRIVER` macro emits the firmware-side
 * `DriverEntry` + `Unload` stubs and wires `axl_driver_init` /
 * `axl_driver_set_unload` automatically; the consumer's entry and
 * unload functions take `AxlHandle` parameters and return `int`.
 *
 * Demonstrated:
 *   - AXL_DRIVER()             — DriverEntry/Unload adapter macro
 *   - axl_protocol_register_name() — pin a custom protocol name
 *                                to a published vendor GUID
 *   - axl_protocol_register() / _find() / _unregister() — installed
 *                                interface lifecycle bookkeeping
 *   - axl_printf()             — console output (UTF-8)
 *   - AXL_LOG_DOMAIN / axl_info — structured logging
 *   - AXL_AUTO_FREE / AXL_AUTOPTR — RAII auto-cleanup
 *   - axl_malloc / axl_free    — heap allocation with leak tracking
 *   - AxlString                — auto-growing string builder
 *   - AXL_ARRAY_SIZE           — static array element count
 *   - axl_mem_dump_leaks()     — debug leak report on unload
 *
 * Build:  axl-cc --type driver driver.c -o driver.efi
 * Load:   load driver.efi
 * Unload: unload -n driver.efi
 */

#include <axl.h>

AXL_LOG_DOMAIN("example-drv");

// ---------------------------------------------------------------------------
// Protocol definition — AxlGuid + a plain consumer-defined struct.
// ---------------------------------------------------------------------------

static const AxlGuid gExampleGuid =
    AXL_GUID(0x12345678, 0xabcd, 0xef01,
             0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01);

typedef struct {
    uint32_t    version;
    const char *description;
} ExampleProtocol;

static ExampleProtocol  mProtocol;
static AxlHandle        mHandle;

// ---------------------------------------------------------------------------
// Feature table — demonstrates AXL_ARRAY_SIZE
// ---------------------------------------------------------------------------

static const char *features[] = {
    "printf",
    "logging",
    "malloc",
    "string-builder",
    "leak-tracking",
};

// ---------------------------------------------------------------------------
// Forward declarations the AXL_DRIVER macro expects.
// ---------------------------------------------------------------------------

static int driver_main(AxlHandle image, AxlSystemTable *st);
static int driver_unload(AxlHandle image);

AXL_DRIVER(driver_main, driver_unload)

// ---------------------------------------------------------------------------
// DriverEntry body
// ---------------------------------------------------------------------------

static int
driver_main(
    AxlHandle        image,
    AxlSystemTable  *st
    )
{
    (void)image;
    (void)st;

    // --- Structured logging ---
    axl_info("loading");

    // --- Heap allocation with RAII ---
    AXL_AUTO_FREE char *buf = axl_malloc(64);
    if (buf == NULL) {
        axl_printf("FAIL: driver-malloc\n");
        return AXL_ERR;
    }
    axl_printf("PASS: driver-malloc\n");

    // --- String builder with RAII ---
    AXL_AUTOPTR(AxlString) desc = axl_string_new("AXL example driver (features:");
    for (size_t i = 0; i < AXL_ARRAY_SIZE(features); i++) {
        axl_string_append_printf(desc, " %s", features[i]);
    }
    axl_string_append(desc, ")");

    mProtocol.version     = 1;
    mProtocol.description = axl_string_steal(desc);

    axl_printf("PASS: driver-string-builder\n");

    // --- Pin a stable GUID to a custom protocol name. Without this
    //     call axl_protocol_register("example-svc", ...) would synthesize
    //     a name-derived GUID; pinning lets external consumers
    //     LocateProtocol against the same gExampleGuid the driver
    //     publishes.
    if (axl_protocol_register_name("example-svc", &gExampleGuid) != AXL_OK) {
        axl_printf("FAIL: driver-protocol-pin\n");
        goto fail;
    }
    axl_printf("PASS: driver-protocol-pin\n");

    // --- Install the protocol via the AXL protocol registry. The
    //     name lookup goes name → pinned GUID → InstallProtocolInterface,
    //     so the firmware-side GUID matches gExampleGuid.
    mHandle = NULL;
    if (axl_protocol_register("example-svc", &mProtocol, &mHandle) != AXL_OK
        || mHandle == NULL) {
        axl_printf("FAIL: driver-install\n");
        goto fail;
    }
    axl_printf("PASS: driver-install\n");

    // --- Self-discover via the same name to verify the round-trip.
    void *found = NULL;
    if (axl_protocol_find("example-svc", &found) != AXL_OK
        || found != &mProtocol) {
        axl_printf("FAIL: driver-protocol-find\n");
        // Roll back the install we just made — failed DriverEntry
        // means firmware will NOT call the unload callback, so we
        // own teardown of every visible side effect.
        axl_protocol_unregister(mHandle, "example-svc", &mProtocol);
        mHandle = NULL;
        goto fail;
    }
    axl_printf("PASS: driver-protocol-find\n");

    axl_info("ready — %s", mProtocol.description);

    return AXL_OK;

fail:
    // Firmware does not call the unload callback when the entry
    // returns non-zero, so any allocation made above this point
    // would leak. Free the description string the AxlString
    // builder produced; AXL_AUTO_FREE buf and AXL_AUTOPTR desc
    // are already handled by the cleanup attribute.
    axl_free((void *)mProtocol.description);
    mProtocol.description = NULL;
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// DriverUnload body
//
// Registered protocols are NOT auto-released — the AXL protocol
// registry never owned the install. Walk our protocols and call
// axl_protocol_unregister for each one before returning.
// ---------------------------------------------------------------------------

static int
driver_unload(
    AxlHandle  image
    )
{
    (void)image;

    if (mHandle != NULL) {
        axl_protocol_unregister(mHandle, "example-svc", &mProtocol);
    }

    axl_free((void *)mProtocol.description);

    axl_info("unloaded — checking for leaks");
    axl_mem_dump_leaks();
    axl_printf("PASS: driver-unload\n");
    return AXL_OK;
}
