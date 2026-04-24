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
#include "axl-registry-internal.h"
#include "axl-signal-internal.h"
#include <axl/axl-runtime.h>
#include <axl/axl-signal.h>
#include <axl/axl-loop.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-io.h>

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
axl_yield(void)
{
    AxlLoop *loop = mDefaultLoop;

    /* Phase 1: break detection. Poll the shell break flag directly
     * when no default loop exists yet — otherwise a yield-only app
     * (no event sources, no loops) would never see Ctrl-C. When the
     * default loop IS live, axl_loop_dispatch below handles the
     * check as part of its normal flow; we skip the direct poll to
     * avoid double-clearing the UEFI break event. */
    if (loop == NULL) {
        if (axl_backend_shell_break_flag()) {
            _axl_signal_on_break();
        }
    } else {
        /* Phase 2 (see docs/AXL-Runtime.md §3): dispatch any
         * immediately-ready work on the default loop. One
         * iteration only. */
        int r = axl_loop_dispatch(loop, /*blocking=*/false);
        if (r < 0) {
            _axl_signal_on_break();
        }
    }

    /* Default-policy exit: if Ctrl-C fired and no user handler is
     * installed, the blessed behavior is to terminate cleanly at
     * the next yield point. Apps that want to handle interrupts
     * themselves must axl_signal_install before the first yield. */
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
// CRT0 entry points
// ---------------------------------------------------------------------------

void
_axl_init(void *image_handle, void *system_table)
{
    /* Set firmware globals before anything else — backend functions
     * depend on gST / gBS / gRT / gImageHandle being live. */
    extern EFI_HANDLE gImageHandle;
    gImageHandle = (EFI_HANDLE)image_handle;
    gST = (EFI_SYSTEM_TABLE *)system_table;
    gBS = gST->BootServices;
    gRT = gST->RuntimeServices;

    axl_io_init();
    _axl_registry_init();
    _axl_atexit_init();
    _axl_args_init(image_handle);
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
