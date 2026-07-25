/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-nosh.c
    Shell-free file access for the native backend.

    When BdsDxe launches an app directly from the removable-media boot slot
    there is no shell of any kind — neither EFI_SHELL_PROTOCOL nor the EFI 1.x
    SHELL_ENVIRONMENT — so neither of the backend's other two file paths can
    resolve anything. This one goes straight to the firmware:

        fsN:\dir\file   ->  LocateHandleBuffer(SimpleFileSystem)[N] -> handle
                        ->  HandleProtocol(SimpleFileSystem)
                        ->  OpenVolume() -> root EFI_FILE_PROTOCOL
                        ->  root->Open("\dir\file")

    The `fsN` namespace is positional in that same LocateHandleBuffer array —
    the only naming available with no map to consult, and the one
    axl_volume_enumerate already falls back to. See the header for why that
    is safe here and why it must never be reached while a shell is live.

    The resulting handle is an EFI_FILE_PROTOCOL *, the same shape the old
    shell's path produces, so the rest of the backend's file layer works on it
    unchanged.
**/

#include "axl-backend-native-nosh.h"
#include <axl/axl-log.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("backend");

/* Longest remainder (in CHAR16, NUL included) we will hand to
   EFI_FILE_PROTOCOL.Open. FAT's own limit is far below this. */
#define AXL_NOSH_PATH_MAX  512u

/* Ceiling on the volume index a path may name. Nothing enforces a limit in
   the firmware, but a path claiming `fs100000:` is a caller bug, and the cap
   keeps the digit parse free of overflow concerns. */
#define AXL_NOSH_MAX_FS    4096u

// ===================================================================
// Volume enumeration
// ===================================================================

/* Every SimpleFileSystem handle, in LocateHandleBuffer order. On success the
   caller frees @p handles with axl_backend_free.

   @return AXL_OK with @p handles / @p count filled; AXL_ERR otherwise. */
static int
volume_handles(
    EFI_HANDLE  **handles,
    size_t       *count
    )
{
    EFI_GUID    guid = gEfiSimpleFileSystemProtocolGuid;
    EFI_HANDLE *buf  = NULL;
    UINTN       n    = 0;

    EFI_STATUS st = gBS->LocateHandleBuffer(ByProtocol, &guid, NULL, &n, &buf);
    if (EFI_ERROR(st) || buf == NULL || n == 0) {
        /* A success with n == 0 should not happen (EDK2 reports NOT_FOUND
           with buf NULL), but the pool block is ours the moment the call
           succeeds — drop it rather than trust that. */
        if (buf != NULL) {
            axl_backend_free(buf);
        }
        return AXL_ERR;
    }
    *handles = buf;
    *count   = (size_t)n;
    return AXL_OK;
}

/* Open the root directory of the @p index'th SimpleFileSystem volume.

   @return the root EFI_FILE_PROTOCOL (caller closes), or NULL. */
static EFI_FILE_PROTOCOL *
open_volume_root(
    size_t  index
    )
{
    EFI_HANDLE *handles = NULL;
    size_t      count   = 0;

    if (volume_handles(&handles, &count) != AXL_OK) {
        return NULL;
    }
    if (index >= count) {
        /* The ordinary "no such volume" result — a caller probing fs0..fsN
           hits it constantly, so it is not worth a log line. */
        axl_backend_free(handles);
        return NULL;
    }

    EFI_GUID                         guid = gEfiSimpleFileSystemProtocolGuid;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs   = NULL;
    EFI_STATUS st = gBS->HandleProtocol(handles[index], &guid, (VOID **)&fs);
    axl_backend_free(handles);
    if (EFI_ERROR(st) || fs == NULL) {
        return NULL;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st) || root == NULL) {
        axl_debug("nosh: OpenVolume failed (status=0x%llx)",
                  (unsigned long long)st);
        return NULL;
    }
    return root;
}

// ===================================================================
// Path resolution
// ===================================================================

/* Parse a leading `fsN:` (case-insensitive) volume prefix.

   @return true when @p path starts with one that fits the index cap; @p index
       receives N and @p rest points just past the ':'. */
static bool
split_fs_index(
    const CHAR16  *path,
    size_t        *index,
    const CHAR16 **rest
    )
{
    if ((path[0] != (CHAR16)'f' && path[0] != (CHAR16)'F')
        || (path[1] != (CHAR16)'s' && path[1] != (CHAR16)'S'))
    {
        return false;
    }

    size_t i = 2;
    size_t n = 0;
    if (path[i] < (CHAR16)'0' || path[i] > (CHAR16)'9') {
        return false;   /* "fs:" / "fsx:" is not a volume we name */
    }
    while (path[i] >= (CHAR16)'0' && path[i] <= (CHAR16)'9') {
        n = n * 10u + (size_t)(path[i] - (CHAR16)'0');
        if (n > AXL_NOSH_MAX_FS) {
            return false;
        }
        i++;
    }
    if (path[i] != (CHAR16)':') {
        return false;   /* "fs0abc" — not a prefix, just a name starting "fs" */
    }

    *index = n;
    *rest  = path + i + 1;
    return true;
}

