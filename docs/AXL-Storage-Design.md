# AXL Storage Access (NVMe / ATA / SCSI) + SMART — Design

**Status:** proposed. Three new Platform Access Modules that give UEFI
apps typed identity and health (SMART) access to the storage devices the
firmware enumerates, plus a normalized cross-transport health view — the
`smartctl`-equivalent the SDK is missing today.

This document covers the three transport modules and the unifying health
layer:

1. **`AxlNvme`** — NVM Express admin access (Identify + Get Log Page),
   over `EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL`. *Phase 1 — most tractable;
   the pass-thru struct is already proven in `tools/mkfixture.c`.*
2. **`AxlAta`** — ATA/SATA task-file access (IDENTIFY DEVICE + SMART),
   over `EFI_ATA_PASS_THRU_PROTOCOL`.
3. **`AxlScsi`** — SCSI/SAS CDB access (INQUIRY + LOG SENSE), over
   `EFI_EXT_SCSI_PASS_THRU_PROTOCOL`.
4. **`AxlSmart`** — a thin synthesis layer: one normalized
   `AxlSmartHealth` struct each transport populates, so a consumer asks
   "is this disk healthy, how hot, how worn" without knowing the wire
   protocol. This is what makes the set `smartctl`-like rather than three
   disjoint pass-thru bindings.

**Explicitly out of scope** (see *Non-goals*): RAID/HBA management
(`storelib`), block read/write, partition/GPT parsing, AML, and
destructive device commands (FORMAT / SANITIZE / firmware download) as
typed wrappers. The raw pass-thru entry point can carry any command a
power user assembles, but the typed surface is read-and-health only.

---

## Where this sits relative to what exists

| Today | This design |
|-------|-------------|
| `AxlBlock` — enumerates `EFI_BLOCK_IO` handles + media geometry (capacity, block size, removable). Logical-block view, no device identity or health. | unchanged; the storage modules are the *device* view that complements it. |
| NVMe pass-thru struct hand-written in `axl-uefi-extra.h`, consumed only by `tools/mkfixture.c` (Identify Controller/Namespace for fixture capture). | promoted into a real `AxlNvme` module with a public API; `mkfixture` becomes a consumer of it (DRY — one NVMe Identify path, not two). |
| GUIDs defined but no structs/API: `AtaPassThru`, `ExtScsiPassThru`, `StorageSecurityCommand`, `EraseBlock`. | `AtaPassThru` / `ExtScsiPassThru` get protocol structs (generated where the spec HTML permits, else hand-written in `axl-uefi-extra.h`, the same fallback the NVMe struct and the HwInfo readers used). |
| `AxlPci` class decode names "Mass storage controller" / "RAID" / "NVM controller". | inventory labels only; unchanged. |

**`AxlBlock` correlation.** A storage device (an NVMe namespace, an ATA
device, a SCSI LUN) usually also publishes an `EFI_BLOCK_IO` handle. The
modules expose each device's UEFI device-path so a consumer can correlate
"the disk SMART says is failing" with "the `AxlBlock` handle of capacity
N" by matching device paths. A first cut does not auto-join them; the
normalized health struct simply carries the device path and the
transport's own identity (model/serial), which is what an operator keys
on anyway.

---

## Design conventions (inherited)

These are Platform Access Modules and follow the shared shape documented
in `AXL-Design.md` (AxlIpmi is the reference): opaque session handle,
**auto-detect at construction** (locate the pass-thru protocol; no naming
the interface), a **raw command entry point** for edge cases plus typed
wrappers for the common path, a transport-free **public API in standard C
types** (no UEFI types leak; header joins the `<axl.h>` umbrella), and a
**dogfood tool in `tools/`**. Module file layout mirrors the AxlIpmi
roster (`src/<family>/axl-<family>.c` + `-internal.h` + `-cmd.c` +
`-format.c` + `README.md`).

One deliberate departure from the AxlIpmi single-session model: storage
has **many devices per controller and many controllers per system**, so
the iteration shape matches `AxlBlock`/`AxlUsb` (walk handles / walk
devices) rather than one global session. See *Iteration model* below.

---

## Architecture

