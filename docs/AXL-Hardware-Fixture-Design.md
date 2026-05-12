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
  Both are **already exposed** via run-qemu.sh's `--ipmi` /
  `--ipmi-extern` / `--ipmi-prop` flags — no new flag needed.
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
- **Non-class-compliant USB devices** — proprietary BMC virtual
  media protocols, vendor-specific HID extensions, IDSDM controller
  internals, etc. Same reasoning as vendor PCI: faking presence
  without faking class-specific protocol is performative.
  Class-compliant USB (CDC-ECM/NCM, HID keyboard/mouse, mass
  storage) is partially replayable via QEMU's stock devices — see
  Replay below.
- **GPU emulation** — ASPEED AST2500/2600 BMC-VGA, Matrox G200,
  discrete NVIDIA/AMD compute cards. GPU emulation is the hardest
  hardware to fake; QEMU's stock `-vga std` / `-device VGA` covers
  GOP discovery and framebuffer tests, which is all axl-sdk's gfx
  code currently exercises. Discrete-GPU passthrough is a real-
  hardware-lab activity (vfio), not a fixture-replay scenario.

### Explicitly out of scope

To save future readers from re-asking — these have been considered
and intentionally deferred:

- **HII (BIOS Setup forms/strings)** — capturable, but exercising
  axl-sdk against captured BIOS Setup screens has no clear
  use case.
- **SPI flash layout** — chipset-specific, BMC-mediated on most
  servers; capture is intrusive and replay needs faithful chipset
  emulation we don't have.
- **Intel ME / AMD PSP** — vendor-specific HECI/mailbox protocols,
  intractable to emulate.
- **Embedded Controller (EC)** — laptop concern; on servers the BMC
  plays this role and is covered by Redfish/IPMI.
- **Thermal/fan sensors via direct SMBus** — already reachable via
  BMC IPMI/Redfish (covered) or, when we hit a real need, the HF9
  non-EEPROM SMBus sensors patch.
- **Platform LEDs / enclosure ID / hotplug controllers** — too niche
  for axl-sdk's tooling.
- **NVDIMM/PMem (NFIT)**, **CXL (CEDT)** — already in the ACPI
  bundle; no extra work.
- **PCIe AER / HEST RAS** — AER caps are in `pci.json`'s extended
  config; HEST is in the ACPI bundle.
- **NUMA topology (SRAT/SLIT/HMAT)** — on the ACPI denylist; QEMU
  generates its own.
