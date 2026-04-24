/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbios.c
    SMBIOS helpers -- string extraction and table lookup.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-smbios.h>

AXL_LOG_DOMAIN("smbios");

// ---------------------------------------------------------------------------
// Internal: locate SMBIOS table range
// ---------------------------------------------------------------------------

static int
smbios_get_range(
    uint8_t **out_start,
    uint8_t **out_end
    )
{
#define smbios_guid_equal(a, b)  axl_guid_equal((a), (b))
    SMBIOS3_STRUCTURE_TABLE *smbios3 = NULL;
    SMBIOS_STRUCTURE_TABLE  *smbios2 = NULL;

    for (size_t i = 0; i < axl_st()->NumberOfTableEntries; i++) {
        EFI_GUID *guid = &axl_st()->ConfigurationTable[i].VendorGuid;
        if (smbios_guid_equal(guid, &SMBIOS3_TABLE_GUID)) {
            smbios3 = axl_st()->ConfigurationTable[i].VendorTable;
        } else if (smbios_guid_equal(guid, &SMBIOS_TABLE_GUID)) {
            smbios2 = axl_st()->ConfigurationTable[i].VendorTable;
        }
    }
#undef smbios_guid_equal

    if (smbios3 != NULL) {
        *out_start = (uint8_t *)(size_t)smbios3->TableAddress;
        *out_end = *out_start + smbios3->TableMaximumSize;
        return 0;
    }
    if (smbios2 != NULL) {
        *out_start = (uint8_t *)(size_t)smbios2->TableAddress;
        *out_end = *out_start + smbios2->TableLength;
        return 0;
    }

    axl_debug("SMBIOS table not found in configuration table");
    return -1;
}

/// Advance past a table entry (formatted area + string table + double NUL).
static uint8_t *
smbios_skip_entry(
    uint8_t *ptr,
    uint8_t *end
    )
{
    typedef struct { uint8_t Type; uint8_t Length; uint16_t Handle; } SmHdr;
    SmHdr *hdr = (SmHdr *)ptr;

    if (hdr->Length < 4) {
        return end;
    }
    ptr += hdr->Length;
    while (ptr < end - 1 && !(ptr[0] == 0 && ptr[1] == 0)) {
        ptr++;
    }
    return ptr + 2;
}

// ---------------------------------------------------------------------------
// axl_smbios_get_string (UCS-2, legacy)
// ---------------------------------------------------------------------------

unsigned short *
axl_smbios_get_string(
    AxlSmbiosHeader  *hdr,
    unsigned char     string_index
    )
{
    static unsigned short  buffer[128];
    char          *str;
    size_t         idx;
    size_t         i;

    buffer[0] = L'\0';

    if (hdr == NULL || string_index == 0) {
        return (unsigned short *)buffer;
    }

    str = (char *)hdr + hdr->Length;
    for (idx = 1; idx < string_index; idx++) {
        while (*str != '\0') {
            str++;
        }
        str++;
        if (*str == '\0') {
            return (unsigned short *)buffer;
        }
    }

    for (i = 0; i < 127 && str[i] != '\0'; i++) {
        buffer[i] = (unsigned short)str[i];
    }
    buffer[i] = L'\0';

    return (unsigned short *)buffer;
}

// ---------------------------------------------------------------------------
// axl_smbios_get_string_utf8
// ---------------------------------------------------------------------------

const char *
axl_smbios_get_string_utf8(
    AxlSmbiosHeader  *hdr,
    unsigned char     string_index
    )
{
    static char  buffer[128];
    char        *str;
    size_t       idx;
    size_t       i;

    buffer[0] = '\0';

    if (hdr == NULL || string_index == 0) {
        return buffer;
    }

    str = (char *)hdr + hdr->Length;
    for (idx = 1; idx < string_index; idx++) {
        while (*str != '\0') {
            str++;
        }
        str++;
        if (*str == '\0') {
            return buffer;
        }
    }

    for (i = 0; i < 127 && str[i] != '\0'; i++) {
        buffer[i] = str[i];
    }
    buffer[i] = '\0';

    return buffer;
}

// ---------------------------------------------------------------------------
// axl_smbios_find / axl_smbios_find_next
// ---------------------------------------------------------------------------

AxlSmbiosHeader *
axl_smbios_find(
    unsigned char type
    )
{
    return axl_smbios_find_next(type, NULL);
}

AxlSmbiosHeader *
axl_smbios_find_next(
    unsigned char     type,
    AxlSmbiosHeader  *prev
    )
{
    typedef struct { uint8_t Type; uint8_t Length; uint16_t Handle; } SmHdr;
    uint8_t *start;
    uint8_t *end;
    uint8_t *ptr;

    if (smbios_get_range(&start, &end) != 0) {
        return NULL;
    }

    /* If prev is given, skip past it to start searching after it */
    if (prev != NULL) {
        ptr = (uint8_t *)prev;
        if (ptr < start || ptr >= end) {
            return NULL;
        }
        ptr = smbios_skip_entry(ptr, end);
    } else {
        ptr = start;
    }

    /* Walk remaining entries */
    while (ptr < end) {
        SmHdr *hdr = (SmHdr *)ptr;
        if (hdr->Length < 4) {
            break;
        }
        if (hdr->Type == type) {
            return (AxlSmbiosHeader *)hdr;
        }
        ptr = smbios_skip_entry(ptr, end);
    }

    return NULL;
}
