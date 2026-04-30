# Third-party components

This AXL SDK distribution statically links the following third-party
components into `libaxl.a` and into the pre-built tool `.efi` binaries
shipped in the `axl-sdk-tools-*.tar.gz` tarballs. Each component retains
its original copyright and license. The full license text of each
component is included in the `third_party/<component>/` subdirectory
next to this file.

Redistributors of binaries built against this SDK must preserve these
attributions.

## mbedtls

- **Source:** https://github.com/Mbed-TLS/mbedtls
- **Version shipped:** 3.6.3
- **Vendored path in source tree:** `deps/mbedtls/`
- **Copyright:** The Mbed TLS Contributors
- **License:** Dual-licensed under
  [Apache License 2.0](https://spdx.org/licenses/Apache-2.0.html)
  **OR**
  [GNU General Public License v2.0 or later](https://spdx.org/licenses/GPL-2.0-or-later.html).
  **This distribution elects the Apache 2.0 license.**
- **Full license text:** `third_party/mbedtls/LICENSE`

The mbedtls `3rdparty/everest/` and `3rdparty/p256-m/` subdirectories
contain code from separate upstream projects (Project Everest and
mpg/p256-m, respectively). Both are distributed within mbedtls under
compatible Apache 2.0 terms; see the mbedtls LICENSE file for details.

No modifications have been made to the vendored mbedtls source.

## EDK2 — RamDiskDxe.efi

- **Source:** https://github.com/tianocore/edk2 — `MdeModulePkg/Universal/Disk/RamDiskDxe/`
- **Vendored path in source tree:** `third_party/edk2/RamDiskDxe-{x64,aa64}.efi`
- **Copyright:** 2017–2024, Intel Corporation. All rights reserved.
- **License:** [BSD-2-Clause-Patent](https://spdx.org/licenses/BSD-2-Clause-Patent.html)
- **Full license text:** `third_party/edk2/LICENSE`

The pre-built `.efi` driver binaries are embedded into `tools/mkrd.efi`
as `static const unsigned char[]` arrays at build time and
`LoadImage`'d from memory at runtime when the host UEFI firmware does
not ship `EFI_RAM_DISK_PROTOCOL` (a UEFI 2.6+ optional protocol).
This lets `mkrd.efi` work as a self-contained binary on legacy /
minimal firmware. No source modifications; the binaries are stock
EDK2 GCC5 builds.

A small set of additional `MdeModulePkg/Bus/Usb/UsbNetwork/...`
driver binaries (`NetworkCommon`, `UsbCdcEcm`, `UsbCdcNcm`,
`UsbRndis`) ship alongside under the same source/license/build
recipe. See `third_party/edk2/README.md`.

## iPXE — universal NIC driver

- **Source:** https://github.com/ipxe/ipxe
- **Vendored at:** `third_party/ipxe/{COPYING,COPYING.GPLv2,COPYING.UBDL,README.md}`
  (license texts only — the binary is built from upstream at a pinned
  commit by [`scripts/build-ipxe.sh`](scripts/build-ipxe.sh))
- **Pinned commit:** see `IPXE_COMMIT=` in `scripts/build-ipxe.sh`
- **License (aggregate):** [GPL-2.0-or-later](https://spdx.org/licenses/GPL-2.0-or-later.html)
- **Full license texts:** `third_party/ipxe/COPYING.GPLv2`,
  `third_party/ipxe/COPYING.UBDL`

`axl-sdk-tools-{x64,aa64}.tar.gz` ships
`drivers/<arch>/ipxe-all.efidrv` — an unmodified upstream iPXE build
of `bin-<arch>-efi/ipxe.efidrv` (~1.1 MB) — as a universal NIC driver
fallback. Covers Intel (e1000 / e1000e / i219 / i225), Broadcom
(BCM4401 / 5760x / 957454), Realtek (RTL8139 / 8169 / 8125 / 8153
USB), Atheros, 3Com, AMD, NSC, VIA, USB CDC-ECM / CDC-NCM / RNDIS,
AX88179, and many more — ~2.9k chip IDs total.

The binary ships in the tools tarball alongside our Apache-2.0 tools
under "mere aggregation" (GPL-2.0 §3) — no static linking into our
binaries. The build is reproducible from the pinned commit; per
GPL §3(b) the upstream URL + commit hash printed by
`scripts/build-ipxe.sh` constitutes the "written offer" for source.
