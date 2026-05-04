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
#include <axl/axl-image.h>
#include <axl/axl-driver.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

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
