/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-runtime.c
    AXL runtime glue: _axl_init / _axl_cleanup (called by CRT0), plus
    the singleton default loop and cooperative yield.

    Later commits in the Phase A7 series add:
      - tier-1 resource registry init/sweep (src/runtime/axl-registry.c)
      - atexit callback registry (src/runtime/axl-atexit.c)
      - break-event notify + axl_signal_install (src/runtime/axl-signal.c)
**/

#include "../backend/axl-backend.h"
#include "../posix/axl-app-internal.h"
#include "axl-atexit-internal.h"
#include "axl-cxxabi-internal.h"
#include "axl-registry-internal.h"
#include "axl-signal-internal.h"
#include <axl/axl-runtime.h>
#include <axl/axl-signal.h>
#include <axl/axl-loop.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-stream.h>

AXL_LOG_DOMAIN("runtime");

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

/* g_axl_interrupted now lives in src/runtime/axl-signal.c. */

static AxlLoop *mDefaultLoop;
static bool     mCleanupRan;

// ---------------------------------------------------------------------------
// Default loop
// ---------------------------------------------------------------------------

AxlLoop *
axl_loop_default(void)
{
    if (mDefaultLoop == NULL) {
        mDefaultLoop = axl_loop_new();
    }
    return mDefaultLoop;
}

// ---------------------------------------------------------------------------
// Yield
// ---------------------------------------------------------------------------

void
_axl_poll_break(void)
{
    /* Observe Ctrl-C without touching the loop. The yield-only app (no
     * event sources, no loops) and all in-library yields go through here:
     * poll the shell break flag directly. */
    if (axl_backend_shell_break_flag()) {
        _axl_signal_on_break();
    }

    /* Default-policy exit: if Ctrl-C fired and no user handler is
     * installed, the blessed behavior is to terminate cleanly at the next
     * yield point. Apps that want to handle interrupts themselves must
     * axl_signal_install before the first yield. */
    if (g_axl_interrupted && !_axl_signal_has_handler()) {
        axl_exit(1);
    }
}

void
axl_yield(void)
{
    AxlLoop *loop = mDefaultLoop;

    /* No default loop yet: break-poll + exit-policy only (a yield-only app
     * would otherwise never see Ctrl-C). Identical to what library code does
     * via _axl_poll_break. */
    if (loop == NULL) {
        _axl_poll_break();
        return;
    }

    /* Default loop is live — the app opted into the yield-as-scheduler idiom
     * (docs/AXL-Lifecycle.md §2.4/§3): dispatch any immediately-ready work on
     * it, one non-blocking iteration. axl_loop_dispatch checks the break flag
     * itself (we skip the direct poll to avoid double-clearing the UEFI break
     * event), returning < 0 on break.
     *
     * NOTE: in-library code must NOT reach this branch — it calls
     * _axl_poll_break so it never re-dispatches the consumer's loop (firing
     * their callbacks re-entrantly) from deep inside an unrelated operation. */
    if (axl_loop_dispatch(loop, /*blocking=*/false) < 0) {
        _axl_signal_on_break();
    }
    if (g_axl_interrupted && !_axl_signal_has_handler()) {
        axl_exit(1);
    }
}

// ---------------------------------------------------------------------------
// Registry inspection
// ---------------------------------------------------------------------------

size_t
axl_registry_count(void)
{
    return _axl_registry_size();
}

// ---------------------------------------------------------------------------
// EFI System Configuration Table accessor
// ---------------------------------------------------------------------------

void *
axl_efi_find_config_table(
    const AxlGuid *guid
    )
{
    if (guid == NULL) {
        return NULL;
    }
    /* AxlGuid is layout-compatible with EFI_GUID — see comment on
       AxlGuid in axl-sys.h. The cast lets the public API stay
       UEFI-type-free while the implementation uses the EDK2 types
       it has to walk. */
    const EFI_GUID *efi_guid = (const EFI_GUID *)guid;
    for (size_t i = 0; i < axl_st()->NumberOfTableEntries; i++) {
        if (axl_guid_equal(&axl_st()->ConfigurationTable[i].VendorGuid, efi_guid)) {
            return axl_st()->ConfigurationTable[i].VendorTable;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// CRT0 entry points
// ---------------------------------------------------------------------------

void
_axl_init(void *image_handle, void *system_table)
{
    /* Set firmware globals before anything else — backend functions
     * depend on gST / gBS / gRT / gImageHandle being live. */
    gImageHandle = (EFI_HANDLE)image_handle;
    gST = (EFI_SYSTEM_TABLE *)system_table;
    gBS = gST->BootServices;
    gRT = gST->RuntimeServices;

    axl_stream_init();
    _axl_registry_init();
    _axl_atexit_init();
    _axl_args_init(image_handle);

    /* C++ global constructors fire last, after the rest of the
     * runtime is up so ctors may use axl_printf / axl_malloc / etc.
     * No-op for pure-C apps (empty .init_array). */
    _axl_cxxabi_run_init_array();
}

void
_axl_cleanup(void)
{
    /* Guard against double-run: axl_exit calls _axl_cleanup before
     * gBS->Exit, and CRT0 calls it after main returns. If axl_exit
     * fires, the firmware exit never returns and CRT0 never sees
     * main return — but if it ever did (paranoia), we must not
     * run cleanup twice. */
    if (mCleanupRan) {
        return;
    }
    mCleanupRan = true;

    /* atexit callbacks fire first — they may free resources that
     * would otherwise be caught (noisily) by the sweep. */
    _axl_atexit_run_all();

    _axl_args_free();

    /* Explicitly free the default loop (if any) before sweep so its
     * registry entry unregisters cleanly; otherwise it'd appear as
     * a "leak" on every run. */
    if (mDefaultLoop != NULL) {
        axl_loop_free(mDefaultLoop);
        mDefaultLoop = NULL;
    }

    _axl_registry_sweep();

#ifdef AXL_MEM_DEBUG
    axl_mem_dump_leaks();
#endif
}
