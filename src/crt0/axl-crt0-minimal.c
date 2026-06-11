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
 *   - Initialize the stdout/stderr file handles via axl_stream_init so
 *     axl_printf / axl_error work as expected.
 *   - Parse argv from the UEFI shell's image load options via
 *     _axl_args_init / _axl_get_args, so `int main(argc, argv)`
 *     still receives arguments.
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
void _axl_args_init(void *image_handle);
void _axl_get_args(int *argc, char ***argv);
void _axl_args_free(void);

void axl_stream_init(void);

/* Resolve the exit status for a main() return code: a pending
 * axl_set_exit_status wins verbatim, else rc maps 0 -> EFI_SUCCESS /
 * nonzero -> EFI_ABORTED. Defined in the backend (libaxl.a); UINTN-width.
 * The minimal CRT0 returns from main rather than calling axl_exit (which is
 * unsound here — _axl_init never ran, so the cleanup registries are absent),
 * so the armed status must be honored on THIS return path. */
unsigned long long axl_backend_resolve_exit_status(int rc);

#ifdef AXL_MEM_DEBUG
void axl_mem_dump_leaks(void);
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

    axl_stream_init();
    _axl_args_init(ImageHandle);

    int argc;
    char **argv;
    _axl_get_args(&argc, &argv);

    int rc = main(argc, argv);

    _axl_args_free();

#ifdef AXL_MEM_DEBUG
    axl_mem_dump_leaks();
#endif

    /* A pending axl_set_exit_status (if any) overrides the rc->status map, so
     * a `return N` from a minimal-runtime main yields an exact EFI_STATUS —
     * symmetric to axl-crt0-native.c. */
    return (EFI_STATUS)axl_backend_resolve_exit_status(rc);
}
