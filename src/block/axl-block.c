/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-block.c
    Block-device enumeration and media descriptors.

    Enumerates the handles publishing EFI_BLOCK_IO_PROTOCOL and exposes
    each device's EFI_BLOCK_IO_MEDIA descriptor as a typed AxlBlockMedia.
    No firmware block I/O is performed — this is discovery + media
    readout only.

    The handle set is located once with LocateHandleBuffer and cached for
    the image lifetime (the AxlUsb model). Because `axl_block_next`
    recovers the iteration position from the handle the caller passes
    back, there is no shared mutable cursor: independent walks do not
    interfere, and a handle outside the cached set rewinds to the first
    device. The cached buffer is freed at exit so AxlMem's leak detector
    stays quiet on real hardware.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* EFI_BLOCK_IO_PROTOCOL (extra) + EFI_BLOCK_IO_MEDIA */
#include "../util/axl-handle-iter.h"
#include <axl/axl-block.h>

/* Enumeration cursor shared with the other platform readers; the handle
   set is located once and cached for the image lifetime. */
static AxlHandleIter block_iter = {
    .guid = &gEfiBlockIoProtocolGuid,
    .what = "block device"
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlHandle
axl_block_next(
    AxlHandle prev
    )
{
    return axl_handle_iter_next(&block_iter, prev);
}

int
axl_block_get_media(
    AxlHandle      handle,
    AxlBlockMedia *out
    )
{
    if (handle == NULL || out == NULL) {
        return AXL_ERR;
    }

    EFI_BLOCK_IO_PROTOCOL *bio    = NULL;
    EFI_STATUS             status = axl_efi_call(
        axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handle, &gEfiBlockIoProtocolGuid, (void **)&bio);
    if (EFI_ERROR(status) || bio == NULL || bio->Media == NULL) {
        return AXL_ERR;
    }

    EFI_BLOCK_IO_MEDIA *md = bio->Media;
    out->media_id          = md->MediaId;
    out->removable_media   = md->RemovableMedia != 0;
    out->media_present     = md->MediaPresent != 0;
    out->logical_partition = md->LogicalPartition != 0;
    out->read_only         = md->ReadOnly != 0;
    out->write_caching     = md->WriteCaching != 0;
    out->block_size        = md->BlockSize;
    out->last_block        = md->LastBlock;
    return AXL_OK;
}
