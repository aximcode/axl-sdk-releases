/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-image.c
    Executable-image lifecycle.

    A thin wrapper over `axl_driver_load`/`_unload` that adds the
    one piece those functions don't expose: the image's exit code
    on `StartImage`. Drivers don't return cleanly so
    `axl_driver_start` drops the EFI_STATUS; user-image consumers
    typically want it.

    The opaque @ref AxlImage handle is a tiny struct holding the
    underlying `AxlDriverHandle`. We pay one heap allocation per
    load to keep the public type opaque (callers can't peek at
    EFI_HANDLE through it) and to give axl_image_unload a clear
    free target.
**/

#include "../backend/axl-backend.h"
#include "axl-image-internal.h"
#include "../fv/axl-fv-internal.h"
#include <axl/axl-image.h>
#include <axl/axl-driver.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-sys.h>     /* gImageHandle pulled in for self lookup */

AXL_LOG_DOMAIN("image");

struct AxlImage {
    AxlDriverHandle  handle;   /* underlying EFI_HANDLE, opaque */
};

int
axl_image_load(
    const char  *path,
    AxlImage   **out
    )
{
    if (path == NULL || out == NULL) {
        return AXL_ERR;
    }
    *out = NULL;

    AxlDriverHandle h = NULL;
    if (axl_driver_load(path, &h) != AXL_OK || h == NULL) {
        return AXL_ERR;
    }

    AxlImage *img = axl_malloc(sizeof(*img));
    if (img == NULL) {
        axl_driver_unload(h);
        return AXL_ERR;
    }
    img->handle = h;
    *out = img;
    return AXL_OK;
}

int
axl_image_set_load_options(
    AxlImage    *img,
    const void  *data,
    size_t       size
    )
{
    if (img == NULL || img->handle == NULL) {
        return AXL_ERR;
    }
    /* Underlying driver handle owns the tracking-table slot; the
       copy is released by axl_image_unload → axl_driver_unload. */
    return axl_driver_set_load_options(img->handle, data, size);
}

int
axl_image_start(
    AxlImage  *img,
    int       *exit_code
    )
{
    if (img == NULL || img->handle == NULL) {
        return AXL_ERR;
    }

    UINTN       exit_data_size = 0;
    EFI_STATUS  status = axl_bs()->StartImage(
        (EFI_HANDLE)img->handle,
        &exit_data_size,
        NULL);

    if (exit_code != NULL) {
        /* EFI_SUCCESS == 0 round-trips cleanly. For images that call
           Exit(EFI_SUCCESS, ...) we get 0; for images that return a
           non-zero EFI_STATUS, the truncation to int loses the
           high-bit error marker — DEVICE_ERROR (0x8000000000000007)
           reports as 7, indistinguishable from a deliberate
           Exit(7, ...). That ambiguity is inherent in the UEFI
           API: Exit() and a thrown error use the same channel.
           Callers that need to disambiguate should treat any
           non-zero value as "image did not succeed" without
           relying on the specific error code. */
        *exit_code = (int)(intptr_t)status;
    }

    /* StartImage returning EFI_ERROR is normal — it means the image
       returned a non-success status. We treat that as a successful
       start with a non-zero exit code (reported above). The only
       hard failure is when StartImage couldn't transfer control at
       all; UEFI doesn't distinguish those cleanly, but in practice
       those failures manifest as crashes that don't return here. */
    return AXL_OK;
}

int
axl_image_unload(
    AxlImage  *img
    )
{
    if (img == NULL) {
        return AXL_OK;
    }
    int rc = AXL_OK;
    if (img->handle != NULL) {
        rc = axl_driver_unload(img->handle);
    }
    axl_free(img);
    return rc;
}

/* Install @p args as UCS-2 LoadOptions, StartImage (blocks), and unload —
   the shared tail of axl_image_run and axl_image_run_fv_file. @p img is
   always consumed (unloaded) regardless of outcome. */
