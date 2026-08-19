/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-atexit-internal.h
    Internal hooks that drive the atexit registry. Not a public header.

    TWO callers, one per image kind, and they must stay in step:
    src/runtime/axl-runtime.c (_axl_init / _axl_cleanup) for an app,
    and src/util/axl-driver.c (axl_driver_init / axl_driver_cleanup)
    for a driver. C++ static destructors register here through
    __cxa_atexit, and axl_atexit REFUSES registration while the table
    is NULL -- so an entry path that runs .init_array without calling
    _axl_atexit_init first drops every destructor silently.
**/

#ifndef AXL_ATEXIT_INTERNAL_H
#define AXL_ATEXIT_INTERNAL_H

#include <stdbool.h>

/** Called once before any user code runs -- by _axl_init for an app,
 *  by axl_driver_init for a driver. Idempotent. */
void _axl_atexit_init(void);

/** Called once on the way out -- by _axl_cleanup (before the
 *  resource-registry sweep) for an app, by axl_driver_cleanup for a
 *  driver. Walks live callbacks in descending-seq (LIFO) order, then
 *  frees the table and NULLs it, which is what makes a second call a
 *  no-op and a driver reload start from a clean table. */
void _axl_atexit_run_all(void);

/** Mark this image as a DRIVER, so `_axl_cleanup` leaves the atexit table
 *  to `axl_driver_cleanup`.
 *
 *  `_axl_poll_break` calls `axl_exit(1)` on a shell break when no handler is
 *  installed, and it is reachable from ordinary library work a driver does
 *  (axl-fs, axl-http-client, axl-digest, axl-sort). That lands in
 *  `_axl_cleanup`, which drains this table -- a guaranteed no-op in a driver
 *  until axl_driver_init started populating it, and now every global
 *  destructor. `gBS->Exit` then FAILS for a non-current image and spins, so
 *  the image keeps running with destructed globals. The wedge predates this;
 *  tearing the image's C++ state down first does not.
 *
 *  Set by `axl_driver_init`. Never cleared: an image is one kind for life. */
void _axl_atexit_mark_driver_image(void);

/** True when `_axl_atexit_mark_driver_image` has been called. */
bool _axl_atexit_is_driver_image(void);

#endif /* AXL_ATEXIT_INTERNAL_H */
