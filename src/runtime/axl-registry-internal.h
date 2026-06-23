/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-registry-internal.h
    Tier-1 resource registry — internal API called by the wrappers
    for AxlEvent, AxlLoop, AxlCancellable, and AxlArena. See
    docs/AXL-Lifecycle.md §4.2 for the design.

    Each registered resource gets a handle (slot index + 1). The _free
    path unregisters by handle; a final sweep at _axl_cleanup catches
    anything still registered (a leak) and closes it before gBS->Exit
    returns control to the shell.

    Not a public header — never include from outside src/.
**/

#ifndef AXL_REGISTRY_INTERNAL_H
#define AXL_REGISTRY_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AXL_RES_EVENT,
    AXL_RES_LOOP,
    AXL_RES_CANCELLABLE,
    AXL_RES_ARENA,
} AxlResKind;

/** Called once by _axl_init before any user code runs. Allocates the
 *  backing AxlArray. NULL-safe to call twice (second call is a no-op). */
void _axl_registry_init(void);

/** Called once by _axl_cleanup after atexit callbacks run. Walks live
 *  entries in descending-seq (LIFO) order, logs each leak with
 *  file:line, and calls the kind-appropriate _free. Releases the
 *  backing AxlArray on the way out. */
void _axl_registry_sweep(void);

/** Register a resource. Returns a non-zero handle on success, 0 on
 *  failure (registry not initialized, allocation failure). Caller
 *  stores the handle in the resource struct for later remove.
 *
 *  @p dtor is the resource's free function (a thin wrapper over
 *  axl_loop_free / axl_event_free / etc.). Storing it per entry —
 *  rather than switching on @p kind in the sweep — keeps the
 *  always-linked registry from statically referencing those frees, so a
 *  --gc-sections build of an app that never creates that resource type
 *  drops the whole subsystem. @p kind is retained only for the leak
 *  diagnostic name. */
uint32_t _axl_registry_add(
    AxlResKind   kind,
    void        *resource,
    void       (*dtor)(void *resource),
    const char  *file,
    int          line
);

/** Remove a resource by handle. NULL-safe: handle==0 is accepted
 *  (no-op). Idempotent: double-remove is safe. */
void _axl_registry_remove(uint32_t handle);

/** Current count of live entries. Public surface via axl_registry_count. */
size_t _axl_registry_size(void);

#endif /* AXL_REGISTRY_INTERNAL_H */
