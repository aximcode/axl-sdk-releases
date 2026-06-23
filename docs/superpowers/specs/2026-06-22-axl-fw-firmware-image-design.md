# Design: native firmware-image library (`AxlFw`) + `fwtool` — Phase 0 & 1

**Date:** 2026-06-22
**Status:** approved (brainstorm), pending implementation plan
**Supersedes naming in:** `docs/handoffs/2026-06-22-uefitool-port-firmware-library.md`
(that brief sketched `AxlFvImage`; see "Naming" below for why this became `AxlFw`)

## Summary

Port read-only UEFITool functionality into axl-sdk as a native firmware-image
parser library plus a `fwtool` CLI, replacing the residual `uefiextract` + Python
dependency in `find_shell_efi` and unlocking on-target firmware inspection.

This is a **multi-phase** effort (full-OEM read-only coverage is too large for one
spec). This document specs **Phase 0 + Phase 1** — the first shippable sub-project
— and records the agreed phase decomposition for the rest.

## Decisions (settled in brainstorming)

| # | Decision | Choice |
|---|---|---|
| 1 | Ambition | **Read-only inspect** (list / extract / find / report). No image rewriting. |
| 2 | Library vs tool | **Public library** `<axl/axl-fw.h>` + thin `fwtool` CLI (house pattern: every tool sits on a public header). |
| 3 | LZMA home | **`axl-compress`, encode + decode** (new `AXL_COMPRESS_LZMA`). Encoder is dead-stripped by `--gc-sections` from read-only images. |
| 4 | Runtime vs host | **Both.** UEFI app *and* host binary; host build replaces Python in `find_shell_efi`. |
| 5 | Format breadth (overall) | **Full OEM** eventually — but phased (see below). |
| 6 | First sub-project | **Phase 0 + Phase 1** together. |
| 7 | Module name | **`AxlFw` / `axl-fw.h`**, tool `fwtool`. |

### Naming

The handoff sketched `AxlFvImage`. Rejected because:
- `AxlFv` (`include/axl/axl-fv.h`) is already the **runtime** enumerator over
  `EFI_FIRMWARE_VOLUME2_PROTOCOL`; an `AxlFv*` byte-parser would collide and blur
  "live protocol" vs "offline bytes."
- Under full-OEM scope the **FV is not the root object**. A firmware image is:

  ```
  firmware image (SPI dump)
  └─ Intel flash descriptor → regions (BIOS / ME / GbE / EC)   ← Phase 3
     └─ firmware volume (FV)                                    ← Phase 1
        └─ FFS file
           └─ section → (PE32 | nested FV | NVRAM store | …)    ← Phase 4
  ```

  Naming the top-level parser after a child (`Fv`) reads wrong the moment regions
  arrive. The root noun is "firmware image."

Chosen: header `<axl/axl-fw.h>`; `AxlFw` prefix, tool `fwtool`. The conceptual
hierarchy (image → region → volume → file → section) is expressed as a
**single opaque `AxlFwNode` type discriminated by an `AxlFwNodeKind` enum**
(`IMAGE`/`REGION`/`VOLUME`/`FILE`/`SECTION`/`NVRAM`), not as distinct C structs
per level — a uniform node keeps the tree-walk API small and lets later phases add
kinds without new types. `AxlFwImage` is the one separate type (the owning root /
parse handle).

## Phase decomposition (full project)

Each phase ships independent value and gets its own spec → plan → test-first cycle,
validated by golden byte-match against `uefiextract` / `extract-fv-shell.py`.

- **Phase 0 — LZMA codec in `axl-compress`** *(gating)*. Decode + encode. Validate
  decode byte-matches Python `lzma` on an OVMF GUIDED-LZMA section. Nothing else
  builds without it.
- **Phase 1 — `AxlFw` core parser + `fwtool` (list/extract/find)** *(first
  deliverable)*. Raw `.fd` → FV → FFS → section tree, on-demand decompress (LZMA +
  COMPRESSION-none). UEFI app + host tool. Replaces Python in `find_shell_efi`.
- **Phase 2 — Tiano/EFI decompress + `report`**. `SECTION_COMPRESSION` type-1
  (BSD `BaseUefiDecompressLib` port) + VERSION / USER_INTERFACE / freeform section
  types → readable tree `report`. Works on most real-OEM FV content.
- **Phase 3 — Intel flash-descriptor regions** (BIOS / ME / GbE / EC). Whole-SPI
  entry point; hand the BIOS region to the Phase-1 parser.
- **Phase 4 — NVRAM variable stores** (VSS / VSS2 / FDC families).
- **Phase 5 — microcode / capsules / padding / remaining section types**.

**This spec covers Phase 0 + Phase 1 only.** Phases 2–5 are recorded for context;
they are not designed here.

---

## Phase 0 — LZMA in `axl-compress`

### API surface

No new functions. One enum value:

