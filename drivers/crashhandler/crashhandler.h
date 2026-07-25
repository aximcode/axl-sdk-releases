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

/* Pre-allocated image table (filled during init, used during crash) */
extern AxlCrashImageEntry         g_image_table[AXL_CRASH_MAX_IMAGES];
extern uint32_t                g_image_count;

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
