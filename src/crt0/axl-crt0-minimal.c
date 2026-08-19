/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-crt0-minimal.c — Native UEFI entry point that skips the
 * AXL runtime.
 *
 * Peer to axl-crt0-native.c. Apps link this variant via
 * `axl-cc --minimal-runtime` when they want to opt out of the
 * runtime services (tier-1 resource registry, atexit list,
 * shell-break signal notify, default loop) that the standard CRT0
 * would otherwise wire up by calling _axl_init. What this CRT0
 * *does* still do:
 *
 *   - Set the firmware globals (gST, gBS, gRT, gImageHandle) so
 *     any AXL call the app does make works.
 *   - Honour a pending axl_set_exit_status on the way out.
 *
 * And NOTHING ELSE by default. stdio and argv used to be unconditional
 * here, which is why the flag measured a 785-byte saving against the full
 * runtime: it skipped the registry/atexit/signal/loop and kept the stream
 * layer, and the stream layer is what reaches the console, the event backend
 * and the printf engine -- the bulk of the floor. Both are opt-in now:
 *
 *     --minimal-runtime            nothing (stdio still self-initialises if
 *                                  the app references it -- see below)
 *     --minimal-runtime=stdio      force the stream layer in
 *     --minimal-runtime=args       give main() its argc/argv
 *     --minimal-runtime=stdio,args the previous behaviour, spelled out
 *   - On AXL_MEM_DEBUG builds, dump the allocation leak report on
 *     return so size-constrained or custom-runtime apps still get
 *     the same debug-time feedback.
 *
 * What it does NOT do (compared to axl-crt0-native.c):
 *
 *   - _axl_registry_init — registry stays NULL, _axl_registry_add
 *     no-ops, no exit-time sweep. Apps that leak an AxlEvent /
 *     AxlLoop / AxlCancellable / AxlArena will leak quietly.
 *   - _axl_atexit_init — axl_atexit returns 0, no callbacks fire.
 *   - Shell-break notify install — axl_interrupted() always reports
 *     false; axl_signal_install is a no-op; axl_yield can still poll
 *     the break flag directly via the backend helper but will not
 *     auto-exit (because it already special-cases "no default loop"
 *     and falls back to direct-flag polling — apps that want
 *     auto-exit on Ctrl-C should NOT opt out).
 *   - axl_loop_default() creation — the lazy allocator still works
 *     on first call, so apps that explicitly ask for the default
 *     loop get one; it just isn't pre-created.
 *
 * Drivers do NOT use this CRT0. They provide their own entry point
 * and don't call _axl_init — same as under the full runtime.
 *
 * Part of the AximCode AXL SDK.
 */

#include <uefi/axl-uefi.h>

/* Hand-declare the two runtime hooks we need. We deliberately do
 * not call _axl_init / _axl_cleanup — that's the whole point. */
/* ALL WEAK. --minimal-runtime means MINIMAL: firmware globals, main, exit
 * status. Anything else is opted back in, and the opt-in is a LINK decision
 * rather than a code path -- axl-cc turns `--minimal-runtime=args` into
 * `-u _axl_args_init`, which pulls the archive member and resolves these.
 *
 * Weak also makes stdio self-correcting, which is the property worth having:
 * an app that calls axl_printf pulls axl-stream.o (it defines axl_print AND
 * axl_stream_init), so the reference that needs the runtime is the one that
 * supplies it. An app that never prints leaves the whole console/format chain
 * unlinked. And an app that somehow reaches axl_print with the layer absent
 * gets -1, not a fault: axl_print NULL-checks axl_stdout.
 *
 * argv has no such self-correction: reading `argv` in `main` emits no symbol
 * at all -- it arrives in registers -- so nothing about the app's own code
 * can pull _axl_args_init the way calling axl_printf pulls the stream layer.
 * Unresolved, the weak reference below is NULL, the call is skipped and `main`
 * receives argc=0/argv=NULL. That is why it is an explicit feature rather
 * than automatic.
 *
 * BEWARE: on today's tree that failure does not actually occur, and the
 * reason is an accident worth writing down rather than relying on.
 * axl-cxxrt-stubs.o is on EVERY link (the porting layer travels with the C
 * library), it strongly references axl_file_info, and that drags
 * axl-fs.o -> axl-driver.o -> axl-app.o -- which is where _axl_args_init
 * lives. So the weak reference binds, and a bare --minimal-runtime image gets
 * argv through an edge that has nothing to do with argv. Measured: an app
 * returning argc as its exit status answers 3 for two arguments.
 *
 * An earlier version of this comment claimed "nothing but this CRT0
 * references _axl_args_init", which was already false -- axl-runtime.o
 * carries a strong reference, and axl-path.o/axl-driver.o strongly reference
 * _axl_app_image_anchor from the same member. Do not restore that claim; it
 * is the kind that reads true and links false.
 *
 * Because the guarantee is a coincidence, axl-cc REFUSES a --minimal-runtime
 * link whose `main` is declared with parameters unless the caller says `args`
 * or `noargs` -- main's arity read from DWARF, since nm cannot see it. Same
 * doctrine as the log engine's log/nolog: an answer the driver will not
 * assume on your behalf. */