```c
typedef enum {
    AXL_COMPRESS_GZIP        = 0,
    AXL_COMPRESS_ZLIB        = 1,
    AXL_COMPRESS_DEFLATE_RAW = 2,
    AXL_COMPRESS_LZMA        = 3   /**< LZMA "alone" stream (.lzma): 1-byte props
                                        + 4-byte dict size + 8-byte uncompressed
                                        size + data. Matches EDK2 GUIDED-LZMA
                                        (EE4E5898-…) and Python FORMAT_ALONE. */
} AxlCompressFormat;
```

- `axl_decompress(AXL_COMPRESS_LZMA, in, in_len, out, out_len)` — parse the 13-byte
  alone header, drive the SDK decoder, allocate exact output (uncompressed-size
  from the header, capped at `AXL_COMPRESS_MAX_OUTPUT`).
- `axl_compress(AXL_COMPRESS_LZMA, in, in_len, out, out_len, level)` — emit an
  alone stream. `level` maps onto LZMA SDK preset levels (clamped).
- Stream filters (`axl_compress_reader`/`writer`) gain LZMA for free via the same
  `fmt` dispatch — no extra work, but **not a Phase-0 deliverable to test
  exhaustively**; cover one-shot first.

### Vendoring

Vendor the **public-domain LZMA SDK** (Igor Pavlov / 7-Zip) into `deps/lzma/`:
`LzmaDec.c/.h`, `LzmaEnc.c/.h`, and the minimal `7zTypes.h` shim they need.
Public-domain license; canonical source EDK2 and UEFITool both derive from.

A thin AXL wrapper (`src/data/axl-compress-lzma.c` or folded into
`axl-compress.c`) parses/emits the alone header and bridges SDK allocation to
`axl_malloc`/`axl_free` via an `ISzAlloc` adapter. No SIMD in LZMA → identical on
x64 / aa64 freestanding. Decoder needs a probability model + dictionary window
(dict size from the header) — allocated through the adapter, freed on completion.

**Open implementation detail for the plan:** the SDK files use some hosted-isms
(`memcpy`, `size_t`); confirm they compile freestanding under the project's CFLAGS,
shimming as the existing `mem_cpy`/`mem_set` helpers do where needed.

### Tests (gating — must pass before Phase 1)

`test/unit/axl-test-compress.c` (existing) gains:
- **Golden decode:** the LZMA-alone payload of the OVMF and AAVMF GUIDED-LZMA DXE
  section decodes to bytes that `axl_strcmp`/`axl_memcmp`-match the Python `lzma`
  output. (Fixture: a small captured `.lzma` blob embedded in the test, byte-match
  against its known plaintext — avoids shipping a whole `.fd` into the unit test.)
- **Round-trip:** `axl_compress(LZMA)` → `axl_decompress(LZMA)` returns the input
  for several sizes incl. empty and > window.
- **Negative:** truncated header, bogus props byte, forged oversize → `AXL_ERR`,
  no over-read (run under the existing 8-byte-pad discipline if applicable).

---

## Phase 1 — `AxlFw` library + `fwtool`

### Parsing model: materialized tree (decided)

`axl_fw_open` parses the whole image into a node tree, **eagerly decompressing**
encapsulation sections. Rationale: decompression yields *new* buffers and sections
live *inside* them; a borrowed-cursor model cannot cleanly own those. The image
owns every decompressed buffer; the input `data` is **borrowed** and must outlive
the image. `axl_fw_close` frees the tree and all owned buffers.

### Header contract (`include/axl/axl-fw.h`) — sketch

```c
/* Opaque parsed image + node. */
typedef struct AxlFwImage AxlFwImage;
typedef struct AxlFwNode  AxlFwNode;

/* Node kind. REGION/NVRAM are reserved now for forward-compat (Phase 3/4)
   even though Phase 1 emits only VOLUME/FILE/SECTION. */
typedef enum {
    AXL_FW_NODE_IMAGE   = 0,  ///< root
    AXL_FW_NODE_REGION  = 1,  ///< Intel descriptor region (Phase 3)
    AXL_FW_NODE_VOLUME  = 2,  ///< firmware volume (FV)
    AXL_FW_NODE_FILE    = 3,  ///< FFS file
    AXL_FW_NODE_SECTION = 4,  ///< leaf/encapsulation section
    AXL_FW_NODE_NVRAM   = 5   ///< NVRAM variable store (Phase 4)
} AxlFwNodeKind;

/* Parse `data` (len bytes) into a tree. `data` is BORROWED — it must
   outlive the returned image. Returns NULL if no firmware volume is found
   or on allocation failure. */
AxlFwImage *axl_fw_open(const void *data, size_t len);
void        axl_fw_close(AxlFwImage *img);

/* Tree walk (cursor-style children, like the FV iterators). */
AxlFwNode  *axl_fw_root(AxlFwImage *img);
AxlFwNode  *axl_fw_node_first_child(AxlFwNode *node);
AxlFwNode  *axl_fw_node_next_sibling(AxlFwNode *node);

/* Accessors. */
AxlFwNodeKind axl_fw_node_kind(AxlFwNode *node);
/* For FILE: FFS file type (EFI_FV_FILETYPE_*). For SECTION: section type
   (EFI_SECTION_*). 0 for container kinds. */
int           axl_fw_node_type(AxlFwNode *node);
/* GUID for FILE (FFS name) and GUID_DEFINED SECTION (codec); false if the
   node has no GUID. */
bool          axl_fw_node_guid(AxlFwNode *node, AxlGuid *out);
/* Body bytes of the node (for a PE32 section, the PE image itself). The
   pointer is owned by `img` and valid until axl_fw_close. */
bool          axl_fw_node_data(AxlFwNode *node, const void **ptr, size_t *len);

/* Convenience: first node whose GUID matches, anywhere in the tree
   (depth-first). Restrict by kind, or AXL_FW_NODE_IMAGE to match any kind.
   Returns NULL if not found. */
AxlFwNode  *axl_fw_find(AxlFwImage *img, const AxlGuid *guid, AxlFwNodeKind kind);
```

