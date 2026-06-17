/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-output.c
    GOP output inventory + per-output mode query.

    These accessors are pure EFI_GRAPHICS_OUTPUT_PROTOCOL reads — they
    enumerate displays and their modes and never rasterize a vector path.
    They live in their own translation unit, separate from the drawing
    primitives in axl-gfx.c, so a consumer that only enumerates displays
    does not drag the FreeType ftgrays rasterizer (and its FTL credit
    obligation) into its link: the EFI link exports every global as a
    --gc-sections root and selects archive members at object granularity,
    so sharing an object with axl_gfx_push_clip_path (-> ftgrays) would
    root the rasterizer in any binary that touches a single accessor.
**/

#include "../backend/axl-backend.h"
#include "axl-gfx-internal.h"
#include <axl/axl-gfx.h>

bool
axl_gfx_internal_map_pixel_format(
    EFI_GRAPHICS_PIXEL_FORMAT  in,
    AxlGfxPixelFormat         *out
    )
{
    switch (in) {
    case PixelRedGreenBlueReserved8BitPerColor:
        *out = AXL_GFX_PIXEL_FORMAT_RGBX8;
        return true;
    case PixelBlueGreenRedReserved8BitPerColor:
        *out = AXL_GFX_PIXEL_FORMAT_BGRX8;
        return true;
    case PixelBitMask:
        *out = AXL_GFX_PIXEL_FORMAT_BITMASK;
        return true;
    case PixelBltOnly:
        *out = AXL_GFX_PIXEL_FORMAT_BLT_ONLY;
        return true;
    default:
        return false;
    }
}

/* Locate and open the GOP for output @index — the same firmware-handle
   indexing axl_gfx_output_count / axl_gfx_output_get use. Returns the
   protocol on success and, if @out_handle is non-NULL, the handle (so the
   caller can read sibling protocols like EDID off it). The
   LocateHandleBuffer allocation is freed before returning; the handle
   value stays valid (it is an opaque firmware pointer, not into the
   buffer). Returns NULL if there is no GOP, @index is out of range, or
   the handle could not be opened. */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *
gop_open_output(
    size_t       index,
    EFI_HANDLE  *out_handle
    )
{
    EFI_GUID    gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    size_t      n        = 0;
    EFI_HANDLE *handles  = NULL;
    EFI_STATUS  st = axl_bs()->LocateHandleBuffer(ByProtocol, &gop_guid, NULL,
                                                  &n, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        return NULL;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (index < n) {
        EFI_HANDLE h = handles[index];
        if (!EFI_ERROR(axl_bs()->HandleProtocol(h, &gop_guid, (void **)&gop))
            && gop != NULL) {
            if (out_handle != NULL) {
                *out_handle = h;
            }
        } else {
            gop = NULL;
        }
    }

    axl_backend_free(handles);
    return gop;
}

size_t
axl_gfx_output_count(void)
{
    EFI_GUID    guid    = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    size_t      n       = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS  st = axl_bs()->LocateHandleBuffer(ByProtocol, &guid, NULL,
                                                  &n, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        return 0;
    }
    /* LocateHandleBuffer allocates with gBS->AllocatePool. */
    axl_backend_free(handles);
    return n;
}

int
axl_gfx_output_get(
    size_t         index,
    AxlGfxOutput  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }

    EFI_HANDLE                    h   = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = gop_open_output(index, &h);
    if (gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        return AXL_ERR;
    }

    AxlGfxOutput o = {0};
    if (!axl_gfx_internal_map_pixel_format(gop->Mode->Info->PixelFormat,
                                           &o.pixel_format)) {
        return AXL_ERR;
    }
    o.width            = gop->Mode->Info->HorizontalResolution;
    o.height           = gop->Mode->Info->VerticalResolution;
    o.stride           = gop->Mode->Info->PixelsPerScanLine;
    o.framebuffer      = gop->Mode->FrameBufferBase;
    o.framebuffer_size = gop->Mode->FrameBufferSize;
    o.mode_count       = gop->Mode->MaxMode;
    o.current_mode     = gop->Mode->Mode;

    /* EDID, if this display published it, lives on the SAME handle. */
    EFI_GUID edid_guid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;
    EFI_EDID_DISCOVERED_PROTOCOL *edid = NULL;
    if (!EFI_ERROR(axl_bs()->HandleProtocol(h, &edid_guid, (void **)&edid))
        && edid != NULL && edid->Edid != NULL && edid->SizeOfEdid > 0) {
        o.edid     = edid->Edid;
        o.edid_len = edid->SizeOfEdid;
    }

    *out = o;
    return AXL_OK;
}

int
axl_gfx_output_query_mode(
    size_t             output_index,
    uint32_t           mode_index,
    AxlGfxOutputMode  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = gop_open_output(output_index, NULL);
    if (gop == NULL || gop->Mode == NULL || mode_index >= gop->Mode->MaxMode) {
        return AXL_ERR;
    }

    UINTN                                 size = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi   = NULL;
    EFI_STATUS status = gop->QueryMode(gop, mode_index, &size, &mi);
    if (status != 0 || mi == NULL) {
        return AXL_ERR;
    }

    AxlGfxOutputMode m  = {0};
    bool             ok = axl_gfx_internal_map_pixel_format(mi->PixelFormat,
                                                            &m.pixel_format);
    if (ok) {
        m.index  = mode_index;
        m.width  = mi->HorizontalResolution;
        m.height = mi->VerticalResolution;
        m.stride = mi->PixelsPerScanLine;
    }

    /* QueryMode allocates the info via AllocatePool; the caller owns it. */
    axl_bs()->FreePool(mi);
    if (!ok) {
        return AXL_ERR;
    }
    *out = m;
    return AXL_OK;
}

int
axl_gfx_output_get_pixel_bitmask(
    size_t              output_index,
    AxlGfxPixelBitmask *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = gop_open_output(output_index, NULL);
    if (gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL
        || gop->Mode->Info->PixelFormat != PixelBitMask) {
        return AXL_ERR;
    }

    const EFI_PIXEL_BITMASK *pi = &gop->Mode->Info->PixelInformation;
    out->red_mask      = pi->RedMask;
    out->green_mask    = pi->GreenMask;
    out->blue_mask     = pi->BlueMask;
    out->reserved_mask = pi->ReservedMask;
    return AXL_OK;
}