- **Dynamic protocol traces** (KCS state-machine logs, IPMI
  request/response timing, USB enumeration order) are NOT fixture
  artifacts — they are validation data for axl-sdk's state
  machines. The XE7745 lab archive contains examples (e.g.
  `10-axl-sdk-kcs-WORKING-on-iDRAC10.log`,
  `15-memspd-port0-only-traces.log`) that proved the bit-position
  bug in the KCS BaseAddressModifier; these get re-run during
  bring-up, not replayed against fixtures. If we ever want
  trace-driven replay (e.g. "feed this captured KCS sequence to
  the BMC sim and verify the same state walk"), it's a separate
  feature — not part of HF.

## Architecture

```
┌──────────────────────────┐                ┌─────────────────────────┐
│   Capture (UEFI tool)    │                │   Replay (host wrapper) │
│                          │                │                         │
│  mkfixture.efi <destdir> │  ─── files ──> │  axl-emulate <dir>      │
│                          │                │  (wraps run-qemu.sh)    │
│   ├── smbios.bin         │                │   ├── -smbios file=...  │
│   ├── acpi/*.dat         │                │   ├── -acpitable file=  │
│   ├── pci.json           │                │   ├── (manifest only)   │
│   ├── usb.json + usb/*   │                │   ├── usb-shim opt-in   │
│   ├── video.json + edid/ │                │   ├── -edid / -gpu-rom  │
│   ├── cpu.json           │                │   ├── -cpu MODEL map    │
│   ├── net.json           │                │   ├── -device ...,mac=  │
│   ├── tpm.json + log     │                │   ├── swtpm + tpm-tis   │
│   ├── vars/global/*      │                │   ├── vars.fd injection │
│   ├── vars/secureboot/*  │                │   ├── vars.fd injection │
│   ├── esrt.json          │                │   ├── (manifest only)   │
│   ├── nvme/*.json        │                │   ├── (manifest only)   │
│   ├── spd/*.bin          │                │   ├── smbus-eeprom +    │
│   │                      │                │   │   memdev= (patched) │
│   ├── redfish/**/*.json  │                │   ├── mock-server +     │
│   └── ipmi/*.bin         │                │   │   --hostfwd         │
│                          │                │   └── ipmi_sim +        │
│                          │                │       -device ipmi-...  │
└──────────────────────────┘                └─────────────────────────┘
```

### Capture: native UEFI, no OS

A new dedicated UEFI tool — `tools/mkfixture.c`, mirroring the
`mkrd.efi` naming pattern (mk = make). Capture is structurally
different from inventory display: it writes binary blobs and JSON
manifests to a strict directory layout, with byte-exact round-trip
guarantees. Conflating it into `sysinfo` (a human-readable
inventory tool) would muddy both. Cross-tool sharing of SMBIOS /
PCI / USB walking happens at the library layer, not at the
command line.

```
mkfixture.efi <destdir> [--include-redfish] [--include-ipmi] ...
```

Why native UEFI rather than a Linux capture script:

- **Vendor-neutral, OS-neutral.** Boot a uefi-devkit USB on any
  machine — corporate Windows laptop, server, lab box — run, reboot.
  No Linux install, no IT permissions, no WSL caveats (WSL2 sees
  Hyper-V's synthetic SMBIOS, not the real platform).
- **Dogfoods axl-sdk hard.** Capture exercises
  [src/smbios/](../src/smbios/), [src/ipmi/](../src/ipmi/),
  [src/net/](../src/net/) (HTTP client for Redfish), PCI I/O,
  USB I/O, GOP, TCG2, Variable Services, and the JSON formatter
  in one tool. It's an integration test as much as a feature.
- **Direct config-table access.** SMBIOS3 and ACPI 2.0 are reachable
  from `EFI_SYSTEM_TABLE->ConfigurationTable` — no kernel needed.
  PCI via `EFI_PCI_IO_PROTOCOL`. IPMI via in-band KCS (already in
  axl-sdk).

#### Captured artifacts

| Artifact | Source | Format |
|----------|--------|--------|
| `smbios.bin` | EFI Config Table SMBIOS3 GUID | raw SMBIOS structure data (no entry-point prefix — see SMBIOS format note below) |
| `acpi/<sig>.dat` | EFI Config Table ACPI 2.0 GUID, walk RSDT/XSDT | raw bytes per table |
| `pci.json` | `EFI_PCI_IO_PROTOCOL` per device | JSON manifest (VID/DID/class/subsys/BARs/config) |
| `usb.json` | `EFI_USB_IO_PROTOCOL` + `EFI_USB2_HC_PROTOCOL` walk | JSON manifest (topology, VID/PID, class/subclass/protocol, strings) |
| `usb/<bus>-<port>.bin` | per-device full config descriptor blob | raw bytes (for offline inspection / future replay) |
| `video.json` | `EFI_GRAPHICS_OUTPUT_PROTOCOL` per GPU | JSON manifest (mode list, current mode, framebuffer base, pixel format) |
| `edid/<port>.bin` | `EFI_EDID_DISCOVERED_PROTOCOL` per display | raw EDID bytes (128/256 B) |
| `gpu-rom/<bdf>.bin` | optional — PCI option ROM extraction | raw VBIOS bytes (only on demand; can be large) |
| `firmware.json` | EFI System Table fields | JSON (vendor, revision, boot services revision) |
| `cpu.json` | direct CPUID instructions + UEFI CPU services | JSON (vendor, family/model/stepping, brand string, feature flags, cache info, microcode rev) |
| `net.json` | `EFI_SIMPLE_NETWORK_PROTOCOL` per NIC + PCI extended config | JSON (per-NIC MAC, link state, MTU, PHY hints, SR-IOV VF count) |
| `tpm.json` | `EFI_TCG2_PROTOCOL` (TPM 2.0) or `EFI_TCG_PROTOCOL` (TPM 1.2) | JSON (manufacturer ID, firmware version, capabilities, current PCR values for SHA-1/SHA-256 banks) |
| `tpm/event-log.bin` | `EFI_TCG2_PROTOCOL.GetEventLog()` | raw TCG event log bytes |
| `vars/global/<name>.bin` | UEFI Variable Services for `EFI_GLOBAL_VARIABLE` GUID | raw bytes per `Boot####`, `BootOrder`, `BootCurrent`, `BootNext`, `Timeout`, etc. |
| `vars/secureboot/{PK,KEK,db,dbx}.bin` | UEFI Variable Services for `EFI_IMAGE_SECURITY_DATABASE` + global GUIDs | raw `EFI_SIGNATURE_LIST` bytes per Secure Boot key database |
| `esrt.json` | EFI Config Table `EFI_SYSTEM_RESOURCE_TABLE_GUID` | JSON manifest (per-component FwClass GUID, current/lowest-supported version, capsule flags) |
| `nvme/<bdf>.json` | `EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL` Identify Controller + Identify Namespace | JSON manifest (per-controller VID/SSVID/serial/model, namespace LBA format, log page summaries) |
| `spd/<addr>.bin` | SMBus EEPROMs at 0x50–0x57 (and beyond) via AxlSmbus (mkfixture's SPD walk; memspd remains an inspection tool) | raw 256/512/1024 B blobs per DIMM slot |
| `redfish/**/*.json` | optional — walk service root via HTTP | JSON tree mirroring `/redfish/v1/...` |
| `ipmi/<cmd>.bin` | optional — canned Get-* commands via KCS | raw response bytes per command |
| `manifest.json` | top-level metadata | JSON: vendor, model, serial, BIOS rev, capture date, capture-tool version |

Optional captures (Redfish, IPMI, SPD) are gated by CLI flags. The
SMBIOS/ACPI/PCI capture is always cheap and always on. SPD capture
is gated because it requires the platform to expose an SMBus
controller through `EFI_SMBUS_HC_PROTOCOL` or `EFI_I2C_MASTER_PROTOCOL`.
Four real-world cases:

1. **QEMU**: `SmbusHcShim.efi` republishes the ICH9 SMBus as
   `EFI_I2C_MASTER_PROTOCOL`. Always available in our test rig.
2. **Native Intel server / desktop**: ICH/PCH driver in firmware,
   typically present on enterprise boards. Works.
3. **Other vendor SMBus driver** (e.g. AMD FCH on a board where
   the firmware DOES expose it): works.
4. **Vendor server where firmware deliberately hides the SMBus
   controller** — e.g. **Dell PowerEdge XE7745** has an AMD FCH
   PIIX4 SMBus at I/O port 0xB20 visible to Linux's `piix4` driver,
   carrying the DDR5 SPDs at 0x50–0x57, but Dell's UEFI publishes
   neither `EFI_SMBUS_HC_PROTOCOL` nor `EFI_I2C_MASTER_PROTOCOL`
   for it. mkfixture's `spd/` directory will be **empty** on these
   boxes even though SPDs are physically present and readable from
   any OS that can drive the chipset directly. This is a vendor
   firmware choice, not a tooling bug. Capture from a Linux boot
   on the same hardware (a future HF-host pipeline, not in scope
   yet) is the only workaround. See `DellXE7745/17-fedora44-spd5118-binding-fails.md`
   in the gitignored capture archive for the live-system evidence.

Memo for future readers: when a Dell-class server captures with no
SPDs, the missing data is almost certainly category 4 above —
don't chase it as a mkfixture bug.

**SMBIOS format note**: `smbios.bin` is the **raw structure region**
— a sequence of type/length/handle records terminated by Type 127 —
with no entry-point structure prefix. This matches QEMU's
`-smbios file=` consumer, which builds its own entry-point around
the user-supplied data (see `hw/smbios/smbios.c` in QEMU 10.x). The
file is therefore **NOT** directly interchangeable with
`dmidecode --from-dump` (dmidecode prepends a 31-byte SMBIOS 2.x
or 24-byte SMBIOS 3.x entry-point structure). Tools that want a
dmidecode-compatible export should concatenate the captured EP
(via `axl_smbios_entry_point`) with the table bytes. The fragile
"prepend EP and hope OS walks past it as a bogus type-95 record"
approach was tried and rejected — it only works for some captures
by alignment luck and silently hangs others.

#### Write targets

Pick at runtime — all three supported:

1. **Local FS** — `fs0:\fixtures\<vendor>-<model>-<bios>\` on the
   boot USB. Default for the bare-USB workflow.
2. **virtiofs `--mount`** — for the dev loop:
   ```sh
   ./scripts/run-qemu.sh --mount fixtures/ \
       mkfixture.efi hostfs:/proxmox-test
   ```
3. **HTTP POST** — for headless/net-only environments. POST a
   tarball to a host-side collector endpoint.

### Replay: `axl-emulate <fixture-dir>`

A separate user-facing tool — `scripts/axl-emulate` (Python, no
extension; ships in the host-tools tarball alongside `run-qemu.sh`
and `axl-cc`) — that consumes a fixture directory, translates it
into the right run-qemu.sh primitives, and `exec`s run-qemu.sh.

This is a **wrapper, not a duplicate**. run-qemu.sh stays the
primitive layer (low-level QEMU launching, OVMF/firmware discovery,
disk-image build, KVM acceleration, GDB stub, etc.) and exposes
the per-artifact flags (`--smbios-file`, `--acpi-table`, `--spd`,
`--tpm`, …) as primitives. axl-emulate is the persona that knows
about the fixture *layout* — directory structure, ACPI denylist,
manifest.json — and never duplicates run-qemu.sh's launching
logic.

```
axl-emulate <fixture-dir> [efi-file] [args...]
            [--keep-acpi NAME] [--drop-acpi NAME] [--strict-acpi]
            [--arch X64|AARCH64]
            [-- run-qemu-args...]
```

Anything after `--` passes through to run-qemu.sh verbatim, so
`axl-emulate` can compose with run-qemu.sh's existing knobs
(`--background`, `-i`, `--mount`, …).

A `<fixture-dir>` resolves to:

- `smbios.bin`        → `-smbios file=DIR/smbios.bin`
- `acpi/*.dat`        → `-acpitable file=...` (one per file)
- `spd/*.bin`         → per blob: `-object memory-backend-file,id=spdN,
                        mem-path=...,size=4096,share=off` plus
                        `-device smbus-eeprom,address=0xNN,memdev=spdN`
                        (depends on the locally-patched QEMU — see
                        QEMU patch dependencies below)
- `redfish/`          → spawn mock server, `--hostfwd <port>:443`
- `ipmi/`             → spawn `ipmi_sim` with captured replies, wire
                        `-device ipmi-bmc-extern,chardev=...`
- `pci.json`          → manifest only (logged, not replayed)
- `usb.json`          → manifest + optional class-compliant shims
                        (see USB replay shims below); raw descriptor
                        blobs in `usb/*.bin` are inspection-only
- `video.json`        → manifest only (QEMU's stock `-vga std` is
                        sufficient for GOP discovery tests)
- `edid/*.bin`        → optional `--edid FILE` injects raw EDID
                        into QEMU's display chardev
- `gpu-rom/*.bin`     → optional `--gpu-rom FILE` →
                        `-device VGA,romfile=FILE` (no patch needed)
- `cpu.json`          → `--cpu-from-fixture` maps captured CPUID
                        vendor/family/model to the closest QEMU
                        `-cpu MODEL` (Cascadelake-Server, EPYC-Milan,
                        etc.); fuzzy match, never byte-exact (see
                        CPU model caveats below)
- `net.json`          → informs default `--nic-model` choice;
                        per-NIC MAC injected via
                        `-device <model>,mac=XX:XX:XX:XX:XX:XX` so
                        guests see the captured-platform addresses
- `tpm.json` +
  `tpm/event-log.bin` → spawn `swtpm` (lifecycle modeled on
                        virtiofsd), wire `-tpmdev emulator,
                        chardev=...` plus `-device tpm-tis,
                        tpmdev=...` (or `tpm-crb` per fixture)
- `vars/global/*` +
  `vars/secureboot/*`  → injected into the OVMF `vars.fd` copy
                        before QEMU launch (uses the existing
                        `cp "$FW_VARS" "$TMPDIR/vars.fd"` seam in
                        run-qemu.sh; see Variable replay below)
- `esrt.json`         → manifest only (replay would need an HF9-class
                        QEMU patch to publish a custom ESRT)
- `nvme/*.json`       → manifest only initially; faithful replay
                        of Identify Controller responses is an HF9
                        patch candidate
- `manifest.json`     → printed at startup so the user sees which
                        machine the guest is impersonating

Lower-level flags also exposed for mix-and-match:

```
--smbios-file FILE              # single SMBIOS blob
--acpi-table FILE               # repeatable
--ipmi                          # (already in run-qemu.sh) ipmi-bmc-sim + KCS
--ipmi-extern PATH              # (already in run-qemu.sh) ipmi_sim socket
--redfish-mock DIR              # spawn DMTF mockup, hostfwd to it
--openbmc-qemu PATH             # alternative: sibling OpenBMC QEMU
--tpm                           # spawn swtpm with empty state
--tpm-state DIR                 # spawn swtpm with DIR as its state directory
                                # (raw swtpm format; captured-fixture seeding
                                # is HF5's scope, not HF1)
--tpm-model tpm-tis|tpm-crb     # arch default (tpm-tis x64 / tpm-crb aa64)
--secureboot DIR                # inject PK/KEK/db/dbx from DIR/*.bin
--boot-vars DIR                 # inject Boot####/BootOrder from DIR/*.bin
--cpu-from-fixture              # map cpu.json → -cpu MODEL
--mac ADDR=XX:XX:..             # repeatable; pin per-NIC MAC
--edid FILE                     # raw EDID injection
--gpu-rom FILE                  # -device VGA,romfile=FILE
```

The mock-server / swtpm lifecycle mirrors the existing virtiofsd
handling in run-qemu.sh: spawn before QEMU, capture PID, kill on
exit trap, report PID in `--background` mode.

#### TPM replay (swtpm lifecycle)

`swtpm` (Stefan Berger's userspace TPM simulator) is the standard
backend for QEMU TPM emulation and the canonical mate for `-tpmdev
emulator`. Spawn it before QEMU with the captured state as input:

```
swtpm socket --tpmstate dir=$TMPDIR/tpm-state \
    --ctrl type=unixio,path=$TMPDIR/swtpm-ctrl \
    --tpm2 --flags startup-clear &
```

Then wire QEMU:

```
-chardev socket,id=chrtpm,path=$TMPDIR/swtpm-ctrl
-tpmdev emulator,id=tpm0,chardev=chrtpm
-device tpm-tis,tpmdev=tpm0     # or tpm-crb for CRB-interface fixtures
```

**State-seeding is two layers** and they should not be conflated:

1. **HF1 (`--tpm-state DIR`)** — DIR is a **raw swtpm-format state
   directory** (NVChip files: `tpm2-00.permall`, etc.). swtpm
   passes through unchanged. `--tpm` with no DIR → fresh empty
   state in `$TMPDIR/tpm-state` (all-zero PCRs; useful for clean-
   slate measured-boot tests).
2. **HF5 (captured-fixture seeding)** — `axl-emulate <dir>` reads
   the captured `tpm.json` + `tpm/event-log.bin` and **converts**
   them into swtpm's NVChip state format before swtpm starts. The
   conversion does not exist yet; swtpm's state format is its own
   binary layout, not a copy of the raw TCG event log.

HF1 only provides layer 1. The captured-to-swtpm conversion is HF5
work, gated on us actually having captures to convert.

Lifecycle parallels virtiofsd: spawn, wait for socket, kill on exit
trap, expose PID in `--background` mode. swtpm absent on PATH ⇒
hard error with install hint, not a warning. (Same precedent as
`--mount` / virtiofsd.)

**Caveat**: capture-time PCR values reflect the source platform's
firmware measurements. If the replay guest's OVMF measures different
code into PCR 0/1/7, the seeded values won't match what actually
ran — useful for *replaying* a known measurement state but not for
attestation tests that verify boot integrity end-to-end. Document
this clearly so users don't conclude "secure boot is broken under
replay" when the discrepancy is by design.

#### Variable replay (Secure Boot + boot order)

UEFI variables live in OVMF's NVRAM file (`$FW_VARS`, copied to
`$TMPDIR/vars.fd` per QEMU launch). Replay injects captured
variables by writing them into that file before QEMU starts.

Two implementation paths:

1. **`virt-fw-vars` (libvirt)** — established Python tool; reads/
   writes OVMF vars files and accepts EFI variable blobs. Likely
   the cleanest dependency.
2. **In-tree Python helper** — small script using known OVMF
   vars-file layout (FFS volume + NVRAM HOB). More work, no
   external dep. Reserve for if `virt-fw-vars` ends up unsuitable.

Captured artifacts are raw `EFI_VARIABLE_AUTHENTICATION_2`-wrapped
bytes (Secure Boot) or plain variable bodies (`Boot####`); the
injector preserves attribute flags and authentication headers
verbatim.

**Caveat**: Secure Boot on the replay guest only works if OVMF was
built with Secure Boot support (`OVMF_CODE_4M.secboot.fd` on Debian/
Fedora). The default `OVMF_CODE.fd` ignores Secure Boot variables.
The replay layer should detect the firmware variant and warn loudly
if the user injected `--secureboot` against a non-secboot OVMF.

#### CPU model caveats

`cpu.json` records exact CPUID leaves; QEMU's `-cpu` only exposes
named models with feature toggles. The `--cpu-from-fixture` mapping
is fuzzy by design:

| CPUID family.model.stepping → | QEMU `-cpu` |
|-------------------------------|-------------|
| Intel Skylake-SP (6.85)       | `Skylake-Server` |
| Intel Cascade Lake-SP (6.85.7) | `Cascadelake-Server-noTSX` |
| Intel Ice Lake-SP (6.106)     | `Icelake-Server-noTSX` |
| Intel Sapphire Rapids (6.143) | `SapphireRapids-noTSX` |
| AMD Rome (23.49)              | `EPYC-Rome` |
| AMD Milan (25.1)              | `EPYC-Milan` |
| AMD Genoa (25.17)             | `EPYC-Genoa` |
| AMD Bergamo (25.17, 32C/64T)  | `EPYC-Genoa` (with `-smp` cores= override) |
| AMD Turin (26.17)             | `EPYC-Turin` (QEMU 9.0+) |
| (unknown family.model)        | `host` if KVM, else `qemu64` |

Exact CPUID-leaf replay is intractable without per-leaf overrides —
QEMU's `-cpu host,+feat,-feat` knob only toggles whole features.
For tools that depend on specific feature flags (AVX-512, SGX, etc.),
the manifest's feature flags should be cross-checked against the
mapped model and divergences printed at startup.

#### USB replay shims

USB device behavior in general is not replayable (see Intractable
above), but **class-compliant** devices can be approximated using
QEMU's stock USB devices. When the captured `usb.json` indicates a
class-compliant device was present, the replay layer can optionally
add a stand-in:

| Captured class | QEMU stand-in | Useful for |
|----------------|---------------|------------|
| CDC-ECM / NCM (class 0x02 subclass 0x06/0x0D) | `-device usb-net,netdev=...` | iDRAC/iLO USB-NIC discovery via SNP |
| HID keyboard (class 0x03 protocol 0x01) | `-device usb-kbd` | console-input tests on otherwise-headless guests |
| HID mouse/tablet (class 0x03 protocol 0x02) | `-device usb-tablet` | same |
| Mass storage (class 0x08) | skipped | we don't capture the underlying disk image |
| Hub (class 0x09) | skipped | QEMU manages its own USB tree |

Stand-ins are **opt-in** — `--fixture` defaults to manifest-only,
add `--usb-shim` (or `--usb-shim CLASS,...`) to enable. The
manifest distinction matters: a CDC-ECM stand-in is a virtio-style
USB-NIC, NOT the real BMC; tests that rely on the actual BMC USB
protocol will mislead if the user thinks the shim is faithful.

**Concrete example — Dell PowerEdge XE7745 iDRAC**: the iDRAC10
publishes a USB-RNDIS NIC (NOT plain CDC-ECM) plus a Realtek
RTL8153 over the host-side virtual USB bus, plus a "DRAC virtual
KB/M" HID composite. mkfixture's `usb.json` will record all three
(VID/PID, class/subclass, descriptor blob in `usb/<bus>-<port>.bin`).
Replay options:
- `usb-net` shim: gets us "an SNP NIC appears" but not the
  Dell-RNDIS framing. axl-sdk's existing UsbRndis driver bundle
  works against a real iDRAC, NOT against the QEMU `usb-net` shim.
- HID stand-ins (`usb-kbd`/`usb-tablet`): work for input, but the
  Dell virtual-KB/M's vendor-extension descriptors (which the
  iDRAC firmware reads to do special key remapping) are lost.
- For real BMC behavior, point axl-sdk at OpenBMC-in-QEMU (HF7
  reference setup) — a real BMC firmware stack — instead of
  trying to fake the Dell USB-RNDIS protocol.

Anything beyond class-compliant — proprietary BMC virtual media,
IDSDM control endpoints, vendor HID extensions — needs a future
"usb-stub" QEMU patch (Phase HF7 candidate) that responds to
standard descriptor requests from a captured blob and stalls
class-specific traffic. Useful only for "does device appear in bus
walk" tests.

#### Serial ports

Serial topology is described almost entirely in ACPI on x86: SPCR
(Serial Port Console Redirection) gives the console UART location;
DBG2 (Debug Port Table 2) lists secondary debug ports. Both are
captured automatically as part of the ACPI artifact bundle.

**Replay caveat**: a captured SPCR pointing at e.g. a BMC-mediated
MMIO UART at `0xFE000000` will confuse a guest that trusts the
table — the address has nothing behind it in QEMU, and the guest's
own console redirection will hang trying to use it. SPCR and DBG2
join the default-drop denylist for that reason; the guest falls
back to QEMU's standard COM1 (`-nographic` already wires this up).

For more elaborate scenarios — testing axl-sdk's behavior with
multiple captured UARTs, validating BMC SOL handoff — the existing
run-qemu.sh `-serial`/`-chardev` infrastructure is sufficient. No
new flag needed in the initial design.

#### ACPI replay caveats

A captured ACPI table set describes the **source platform's**
hardware topology, parts of which contradict QEMU's emulated
hardware. Naive replay of every captured table breaks the guest.

The clearest example is **MCFG**: it declares the PCIe ECAM base
address and per-segment bus range. A captured OEM MCFG points at
(say) `0xE0000000` covering buses `0x00–0xFF`. QEMU's pc-q35
emulates ECAM at a different address with a different range. If
the guest trusts the captured MCFG, every PCIe enumeration via
ECAM reads all-`0xFF` from the captured range — every QEMU virtio
device disappears from the guest's view. On stricter firmware the
mismatch faults instead of returning 0xFF.

**Default-drop denylist on replay** — two categories, both
verified empirically (per-table boot bisection against the Proxmox
fixture) and in source (QEMU `hw/acpi/core.c:acpi_table_install`,
OVMF `OvmfPkg/Library/AcpiPlatformLib/QemuFwCfgAcpi.c`):

*Core ACPI singletons* (FACP/FACS/DSDT have absolute physical
pointers in their bodies; QEMU's `-acpitable` does NOT fix these
up — only the header is rewritten — and OVMF's
`EFI_ACPI_TABLE_PROTOCOL.InstallAcpiTable` follows them per the
spec, dereferencing source-platform addresses that don't exist in
QEMU's memory map and hanging the boot):

- `FACP` — Fixed ACPI Description Table (FADT). Body contains
  `FIRMWARE_CTRL`, `DSDT`, `X_FIRMWARE_CTRL`, `X_DSDT`, plus
  PM1a/b/GPE register block addresses.
- `FACS` — Firmware ACPI Control Structure. Pointed-to by FACP;
  installing alone causes FACP-pointer rewriting and corruption.
- `DSDT` — Differentiated System Description Table. AML namespace
  with embedded opregion physical addresses; AML interpretation
  hits invalid addresses and hangs.

(APIC, SSDT, HPET, WAET, BERT, BGRT, MSDM, SLIC, FPDT, MCHI inject
cleanly. APIC/SSDT/HPET/WAET have no absolute physical addresses
in their bodies; multiple instances are spec-tolerated. BERT,
BGRT, FPDT, MCHI *do* have physical pointers (boot error region,
boot graphic image, perf records, BMC interface), but OVMF and
Linux don't dereference those pointers on the boot path — they're
read only on demand (logged error, OS rendering the logo, etc.) —
so stale captured pointers are inert. Add to this list ONLY when
an actual hang or misbehavior is observed.)

*Platform topology* (the source platform's PCIe/NUMA/serial
topology doesn't match QEMU's emulated platform):

- `MCFG` — PCIe ECAM map (the canonical example — see this
  subsection's introduction).
- `MPST` — memory power state table (references real DIMMs).
- `PMTT` — platform memory topology (same).
- `HMAT`, `SLIT`, `SRAT` — NUMA/proximity topology.
- `SPCR` — serial console redirection (captured MMIO won't exist
  in QEMU; guest hangs on console init).
- `DBG2` — debug port table 2 (same issue, secondary ports).

**Default-keep**: APIC, SSDT, HPET, BERT, BGRT, MSDM, SLIC, FPDT,
BOOT, MCHI, ASF!, WAET, IORT (AArch64) — either no absolute
pointers, or the pointers exist but aren't dereferenced on the
boot path. Add to the denylist only when a per-table boot
bisection shows a hang.

**Practical implication**: wholesale ACPI replay is rarely
useful. The interesting OEM data lives in SMBIOS Type 11 (already
covered by `--smbios-file`) and in DSDT (which we can't replay
safely — it would need to be REPLACED, not added, and OVMF
doesn't expose that knob). For most fixture-replay use cases
the SMBIOS replay is the lever; ACPI replay is reserved for
specific tables a parser test needs to exercise (BERT for error-
log handling, MSDM for Windows-licensing checks, etc.).

Override flags:
```
--keep-acpi NAME           # force-keep a default-dropped table
--drop-acpi NAME           # force-drop a default-kept table
--strict-acpi              # drop only the MCFG denylist; keep all others
```

PCI ECAM support note: the capture tool needs **no** ECAM-specific
code. `EFI_PCI_IO_PROTOCOL` provides extended config-space access
(offsets 0x100–0xFFF) regardless of whether the underlying platform
uses CF8/CFC, ECAM, or a hybrid. The replay layer's job is to
prevent ECAM topology mismatches by filtering ACPI, not to emulate
the captured ECAM region itself.

### QEMU patch dependencies

Stock QEMU's flag surface doesn't cover everything fixture replay
needs. Some devices have programmatic-only init (no QOM property to
load data from a file), so injecting fixture data from the command
line requires patching the device.

This is an accepted pattern in this project. Patches live in
[scripts/qemu-patches/](../scripts/qemu-patches/) and are applied
to the locally-built QEMU at `$QEMU_DIR`. axl-common.sh's QEMU
discovery already prefers that build over a system QEMU.

Existing patch:

- **`0001-smbus-eeprom-add-memdev-link.patch`** — adds a
  `memdev=<link<memory-backend>>` property to QEMU's `smbus-eeprom`
  device, enabling command-line SPD blob injection. Originally
  written for AxlSpd's wire-path test
  ([test/integration/test-spd-qemu.sh](../test/integration/test-spd-qemu.sh));
  reused as-is for fixture replay.

Likely future candidates (none committed yet — add only when a
fixture artifact actually needs them):

- SMBIOS handle-preserving injection for OEM Type-N structures
  whose handles are referenced by other tables.
- Vendor-specific SMBus sensors (LM75-style temperature, fan
  controllers) at non-EEPROM addresses.
- TPM event log seeding (`-device tpm-tis,...` with an init blob).
- IPMI FRU storage seeding for `ipmi-bmc-sim`.
- `usb-stub` device — responds to standard descriptor requests
  from a captured blob and stalls class-specific traffic. Enables
  "device appears in bus walk" replay for non-class-compliant USB.

Replay layer behavior: when `axl-emulate <dir>` discovers an
artifact that requires a patched-QEMU feature, it checks for the
feature's presence (e.g., probes `smbus-eeprom` for the `memdev`
property via `-device help`) and either uses it or warns clearly.
No silent fallback to a degraded fixture.

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
   `--smbios-file`, `--acpi-table`, `--ipmi-sim`, `--spd <addr>:<file>`
   (repeatable; reuses the existing patched QEMU). Hand-craft the
   first fixture from the Proxmox VM (`dmidecode --dump-bin`,
   `acpidump -b`) to validate the replay path before writing the
   capture tool. Smallest possible diff.
2. **Phase HF2** — `tools/mkfixture.c` (new dedicated tool;
   `mkfixture.efi`, mirroring `mkrd.efi` in the existing tools
   tree). Cheap UEFI-protocol walks: SMBIOS, ACPI, PCI manifest,
   USB manifest + descriptors, GOP/EDID, ESRT, CPU (CPUID +
   microcode), network details (`net.json`), and `manifest.json`.
   All write to `fs0:`, `--mount` virtiofs, or HTTP POST. Run on
   the Proxmox VM, replay the captured fixture, compare against
   Phase HF1's hand-crafted one. Plumbing smoke test.
3. **Phase HF3** — `scripts/axl-emulate` (new Python wrapper;
   shipped in host-tools alongside `run-qemu.sh`). Consumes a
   fixture directory, auto-wires SMBIOS/ACPI/SPD/TPM artifacts to
   the corresponding run-qemu.sh primitives, default-drops the
   ACPI denylist, prints `manifest.json` summary at startup, and
   `exec`s run-qemu.sh. Brought forward in front of HF2 so the
   replay structure is settled before the capture tool's output
   format hardens.
4. **Phase HF4** — SPD capture: fold into `mkfixture` (preferred
   over extending `memspd`, which stays an inspection tool — same
   sysinfo-vs-mkfixture separation argument). Dump every populated
   SMBus EEPROM at 0x50–0x57 to `spd/0xNN.bin`. Validate by
   capturing on a real box and replaying via the Phase HF3 path;
   AxlSpd output should match bit-for-bit.
5. **Phase HF5** — TPM capture and replay. Capture: PCR values,
   capabilities, full TCG event log via `EFI_TCG2_PROTOCOL`. Replay:
   spawn `swtpm` with seeded state, wire `-tpmdev emulator` +
   `tpm-tis`/`tpm-crb`. Lifecycle modeled on virtiofsd (PID,
   exit-trap kill, `--background` reporting).
6. **Phase HF6** — UEFI Variable injection. Capture: walk
   `EFI_GLOBAL_VARIABLE` GUID for boot variables; capture Secure
   Boot key databases (PK, KEK, db, dbx). Replay: inject into the
   OVMF `vars.fd` copy before QEMU launch (likely via `virt-fw-vars`).
   Detect non-secboot OVMF and warn when `--secureboot` is requested
   against it.
7. **Phase HF7** — Redfish capture (HTTP walk) and replay. Capture
   on the UEFI side via `mkfixture`. **Endpoint discovery** uses
   the SMBIOS captures from HF2.1: SMBIOS Type 42 (Management
   Controller Host Interface) records publish the BMC's Redfish
   endpoint URL, credentials hint, and host-interface type
   (USB-NIC vs PCI vs OEM). mkfixture's HF2.1 `smbios.bin` already
   contains Type 42 — HF7 just parses it to know which IP/port to
   walk. Confirmed on Dell XE7745: dmidecode --type 42 shows the
   iDRAC10 Redfish-over-IP record, and axl-sdk's existing Type 42
   reader (`axl_smbios_find_redfish_host_interface`) decodes it.
   Replay lives in `axl-emulate`, which spawns DMTF
   `Redfish-Mockup-Server` from the captured `redfish/` tree and
   adds the appropriate `--hostfwd` arg to its run-qemu.sh
   invocation. Validate against OpenBMC-in-QEMU as the "real BMC"
   reference.
8. **Phase HF8** — IPMI capture (KCS sweep) and replay. Replay
   lives in `axl-emulate`, which spawns OpenIPMI `ipmi_sim` seeded
   with captured replies and tells run-qemu.sh `--ipmi-extern
   <socket>` to wire it.
9. **Phase HF9** — Additional QEMU device-injection patches as
   future fixture artifacts demand (SMBIOS handle preservation,
   non-EEPROM SMBus sensors, TPM event log seeding refinements,
   ESRT publication, NVMe Identify-Controller replay, `usb-stub`).
   The existing SPD patch is the canonical example of the pattern.
10. **Phase HF10** — Sanitization pass (`--sanitize`) and the public
    fixtures decision (separate repo vs. local-only).

Phases HF1–HF3 cover ~80% of the value; HF4–HF8 are where the
"emulate any server" goal really lands.
