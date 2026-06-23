# Design: AxlFw Phase 2 — Tiano/EFI decompress + metadata + `report`

**Date:** 2026-06-23
**Status:** approved (brainstorm), pending implementation plan
**Builds on:** Phase 0+1 (merged) — `docs/superpowers/specs/2026-06-22-axl-fw-firmware-image-design.md`

## Summary

Phase 2 of the AxlFw firmware-image library. Adds the EFI/Tiano decompressor so
`SECTION_COMPRESSION` CompressionType=1 sections decode (the "works on real OEM
firmware" unlock — OVMF/AAVMF are all LZMA, but real OEM FVs use Tiano), decodes
USER_INTERFACE and VERSION metadata so the tree shows human names, and adds a
`fwtool report` command that extracts the whole image to disk with a manifest.

## Decisions (settled in brainstorming)

| # | Decision | Choice |
|---|---|---|
| 1 | `report` command | Enrich `list` (UI name + version inline); `report <image> <outdir>` = recursive **extract-all** to a directory tree + a text manifest. |
| 2 | EFI/Tiano decoder home + scope | **`axl-compress`, decode-only** (`AXL_COMPRESS_EFI`). Port EDK2 `BaseUefiDecompressLib` (BSD). `axl_compress(AXL_COMPRESS_EFI,…)` → unsupported. |
| 3 | Metadata sections | **USER_INTERFACE (0x15) + VERSION (0x14)**, UTF-16LE→UTF-8. New public accessor `axl_fw_node_name`. Skip FREEFORM_SUBTYPE_GUID. |
| 4 | Tiano test vectors | Locally-installed **EDK2 BaseTools** (`TianoCompress`, `GenSec`, `GenFfs`) + `uefiextract`. No public OEM image needed. |
| 5 | Tool-dependent tests | **Skip + warn** (never fail) when `TianoCompress`/`GenSec`/`GenFfs`/`uefiextract` are absent. The unit golden uses committed fixture bytes and always runs. |

## Environment (verified 2026-06-23)

`/usr/bin/TianoCompress`, `/usr/bin/GenSec`, `/usr/bin/GenFfs`, `/usr/bin/LzmaCompress`
present (EDK2 BaseTools); `uefiextract` at `~/.local/bin`. `TianoCompress -e`/`-d`
round-trips. Local OVMF has **no** Tiano sections (all LZMA), confirming the need
for tool-generated Tiano vectors.

---

## Component 1 — `AXL_COMPRESS_EFI` decoder (`axl-compress`)

- New enum value `AXL_COMPRESS_EFI` in `AxlCompressFormat`.
- `axl_decompress(AXL_COMPRESS_EFI, in, in_len, &out, &out_len)` decodes an
  EFI/Tiano-compressed stream (the payload of a `SECTION_COMPRESSION`
  CompressionType=1 section: an 8-byte header — CompSize LE32 + OrigSize LE32 —
  followed by the compressed data). `axl_compress(AXL_COMPRESS_EFI,…)` → `AXL_ERR`
  (decode-only).
- **Vendoring:** EDK2 `MdePkg/Library/BaseUefiDecompressLib` core into
  `deps/edk2-decompress/` (BSD-2-Clause-Patent; keep the license). The core
  `Decompress()` takes a `Version` flag (1 = EFI 1.1, 2 = Tiano). A thin AXL
  wrapper (`src/data/axl-compress-efi.c`) runs `UefiDecompressGetInfo` to size the
  output + scratch, allocates via `axl_malloc`, and calls `Decompress`. Like
  UEFITool, **try Tiano (v2) first, fall back to EFI 1.1 (v1)** on failure.
- **Bounds/safety:** the GetInfo-reported OrigSize is checked against
  `AXL_COMPRESS_MAX_OUTPUT` before allocation; malformed/truncated input →
  `AXL_ERR` with `*out`/`*out_len` cleared (mirror the LZMA wrapper's discipline).
- **Freestanding:** the EDK2 lib is already UEFI/freestanding C; shim its few
  type/`memcpy`-isms as the LZMA SDK was shimmed if the build requires it.

**Open detail for the plan:** confirm whether the EDK2 source needs the
`SECTION_COMPRESSION` 8-byte section header stripped by the caller (the parser) or
the codec — decide one owner. Proposed: the *parser* passes the post-header
compressed payload + the OrigSize, the codec decodes raw (consistent with how the
LZMA wrapper receives the alone stream).

## Component 2 — parser changes (`src/fw/axl-fw.c`, `include/axl/axl-fw.h`)

- **Tiano section decode:** in `fw_parse_sections`, `SECTION_COMPRESSION` with
  `comp_type == 1` → `axl_decompress(AXL_COMPRESS_EFI, payload, payload_len, &dec,
  &dec_len)`; on success record `dec` in `owned_bufs` **before** recursing, then
  recurse into it as a section stream (identical ownership discipline to the
  GUIDED-LZMA path). On failure → opaque leaf (unchanged). Respect `FW_MAX_DEPTH`.
- **Metadata decode (UI name, in the library):** when a FILE has a USER_INTERFACE
  (0x15) child section, decode its UTF-16LE body → UTF-8 and store it as the FILE
  node's name. Reuse AXL's existing UCS-2→UTF-8 conversion (`axl-str`).
- **New public API** (`axl-fw.h`, contract-first reviewed) — exactly one accessor:
  `bool axl_fw_node_name(AxlFwNode *node, char *buf, size_t cap)` — writes the
  node's display name (the FILE's UI name; for a USER_INTERFACE section node, its
  own string) NUL-terminated into `buf`, returns true if a name exists. NULL-safe.
  This is the only new public symbol — file identity requires walking child
  sections, which is awkward for a consumer to do, so the library owns it.
- **VERSION is NOT a new library accessor.** It is niche; `fwtool` decodes the
  VERSION (0x14) section body inline (2-byte BuildNumber + UTF-16LE string) for
  display and the manifest, reading it via the existing `axl_fw_node_data`. Keeps
  the public surface minimal.

## Component 3 — `fwtool` (`tools/fwtool.c`)

- **`list` enrichment:** for a FILE node, print its UI name (via `axl_fw_node_name`)
  and version after the GUID. Tiano sections now expand (children printed) instead
  of appearing as opaque leaves.
- **`report <image> <outdir>` (new):** recursively walk the tree; for every leaf
  section with a body, write the body to a file under a directory tree that mirrors
  the FV/FFS/section nesting (dir/file names = `NN-KIND[-TYPE][-GUID]`). Write a
  text **manifest** (`manifest.txt`) at the root: one line per node with
  indent/path, kind, type, GUID, name, offset, size. The directory layout is
  fwtool's own (not uefiextract's); each extracted body is byte-exact (same
  guarantee as `extract`). Host + UEFI builds (the host `report` writes via the
  existing `axl_file_set_contents`/host shim; directory creation uses the host/UEFI
  fs API — confirm a mkdir-equivalent exists in `axl-fs`, else create dirs via the
  available primitive, plan-time detail).

