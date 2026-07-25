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
#include <stdbool.h>
#include <stdint.h>

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

/**
 * @brief Launch the firmware-embedded UEFI Shell out of a Firmware Volume.
 *
 * The no-file-staged counterpart of @ref axl_shell_launch(): rather than
 * locating a `Shell.efi` file, it finds the platform's built-in UEFI Shell
 * — the EDK2 ShellPkg application, FILE_GUID
 * `EA4BB293-2D7F-4456-A681-1F22F42CD0BC` — in a readable Firmware Volume and
 * runs it in the foreground via @ref axl_image_run_fv_file. `StartImage`
 * **blocks**: this returns only when the Shell exits. Use it when a consumer
 * runs from the host firmware's own shell (e.g. mounted virtual media) with
 * no `Shell.efi` staged, or on a vendor firmware that embeds the Shell.
 *
 * Same `-nostartup` rationale as @ref axl_shell_launch applies; pass that
 * (or any Shell command line) via @p load_options, NULL for none. The
 * launched instance is a **fresh** Shell, not the parent the consumer may
 * itself have been started from (that parent is blocked in `StartImage` and
 * cannot be reattached) — same binary and capabilities.
 *
 * @p out_exit_code is set to 0 up front, so it reads 0 on every failure path.
 *
 * @return AXL_OK once the Shell has been started and exited (its exit code
 *     is in @p out_exit_code); AXL_ERR if no readable FV carries the Shell
 *     or it could not be loaded/started.
 */
int
axl_shell_launch_fv(
    const char *load_options,   ///< Shell command line (UTF-8), or NULL
    int        *out_exit_code   ///< [out] Shell's exit code (NULL allowed)
);

/**
 * @brief Run a shell command line and wait for it to complete.
 *
 * Public wrapper over the same shell-Execute bridge several AXL modules
 * already use internally (`axl-fs.c`'s alias mapping, `axl-ramdisk.c`'s
 * `map -r` advisory, `axl-sys.c`'s PATH reload): converts @p cmd from UTF-8
 * to UCS-2 and runs it via `EFI_SHELL_PROTOCOL.Execute` when the modern
 * UEFI Shell is hosting this image. On the old EFI 1.x shell (no
 * `EFI_SHELL_PROTOCOL`, only `SHELL_ENVIRONMENT`) it falls back to
 * `SHELL_ENVIRONMENT.Execute` on the shell-launched parent handle, so the
 * same call works on either shell — the fallback @ref axl_shell_kind()
 * distinguishes but a naive `LocateProtocol(EFI_SHELL_PROTOCOL)` misses
 * entirely.
 *
 * The command's own output goes wherever `Execute` sends it — the current
 * console, same as running it interactively. This call does not capture or
 * redirect it; embed a `>`/`>>a` redirect in @p cmd for that. It is a
 * different mechanism than capturing THIS image's own stdout (see
 * `axl_stream_set_stdout_tee` in `<axl/axl-stream.h>`).
 *
 * @return AXL_OK if the command was handed to a shell and executed
 *     (regardless of the command's own exit status, which is not
 *     surfaced); AXL_ERR if neither shell protocol is present (this image
 *     was not launched from a UEFI shell), @p cmd is NULL, or the UTF-8 to
 *     UCS-2 conversion failed.
 */
int
axl_shell_execute(
    const char *cmd   ///< shell command line (UTF-8)
);

/**
 * @brief Where a real UEFI Shell can be found, without launching it.
 *
 * The availability query behind @ref axl_shell_launch / @ref
 * axl_shell_launch_fv — lets a consumer surface "is a shell available, and
 * from where" (e.g. enable a remote Terminal) without paying a blocking
 * launch.
 */
typedef enum {
    AXL_SHELL_NONE = 0,   ///< neither a file nor a firmware-embedded Shell found
    AXL_SHELL_FILE,       ///< a `Shell.efi` file is locatable (launch via axl_shell_launch)
    AXL_SHELL_FIRMWARE    ///< no file, but a readable FV embeds the Shell (launch via axl_shell_launch_fv)
} AxlShellSource;

/**
 * @brief Report where a real UEFI Shell can be launched from.
 *
 * Checks for a staged `Shell.efi` first (the @ref axl_shell_launch search
 * path) and, failing that, for the ShellPkg Shell in a readable Firmware
 * Volume (the @ref axl_shell_launch_fv search). The file path is preferred
 * because it is the cheaper launch and matches a consumer's own staged copy.
 *
 * Read-only: it walks the firmware volumes and mounted volumes but loads
 * nothing and has no side effects. The walk is not free (it touches mounted
 * filesystems), so a consumer polling it for a UI flag should cache the
 * result rather than call it on every refresh.
 *
 * @return `AXL_SHELL_FILE` if a `Shell.efi` file is locatable;
 *     `AXL_SHELL_FIRMWARE` if no file exists but the firmware embeds the
 *     Shell in a readable FV; `AXL_SHELL_NONE` if neither is available.
 */
