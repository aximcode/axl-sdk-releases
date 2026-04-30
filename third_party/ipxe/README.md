# iPXE — vendored license texts

We do **not** vendor an iPXE binary or source in this directory. The
universal iPXE driver shipped in the tools tarball
(`drivers/<arch>/ipxe-all.efidrv`) is built fresh in CI from upstream
source at a pinned commit. See [`scripts/build-ipxe.sh`](../../scripts/build-ipxe.sh)
for the build recipe and the pinned commit hash.

What lives here:

- [`COPYING`](COPYING) — iPXE's umbrella licensing statement.
- [`COPYING.GPLv2`](COPYING.GPLv2) — full GPL-2.0 text.
- [`COPYING.UBDL`](COPYING.UBDL) — iPXE's Unmodified Binary
  Distribution License (a permissive "you may redistribute the
  unmodified binary" carve-out used by individual files).

## License (aggregate of `bin-x86_64-efi/ipxe.efidrv`)

iPXE's per-build aggregate license is determined by `make
bin-<arch>/<target>.licence`. For the universal driver build
(`bin-x86_64-efi/ipxe.efidrv`), the aggregate is **GPL-2.0-or-later**
— the most-restrictive license among the included files governs.

## How we handle GPL-2.0 §3 (binary distribution → source offer)

### Written offer (GPL-2.0 §3(b))

> AximCode hereby offers, for a period of at least three (3) years
> from the date of this distribution, to provide a complete
> machine-readable copy of the corresponding source code for the
> iPXE binary (`drivers/<arch>/ipxe-all.efidrv`) included in this
> distribution, on a medium customarily used for software
> interchange, for a charge no more than the reasonable cost of
> physically performing source distribution. Send requests to:
>
>   - **Email:** support@aximcode.com
>   - **Web:** https://aximcode.com
>
> Alternatively, the same source is publicly available at
> https://github.com/ipxe/ipxe at the pinned commit
> recorded in `scripts/build-ipxe.sh` (`IPXE_COMMIT=...`); this
> URL is also printed by every successful invocation of that
> build script.

To reproduce our binary exactly:

```bash
git clone https://github.com/ipxe/ipxe
cd ipxe && git checkout <pinned-commit>
make -C src bin-x86_64-efi/ipxe.efidrv
```

(Equivalently: run `scripts/build-ipxe.sh` from the axl-sdk tree.)

## Why we don't bundle a pre-built binary in this repo

Building from upstream at a pinned commit is reproducible and keeps
the axl-sdk repo lighter. CI builds the binary every release; the
build is ~35s on a fresh runner. See
`docs/AXL-Network-Driver-Bundle-Design.md` for the rationale.

## Mere-aggregation note

`ipxe-all.efidrv` ships in the tools tarball alongside axl-sdk's own
Apache-2.0-licensed `.efi` binaries. The iPXE driver is unmodified
upstream output — separate file, separate license, no static linking
into axl-sdk binaries. Per GPL-2.0 §3, this is "mere aggregation on
a volume of a storage or distribution medium" and does not bring
axl-sdk's own files under GPL terms.
