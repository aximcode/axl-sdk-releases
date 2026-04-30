# EDK2 Vendored Binaries

Pre-built UEFI driver binaries from [tianocore/edk2](https://github.com/tianocore/edk2),
embedded into AXL SDK tools so they can run on minimal firmware that
omits the corresponding optional UEFI 2.6+ DXE modules.

## Components

### `RamDiskDxe-x64.efi`, `RamDiskDxe-aa64.efi`

- **Source:** `MdeModulePkg/Universal/Disk/RamDiskDxe/` in the EDK2
  tree.
- **Purpose:** Publishes `EFI_RAM_DISK_PROTOCOL` (UEFI Spec 2.6+,
  §13.20) — registers a memory region as a virtual block device that
  the firmware's partition + FAT drivers attach to.
- **Used by:** `tools/mkrd.c` — embedded into `mkrd.efi` and
  `LoadImage`-from-memory'd at runtime when the host firmware doesn't
  ship the protocol. See `axl_driver_ensure_with_embedded` in
  `src/util/axl-driver.c`.
- **Build:** GCC5 toolchain via the EDK2 `MdeModulePkg.dsc` build,
  no patches.

### USB-network drivers (NetworkCommon, UsbCdcEcm, UsbCdcNcm, UsbRndis)

Same source/license/build as `RamDiskDxe` above
(`MdeModulePkg/Bus/Usb/UsbNetwork/...`). Staged in the tools tarball
at `drivers/<arch>/<name>.efi` for `axl_net_ensure_drivers` to load
when the host firmware lacks USB-class ethernet bindings.

To regenerate from upstream EDK2:

```bash
git clone --depth=1 https://github.com/tianocore/edk2.git
cd edk2
git submodule update --init --depth=1
. edksetup.sh
make -C BaseTools
build -p MdeModulePkg/MdeModulePkg.dsc -a X64 -t GCC5 \
      -m MdeModulePkg/Universal/Disk/RamDiskDxe/RamDiskDxe.inf
# Result: Build/MdeModule/{DEBUG,RELEASE}_GCC5/X64/RamDiskDxe.efi
build -p MdeModulePkg/MdeModulePkg.dsc -a AARCH64 -t GCC5 \
      -m MdeModulePkg/Universal/Disk/RamDiskDxe/RamDiskDxe.inf
```

## License

BSD-2-Clause-Patent. See [`LICENSE`](LICENSE) (canonical text from
the EDK2 distribution). Compatible with AXL SDK's Apache-2.0; the
attribution above is reproduced in `THIRD_PARTY.md` per §4(a).
