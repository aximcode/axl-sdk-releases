/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-sidecar-internal.h
    Library-internal scaffolding for sidecar consumers within axl-sdk.

    Shared between `src/pci/axl-pci-ids.c`, `src/pci/axl-pci-class.c`,
    `src/spd/axl-spd-ids.c`, and the future `src/usb/axl-usb-ids.c`.
    Not part of the public API — consumers writing their own sidecar
    loaders should use the public `axl-sidecar.h` primitives directly
    and roll their own typed singleton wrappers (the function-pointer
    surface here is awkward without C++ generics).
**/

#ifndef AXL_SIDECAR_INTERNAL_H
#define AXL_SIDECAR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-hash-table.h>
#include <axl/axl-sidecar.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Singleton lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Open callback for the singleton load helper.
 *
 * Wraps the module's own typed `axl_*_open(path, &handle)` so the
 * helper can call it through a generic pointer slot.
 */
typedef AxlSidecarStatus (*AxlSidecarOpenFn)(const char  *path,
                                             void       **handle);

/**
 * @brief Close callback for the singleton load helper.
 *
 * Wraps the module's own typed `axl_*_close(handle)`.
 */
typedef void (*AxlSidecarCloseFn)(void *handle);

/**
 * @brief Singleton load with idempotent guard, override-or-autodiscover
 *     dispatch, and atexit cleanup registration.
 *
 * Behaviour mirrors today's `axl_pci_ids_load` exactly. Consumers
 * own three `static` slots:
 *
 * @code
 *   static void     *g_handle;
 *   static uint32_t  g_atexit;
 *   static void     *g_atexit_ctx;     // opaque; helper owns the lifetime
 *
 *   static AxlSidecarStatus open_thunk(const char *path, void **out) {
 *       return axl_foo_ids_open(path, (AxlFooIds **)out);
 *   }
 *   static void close_thunk(void *h) {
 *       axl_foo_ids_close((AxlFooIds *)h);
 *   }
 *
 *   AxlSidecarStatus axl_foo_ids_load(const char *override_path) {
 *       return _axl_sidecar_singleton_load(&g_handle, &g_atexit,
 *                                          &g_atexit_ctx,
 *                                          override_path,
 *                                          "foo-ids.json5",
 *                                          open_thunk, close_thunk);
 *   }
 * @endcode
 *
 * On the first successful load the helper allocates a small thunk
 * context, registers @p close_fn via @ref axl_atexit, and stashes
 * the context pointer in @p *atexit_ctx_slot. @ref
 * _axl_sidecar_singleton_free uses that slot to free the context
 * when the consumer drops the handle before process exit (otherwise
 * the atexit thunk frees it during shutdown).
 *
 * @return @c AXL_SIDECAR_OK on a successful load (idempotent — a
 *     second call on an already-loaded slot is also OK without
 *     re-opening), the @p open_fn return on first-load failure.
 */
AxlSidecarStatus
_axl_sidecar_singleton_load(
    void              **handle_slot,
    uint32_t           *atexit_slot,
    void              **atexit_ctx_slot,
    const char         *override_path,
    const char         *autodiscover_name,
    AxlSidecarOpenFn    open_fn,
    AxlSidecarCloseFn   close_fn
);

/**
 * @brief Singleton free + atexit deregistration + ctx reclaim.
 *
 * NULL-safe. After return, *handle_slot == NULL, *atexit_slot == 0,
 * and *atexit_ctx_slot == NULL — subsequent _load calls re-open
 * cleanly with no residual heap allocation.
 */
void
_axl_sidecar_singleton_free(
    void              **handle_slot,
    uint32_t           *atexit_slot,
    void              **atexit_ctx_slot,
    AxlSidecarCloseFn   close_fn
);

// ---------------------------------------------------------------------------
// Hash-table foreach with early-stop
// ---------------------------------------------------------------------------

/**
 * @brief Per-entry callback for @ref _axl_sidecar_foreach.
 *
 * @return 0 to continue, non-zero to stop iteration. The non-zero
 *     value propagates as the foreach return — same convention as
 *     the public `axl_pci_ids_foreach_*` family.
 */
typedef int (*AxlSidecarEntryFn)(const void  *key,
                                 void        *value,
                                 void        *ctx);

/**
 * @brief Walk every entry in @p t with an early-stop signal.
 *
 * Replaces the three near-identical Vendor/Device/Subsys trampolines
 * AxlPciIds carries today. The hash-table walk continues to
 * completion (every entry visited internally), but @p fn is called
 * only until it returns non-zero — the wasted hash iterations are
 * negligible at the scale axl-sdk sidecars run (hundreds of entries
 * tops).
 *
 * @return 0 if @p fn never stopped the walk, the first non-zero
 *     return otherwise, or -1 if @p t or @p fn is NULL.
 */
int
_axl_sidecar_foreach(
    AxlHashTable       *t,
    AxlSidecarEntryFn   fn,
    void               *ctx
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SIDECAR_INTERNAL_H */
