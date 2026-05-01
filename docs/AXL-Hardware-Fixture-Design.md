# AXL Hardware Fixture Capture & Replay — Design

Status: **Proposal**, no target release yet.

## Problem

`run-qemu.sh` is great for running .efi binaries against a generic
OVMF/AAVMF environment, but the firmware view it presents is
QEMU-synthetic: SMBIOS strings say `QEMU`, ACPI tables come from
QEMU's generators, the PCI topology is virtio-flavored. Tools like
`sysinfo`, `rfbrowse`, `axl-ipmi`, and the SMBIOS/IPMI/Redfish
parsers in axl-sdk only get exercised against real-world vendor
quirks when pointed at real hardware — which is slow, lab-bound,
and not always available.

We want a way to capture an environment from any UEFI machine
(server, workstation, laptop, VM — vendor-neutral) and replay
enough of it under QEMU that axl-sdk tools see something close to
the real platform's identity, configuration, and management surface.

## Scope: what we can and cannot fake

QEMU has first-class flags for some of this and nothing for the
rest. Setting expectations up front matters.

### Tractable (QEMU has direct flags)

- **SMBIOS** — `-smbios file=blob.bin` injects a complete SMBIOS
  table; `-smbios type=N,field=...` patches individual structures.
  Captured raw bytes from the real machine load cleanly.
- **ACPI** — `-acpitable file=ssdt.dat` (repeatable). Captured
  per-table `.dat` files load alongside QEMU's own.