static int
image_run_and_unload(
    AxlImage    *img,
    const char  *args,
    int         *out_exit_code
    )
{
    /* set_load_options copies the buffer, so the temporary is freed
       immediately (NUL-terminated, sized in bytes — the UEFI command-line
       convention). */
    if (args != NULL && args[0] != '\0') {
        size_t          n = axl_strlen(args);
        unsigned short *w = axl_malloc((n + 1) * sizeof(unsigned short));
        if (w != NULL) {
            size_t chars = axl_utf8_to_ucs2_buf(args, w, n + 1);
            axl_image_set_load_options(img, w,
                                       (chars + 1) * sizeof(unsigned short));
            axl_free(w);
        }
    }

    int rc = axl_image_start(img, out_exit_code);   /* blocks */
    axl_image_unload(img);
    return rc;
}

int
axl_image_run(
    const char *path,
    const char *args,
    int        *out_exit_code
    )
{
    if (out_exit_code != NULL) {
        *out_exit_code = 0;
    }
    if (path == NULL) {
        return AXL_ERR;
    }

    AxlImage *img = NULL;
    if (axl_image_load(path, &img) != AXL_OK || img == NULL) {
        return AXL_ERR;
    }
    return image_run_and_unload(img, args, out_exit_code);
}

/* Build an FvFile device path: the FV handle's own device path with its
   END node replaced by a MEDIA_PIWG_FW_FILE node carrying @p name_guid,
   then a fresh END node. Suitable for gBS->LoadImage's DevicePath — the
   firmware reads the file's PE32 section straight out of the FV.

   Returns a gBS->AllocatePool buffer (caller FreePools), or NULL on any
   failure (handle has no device path, malformed, OOM). */
static EFI_DEVICE_PATH_PROTOCOL *
fv_file_build_dp(
    AxlHandle       fv_handle,
    const AxlGuid  *name_guid
    )
{
    EFI_DEVICE_PATH_PROTOCOL *fv_dp = NULL;
    if (axl_bs()->HandleProtocol((EFI_HANDLE)fv_handle,
                                 &EFI_DEVICE_PATH_PROTOCOL_GUID,
                                 (void **)&fv_dp) != EFI_SUCCESS
        || fv_dp == NULL)
    {
        return NULL;
    }
    size_t fv_dp_size = axl_device_path_size(fv_dp);
    if (fv_dp_size < 4) {
        return NULL;
    }
    size_t fv_dp_body = fv_dp_size - 4;   /* strip the trailing END node */

    /* FW_FILE node (4-byte header + 16-byte GUID) + 4-byte END node. */
    const size_t fw_node_size = 4 + sizeof(AxlGuid);
    size_t       total        = fv_dp_body + fw_node_size + 4;

    void *out = NULL;
    if (axl_bs()->AllocatePool(EfiBootServicesData, total, &out) != EFI_SUCCESS  /* axl-pool-direct: device path for LoadImage */
        || out == NULL)
    {
        return NULL;
    }
    uint8_t *p = (uint8_t *)out;
    axl_memcpy(p, fv_dp, fv_dp_body);
    p += fv_dp_body;

    /* MEDIA_PIWG_FW_FILE_DP node: MEDIA_DEVICE_PATH(0x04) / 0x06. */
    p[0] = 0x04;
    p[1] = 0x06;
    p[2] = (uint8_t)(fw_node_size & 0xff);
    p[3] = (uint8_t)((fw_node_size >> 8) & 0xff);
    axl_memcpy(p + 4, name_guid, sizeof(AxlGuid));   /* EFI_GUID byte layout */
    p += fw_node_size;

    /* END node */
    p[0] = 0x7f; p[1] = 0xff; p[2] = 4; p[3] = 0;

    return (EFI_DEVICE_PATH_PROTOCOL *)out;
}

int
axl_image_run_fv_file(
    const AxlGuid  *name_guid,
    const char     *args,
    int            *out_exit_code
    )
{
    if (out_exit_code != NULL) {
        *out_exit_code = 0;
    }
    if (name_guid == NULL) {
        return AXL_ERR;
    }

    AxlHandle fv_handle = NULL;
    if (_axl_fv_find_app_file(name_guid, &fv_handle) != AXL_OK) {
        return AXL_ERR;   /* no readable FV carries the file */
    }

    EFI_DEVICE_PATH_PROTOCOL *dp = fv_file_build_dp(fv_handle, name_guid);
    if (dp == NULL) {
        return AXL_ERR;
    }

    EFI_HANDLE image  = NULL;
    EFI_STATUS status = axl_bs()->LoadImage(
        FALSE,           /* BootPolicy */
        gImageHandle,    /* ParentImageHandle */
        dp,              /* FvFile DevicePath */
        NULL, 0,         /* firmware reads the section from the FV */
        &image);
    axl_bs()->FreePool(dp);  /* axl-pool-direct: device path for LoadImage */

    if (EFI_ERROR(status) || image == NULL) {
        axl_warning("image_run_fv_file: LoadImage failed: 0x%llx",
                    (unsigned long long)status);
        return AXL_ERR;
    }

    /* Wrap the firmware EFI_HANDLE in an opaque AxlImage so the shared
       tail (set options + StartImage + UnloadImage) handles it exactly
       like a path-loaded image. */
    AxlImage *img = axl_malloc(sizeof(*img));
    if (img == NULL) {
        axl_bs()->UnloadImage(image);
        return AXL_ERR;
    }
    img->handle = (AxlDriverHandle)image;
    return image_run_and_unload(img, args, out_exit_code);
}

