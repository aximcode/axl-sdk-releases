/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-uefi.h
    Umbrella header for AXL native UEFI type definitions.

    Provides all UEFI types, status codes, system table structs,
    protocol definitions, and GUIDs needed by the AXL native backend.
    Self-contained — depends only on <stdint.h> and <stddef.h>.

    Self-contained — depends only on <stdint.h> and <stddef.h>.
**/

#ifndef AXL_UEFI_H
#define AXL_UEFI_H

/* APPLICATIONS MAY NOT INCLUDE THIS.
 *
 * AXL's contract is that consumers write standard C against axl_*: <axl.h> is
 * uefi-free on purpose and never pulls this header in (see its own note on
 * AxlEfiStatus / AxlHandle). But `uefi/` ships inside include/axl-sdk/, which
 * axl-cc puts on -isystem, so "opt in explicitly" meant nothing stronger than
 * "type the #include" -- and an app that did so silently acquired the whole
 * EDK2 surface the library exists to hide.
 *
 * AXL_ALLOW_UEFI makes the opt-in real rather than nominal:
 *
 *   - the library and its tools define it (they ARE the backend);
 *   - `axl-cc --type driver` defines it, because producing or interposing on
 *     a protocol means implementing that protocol's own types -- a shim
 *     cannot be written without them;
 *   - `axl-cc --allow-uefi` defines it for an application that genuinely
 *     needs raw firmware access, which is then visible on the build line
 *     rather than buried in an include;
 *   - a plain application gets the error below.
 *
 * Reach for an axl_* API first. If none covers what you need, that gap is
 * worth reporting -- it is more useful than the workaround.
 */
#if !defined(AXL_ALLOW_UEFI)
#  error "<uefi/axl-uefi.h> is not available to applications. Use the axl_* API; build a driver with `axl-cc --type driver` (CMake: axl_add_driver), or pass `--allow-uefi` (CMake: ALLOW_UEFI) to opt in deliberately."
#endif

#include "generated/all.h"
#include "axl-uefi-extra.h"

#endif /* AXL_UEFI_H */
