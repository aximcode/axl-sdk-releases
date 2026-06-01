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

## stb (stb_image, stb_truetype)

- **Source:** https://github.com/nothings/stb
- **Versions shipped:** `stb_image` v2.30, `stb_truetype` v1.26
- **Vendored path in source tree:** `deps/stb/{stb_image.h,stb_truetype.h}`
- **Copyright:** Sean Barrett
- **License:** Dual-licensed under
  [MIT](https://spdx.org/licenses/MIT.html) **OR**
  [the Unlicense / public domain](https://spdx.org/licenses/Unlicense.html),
  at the recipient's option.
- **Full license text:** at the foot of each header (no separate
  file — the dual-license block ships inline with the source).

The two single-header libraries are `#include`d by `AxlPixmap`
(image decode: PNG/JPG/GIF/BMP) and `AxlTtf` (TrueType glyph
rasterization), so they are statically compiled into **every**
`libaxl.a` regardless of build flags — unlike mbedtls, which is
gated behind `AXL_TLS=1`. Being public-domain-or-MIT, stb imposes
no attribution obligation on redistributed binaries; this entry is
documentary. No source modifications.

## FreeType (ftgrays — analytic rasterizer)

- **Source:** https://gitlab.freedesktop.org/freetype/freetype
- **Component shipped:** `src/smooth/ftgrays.c` (the "FreeType-smooth"
  anti-aliased rasterizer) built in `STANDALONE_` mode, plus the
  headers it needs (`include/freetype/ftimage.h`, `src/smooth/ftgrays.h`).
- **Vendored path in source tree:**
  `deps/freetype/{ftgrays.c,ftimage.h,ftgrays.h}`
- **Copyright:** The FreeType Project (David Turner, Robert Wilhelm,
  Werner Lemberg, and contributors)
- **License:** Dual-licensed under the
  [FreeType License (FTL)](https://spdx.org/licenses/FTL.html) **OR**
  [GPL-2.0-or-later](https://spdx.org/licenses/GPL-2.0-or-later.html),
  at the recipient's option. **AXL takes the FTL.**
- **Full license text:** `deps/freetype/FTL.TXT` and
  `deps/freetype/LICENSE.TXT`.

`ftgrays.c` provides `AxlGfx`'s analytic path rasterizer
(`axl_gfx_fill_path`, G14) via `axl-gfx-rasterize.c`, replacing the
former 4x4 supersampler. It is compiled into **every** `libaxl.a`
(not gated). **Unlike stb, the FTL carries a credit clause:**
redistributions must acknowledge FreeType in their documentation —
"Portions of this software are copyright © The FreeType Project
(www.freetype.org). All rights reserved." Downstream products that
ship `AxlGfx` path filling must reproduce that acknowledgment. No
source modifications were made to the vendored files; the
`STANDALONE_` integration shim lives in `src/gfx/axl-gfx-rasterize.c`.

## DejaVu Sans — built-in default font

- **Source:** https://dejavu-fonts.github.io/ (DejaVu Sans)
- **Subset shipped:** ASCII + Latin-1 (U+0020..U+00FF) plus common
  typographic punctuation (U+2013/2014 dashes, U+2018..201D quotes,
  U+2022 bullet, U+2026 ellipsis) — ~23 KB.
- **Vendored path in source tree:** byte array in
  `src/gfx/fonts/font-dejavu-default.c`
- **Copyright:** Bitstream Vera Fonts © 2003 Bitstream, Inc.;
  DejaVu changes dedicated to the public domain; Arev-derived
  glyphs © Tavmjong Bah.
- **License:** [Bitstream Vera license](https://spdx.org/licenses/Bitstream-Vera.html)
  (permissive) for the Bitstream-origin glyphs; public domain for
  the DejaVu changes.
- **Full license text:** `third_party/dejavu/LICENSE`

The subset is returned by `axl_ttf_default()` and is compiled into
`libaxl.a`, but `--gc-sections` drops it from any binary that never
calls `axl_ttf_default` (consumers that load their own font pay no
size cost). **Unlike stb, the Bitstream Vera license requires the
copyright/permission notice to accompany redistributed binaries**,
so redistributors must carry `third_party/dejavu/LICENSE`. The
subset was produced with `pyftsubset` (no glyph outlines modified);
see the regeneration recipe in `font-dejavu-default.c`.

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