// ---------------------------------------------------------------------------
// Public — enumeration + self introspection
// ---------------------------------------------------------------------------

extern EFI_HANDLE gImageHandle;   /* set by the runtime at startup */

/* Fill @p out for the loaded image at @p handle. The decoded
   filepath is heap-allocated via _axl_decode_image_filepath /
   _axl_prepend_volume_mapping; ownership transfers into
   @p out_owned_path so the caller can free after the consumer
   callback returns. Returns AXL_OK / AXL_ERR. */
static int
fill_image_info(
    EFI_HANDLE     handle,
    AxlImageInfo  *out,
    char         **out_owned_path)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_GUID                   li_guid = gEfiLoadedImageProtocolGuid;
    EFI_STATUS                 status =
        axl_bs()->HandleProtocol(handle, &li_guid, (void **)&li);
    if (EFI_ERROR(status) || li == NULL || li->ImageBase == NULL) {
        return AXL_ERR;
    }

    out->base = li->ImageBase;
    out->size = (size_t)li->ImageSize;
    out->path = NULL;
    *out_owned_path = NULL;

    if (li->FilePath != NULL) {
        char *decoded = _axl_decode_image_filepath(
            (EFI_DEVICE_PATH_PROTOCOL *)li->FilePath);
        if (decoded != NULL && li->DeviceHandle != NULL) {
            char *with_volume =
                _axl_prepend_volume_mapping(li->DeviceHandle, decoded);
            if (with_volume != NULL) {
                axl_free(decoded);
                decoded = with_volume;
            }
        }
        *out_owned_path = decoded;
        out->path       = decoded;
    }
    return AXL_OK;
}

int
axl_image_enumerate(
    AxlImageIterFn  cb,
    void           *ctx)
{
    if (cb == NULL) {
        return AXL_ERR;
    }

    EFI_GUID    li_guid       = gEfiLoadedImageProtocolGuid;
    UINTN       handle_count  = 0;
    EFI_HANDLE *handles       = NULL;
    EFI_STATUS  status        = axl_bs()->LocateHandleBuffer(
        ByProtocol, &li_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || handles == NULL) {
        return AXL_ERR;
    }

    int rc = AXL_OK;
    for (UINTN i = 0; i < handle_count; i++) {
        AxlImageInfo info       = { 0 };
        char        *owned_path = NULL;
        if (fill_image_info(handles[i], &info, &owned_path) != AXL_OK) {
            continue;
        }
        int cb_rc = cb(&info, ctx);
        axl_free(owned_path);
        if (cb_rc != 0) {
            rc = cb_rc;
            break;
        }
    }

    axl_bs()->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    return rc;
}

int
axl_image_self_get_range(
    void   **out_base,
    size_t  *out_size)
{
    if (out_base == NULL) {
        return AXL_ERR;
    }
    *out_base = NULL;
    if (out_size != NULL) {
        *out_size = 0;
    }
    if (gImageHandle == NULL) {
        return AXL_ERR;
    }

    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_GUID                   li_guid = gEfiLoadedImageProtocolGuid;
    EFI_STATUS                 status =
        axl_bs()->HandleProtocol(gImageHandle, &li_guid, (void **)&li);
    if (EFI_ERROR(status) || li == NULL) {
        return AXL_ERR;
    }
    *out_base = li->ImageBase;
    if (out_size != NULL) {
        *out_size = (size_t)li->ImageSize;
    }
    return AXL_OK;
}