```
            ┌─────────────────────────────────────────────┐
            │ AxlSmart  (axl/axl-smart.h)                  │
            │   axl_smart_health(dev, &AxlSmartHealth)     │  normalized,
            │   one struct, transport-agnostic             │  read-only
            └───────────────┬─────────────────────────────┘
                            │ dispatches by transport
        ┌───────────────────┼───────────────────────┐
        ▼                   ▼                       ▼
 ┌─────────────┐     ┌─────────────┐         ┌─────────────┐
 │  AxlNvme    │     │  AxlAta     │         │  AxlScsi    │
 │ identify    │     │ identify    │         │ inquiry     │
 │ +health log │     │ +SMART data │         │ +log sense  │
 │ +raw admin  │     │ +raw taskfl │         │ +raw CDB    │
 └──────┬──────┘     └──────┬──────┘         └──────┬──────┘
        ▼                   ▼                       ▼
 NvmExpressPassThru   AtaPassThru          ExtScsiPassThru   (UEFI protocols)
```

Each transport module is independently usable (a consumer that only cares
about NVMe links only `AxlNvme`). `AxlSmart` is a thin rollup that depends
on all three but adds no transport knowledge of its own beyond "given a
device, call the right module and map its native health into the common
struct."

### Iteration model

Mirror `AxlBlock`'s opaque-cursor walk, one level deeper (controller →
device):

```c
// Walk every storage device the firmware exposes, any transport.
AxlStorageDev *dev = NULL;
while ((dev = axl_storage_next(dev)) != NULL) {
    AxlStorageInfo info;
    if (axl_storage_get_info(dev, &info) == AXL_OK)
        // info.transport (NVME/ATA/SCSI), model, serial, device_path, ...
}
```

