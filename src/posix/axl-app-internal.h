/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-app-internal.h
    Internal handshake between src/posix/axl-app.c (POSIX shim: argv
    from UEFI shell parameters) and src/runtime/axl-runtime.c (glue
    called by CRT0). Not a public header.
**/

#ifndef AXL_APP_INTERNAL_H
#define AXL_APP_INTERNAL_H

/** Build argc/argv from the image's EFI_SHELL_PARAMETERS_PROTOCOL
 *  (or fall back to argc=1/argv[0]="app" if no shell is attached).
 *  Called once from _axl_init. */
void _axl_args_init(void *image_handle);

/** Free the argv strings and array allocated by _axl_args_init.
 *  Called once from _axl_cleanup. */
void _axl_args_free(void);

#endif /* AXL_APP_INTERNAL_H */
