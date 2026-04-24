/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smbios.c
    SMBIOS helpers — string extraction and table lookup.
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

    if (smbios_get_range(&start, &end) != 0) {
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
    unsigned char *major,
    unsigned char *minor
    )
{
    SMBIOS3_STRUCTURE_TABLE *smbios3 = NULL;
    SMBIOS_STRUCTURE_TABLE  *smbios2 = NULL;

    for (size_t i = 0; i < axl_st()->NumberOfTableEntries; i++) {
        EFI_GUID *guid = &axl_st()->ConfigurationTable[i].VendorGuid;
        if (axl_guid_equal(guid, &SMBIOS3_TABLE_GUID)) {
            smbios3 = axl_st()->ConfigurationTable[i].VendorTable;
        } else if (axl_guid_equal(guid, &SMBIOS_TABLE_GUID)) {
            smbios2 = axl_st()->ConfigurationTable[i].VendorTable;
        }
    }

    if (smbios3 != NULL) {
        if (major != NULL) { *major = smbios3->MajorVersion; }
        if (minor != NULL) { *minor = smbios3->MinorVersion; }
        return 0;
    }
    if (smbios2 != NULL) {
        if (major != NULL) { *major = smbios2->MajorVersion; }
        if (minor != NULL) { *minor = smbios2->MinorVersion; }
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Typed record accessors
// ---------------------------------------------------------------------------

/* Raw record layouts. We define the fields AXL cares about inline rather
   than pulling in EDK2's IndustryStandard/SmBios.h, keeping this file
   decoupled from the EDK2 package layout. Field offsets match the SMBIOS
   specification. */

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

int
axl_smbios_read_bios_info(
    AxlSmbiosBiosInfo *out
    )
{
    if (out == NULL) { return -1; }
    SmbType0 *t = (SmbType0 *)axl_smbios_find(AXL_SMBIOS_TYPE_BIOS_INFO);
    if (t == NULL) { return -1; }

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
    return 0;
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
    if (out == NULL) { return -1; }
    SmbType1 *t = (SmbType1 *)axl_smbios_find(AXL_SMBIOS_TYPE_SYSTEM_INFO);
    if (t == NULL) { return -1; }

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
    return 0;
}

int
axl_smbios_read_baseboard(
    AxlSmbiosBaseboardInfo *out
    )
{
    if (out == NULL) { return -1; }
    SmbType2 *t = (SmbType2 *)axl_smbios_find(AXL_SMBIOS_TYPE_BASEBOARD);
    if (t == NULL) { return -1; }
    out->manufacturer  = axl_smbios_get_string_utf8(&t->Hdr, t->Manufacturer);
    out->product_name  = axl_smbios_get_string_utf8(&t->Hdr, t->ProductName);
    out->version       = axl_smbios_get_string_utf8(&t->Hdr, t->Version);
    out->serial_number = axl_smbios_get_string_utf8(&t->Hdr, t->SerialNumber);
    out->asset_tag     = axl_smbios_get_string_utf8(&t->Hdr, t->AssetTag);
    return 0;
}

int
axl_smbios_read_chassis(
    AxlSmbiosChassisInfo *out
    )
{
    if (out == NULL) { return -1; }
    SmbType3 *t = (SmbType3 *)axl_smbios_find(AXL_SMBIOS_TYPE_CHASSIS);
    if (t == NULL) { return -1; }
    out->manufacturer  = axl_smbios_get_string_utf8(&t->Hdr, t->Manufacturer);
    out->version       = axl_smbios_get_string_utf8(&t->Hdr, t->Version);
    out->serial_number = axl_smbios_get_string_utf8(&t->Hdr, t->SerialNumber);
    out->asset_tag     = axl_smbios_get_string_utf8(&t->Hdr, t->AssetTag);
    out->type          = t->Type & 0x7F;   /* strip the 0x80 lock-bit flag */
    return 0;
}

int
axl_smbios_read_processor(
    AxlSmbiosHeader          *hdr,
    AxlSmbiosProcessorInfo   *out
    )
{
    if (out == NULL || hdr == NULL || hdr->Type != AXL_SMBIOS_TYPE_PROCESSOR) {
        return -1;
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
    return 0;
}

int
axl_smbios_read_memory_device(
    AxlSmbiosHeader          *hdr,
    AxlSmbiosMemoryDevice    *out
    )
{
    if (out == NULL || hdr == NULL || hdr->Type != AXL_SMBIOS_TYPE_MEMORY_DEVICE) {
        return -1;
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
    return 0;
}

int
axl_smbios_get_system_uuid(
    uint8_t out[16]
    )
{
    AxlSmbiosSystemInfo sys;
    if (axl_smbios_read_system_info(&sys) != 0) { return -1; }
    if (!sys.has_uuid) { return -1; }
    for (size_t i = 0; i < 16; i++) { out[i] = sys.uuid[i]; }
    return 0;
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
    if (out == NULL) { return -1; }
    AxlSmbiosHeader *hdr = axl_smbios_find(AXL_SMBIOS_TYPE_IPMI_DEVICE_INFO);
    if (hdr == NULL || hdr->Length < 0x10) { return -1; }

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
    return 0;
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
        return -1;
    }

    /* Minimum length for the modern layout: header(4) + type(1) + data_len(1)
       + proto_count(1) = 7 bytes. The old pre-3.0 layout is 5 bytes and not
       supported here. */
    if (hdr->Length < 7) { return -1; }

    const uint8_t *base = (const uint8_t *)hdr;
    const uint8_t *end  = base + hdr->Length;

    out->interface_type     = base[0x04];
    out->interface_data_len = base[0x05];
    out->interface_data     = base + 0x06;

    /* Validate the interface_data fits inside the record. */
    const uint8_t *p = out->interface_data + out->interface_data_len;
    if (p + 1 > end) { return -1; }

    uint8_t proto_count = *p++;
    out->protocol_count = 0;

    for (uint8_t i = 0; i < proto_count && i < 8; i++) {
        if (p + 2 > end) { return -1; }
        uint8_t  type = p[0];
        uint8_t  dlen = p[1];
        if (p + 2 + dlen > end) { return -1; }
        out->protocols[i].protocol_type = type;
        out->protocols[i].data_len      = dlen;
        out->protocols[i].data          = p + 2;
        p += 2 + dlen;
        out->protocol_count++;
    }
    return 0;
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
                return 0;
            }
        }
    }
    return -1;
}
