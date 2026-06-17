/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-shell.h
    Locate and launch the real UEFI Shell as a foreground child.

    Shell-specific policy over the generic foreground launcher
    @ref axl_image_run (`<axl/axl-image.h>`, the "run any blocking UEFI app
    and get its exit code" mechanism): find a `Shell.efi` across the
    conventional locations and run it with `-nostartup` (so a child Shell
    launched *from* `startup.nsh` doesn't re-run `startup.nsh` and recurse).
    For any *other* blocking app — a diagnostic tool, a vendor setup app, a
    recovery menu — call @ref axl_image_run directly with its path.

    Companion to @ref AxlConsoleMirror (the mirror wraps the console so a
    remote terminal can drive whatever runs in the foreground); this puts
    the real Shell there. `StartImage` blocks until the Shell exits.

    @code
    // Host a real Shell while a background HTTP server keeps serving,
    // pumped off a firmware timer (the resident-driver model):
    axl_loop_attach_driver(loop, 10);   // network pumped in the background
    int exit_code = 0;
    axl_shell_launch(&exit_code);        // blocks until the Shell exits
    axl_loop_detach_driver(loop);
    @endcode
**/

#ifndef AXL_SHELL_H
#define AXL_SHELL_H

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Locate a real `Shell.efi` and run it in the foreground.
 *
 * Searches for `Shell.efi` using @ref axl_driver_locate (the running
 * image's own directory, its `drivers/<arch>/`, and other mounted
 * volumes' `drivers/<arch>/`). When a consumer deploys the Shell to a
 * non-standard location (e.g. an ESP layout like `\\x64\\Shell.efi`),
 * call @ref axl_image_run with the explicit path and `"-nostartup"`.
 *
 * The Shell is started with the `-nostartup` load option so that, when
 * the launcher itself was started from `startup.nsh`, the child Shell
 * does not re-run `startup.nsh` and recurse. `StartImage` **blocks**:
 * this call returns only when the Shell exits.
 *
 * @return AXL_OK if a Shell was found, started, and has now exited
 *     (its exit code is reported in @p out_exit_code); AXL_ERR if no
 *     `Shell.efi` could be located or loaded.
 */
int
axl_shell_launch(
    int *out_exit_code  ///< [out] Shell's exit code (NULL allowed)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SHELL_H */
