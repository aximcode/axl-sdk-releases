/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbios.c
    SMBIOS helpers — string extraction and table lookup.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-smbios.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("smbios");

// ---------------------------------------------------------------------------
// Raw SMBIOS record layouts
// ---------------------------------------------------------------------------

/* We define the fields AXL cares about inline rather than pulling in
   EDK2's IndustryStandard/SmBios.h, keeping this file decoupled from
   the EDK2 package layout. Field offsets match the SMBIOS spec.
   Used by axl_smbios_read_*_info() further down. */

#pragma pack(1)
typedef struct {
    AxlSmbiosHeader  Hdr;
    uint8_t          Vendor;              /* string idx */
    uint8_t          BiosVersion;         /* string idx */
    uint16_t         BiosSegment;
    uint8_t          BiosReleaseDate;     /* string idx */
    uint8_t          BiosSize;
    uint64_t         Characteristics;
    uint8_t          BiosCharExt[2];
    uint8_t          MajorRelease;
    uint8_t          MinorRelease;
} SmbType0;

typedef struct {
    AxlSmbiosHeader  Hdr;
    uint8_t          Manufacturer;
    uint8_t          ProductName;
    uint8_t          Version;
    uint8_t          SerialNumber;
    uint8_t          Uuid[16];
    uint8_t          WakeUpType;
    uint8_t          SKUNumber;
    uint8_t          Family;
} SmbType1;

typedef struct {
    AxlSmbiosHeader  Hdr;
    uint8_t          Manufacturer;
    uint8_t          ProductName;
    uint8_t          Version;
    uint8_t          SerialNumber;
    uint8_t          AssetTag;
} SmbType2;

typedef struct {
    AxlSmbiosHeader  Hdr;
    uint8_t          Manufacturer;
    uint8_t          Type;
    uint8_t          Version;
    uint8_t          SerialNumber;
    uint8_t          AssetTag;
} SmbType3;

typedef struct {
    AxlSmbiosHeader  Hdr;
    uint8_t          Socket;              /* string idx */
    uint8_t          ProcessorType;
    uint8_t          ProcessorFamily;
    uint8_t          ProcessorManufacturer;/* string idx */
    uint64_t         ProcessorId;
    uint8_t          ProcessorVersion;    /* string idx */
    uint8_t          Voltage;
    uint16_t         ExternalClock;
    uint16_t         MaxSpeed;
    uint16_t         CurrentSpeed;
    uint8_t          Status;
    uint8_t          ProcessorUpgrade;
    uint16_t         L1CacheHandle;
    uint16_t         L2CacheHandle;
    uint16_t         L3CacheHandle;
    uint8_t          SerialNumber;        /* string idx */
    uint8_t          AssetTag;            /* string idx */
    uint8_t          PartNumber;          /* string idx */
    uint8_t          CoreCount;
    uint8_t          EnabledCoreCount;
    uint8_t          ThreadCount;
} SmbType4;

typedef struct {
    AxlSmbiosHeader  Hdr;
    uint16_t         MemoryArrayHandle;
    uint16_t         MemoryErrorInfoHandle;
    uint16_t         TotalWidth;
    uint16_t         DataWidth;
    uint16_t         Size;
    uint8_t          FormFactor;
    uint8_t          DeviceSet;
    uint8_t          DeviceLocator;       /* string idx */
    uint8_t          BankLocator;         /* string idx */
    uint8_t          MemoryType;
    uint16_t         TypeDetail;
    uint16_t         Speed;
    uint8_t          Manufacturer;        /* string idx */
    uint8_t          SerialNumber;        /* string idx */
    uint8_t          AssetTag;            /* string idx */
    uint8_t          PartNumber;          /* string idx */
    uint8_t          Attributes;
    uint32_t         ExtendedSize;
    uint16_t         ConfiguredClockSpeed;
} SmbType17;
#pragma pack()

// ---------------------------------------------------------------------------
// Internal: locate SMBIOS table range
// ---------------------------------------------------------------------------

/* Internal helper — locate the SMBIOS3 / SMBIOS 2.x entry-point
   structures in the EFI Configuration Table. Either out pointer
   may be NULL if the caller doesn't care about that variant. */
static int
smbios_locate_entry_points(
    SMBIOS3_STRUCTURE_TABLE **out_smbios3,
    SMBIOS_STRUCTURE_TABLE  **out_smbios2
    )
{
    SMBIOS3_STRUCTURE_TABLE *smbios3 = NULL;
    SMBIOS_STRUCTURE_TABLE  *smbios2 = NULL;

    for (size_t i = 0; i < axl_st()->NumberOfTableEntries; i++) {
        EFI_GUID *guid = &axl_st()->ConfigurationTable[i].VendorGuid;
        if (axl_efi_guid_equal(guid, &SMBIOS3_TABLE_GUID)) {
            smbios3 = axl_st()->ConfigurationTable[i].VendorTable;
        } else if (axl_efi_guid_equal(guid, &SMBIOS_TABLE_GUID)) {
            smbios2 = axl_st()->ConfigurationTable[i].VendorTable;
        }
    }

    if (out_smbios3) { *out_smbios3 = smbios3; }
    if (out_smbios2) { *out_smbios2 = smbios2; }
    return (smbios3 || smbios2) ? AXL_OK : AXL_ERR;
}

int
axl_smbios_table_range(
    uint8_t **out_start,
    uint8_t **out_end
    )
{
    SMBIOS3_STRUCTURE_TABLE *smbios3 = NULL;
    SMBIOS_STRUCTURE_TABLE  *smbios2 = NULL;

    if (out_start == NULL || out_end == NULL) {
        return AXL_ERR;
    }

    if (smbios_locate_entry_points(&smbios3, &smbios2) != 0) {
        axl_debug("SMBIOS table not found in configuration table");
        return AXL_ERR;
    }

    if (smbios3 != NULL) {
        *out_start = (uint8_t *)(size_t)smbios3->TableAddress;
        *out_end = *out_start + smbios3->TableMaximumSize;
        return AXL_OK;
    }
    *out_start = (uint8_t *)(size_t)smbios2->TableAddress;
    *out_end = *out_start + smbios2->TableLength;
    return AXL_OK;
}

int
axl_smbios_entry_point(
    uint8_t **out_base,
    size_t   *out_size
    )
{
    SMBIOS3_STRUCTURE_TABLE *smbios3 = NULL;
    SMBIOS_STRUCTURE_TABLE  *smbios2 = NULL;

    if (out_base == NULL || out_size == NULL) {
        return AXL_ERR;
    }

    if (smbios_locate_entry_points(&smbios3, &smbios2) != 0) {
        return AXL_ERR;
    }

    /* Prefer SMBIOS3 over 2.x when both are published — matches
       what axl_smbios_table_range does, so the (entry-point bytes,
       table data bytes) pair stays self-consistent for callers
       that concatenate them into a dump blob. */
    if (smbios3 != NULL) {
        *out_base = (uint8_t *)smbios3;
        *out_size = smbios3->Length;
        return AXL_OK;
    }
    *out_base = (uint8_t *)smbios2;
    *out_size = smbios2->Length;
    return AXL_OK;
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

uint16_t *
axl_smbios_get_string(
    AxlSmbiosHeader  *hdr,
    uint8_t     string_index
    )
{
    static uint16_t  buffer[128];
    char          *str;
    size_t         idx;
    size_t         i;

    buffer[0] = L'\0';

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
        buffer[i] = (uint16_t)str[i];
    }
    buffer[i] = L'\0';

    return buffer;
}

