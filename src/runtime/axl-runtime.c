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
#include "../mem/axl-mem-internal.h"
#include "../posix/axl-app-internal.h"
#include "axl-atexit-internal.h"
#include "axl-cxxabi-internal.h"
#include "axl-libc-heap-internal.h"
#include "axl-registry-internal.h"
#include "axl-runtime-internal.h"
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

/* The default loop is the ONLY thing in the always-linked runtime that would
 * otherwise drag the entire event-loop subsystem (axl_loop_dispatch / _free
 * and their event-backend tail, ~8 KB .text + 6 KB .bss) into every image —
 * even a trivial app that never creates a loop — because axl_yield() and
 * _axl_cleanup() statically reference those functions behind a runtime
 * `mDefaultLoop != NULL` guard the linker can't evaluate. Route those two
 * calls through function pointers that are populated ONLY when the default
 * loop is actually created. An app that never calls axl_loop_default() leaves
 * them NULL, so the only static reference to axl_loop_dispatch/_free lives in
 * axl_loop_default() itself, and --gc-sections drops the whole subsystem. */
static int  (*mLoopDispatchFn)(AxlLoop *loop, bool blocking);
static void (*mLoopFreeFn)(AxlLoop *loop);

// ---------------------------------------------------------------------------
// Default loop
// ---------------------------------------------------------------------------

AxlLoop *
axl_loop_default(void)
{
    if (mDefaultLoop == NULL) {
        mDefaultLoop    = axl_loop_new();
        /* Arm the indirect hooks now the subsystem is in use anyway. */
        mLoopDispatchFn = axl_loop_dispatch;
        mLoopFreeFn     = axl_loop_free;
    }
    return mDefaultLoop;
}

void
_axl_default_loop_free(void)
{
    if (mDefaultLoop != NULL && mLoopFreeFn != NULL) {
        mLoopFreeFn(mDefaultLoop);
        mDefaultLoop = NULL;
    }
}

// ---------------------------------------------------------------------------
// Yield
// ---------------------------------------------------------------------------

/* Depth of _axl_poll_break_hold(): while above zero an observed Ctrl-C is
   recorded but does not end the program. */
static unsigned mBreakHold;

void
_axl_poll_break_hold(void)
{
    mBreakHold++;
}

void
_axl_poll_break_release(void)
{
    if (mBreakHold > 0) {
        mBreakHold--;
    }
}

