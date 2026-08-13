/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-runtime.h
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

#include <stdbool.h>
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

// ---------------------------------------------------------------------------
// Task priority level (TPL) balance
// ---------------------------------------------------------------------------

/**
 * @name Task priority levels
 *
 * The four levels UEFI defines, as plain unsigned values so the public
 * API carries no firmware types. Higher masks more: at
 * #AXL_TPL_HIGH_LEVEL only calls that touch neither the pool allocator
 * nor the event queue still work — `Stall` and `CheckEvent` do, while
 * `AllocatePool`, `CreateEvent`, `SetTimer`, `CloseEvent` and console
 * output all HANG rather than returning an error. Raising is a
 * commitment to restore.
 * @{
 */
#define AXL_TPL_APPLICATION  4u   ///< normal foreground execution
#define AXL_TPL_CALLBACK     8u   ///< event-notify / driver-pump dispatch
#define AXL_TPL_NOTIFY      16u   ///< highest level pool allocation is legal
#define AXL_TPL_HIGH_LEVEL  31u   ///< interrupts masked; almost nothing works
/** @} */

/**
 * @brief The task priority level the caller is executing at.
 *
 * One of the `AXL_TPL_*` values (the firmware may also report an
 * intermediate level; treat the result as an ordered magnitude, not an
 * enum). Cheap — one raise/restore pair, because UEFI offers no direct
 * read.
 *
 * @return the current level; #AXL_TPL_APPLICATION in a normal
 *     foreground path.
 */
unsigned
axl_tpl_current(void);

/**
 * @brief Raise the task priority level, returning the level to restore.
 *
 * Brackets a short critical section: while raised, nothing at or below
 * @a level can preempt. Pair STRICTLY LIFO with @ref axl_tpl_restore on
 * the same call stack, and keep the section short — no blocking, no I/O.
 *
 * Do not raise above #AXL_TPL_NOTIFY unless the body genuinely touches
 * neither the allocator nor the event queue: at #AXL_TPL_HIGH_LEVEL a
 * pool allocation or a console write does not fail, it HANGS.
 *
 * Lowering is not possible through this call — asking for a level below
 * the current one leaves the level unchanged and returns it, so the
 * paired @ref axl_tpl_restore is still correct.
 *
 * @return the previous level, to hand to @ref axl_tpl_restore.
 */
unsigned
axl_tpl_raise(
    unsigned level   ///< level to raise to, e.g. #AXL_TPL_NOTIFY
);

/**
 * @brief Restore the level returned by @ref axl_tpl_raise.
 *
 * Must be called on the same call stack, LIFO, exactly once per raise.
 * A raise whose restore is skipped is fatal — see
 * @ref axl_tpl_restore_baseline for what that costs and how AXL
 * contains it.
 */
void
axl_tpl_restore(
    unsigned previous   ///< the value @ref axl_tpl_raise returned
);

/**
 * @brief Restore the task priority level to the application baseline.
 *
 * A raise that is never restored is unrecoverable, not merely untidy:
 * returning to the shell above #AXL_TPL_APPLICATION wedges the machine
 * — measured at #AXL_TPL_CALLBACK as well as #AXL_TPL_NOTIFY, so every
 * raised level is fatal. On AArch64 the firmware names it
 * (`ASSERT Image->Tpl == gEfiCurrentTpl`) and then deadloops; on x64 a
 * release build says nothing at all and simply spins.
 *
 * AXL calls this first thing in `_axl_cleanup`, before any console or
 * allocator use, so an app that leaks a raise is REPAIRED and told
 * about it rather than hanging.
 *
 * **The baseline is #AXL_TPL_APPLICATION, always** — this is for code
 * that knows it started at the application level, which means an app,
 * not a callback. Do NOT call it from a notify function or a driver
 * tick: the firmware legitimately entered those at #AXL_TPL_CALLBACK or
 * #AXL_TPL_NOTIFY, so this would un-nest the firmware's own raise and
 * report a defect that is not there. To bound a suspect region inside a
 * callback, capture @ref axl_tpl_current on entry and compare against it
 * on exit, restoring with @ref axl_tpl_restore.
 *
 * This lowers the level; it never raises. If the caller is already at
 * baseline it does nothing and reports false.
 *
 * @return true if the level was above baseline and has been restored,
 *     false if it was already at #AXL_TPL_APPLICATION or below.
 */
bool
axl_tpl_restore_baseline(
    unsigned *out_leaked   ///< [out] optional; the level found, untouched when false
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RUNTIME_H */