// ---------------------------------------------------------------------------
// axl_smbios_get_string_utf8
// ---------------------------------------------------------------------------

const char *
axl_smbios_get_string_utf8(
    AxlSmbiosHeader  *hdr,
    uint8_t     string_index
    )
{
    /* SMBIOS strings are NUL-terminated ASCII stored inline in the table
       memory, which persists for the life of the app (UEFI configuration
       table). Return a direct pointer — reentrant, no truncation, no
       static buffer. Callers that need a writable copy can strdup. */
    static const char  empty[] = "";
    char              *str;
    size_t             idx;

    if (hdr == NULL || string_index == 0) {
        return empty;
    }

    str = (char *)hdr + hdr->Length;
    for (idx = 1; idx < string_index; idx++) {
        while (*str != '\0') {
            str++;
        }
        str++;
        if (*str == '\0') {
            return empty;
        }
    }
    return str;
}

// ---------------------------------------------------------------------------
// axl_smbios_find / axl_smbios_find_next
// ---------------------------------------------------------------------------

AxlSmbiosHeader *
axl_smbios_find(
    uint8_t type
    )
{
    return axl_smbios_find_next(type, NULL);
}

AxlSmbiosHeader *
axl_smbios_find_next(
    uint8_t     type,
    AxlSmbiosHeader  *prev
    )
{
    typedef struct { uint8_t Type; uint8_t Length; uint16_t Handle; } SmHdr;
    uint8_t *start;
    uint8_t *end;
    uint8_t *ptr;

    if (axl_smbios_table_range(&start, &end) != 0) {
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

// ---------------------------------------------------------------------------
// axl_smbios_next — iterate every record regardless of type
// ---------------------------------------------------------------------------

AxlSmbiosHeader *
axl_smbios_next(
    AxlSmbiosHeader  *prev
    )
{
    typedef struct { uint8_t Type; uint8_t Length; uint16_t Handle; } SmHdr;
    uint8_t *start;
    uint8_t *end;
    uint8_t *ptr;

    if (axl_smbios_table_range(&start, &end) != 0) {
        return NULL;
    }

    if (prev != NULL) {
        ptr = (uint8_t *)prev;
        if (ptr < start || ptr >= end) {
            return NULL;
        }
        ptr = smbios_skip_entry(ptr, end);
    } else {
        ptr = start;
    }

    if (ptr >= end) {
        return NULL;
    }
    SmHdr *hdr = (SmHdr *)ptr;
    if (hdr->Length < 4 || hdr->Type == AXL_SMBIOS_TYPE_END) {
        return NULL;
    }
    return (AxlSmbiosHeader *)hdr;
}

// ---------------------------------------------------------------------------
// axl_smbios_version — read major/minor from the entry-point structure
// ---------------------------------------------------------------------------

int
axl_smbios_version(
    uint8_t *major,
    uint8_t *minor
    )
{
    SMBIOS3_STRUCTURE_TABLE *smbios3 = NULL;
    SMBIOS_STRUCTURE_TABLE  *smbios2 = NULL;

    for (size_t i = 0; i < axl_st()->NumberOfTableEntries; i++) {
        EFI_GUID *guid = &axl_st()->ConfigurationTable[i].VendorGuid;
        if (axl_efi_guid_equal(guid, &SMBIOS3_TABLE_GUID)) {
            smbios3 = axl_st()->ConfigurationTable[i].VendorTable;
        } else if (axl_efi_guid_equal(guid, &SMBIOS_TABLE_GUID)) {
            smbios2 = axl_st()->ConfigurationTable[i].VendorTable;
        }
    }

    if (smbios3 != NULL) {
        if (major != NULL) { *major = smbios3->MajorVersion; }
        if (minor != NULL) { *minor = smbios3->MinorVersion; }
        return AXL_OK;
    }
    if (smbios2 != NULL) {
        if (major != NULL) { *major = smbios2->MajorVersion; }
        if (minor != NULL) { *minor = smbios2->MinorVersion; }
        return AXL_OK;
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// Typed record accessors
// ---------------------------------------------------------------------------

int
axl_smbios_format_uuid(const uint8_t bytes[16], char out[37])
{
    if (bytes == NULL || out == NULL) {
        return AXL_ERR;
    }
    /* SMBIOS §7.2.1: Data1/2/3 are little-endian fields; Data4 +
       Node are big-endian. Reorder accordingly when printing. */
    axl_snprintf(
        out, 37,
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        bytes[3], bytes[2], bytes[1], bytes[0],     /* Data1 LE swap */
        bytes[5], bytes[4],                          /* Data2 LE swap */
        bytes[7], bytes[6],                          /* Data3 LE swap */
        bytes[8], bytes[9],                          /* Data4 BE */
        bytes[10], bytes[11], bytes[12],
        bytes[13], bytes[14], bytes[15]              /* Node BE */
        );
    return AXL_OK;
}

int
axl_smbios_read_bios_info(
    AxlSmbiosBiosInfo *out
    )
{
    if (out == NULL) { return AXL_ERR; }
    SmbType0 *t = (SmbType0 *)axl_smbios_find(AXL_SMBIOS_TYPE_BIOS_INFO);
    if (t == NULL) { return AXL_ERR; }

    out->vendor       = axl_smbios_get_string_utf8(&t->Hdr, t->Vendor);
    out->version      = axl_smbios_get_string_utf8(&t->Hdr, t->BiosVersion);
    out->release_date = axl_smbios_get_string_utf8(&t->Hdr, t->BiosReleaseDate);

    /* Fields beyond offset 0x14 (MajorRelease, MinorRelease) were added
       in SMBIOS 2.4 — hdr.Length tells us if they're present. */
    if (t->Hdr.Length >= 0x18) {
        out->major_release = t->MajorRelease;
        out->minor_release = t->MinorRelease;
    } else {
        out->major_release = 0xFF;
        out->minor_release = 0xFF;
    }
    return AXL_OK;
}

static bool
smbios_uuid_is_unset(const uint8_t u[16])
{
    uint8_t all_and = 0xFF;
    uint8_t all_or  = 0x00;
    for (size_t i = 0; i < 16; i++) {
        all_and &= u[i];
        all_or  |= u[i];
    }
    return (all_and == 0xFF) || (all_or == 0x00);
}

/* SMBIOS 2.6+ stores the UUID's first three fields little-endian.
   RFC 4122 / dmidecode / most tools expect big-endian. Swap them. */
static void
smbios_uuid_to_rfc4122(const uint8_t in[16], uint8_t out[16])
{
    out[0] = in[3]; out[1] = in[2]; out[2] = in[1]; out[3] = in[0];
    out[4] = in[5]; out[5] = in[4];
    out[6] = in[7]; out[7] = in[6];
    for (size_t i = 8; i < 16; i++) { out[i] = in[i]; }
}

int
axl_smbios_read_system_info(
    AxlSmbiosSystemInfo *out
    )
{
    if (out == NULL) { return AXL_ERR; }
    SmbType1 *t = (SmbType1 *)axl_smbios_find(AXL_SMBIOS_TYPE_SYSTEM_INFO);
    if (t == NULL) { return AXL_ERR; }

    out->manufacturer  = axl_smbios_get_string_utf8(&t->Hdr, t->Manufacturer);
    out->product_name  = axl_smbios_get_string_utf8(&t->Hdr, t->ProductName);
    out->version       = axl_smbios_get_string_utf8(&t->Hdr, t->Version);
    out->serial_number = axl_smbios_get_string_utf8(&t->Hdr, t->SerialNumber);
    /* SKU + Family added in SMBIOS 2.4 (hdr length >= 0x1B). */
    if (t->Hdr.Length >= 0x1B) {
        out->sku    = axl_smbios_get_string_utf8(&t->Hdr, t->SKUNumber);
        out->family = axl_smbios_get_string_utf8(&t->Hdr, t->Family);
    } else {
        out->sku = NULL;
        out->family = NULL;
    }
    if (smbios_uuid_is_unset(t->Uuid)) {
        for (size_t i = 0; i < 16; i++) { out->uuid[i] = 0; }
        out->has_uuid = false;
    } else {
        smbios_uuid_to_rfc4122(t->Uuid, out->uuid);
        out->has_uuid = true;
    }
    return AXL_OK;
}

int
axl_smbios_read_baseboard(
    AxlSmbiosBaseboardInfo *out
    )
{
    if (out == NULL) { return AXL_ERR; }
    SmbType2 *t = (SmbType2 *)axl_smbios_find(AXL_SMBIOS_TYPE_BASEBOARD);
    if (t == NULL) { return AXL_ERR; }
    out->manufacturer  = axl_smbios_get_string_utf8(&t->Hdr, t->Manufacturer);
    out->product_name  = axl_smbios_get_string_utf8(&t->Hdr, t->ProductName);
    out->version       = axl_smbios_get_string_utf8(&t->Hdr, t->Version);
    out->serial_number = axl_smbios_get_string_utf8(&t->Hdr, t->SerialNumber);
    out->asset_tag     = axl_smbios_get_string_utf8(&t->Hdr, t->AssetTag);

    /* BoardType lives at offset 0x0D of the formatted area — has been
     * part of Type 2 since SMBIOS 2.0, so essentially always present.
     * 0 = "not published" for the rare too-short record. The canonical
     * server-blade detector is BoardType == 3 (NOT Type 3 chassis
     * 0x1C/0x1D, which some OEM BIOS doesn't reliably set). */
    if (t->Hdr.Length > 0x0D) {
        const uint8_t *b = (const uint8_t *)&t->Hdr;
        out->board_type = b[0x0D];
    } else {
        out->board_type = AXL_SMBIOS_BOARD_TYPE_UNKNOWN;
    }
    return AXL_OK;
}

int
axl_smbios_read_chassis(
    AxlSmbiosChassisInfo *out
    )
{
    if (out == NULL) { return AXL_ERR; }
    SmbType3 *t = (SmbType3 *)axl_smbios_find(AXL_SMBIOS_TYPE_CHASSIS);
    if (t == NULL) { return AXL_ERR; }
    out->manufacturer  = axl_smbios_get_string_utf8(&t->Hdr, t->Manufacturer);
    out->version       = axl_smbios_get_string_utf8(&t->Hdr, t->Version);
    out->serial_number = axl_smbios_get_string_utf8(&t->Hdr, t->SerialNumber);
    out->asset_tag     = axl_smbios_get_string_utf8(&t->Hdr, t->AssetTag);
    out->type          = t->Type & 0x7F;   /* strip the 0x80 lock-bit flag */
    return AXL_OK;
}

int
axl_smbios_read_processor(
    AxlSmbiosHeader          *hdr,
    AxlSmbiosProcessorInfo   *out
    )
{
    if (out == NULL || hdr == NULL || hdr->Type != AXL_SMBIOS_TYPE_PROCESSOR) {
        return AXL_ERR;
    }
    SmbType4 *t = (SmbType4 *)hdr;
    out->socket_designation = axl_smbios_get_string_utf8(hdr, t->Socket);
    out->manufacturer       = axl_smbios_get_string_utf8(hdr, t->ProcessorManufacturer);
    out->version            = axl_smbios_get_string_utf8(hdr, t->ProcessorVersion);
    out->family             = t->ProcessorFamily;
    out->current_speed_mhz  = t->CurrentSpeed;
    out->max_speed_mhz      = t->MaxSpeed;
    out->status             = t->Status;
    /* Serial/Asset/Part added in SMBIOS 2.3 (hdr length >= 0x23). */
    if (hdr->Length >= 0x23) {
        out->serial_number = axl_smbios_get_string_utf8(hdr, t->SerialNumber);
        out->asset_tag     = axl_smbios_get_string_utf8(hdr, t->AssetTag);
        out->part_number   = axl_smbios_get_string_utf8(hdr, t->PartNumber);
    } else {
        out->serial_number = "";
        out->asset_tag     = "";
        out->part_number   = "";
    }
    /* Core/Thread count added in SMBIOS 2.5 (hdr length >= 0x28). */
    if (hdr->Length >= 0x28) {
        out->core_count   = t->CoreCount;
        out->thread_count = t->ThreadCount;
    } else {
        out->core_count   = 0;
        out->thread_count = 0;
    }
    return AXL_OK;
}

int
axl_smbios_read_memory_device(
    AxlSmbiosHeader          *hdr,
    AxlSmbiosMemoryDevice    *out
    )
{
    if (out == NULL || hdr == NULL || hdr->Type != AXL_SMBIOS_TYPE_MEMORY_DEVICE) {
        return AXL_ERR;
    }
    SmbType17 *t = (SmbType17 *)hdr;
    out->device_locator = axl_smbios_get_string_utf8(hdr, t->DeviceLocator);
    out->bank_locator   = axl_smbios_get_string_utf8(hdr, t->BankLocator);
    out->manufacturer   = axl_smbios_get_string_utf8(hdr, t->Manufacturer);
    out->part_number    = axl_smbios_get_string_utf8(hdr, t->PartNumber);
    out->serial_number  = axl_smbios_get_string_utf8(hdr, t->SerialNumber);
    out->asset_tag      = axl_smbios_get_string_utf8(hdr, t->AssetTag);
    out->memory_type    = t->MemoryType;
    out->speed_mhz      = t->Speed;

    /* FormFactor (0x0E), Total/Data width (0x08/0x0A) are in the minimum
       Type 17 record (length >= 0x15), so always present. 0xFFFF is the
       "unknown" sentinel for the widths — normalize to 0 (matching the
       "0 if unknown" convention of the other fields). */
    out->form_factor = t->FormFactor;
    out->total_width = (t->TotalWidth == 0xFFFF) ? 0 : t->TotalWidth;
    out->data_width  = (t->DataWidth  == 0xFFFF) ? 0 : t->DataWidth;

    /* Rank: Attributes byte (0x1B), bits 3:0. Added in SMBIOS 2.6, so the
       byte exists only when the record reaches it (length >= 0x1C). 0 = unknown. */
    out->rank = (hdr->Length >= 0x1C) ? (uint8_t)(t->Attributes & 0x0F) : 0;

    /* Size: 0 = empty slot, 0x7FFF = use ExtendedSize (2.7+), else value
       in MB (bit 15 clear) or KB (bit 15 set). */
    if (t->Size == 0) {
        out->size_mb = 0;
    } else if (t->Size == 0x7FFF && hdr->Length >= 0x20) {
        out->size_mb = t->ExtendedSize & 0x7FFFFFFF;
    } else if (t->Size & 0x8000) {
        out->size_mb = (t->Size & 0x7FFF) / 1024;
    } else {
        out->size_mb = t->Size;
    }
    return AXL_OK;
}

int
axl_smbios_get_system_uuid(
    uint8_t out[16]
    )
{
    AxlSmbiosSystemInfo sys;
    if (axl_smbios_read_system_info(&sys) != 0) { return AXL_ERR; }
    if (!sys.has_uuid) { return AXL_ERR; }
    for (size_t i = 0; i < 16; i++) { out[i] = sys.uuid[i]; }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Type 38 — IPMI Device Information
//
// Layout (SMBIOS spec):
//   0x00  AxlSmbiosHeader
//   0x04  InterfaceType         (1 byte)
//   0x05  IPMISpecificationRevision (1 byte: high nibble major, low nibble minor)
//   0x06  I2CTargetAddress      (1 byte)
//   0x07  NVStorageDeviceAddress (1 byte; 0xFF = none)
//   0x08  BaseAddress           (8 bytes LSB; bit 0 of LSB = I/O space if set)
//   0x10  BaseAddressModifier   (1 byte, interrupt info flags)
//   0x11  InterruptNumber       (1 byte; 0 = none)
// ---------------------------------------------------------------------------

int
axl_smbios_read_ipmi_device_info(
    AxlSmbiosIpmiDeviceInfo *out
    )
{
    if (out == NULL) { return AXL_ERR; }
    AxlSmbiosHeader *hdr = axl_smbios_find(AXL_SMBIOS_TYPE_IPMI_DEVICE_INFO);
    if (hdr == NULL || hdr->Length < 0x10) { return AXL_ERR; }

    const uint8_t *b = (const uint8_t *)hdr;
    out->interface_type     = b[0x04];
    uint8_t spec            = b[0x05];
    out->spec_major         = (spec >> 4) & 0x0F;
    out->spec_minor         = spec & 0x0F;
    out->i2c_target_address = b[0x06];
    out->nv_storage_address = b[0x07];

    uint64_t base = 0;
    for (int i = 0; i < 8; i++) {
        base |= ((uint64_t)b[0x08 + i]) << (i * 8);
    }
    out->is_memory_mapped = ((base & 0x1) == 0);
    out->base_address     = base & ~(uint64_t)0x1;

    out->interrupt_number = (hdr->Length >= 0x12) ? b[0x11] : 0;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Type 42 — Management Controller Host Interface
//
// Modern layout (SMBIOS 3.0+):
//   0x00  AxlSmbiosHeader (type 42, length, handle)
//   0x04  InterfaceType        (1 byte)
//   0x05  InterfaceDataLength  (1 byte, = N)
//   0x06  InterfaceData        (N bytes)
//   0x06+N  NumberOfProtocolRecords (1 byte, = M)
//   0x07+N  M protocol records, each:
//             0x00  ProtocolType        (1 byte)
//             0x01  ProtocolDataLength  (1 byte, = P)
//             0x02  ProtocolData        (P bytes)
// ---------------------------------------------------------------------------

int
axl_smbios_read_host_interface(
    AxlSmbiosHeader         *hdr,
    AxlSmbiosHostInterface  *out
    )
{
    if (out == NULL || hdr == NULL
        || hdr->Type != AXL_SMBIOS_TYPE_MGMT_HOST_INTERFACE)
    {
        return AXL_ERR;
    }

    /* Minimum length for the modern layout: header(4) + type(1) + data_len(1)
       + proto_count(1) = 7 bytes. The old pre-3.0 layout is 5 bytes and not
       supported here. */
    if (hdr->Length < 7) { return AXL_ERR; }

    const uint8_t *base = (const uint8_t *)hdr;
    const uint8_t *end  = base + hdr->Length;

    out->interface_type     = base[0x04];
    out->interface_data_len = base[0x05];
    out->interface_data     = base + 0x06;

    /* Validate the interface_data fits inside the record. */
    const uint8_t *p = out->interface_data + out->interface_data_len;
    if (p + 1 > end) { return AXL_ERR; }

    uint8_t proto_count = *p++;
    out->protocol_count = 0;

    for (uint8_t i = 0; i < proto_count && i < 8; i++) {
        if (p + 2 > end) { return AXL_ERR; }
        uint8_t  type = p[0];
        uint8_t  dlen = p[1];
        if (p + 2 + dlen > end) { return AXL_ERR; }
        out->protocols[i].protocol_type = type;
        out->protocols[i].data_len      = dlen;
        out->protocols[i].data          = p + 2;
        p += 2 + dlen;
        out->protocol_count++;
    }
    return AXL_OK;
}

int
axl_smbios_read_redfish_over_ip(
    const AxlSmbiosHostInterfaceProtocol *proto,
    AxlSmbiosRedfishOverIp               *out
    )
{
    /* Layout per SMBIOS 3.x §7.43.3:
       16  service_uuid
        1  host_ip_assignment
        1  host_ip_format
       16  host_ip_address
       16  host_ip_mask
        1  service_ip_discovery
        1  service_ip_format
       16  service_ip_address
       16  service_ip_mask
        2  service_port (LE)
        4  service_vlan_id (LE)
        1  hostname_len (= H)
        H  hostname
       Fixed prefix = 91 bytes; total = 91 + hostname_len. */
    if (proto == NULL || out == NULL) {
        return AXL_ERR;
    }
    if (proto->protocol_type != AXL_SMBIOS_HIP_REDFISH_OVER_IP) {
        return AXL_ERR;
    }
    if (proto->data == NULL || proto->data_len < 91) {
        return AXL_ERR;
    }

    const uint8_t *p = proto->data;

    /* Bulk byte fields. */
    for (size_t i = 0; i < 16; i++) {
        out->service_uuid[i] = p[i];
    }
    out->host_ip_assignment = (AxlSmbiosRedfishHostIpAssignment)p[16];
    out->host_ip_format     = (AxlSmbiosRedfishIpFormat)p[17];
    for (size_t i = 0; i < 16; i++) {
        out->host_ip_address[i] = p[18 + i];
    }
    for (size_t i = 0; i < 16; i++) {
        out->host_ip_mask[i] = p[34 + i];
    }
    out->service_ip_discovery = p[50];
    out->service_ip_format    = (AxlSmbiosRedfishIpFormat)p[51];
    for (size_t i = 0; i < 16; i++) {
        out->service_ip_address[i] = p[52 + i];
    }
    for (size_t i = 0; i < 16; i++) {
        out->service_ip_mask[i] = p[68 + i];
    }
    /* Multi-byte fields are little-endian on the SMBIOS wire. */
    out->service_port = (uint16_t)((uint16_t)p[84]
                                 | ((uint16_t)p[85] << 8));
    out->service_vlan_id = (uint32_t)p[86]
                         | ((uint32_t)p[87] << 8)
                         | ((uint32_t)p[88] << 16)
                         | ((uint32_t)p[89] << 24);
    out->hostname_len = p[90];

    /* Hostname bytes follow the fixed prefix. Reject truncated records
       where firmware claims a hostname longer than the protocol_data
       length permits. */
    if ((size_t)91 + (size_t)out->hostname_len > proto->data_len) {
        return AXL_ERR;
    }
    out->hostname = (out->hostname_len > 0)
                  ? (const char *)(p + 91)
                  : NULL;

    return AXL_OK;
}

int
axl_smbios_find_redfish_host_interface(
    AxlSmbiosHeader        **hdr_out,
    AxlSmbiosHostInterface  *iface_out
    )
{
    AxlSmbiosHeader *h = NULL;
    while ((h = axl_smbios_find_next(AXL_SMBIOS_TYPE_MGMT_HOST_INTERFACE, h)) != NULL) {
        AxlSmbiosHostInterface iface;
        if (axl_smbios_read_host_interface(h, &iface) != 0) { continue; }
        if (iface.interface_type != AXL_SMBIOS_HIF_NETWORK)  { continue; }
        for (uint8_t i = 0; i < iface.protocol_count; i++) {
            if (iface.protocols[i].protocol_type == AXL_SMBIOS_HIP_REDFISH_OVER_IP) {
                if (hdr_out != NULL)   { *hdr_out   = h; }
                if (iface_out != NULL) { *iface_out = iface; }
                return AXL_OK;
            }
        }
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// axl_smbios_copy_string_utf8 — reentrant caller-buffer string accessor
// ---------------------------------------------------------------------------

size_t
axl_smbios_copy_string_utf8(
    AxlSmbiosHeader  *hdr,
    uint8_t           string_index,
    char             *buf,
    size_t            buf_size
    )
{
    if (buf != NULL && buf_size > 0) { buf[0] = '\0'; }
    if (buf == NULL || buf_size == 0 || hdr == NULL || string_index == 0) {
        return 0;
    }

    /* SMBIOS strings live inline after the formatted area; reuse the
       direct-pointer helper to find the source then bound-copy. */
    const char *src = axl_smbios_get_string_utf8(hdr, string_index);
    if (src == NULL || src[0] == '\0') { return 0; }

    size_t i = 0;
    size_t cap = buf_size - 1;   /* room for terminator */
    while (i < cap && src[i] != '\0') {
        buf[i] = src[i];
        i++;
    }
    buf[i] = '\0';
    return i;
}

// ---------------------------------------------------------------------------
// Type 8 — Port Connector Information
//
// Layout (SMBIOS spec):
//   0x00  AxlSmbiosHeader (type 8)
//   0x04  InternalReferenceDesignator  (string idx)
//   0x05  InternalConnectorType        (1 byte)
//   0x06  ExternalReferenceDesignator  (string idx)
//   0x07  ExternalConnectorType        (1 byte)
//   0x08  PortType                     (1 byte)
// ---------------------------------------------------------------------------

int
axl_smbios_read_port_connector(
    AxlSmbiosHeader         *hdr,
    AxlSmbiosPortConnector  *out
    )
{
    if (out == NULL || hdr == NULL || hdr->Type != AXL_SMBIOS_TYPE_PORT_CONNECTOR
        || hdr->Length < 0x09)
    {
        return AXL_ERR;
    }
    const uint8_t *b = (const uint8_t *)hdr;
    out->internal_designator     = axl_smbios_get_string_utf8(hdr, b[0x04]);
    out->internal_connector_type = b[0x05];
    out->external_designator     = axl_smbios_get_string_utf8(hdr, b[0x06]);
    out->external_connector_type = b[0x07];
    out->port_type               = b[0x08];
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Type 9 — System Slots
//
// Layout — only the fields AXL surfaces are listed; spec adds more after
// the variable-length peer-grouping array (3.2+) which we don't expose.
//
//   0x00  AxlSmbiosHeader
//   0x04  SlotDesignation       (string idx)
//   0x05  SlotType              (1 byte)
//   0x06  SlotDataBusWidth      (1 byte)
//   0x07  CurrentUsage          (1 byte)
//   0x08  SlotLength            (1 byte)
//   0x09  SlotID                (2 bytes)
//   0x0B  SlotCharacteristics1  (1 byte)
//   0x0C  SlotCharacteristics2  (1 byte)  end of 2.0-2.5 record (length 0x0D)
//   0x0D  SegmentGroupNumber    (2 bytes) added 2.6
//   0x0F  BusNumber             (1 byte)  added 2.6
//   0x10  DeviceFunction        (1 byte)  added 2.6 — record length 0x11
//   0x11  DataBusWidthBase      (1 byte)  added 3.2
//   0x12  PeerGroupingCount     (1 byte)  added 3.2
//   0x13+ PeerGroups[]                    variable, omitted from typed reader
// ---------------------------------------------------------------------------

int
axl_smbios_read_system_slot(
    AxlSmbiosHeader      *hdr,
    AxlSmbiosSystemSlot  *out
    )
{
    if (out == NULL || hdr == NULL || hdr->Type != AXL_SMBIOS_TYPE_SYSTEM_SLOTS
        || hdr->Length < 0x0D)
    {
        return AXL_ERR;
    }
    const uint8_t *b = (const uint8_t *)hdr;
    out->designation         = axl_smbios_get_string_utf8(hdr, b[0x04]);
    out->slot_type           = b[0x05];
    out->slot_data_bus_width = b[0x06];
    out->current_usage       = b[0x07];
    out->slot_length         = b[0x08];
    out->slot_id             = (uint16_t)b[0x09] | ((uint16_t)b[0x0A] << 8);

    /* SMBIOS 2.6+ adds segment / bus / device:function. */
    if (hdr->Length >= 0x11) {
        out->segment_group   = (uint16_t)b[0x0D] | ((uint16_t)b[0x0E] << 8);
        out->bus             = b[0x0F];
        out->device_function = b[0x10];
    } else {
        out->segment_group   = 0xFFFF;
        out->bus             = 0xFF;
        out->device_function = 0xFF;
    }

    /* SMBIOS 3.2+ adds DataBusWidth (base) and PeerGroupingCount. */
    if (hdr->Length >= 0x13) {
        out->data_bus_width_base = b[0x11];
        out->peer_grouping_count = b[0x12];
    } else {
        out->data_bus_width_base = 0;
        out->peer_grouping_count = 0;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Type 11 — OEM Strings
//
// Layout:
//   0x00  AxlSmbiosHeader
//   0x04  Count   (1 byte: number of strings to follow)
//
// The strings themselves live in the standard SMBIOS string area after
// the formatted record. axl_smbios_get_string_utf8 walks them by index.
// ---------------------------------------------------------------------------

int
axl_smbios_read_oem_strings(
    AxlSmbiosHeader      *hdr,
    AxlSmbiosOemStrings  *out
    )
{
    if (out == NULL || hdr == NULL || hdr->Type != AXL_SMBIOS_TYPE_OEM_STRINGS
        || hdr->Length < 0x05)
    {
        return AXL_ERR;
    }
    const uint8_t *b = (const uint8_t *)hdr;
    uint8_t  raw_count = b[0x04];
    uint8_t  cap = (uint8_t)(sizeof(out->strings) / sizeof(out->strings[0]));
    out->count = (raw_count <= cap) ? raw_count : cap;

    for (uint8_t i = 0; i < cap; i++) {
        if (i < out->count) {
            /* SMBIOS strings are 1-based; spec index 1 is at array slot 0. */
            out->strings[i] = axl_smbios_get_string_utf8(hdr, (uint8_t)(i + 1));
        } else {
            out->strings[i] = NULL;
        }
    }
    return AXL_OK;
}

int
axl_smbios_get_oem_string(
    uint8_t  index_one_based,
    char    *buf,
    size_t   buf_cap,
    size_t  *required
    )
{
    if (buf == NULL || buf_cap == 0 || index_one_based == 0) {
        return AXL_ERR;
    }

    /* Walk Type 11 records in firmware order, accumulating string
       counts, until we've reached the record containing the
       requested index. Most platforms ship a single Type 11
       record; the multi-record path is mostly here for robustness
       against firmware that splits OEM strings across records. */
    AxlSmbiosHeader *hdr = NULL;
    uint8_t          base = 0;  /* strings counted before the current record */
    while ((hdr = axl_smbios_find_next(AXL_SMBIOS_TYPE_OEM_STRINGS, hdr))
           != NULL)
    {
        AxlSmbiosOemStrings rec;
        if (axl_smbios_read_oem_strings(hdr, &rec) != 0) {
            continue;
        }
        /* Does the requested index land in this record? Inputs are
           1-based; rec.count is the per-record count. */
        if (index_one_based <= base + rec.count) {
            uint8_t local = (uint8_t)(index_one_based - base);
            const char *s = rec.strings[local - 1];
            if (s == NULL) {
                return AXL_ERR;
            }
            /* Compute the source length so we can refuse to
               truncate. Callers explicitly retry with a larger
               buffer rather than receive a silently-clipped value. */
            size_t need = axl_strlen(s);
            if (need + 1 > buf_cap) {
                if (required != NULL) {
                    *required = need + 1;
                }
                return AXL_ERR;
            }
            for (size_t i = 0; i < need; i++) {
                buf[i] = s[i];
            }
            buf[need] = '\0';
            return AXL_OK;
        }
        base = (uint8_t)(base + rec.count);
        /* If we've consumed all 16 slots from this record's cap, the
           caller's index can't be satisfied without overflowing the
           per-record cap; but the spec doesn't require Type 11 to
           cap at 16, so we keep walking — subsequent records may
           contain more strings legitimately. */
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// Type 16 — Physical Memory Array
//
// Layout:
//   0x00  AxlSmbiosHeader
//   0x04  Location               (1 byte)
//   0x05  Use                    (1 byte)
//   0x06  MemoryErrorCorrection  (1 byte: ECC type)
//   0x07  MaximumCapacity        (4 bytes, kilobytes; 0x80000000 = use ExtendedMaxCapacity)
//   0x0B  MemoryErrorInfoHandle  (2 bytes)
//   0x0D  NumberOfMemoryDevices  (2 bytes)
//   0x0F  ExtendedMaxCapacity    (8 bytes, bytes) — added 2.7
// ---------------------------------------------------------------------------

int
axl_smbios_read_physical_memory_array(
    AxlSmbiosHeader               *hdr,
    AxlSmbiosPhysicalMemoryArray  *out
    )
{
    if (out == NULL || hdr == NULL
        || hdr->Type != AXL_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY
        || hdr->Length < 0x0F)
    {
        return AXL_ERR;
    }
    const uint8_t *b = (const uint8_t *)hdr;
    out->location  = b[0x04];
    out->use       = b[0x05];
    out->ecc_type  = b[0x06];

    uint32_t max_kb =  (uint32_t)b[0x07]
                     | ((uint32_t)b[0x08] << 8)
                     | ((uint32_t)b[0x09] << 16)
                     | ((uint32_t)b[0x0A] << 24);
    if (max_kb == 0x80000000U) {
        /* Sentinel: "actual capacity is in ExtendedMaxCapacity"
         * (spec 2.7+). If the record is too short to carry the
         * Extended field, the firmware is malformed — surface
         * that as 0 ("not published") rather than reading the
         * literal sentinel × 1024 = 2 TB. */
        if (hdr->Length >= 0x17) {
            uint64_t ext = 0;
            for (int i = 0; i < 8; i++) {
                ext |= ((uint64_t)b[0x0F + i]) << (i * 8);
            }
            out->max_capacity_bytes = ext;
        } else {
            out->max_capacity_bytes = 0;
        }
    } else {
        out->max_capacity_bytes = (uint64_t)max_kb * 1024ULL;
    }

    out->error_handle = (uint16_t)b[0x0B] | ((uint16_t)b[0x0C] << 8);
    out->num_devices  = (uint16_t)b[0x0D] | ((uint16_t)b[0x0E] << 8);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Type 19 — Memory Array Mapped Address
//
// Layout:
//   0x00  AxlSmbiosHeader
//   0x04  StartingAddress         (4 bytes, KB units; 0xFFFFFFFF = use Extended)
//   0x08  EndingAddress           (4 bytes, KB units; 0xFFFFFFFF = use Extended)
//   0x0C  MemoryArrayHandle       (2 bytes)
//   0x0E  PartitionWidth          (1 byte)
//   0x0F  ExtendedStartingAddress (8 bytes, bytes) — added 2.7
//   0x17  ExtendedEndingAddress   (8 bytes, bytes) — added 2.7
// ---------------------------------------------------------------------------

int
axl_smbios_read_memory_array_map(
    AxlSmbiosHeader          *hdr,
    AxlSmbiosMemoryArrayMap  *out
    )
{
    if (out == NULL || hdr == NULL
        || hdr->Type != AXL_SMBIOS_TYPE_MEMORY_ARRAY_MAP
        || hdr->Length < 0x0F)
    {
        return AXL_ERR;
    }
    const uint8_t *b = (const uint8_t *)hdr;
    uint32_t start_kb =  (uint32_t)b[0x04]
                       | ((uint32_t)b[0x05] << 8)
                       | ((uint32_t)b[0x06] << 16)
                       | ((uint32_t)b[0x07] << 24);
    uint32_t end_kb   =  (uint32_t)b[0x08]
                       | ((uint32_t)b[0x09] << 8)
                       | ((uint32_t)b[0x0A] << 16)
                       | ((uint32_t)b[0x0B] << 24);

    /* Sentinel 0xFFFFFFFF KB → use Extended* (spec 2.7+). When the
     * record is too short to carry the Extended field, surface the
     * unresolvable sentinel as 0 to match the "not published"
     * convention used by the rest of the readers. */
    if (start_kb == 0xFFFFFFFFU) {
        if (hdr->Length >= 0x1F) {
            uint64_t ext = 0;
            for (int i = 0; i < 8; i++) {
                ext |= ((uint64_t)b[0x0F + i]) << (i * 8);
            }
            out->starting_address = ext;
        } else {
            out->starting_address = 0;
        }
    } else {
        out->starting_address = (uint64_t)start_kb * 1024ULL;
    }
    if (end_kb == 0xFFFFFFFFU) {
        if (hdr->Length >= 0x1F) {
            uint64_t ext = 0;
            for (int i = 0; i < 8; i++) {
                ext |= ((uint64_t)b[0x17 + i]) << (i * 8);
            }
            out->ending_address = ext;
        } else {
            out->ending_address = 0;
        }
    } else {
        out->ending_address = (uint64_t)end_kb * 1024ULL;
    }

    out->array_handle    = (uint16_t)b[0x0C] | ((uint16_t)b[0x0D] << 8);
    out->partition_width = b[0x0E];
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Type 20 — Memory Device Mapped Address
//
// Layout:
//   0x00  AxlSmbiosHeader
//   0x04  StartingAddress              (4 bytes, KB; 0xFFFFFFFF = use Extended)
//   0x08  EndingAddress                (4 bytes, KB; 0xFFFFFFFF = use Extended)
//   0x0C  MemoryDeviceHandle           (2 bytes)
//   0x0E  MemoryArrayMappedAddrHandle  (2 bytes)
//   0x10  PartitionRowPosition         (1 byte)
//   0x11  InterleavePosition           (1 byte)
//   0x12  InterleaveDataDepth          (1 byte)
//   0x13  ExtendedStartingAddress      (8 bytes, bytes) — added 2.7
//   0x1B  ExtendedEndingAddress        (8 bytes, bytes) — added 2.7
// ---------------------------------------------------------------------------

int
axl_smbios_read_memory_device_map(
    AxlSmbiosHeader           *hdr,
    AxlSmbiosMemoryDeviceMap  *out
    )
{
    if (out == NULL || hdr == NULL
        || hdr->Type != AXL_SMBIOS_TYPE_MEMORY_DEVICE_MAP
        || hdr->Length < 0x13)
    {
        return AXL_ERR;
    }
    const uint8_t *b = (const uint8_t *)hdr;
    uint32_t start_kb =  (uint32_t)b[0x04]
                       | ((uint32_t)b[0x05] << 8)
                       | ((uint32_t)b[0x06] << 16)
                       | ((uint32_t)b[0x07] << 24);
    uint32_t end_kb   =  (uint32_t)b[0x08]
                       | ((uint32_t)b[0x09] << 8)
                       | ((uint32_t)b[0x0A] << 16)
                       | ((uint32_t)b[0x0B] << 24);

    /* Sentinel handling matches Type 19 — see that reader's comment. */
    if (start_kb == 0xFFFFFFFFU) {
        if (hdr->Length >= 0x23) {
            uint64_t ext = 0;
            for (int i = 0; i < 8; i++) {
                ext |= ((uint64_t)b[0x13 + i]) << (i * 8);
            }
            out->starting_address = ext;
        } else {
            out->starting_address = 0;
        }
    } else {
        out->starting_address = (uint64_t)start_kb * 1024ULL;
    }
    if (end_kb == 0xFFFFFFFFU) {
        if (hdr->Length >= 0x23) {
            uint64_t ext = 0;
            for (int i = 0; i < 8; i++) {
                ext |= ((uint64_t)b[0x1B + i]) << (i * 8);
            }
            out->ending_address = ext;
        } else {
            out->ending_address = 0;
        }
    } else {
        out->ending_address = (uint64_t)end_kb * 1024ULL;
    }

    out->device_handle         = (uint16_t)b[0x0C] | ((uint16_t)b[0x0D] << 8);
    out->array_map_handle      = (uint16_t)b[0x0E] | ((uint16_t)b[0x0F] << 8);
    out->partition_row_pos     = b[0x10];
    out->interleave_position   = b[0x11];
    out->interleave_data_depth = b[0x12];
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Type 41 — Onboard Devices Extended Information
//
// Layout:
//   0x00  AxlSmbiosHeader
//   0x04  ReferenceDesignation  (string idx)
//   0x05  DeviceType            (1 byte: bit 7 = Status, low 7 = type)
//   0x06  DeviceTypeInstance    (1 byte)
//   0x07  SegmentGroupNumber    (2 bytes)
//   0x09  BusNumber             (1 byte)
//   0x0A  DeviceFunctionNumber  (1 byte: bits 7:3 = device, bits 2:0 = function)
// ---------------------------------------------------------------------------

int
axl_smbios_read_onboard_device_ext(
    AxlSmbiosHeader            *hdr,
    AxlSmbiosOnboardDeviceExt  *out
    )
{
    if (out == NULL || hdr == NULL
        || hdr->Type != AXL_SMBIOS_TYPE_ONBOARD_DEVICE_EXT
        || hdr->Length < 0x0B)
    {
        return AXL_ERR;
    }
    const uint8_t *b = (const uint8_t *)hdr;
    out->reference_designation = axl_smbios_get_string_utf8(hdr, b[0x04]);
    out->device_type           = b[0x05];
    out->device_type_instance  = b[0x06];
    out->segment_group         = (uint16_t)b[0x07] | ((uint16_t)b[0x08] << 8);
    out->bus                   = b[0x09];
    out->device_function       = b[0x0A];
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_smbios_strings_byte_len
//
// Walk from hdr+Length to the spec-required end-of-region 0x00 0x00.
// Returns the byte count of the strings region (not including the
// terminating extra NUL). Records with no strings are formatted-area
// + 0x00 0x00, returning 0 here.
// ---------------------------------------------------------------------------

size_t
axl_smbios_strings_byte_len(
    AxlSmbiosHeader  *hdr
    )
{
    if (hdr == NULL || hdr->Length < 4) {
        return 0;
    }
    /* Bound the walk against the SMBIOS table memory range so a
     * malformed record without a double-NUL doesn't run past the
     * table end into adjacent memory. */
    uint8_t *start;
    uint8_t *end;
    if (axl_smbios_table_range(&start, &end) != 0) {
        return 0;
    }
    uint8_t *base = (uint8_t *)hdr;
    if (base < start || base >= end) {
        return 0;
    }
    uint8_t *strings = base + hdr->Length;
    if (strings + 1 >= end) {
        return 0;
    }
    /* Empty-region special case: zero-string records emit just the
     * two-byte "00 00" sentinel right after the formatted area, so
     * strings[0] is already the terminator. Return 0 to match the
     * documented "may be 0" behavior — distinct from the
     * one-string-of-length-zero case (which can't exist; an empty
     * string takes one byte, the NUL itself). */
    if (strings[0] == 0x00 && strings[1] == 0x00) {
        return 0;
    }
    /* Non-empty: scan for the double-NUL terminator. The strings
     * region runs from `strings` up to and INCLUDING the NUL after
     * the last string (offset P, where P is the position of the
     * first NUL of the double-NUL pair). Length = P + 1. */
    uint8_t *p = strings;
    while (p + 1 < end) {
        if (p[0] == 0x00 && p[1] == 0x00) {
            return (size_t)(p - strings) + 1;
        }
        p++;
    }
    /* Malformed record: no end-of-region marker before the table
     * runs out. Return what we walked rather than 0 — caller can
     * still hexdump the partial region. */
    return (size_t)(p - strings);
}

// ---------------------------------------------------------------------------
// SMBIOS Type 9 (System Slots) spec-value decoders
//
// Pure spec lookups, no allocation, return a static const string or
// NULL. Values match SMBIOS 3.7 Table 13 / EDK2 MdePkg/IndustryStandard/
// SmBios.h. The "modern slot type" set — OCP NIC, EDSFF, PCIe Gen 5/6
// — is included.
//
// NOTE: a number of in-the-wild OEM diagnostic tools carry their own
// slot-type tables; some have been observed with values shifted by 4
// (e.g. PCIe at 0xA1 instead of 0xA5) and PCIe-Mini / U.2 codes
// (0x21-0x25) labelled as M.2 keys. This implementation uses the
// spec values; downstream consumers that switch from a local decoder
// to axl_smbios_slot_type_str will see corrected decoding for any
// slot whose firmware reports a value the local table got wrong.
// ---------------------------------------------------------------------------

const char *
axl_smbios_slot_type_str(
    uint8_t type
    )
{
    switch (type) {
        /* Common legacy + transition values */
        case 0x06: return "PCI";
        case 0x07: return "PC Card (PCMCIA)";
        case 0x0F: return "AGP";
        case 0x10: return "AGP 2X";
        case 0x11: return "AGP 4X";
        case 0x12: return "PCI-X";
        case 0x13: return "AGP 8X";

        /* M.2 keys — true M.2 sockets at 0x14-0x17 (NOT 0x22-0x25;
         * those are PCIe Mini / U.2). */
        case 0x14: return "M.2 Socket 1-DP (Mech Key A)";
        case 0x15: return "M.2 Socket 1-SD (Mech Key E)";
        case 0x16: return "M.2 Socket 2 (Mech Key B)";
        case 0x17: return "M.2 Socket 3 (Mech Key M)";

        /* PCIe SFF-8639 / U.2 family at 0x1F, 0x20, 0x24, 0x25 */
        case 0x1F: return "PCIe Gen 2 SFF-8639 (U.2)";
        case 0x20: return "PCIe Gen 3 SFF-8639 (U.2)";
        case 0x21: return "PCIe Mini 52-pin (CEM 2.0, with BSKO)";
        case 0x22: return "PCIe Mini 52-pin (CEM 2.0, without BSKO)";
        case 0x23: return "PCIe Mini 76-pin (CEM 2.0, Display-Mini)";
        case 0x24: return "PCIe Gen 4 SFF-8639 (U.2)";
        case 0x25: return "PCIe Gen 5 SFF-8639 (U.2)";

        /* OCP NIC 3.0 + Prior */
        case 0x26: return "OCP NIC 3.0 SFF";
        case 0x27: return "OCP NIC 3.0 LFF";
        case 0x28: return "OCP NIC Prior to 3.0";

        /* CXL */
        case 0x30: return "CXL Flexbus 1.0";

        /* PCIe Gen 1 (the "PCIe" with no generation suffix). PCIe values
         * occupy 0xA5 onwards, with each generation taking 6 codes:
         * (no width, x1, x2, x4, x8, x16). 0xB7 is reserved between
         * Gen 3 and Gen 4. */
        case 0xA5: return "PCIe";
        case 0xA6: return "PCIe x1";
        case 0xA7: return "PCIe x2";
        case 0xA8: return "PCIe x4";
        case 0xA9: return "PCIe x8";
        case 0xAA: return "PCIe x16";
        case 0xAB: return "PCIe Gen 2";
        case 0xAC: return "PCIe Gen 2 x1";
        case 0xAD: return "PCIe Gen 2 x2";
        case 0xAE: return "PCIe Gen 2 x4";
        case 0xAF: return "PCIe Gen 2 x8";
        case 0xB0: return "PCIe Gen 2 x16";
        case 0xB1: return "PCIe Gen 3";
        case 0xB2: return "PCIe Gen 3 x1";
        case 0xB3: return "PCIe Gen 3 x2";
        case 0xB4: return "PCIe Gen 3 x4";
        case 0xB5: return "PCIe Gen 3 x8";
        case 0xB6: return "PCIe Gen 3 x16";
        /* 0xB7 reserved */
        case 0xB8: return "PCIe Gen 4";
        case 0xB9: return "PCIe Gen 4 x1";
        case 0xBA: return "PCIe Gen 4 x2";
        case 0xBB: return "PCIe Gen 4 x4";
        case 0xBC: return "PCIe Gen 4 x8";
        case 0xBD: return "PCIe Gen 4 x16";
        case 0xBE: return "PCIe Gen 5";
        case 0xBF: return "PCIe Gen 5 x1";
        case 0xC0: return "PCIe Gen 5 x2";
        case 0xC1: return "PCIe Gen 5 x4";
        case 0xC2: return "PCIe Gen 5 x8";
        case 0xC3: return "PCIe Gen 5 x16";
        case 0xC4: return "PCIe Gen 6 and Beyond";

        /* EDSFF — one code per form-factor family covering both
         * size variants (E1.S + E1.L share 0xC5; E3.S + E3.L share 0xC6). */
        case 0xC5: return "EDSFF E1 (E1.S, E1.L)";
        case 0xC6: return "EDSFF E3 (E3.S, E3.L)";

        default:   return NULL;
    }
}

const char *
axl_smbios_slot_width_str(
    uint8_t bw
    )
{
    /* SMBIOS spec Table 11 — Slot Data Bus Width. */
    switch (bw) {
        case 0x03: return "8b";
        case 0x04: return "16b";
        case 0x05: return "32b";
        case 0x06: return "64b";
        case 0x07: return "128b";
        case 0x08: return "1x";
        case 0x09: return "2x";
        case 0x0A: return "4x";
        case 0x0B: return "8x";
        case 0x0C: return "12x";
        case 0x0D: return "16x";
        case 0x0E: return "32x";
        default:   return NULL;
    }
}

const char *
axl_smbios_slot_usage_str(
    uint8_t cu
    )
{
    /* SMBIOS spec Table 12 — Current Usage. Strings match the spec
     * exactly so callers see canonical decoding regardless of which
     * BIOS family produced the table. Vendor-specific renderings
     * (e.g. an OEM that wants "CPU NOT INSTALLED" for socket-
     * associated 0x05 slots) belong in consumer code: read the raw
     * byte off AxlSmbiosSystemSlot.current_usage and translate. */
    switch (cu) {
        case 0x01: return "Other";
        case 0x02: return "Unknown";
        case 0x03: return "Empty";
        case 0x04: return "InUse";
        case 0x05: return "Unavailable";
        default:   return NULL;
    }
}

// ---------------------------------------------------------------------------
// SMBIOS Type 3 (Chassis) classification
//
// Reference: SMBIOS spec Table 17 + ADDF/Libs/SAL/AddfSAL.cpp:fIsNotebookSmbios
// for the bucket assignments.
//
// Pitfalls worth a defense-in-tests:
//   - 0x18 ("Sealed-case PC") is desktop/SFF, NOT server.
//   - 0x23 ("Mini PC" per SMBIOS 3.7) is EMBEDDED, NOT IoT Gateway.
// ---------------------------------------------------------------------------

AxlSmbiosChassisClass
axl_smbios_chassis_class(
    uint8_t type
    )
{
    /* Strip the 0x80 lock bit — the Type 3 type field stores it
     * inline, but classification is on the low 7 bits. */
    uint8_t t = type & 0x7F;

    switch (t) {
        case 0x00:                /* (invalid / not set) */
        case 0x02:                /* Unknown (per spec) */
            return AXL_SMBIOS_CHASSIS_CLASS_UNKNOWN;

        case 0x03:                /* Desktop */
        case 0x04:                /* Low Profile Desktop */
        case 0x05:                /* Pizza Box (legacy desktop variant) */
        case 0x06:                /* Mini Tower */
        case 0x07:                /* Tower */
        case 0x18:                /* Sealed-case PC — desktop/SFF, NOT server.
                                   * Worth a defense in tests because naive "is server"
                                   * classifiers based on "anything 0x17-0x1F" would
                                   * misclassify it. */
            return AXL_SMBIOS_CHASSIS_CLASS_DESKTOP;

        case 0x08:                /* Portable */
        case 0x09:                /* LapTop */
        case 0x0A:                /* Notebook */
        case 0x0C:                /* Docking Station */
        case 0x0E:                /* Sub Notebook */
        case 0x1E:                /* Tablet */
        case 0x1F:                /* Convertible */
        case 0x20:                /* Detachable */
            return AXL_SMBIOS_CHASSIS_CLASS_NOTEBOOK;

        case 0x17:                /* Rack Mount Chassis */
        case 0x19:                /* Multi-system Chassis */
        case 0x1B:                /* Advanced TCA — server-class.
                                   * (some downstream decoders historically
                                   * label this "Pizza Box"; per the
                                   * canonical SMBIOS spec / EDK2,
                                   * Pizza Box is 0x05 and 0x1B is
                                   * Advanced TCA. Both are server form
                                   * factors, so the bucket is right
                                   * either way.) */
        case 0x1C:                /* Blade */
        case 0x1D:                /* Blade Enclosure */
            return AXL_SMBIOS_CHASSIS_CLASS_SERVER;

        case 0x21:                /* IoT Gateway */
        case 0x22:                /* Embedded PC */
        case 0x23:                /* "Mini PC" per SMBIOS 3.7 — embedded form factor.
                                   * Worth a defense in tests because naive "0x21 only"
                                   * IoT-Gateway classifiers would miss it. */
            return AXL_SMBIOS_CHASSIS_CLASS_EMBEDDED;

        /* Recognized chassis types outside the above buckets:
         *   0x01 Other, 0x0B Hand Held, 0x0D All in One, 0x0F Space-saving,
         *   0x10 Lunch Box, 0x11 Main Server Chassis (legacy), 0x12-0x16
         *   expansion/raid/peripheral chassis, 0x1A Compact PCI, 0x24 Stick PC. */
        default:
            return AXL_SMBIOS_CHASSIS_CLASS_OTHER;
    }
}
