/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-protocol-internal.h
    Internal API shared between axl-protocol.c (which defines the
    name → vendor-GUID registry) and axl-sys.c (which uses the
    lookup to resolve protocol names for axl_handle_protocol).

    Not part of the public SDK surface — consumers go through
    `axl_handle_protocol` (declared in `<axl/axl-sys.h>`) or the
    `AxlVolume` accessors that wrap it.
**/

#ifndef AXL_PROTOCOL_INTERNAL_H
#define AXL_PROTOCOL_INTERNAL_H

#include "../backend/axl-backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve a protocol name to its vendor GUID, with fallback.
 *
 * Lookup priority:
 *   1. Built-in well-known table in `axl-protocol.c` (e.g. "smbios",
 *      "simple-fs", spec networking protocols).
 *   2. Caller-pinned table populated by `axl_protocol_register_name`
 *      (declared in `<axl/axl-sys.h>`).
 *   3. Deterministic FNV-1a fallback derived from the name string —
 *      written into @a fallback and returned, so the caller
 *      supplies the storage for unregistered custom names.
 *
 * @return pointer to the resolved GUID. Never NULL when @a name
 *     and @a fallback are non-NULL.
 */
const EFI_GUID *
axl_protocol_lookup_guid(
    const char *name,
    EFI_GUID   *fallback
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PROTOCOL_INTERNAL_H */
