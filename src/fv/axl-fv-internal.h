/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fv-internal.h
    Internal FV helpers shared inside the library (not part of the
    public surface). Today: locate a firmware-embedded application by
    its FFS file name GUID, used by axl-image's FV-file loader to back
    axl_image_run_fv_file / axl_shell_launch_fv.
**/

#ifndef AXL_FV_INTERNAL_H
#define AXL_FV_INTERNAL_H

#include <axl/axl-sys.h>   /* AxlGuid, AxlHandle */

/**
 * Find a readable Firmware Volume carrying an EFI_FV_FILETYPE_APPLICATION
 * whose FFS file name GUID equals @p name_guid. All FV2 handles are
 * scanned (cached set, via axl_fv_next); volumes without EFI_FV2_READ_STATUS
 * are skipped. The first match wins.
 *
 * @return AXL_OK with the FV handle in *out_fv on a match; AXL_ERR if
 *     @p name_guid or @p out_fv is NULL, or no readable FV carries the file.
 */
int
_axl_fv_find_app_file(
    const AxlGuid  *name_guid,
    AxlHandle      *out_fv
);

#endif /* AXL_FV_INTERNAL_H */
