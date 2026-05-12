/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-runtime.h:
 *
 * AXL runtime surface — the pieces an app interacts with around its
 * own lifecycle and interruptibility. The runtime is the library
 * code in src/runtime/ that powers the program lifecycle; the CRT0
 * entry stub (src/crt0/axl-crt0-native.c) invokes it at two
 * boundaries:
 *   - _axl_init (before main): default loop singleton, shell-break
 *     notify, tier-1 resource registry, atexit registry.
 *   - _axl_cleanup (after main, or on axl_exit): drain atexit,
 *     sweep tier-1 leaks, free the default loop if created.
 *
 * Full design and the runtime-vs-CRT0 split: docs/AXL-Lifecycle.md.
 *
 * Related headers:
 *   - axl-signal.h   Ctrl-C / interrupt handler API + axl_exit
 *   - axl-atexit.h   POSIX-flavored cleanup callback registry
 *   - axl-loop.h     Loop primitives, including axl_loop_iterate_until
 */

#ifndef AXL_RUNTIME_H
#define AXL_RUNTIME_H

#include <stddef.h>

#include <axl/axl-sys.h>   /* AxlGuid for axl_efi_find_config_table */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;

// ---------------------------------------------------------------------------
// Default loop
// ---------------------------------------------------------------------------

/**
 * @brief Return the runtime's default loop, lazy-creating on first call.
 *
 * The default loop is owned by the runtime and freed during
 * _axl_cleanup. Apps can run it directly (axl_loop_run(axl_loop_default())),
 * add their own sources to it, or ignore it entirely and create
 * private loops. axl_yield() dispatches immediately-ready work on
 * this loop opportunistically.
 *
 * @return the default loop, or NULL if allocation failed.
 */
AxlLoop *
axl_loop_default(void);

// ---------------------------------------------------------------------------
// Yield
// ---------------------------------------------------------------------------

/**
 * @brief Cooperative yield point.
 *
 * Call inside CPU-bound loops to keep the app interruptible. Cost is
 * ~nanoseconds when the default loop is idle. Safe from any context
 * except a raised-TPL notify handler.
 *
 * Behavior:
 *   1. If any immediately-ready work is pending on the default loop
 *      (timers, deferred callbacks), dispatch it — bounded to one
 *      iteration.
 *   2. If Ctrl-C was observed during that dispatch, sets the
 *      interrupted flag so axl_interrupted() returns true.
 *   3. Otherwise returns immediately.
 */
void
axl_yield(void);

// ---------------------------------------------------------------------------
// UEFI System Configuration Table accessor
// ---------------------------------------------------------------------------

/**
 * @brief Look up a UEFI Configuration Table entry by VendorGuid.
 *
 * Walks the EFI System Table's ConfigurationTable for an entry
 * whose VendorGuid matches @p guid. Returns the corresponding
 * VendorTable pointer (typed `void *` — caller casts to the
 * spec-defined struct type for the GUID).
 *
 * Common GUIDs and their published types:
 *   - `EFI_ACPI_20_TABLE_GUID`           → RSDP
 *   - `SMBIOS3_TABLE_GUID`               → SMBIOS3 entry-point struct
 *   - `EFI_SYSTEM_RESOURCE_TABLE_GUID`   → ESRT
 *   - `EFI_DEBUG_IMAGE_INFO_TABLE_GUID`  → debug image info
 *
 * Modules with their own typed lookups (axl-acpi, axl-smbios) call
 * this internally. New code that needs a one-shot lookup of an
 * uncommon table (ESRT, MEMATTR, dmar, etc.) should use this
 * directly instead of duplicating the configuration-table walk.
 *
 * @return the matching VendorTable, or NULL if no match or @p guid
 *     is NULL.
 */
void *
axl_efi_find_config_table(
    const AxlGuid *guid    ///< guid to match (NULL → returns NULL)
);

// ---------------------------------------------------------------------------
// Registry inspection (debug)
// ---------------------------------------------------------------------------

/**
 * @brief Return the number of tier-1 resources currently registered.
 *
 * Purely informational — mostly useful in tests to verify
 * resource-balancing. Returns 0 if the registry has not been
 * initialized yet.
 */
size_t
axl_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RUNTIME_H */
