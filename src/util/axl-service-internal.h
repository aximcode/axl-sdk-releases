/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-service-internal.h
    Internal API shared between axl-service.c (which defines the
    name → vendor-GUID registry) and axl-sys.c (which uses the
    lookup to resolve protocol names for axl_handle_protocol).

    Not part of the public SDK surface — consumers go through
    `axl_handle_protocol` (declared in `<axl/axl-sys.h>`) or the
    `AxlVolume` accessors that wrap it.
**/

#ifndef AXL_SERVICE_INTERNAL_H
#define AXL_SERVICE_INTERNAL_H

#include "../backend/axl-backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve a service name to its vendor GUID, with fallback.
 *
 * Looks up @a name in the registry built up by
 * `axl_service_register_namespace` (defined in axl-service.c).
 * On miss, returns @a fallback unchanged so the caller can supply
 * a default per-call.
 *
 * @return pointer to the resolved GUID, or @a fallback if no
 *     registered name matches. Never returns NULL when @a fallback
 *     is non-NULL.
 */
const EFI_GUID *
axl_service_lookup_guid(
    const char *name,
    EFI_GUID   *fallback
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SERVICE_INTERNAL_H */
