/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-driver-internal.h — cross-TU internals shared between axl-driver.c
 * and axl-shared-driver.c. Not part of the public API.
 *
 * Threads an AxlEmbeddedImageInfo through the embedded-driver load path
 * without breaking the public axl_driver_ensure_with_embedded signature:
 * the public function calls this with info == NULL.
 */

#ifndef AXL_DRIVER_INTERNAL_H
#define AXL_DRIVER_INTERNAL_H

#include <stddef.h>

#include <axl/axl-driver.h>   /* AxlEmbeddedImageInfo, AxlGuid */

/*
 * Identity-aware engine behind axl_driver_ensure_with_embedded and
 * axl_shared_driver_locate_with_image_info. When the driver is loaded from
 * the embedded blob, @p info (and the driver filename as the default leaf
 * name) gives the loaded image a non-NULL device path. @p info may be NULL.
 */
int
_axl_driver_ensure_with_embedded_info(
    const AxlGuid              *protocol_guid,
    const char                 *driver_name,
    const unsigned char        *embedded_buf,
    size_t                      embedded_len,
    const char                 *override_name,
    const void                 *load_options,
    size_t                      load_options_size,
    const AxlEmbeddedImageInfo *info);

#endif /* AXL_DRIVER_INTERNAL_H */
