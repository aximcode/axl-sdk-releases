/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-efi1x.h
    Backend-internal support for the old EFI 1.x shell (the EFI Toolkit
    "newshell"), which predates EFI_SHELL_PROTOCOL.

    That shell publishes SHELL_ENVIRONMENT, offering only GetMap (name ->
    device path), CurDir, GetEnv and Execute. Everything the modern shell
    hands us for free — path resolution, file open, the reverse map lookup —
    has to be built on top of those four here.

    Every entry point is a no-op returning failure when SHELL_ENVIRONMENT is
    absent, so callers can use them as an unconditional fallback after
    get_shell() comes back NULL.

    Internal header — not installed, not part of the public API.
**/

#ifndef AXL_BACKEND_NATIVE_EFI1X_H
#define AXL_BACKEND_NATIVE_EFI1X_H

#include "axl-backend.h"

/**
 * @brief Locate the old shell's SHELL_ENVIRONMENT protocol (cached).
 *
 * @return the protocol, or NULL when this is not the old EFI 1.x shell.
 */
EFI_SHELL_ENVIRONMENT *
axl_efi1x_shell_env(
    void
    );

/**
 * @brief Open a file by path, resolving `NAME:` and cwd-relative spellings
 *     through SHELL_ENVIRONMENT (GetMap + CurDir) and EFI_FILE_PROTOCOL.
 *
 * Accepts the same spellings the shell itself does: `fs0:\dir\file`,
 * `\dir\file` (root of the current volume), `dir\file` (relative to the
 * current directory), and `fs0:dir\file` (relative to that volume's current
 * directory). Matching real shell semantics, `fs0:\` opens the volume ROOT
 * while a bare `fs0:` opens that volume's CURRENT directory (they differ once
 * the cwd is not the root). A CREATE mode against a bare-volume path is
 * rejected (no leaf to create).
 *
 * @return AXL_OK on success, AXL_ERR on a bad path, an unmapped volume, a
 *     CREATE against a bare volume, or any firmware failure.
 */
int
axl_efi1x_file_open(
    const unsigned short  *path,        ///< UCS-2 file path
    uint64_t               mode,        ///< EFI file mode flags
    uint64_t               attributes,  ///< EFI file attributes (create only)
    EFI_FILE_PROTOCOL    **out          ///< (out) receives the open file
    );

/**
 * @brief The old shell's current working directory, e.g. `fs0:\dir`.
 *
 * @return firmware-owned UCS-2 string (do not free), or NULL.
 */
const unsigned short *
axl_efi1x_getcwd(
    void
    );

/**
 * @brief Read an old-shell environment variable.
 *
 * @return firmware-owned UCS-2 value (do not free), or NULL if unset.
 */
const unsigned short *
axl_efi1x_getenv(
    const unsigned short  *name  ///< UCS-2 variable name
    );

/**
 * @brief A ParentImageHandle valid for SHELL_ENVIRONMENT.Execute.
 *
 * Execute rejects a handle with no SHELL_INTERFACE (a resident driver's own
 * image handle) as EFI_INVALID_PARAMETER. Returns this image's handle when it
 * is itself shell-launched, otherwise a live shell-launched image's handle
 * (the launcher that called into the driver), falling back to gImageHandle.
 *
 * @return an EFI_HANDLE suitable as Execute's ParentImageHandle.
 */
EFI_HANDLE
axl_efi1x_shell_parent(
    void
    );

/**
 * @brief Set an old-shell environment variable.
 *
 * The old shell's SHELL_ENVIRONMENT has no SetEnv member, so this drives the
 * shell's own `set` command through Execute (the same mechanism mkrd uses for
 * `map -r`). Runs `set [-v] NAME VALUE` in-shell so the assignment lands in the
 * shell's global environment and a later GetEnv (or the interactive `set`
 * command) sees it.
 *
 * @return AXL_OK if the command ran; AXL_ERR on no shell environment, a NULL
 *     name, a name/value the `set` command line can't safely carry (a space or
 *     shell metacharacter — the old shell's `set` has no quoting we can rely
 *     on), or an overflow of the command buffer.
 */
int
axl_efi1x_setenv(
    const unsigned short  *name,      ///< UCS-2 variable name
    const unsigned short  *value,     ///< UCS-2 value (NULL clears)
    bool                   volatile_var  ///< true -> `set -v` (session-scoped)
    );

/**
 * @brief Reverse-look-up @p device_path to its `fs<n>` name.
 *
 * The old shell has no GetMapFromDevicePath, so this iterates `fs0`..`fs63`,
 * GetMap's each, and byte-compares the returned device path. Writes
 * LOWERCASE `fs<n>` (no colon) — the casing the old shell itself displays,
 * so a listing stays consistent with the shell you are in.
 *
 * @return AXL_OK on a match; AXL_ERR when there is no shell environment, the
 *     buffer is too small, or no `fs<n>` maps to this path (e.g. the disk is
 *     not in the map yet because `map -r` has not run).
 */
int
axl_efi1x_map_fs_name_from_dp(
    void   *device_path,  ///< device path to look up
    char   *out,          ///< (out) receives "fs<n>"
    size_t  out_size      ///< size of @p out in bytes
    );

/**
 * @brief True when the old shell maps @p name (bare or ':'-terminated).
 *
 * @return true if the name resolves to a device path.
 */
bool
axl_efi1x_map_exists(
    const unsigned short  *name  ///< UCS-2 map name
    );

#endif /* AXL_BACKEND_NATIVE_EFI1X_H */
