SMBIOS table lookup and string extraction.

The SMBIOS spec defines a set of typed records — BIOS info, System info,
Baseboard, Processor, Memory Array, etc. — that firmware publishes via
the UEFI SMBIOS protocol. AxlSmbios provides a small, stable API for
walking that table and pulling strings out of records.

Headers:

- `<axl/axl-smbios.h>` — Table lookup, header walking, string extraction

## Overview

```c
#include <axl.h>
#include <axl/axl-smbios.h>

// Find the BIOS Information record (SMBIOS Type 0)
AxlSmbiosHeader *hdr = axl_smbios_find(AXL_SMBIOS_TYPE_BIOS_INFO);
if (hdr) {
    const char *vendor  = axl_smbios_get_string_utf8(hdr, 1);  // vendor
    const char *version = axl_smbios_get_string_utf8(hdr, 2);  // version
    axl_printf("BIOS: %s %s\n", vendor, version);
}
```

The table header is a fixed-length record followed by a
double-`NUL`-terminated list of ASCII strings referenced by 1-based
index from within the record. `axl_smbios_get_string_utf8` resolves
those string indices to plain C strings.

## Iterating All Records

To visit every record the firmware published — regardless of type:

```c
AxlSmbiosHeader *h = NULL;
while ((h = axl_smbios_next(h)) != NULL) {
    axl_printf("Type %u  Handle 0x%04x  Length %u\n",
               h->Type, h->Handle, h->Length);
}
```

To iterate every record of a **specific** type (useful for enumerating
DIMMs, CPU cores, cache levels, etc.), use `axl_smbios_find_next`:

```c
// Walk every Memory Device (Type 17) — one per DIMM slot
AxlSmbiosHeader *h = NULL;
while ((h = axl_smbios_find_next(AXL_SMBIOS_TYPE_MEMORY_DEVICE, h)) != NULL) {
    // process DIMM record...
}
```

## Common Type Constants

Use the `AXL_SMBIOS_TYPE_*` enum instead of bare type numbers — the
code reads better and greps cleaner. The enum covers the most common
records (BIOS, System, Baseboard, Chassis, Processor, Cache, Port
Connector, System Slots, OEM Strings, BIOS Language, Physical Memory
Array, Memory Device, Memory Array Mapped Address, Memory Device
Mapped Address, System Boot, IPMI Device, Onboard Devices Extended,
Management Host Interface, end-of-table sentinel, etc.). For
rarely-used types (SMBIOS defines ~45), bare numbers are still fine.

## Typed Readers

For records the SDK has typed-reader support for, prefer them over
hand-walking the raw header — they're length-aware (won't UB on
short records firmware from older spec versions emits) and surface
"not published" via documented sentinels per field.

```c
AxlSmbiosBiosInfo bi;
if (axl_smbios_read_bios_info(&bi) == 0) {
    axl_printf("%s %s (%u.%u)\n",
        bi.vendor, bi.version, bi.major_release, bi.minor_release);
}

AxlSmbiosHeader *h = NULL;
while ((h = axl_smbios_find_next(AXL_SMBIOS_TYPE_SYSTEM_SLOTS, h))) {
    AxlSmbiosSystemSlot sl;
    if (axl_smbios_read_system_slot(h, &sl) == 0) {
        axl_printf("Slot %s: bus %02x:%02x\n",
                   sl.designation, sl.bus, sl.device_function);
    }
}
```

Available readers:

| Type | Reader | Struct |
|------|--------|--------|
| 0  | `axl_smbios_read_bios_info`             | `AxlSmbiosBiosInfo` |
| 1  | `axl_smbios_read_system_info`           | `AxlSmbiosSystemInfo` |
| 2  | `axl_smbios_read_baseboard`             | `AxlSmbiosBaseboardInfo` |
| 3  | `axl_smbios_read_chassis`               | `AxlSmbiosChassisInfo` |
| 4  | `axl_smbios_read_processor`             | `AxlSmbiosProcessorInfo` |
| 8  | `axl_smbios_read_port_connector`        | `AxlSmbiosPortConnector` |
| 9  | `axl_smbios_read_system_slot`           | `AxlSmbiosSystemSlot` |
| 11 | `axl_smbios_read_oem_strings`           | `AxlSmbiosOemStrings` |
| 16 | `axl_smbios_read_physical_memory_array` | `AxlSmbiosPhysicalMemoryArray` |
| 17 | `axl_smbios_read_memory_device`         | `AxlSmbiosMemoryDevice` |
| 19 | `axl_smbios_read_memory_array_map`      | `AxlSmbiosMemoryArrayMap` |
| 20 | `axl_smbios_read_memory_device_map`     | `AxlSmbiosMemoryDeviceMap` |
| 38 | `axl_smbios_read_ipmi_device_info`      | `AxlSmbiosIpmiDeviceInfo` |
| 41 | `axl_smbios_read_onboard_device_ext`    | `AxlSmbiosOnboardDeviceExt` |
| 42 | `axl_smbios_read_host_interface`        | `AxlSmbiosHostInterface` |

## Reading the Spec Version

