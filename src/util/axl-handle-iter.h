/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-handle-iter.h
    Internal helper: cursor over the handles publishing a given UEFI
    protocol GUID.

    Hoisted from `axl-block.c`, `axl-serial.c`, and `axl-fv.c`, which
    carried line-for-line copies of the same "LocateHandleBuffer once,
    cache for the image lifetime, recover the iteration position from
    the handle the caller passes back" enumeration. Each reader keeps
    its own typed getters (the genuinely module-specific concern) and
    drives the cursor through one shared engine.

    The cursor returns the firmware `AxlHandle` directly, so it carries
    no hidden shared state: passing NULL — or any handle not in the
    cached set — restarts at the first handle, and independent (even
    nested) walks over the cached set do not interfere. The cached
    buffer is freed at exit.

    Internal-only: consumers reach this through `axl_block_next`,
    `axl_serial_next`, `axl_fv_next`, etc.
**/

#ifndef AXL_HANDLE_ITER_H
#define AXL_HANDLE_ITER_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-sys.h>   /* AxlHandle */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-protocol enumeration cursor state.
 *
 * Each reader declares one of these as a file-scope static, pinning
 * `guid` and `what` at definition; the rest is filled lazily on the
 * first `axl_handle_iter_next` call and stays cached for the image
 * lifetime. `guid` is an `EFI_GUID *` (kept as `void *` so this
 * internal header stays free of UEFI types); `handles` is the cached
 * `EFI_HANDLE[]` from LocateHandleBuffer.
 */
typedef struct {
    const void  *guid;     ///< EFI_GUID * for the protocol to enumerate (set at init)
    const char  *what;     ///< short label for debug logging, e.g. "block device" (set at init)
    bool         inited;   ///< handle set has been located (even if empty)
    void       **handles;  ///< cached EFI_HANDLE[] (NULL when none)
    size_t       count;    ///< number of cached handles
} AxlHandleIter;

/**
 * @brief Advance a protocol-handle cursor.
 *
 * Locates and caches the handle set on first use. Pass NULL to get
 * the first handle, then pass each returned handle back to get the
 * next; returns NULL at end of enumeration (including when none
 * exist). A handle not in the cached set restarts at the first.
 *
 * @return next handle, or NULL at end of enumeration.
 */
AxlHandle
axl_handle_iter_next(
    AxlHandleIter *it,     ///< cursor state (file-scope static per reader)
    AxlHandle      prev    ///< previous handle, or NULL to start
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HANDLE_ITER_H */
