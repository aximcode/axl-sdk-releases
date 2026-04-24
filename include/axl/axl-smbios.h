/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-smbios.h:
 *
 * UEFI SMBIOS helpers -- string extraction and table lookup.
 */

#ifndef AXL_SMBIOS_H
#define AXL_SMBIOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// SMBIOS table header (standard C types, matches SMBIOS spec layout).
typedef struct {
    uint8_t   Type;
    uint8_t   Length;
    uint16_t  Handle;
} AxlSmbiosHeader;

/**
 * @brief Get a string from an SMBIOS table's string area (UCS-2).
 *
 * Returns a pointer to a static 128-char unsigned short buffer -- caller
 * must use the value before the next call (not reentrant).
 *
 * @return pointer to static unsigned short buffer.
 */
unsigned short *
axl_smbios_get_string(
    AxlSmbiosHeader  *hdr,           ///< SMBIOS table header
    unsigned char     string_index   ///< 1-based string index (0 returns empty string)
);

/**
 * @brief Get a string from an SMBIOS table's string area (UTF-8).
 *
 * Returns a pointer to a static 128-char buffer -- caller must use
 * the value before the next call (not reentrant).
 *
 * @return pointer to static char buffer, or "" if not found.
 */
const char *
axl_smbios_get_string_utf8(
    AxlSmbiosHeader  *hdr,           ///< SMBIOS table header
    unsigned char     string_index   ///< 1-based string index (0 returns "")
);

/**
 * @brief Find the first SMBIOS table of a given type.
 *
 * @return pointer to table header, or NULL if not found.
 */
AxlSmbiosHeader *
axl_smbios_find(
    unsigned char type  ///< SMBIOS table type (e.g. 0 for BIOS, 1 for System)
);

/**
 * @brief Find the next SMBIOS table of a given type after @a prev.
 *
 * Pass NULL as @a prev to find the first (same as axl_smbios_find).
 * Use in a loop to enumerate all tables of a type:
 *
 * @code
 * AxlSmbiosHeader *h = NULL;
 * while ((h = axl_smbios_find_next(17, h)) != NULL) {
 *     // process each Type 17 (Memory Device) entry
 * }
 * @endcode
 *
 * @return pointer to next table header, or NULL if no more.
 */
AxlSmbiosHeader *
axl_smbios_find_next(
    unsigned char     type,  ///< SMBIOS table type
    AxlSmbiosHeader  *prev   ///< previous result (NULL to start from beginning)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SMBIOS_H */
