/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-runtime-internal.h
    Internal hooks into the runtime's own state (src/runtime/axl-runtime.c).
    Not a public header.
**/

#ifndef AXL_RUNTIME_INTERNAL_H
#define AXL_RUNTIME_INTERNAL_H

/** Free the image's default loop (axl_loop_default), if one was created.
 *  Called on the way out of either image kind -- by _axl_cleanup for an app
 *  and by axl_driver_cleanup for a driver -- before the registry sweep, so
 *  the loop's registry entry unregisters instead of reading as a leak.
 *  Idempotent: the next axl_loop_default() creates a fresh loop. */
void _axl_default_loop_free(void);

/** Called by axl_exit with its rc, BEFORE _axl_cleanup: records whether this
 *  exit makes the firmware unload a driver image -- inside an AXL macro's
 *  DriverEntry, with a status that is an EFI error -- so _axl_cleanup tears
 *  the image down as the macro's own error return would. */
void _axl_cleanup_note_exit(int rc);

/** Called by axl_exit AFTER _axl_cleanup, the last thing before gBS->Exit.
 *  For an image that owns its teardown (an app, or a driver this exit
 *  unloads): releases the C heap -- again, harmlessly, unless this is a
 *  nested exit from inside the first one's atexit drain, which never
 *  returns to that cleanup's own release -- and, for the driver, re-arms
 *  the status the decision was taken on, so nothing armed during the
 *  teardown can turn it into a resident-driver exit. */
void _axl_cleanup_finish_exit(void);

#endif /* AXL_RUNTIME_INTERNAL_H */