/* Copy @p rest into @p out as a root-relative remainder: separators
   normalized to '\', any leading and trailing separator stripped. An empty
   result means the volume root itself.

   Without a shell there is no current directory, so a volume-relative
   spelling (`fs0:dir\file`) can only be read as root-relative.

   @return AXL_OK, or AXL_ERR when the remainder does not fit. */
static int
normalize_remainder(
    const CHAR16  *rest,
    CHAR16        *out
    )
{
    while (*rest == (CHAR16)'\\' || *rest == (CHAR16)'/') {
        rest++;
    }

    size_t len = 0;
    for (; rest[len] != 0; len++) {
        if (len + 1 >= AXL_NOSH_PATH_MAX) {
            return AXL_ERR;
        }
        out[len] = (rest[len] == (CHAR16)'/') ? (CHAR16)'\\' : rest[len];
    }

    /* A trailing separator (`fs0:\dir\`) survives the copy; strip it. Some
       FAT drivers reject it, and the modern shell's OpenFileByName does not
       produce it. */
    while (len > 0 && out[len - 1] == (CHAR16)'\\') {
        len--;
    }
    out[len] = 0;
    return AXL_OK;
}

int
axl_nosh_file_open(
    const unsigned short  *path,
    uint64_t               mode,
    uint64_t               attributes,
    EFI_FILE_PROTOCOL    **out
    )
{
    if (path == NULL || out == NULL) {
        return AXL_ERR;
    }

    size_t        index = 0;
    const CHAR16 *rest  = NULL;
    if (!split_fs_index((const CHAR16 *)path, &index, &rest)) {
        /* No volume named, and no current directory to supply one. Refusing
           beats guessing volume 0 — that would silently read or, worse,
           CREATE on whichever volume happened to enumerate first. */
        return AXL_ERR;
    }

    CHAR16 rel[AXL_NOSH_PATH_MAX];
    if (normalize_remainder(rest, rel) != AXL_OK) {
        return AXL_ERR;
    }

    EFI_FILE_PROTOCOL *root = open_volume_root(index);
    if (root == NULL) {
        return AXL_ERR;
    }

    if (rel[0] == 0) {
        *out = root;   /* the path named the volume itself */
        return AXL_OK;
    }

    /* EFI_FILE_PROTOCOL::Open accepts only READ, READ|WRITE or
       READ|WRITE|CREATE. The shell's OpenFileByName is laxer, so normalize
       rather than fail a write-only request that works under a shell. */
    if ((mode & AXL_FILE_MODE_WRITE) != 0) {
        mode |= AXL_FILE_MODE_READ;
    }

    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS st = root->Open(root, &file, rel, mode, attributes);
    root->Close(root);
    if (EFI_ERROR(st)) {
        /* EFI_NOT_FOUND is the ordinary "no such file" probe result and would
           flood the log; real errors still surface. */
        if (st != EFI_NOT_FOUND) {
            axl_debug("nosh: open failed (status=0x%llx)",
                      (unsigned long long)st);
        }
        return AXL_ERR;
    }
    *out = file;
    return AXL_OK;
}

// ===================================================================
// Positional volume naming
// ===================================================================

/* Render "fs<index>" into @p out. */
static int
format_fs_name(
    size_t  index,
    char   *out,
    size_t  out_size
    )
{
    int n = axl_snprintf(out, out_size, "fs%zu", index);
    return (n > 0 && (size_t)n < out_size) ? AXL_OK : AXL_ERR;
}

int
axl_nosh_volume_name_for_handle(
    void   *device_handle,
    char   *out,
    size_t  out_size
    )
{
    if (device_handle == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    EFI_HANDLE *handles = NULL;
    size_t      count   = 0;
    if (volume_handles(&handles, &count) != AXL_OK) {
        return AXL_ERR;
    }

    int rc = AXL_ERR;
    for (size_t i = 0; i < count; i++) {
        if (handles[i] == (EFI_HANDLE)device_handle) {
            rc = format_fs_name(i, out, out_size);
            break;
        }
    }
    axl_backend_free(handles);
    return rc;
}

int
axl_nosh_map_fs_name_from_dp(
    void   *device_path,
    char   *out,
    size_t  out_size
    )
{
    if (device_path == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    EFI_HANDLE *handles = NULL;
    size_t      count   = 0;
    if (volume_handles(&handles, &count) != AXL_OK) {
        return AXL_ERR;
    }

    EFI_GUID dp_guid = gEfiDevicePathProtocolGuid;
    int      rc      = AXL_ERR;
    for (size_t i = 0; i < count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &dp_guid, (VOID **)&dp))
            || dp == NULL)
        {
            continue;
        }
        if (axl_backend_dp_equal(dp, device_path)) {
            rc = format_fs_name(i, out, out_size);
            break;
        }
    }
    axl_backend_free(handles);
    return rc;
}
