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

#include <stdbool.h>
#include <stddef.h>

#include <axl/axl-driver.h>   /* AxlEmbeddedImageInfo, AxlGuid */

/*
 * axl_driver_cleanup WITHOUT its last step, giving newlib's arena back to the
 * firmware. For a teardown that still has work to do afterwards that may
 * malloc or free (the service unload stub's registry sweep), which must then
 * call _axl_libc_heap_release itself, last. See axl-libc-heap-internal.h.
 */
void
_axl_driver_cleanup_keep_heap(void);

/*
 * Identity-aware engine behind axl_driver_ensure_with_embedded and
 * axl_shared_driver_locate_with_image_info. When the driver is loaded from
 * the embedded blob, @p info (and the driver filename as the default leaf
 * name) gives the loaded image a non-NULL device path. @p info may be NULL.
 * @p api is the public call it serves, named in a TPL refusal's log line.
 */
int
_axl_driver_ensure_with_embedded_info(
    const char                 *api,
    const AxlGuid              *protocol_guid,
    const char                 *driver_name,
    const unsigned char        *embedded_buf,
    size_t                      embedded_len,
    const char                 *override_name,
    const void                 *load_options,
    size_t                      load_options_size,
    const AxlEmbeddedImageInfo *info);

/*
 * UEFI 2.11 Table 7.3: LoadImage() and StartImage() only below TPL_CALLBACK.
 * True -- and one error line naming @p api and the rule -- when the caller
 * runs at TPL_CALLBACK or above: a loop callback, a timer, any event notify.
 * Every public path that reaches LoadImage or StartImage asks this first and
 * returns AXL_UNSUPPORTED on true, before it touches the firmware.
 *
 * Not just the letter of the spec: EDK2's CoreFreePages applies the NX policy
 * to freed pages only AFTER releasing its lock, which dispatches pending
 * callbacks; an image loaded in one of them can land in pages an exiting
 * application is giving back, and then be made execute-never under it.
 */
bool
_axl_image_tpl_refused(
    const char *api);

/*
 * Load @p len bytes of an image as if read from the file @p path: LoadImage
 * gets the file's device path, so the image's FilePath/DeviceHandle are
 * @p path's even though the bytes came from elsewhere (a staged candidate
 * runs with the identity of the image it may replace). A packed image is
 * unpacked first. The caller has checked the TPL. AXL_OK with @p *out set,
 * else AXL_ERR.
 */
int
_axl_driver_load_buffer_at(
    const char      *path,
    const void      *buf,
    size_t           len,
    AxlDriverHandle *out);

/*
 * axl_driver_load without the refusal of the running image's own file: for
 * the APPLICATION loaders (axl_image_load / _run), where loading one's own
 * path -- a self-update probing the file it staged over itself, a re-exec --
 * is the caller's explicit choice.
 */
int
_axl_driver_load_self_ok(
    const char       *path,
    AxlDriverHandle  *handle);

#endif /* AXL_DRIVER_INTERNAL_H */