void
_axl_poll_break(void)
{
    /* Not in a driver image. Resident code does not own the Shell's break:
     * reading the flag CONSUMES it, so the Shell's script runner (which polls
     * the same event between lines) would never see the Ctrl-C meant for it;
     * and the exit below cannot work for an image that is not the current
     * one -- gBS->Exit fails and axl_exit spins, wedging the machine at
     * whatever TPL the driver was running. Accepted cost: a driver blocked in
     * library work cannot be aborted with Ctrl-C. */
    if (_axl_atexit_is_driver_image()) {
        return;
    }

    /* Observe Ctrl-C without touching the loop. The yield-only app (no
     * event sources, no loops) and all in-library yields go through here:
     * poll the shell break flag directly. */
    if (axl_backend_shell_break_flag()) {
        _axl_signal_on_break();
    }

    /* Default-policy exit: if Ctrl-C fired and no user handler is
     * installed, the blessed behavior is to terminate cleanly at the next
     * yield point. Apps that want to handle interrupts themselves must
     * axl_signal_install before the first yield. Not while a hold is
     * taken: the caller is inside a sequence that must be undone or
     * finished first, and leaves the interrupt pending for the next yield
     * after it releases the hold. */
    if (g_axl_interrupted && !_axl_signal_has_handler() && mBreakHold == 0) {
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
    /* Indirect call (see mLoopDispatchFn): a live mDefaultLoop implies
     * axl_loop_default() ran, which armed the hook. */
    if (mLoopDispatchFn != NULL && mLoopDispatchFn(loop, /*blocking=*/false) < 0) {
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
        if (axl_efi_guid_equal(&axl_st()->ConfigurationTable[i].VendorGuid, efi_guid)) {
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

/* Set by _axl_cleanup_note_exit; read by _axl_cleanup and
   _axl_cleanup_pin_exit_status. */
static bool     mExitUnloadsDriver;
static uint64_t mExitStatus;
static bool     mExitNoted;

void
_axl_cleanup_note_exit(
    int rc
    )
{
    /* LATCHED: the first exit decides. A second one can only come from
       inside the teardown that first exit started -- a destructor reaching
       std::terminate in the atexit drain calls axl_exit(1) -- and by then the
       image is half torn down; re-deciding on its non-error code would send
       it back to the firmware RESIDENT (test-heap-release-qemu.sh). */
    if (mExitNoted) {
        return;
    }
    mExitNoted = true;

    /* EFI_ERROR: the status word's top bit, on both arches. A non-error
       status -- 0, or the small POSIX code a bare nonzero rc maps to, which
       is what std::terminate's _exit(1) reaches -- leaves a driver RESIDENT
       (EDK2 unloads only on an error), so it keeps everything.

       No child running: while an image this one started is running (the
       entry inside axl_image_run, a driver start, a Shell command), the
       CHILD is the firmware's current image, and an exit from one of this
       image's callbacks makes gBS->Exit FAIL and spin -- that exit must not
       tear the image down. A raised TPL is NOT that case and once stood in
       for it, keeping the heap of an entry that exits from a raise it never
       restored (test-heap-release-qemu.sh, tplexit). */
    mExitStatus        = axl_backend_resolve_exit_status(rc);
    mExitUnloadsDriver = _axl_atexit_driver_entry_live()
                         && !axl_backend_child_images_running()
                         && (mExitStatus >> 63) != 0;
}

void
_axl_cleanup_finish_exit(
    void
    )
{
    if (_axl_atexit_is_driver_image() && !mExitUnloadsDriver) {
        return;     /* a driver staying resident keeps everything */
    }

    /* The heap, again -- because THIS call may be a nested exit. An
       axl_exit from inside the atexit drain (a destructor reaching
       std::terminate) never returns to the _axl_cleanup that drain belongs
       to, so that cleanup's own last step never runs and the arena would go
       with the image unreleased. Idempotent: on the ordinary path
       _axl_cleanup released it already and this does nothing. */
    _axl_libc_heap_release();

    /* The teardown just taken was decided on mExitStatus; an atexit handler
       or destructor that armed another status in it must not reach
       gBS->Exit. A success there would leave the driver RESIDENT, with its
       destructors run and its heap released. */
    if (mExitUnloadsDriver) {
        axl_backend_set_exit_status(mExitStatus);
    }
}

unsigned
axl_tpl_current(void)
{
    return (unsigned)axl_backend_tpl_current();
}

unsigned
axl_tpl_raise(
    unsigned  level
    )
{
    return (unsigned)axl_backend_tpl_raise((uintptr_t)level);
}

void
axl_tpl_restore(
    unsigned  previous
    )
{
    axl_backend_tpl_restore((uintptr_t)previous);
}

bool
axl_tpl_restore_baseline(
    unsigned  *out_leaked
    )
{
    unsigned  now = (unsigned)axl_backend_tpl_current();

    if (now <= AXL_TPL_APPLICATION) {
        return false;
    }

    /* Repair BEFORE reporting. Above TPL_NOTIFY the log path itself
       would hang (it allocates and writes the console), so a diagnostic
       emitted from inside the leak would be the last thing that ever
       ran. */
    axl_backend_tpl_restore((uintptr_t)AXL_TPL_APPLICATION);

    if (out_leaked != NULL) {
        *out_leaked = now;
    }
    return true;
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

    /* FIRST, before any console write, allocation or callback below.
     * An image that returns above TPL_APPLICATION wedges the machine —
     * measured at TPL_CALLBACK as well as TPL_NOTIFY, so every raised
     * level is fatal. AArch64 firmware names it (`ASSERT Image->Tpl ==
     * gEfiCurrentTpl`) and deadloops; an x64 release build says nothing
     * and simply spins. Repairing here turns an unrecoverable hang into
     * a logged defect, and it must precede the rest of cleanup because
     * at TPL_HIGH_LEVEL the logging itself would hang. */
    if (axl_tpl_restore_baseline(NULL)) {
        /* Reported through the raw console writer, NOT axl_error. The
         * repair itself is 158 bytes; an axl_error here cost a further
         * 550-1060 in every C-only image, because it drags the whole
         * formatting path into apps that never log otherwise (measured:
         * hexdump +710, grep +1222, while apps already logging paid
         * +198). The Ctrl-C notice below already links this writer, so
         * the diagnostic is now close to free -- and the point of it is
         * that a wedge becomes a NAMED failure, which one fixed line
         * does as well as a formatted one. The level is spelled out by
         * hand for the same reason. */
        axl_backend_console_write_err(
            (const unsigned short *)
            L"\r\nAXL: exited at a raised TPL -- a raise was never "
            L"restored. Restored it; the machine stays usable.\r\n");
    }

    /* Universal Ctrl-C notice: if the run was interrupted and the app did not
     * install its own handler (which owns its messaging), announce the break on
     * every exit path — the default-policy axl_exit AND the loop-returns-then-
     * main-returns path both funnel through here. stderr so a piped stdout stays
     * clean. Printed before atexit/leak output so it leads the shutdown. */
    if (g_axl_interrupted && !_axl_signal_has_handler()) {
        axl_backend_console_write_err(
            (const unsigned short *)L"\r\nInterrupted (Ctrl-C)\r\n");
    }

    /* Guarantee the shell's page break is off on every exit path. The
     * universal `-b` handler (axl_args_run) enables it and clears it on
     * normal return, but a handler that ends via axl_exit() bypasses that
     * return — without this, an enabled page break would leak into the
     * next shell command (a later script would then hang on -- More --).
     * No-op when it was never enabled / no shell. */
    axl_backend_console_set_page_break(false);

    /* atexit callbacks fire first — they may free resources that
     * would otherwise be caught (noisily) by the sweep.
     *
     * NOT in a driver image, where that table belongs to
     * axl_driver_cleanup. This path is still reachable from a driver: its
     * own code can call axl_exit(), and axl_yield exits on a break latched
     * by a loop the driver opted in to intercepting. (_axl_poll_break, under
     * ordinary library work, no longer leads here from a driver image.) The
     * drain was a guaranteed no-op there until axl_driver_init began
     * populating the table; left unguarded it would run every global
     * destructor, after which gBS->Exit FAILS for a non-current image and
     * spins — leaving a live driver on destructed state. The spin predates
     * this; destroying the image's C++ state on the way into it would not. */
    /* ...UNLESS this exit unloads the driver image: inside an AXL macro's
     * DriverEntry, with an EFI error (_axl_cleanup_note_exit). Then the
     * firmware unloads it through CoreUnloadAndCloseImage, which never calls
     * its Unload, so nothing else will ever run its destructors -- the same
     * teardown the macro's own error return does through
     * axl_driver_cleanup. */
    bool own_teardown = !_axl_atexit_is_driver_image() || mExitUnloadsDriver;

    if (own_teardown) {
        _axl_atexit_run_all();
    }

    _axl_args_free();

    /* Explicitly free the default loop (if any) before sweep so its
     * registry entry unregisters cleanly; otherwise it'd appear as
     * a "leak" on every run. */
    _axl_default_loop_free();

    _axl_registry_sweep();

#ifdef AXL_MEM_DEBUG
    _axl_mem_dump_leaks_at_exit();
#endif

    /* LAST: newlib's arena back to the firmware, which does not reclaim it
     * at unload. Everything above may still malloc or free -- atexit
     * handlers, C++ destructors, a destroy callback the sweep runs -- and
     * nothing below this line may. See axl-libc-heap-internal.h.
     *
     * NOT in a driver image, for the reason the atexit drain above is
     * skipped there: a driver that reaches this through axl_exit(0) in its
     * entry is then Exit()ed with EFI_SUCCESS and STAYS RESIDENT, still
     * using its heap -- releasing it here made that driver's later free()
     * fault on the pages EDK2 had filled with 0xAF (test-heap-release-qemu).
     * A driver's heap goes with axl_driver_cleanup, on its real unload --
     * or here, when this exit is what unloads it (own_teardown, above). */
    if (own_teardown) {
        _axl_libc_heap_release();
    }
}
