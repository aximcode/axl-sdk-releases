# Third-party components

This AXL SDK distribution statically links the following third-party
components into `libaxl.a` and into the pre-built tool `.efi` binaries
shipped in the `axl-sdk-uefi-tools-*.tar.gz` tarballs. Each component retains
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

## sdefl / sinfl (DEFLATE codec)

- **Source:** https://github.com/vurtun/lib (`sdefl.h`, `sinfl.h`)
- **Version shipped:** single-header, Copyright (c) 2020–2023 Micha
  Mettke. Upstream is unversioned; vendored verbatim.
- **Vendored path in source tree:** `deps/sdefl/{sdefl.h,sinfl.h}`
- **Copyright:** Micha Mettke
- **License:** Dual-licensed under
  [MIT](https://spdx.org/licenses/MIT.html) **OR**
  [the Unlicense / public domain](https://spdx.org/licenses/Unlicense.html),
  at the recipient's option.
- **Full license text:** at the foot of each header (`ALTERNATIVE A —
  MIT License` / `ALTERNATIVE B — Public Domain`) — no separate file,
  the dual-license block ships inline with the source (like stb).

`sdefl.h` (encoder) and `sinfl.h` (decoder) provide the raw-DEFLATE
(RFC 1951) core behind `AxlCompress` (`<axl/axl-compress.h>`, via
`src/data/axl-compress.c`); AXL adds the gzip/zlib framing and
CRC-32 / Adler-32 verification itself (`AxlDigest`), so only the
`sdeflate` / `sinflate` raw cores are used. `AxlCompress` is compiled
into **every** `libaxl.a` (not gated by any build flag), but
`--gc-sections` drops it from any binary that never calls the compress
API. Among the shipped tools, only `tar` (its `-z` gzip mode) and
`mkfixture` (gzip HTTP POST of captured fixtures) link it in; every
other tool GCs it out. Being MIT/public-domain, sdefl imposes no
attribution obligation on redistributed binaries; this entry is
documentary. The vendored headers are unmodified — the only build-time
change is `#define SINFL_NO_SIMD` set by the consuming TU before
including `sinfl.h`, keeping the decoder portable across freestanding
x64 / aarch64 (no `<emmintrin.h>` / `<arm_neon.h>` dependency).

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
ship `AxlGfx` path filling must reproduce that acknowledgment. The
`STANDALONE_` integration shim lives in `src/gfx/axl-gfx-rasterize.c`.

**One source modification has been made**, marked in-place with an
`AXL fix:` comment in `deps/freetype/ftgrays.c`. Upstream's memory
estimate reads `max_ex - min_ey`, mixing the x-span's end with the
y-span's start; the typo is present in current ftgrays master. When a
clip box has `max_ex < min_ey` — a glyph rotated so its narrow x-extent
sits at a large y-offset — that signed span goes negative and converts
to a huge `estimate`, after which `(size_t)estimate * sizeof(TCell)`
wraps to a small allocation while `cell_null` points far past it,
corrupting memory. It was observed as a hang rasterizing rotated text
through `axl_ttf_draw_affine`. Corrected to `max_ex - min_ex`, matching
the neighbouring `max_ey - min_ey` y-span term. The change is safe
independently of the bug: `estimate` is only a sizing hint, and
`gray_convert_glyph` bands-and-retries on pool overflow.

Consumers that only **enumerate** displays do not incur the FTL: the
GOP-inventory / mode-query accessors (`axl_gfx_output_count` / `_get` /
`_query_mode` / `_get_pixel_bitmask`) live in their own translation unit
(`src/gfx/axl-gfx-output.c`) with no path-rasterization code, so
`--gc-sections` keeps `ftgrays` out of a binary that never calls a
path-fill API. The FTL acknowledgment is required only of products that
ship `AxlGfx` path filling.

## libvterm (VT/xterm terminal-emulator core)

- **Source:** https://github.com/neovim/libvterm (neovim's maintained
  fork of Paul Evans' libvterm)
- **Version shipped:** commit `934bc2fbf21800ac3458a499df8820ca5fb45fd3`
  (2025-11-20), reporting `VTERM_VERSION` 0.3.3
- **Vendored path in source tree:** `deps/libvterm/`
- **Copyright:** Copyright (c) 2008 Paul Evans <leonerd@leonerd.org.uk>
- **License:** [MIT](https://spdx.org/licenses/MIT.html)
- **Full license text:** `deps/libvterm/LICENSE`

libvterm parses a real VT/xterm byte stream — a serial or SOL console —
into structured terminal operations, and is the second producer behind
the `AxlConsoleOps` contract (`AxlVterm`, `<axl/axl-vterm.h>`, Layer 2).
The library is vendored in full; only Layer 2 is compiled and bound, so
`--gc-sections` keeps it out of any binary that never constructs a
`VTerm`.

**The fork is not ABI-identical to Paul Evans' 0.3.3 despite reporting
the same version macros** — neovim's fork adds
`VTermStateCallbacks.premove` and `VTermScreenCallbacks.sb_pushline4`.
Do not check this copy's contract against a distro `/usr/include/vterm.h`.

**Eight local modifications**, confined to the five compiled files
(`screen.c` is unmodified). Each is marked in-source as
`AXL patch [n/8]`; `deps/libvterm/README.md` documents them individually
and `grep -rn 'AXL patch \[' deps/libvterm/src/` finds them all. Seven
are freestanding-portability or memory-safety fixes — routing the
default allocator through `axl_calloc`/`axl_free`, dropping `<stdio.h>`
and `<stdlib.h>`, and removing a `vterm_screen_free()` reference that
would otherwise pull all of `screen.c` into every image that frees a
`VTerm`. Patch 8 is the sole behaviour change; see its entry in that
README. Being MIT, libvterm imposes no attribution obligation beyond
reproducing the copyright and permission notice, which
`deps/libvterm/LICENSE` carries.

## LZMA SDK (LZMA codec)

- **Source:** LZMA SDK by Igor Pavlov (https://7-zip.org/sdk.html)
- **Version shipped:** LZMA SDK 26.01; the vendored files carry their
  own dates (e.g. `LzmaDec.h` 2023-04-02)
- **Vendored path in source tree:** `deps/lzma/`
- **Copyright:** Igor Pavlov. Portions are based on public-domain code
  by other authors, as the SDK's own LICENSE records: PPMd var.H (2001)
  by Dmitry Shkarin, and SHA-256 by Wei Dai (Crypto++ library).
- **License:** **public domain** — "LZMA SDK is written and placed in
  the public domain by Igor Pavlov."
- **Full license text:** `deps/lzma/LICENSE`

A curated subset (encoder, decoder and their support headers) providing
LZMA decompression for `AxlFw`, which parses raw `.fd` / SPI images and
decompresses LZMA-compressed firmware-volume sections on demand.

Being public domain, the LZMA SDK imposes no attribution obligation on
redistributed binaries; this entry is documentary. `deps/lzma/errno.h`
is **not** upstream — it is an AXL-authored shim supplying the handful
of `errno` definitions the SDK expects, which a freestanding UEFI build
does not otherwise provide.

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

`axl-sdk-uefi-tools-*-{x64,aa64}.tar.gz` ships
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

## Not part of this distribution

For the avoidance of doubt during license audits, the components listed
above are the **complete** set of third-party code that AXL vendors and
redistributes. `deps/` in a working tree may contain other directories
— `deps/quickjs/`, `deps/lexbor/`, and the spec dirs
`deps/{uefi,pi,acpi,shell}-spec/` — but these are **not** part of the
SDK:

- The `.gitignore` excludes everything under `deps/` except the four
  vendored components (`mbedtls`, `stb`, `freetype`, `sdefl`), so
  `deps/quickjs/` and `deps/lexbor/` are untracked local checkouts. No
  AXL source or build rule references them; they are never compiled into
  `libaxl.a`, never linked into any tool, and absent from every release
  artifact.
- `deps/*-spec/` are HTML/PDF specification documents downloaded by
  `scripts/download-uefi-specs.py` purely as input to the header
  generator (`scripts/generate-uefi-headers.py`). They are gitignored
  reference material — not compiled code and not redistributed.

If any of these is ever genuinely vendored and linked, it must be
whitelisted in `.gitignore` and given an entry above.