`AxlStorageDev` is an opaque per-device handle owned by the walk (like
`AxlBlock`'s `AxlHandle`); it names the controller's pass-thru protocol +
the device address (NVMe namespace id / ATA port+PMPN / SCSI target+lun).
The transport modules also expose their own native walk
(`axl_nvme_next` / `axl_ata_next` / `axl_scsi_next`) for callers that want
transport-specific commands. `axl_storage_next` is the union view that
`AxlSmart` and the `smart` tool iterate.

The union walk (`axl_storage_next` / `AxlStorageDev`) is part of `AxlSmart`
(Phase 4); the per-transport `axl_<t>_next` walks ship with their modules.
(Names firm up when the `AxlSmart` contract is drafted.)

### Raw + typed, read-first

Each module exposes, like `axl_ipmi_raw`:

- **`axl_<t>_passthru(dev, …command…, …response buffer…)`** — submit an
  arbitrary command/CDB/task-file. The escape hatch for anything the
  typed layer doesn't cover. **This is where a destructive command would
  go; the module does not stop you, but ships no typed wrapper that
  issues one.**
- **Typed read wrappers** — `axl_nvme_identify_controller`,
  `axl_nvme_smart`, `axl_ata_identify`, `axl_ata_smart_read_data`,
  `axl_scsi_inquiry`, `axl_scsi_log_sense`, etc. All read-only.
- **`axl_<t>_self_test(dev, kind)`** — the one *active* typed command,
  included in each module's first cut because it is the defining smartctl
  operation (`-t short`/`long`). It is **non-destructive** (the device
  exercises itself; no host data is touched), but it *is* a write to the
  device, so it is gated behind an explicit call and never implied by a
  health read. NVMe: Device Self-test (Admin opcode 0x14) + Get Log Page
  0x06 for results; ATA: SMART EXECUTE OFF-LINE IMMEDIATE (0xD4) + READ
  LOG 0x06; SCSI: SEND DIAGNOSTIC + LOG SENSE self-test page 0x10. The
  truly destructive commands (FORMAT / SANITIZE / SECURE ERASE / firmware
  download) remain raw-pass-thru-only with no typed wrapper.

---

## The normalized health struct

The payoff layer. One struct, populated from each transport's native
source; fields that a transport cannot supply are marked absent rather
than guessed, via a **documented per-field sentinel** (cleaner at call
sites than a parallel `present` bitmask, and unambiguous because each
field's "unknown" value is reserved): `temperature_c == INT32_MIN`,
`power_on_hours`/`power_cycles`/`media_errors`/`data_units_*` `== UINT64_MAX`,
`percent_used == 0xFF`. `healthy` and `warning_flags` are always present
(every transport reports an overall pass/fail).

```c
typedef enum { AXL_STORAGE_NVME, AXL_STORAGE_ATA, AXL_STORAGE_SCSI }
    AxlStorageTransport;

typedef struct {
    AxlStorageTransport transport;
    char     model[64];          // INQUIRY product / ATA model / NVMe MN
    char     serial[32];         // serial number
    char     firmware[16];       // firmware revision

    bool     healthy;            // overall pass/fail (the headline answer)
    uint32_t warning_flags;      // transport-normalized critical-warning bits

    int32_t  temperature_c;      // current composite temperature, or INT32_MIN
    uint64_t power_on_hours;     // or UINT64_MAX if unknown
    uint64_t power_cycles;       // or UINT64_MAX
    uint8_t  percent_used;       // SSD wear 0..255 (NVMe % used; ATA attr), 0xFF unknown
    uint64_t media_errors;       // NVMe media errors / ATA reallocated / SCSI IE count
    uint64_t data_units_read;    // 1000*512-byte units (NVMe convention; ATA/SCSI normalized to it)
    uint64_t data_units_written;
} AxlSmartHealth;
```

Per-transport source of each field:

| Field | NVMe | ATA | SCSI |
|-------|------|-----|------|
| healthy | Critical Warning byte == 0 (Log 0x02) | SMART RETURN STATUS (0xDA) | Informational Exceptions log (0x2F) ASC==0 |
| temperature_c | Composite Temperature (Log 0x02, Kelvin) | attr 194/190 | Temperature log page (0x0D) |
| power_on_hours | Log 0x02 | attr 9 | Start-Stop / IE log |
| percent_used | Log 0x02 "Percentage Used" | attr 177/231/233 | (often absent) |
| media_errors | Log 0x02 "Media and Data Integrity Errors" | attr 5 reallocated | IE log counters |
| model/serial/fw | Identify Controller (MN/SN/FR) | IDENTIFY DEVICE words | INQUIRY std + page 0x80 |

ATA vendor-attribute IDs are not fully standardized; the ATA mapping ships
a small built-in table for the well-known IDs (5, 9, 194, 177, …) and
leaves the rest accessible only through the raw attribute dump. This is
the same honesty `smartctl` applies (it ships a drive database; we ship
the universal subset and expose raw for the rest).

---

## Transport detail

### AxlNvme (Phase 1)

- Protocol: `EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL` (struct already in
  `axl-uefi-extra.h`). Controller handle → `GetNextNamespace` walk.
- Admin commands used: **Identify** (CNS 1 controller, CNS 0 namespace),
  **Get Log Page** LID **0x02** (SMART/Health — the whole health struct),
  optionally **0x01** (Error) and **0x06** (Device Self-test) later.
- This subsumes `mkfixture`'s private Identify path; that tool refactors
  onto `axl_nvme_identify_*`.
- QEMU: OVMF + `-device nvme` exposes the pass-thru protocol and answers
  Identify + the SMART log → **unit/integration testable without
  hardware** (the strongest reason NVMe is Phase 1).

### AxlAta (Phase 2)

- Protocol: `EFI_ATA_PASS_THRU_PROTOCOL` (GUID present; struct to add).
  `GetNextPort` / `GetNextDevice` walk; `Passthru` carries an
  `EFI_ATA_PASS_THRU_COMMAND_PACKET` (ATA task file).
- Commands: **IDENTIFY DEVICE** (0xEC), **SMART** (0xB0) feature
  **READ DATA** (0xD0), **READ THRESHOLDS** (0xD1), **RETURN STATUS**
  (0xDA, overall health), **READ LOG** (0xD5) for self-test/error logs.
- ATA attribute decode: built-in table for the universal IDs; raw dump
  for the rest.
- QEMU: AHCI (`-device ich9-ahci` + `-device ide-hd`) publishes
  AtaPassThru; IDENTIFY works under OVMF. SMART data emulation is
  thinner — validate the framing in QEMU, mark attribute-value fidelity
  real-hardware-tested (see *Testing*).

### AxlScsi (Phase 3)

- Protocol: `EFI_EXT_SCSI_PASS_THRU_PROTOCOL` (GUID present; struct to
  add). `GetNextTargetLun` walk; `PassThru` carries a CDB.
- Commands: **INQUIRY** (0x12, std + VPD page 0x80 serial / 0x83 device
  id), **LOG SENSE** (0x4D) for the **Informational Exceptions** page
  (0x2F, SMART-equivalent health), **Temperature** (0x0D), **Self-Test**
  (0x10); **READ CAPACITY** for geometry cross-check.
- QEMU: `-device scsi-hd` on a virtio/LSI SCSI HBA publishes
  ExtScsiPassThru; INQUIRY + basic LOG SENSE are exercisable.

---

## Testing

Per the test-first workflow (`CLAUDE.md`) and the UEFI firmware-lifecycle
hazards memo:

- **Parser-level unit tests, no hardware.** The value-add of these
  modules is *decoding* fixed-layout binary (NVMe Identify 4 KiB struct,
  the 512-byte SMART log, ATA IDENTIFY 256 words + the 512-byte SMART data
  page, SCSI INQUIRY + log pages). These decoders are pure functions over
  byte buffers → exact-string / exact-value unit tests against captured
  real-device blobs committed as test vectors. **This is where most tests
  live** and it needs no QEMU device.
- **QEMU integration**, per transport, against the emulated device
  (NVMe strongest; AHCI/SCSI for framing). `AXL_TLS`-style segregated:
  these need no TLS, but NVMe/AHCI/SCSI devices must be added to the
  `run-qemu` command — likely a `test-storage-qemu.sh` that adds
  `-device nvme,...` etc. and asserts the typed reads succeed.
- **Safe negatives only** for the pass-thru lifecycle (NULL / bad
  address / protocol-absent), never a malformed command that wedges
  firmware. Pass-thru with a bogus CDB is real-hardware territory; the
  destructive-command surface is intentionally absent.
- Capture real-device blobs (an NVMe SMART log, an ATA SMART page, a SCSI
  IE log) via the dogfood tool on lab hardware → commit as
  `test/data/*.bin` vectors so the decoders are pinned cross-arch without
  the device.

---

## Non-goals

- **RAID / HBA management (`storelib`).** No MegaRAID/Broadcom controller
  access, virtual/physical-drive or enclosure enumeration, or array
  config. UEFI surfaces nothing generic; `storelib` drives vendor MFI/MPI
  mailboxes over PCI BARs — a separate, per-controller-family,
  real-hardware-only effort tracked elsewhere if ever pursued.
- **Block read/write** — `AxlBlock` stays enumeration-only; raw block I/O
  is a distinct concern.
- **Partition / GPT parsing** — not here.
- **Destructive typed commands** — FORMAT NVM, SANITIZE, SECURE ERASE,
  firmware download/commit. Reachable only by hand-assembling a raw
  pass-thru command; no typed wrapper ships, and the docs say why.
- **AML / ACPI device-method storage** — out, like `AxlAcpi`.
- **`StorageSecurityCommand` / `EraseBlock`** (Opal, sanitize) — GUIDs
  exist; deferred, and destructive, so not in the first cut.

---

## Phases

Each phase is the standard module slice (header contract → contract-first
review → test-first impl → dogfood tool → README), shippable on its own.

- **Phase 1 — AxlNvme.** Promote the pass-thru struct; `axl_nvme_next` /
  `_identify_controller` / `_identify_namespace` / `_smart` /
  `_self_test` (+ Log 0x06 results) / `_passthru`. Refactor `mkfixture`
  onto it. Dogfood: extend `sysinfo` or a new `tools/nvme` (`nvme id-ctrl`
  / `smart-log` / `device-self-test` shape).
- **Phase 2 — AxlAta.** Add the AtaPassThru struct; IDENTIFY + SMART READ
  DATA / RETURN STATUS + EXECUTE OFF-LINE (self-test) + the
  universal-attribute table.
- **Phase 3 — AxlScsi.** Add the ExtScsiPassThru struct; INQUIRY +
  LOG SENSE (IE / temperature) + SEND DIAGNOSTIC (self-test).
- **Phase 4 — AxlSmart + `tools/smart`.** The normalized `AxlSmartHealth`
  rollup and the `smartctl`-analog tool over `axl_storage_next`. Lands
  after at least Phase 1 so it has one transport to dispatch to, then
  grows as 2/3 land.

`AxlSmartHealth` and the union walk are designed up front (this doc) so
the three transport modules expose the fields it needs from day one,
rather than being retrofitted.

---

## Resolved decisions

1. **Union walk lives in `AxlSmart`** — `axl_storage_next` is part of the
   `AxlSmart` rollup (the only thing that needs the union view); the three
   transport modules keep their own native walks.
2. **Absent-field encoding: per-field sentinels** (documented above) — no
   parallel `present` bitmask.
3. **`mkfixture` refactors onto `AxlNvme` in Phase 1** — it is the second
   consumer that justifies the module and is already
   real-hardware-exercised, so the refactor doubles as validation (one
   NVMe Identify path, not two).
4. **Self-test ships in each module's first cut** — the one active
   (non-destructive) typed command, gated behind an explicit call. Truly
   destructive commands stay raw-only.

## Open question (for contract-first review)

- **Generate vs hand-write** the AtaPassThru / ExtScsiPassThru structs —
  try the spec-HTML generator first (`uefi-manifest.json5`), fall back to
  `axl-uefi-extra.h` as the NVMe struct did. Resolved when Phase 2/3 start.