- **IPMI BMC (standard commands)** — `-device ipmi-bmc-extern,
  chardev=...` connects QEMU's KCS frontend to an external
  simulator (OpenIPMI's `ipmi_sim`) speaking the real wire
  protocol. `-device ipmi-bmc-sim` runs a simpler built-in.
- **NIC variety** — already exposed via `--nic-model` in run-qemu.sh.

### Tractable with host-side mocks

- **Redfish** — QEMU has nothing. Two viable host-side approaches:
  1. **Static mock**: DMTF `Redfish-Mockup-Server` serves a captured
     directory tree of JSON responses. Simple, deterministic.
  2. **Dynamic mock**: OpenBMC firmware running in a sibling QEMU
     instance (the OpenBMC project's own dev workflow). Real bmcweb,
     real auth, no captured-data staleness. More moving parts.

  Either way, run-qemu.sh `--hostfwd 18443:443` (or similar) wires
  the guest to the host-side mock.

### Intractable

- **Vendor-specific PCI silicon** — Dell PERC, iDRAC USB-NIC,
  Broadcom OEM rebadges, HP Smart Array, etc. QEMU only emulates
  what it emulates; spoofing VID/DID on `pci-testdev` falls over
  the moment a driver issues real MMIO. SMBIOS/ACPI/Redfish let us
  fake the **inventory view**, but device-level interaction needs
  PCIe passthrough on real hardware. The capture tool will record
  PCI config space for inspection/manifest purposes only.

## Architecture

```
┌──────────────────────────┐                ┌─────────────────────────┐
│   Capture (UEFI tool)    │                │   Replay (run-qemu.sh)  │
│                          │                │                         │
│  sysinfo --capture DIR   │  ─── files ──> │  --fixture DIR          │
│   ├── smbios.bin         │                │   ├── -smbios file=...  │
│   ├── acpi/*.dat         │                │   ├── -acpitable file=  │
│   ├── pci.json           │                │   ├── (manifest only)   │
│   ├── redfish/**/*.json  │                │   ├── mock-server +     │
│   └── ipmi/*.bin         │                │   │   --hostfwd         │
│                          │                │   └── ipmi_sim +        │
│                          │                │       -device ipmi-...  │
└──────────────────────────┘                └─────────────────────────┘
```

### Capture: native UEFI, no OS

A UEFI tool — most likely an extension to `tools/sysinfo.c` rather
than a new binary, since sysinfo already walks SMBIOS/PCI/firmware.

```
sysinfo --capture <destdir>
```

Why native UEFI rather than a Linux capture script:

- **Vendor-neutral, OS-neutral.** Boot a uefi-devkit USB on any
  machine — corporate Windows laptop, server, lab box — run, reboot.
  No Linux install, no IT permissions, no WSL caveats (WSL2 sees
  Hyper-V's synthetic SMBIOS, not the real platform).
- **Dogfoods axl-sdk.** Capture exercises [src/smbios/](../src/smbios/),
  [src/ipmi/](../src/ipmi/), [src/net/](../src/net/) (HTTP client
  for Redfish), and PCI I/O all in one tool. It's an integration
  test as much as a feature.
- **Direct config-table access.** SMBIOS3 and ACPI 2.0 are reachable
  from `EFI_SYSTEM_TABLE->ConfigurationTable` — no kernel needed.
  PCI via `EFI_PCI_IO_PROTOCOL`. IPMI via in-band KCS (already in
  axl-sdk).

#### Captured artifacts

| Artifact | Source | Format |
|----------|--------|--------|
| `smbios.bin` | EFI Config Table SMBIOS3 GUID | raw bytes |
| `acpi/<sig>.dat` | EFI Config Table ACPI 2.0 GUID, walk RSDT/XSDT | raw bytes per table |
| `pci.json` | `EFI_PCI_IO_PROTOCOL` per device | JSON manifest (VID/DID/class/subsys/BARs/config) |
| `firmware.json` | EFI System Table fields | JSON (vendor, revision, boot services revision) |
| `redfish/**/*.json` | optional — walk service root via HTTP | JSON tree mirroring `/redfish/v1/...` |
| `ipmi/<cmd>.bin` | optional — canned Get-* commands via KCS | raw response bytes per command |
| `manifest.json` | top-level metadata | JSON: vendor, model, serial, BIOS rev, capture date, capture-tool version |

Optional captures (Redfish, IPMI) are gated by CLI flags. The
SMBIOS/ACPI/PCI capture is always cheap and always on.

#### Write targets

Pick at runtime — all three supported:

1. **Local FS** — `fs0:\fixtures\<vendor>-<model>-<bios>\` on the
   boot USB. Default for the bare-USB workflow.
2. **virtiofs `--mount`** — for the dev loop:
   ```sh
   ./scripts/run-qemu.sh --mount fixtures/ \
       sysinfo.efi --capture hostfs:/proxmox-test
   ```
3. **HTTP POST** — for headless/net-only environments. POST a
   tarball to a host-side collector endpoint.

### Replay: run-qemu.sh `--fixture`

A single new flag that auto-discovers files in the directory and
wires the QEMU command:

```
--fixture DIR
```

resolves to:

- `smbios.bin`        → `-smbios file=DIR/smbios.bin`
- `acpi/*.dat`        → `-acpitable file=...` (one per file)
- `redfish/`          → spawn mock server, `--hostfwd <port>:443`
- `ipmi/`             → spawn `ipmi_sim` with captured replies, wire
                        `-device ipmi-bmc-extern,chardev=...`
- `pci.json`          → manifest only (logged, not replayed)
- `manifest.json`     → printed at startup so the user sees which
                        machine the guest is impersonating

Lower-level flags also exposed for mix-and-match:

```
--smbios-file FILE              # single SMBIOS blob
--acpi-table FILE               # repeatable
--ipmi-sim                      # built-in ipmi-bmc-sim
--ipmi-extern PATH              # ipmi_sim socket path
--redfish-mock DIR              # spawn DMTF mockup, hostfwd to it
--openbmc-qemu PATH             # alternative: sibling OpenBMC QEMU
```

The mock-server lifecycle mirrors the existing virtiofsd handling
in run-qemu.sh: spawn before QEMU, capture PID, kill on exit trap,
report PID in `--background` mode.

## Bootstrap targets

In priority order, each adding incremental coverage:

1. **Proxmox VM (the user's current dev host)** — fast iteration,
   OVMF + virtio. Used as the **plumbing smoke test** ("does
   capture-and-replay round-trip the bytes faithfully?"). Doesn't
   exercise vendor quirks because the source SMBIOS/ACPI is
   QEMU-synthetic — replaying it into QEMU is tautological — but
   that's a feature for CI, not a bug.
2. **OpenBMC in QEMU (sibling instance)** — covers the Redfish
   capture+replay path with a real BMC firmware stack, no lab
   hardware required. OpenBMC's own dev workflow does this.
3. **`ipmi_sim` standalone** — covers IPMI capture+replay against
   a real wire-protocol simulator. Standard commands only, no
   OEM, but a solid foundation.
4. **Real hardware fixtures** added opportunistically — the user's
   Dell corporate laptop (booted from a uefi-devkit USB, no OS
   involvement), the lab Dell R470, anything else that turns up.
   Tagged with `vendor-model-biosrev`.

The MacBook is **not** a target — Apple Silicon doesn't boot
generic UEFI; older Intel Macs are dying as a use case.

## Fixture redistribution

Captures contain identifying information (serial numbers, asset
tags, MAC addresses, SMBIOS OEM strings that may include
proprietary data). Two policies:

- **Local fixtures** — checked into a private working tree, not
  redistributed. Use freely for dev/test.
- **Public fixtures** — sanitized: serial numbers and asset tags
  zeroed in SMBIOS structures, MAC addresses replaced with locally-
  administered ranges, OEM strings reviewed. A `--sanitize` flag
  on the capture tool (or a separate post-processor) does this
  mechanically.

If we eventually want a shared fixture corpus, it should live in a
sibling repo (`aximcode/axl-fixtures`?) with a contributor process
for sanitization review. Out of scope for the initial design.

## Open questions

- **PCI Option ROM capture?** Some servers ship critical drivers
  in PCI ROMs the firmware loads. Capturing ROM contents and
  feeding them back to QEMU is theoretically possible (`-device
  ...,romfile=...`) but only useful if the corresponding device
  is being emulated — which it usually isn't. Probably skip.
- **Capture-time NIC presence.** If the capture machine has no
  network, Redfish capture is silently skipped. Should the tool
  fail loud or warn? Probably warn — capture is opportunistic.
- **Replay-time signature mismatch.** If a fixture's SMBIOS claims
  "Dell PowerEdge R470" but the user runs it on `--arch AARCH64`,
  the guest will be confused. The replay layer should print a
  warning when fixture metadata disagrees with QEMU args, not block.
- **OpenBMC sibling lifecycle.** Bringing up a second QEMU adds
  ~15 s to test runtime and ~512 MB of RAM. Worth it for Redfish
  iteration loops; probably gated behind an explicit flag, not
  enabled by `--fixture` automatically.

## Implementation phases

Suggested ordering, smallest viable slices first:

1. **Phase HF1** — `run-qemu.sh` low-level flags only:
   `--smbios-file`, `--acpi-table`, `--ipmi-sim`. Hand-craft the
   first fixture from the Proxmox VM (`dmidecode --dump-bin`,
   `acpidump -b`) to validate the replay path before writing the
   capture tool. Smallest possible diff.
2. **Phase HF2** — `sysinfo --capture` for SMBIOS + ACPI + PCI
   manifest + write-to-`fs0:`. Run on the Proxmox VM, replay the
   captured fixture, compare against Phase HF1's hand-crafted one.
   Plumbing smoke test.
3. **Phase HF3** — `--fixture DIR` flag in run-qemu.sh, auto-wires
   SMBIOS/ACPI from a directory.
4. **Phase HF4** — Redfish capture (HTTP walk) and replay
   (`--redfish-mock` spawning DMTF mockup-server). Validate against
   OpenBMC-in-QEMU as the "real BMC" reference.
5. **Phase HF5** — IPMI capture (KCS sweep) and replay
   (`--ipmi-extern` against `ipmi_sim`).
6. **Phase HF6** — Sanitization pass (`--sanitize`) and the public
   fixtures decision (separate repo vs. local-only).

Phases HF1–HF3 cover ~80% of the value; HF4–HF5 are where the
"emulate Dell/HP" goal really lands.
