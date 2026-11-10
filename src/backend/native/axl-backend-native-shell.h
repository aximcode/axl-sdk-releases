/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-shell.h
    Backend-internal path resolution under the modern UEFI Shell
    (EFI_SHELL_PROTOCOL) that parks nothing on the Shell's free list.

    EDK2's Shell hands several EFI_SHELL_PROTOCOL results back through
    AddBufferToFreeList (ShellPkg/Application/Shell/Shell.c) and frees that
    list only when the CURRENT SHELL COMMAND ends: GetCurDir, GetEnv,
    GetMapFromDevicePath, and through them GetDevicePathFromFilePath on a
    relative path (it calls GetCurDir) and OpenFileByName / CreateFile on
    EVERY path (OpenRootByHandle names the root through
    GetMapFromDevicePath). A short command gets it all back; an image that
    loops for hours, or a resident driver whose calls park onto whatever
    command is current, grows without bound.

    So the backend resolves paths itself, the way the Shell does, from the
    calls that park nothing:

        NAME:rest  ->  GetDevicePathFromMap("NAME:")   (the map's own path)
                   ->  LocateDevicePath(SimpleFileSystem) -> volume
                   ->  OpenVolume -> root->Open(rest, mode, attributes)

    and a path without a ':' is first joined onto the current directory,
    which is kept in a cache read with ONE GetCurDir. The algorithm is
    EfiShellGetDevicePathFromFilePath / InternalOpenFileDevicePath's,
    reproduced string for string (including its quirks: a one-character
    remainder names the root, a leading '\' climbs to the current volume's
    root), so every spelling resolves exactly as it did through the Shell.

    Internal header -- not installed, not part of the public API.
**/

#ifndef AXL_BACKEND_NATIVE_SHELL_H
#define AXL_BACKEND_NATIVE_SHELL_H

#include "axl-backend.h"

/**
 * @brief Where a path lands under the modern Shell.
 *
 * @p rest is what the Shell would hand EFI_FILE_PROTOCOL.Open against the
 * volume root, verbatim. It points into the caller's path, into a static
 * string, or into @p joined, which axl_shell_path_done releases.
 */
typedef struct {
    EFI_HANDLE     volume;   ///< SimpleFileSystem handle
    const CHAR16  *rest;     ///< remainder below the root, as the Shell builds it
    CHAR16        *joined;   ///< allocation behind a relative path's @p rest, or NULL
} AxlShellPath;

/**
 * @brief Resolve @p path as EFI_SHELL_PROTOCOL.GetDevicePathFromFilePath
 *     does, without parking a buffer.
 *
 * Call axl_shell_path_done on @p out after AXL_OK.
 *
 * @return AXL_OK with @p out filled; AXL_NOT_FOUND where the Shell answers
 *     NULL (no current directory, an unmapped name, no SimpleFileSystem on
 *     the mapping, no memory -- the Shell does not tell these apart either).
 */
int
axl_shell_path_resolve(
    EFI_SHELL_PROTOCOL  *shell,  ///< the live Shell
    const CHAR16        *path,   ///< '\'-separated path
    AxlShellPath        *out     ///< [out] the resolution
    );

/**
 * @brief Release what axl_shell_path_resolve allocated. NULL-safe.
 */
void
axl_shell_path_done(
    AxlShellPath  *p  ///< a resolution, or NULL
    );

/**
 * @brief Open @p path as EFI_SHELL_PROTOCOL.OpenFileByName (attributes 0)
 *     or CreateFile (READ|WRITE|CREATE with attributes) opens a regular
 *     file, without parking a buffer.
 *
 * The special names OpenFileByName treats itself (`>i`, `>o`, `>e`, `NUL`,
 * `>v...`) are the caller's to route to the Shell; this opens only files.
 * The handle is the volume's own EFI_FILE_PROTOCOL, which is what a
 * SHELL_FILE_HANDLE for a file is, so the Shell's handle calls (ReadFile,
 * GetFileInfo, CloseFile, ...) take it unchanged.
 *
 * @return the EFI_STATUS the Shell would return: EFI_NOT_FOUND when the
 *     path does not resolve, else OpenVolume's or Open's status.
 */
EFI_STATUS
axl_shell_path_open(
    EFI_SHELL_PROTOCOL   *shell,       ///< the live Shell
    const CHAR16         *path,        ///< '\'-separated path
    UINT64                mode,        ///< EFI_FILE_MODE_* flags, passed through
    UINT64                attributes,  ///< attributes for a create, else 0
    EFI_FILE_PROTOCOL   **out          ///< [out] the open file
    );

/**
 * @brief EFI_SHELL_PROTOCOL.GetCurDir(NULL), parking one buffer only when
 *     the current directory has changed.
 *
 * The Shell mirrors `cwd` into a volatile UEFI variable under
 * gShellVariableGuid, and reading that parks nothing. An answer is kept
 * only while the variable holds exactly it, and reused while that stays so
 * and the directory's own mapping still resolves. When the two disagree or
 * there is no variable (a Shell that keeps none), every call is a
 * GetCurDir -- what this replaced -- never a stale answer.
 *
 * @return the current directory (valid until the next call; do not free),
 *     or NULL when the Shell reports none.
 */
const CHAR16 *
axl_shell_cwd(
    EFI_SHELL_PROTOCOL  *shell  ///< the live Shell
    );

/**
 * @brief Which of `FS0:`..`FS63:` the Shell maps to @p device_path,
 *     found without parking a buffer.
 *
 * Each name is looked up with GetDevicePathFromMap and its device path
 * compared byte for byte. When exactly one matches it IS the volume's first
 * fs<n> name, whatever the order of the Shell's list -- which no call that
 * parks nothing can show (SetMap of a name a volume already has re-appends
 * it). When none or several match, the caller must ask the Shell.
 *
 * What it cannot see: an fs<n> name outside the probe (fs64 and up, or a
 * custom spelling such as `fs07`) on a volume that also has one inside it,
 * or a name set for a device path that is equal only as text.
 *
 * @return how many of the probed names map to @p device_path; with 1,
 *     @p index receives its number.
 */
unsigned
axl_shell_fs_probe(
    EFI_SHELL_PROTOCOL  *shell,        ///< the live Shell
    void                *device_path,  ///< the volume's device path
    unsigned            *index         ///< [out] n of the matching FSn
    );

#endif /* AXL_BACKEND_NATIVE_SHELL_H */
