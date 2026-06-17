/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fv.c
    Firmware-volume enumeration and attributes.

    Enumerates the handles publishing EFI_FIRMWARE_VOLUME2_PROTOCOL and
    reports each volume's GetVolumeAttributes status bits and a file
    count from the GetNextFile walk. Read-only inventory probe — no file
    contents or sections are read.

    The handle set is located once and cached for the image lifetime
    (the AxlBlock / AxlSerial model); `axl_fv_next` recovers its position
    from the handle the caller passes back. The cached buffer is freed at
    exit.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* EFI_FIRMWARE_VOLUME2_PROTOCOL (extra) */
#include "../util/axl-handle-iter.h"
#include <axl/axl-mem.h>
#include <axl/axl-fv.h>

/* GetVolumeAttributes status bits (PI 1.8, EFI_FV_ATTRIBUTES). The
   generated headers do not carry these. */
#define FV2_READ_STATUS   0x0000000000000004ULL
#define FV2_WRITE_STATUS  0x0000000000000020ULL
#define FV2_LOCK_STATUS   0x0000000000000080ULL

/* EFI_FIRMWARE_VOLUME2_PROTOCOL_GUID — not in generated/guids.h, so it
   is pinned here (the TCG2 precedent in axl-protocol.c). */
static const EFI_GUID FV2_PROTOCOL_GUID = {
    0x220e73b6, 0x6bdb, 0x4413,
    { 0x84, 0x05, 0xb9, 0x74, 0xb1, 0x08, 0x61, 0x9a }
};

/* Enumeration cursor shared with the other platform readers; the handle
   set is located once and cached for the image lifetime. */
static AxlHandleIter fv_iter = {
    .guid = &FV2_PROTOCOL_GUID,
    .what = "firmware volume"
};

static EFI_FIRMWARE_VOLUME2_PROTOCOL *
fv_proto(
    AxlHandle handle
    )
{
    if (handle == NULL) {
        return NULL;
    }
    EFI_FIRMWARE_VOLUME2_PROTOCOL *fv = NULL;
    EFI_STATUS status = axl_efi_call(
        axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handle, (EFI_GUID *)&FV2_PROTOCOL_GUID, (void **)&fv);
    if (EFI_ERROR(status)) {
        return NULL;
    }
    return fv;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlHandle
axl_fv_next(
    AxlHandle prev
    )
{
    return axl_handle_iter_next(&fv_iter, prev);
}

int
axl_fv_get_attributes(
    AxlHandle        handle,
    AxlFvAttributes *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    EFI_FIRMWARE_VOLUME2_PROTOCOL *fv = fv_proto(handle);
    if (fv == NULL || fv->GetVolumeAttributes == NULL) {
        return AXL_ERR;
    }

    EFI_FV_ATTRIBUTES attr   = 0;
    EFI_STATUS        status = axl_efi_call(fv->GetVolumeAttributes, 2, fv, &attr);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    out->readable = (attr & FV2_READ_STATUS) != 0;
    out->writable = (attr & FV2_WRITE_STATUS) != 0;
    out->locked   = (attr & FV2_LOCK_STATUS) != 0;
    return AXL_OK;
}

int
axl_fv_count_files(
    AxlHandle  handle,
    size_t    *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    EFI_FIRMWARE_VOLUME2_PROTOCOL *fv = fv_proto(handle);
    if (fv == NULL || fv->GetNextFile == NULL) {
        return AXL_ERR;
    }

    /* GetNextFile tracks position in a caller-allocated key buffer whose
       size the protocol publishes in KeySize. Zeroed = start at the
       first file. */
    size_t key_size = (fv->KeySize > 0) ? fv->KeySize : 1;
    void  *key      = axl_calloc(1, key_size);
    if (key == NULL) {
        return AXL_ERR;
    }

    size_t n = 0;
    for (;;) {
        EFI_FV_FILETYPE        type  = 0;   /* EFI_FV_FILETYPE_ALL */
        EFI_GUID               name;
        EFI_FV_FILE_ATTRIBUTES fattr = 0;
        UINTN                  fsize = 0;
        EFI_STATUS             status = axl_efi_call(
            fv->GetNextFile, 6, fv, key, &type, &name, &fattr, &fsize);
        if (status == EFI_NOT_FOUND) {
            break;   /* clean end of enumeration */
        }
        if (EFI_ERROR(status)) {
            axl_free(key);
            return AXL_ERR;   /* hard read error — not a truncated count */
        }
        n++;
    }

    axl_free(key);
    *out = n;
    return AXL_OK;
}