AxlShellSource
axl_shell_locate(void);

/**
 * @brief Every place a real UEFI Shell is available, reported independently.
 *
 * The richer companion to @ref AxlShellSource: where that enum collapses to
 * a single file-first verdict — a locatable `Shell.efi` file *masks* the
 * firmware-embedded Shell entirely — this reports each source on its own, so
 * a consumer with an FV-first policy (launch the firmware Shell by default,
 * fall back to a file only where the firmware carries none) can see and
 * prefer the FV even when a foreign `Shell.efi` also happens to exist.
 *
 * `fv` is exactly `fv_count > 0`; the count is how many of the known
 * Shell FV file GUIDs matched a readable Firmware Volume (most firmware
 * carries one, so 0 or 1 is typical).
 */
typedef struct {
    bool     file;              ///< a `Shell.efi` file is locatable (launch via @ref axl_shell_launch)
    char     file_path[256];    ///< path to that file; empty string ("") unless `file`
    bool     fv;                ///< a readable Firmware Volume embeds the Shell (launch via @ref axl_shell_launch_fv)
    uint32_t fv_count;          ///< how many known Shell FV file GUIDs matched a readable FV; `fv` == (`fv_count` > 0)
} AxlShellSources;

/**
 * @brief Report every source a real UEFI Shell can be launched from.
 *
 * Unlike @ref axl_shell_locate (file-first, single verdict), this fills
 * @p out with each source independently — a staged `Shell.efi` file (with
 * its path) AND the firmware-embedded FV Shell (with a match count) — so an
 * FV-first consumer can prefer the firmware Shell without the file masking
 * it. Finding *nothing* is still a successful query: @p out is all-zero
 * (`file` / `fv` false, `fv_count` 0, `file_path` empty) and the call
 * returns `AXL_OK`.
 *
 * Read-only: it walks the firmware volumes and mounted volumes but loads
 * nothing and has no side effects — the same walk (and cost) as
 * @ref axl_shell_locate, so a consumer polling it for a UI flag should cache
 * the result rather than call it on every refresh.
 *
 * @return `AXL_OK` once the query completes (even if no Shell is
 *     available anywhere); `AXL_ERR` only if @p out is NULL.
 */
AXL_WARN_UNUSED int
axl_shell_sources(
    AxlShellSources *out   ///< [out] filled with each available Shell source
);

/**
 * @brief Which command shell, if any, is hosting this image.
 *
 * Distinct from @ref AxlShellSource (where a shell could be *launched*
 * from): this reports the shell *currently present in the firmware*, by
 * the protocol it publishes. It is the single branch point for
 * shell-dependent behavior that differs between the modern EDK2 shell and
 * the old EFI 1.x shell — e.g. whether a programmatic map injection
 * (`SetMap`) will be honored.
 */
typedef enum {
    AXL_SHELL_KIND_NONE = 0,  ///< no shell protocol present (BDS, a driver, or minimal firmware)
    AXL_SHELL_KIND_UEFI,    ///< the modern EDK2 UEFI Shell 2.x — `EFI_SHELL_PROTOCOL` is present
    AXL_SHELL_KIND_EFI_1X   ///< the old EFI 1.x shell (EFI Toolkit "newshell") — `SHELL_ENVIRONMENT`/`SHELL_INTERFACE` is present
} AxlShellKind;

/**
 * @brief Detect which command shell is hosting the running image.
 *
 * Probes, in order: `EFI_SHELL_PROTOCOL` (→ @ref AXL_SHELL_KIND_UEFI); then
 * the EFI 1.x `SHELL_ENVIRONMENT` global protocol or the `SHELL_INTERFACE`
 * protocol on this image's own handle (→ @ref AXL_SHELL_KIND_EFI_1X);
 * otherwise @ref AXL_SHELL_KIND_NONE. The modern shell is checked first
 * because a modern shell never publishes the legacy GUIDs, so the order is
 * unambiguous.
 *
 * Read-only: a couple of `LocateProtocol`/`HandleProtocol` lookups, no
 * side effects. The result reflects the firmware at call time; it does not
 * change over a boot, so a caller may cache it.
 *
 * @return the hosting shell's @ref AxlShellKind.
 */
AxlShellKind
axl_shell_kind(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SHELL_H */
