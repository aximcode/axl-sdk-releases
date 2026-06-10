/** @file entrypoint.c
    CrashHandler DXE driver entry point.

    On load:
    1. Check if already active (idempotent -- safe to load multiple times)
    2. Process any crash records from NVRAM (write crash-report.txt)
    3. Snapshot loaded images (pre-allocate for use during crash)
    4. Register exception handlers

    Build: axl-cc --type driver entrypoint.c exception.c report.c -I. -o CrashHandler.efi
**/

#include "crashhandler.h"

/* GUIDs */
AxlGuid g_crash_handler_variable_guid = CRASH_HANDLER_VARIABLE_GUID;
AxlGuid g_crash_handler_protocol_guid = CRASH_HANDLER_PROTOCOL_GUID;

/* Global state */
AxlHandle               g_image_handle = NULL;
CrashImageEntry         g_image_table[CRASH_MAX_IMAGES];
uint32_t                g_image_count  = 0;

/* Sentinel protocol instance (value doesn't matter -- existence is the signal) */
static uint8_t sentinel = 0;

/* Exception kinds we monitor. Per-arch lists since x64 splits
   what aa64 collapses into the SYNCHRONOUS umbrella. */
#if defined(__x86_64__)
static const AxlCpuExceptionKind exception_kinds[] = {
    AXL_CPU_EXCEPTION_DIVIDE_ERROR,
    AXL_CPU_EXCEPTION_DEBUG,
    AXL_CPU_EXCEPTION_OVERFLOW,
    AXL_CPU_EXCEPTION_BOUND,
    AXL_CPU_EXCEPTION_INVALID_OPCODE,
    AXL_CPU_EXCEPTION_DEVICE_NA,
    AXL_CPU_EXCEPTION_DOUBLE_FAULT,
    AXL_CPU_EXCEPTION_SEGMENT_NP,
    AXL_CPU_EXCEPTION_STACK_FAULT,
    AXL_CPU_EXCEPTION_GP_FAULT,
    AXL_CPU_EXCEPTION_PAGE_FAULT,
    AXL_CPU_EXCEPTION_FP_ERROR,
    AXL_CPU_EXCEPTION_ALIGNMENT_CHECK,
    AXL_CPU_EXCEPTION_SIMD,
};
#elif defined(__aarch64__)
static const AxlCpuExceptionKind exception_kinds[] = {
    AXL_CPU_EXCEPTION_SYNCHRONOUS,
    AXL_CPU_EXCEPTION_SERROR,
};
#endif

static const size_t exception_count = AXL_ARRAY_SIZE(exception_kinds);

/**
 * Driver entry point.
 *
 * AXL_DRIVER wires this into the firmware DriverEntry shim; the
 * macro at the bottom of the file emits the EFIAPI entry stub and
 * calls axl_driver_init for us.
 */
static int
crash_handler_main(AxlHandle image, AxlSystemTable *st)
{
    (void)st;
    void   *existing;
    size_t  crash_count;
    void   *sentinel_handle = NULL;

    g_image_handle = image;

    /* 1. Bind the sentinel protocol name to its vendor GUID. The
       binding is per-image so we re-register on every load; the
       call is idempotent if another image in the same address
       space already bound it. */
    if (axl_protocol_register_name(CRASH_HANDLER_PROTOCOL_NAME,
                                   &g_crash_handler_protocol_guid)
        != AXL_OK) {
        axl_printf("CrashHandler: protocol-name bind failed\n");
        return AXL_ERR;
    }

    /* 2. Idempotency -- check if we're already loaded */
    if (axl_protocol_find(CRASH_HANDLER_PROTOCOL_NAME, &existing) == AXL_OK) {
        axl_printf("CrashHandler: already active\n");
        return AXL_ERR;
    }

    /* 2. Register the "crashdump" nvstore namespace so report.c +
       exception.c can address slots by simple UTF-8 keys instead
       of repeating the vendor GUID at every call site. */
    if (axl_nvstore_register_namespace(
            "crashdump", &g_crash_handler_variable_guid) != AXL_OK) {
        axl_printf("CrashHandler: nvstore namespace register failed\n");
        return AXL_ERR;
    }

    /* 3. Process any crash records from previous session */
    crash_count = process_crash_records();
    if (crash_count > 0) {
        axl_printf("CrashHandler: %zu crash record(s) saved to crash-report.txt\n",
                   crash_count);
    }

    /* 4. Snapshot loaded images (pre-allocate for crash handler) */
    snapshot_loaded_images();

    /* 5. Register exception handlers via axl-sdk's typed CPU API.
       Each registration is independent — kinds that aren't
       available on this arch return AXL_ERR but don't abort the
       loop; firmware quirks that refuse a specific registration
       (rare) are logged and skipped. */
    size_t registered = 0;
    for (size_t i = 0; i < exception_count; i++) {
        if (axl_cpu_register_exception(exception_kinds[i],
                                       crash_exception_handler,
                                       NULL) == AXL_OK) {
            registered++;
        }
    }
    if (registered == 0) {
        axl_printf("CrashHandler: no exception kinds could be monitored\n");
        return AXL_ERR;
    }

    /* 6. Install sentinel protocol on a new handle */
    axl_protocol_register(CRASH_HANDLER_PROTOCOL_NAME,
                          &sentinel,
                          &sentinel_handle);

    axl_printf("CrashHandler: active (%zu exception types monitored)\n",
               exception_count);

    return AXL_OK;
}

/**
 * UnloadImage handler. Exception registrations are leaked
 * intentionally: the CPU arch protocol's RegisterInterruptHandler
 * has no public unregister-by-callback API symmetric to the
 * registration, and the driver is one-shot per boot (idempotency
 * check in main() refuses a second instance). We rely on firmware
 * tearing down per-image state at UnloadImage time.
 */
static int
crash_handler_unload(AxlHandle image)
{
    (void)image;
    return AXL_OK;
}

AXL_DRIVER(crash_handler_main, crash_handler_unload)