```c
uint8_t major, minor;
if (axl_smbios_version(&major, &minor) == 0) {
    axl_printf("SMBIOS %u.%u\n", major, minor);
}
```

Useful for gating on fields that were added in later spec revisions
(e.g. several Type 17 Memory Device fields are post-2.7).

## Consumers

- [`tools/sysinfo`](https://github.com/aximcode/axl-sdk-releases/blob/main/tools/sysinfo.c)
  uses SMBIOS to report BIOS, system, baseboard, and processor
  inventory.
- [`src/ipmi/axl-ipmi.c`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/ipmi/axl-ipmi.c)
  probes SMBIOS Type 38 (IPMI Device Information) during transport
  auto-detection.

## String Accessors

Two flavors:

- `axl_smbios_get_string_utf8(hdr, idx)` — returns a pointer directly
  into the SMBIOS table memory (which persists for the life of the
  app via the UEFI configuration table). Reentrant — safe to call
  twice in one `printf` argument list.
- `axl_smbios_copy_string_utf8(hdr, idx, buf, buf_size)` — copies
  into a caller buffer, length-bounded with safe truncation, always
  NUL-terminates. Returns the byte count written. Use this when you
  need a writable copy or want explicit truncation control.

The legacy `axl_smbios_get_string(hdr, idx)` returns UCS-2 in a
static buffer and is kept for back-compat; new code should use the
UTF-8 variants.

### Type 11 OEM Strings — convenience accessor

For the common "fetch the OEM string at index N" pattern,
`axl_smbios_get_oem_string(idx, buf, buf_cap, *required)` walks
every Type 11 record in firmware order, treats their strings as
one contiguous 1-based list, and copies the requested string into
the caller's buffer:

```c
char   product_id[64];
size_t need = 0;
if (axl_smbios_get_oem_string(/*1-based*/ 1, product_id,
                              sizeof(product_id), &need) == 0) {
    axl_printf("OEM string 1: %s\n", product_id);
}
```

A too-small buffer returns -1 *without* copying — the call refuses
to truncate silently. When `*required` is non-NULL it is set to
the byte count needed (string length + NUL), letting callers
size a follow-up allocation exactly:

```c
size_t need = 0;
if (axl_smbios_get_oem_string(1, NULL, 0, &need) == -1 && need > 0) {
    char *buf = axl_malloc(need);
    axl_smbios_get_oem_string(1, buf, need, NULL);
    /* ... */
    axl_free(buf);
}
```

Most platforms ship a single Type 11 record; the multi-record path
is robustness for firmware that splits OEM strings across records.
Other failure modes (no Type 11 record, index out of range, bad
buffer) leave `*required` unchanged. Callers that want raw
`(count, strings[])` arrays can still use
`axl_smbios_read_oem_strings(hdr, *out)` after locating a header
with `axl_smbios_find(AXL_SMBIOS_TYPE_OEM_STRINGS)`.

## Spec-Value Decoders

Type 9 (System Slots) carries enumerated values for slot type, bus
width, and current usage. The SDK ships pure-spec lookup tables so
consumers don't reinvent the decoder (and so spec additions like
EDSFF E1.S/E1.L, OCP NIC 3.0, PCIe Gen 5/6 propagate via SDK bumps):

```c
AxlSmbiosSystemSlot sl;
if (axl_smbios_read_system_slot(h, &sl) == 0) {
    const char *t = axl_smbios_slot_type_str(sl.slot_type);   // "PCIe Gen 5 x16"
    const char *w = axl_smbios_slot_width_str(sl.slot_data_bus_width); // "16x"
    const char *u = axl_smbios_slot_usage_str(sl.current_usage);       // "InUse"
    axl_printf("%s  %s %s%s\n", sl.designation,
               t ? t : "(unknown)",
               w ? w : "(unknown)",
               u ? u : "(unknown)");
}
```

Unknown values return NULL so callers can fall back to printing raw
"0x%02X". Strings match SMBIOS 3.7 Table 12 exactly (e.g.
`slot_usage_str(0x05)` returns "Unavailable"); vendor-specific
renderings (e.g. an OEM that wants "CPU NOT INSTALLED" for
socket-associated 0x05 slots) belong in consumer code, which can
read `AxlSmbiosSystemSlot.current_usage` directly and translate.

`axl_smbios_chassis_class(type)` classifies the Type 3 chassis byte
into a coarse `AxlSmbiosChassisClass` (DESKTOP / NOTEBOOK / SERVER /
EMBEDDED / OTHER / UNKNOWN). Two pitfalls worth knowing:
0x18 ("Sealed-case PC") is DESKTOP, not server; 0x23 ("Mini PC" per
SMBIOS 3.7) is EMBEDDED, not IoT Gateway. Vendor-specific server
detection (sysId tables, PCI audio probes) lives in consumer code
on top.

## Walking the Table

`axl_smbios_find(type)` returns the first record of a given type;
`axl_smbios_find_next(type, prev)` walks all records of that type
(pass NULL for the first call); `axl_smbios_next(prev)` walks every
record regardless of type. All terminate at the Type 127
end-of-table sentinel.
