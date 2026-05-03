JEDEC Serial Presence Detect (SPD) reader for DDR4/DDR5 DIMMs.

Header: `<axl/axl-spd.h>`. Lazy on first call: AxlSpd opens an
@ref AxlSmbus session against whatever controller the platform
exposes (HC protocol, then I2C Master), probes the eight standard
SPD addresses (0x50..0x57), and hands off to the codec selected
by the memory-type byte at SPD offset 2.

DDR3 is intentionally out of scope for v1 — the motivating
consumer use case (Linux-side `dump-memory` parity on UEFI) is
DDR4/DDR5 server fleets. DDR3 is a small additional codec when
a consumer asks for it.

```c
uint8_t *slot = NULL;
while ((slot = axl_spd_next(slot)) != NULL) {
    AxlSpdInfo info;
    if (axl_spd_read(*slot, &info) == 0 && info.ddr_generation != 0) {
        axl_printf("Slot 0x%02X  DDR%u  %u MT/s  %llu bytes  ECC=%s\n",
                   *slot, info.ddr_generation, info.speed_mts,
                   (unsigned long long)info.capacity_bytes,
                   info.has_ecc ? "yes" : "no");
    }
}
```

Cursor iteration follows the established pattern (matches
`axl_smbios_find_next`, `axl_acpi_find_next`, `axl_pci_next`).
The cursor is module-global — UEFI is single-threaded so two
overlapping walks would corrupt each other; treat the iterator
as exclusive.

## Decoded info

```c
typedef struct {
    uint8_t   ddr_generation;     /* 4, 5; 0 = unknown */
    uint16_t  mfg_code_module;    /* JEP-106 (bank<<8) | id; 0 if unset */
    uint16_t  mfg_code_dram;
    uint8_t   mfg_location;
    uint16_t  mfg_year;           /* 2000 + BCD year byte */
    uint8_t   mfg_week;           /* 1..53 */
    uint32_t  serial;             /* 4 bytes BE */
    char      part_number[31];    /* trimmed ASCII */
    uint64_t  capacity_bytes;
    uint16_t  speed_mts;          /* JEDEC speed grade in MT/s */
    bool      has_ecc;
    bool      registered;
} AxlSpdInfo;
```

Manufacturer fields are exposed as **raw 16-bit JEP-106 codes**
— high byte is the continuation-bank index, low byte is the
position within that bank as physically stored on the SPD
(parity bit included). For human-readable rendering, the
`axl_spd_ids_*` API loads a curated JSON5 sidecar (`jedec.json5`)
into a process-global table and exposes lookup / format helpers
parallel to `axl_pci_ids_load` and `axl_pci_format_name`:

```c
if (axl_spd_ids_load(NULL) == AXL_SIDECAR_OK) {
    const char *name = axl_spd_vendor_name(info.mfg_code_module);
    /* NULL-safe; consumers fall back to numeric IDs */
}

char buf[AXL_SPD_NAME_COMPOSED_MAX];
axl_spd_format_name(info.mfg_code_module, buf, sizeof(buf));
/* known → "Micron Technology"; unknown → "0xCCCC" */
```

The handle API (`axl_spd_ids_open` / `_open_from_buffer` /
`_close` / `_vendor_name` / `_foreach_vendor` / `_format_name`)
mirrors AxlPciIds for consumers that want layered databases
(public + private overlay). Schema 1 only — JEDEC has no
subsystem dimension that motivated AxlPciIds's v1/v2 split.

`tools/memspd.c` is the reference consumer: at startup it calls
`axl_spd_ids_load(--jedec-file or NULL)` and renders manufacturer
fields via `axl_spd_vendor_name` and `axl_spd_format_name`.
`share/jedec.json5` carries ~30 common server vendors; the
file is hand-curated (no auto-converter — JEDEC publishes
JEP-106 as PDF, not a canonical text database).

## Wire-protocol notes

DDR4 (key byte `0x0C`) — EE1004 hub. Lower 256 bytes are accessed
directly. The upper 256 bytes (Module Manufacturing Information at
offsets 320..511) are reached after a Set Page Address (SPA) write
to the dedicated pseudo-slaves 0x36 (lower) / 0x37 (upper). AxlSpd
attempts SPA via `axl_smbus_write_byte`; standards-compliant EE1004
devices ignore the data byte. SPA failure is non-fatal — the codec
falls back to "lower page only" silently and the manufacturer fields
remain at zero. QEMU's smbus-eeprom doesn't model SPA, so wire-path
QEMU coverage is lower-page only.

DDR5 (key byte `0x12`) — SPD5118 hub. The 1024-byte address space
is divided into eight 128-byte pages (0..7); each page is read at
hub-relative offsets 0x80..0xFF after writing the page index to
MR11 (register `0x0B`). AxlSpd handles paging internally and
restores page 0 after every read — including mid-read failures
— so subsequent consumers see a predictable hub state.

## Pure-decoder API

`axl_spd_decode(buf, len, *out)` runs the same codec on a captured
buffer with no SMBus involvement. Useful for offline analysis
(decoded fields out of a raw blob captured on real hardware) and
for cross-arch unit testing — the AxlPlatform suite's DDR4/DDR5
decode tests use this entry point so coverage works the same on
x86 (where QEMU has SMBus) and AArch64 (where it doesn't).

Pair with `axl_spd_dump_raw(addr, *buf, cap, *len)` to capture a
DIMM's SPD bytes for off-box analysis.

## tools/memspd.c

```
memspd list                       — one-line summary per populated slot
memspd show <slot>                — decoded fields for one slot
memspd decode <slot>              — raw hex dump + decoded fields
```

Common flag: `--jedec-file <path>` overrides
`axl_spd_ids_load`'s autodiscovery (binary's directory →
`./jedec.json5`). When no sidecar loads, manufacturer fields
print as raw hex codes — the information still reaches the user,
just unresolved.

## Testing

Pure decoder coverage runs in `AxlTestPlatform` against canned
blobs synthesised by `test/data/gen-spd.py` — 21 tests, balanced
across x86 and AArch64.

Wire-path coverage lives in `test/integration/test-spd-qemu.sh`
(auxiliary, opt-out of the test-axl.sh ratchet). It depends on:
- A locally-patched QEMU built from
  `scripts/qemu-patches/0001-smbus-eeprom-add-memdev-link.patch`,
  which adds a `memdev=<link<memory-backend>>` property to the
  `smbus-eeprom` device. Stock QEMU 10.x rejects the argument.
- `SmbusHcShim.efi`, which publishes `EFI_I2C_MASTER_PROTOCOL`
  on top of QEMU's ICH9 SMBus controller (OVMF doesn't ship a
  SMBUS HC driver).

The test attaches the canned DDR4 blob to slot 0x50 and verifies
`memspd.efi` decodes it to "DDR4 / 8 GB / 2400 MT/s / ECC: yes".
