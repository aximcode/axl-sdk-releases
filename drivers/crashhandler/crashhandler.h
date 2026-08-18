/** @file crashhandler.h
    Internal header for CrashHandler DXE driver.
**/

#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#include <uefi/axl-uefi.h>
#include <axl.h>
#include <axl/axl-crashrecord.h>

/* Global state -- set during entry, used by exception handler */
extern AxlHandle               g_image_handle;

/* Pre-allocated image table. Refreshed at TPL_CALLBACK whenever an image
   loads (see g_image_watch), never from exception context -- rebuilding it
   there would allocate. */
extern AxlCrashImageEntry         g_image_table[AXL_CRASH_MAX_IMAGES];
extern uint32_t                g_image_count;

/* Loaded-image watch keeping g_image_table current. NULL if the firmware
   refused the registration, in which case the table is still the init-time
   snapshot and a crash in a later-loaded image goes unattributed. */
extern AxlImageWatch          *g_image_watch;

/* GUIDs (binary-compatible with EFI_GUID; both halves of the
   driver use AxlGuid so no consumer code needs <uefi/...>). */
extern AxlGuid g_crash_handler_variable_guid;
extern AxlGuid g_crash_handler_protocol_guid;

/* -----------------------------------------------------------------------
   exception.c
   ----------------------------------------------------------------------- */

/**
 * CPU exception handler -- saves crash record to NVRAM.
 * Called by axl-sdk's typed CPU dispatcher when a registered
 * exception fires. Matches AxlCpuExceptionFn signature.
 */
void
crash_exception_handler(
    const AxlCpuException *exc,
    void                  *user
);

/**
 * Snapshot the loaded image table into g_image_table/g_image_count.
 * Called during driver init so the data is available during a crash
 * without needing heap allocation.
 */
void
snapshot_loaded_images(void);

/**
 * Re-snapshot the loaded image table. Matches AxlImageLoadFn, and is
 * registered with axl_image_watch_loads so the firmware calls it at
 * TPL_CALLBACK whenever an image loads.
 */
void
refresh_loaded_images(
    void *ctx
) AXL_CB_NOEXCEPT;

/**
 * Human-readable name for an AxlCpuExceptionKind ("#UD (Invalid Opcode)").
 *
 * ONE definition, used by both halves of the driver: the live handler that
 * prints at fault time and the reboot path that renders a persisted record.
 * They used to carry a switch each, over two DIFFERENT numberings, so a
 * crash printed as "#UD" came back from NVRAM as "#BR".
 */
const char *
crash_exception_name(
    AxlCpuExceptionKind kind
);

/* -----------------------------------------------------------------------
   report.c
   ----------------------------------------------------------------------- */

/**
 * Check NVRAM for crash records and write crash-report.txt to the ESP.
 * Deletes processed NVRAM variables.
 *
 * @return Number of crash records processed.
 */
size_t
process_crash_records(void);

#endif /* CRASH_HANDLER_H */
