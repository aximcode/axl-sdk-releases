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
AxlGuid g_crash_handler_variable_guid = AXL_CRASH_HANDLER_VARIABLE_GUID;
AxlGuid g_crash_handler_protocol_guid = AXL_CRASH_HANDLER_PROTOCOL_GUID;

/* Global state */
AxlHandle               g_image_handle = NULL;
AxlCrashImageEntry         g_image_table[AXL_CRASH_MAX_IMAGES];
uint32_t                g_image_count  = 0;
AxlImageWatch          *g_image_watch  = NULL;

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
    if (axl_protocol_register_name(AXL_CRASH_HANDLER_PROTOCOL_NAME,
                                   &g_crash_handler_protocol_guid)
        != AXL_OK) {
        axl_printf("CrashHandler: protocol-name bind failed\n");
        return AXL_ERR;
    }

    /* 2. Idempotency -- check if we're already loaded */
    if (axl_protocol_find(AXL_CRASH_HANDLER_PROTOCOL_NAME, &existing) == AXL_OK) {
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

    /* 4. Snapshot loaded images (pre-allocate for crash handler), and keep
       that snapshot CURRENT. Taking it once here is not enough and used to
       be all this did: the driver loads before the application that goes on
       to fault, so the faulting image was never in the table, the report
       carried no base to rebase against, and rsod-decode had nothing to
       resolve. The watch fires at TPL_CALLBACK, where re-enumerating may
       allocate; exception context, where it may not, only ever READS. */
    snapshot_loaded_images();
    g_image_watch = axl_image_watch_loads(refresh_loaded_images, NULL);
    if (g_image_watch == NULL) {
        /* Not fatal -- a stale table still names the images present at load
           time. Say so rather than degrade silently, since the symptom is a
           report that looks complete and attributes nothing. */
        axl_printf("CrashHandler: image-load watch unavailable; "
                   "crash reports may not name the faulting image\n");
    }

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
        /* Release the watch on the way out. A DriverEntry that returns an
           error is torn down by the core WITHOUT the Unload handler running,
           so leaving it registered points a live firmware notify at code
           about to be freed -- it would fire on the next image load. */
        axl_image_unwatch_loads(g_image_watch);
        g_image_watch = NULL;
        return AXL_ERR;
    }

    /* 6. Install sentinel protocol on a new handle */
    axl_protocol_register(AXL_CRASH_HANDLER_PROTOCOL_NAME,
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
    /* The image-load watch IS releasable, unlike the exception
       registrations above, and must be released: its notify would
       otherwise be invoked into unloaded code the next time any image
       loads. */
    axl_image_unwatch_loads(g_image_watch);
    g_image_watch = NULL;
    return AXL_OK;
}

AXL_DRIVER(crash_handler_main, crash_handler_unload)
