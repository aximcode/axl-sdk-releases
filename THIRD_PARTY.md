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

See `third_party/edk2/README.md` for regeneration instructions.
