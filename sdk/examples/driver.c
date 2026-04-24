/**
 * driver.c — example DXE driver showcasing AXL SDK features.
 *
 * Demonstrates:
 *   - axl_driver_init()       — one-call runtime setup
 *   - axl_driver_set_unload() — register cleanup callback
 *   - axl_printf()            — console output (UTF-8)
 *   - AXL_LOG_DOMAIN / axl_info — structured logging
 *   - AXL_AUTO_FREE / AXL_AUTOPTR — RAII auto-cleanup
 *   - axl_malloc / axl_free   — heap allocation with leak tracking
 *   - AxlString               — auto-growing string builder
 *   - AXL_ARRAY_SIZE           — static array element count
 *   - gBS / gST / gImageHandle — firmware globals (no redeclaration)
 *   - axl_mem_dump_leaks()    — debug leak report on unload
 *
 * Build:  axl-cc --type driver driver.c -o driver.efi
 * Load:   load driver.efi
 * Unload: unload -n driver.efi
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

AXL_LOG_DOMAIN("example-drv");

// ---------------------------------------------------------------------------
// Protocol definition
// ---------------------------------------------------------------------------

static EFI_GUID gExampleProtocolGuid = {
    0x12345678, 0xabcd, 0xef01,
    { 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01 }
};

typedef struct {
    UINT32      Version;
    const char *Description;
} EXAMPLE_PROTOCOL;

static EXAMPLE_PROTOCOL mProtocol;
static EFI_HANDLE       mHandle;

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
// Unload callback — registered via axl_driver_set_unload
// ---------------------------------------------------------------------------

static
EFI_STATUS
EFIAPI
DriverUnload(
    EFI_HANDLE ImageHandle
    )
{
    (void)ImageHandle;

    if (mHandle != NULL) {
        gBS->UninstallProtocolInterface(
            mHandle,
            &gExampleProtocolGuid,
            &mProtocol);
    }

    axl_free((void *)mProtocol.Description);

    axl_info("unloaded — checking for leaks");
    axl_mem_dump_leaks();
    axl_printf("PASS: driver-unload\n");
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
DriverEntry(
    EFI_HANDLE        ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
    )
{
    EFI_STATUS status;

    // One call sets up gST/gBS/gRT/gImageHandle + I/O streams
    axl_driver_init(ImageHandle, SystemTable);

    // Register unload callback so firmware calls us on 'unload'
    axl_driver_set_unload(DriverUnload);

    // --- Structured logging ---
    axl_info("loading (image=%p)", (void *)gImageHandle);

    // --- Heap allocation with RAII ---
    AXL_AUTO_FREE char *buf = axl_malloc(64);
    if (buf == NULL) {
        axl_printf("FAIL: driver-malloc\n");
        return EFI_OUT_OF_RESOURCES;
    }
    axl_printf("PASS: driver-malloc\n");

    // --- String builder with RAII ---
    AXL_AUTOPTR(AxlString) desc = axl_string_new("AXL example driver (features:");
    for (size_t i = 0; i < AXL_ARRAY_SIZE(features); i++) {
        axl_string_append_printf(desc, " %s", features[i]);
    }
    axl_string_append(desc, ")");

    mProtocol.Version     = 1;
    mProtocol.Description = axl_string_steal(desc);

    axl_printf("PASS: driver-string-builder\n");

    // --- Install protocol ---
    mHandle = NULL;
    status = gBS->InstallProtocolInterface(
        &mHandle,
        &gExampleProtocolGuid,
        EFI_NATIVE_INTERFACE,
        &mProtocol);

    if (EFI_ERROR(status)) {
        axl_printf("FAIL: driver-install (0x%llx)\n",
                   (unsigned long long)status);
        return status;
    }

    axl_printf("PASS: driver-install\n");
    axl_info("ready — %s", mProtocol.Description);

    return EFI_SUCCESS;
}
