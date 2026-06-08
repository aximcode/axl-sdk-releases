# sdefl / sinfl (vendored)

Small single-header DEFLATE (RFC 1951) codec by Micha Mettke.

- **sdefl.h** — encoder (`sdeflate` raw DEFLATE, `zsdeflate` zlib, `sdefl_bound`).
- **sinfl.h** — decoder (`sinflate` raw DEFLATE, `zsinflate` zlib).

Backs `<axl/axl-compress.h>` (AxlCompress). AXL prepends/strips the gzip
and zlib framing itself and verifies CRC-32 / Adler-32 via AxlDigest, so
only the raw-DEFLATE core of these headers is used through `sdeflate` /
`sinflate`.

- **Source:** https://github.com/vurtun/lib (`sdefl.h`, `sinfl.h`)
- **License:** dual MIT / public-domain (Unlicense) — text in each header.
- **Local changes:** none — vendored verbatim. The implementation TU
  (`src/data/axl-compress.c`) sets `SINFL_NO_SIMD` before including
  `sinfl.h` so the decoder stays portable across x64/aarch64 freestanding
  builds (no `<emmintrin.h>` / `<arm_neon.h>` dependency) and avoids the
  SIMD fast-copy paths.
