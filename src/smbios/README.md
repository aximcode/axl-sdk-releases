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
records (BIOS, System, Baseboard, Chassis, Processor, Memory Device,
IPMI Device Info, end-of-table sentinel, etc.). For rarely-used types
(SMBIOS defines ~45), bare numbers are still fine.

## Reading the Spec Version

```c
unsigned char major, minor;
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

## Notes

- String accessors share a static 128-byte buffer; copy the result if
  you need to retain it across another call.
- Only the first matching record is returned by `axl_smbios_find`; a
  future addition (`axl_smbios_next`) would allow walking all records
  of a given type.
