# AXL Network Driver Bundle — v0.6.0 Design

Status: **Prototype validated 2026-04-29**, target release v0.6.0.

## Problem

`mkrd` v0.5.2 embeds `RamDiskDxe.efi` directly into the tool binary,
making it self-contained on minimal firmware that lacks UEFI 2.6
`EFI_RAM_DISK_PROTOCOL`. That trick works because RAM disk is **one
canonical hardware-independent driver** — every consumer needs the
same blob.

Network tools (`netinfo`, `fetch`, `rfbrowse`) cannot use the same
trick:

- NIC drivers are **hardware-specific** (PCI VID/DID match on the
  actual silicon).
- Bundling all of them inflates each binary by hundreds of KB.
- Most vendor drivers (Realtek, ASIX, Broadcom OEM builds) are
  proprietary; aggregating them inside an Apache-2.0 binary creates
  redistribution problems we don't want.

Real-user-affecting data point: a 2010-era Dell EDK1 firmware
(`Edk-Dev-Snapshot-20100527`) has no UEFI 2.6 NIC drivers built in
and no UEFI 2.x option ROMs to fall back on. The current tools
fail there.

## Approach: ship drivers as separate files in the tools tarball

`axl-sdk-tools-<arch>.tar.gz` gains a `drivers/<arch>/` directory:

```
axl-sdk-tools-x64.tar.gz
├── (existing tool .efis)
├── drivers/x64/
│   ├── RamDiskDxe.efi          # also embedded in mkrd
│   ├── NetworkCommon.efi       # MNP/IP4/TCP4 stack glue (EDK2)
│   ├── UsbCdcEcm.efi           # USB CDC-ECM ethernet (EDK2)
│   ├── UsbCdcNcm.efi           # USB CDC-NCM ethernet (EDK2)
│   ├── UsbRndis.efi            # USB RNDIS ethernet (EDK2)
│   └── ipxe-all.efidrv         # universal iPXE NIC driver (GPL-2.0+)
├── third_party/
│   ├── edk2/{LICENSE,README.md}
│   └── ipxe/{LICENSE,README.md}
└── README.txt                  # explains drivers/ layout
```