(Exact docstrings written contract-first; this is the shape, reviewed before impl.)

Internals mirror the proven `extract-fv-shell.py` structure: `iter_top_level_fvs`,
`iter_ffs_files`, `iter_sections`, recursive encapsulation handling — but
materialized into nodes, with GUID_DEFINED-LZMA and COMPRESSION-none decode in
Phase 1 (Tiano deferred to Phase 2; a node over an undecodable encapsulation is
kept as an opaque SECTION leaf rather than dropped).

### `fwtool` CLI (`tools/fwtool.c`)

Output styled like `dmidecode` / `lspci` (plain, stable, exact-string-testable):

- `fwtool list <image>` — indented tree: volume / file (GUID + type) / section
  (type) with sizes.
- `fwtool extract <image> <guid> [-o out]` — write the matched node body (PE32 /
  section body) to `out` (default stdout/file). The Shell-extraction use case.
- `fwtool find <image> <guid|hexpattern>` — report offsets / node paths where the
  GUID or hex pattern occurs.

`report` is Phase 2. All three are thin walks over the tree + `axl_fw_find`.

### Host build + `find_shell_efi` integration

The parser (`src/fw/axl-fw.c`) and the LZMA core are **backend-free**: freestanding
C plus `axl_malloc`/`axl_free`/`axl_memcpy`/`axl_memset` only (no UEFI protocols,
no console). Two builds share the source:

- **UEFI:** `$(PREFIX)/tools/fwtool.efi` via the existing tool rule, links
  `libaxl.a`.
- **Host:** `fwtool` host binary via `$(HOSTCC)` (the `pe-set-debug` precedent),
  unity-compiling `axl-fw.c` + the LZMA core + `fwtool.c` with a small host-shim
  header mapping `axl_malloc→malloc`, `axl_free→free`, `axl_memcpy→memcpy`,
  `axl_memset→memset`. Decision for the plan: shim header vs a tiny host static
  lib — prefer the shim header (one file, no new archive).

`find_shell_efi` (`scripts/axl-common.sh`) tier change:
- new **4a′**: if a host `fwtool` is available, `fwtool extract <fw> <shell-guid>`;
- demote Python to **4a″**, `uefiextract` to **4b**.

Shed python3 from the path once the C tool covers the same firmware (it covers
exactly what the Python tool did — same format subset).

### Tests

- `test/unit/axl-test-fw.c` (new, ratcheted, runs in QEMU): parse the embedded
  OVMF/AAVMF DXE `.fd` fixture, extract the Shell PE32, byte-match known-good
  bytes (the method that proved `extract-fv-shell.py`). Walk assertions: volume
  count, a known file GUID present, section types, `axl_fw_find` hit/miss.
- Host golden: the host `fwtool extract` emits bytes identical to
  `extract-fv-shell.py` for the same `.fd` (wire into the integration suite or a
  standalone check).
- Negative: truncated FV, bad section size, missing GUID → clean `NULL`/`AXL_ERR`,
  no over-read past `len`.

### Docs (same-change, per CLAUDE.md)

- `///<` param + `@brief`/`@return` on every `axl-fw.h` declaration.
- New `src/fw/README.md`; `docs/sphinx/modules/fw.rst` with `.. doxygenfile::
  axl-fw.h`; add to `index.rst`. `make check-docs` must pass.
- `axl-compress` README note: LZMA now supported (decode + encode).
- CLAUDE.md module table: add the `AxlFw` row.

## Out of scope (Phase 0 + 1)

- Image rewriting / UEFIReplace (the whole project is read-only).
- Tiano/EFI decompress, `report` (Phase 2).
- Intel descriptor regions, NVRAM, microcode, capsules (Phases 3–5).
- LZMA stream-filter exhaustive coverage (one-shot first; filters ride the dispatch
  but aren't a Phase-0 test gate).

## Risks / watch-items

- **LZMA SDK freestanding build** is the main unknown; prototype the decode
  byte-match (Phase 0) before anything depends on it.
- **Host/target source sharing** must stay backend-free — any `axl_*` call beyond
  the four mem primitives breaks the host unity build. Enforce by actually building
  the host tool in CI-or-make, not just the `.efi`.
- **Forward-compat enum** (`REGION`/`NVRAM` reserved) so later phases don't break
  the Phase-1 ABI.
```