void _axl_args_init(void *image_handle) __attribute__((weak));
void _axl_get_args(int *argc, char ***argv) __attribute__((weak));
void _axl_args_free(void) __attribute__((weak));

/* WEAK, and this is what finally makes --minimal-runtime mean something.
 *
 * A strong call here pulled axl-stream.o into EVERY minimal-runtime image, and
 * axl-stream.o reaches the console, which reaches the event backend and the
 * printf engine -- the bulk of the ~47 KB floor. That is why the flag measured
 * a 785-byte saving against the full runtime: it skipped the registry, atexit,
 * signal and loop, and kept the expensive part.
 *
 * Weak makes the decision follow the LINK instead of the flag, and it is
 * self-correcting in both directions:
 *
 *   app calls axl_printf  -> axl-stream.o is linked (it defines axl_print AND
 *                            axl_stream_init) -> this resolves -> init runs
 *   app never prints      -> axl-stream.o is never pulled -> this is 0 -> the
 *                            whole console/format chain is absent
 *
 * So an app cannot end up printing through an uninitialised stream: the very
 * reference that would need init is the one that supplies it. And an app that
 * does reach axl_print without the layer linked gets -1, not a fault --
 * axl_print NULL-checks axl_stdout (src/stream/axl-stream.c).
 *
 * The FULL runtime (axl-crt0-native.c) keeps its strong call deliberately:
 * _axl_init promises a wired-up runtime, and an app on that path expects
 * axl_printf to work whether or not the linker agrees. */
void axl_stream_init(void) __attribute__((weak));

/* Resolve the exit status for a main() return code: a pending
 * axl_set_exit_status wins verbatim, else rc maps 0 -> EFI_SUCCESS /
 * nonzero -> EFI_ABORTED. Defined in the backend (libaxl.a); UINTN-width.
 * The minimal CRT0 returns from main rather than calling axl_exit (which is
 * unsound here — _axl_init never ran, so the cleanup registries are absent),
 * so the armed status must be honored on THIS return path. */
unsigned long long axl_backend_resolve_exit_status(int rc);

#ifdef AXL_MEM_DEBUG
/* Declared in src/mem/axl-mem-internal.h; hand-declared here for the same
 * reason as the hooks above — this stub links against libaxl.a but does not
 * pull in the runtime's internal headers. */
void _axl_mem_dump_leaks_at_exit(void);

/* NOTE: the report is emitted with axl_warning (src/mem/axl-mem.c), and the
 * log engine is opt-in at link time — so a --minimal-runtime image linked
 * against an AXL_MEM_DEBUG libaxl runs the report and prints NOTHING unless
 * the link asked for the engine ($(LOG_ENGINE_PULL) / `--minimal-runtime=log`;
 * see src/log/axl-log-dispatch.h).
 *
 * That is left to the link rather than forced here on purpose. Forcing it
 * would make --minimal-runtime behave differently in DEBUG and RELEASE, and
 * would mean the in-tree DEBUG suite never exercised the engine-less path at
 * all — which is the path with the weak indirect call in it. An axl-cc build
 * is unaffected either way: --debug changes only the caller's own flags, and
 * install.sh stages a RELEASE libaxl where AXL_MEM_DEBUG is off. */
#endif

/* User's application entry point. */
int main(int argc, char **argv);

EFI_STATUS
EFIAPI
_AxlEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    gImageHandle = ImageHandle;
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gRT = SystemTable->RuntimeServices;

    if (axl_stream_init != NULL) {
        axl_stream_init();
    }

    int    argc = 0;
    char **argv = NULL;
    if (_axl_args_init != NULL && _axl_get_args != NULL) {
        _axl_args_init(ImageHandle);
        _axl_get_args(&argc, &argv);
    }

    int rc = main(argc, argv);

    if (_axl_args_free != NULL) {
        _axl_args_free();
    }

#ifdef AXL_MEM_DEBUG
    _axl_mem_dump_leaks_at_exit();
#endif

    /* A pending axl_set_exit_status (if any) overrides the rc->status map, so
     * a `return N` from a minimal-runtime main yields an exact EFI_STATUS —
     * symmetric to axl-crt0-native.c. */
    return (EFI_STATUS)axl_backend_resolve_exit_status(rc);
}