`axl_net_ensure_drivers` ([`src/net/axl-net-dhcp.c:146`](../src/net/axl-net-dhcp.c#L146))
already loops through a fixed candidate list of driver names and
locates each via `axl_driver_locate`'s standard 4-path search.
Adding `ipxe-all.efidrv` to the candidate list and dropping the
files at `drivers/<arch>/` is the entire scheme.

**Mere aggregation, not static link.** Putting separately-licensed
binaries in the same tarball does not propagate license obligations
across them. This is how Linux distros ship GPL binaries alongside
permissively-licensed ones. iPXE's GPL-2.0+ stays self-contained
in `ipxe-all.efidrv`; our Apache-2.0 tools stay self-contained in
their own `.efi` files. Required: top-level GPL notice +
source availability for the iPXE blob (canonical `git clone` URL +
exact commit hash, or a companion source tarball).

## Bundle composition

| Component | Size (x64) | License | Origin |
|---|---|---|---|
| `RamDiskDxe.efi` | 33 KB | BSD-2-Clause-Patent | EDK2 `MdeModulePkg/Universal/Disk/RamDiskDxe` |
| `NetworkCommon.efi` | 13 KB | BSD-2-Clause-Patent | EDK2 `MdeModulePkg/Bus/Usb/UsbNetwork/NetworkCommon` |
| `UsbCdcEcm.efi` | 7.5 KB | BSD-2-Clause-Patent | EDK2 `MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEcm` |
| `UsbCdcNcm.efi` | 8.2 KB | BSD-2-Clause-Patent | EDK2 `MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcNcm` |
| `UsbRndis.efi` | 12 KB | BSD-2-Clause-Patent | EDK2 `MdeModulePkg/Bus/Usb/UsbNetwork/UsbRndis` |
| `ipxe-all.efidrv` | ~500–800 KB | GPL-2.0+ | iPXE master, `ALL_DRIVERS=1` build |

**Excluded** (proprietary, license review pain not worth it):
`RtkUndiDxe.efi`, `RtkUsbUndiDxe.efi`, `AsixUsbUndiDxe.efi`. iPXE
covers Realtek RTL8139/8169 series and most USB ethernet classes
internally, so practical NIC coverage stays good without them.
Users with vendor-locked NICs can still drop those drivers next to
the tools manually — the tarball just won't ship them.

## Validation in QEMU

Prototype confirmed 2026-04-29 with four-test matrix:

| Test | QEMU setup | Expected | Actual |
|---|---|---|---|
| A | `--net` (virtio-net default + ROM) | netinfo works via OVMF VirtioNetDxe | ✅ 3 SNP handles, NIC up |
| B | `--nic-model e1000 --nic-no-rom` | "no NIC drivers found" — gap reproduced | ✅ aborts cleanly |
| C | Same gap + `ipxe-intel.efi` staged + shell `load` | netinfo works via staged driver | ✅ NIC up, MAC verified |
| D | Same gap + `ipxe-intel.efi` staged + shell `load` + `fetch` over user-mode networking | DHCP + HTTP file transfer succeeds | ✅ DHCP in 5s, HTTP 200, file body matches |

### Test D evidence (full end-to-end)

Host setup: `python3 -m http.server 18080` serving `/tmp/iperetest-http/test.txt`.

QEMU command:

```bash
./scripts/run-qemu.sh --nic-model e1000 --nic-no-rom --net \
    --extra ipxe-intel.efi:drivers/x64/ipxe-intel.efi \
    --nsh fetch.nsh \
    --timeout 60 \
    out/native-x64/tools/fetch.efi
```

`fetch.nsh`:

```
@echo -off
load fs0:\drivers\x64\ipxe-intel.efi
connect -r
fs0:\fetch.efi -v http://10.0.2.2:18080/test.txt
```

Guest output (key lines):

```
Image 'fs0:\drivers\x64\ipxe-intel.efi' loaded at 1DE90000 - Success
Connect - Handle [B4] Result Success.
[INFO]  net: network ready after 5 seconds      ← DHCP succeeded
> GET http://10.0.2.2:18080/test.txt
< HTTP 200
< server: SimpleHTTP/0.6 Python/3.12.12
< content-type: text/plain
< content-length: 110
Hello from host HTTP server — verified by axl-sdk fetch through
  iPXE-driven NIC. Date: 2026-04-30T01:04:27Z
```

This proves the full network stack — NIC → SNP → MNP → IP4 →
DHCP4 → TCP4 → HTTP — flows through the **staged-from-disk iPXE
driver** with no firmware NIC support and no QEMU option ROM.
The same path on Mark's 2010 Dell EDK1 firmware should produce
the same result.

TLS path (https:// in `fetch`, or `rfbrowse`) was not separately
tested — TLS sits atop TCP4 and depends on mbedtls (statically
linked into the tool). Once TCP4 works through the iPXE driver,
TLS works without additional bundle pieces.

### Avoiding QEMU/OVMF false positives

QEMU bundles iPXE PXE option ROMs for most common NICs (Intel,
Broadcom, Realtek RTL8139/8169, virtio). When you do `-device
e1000,netdev=...` without `romfile=`, QEMU loads its bundled
`pxe-e1000.rom` and OVMF wraps the resulting UNDI as SNP — making
it appear that "the firmware has the driver" for any common NIC.

To validate the staged-driver path under QEMU, suppress the option
ROM. New `--nic-no-rom` flag in [`scripts/run-qemu.sh`](../scripts/run-qemu.sh)
passes `romfile=` to the `-device` line. Do that, and any NIC
QEMU emulates becomes an "OVMF lacks driver" scenario — the
authentic test case for our bundle. Without it, results are
meaningless.

The `--mount` route was also tried (host directory exposed via
virtiofs, expecting `axl_driver_locate` path 4 to find the driver
on the second mounted volume) but the OVMF in this build doesn't
ship `VirtioFsDxe`. Working around it: `--extra SRC:DEST` with a
relative `DEST` path now stages files into arbitrary subpaths of
the boot disk image. (`--mount` becomes the cleaner choice on
firmware that includes `VirtioFsDxe`, or once a standalone
`VirtioFsDxe.efi` ships in our host-tools tarball.)

## Open SDK fixes (gated by v0.6.0 release)

Two issues surfaced during prototyping that need to land before
v0.6.0 ships. Neither blocks the bundle design — but both are
required for the bundle to actually work without manual `load`
gymnastics from a startup.nsh.

### 1. `axl_driver_load` must load via DevicePath, not memory buffer

[`src/util/axl-driver.c:32`](../src/util/axl-driver.c#L32) reads the
`.efi` file into memory and calls
`gBS->LoadImage(SourceBuffer=..., DevicePath=NULL)`. The firmware
leaves `LoadedImage->FilePath = NULL` for buffer-loaded images.
iPXE's driver entry reads `FilePath` to figure out where it was
loaded from (so it can find iPXE settings + scripts), and bails
with `EFI_INVALID_PARAMETER` from `StartImage` when it's NULL.

UEFI shell `load` succeeds because it loads via DevicePath: the
firmware constructs `<volume DP> + MEDIA_FILEPATH_DP` automatically
and sets `FilePath` correctly.

**Fix**: rework `axl_driver_load(path)` to:

1. Parse `path` for the `fsN` volume name and the file portion.
2. Look up the volume handle by name (axl-sdk's `axl_volume_enumerate`
   already does this).
3. `HandleProtocol(vol_handle, EFI_DEVICE_PATH_PROTOCOL_GUID)` to
   get the volume's device path.
4. Append a `MEDIA_FILEPATH_DP` node containing the file portion +
   `END` node.
5. Call `LoadImage(BootPolicy=FALSE, parent, dp, NULL, 0, &h)` —
   firmware reads file via DevicePath and sets `FilePath`.

A previously-tried workaround — load from buffer then patch
`LoadedImage->FilePath` to a synthetic single-node DP — does **not**
satisfy iPXE; it likely walks for a parent volume node. Real
device-path load is required.

~50–80 lines of code; tests in QEMU should pass once this lands.

### 2. netinfo diagnostic must walk past the SnpDxe wrapper

The current `-v` output reports the agent that opened the SNP
handle BY_DRIVER. With iPXE driving the NIC, the chain is:

```
iPXE → installs EFI_NETWORK_INTERFACE_IDENTIFIER (NII)
OVMF SnpDxe → installs EFI_SIMPLE_NETWORK_PROTOCOL on top of NII
netinfo → reads SNP, sees SnpDxe (firmware volume) as the "driver"
```

So `<firmware volume>` is technically correct but uninformative
when the actual NIC-binding driver is iPXE.

**Fix**: when reporting per-NIC drivers, also call
`OpenProtocolInformation(handle, EFI_NETWORK_INTERFACE_IDENTIFIER_PROTOCOL_GUID)`
and report that agent's image path instead of (or alongside) the
SNP-installing one. The NII installer is the actual NIC driver.

## v0.6.0 work breakdown

Status as of 2026-04-29 (in working tree, uncommitted):

1. ✅ **`axl_driver_load` DevicePath rework** — done; loads via
   the volume's full DP + MEDIA_FILEPATH_DP, with memory-buffer
   fallback for drivers that don't read FilePath. iPXE now starts
   cleanly via `axl_net_ensure_drivers` (was returning
   EFI_INVALID_PARAMETER).
2. ✅ **`axl_driver_connect(NULL)` enumerate-and-connect** — fixes
   the silent no-op where `gBS->ConnectController(NULL,...)`
   returns EFI_INVALID_PARAMETER per spec. Now mirrors UEFI shell's
   `connect -r`.
3. ✅ **netinfo diagnostic walks NII layer** — `-v` output surfaces
   the actual NIC-binding driver image (e.g.
   `\drivers\x64\ipxe-all.efidrv`) instead of the SnpDxe wrapper.
   NII3.1 → NII (legacy) → SNP fallback chain.
4. ✅ **iPXE build script** — [`scripts/build-ipxe.sh`](../scripts/build-ipxe.sh)
   clones iPXE at pinned commit `df4eec8c` and builds the universal
   `bin-<arch>-efi/ipxe.efidrv` (~1.1 MB, ~2.9k chip IDs on x64).
   Reproducible (35s wall on 16-thread). Validated against e1000,
   e1000e, rtl8139, pcnet under `--nic-no-rom`.
5. ✅ **Packaging wired** — `release.yml` and `build-packages.sh`
   build iPXE in CI, stage `drivers/<arch>/` with `ipxe-all.efidrv`
   plus a small set of auxiliary USB-network drivers (kept for
   compat with firmware that benefits from them — see
   `third_party/edk2/README.md`), and carry
   `third_party/{edk2,ipxe,mbedtls}/...` attribution.
   THIRD_PARTY.md updated.

Pending:

7. **Tag v0.6.0**, update CHANGELOG, push.
8. **Validate on real hardware** — loaner with a NIC OVMF doesn't
   natively support (Mark's 2010 EDK1 Dell would do; another
   option is a Realtek-only PC).

## Final consumer-flow validation

Tarball produced by local `build-packages.sh` smoke-test contains:

```
drivers/x64/
  ipxe-all.efidrv          (1.1 MB, primary universal NIC driver)
  RamDiskDxe.efi           (33 KB, also embedded in mkrd.efi)
  + 4 auxiliary USB-network drivers (~40 KB total — see third_party/)
third_party/
  edk2/{LICENSE,README.md}
  ipxe/{COPYING,COPYING.GPLv2,COPYING.UBDL,README.md}
  mbedtls/LICENSE
```

End-to-end consumer flow: extract tarball → run
`fetch.efi http://...` under `--nic-model rtl8139 --nic-no-rom` →
`axl_net_ensure_drivers` auto-loads `UsbCdcNcm.efi`, `UsbRndis.efi`,
`ipxe-all.efidrv` from `drivers/x64/` → iPXE binds rtl8139 via NII →
DHCP → HTTP 200 → 53-byte file body received, no memory leaks.
Verified 2026-04-29.

## Tests at v0.6.0

- 1695/1695 unit tests (x64), 1695/1695 (aa64)
- 23/23 tools integration
- iPXE build script: 35s, reproducible, both archs
- Tarball pipeline: end-to-end consumer flow proven against
  e1000, e1000e, rtl8139, pcnet

## Prototype artifacts kept

These were added during prototyping and are useful independently:

- [`scripts/run-qemu.sh`](../scripts/run-qemu.sh): `--nic-model
  MODEL`, `--nic-no-rom`, and `--extra SRC:DEST` (relative-path
  staging into the boot disk).
- [`tools/netinfo.c`](../tools/netinfo.c): `-v` now prints a "NIC
  Drivers" section identifying the driver image bound to each SNP
  handle, and raises log level so `axl_net_ensure_drivers` debug
  surfaces. Note: per "fix 2" above this output is correct but
  one layer too high until the NII walk is added.