## Component 4 — testing

- **Unit golden — decode** (`test/unit/axl-test-data.c`): a `TianoCompress -e`'d
  blob, **committed as a byte array** in the test (generated once offline; the test
  has NO runtime tool dependency and always runs), decodes byte-exact to its known
  plaintext via `axl_decompress(AXL_COMPRESS_EFI,…)`. Negatives: truncated header,
  forged oversize, truncated body → `AXL_ERR`, `*out` cleared.
- **Unit — parser** (`test/unit/axl-test-fw.c`): a synthetic FV (hand-built bytes,
  or a committed `TianoCompress`-generated section) with a Tiano-compressed PE32
  section expands to the PE32 child; a USER_INTERFACE section yields the file name
  via `axl_fw_node_name`; a VERSION section decodes. Committed fixtures, always run.
- **Integration — real-tooling cross-check**
  (`test/integration/test-fwtool-tiano.sh`): build a Tiano-compressed FV at test
  time with `GenSec`/`GenFfs` (+ `TianoCompress`), then assert `fwtool`'s
  extraction of the inner body byte-matches `uefiextract`'s. **Skips with a warning
  (exit 0, logged) if any of `TianoCompress`/`GenSec`/`GenFfs`/`uefiextract` is
  absent** — never fails on a toolless box. `TEST_SKIP_RATCHET=1` (integration).
- **Skip discipline:** per `feedback_uefi_firmware_test_hazards` / the balanced-SKIP
  rule, the integration test's skip is a clean logged SKIP, balanced across arches
  (the host tool is arch-independent, so it's a single skip).

## Out of scope (Phase 2)

- EFI/Tiano *encoding* (decode-only).
- FREEFORM_SUBTYPE_GUID decoding; STRING/other section-type bodies.
- Intel flash-descriptor regions, NVRAM, microcode, capsules (Phases 3–5).
- Matching `uefiextract`'s on-disk directory layout in `report` (only per-leaf bytes
  are guaranteed identical).
- Image rewriting (the whole project is read-only).

## Risks / watch-items

- **EDK2 decompress freestanding build** — the main unknown; prototype the
  `TianoCompress`-golden decode byte-match first (like the LZMA gate), before the
  parser/tool layer on it.
- **Tiano vs EFI-1.1 ambiguity** for CompressionType=1 — the try-Tiano-then-EFI-1.1
  fallback must be validated against both a `TianoCompress` vector and (if one
  exists) an EFI-1.1 vector; document which `GenSec` flag produces which.
- **`report` directory creation** on the UEFI side — confirm an `axl-fs` mkdir
  primitive exists; if not, the UEFI `report` may be host-first with the UEFI path
  using whatever directory API is available (plan-time decision; do not block the
  host path on it).
