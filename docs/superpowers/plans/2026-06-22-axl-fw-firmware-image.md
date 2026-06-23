# AxlFw Firmware-Image Library + fwtool — Implementation Plan (Phase 0 + 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a native LZMA codec in `axl-compress` (Phase 0) and a read-only firmware-image parser library `AxlFw` + `fwtool` CLI (Phase 1) that lists/extracts/finds FV→FFS→section content, replacing the `uefiextract`+Python dependency in `find_shell_efi`.

**Architecture:** Port the proven recursive walker in `scripts/extract-fv-shell.py` into C, materializing the FV/FFS/section tree into an owned `AxlFwNode` graph (`axl_fw_open`/`axl_fw_close`). Decompression of GUIDED-LZMA sections runs through a new `AXL_COMPRESS_LZMA` codec backed by the vendored public-domain LZMA SDK. The parser and codec are backend-free so the same sources build both a `fwtool.efi` (links `libaxl.a`) and a host `fwtool` binary (`$(HOSTCC)` + a `malloc→axl_malloc` shim).

**Tech Stack:** C (freestanding + hosted), public-domain LZMA SDK (`deps/lzma/`), existing AxlCompress / AxlDigest / AxlMem, GNU Make, QEMU test harness.

**Spec:** `docs/superpowers/specs/2026-06-22-axl-fw-firmware-image-design.md`
**Reference port source:** `scripts/extract-fv-shell.py` (FV/FFS/section/LZMA structure to port — cite line numbers as noted).

## Global Constraints

- Coding style: `axl_snake_case` funcs, `AxlPascalCase` types, `AXL_SCREAMING_CASE` macros; 4-space indent, K&R braces, no space before parens. Multi-line function signatures even for single param. (`docs/AXL-Coding-Style.md`)
- Public API uses **standard C types only** — no UEFI types leak through `axl-fw.h`. Snake_case, UTF-8.
- Doc comments: `///<` inline param docs, `@brief`/`@return` in the block comment. Every public decl documented.
- No external runtime tools; vendored code only. LZMA SDK is **public domain** — keep its license/notice in `deps/lzma/`.
- Exact-string assertions for any text output (`axl_strcmp(buf,"...")==0`), exact-byte (`axl_memcmp`) for binary. Never `axl_strstr` for assertions.
- Confirm RED before implementing. Refactor while green. Independent review before commit (contract-first for the new header).
- Ratchet: new unit tests raise the baseline count; never let it drop. Keep SKIP counts balanced across arches.
- Build both arches: `make` (X64) and `make ARCH=aa64`. Tests run under `./test/integration/test-axl.sh [--arch X64|AARCH64]`.
- LZMA `axl_compress` ships encode+decode; `--gc-sections` strips the encoder from images that never call it (do not gate the encoder behind a macro).
- Parser + LZMA core stay **backend-free**: may call only `axl_malloc`/`axl_calloc`/`axl_free`/`axl_memcpy`/`axl_memset` (plus `axl_memcmp`/`axl_guid_cmp` in the parser). No console, no UEFI protocols — enforced by the host build actually compiling them.

---

## Phase 0 — LZMA codec in `axl-compress`

### Task 0.1: Vendor LZMA SDK + add `AXL_COMPRESS_LZMA` enum + stubs (infra)

**Files:**
- Create: `deps/lzma/LzmaDec.c`, `deps/lzma/LzmaDec.h`, `deps/lzma/LzmaEnc.c`, `deps/lzma/LzmaEnc.h`, `deps/lzma/7zTypes.h`, `deps/lzma/LICENSE` (vendored, unmodified)
- Create: `src/data/axl-compress-lzma.c` (AXL wrapper: alone-header parse/emit + `ISzAlloc` adapter)
- Modify: `include/axl/axl-compress.h` (add enum value + doc)
- Modify: `Makefile` (add `src/data/axl-compress-lzma.c` and the LZMA SDK `.c` files to `LIB_SOURCES` near line 518; ensure `deps/lzma` is on `INCLUDES`)

**Interfaces:**
- Produces: `AXL_COMPRESS_LZMA = 3` (new `AxlCompressFormat` value); `axl_decompress`/`axl_compress` accept it (stubbed to `AXL_ERR` in this task).
- Consumes: existing `axl_malloc`/`axl_free`.

This is infra (CLAUDE.md bucket E): land the vendored code compiling freestanding before any behavior test.

- [ ] **Step 1: Vendor the SDK.** Download the LZMA SDK (Igor Pavlov, public domain; e.g. the `lzma2407` 7-Zip SDK). Copy only `C/LzmaDec.{c,h}`, `C/LzmaEnc.{c,h}`, `C/7zTypes.h`, and the license text into `deps/lzma/`. Do not edit the SDK sources except as Step 3 requires.

- [ ] **Step 2: Add the enum value.** In `include/axl/axl-compress.h`, extend `AxlCompressFormat`:

```c
typedef enum {
    AXL_COMPRESS_GZIP        = 0,  /**< gzip (RFC 1952): magic + CRC-32 + size */
    AXL_COMPRESS_ZLIB        = 1,  /**< zlib (RFC 1950): 2-byte header + Adler-32 */
    AXL_COMPRESS_DEFLATE_RAW = 2,  /**< bare DEFLATE (RFC 1951): no header/trailer */
    AXL_COMPRESS_LZMA        = 3   /**< LZMA "alone" (.lzma): 1-byte props + 4-byte
                                        dict size + 8-byte uncompressed size + data.
                                        Matches EDK2 GUIDED-LZMA (EE4E5898-…) and
                                        Python lzma FORMAT_ALONE. Encode + decode. */
} AxlCompressFormat;
```

Update the header's "Format coverage" block comment to list LZMA.

- [ ] **Step 3: Wrapper skeleton + alloc adapter.** Create `src/data/axl-compress-lzma.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-compress-lzma.c
    LZMA "alone" (.lzma) one-shot encode/decode for AxlCompress, backed by
    the vendored public-domain LZMA SDK (deps/lzma). The 13-byte alone
    header (1 props + 4 dict-size + 8 uncompressed-size) is what EDK2's
    GUIDED-LZMA section and Python's FORMAT_ALONE use. */

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-compress.h>
#include <axl/axl-mem.h>
#include "../../deps/lzma/LzmaDec.h"
#include "../../deps/lzma/LzmaEnc.h"

/* Bridge the SDK's ISzAlloc to axl_malloc/axl_free. */
static void *lzma_alloc(ISzAllocPtr p, size_t size) { (void)p; return axl_malloc(size); }
static void  lzma_free (ISzAllocPtr p, void *addr)  { (void)p; axl_free(addr); }
static const ISzAlloc g_lzma_alloc = { lzma_alloc, lzma_free };

int
axl_lzma_decompress(const void *in, size_t in_len, void **out, size_t *out_len);  /* impl in Task 0.2 */
int
axl_lzma_compress(const void *in, size_t in_len, void **out, size_t *out_len, int level);  /* impl in Task 0.3 */
```

In `src/data/axl-compress.c`, route the new format. In `axl_decompress`'s `switch`, add `case AXL_COMPRESS_LZMA: return AXL_ERR;` for now; same in `axl_compress`. (Real routing wired in 0.2/0.3.) Declare `axl_lzma_*` in a small internal header `src/data/axl-compress-internal.h` if cleaner, or `extern` at top of `axl-compress.c`.

- [ ] **Step 4: Makefile wiring.** Add the four SDK `.c` files plus `src/data/axl-compress-lzma.c` to `LIB_SOURCES`. Add `-Ideps/lzma` to `INCLUDES`. If the SDK files need freestanding shims (no `<string.h>`), define `memcpy`/`memset` via the project's existing helpers or add `-D` mappings — confirm by building.

- [ ] **Step 5: Build both arches.**

Run: `make clean && make && make ARCH=aa64`
Expected: PASS — `libaxl.a` builds for both arches with the SDK compiled in. Fix any freestanding-build issues (missing libc symbols, `size_t` includes) now.

- [ ] **Step 6: Commit.**

```bash
git add deps/lzma include/axl/axl-compress.h src/data/axl-compress-lzma.c src/data/axl-compress.c Makefile
git commit -m "compress: vendor public-domain LZMA SDK + AXL_COMPRESS_LZMA enum (stub)"
```

---

### Task 0.2: LZMA decode (golden byte-match)

**Files:**
- Modify: `src/data/axl-compress-lzma.c` (implement `axl_lzma_decompress`)
- Modify: `src/data/axl-compress.c` (`axl_decompress` LZMA case → `axl_lzma_decompress`)
- Test: `test/unit/axl-test-data.c` (add to the compress test section near line 4498)

**Interfaces:**
- Produces: `axl_decompress(AXL_COMPRESS_LZMA, in, in_len, &out, &out_len)` decodes an alone stream; `out` is `axl_free`-owned, exact uncompressed size from the header.
- Consumes: SDK `LzmaDecode`, `g_lzma_alloc`.

- [ ] **Step 1: Generate the golden fixture.** Run this to emit a C byte array of a FORMAT_ALONE stream of a known plaintext:

```bash
python3 - <<'PY'
import lzma
pt = b"AxlFw LZMA golden fixture: the quick brown fox 0123456789." * 4
blob = lzma.compress(pt, format=lzma.FORMAT_ALONE)
print("static const unsigned char lzma_golden[] = {")
print("    " + ", ".join(f"0x{b:02x}" for b in blob) + " };")
print(f"/* decodes to {len(pt)} bytes */")
PY
```

Paste the emitted array into the new test (Step 2) and record the plaintext string literal as the expected result.

- [ ] **Step 2: Write the failing test.** Add to `test/unit/axl-test-data.c`:

```c
static void
test_lzma_decode_golden(void)
{
    static const unsigned char lzma_golden[] = { /* ...paste from Step 1... */ };
    static const char expected[] =
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789."
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789."
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789."
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789.";
    void  *out = NULL;
    size_t out_len = 0;
    int rc = axl_decompress(AXL_COMPRESS_LZMA, lzma_golden, sizeof(lzma_golden),
                            &out, &out_len);
    test_check(rc == AXL_OK, "lzma decode: returns AXL_OK");
    test_check(out_len == sizeof(expected) - 1, "lzma decode: exact length");
    test_check(out != NULL && axl_memcmp(out, expected, sizeof(expected) - 1) == 0,
               "lzma decode: byte-exact plaintext");
    axl_free(out);
}
```

Register the call in the compress test runner (alongside `test_compress_roundtrip`).

- [ ] **Step 3: Confirm RED.**

Run: `make tests && TEST_APPS_ONLY=AxlTestData ./test/integration/test-axl.sh`
Expected: FAIL — `lzma decode: returns AXL_OK` fails (case still returns `AXL_ERR`).

- [ ] **Step 4: Implement `axl_lzma_decompress`.** In `src/data/axl-compress-lzma.c`: parse the 13-byte alone header — byte 0..4 = `LzmaProps` (5 bytes: props + 4-byte dict size), bytes 5..12 = little-endian uncompressed size. Reject `in_len < 13`, reject uncompressed size `> AXL_COMPRESS_MAX_OUTPUT` (reuse the cap; expose it from `axl-compress.c` or redefine). Allocate exact output, call `LzmaDecode(out, &outProcessed, payload, &srcLen, props5, LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &g_lzma_alloc)`. Map SZ_OK + full output to `AXL_OK`; anything else frees and returns `AXL_ERR`. Handle the unknown-size sentinel (`0xFFFFFFFFFFFFFFFF`) by streaming-grow like `inflate_grow` — but EDK2/OVMF always writes a real size, so a first cut may `AXL_ERR` on the sentinel; document that.
Wire `axl_decompress`'s `AXL_COMPRESS_LZMA` case to call it.

- [ ] **Step 5: Confirm GREEN.**

Run: `make tests && TEST_APPS_ONLY=AxlTestData ./test/integration/test-axl.sh`
Expected: PASS (all three checks).

- [ ] **Step 6: Refactor while green.** Factor the header-parse into a static helper; ensure the output cap and over-read safety mirror the DEFLATE path's discipline. Re-run tests.

- [ ] **Step 7: Commit.**

```bash
git add src/data/axl-compress-lzma.c src/data/axl-compress.c test/unit/axl-test-data.c
git commit -m "compress: implement LZMA-alone decode (golden byte-match)"
```

---

### Task 0.3: LZMA encode (round-trip)

**Files:**
- Modify: `src/data/axl-compress-lzma.c` (implement `axl_lzma_compress`)
- Modify: `src/data/axl-compress.c` (`axl_compress` LZMA case)
- Test: `test/unit/axl-test-data.c`

**Interfaces:**
- Produces: `axl_compress(AXL_COMPRESS_LZMA, in, in_len, &out, &out_len, level)` emits an alone stream that `axl_decompress(AXL_COMPRESS_LZMA, …)` round-trips.

- [ ] **Step 1: Write the failing test.** Add `test_lzma_roundtrip()` to `axl-test-data.c`: for inputs `{"", "a", a 64 KiB pattern}`, `axl_compress(AXL_COMPRESS_LZMA, in, len, &c, &cl, AXL_COMPRESS_LEVEL_DEFAULT)` then `axl_decompress` and assert `axl_memcmp` equals the input and lengths match. Register it.

- [ ] **Step 2: Confirm RED.**

Run: `make tests && TEST_APPS_ONLY=AxlTestData ./test/integration/test-axl.sh`
Expected: FAIL — encode returns `AXL_ERR`.

- [ ] **Step 3: Implement `axl_lzma_compress`.** Use `LzmaEncode` (or `LzmaEnc_*` handle API): build a `CLzmaEncProps`, `LzmaEncProps_Init`, set `level` (map `AXL_COMPRESS_LEVEL_DEFAULT`→5, clamp 0..9), `LzmaEncProps_Normalize`. Emit the 13-byte alone header: 5 props bytes via `LzmaEnc_WriteProperties`, then 8-byte little-endian uncompressed size, then the encoded payload. Allocate a worst-case output (`in_len + in_len/2 + 256`) and shrink. Return `AXL_OK`/`AXL_ERR`. Wire `axl_compress`'s LZMA case.

- [ ] **Step 4: Confirm GREEN.**

Run: `make tests && TEST_APPS_ONLY=AxlTestData ./test/integration/test-axl.sh`
Expected: PASS.

- [ ] **Step 5: Cross-check against Python.** Sanity (not a committed test): confirm `axl_decompress` reads a Python-`FORMAT_ALONE` blob (already covered by 0.2) and that the Step-1 golden still passes — proves encoder and decoder agree with liblzma's framing.

- [ ] **Step 6: Commit.**

```bash
git add src/data/axl-compress-lzma.c src/data/axl-compress.c test/unit/axl-test-data.c
git commit -m "compress: implement LZMA-alone encode (round-trip)"
```

---

### Task 0.4: LZMA negative/guard tests

**Files:**
- Modify: `src/data/axl-compress-lzma.c` (guards)
- Test: `test/unit/axl-test-data.c`

- [ ] **Step 1: Write failing tests.** `test_lzma_negative()`: assert `AXL_ERR` (and `out==NULL`, `out_len==0`) for: `in_len < 13`; a header whose props byte is invalid (`> 224`); a forged uncompressed size `> AXL_COMPRESS_MAX_OUTPUT`; a truncated payload (header says N bytes, body cut short). Use a copy of the golden blob with mutated bytes.

- [ ] **Step 2: Confirm RED** (if any guard missing).

Run: `make tests && TEST_APPS_ONLY=AxlTestData ./test/integration/test-axl.sh`
Expected: FAIL on the missing-guard case(s).

- [ ] **Step 3: Add the guards** in `axl_lzma_decompress` (length, props validity via `LzmaProps_Decode`, output cap, decode-status check). Ensure no read past `in + in_len`.

- [ ] **Step 4: Confirm GREEN.** Run the same command — PASS.

- [ ] **Step 5: Docs + commit.** Add a line to `src/data/README.md` (or the compress README) noting LZMA decode+encode support. Run `make check-docs`.

```bash
git add src/data/axl-compress-lzma.c test/unit/axl-test-data.c src/data/README.md
git commit -m "compress: LZMA decode guards + negative tests; doc note"
```

**Phase 0 gate:** LZMA decode is proven byte-exact against liblzma. Phase 1 may now depend on it.

---

## Phase 1 — `AxlFw` library + `fwtool`

### Task 1.1: `axl-fw.h` contract (header-first) + linking stub

**Files:**
- Create: `include/axl/axl-fw.h`
- Create: `src/fw/axl-fw.c` (stubs returning NULL/0 so the lib links)
- Modify: `include/axl.h` (add `#include <axl/axl-fw.h>`)
- Modify: `Makefile` (`src/fw/axl-fw.c` → `LIB_SOURCES`)

**Interfaces:**
- Produces: the full `AxlFw` public API (types + signatures below). Later tasks implement it.

- [ ] **Step 1: Write the header.** Create `include/axl/axl-fw.h` with full docstrings (`@brief`/`@return`, `///<`). Exact contract:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-fw.h
    Read-only parser for raw UEFI firmware images (.fd / SPI dumps).

    Parses an image into an owned tree of nodes — firmware volumes, FFS
    files, and sections — decompressing GUIDED-LZMA and uncompressed
    COMPRESSION sections on the way. Unlike <axl/axl-fv.h> (which
    enumerates LIVE volumes over EFI_FIRMWARE_VOLUME2_PROTOCOL), this
    works on raw bytes, on host or target. */
#ifndef AXL_FW_H
#define AXL_FW_H
#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>   /* AxlGuid */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlFwImage AxlFwImage;
typedef struct AxlFwNode  AxlFwNode;

/** @brief Kind of a firmware-tree node. REGION/NVRAM are reserved for
    later phases; Phase 1 emits only IMAGE/VOLUME/FILE/SECTION. */
typedef enum {
    AXL_FW_NODE_IMAGE   = 0,  ///< parse root
    AXL_FW_NODE_REGION  = 1,  ///< Intel descriptor region (reserved)
    AXL_FW_NODE_VOLUME  = 2,  ///< firmware volume (FV)
    AXL_FW_NODE_FILE    = 3,  ///< FFS file
    AXL_FW_NODE_SECTION = 4,  ///< section (leaf or encapsulation)
    AXL_FW_NODE_NVRAM   = 5   ///< NVRAM variable store (reserved)
} AxlFwNodeKind;

/** @brief Parse @p data (@p len bytes) into an owned firmware tree.
    @p data is BORROWED and must outlive the returned image (uncompressed
    node bodies point into it; decompressed bodies are owned by the image).
    @return a parsed image (free with axl_fw_close), or NULL if no firmware
        volume is found or on allocation failure. */
AxlFwImage *
axl_fw_open(
    const void *data,   ///< firmware image bytes (borrowed)
    size_t      len     ///< image length
);

/** @brief Free a parsed image and every buffer it owns. NULL-safe. */
void
axl_fw_close(
    AxlFwImage *img   ///< image from axl_fw_open (may be NULL)
);

/** @brief The root (AXL_FW_NODE_IMAGE) node, or NULL if @p img is NULL. */
AxlFwNode *
axl_fw_root(
    AxlFwImage *img   ///< parsed image
);

/** @brief First child of @p node, or NULL if it has none. */
AxlFwNode *
axl_fw_node_first_child(
    AxlFwNode *node   ///< any node
);

/** @brief Next sibling of @p node, or NULL at the end of the sibling list. */
AxlFwNode *
axl_fw_node_next_sibling(
    AxlFwNode *node   ///< any node
);

/** @brief Kind of @p node. */
AxlFwNodeKind
axl_fw_node_kind(
    AxlFwNode *node   ///< any node
);

/** @brief Type byte of @p node: FFS file type for FILE, section type for
    SECTION, 0 for container kinds. */
int
axl_fw_node_type(
    AxlFwNode *node   ///< any node
);

/** @brief GUID of @p node: FFS name for FILE, codec GUID for a
    GUID_DEFINED SECTION. @return true and fills @p out if the node has a
    GUID; false otherwise. */
bool
axl_fw_node_guid(
    AxlFwNode *node,   ///< any node
    AxlGuid   *out     ///< [out] GUID, set on true
);

/** @brief Body bytes of @p node (for a PE32 section, the PE image). The
    pointer is owned by @p img's tree and valid until axl_fw_close.
    @return true and sets @p ptr/@p len when the node has a body; false
        otherwise. */
bool
axl_fw_node_data(
    AxlFwNode   *node,   ///< any node
    const void **ptr,    ///< [out] body pointer
    size_t      *len     ///< [out] body length
);

/** @brief Depth-first search for the first node whose GUID equals @p guid.
    Restrict to a kind, or pass AXL_FW_NODE_IMAGE to match any kind.
    @return the matching node, or NULL if none. */
AxlFwNode *
axl_fw_find(
    AxlFwImage    *img,    ///< parsed image
    const AxlGuid *guid,   ///< GUID to match
    AxlFwNodeKind  kind     ///< kind filter, or AXL_FW_NODE_IMAGE for any
);

#ifdef __cplusplus
}
#endif
#endif /* AXL_FW_H */
```

- [ ] **Step 2: Stub the impl.** Create `src/fw/axl-fw.c` with `#include <axl/axl-fw.h>` and stubs: `axl_fw_open`→`return NULL;`, `axl_fw_close`→`(void)img;`, every accessor→`return NULL/0/false;`. Add to `LIB_SOURCES`. Add the include to `include/axl.h`.

- [ ] **Step 3: Build both arches.**

Run: `make && make ARCH=aa64`
Expected: PASS — links with stubs.

- [ ] **Step 4: Contract-first review.** Per CLAUDE.md `feedback_code_review_before_commit`: independent review of *just the header* — node model, ownership (borrowed `data`, owned decompressed buffers), `find` semantics, forward-compat enum, docstring completeness. Apply fixes. Run `make check-docs` (the doxygenfile directive is added in Task 1.8 — if check-docs fails for the new header now, add the directive in this task instead and note it).

- [ ] **Step 5: Commit.**

```bash
git add include/axl/axl-fw.h src/fw/axl-fw.c include/axl.h Makefile
git commit -m "fw: AxlFw public header contract + linking stub"
```

---

### Task 1.2: Parse uncompressed FV → file → section tree

**Files:**
- Modify: `src/fw/axl-fw.c` (real `axl_fw_open`/`close` + accessors + node arena)
- Test: `test/unit/axl-test-fw.c` (new)
- Modify: `Makefile` (`BUILD_TEST(AxlTestFw,axl-test-fw)`; add `AxlTestFw` to `TESTS` near line 1564)

**Interfaces:**
- Consumes: parse helpers ported from `extract-fv-shell.py` (`_iter_top_level_fvs` L155-167, `_iter_ffs_files` L66-95, `_iter_sections` L98-113).
- Produces: a working tree for uncompressed input; `axl_fw_node_*` accessors return real data.

- [ ] **Step 1: Build the synthetic fixture + failing test.** Create `test/unit/axl-test-fw.c`. Hand-construct a minimal FV in a `static unsigned char[]` (or a small builder): an `EFI_FIRMWARE_VOLUME_HEADER` (`_FVH` at offset 40, `FvLength` at 32, `HeaderLength` at 48, `ExtHeaderOffset` 0), one FFS file (16-byte GUID name, type `0x07` DRIVER at +18, attrib 0 at +19, 24-bit size at +20, header 24 bytes), containing one PE32 section (`0x10`, 4-byte header, body = `"MZ\x90\x00testbody"`). Document the exact byte layout in comments. Test:

```c
static void
test_fw_uncompressed_tree(void)
{
    extern const unsigned char fixture_fv[];
    extern const unsigned int  fixture_fv_len;
    AxlFwImage *img = axl_fw_open(fixture_fv, fixture_fv_len);
    test_check(img != NULL, "fw: open synthetic FV");
    AxlFwNode *vol = axl_fw_node_first_child(axl_fw_root(img));
    test_check(vol != NULL && axl_fw_node_kind(vol) == AXL_FW_NODE_VOLUME,
               "fw: root has a volume child");
    AxlFwNode *file = axl_fw_node_first_child(vol);
    test_check(file != NULL && axl_fw_node_kind(file) == AXL_FW_NODE_FILE,
               "fw: volume has a file child");
    test_check(axl_fw_node_type(file) == 0x07, "fw: file type DRIVER");
    AxlFwNode *sec = axl_fw_node_first_child(file);
    test_check(sec != NULL && axl_fw_node_kind(sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(sec) == 0x10, "fw: file has PE32 section");
    const void *body = NULL; size_t blen = 0;
    test_check(axl_fw_node_data(sec, &body, &blen) && blen >= 2
               && axl_memcmp(body, "MZ", 2) == 0, "fw: section body is the PE32");
    axl_fw_close(img);
}
```

- [ ] **Step 2: Confirm RED.**

Run: `make tests && TEST_APPS_ONLY=AxlTestFw ./test/integration/test-axl.sh`
Expected: FAIL — `axl_fw_open` returns NULL (stub).

- [ ] **Step 3: Implement the parser.** In `src/fw/axl-fw.c`: define `struct AxlFwNode { AxlFwNodeKind kind; int type; bool has_guid; AxlGuid guid; const uint8_t *body; size_t body_len; AxlFwNode *first_child, *next_sibling; };` and `struct AxlFwImage { const uint8_t *data; size_t len; AxlFwNode *root; /* node arena */ AxlFwNode **owned_bufs; ... };`. Allocate nodes from an arena (or a simple linked list of `axl_malloc`'d nodes tracked for `axl_fw_close`). Port, materializing into nodes:
  - top-level FV scan (`_iter_top_level_fvs`) → VOLUME nodes under the IMAGE root;
  - FFS iteration (`_iter_ffs_files`, incl. LARGE_FILE attrib + 8-byte align + pad/erased skip) → FILE nodes;
  - section iteration (`_iter_sections`, 24-bit + 0xFFFFFF extended size) → SECTION nodes.
  Leaf sections (PE32/TE/etc.) store `body = section + body_off`. (Encapsulation handling in 1.3/1.4.) Implement all accessors against the struct. `axl_fw_close` frees every node and every owned decompressed buffer (none yet).

- [ ] **Step 4: Confirm GREEN.**

Run: `make tests && TEST_APPS_ONLY=AxlTestFw ./test/integration/test-axl.sh`
Expected: PASS (all checks).

- [ ] **Step 5: Refactor while green.** De-dup the 24-bit-size read (FFS + section share the pattern) into a helper. Re-run.

- [ ] **Step 6: Commit.**

```bash
git add src/fw/axl-fw.c test/unit/axl-test-fw.c Makefile
git commit -m "fw: parse uncompressed FV/FFS/section tree"
```

---

### Task 1.3: Recurse into COMPRESSION-none and nested-FV sections

**Files:**
- Modify: `src/fw/axl-fw.c`
- Test: `test/unit/axl-test-fw.c`

**Interfaces:**
- Consumes: `_search_sections` COMPRESSION (L133-139) + FIRMWARE_VOLUME_IMAGE (L140-143) handling.
- Produces: children under `SECTION_COMPRESSION` (type 0) and `SECTION_FIRMWARE_VOLUME_IMAGE` nodes.

- [ ] **Step 1: Failing test.** Extend the fixture: wrap the PE32 section inside a `SECTION_COMPRESSION` (`0x01`) with `CompressionType=0` (uncompressed; body at +9 is a raw section stream), and add a `SECTION_FIRMWARE_VOLUME_IMAGE` (`0x17`) whose body is a second minimal FV. Assert: the COMPRESSION node has a PE32 child; the FV-image node has a VOLUME child.

- [ ] **Step 2: Confirm RED.** `TEST_APPS_ONLY=AxlTestFw ./test/integration/test-axl.sh` → FAIL (no children built under encapsulation).

- [ ] **Step 3: Implement.** When a SECTION is `SECTION_COMPRESSION` with `comp_type==0`, recurse `parse_sections(body+9, ...)` and attach the results as children. When `SECTION_FIRMWARE_VOLUME_IMAGE`, recurse `parse_fv(body+body_off, ...)` as a VOLUME child. Mirror the Python recursion; cap recursion depth (e.g. 32) to defend against crafted cycles.

- [ ] **Step 4: Confirm GREEN.** Re-run — PASS.

- [ ] **Step 5: Commit.**

```bash
git add src/fw/axl-fw.c test/unit/axl-test-fw.c
git commit -m "fw: recurse COMPRESSION-none and nested-FV sections"
```

---

### Task 1.4: GUIDED-LZMA decode + `axl_fw_find`

**Files:**
- Modify: `src/fw/axl-fw.c`
- Test: `test/unit/axl-test-fw.c`

**Interfaces:**
- Consumes: `axl_decompress(AXL_COMPRESS_LZMA, …)` (Phase 0); `_search_sections` GUID_DEFINED branch (L121-132); `axl_guid_cmp`.
- Produces: children under GUIDED-LZMA sections (owned decompressed buffer); `axl_fw_find` working.

- [ ] **Step 1: Failing test (build the LZMA fixture at runtime).** In the test, take a minimal inner FV byte array, `axl_compress(AXL_COMPRESS_LZMA, inner_fv, len, &lz, &lzlen, DEFAULT)`, then assemble a GUID_DEFINED section: 4-byte section header (type `0x02`), 16-byte LZMA codec GUID (`EE4E5898-3914-4259-9D6E-DC7BD79403CF`, store as `bytes_le`), 2-byte `DataOffset`, 2-byte `Attributes=0x01` (PROCESSING_REQUIRED), then the `lz` payload. Wrap that section as the body of an FFS file in an outer FV. Assert: walking reaches the inner FV's file; and:

```c
AxlGuid want = /* the inner file's GUID */;
AxlFwNode *hit = axl_fw_find(img, &want, AXL_FW_NODE_FILE);
test_check(hit != NULL, "fw: find inner file by GUID across LZMA boundary");
```

Also assert `axl_fw_find` returns NULL for an absent GUID.

- [ ] **Step 2: Confirm RED.** Re-run — FAIL (GUIDED section not decoded; `axl_fw_find` stubbed/absent).

- [ ] **Step 3: Implement.** In the SECTION loop, on `SECTION_GUID_DEFINED` read codec GUID (`section+4`, 16 bytes), `DataOffset` (`+20`), `Attributes` (`+22`); `payload = section + DataOffset`. If codec == LZMA GUID, `axl_decompress(AXL_COMPRESS_LZMA, payload, sec_end-payload, &dec, &dec_len)`; on success **record `dec` in the image's owned-buffer list**, recurse `parse_sections(dec, dec_len, …)` as children. If decode fails and `!(Attributes & PROCESSING_REQUIRED)`, treat `payload` as a raw section stream (Python L127-128). If still undecodable, keep the GUIDED node as an opaque leaf (do not drop). Implement `axl_fw_find` as a DFS over the tree using `axl_guid_cmp`, honoring the `kind` filter (with `AXL_FW_NODE_IMAGE` meaning "any").

- [ ] **Step 4: Confirm GREEN.** Re-run — PASS. Confirm `axl_fw_close` frees the recorded `dec` buffers (run under the existing leak/ASAN discipline if present; at minimum, no double-free).

- [ ] **Step 5: Build both arches + full data suite.**

Run: `make tests && TEST_APPS_ONLY="AxlTestFw AxlTestData" ./test/integration/test-axl.sh` and `... --arch AARCH64`
Expected: PASS both.

- [ ] **Step 6: Commit.**

```bash
git add src/fw/axl-fw.c test/unit/axl-test-fw.c
git commit -m "fw: decode GUIDED-LZMA sections; axl_fw_find DFS"
```

---

### Task 1.5: `fwtool` CLI (UEFI app)

**Files:**
- Create: `tools/fwtool.c`
- Modify: `Makefile` (add `fwtool` to `TOOL_NAMES` line 1622)

**Interfaces:**
- Consumes: the full `AxlFw` API; house tool patterns from `tools/dmidecode.c` / `tools/lspci.c` (arg parsing, `axl_print`, file read).
- Produces: `fwtool list|extract|find` over a `.fd` path.

- [ ] **Step 1: Implement the tool.** `int main(int argc, char **argv)` dispatching on `argv[1]`:
  - `list <image>`: read file into a buffer (`axl_file_read_all` or the pattern used by `hexdump`/`tar`), `axl_fw_open`, DFS-print an indented tree: per node a line with kind, type, GUID (via `axl_guid_to_string`), and body size. Exact, stable formatting.
  - `extract <image> <guid> [-o out]`: `axl_fw_open` + `axl_fw_find(img, guid, AXL_FW_NODE_IMAGE)`; write `axl_fw_node_data` body to `-o` (or stdout). Parse the GUID arg with the existing GUID-from-string helper. Non-zero exit if not found.
  - `find <image> <guid|hex>`: walk and print the path/offset of each match.
  Print a usage line for unknown subcommands. Follow `tools/lspci.c` for option handling and exit codes.

- [ ] **Step 2: Build + smoke (both arches).**

Run: `make tools && make ARCH=aa64 tools`
Expected: `$(PREFIX)/tools/fwtool.efi` builds for both.

- [ ] **Step 3: Manual smoke in QEMU (optional but recommended).** Boot `fwtool list <ovmf-dxe.fd>` via the run-qemu harness against a staged firmware image and eyeball the tree. (Automated golden is Task 1.6.)

- [ ] **Step 4: Commit.**

```bash
git add tools/fwtool.c Makefile
git commit -m "fwtool: list/extract/find CLI over AxlFw (UEFI app)"
```

---

### Task 1.6: Host build of `fwtool` + golden byte-match vs Python

**Files:**
- Create: `tools/fwtool-host-shim.h` (`#define axl_malloc malloc`, `axl_calloc calloc`, `axl_free free`, `axl_memcpy memcpy`, `axl_memset memset`, `axl_memcmp memcmp`; declare `axl_guid_cmp`/etc. or include the real headers if hosted-safe)
- Create: `test/integration/test-fwtool-host-qemu.sh` (or a plain host script `test/integration/test-fwtool-host.sh` — no QEMU needed)
- Modify: `Makefile` (host `fwtool` rule via `$(HOSTCC)`)

**Interfaces:**
- Consumes: `src/fw/axl-fw.c`, `src/data/axl-compress*.c`, `deps/lzma/*.c`, `tools/fwtool.c`.
- Produces: a host `out/host/fwtool` binary; a test asserting it matches `scripts/extract-fv-shell.py`.

- [ ] **Step 1: Host build rule.** Add a Makefile target building a host `fwtool` with `$(HOSTCC) -Wall -O2`, unity-compiling the backend-free sources + `fwtool.c`, with `-include tools/fwtool-host-shim.h` and `-Iinclude -Ideps/lzma`. Resolve any UEFI-only references in `fwtool.c` (e.g. swap `axl_print` for `fputs`/`fwrite` under a `#ifndef AXL_HOSTED` split, or keep tool I/O in small host/target-conditional helpers). Output to `$(BUILDDIR)/fwtool-host` or `out/host/fwtool`.

```make
# Host fwtool: backend-free parser + LZMA, compiled with the host toolchain.
FWTOOL_HOST = $(BUILDDIR)/fwtool-host
FWTOOL_HOST_SRCS = tools/fwtool.c src/fw/axl-fw.c src/data/axl-compress.c \
    src/data/axl-compress-lzma.c $(LZMA_SDK_SRCS)
$(FWTOOL_HOST): $(FWTOOL_HOST_SRCS) tools/fwtool-host-shim.h | $(BUILDDIR)
	$(HOSTCC) -Wall -O2 -DAXL_HOSTED -include tools/fwtool-host-shim.h \
	    -Iinclude -Ideps/lzma -Ideps/sdefl -o $@ $(FWTOOL_HOST_SRCS)
```

- [ ] **Step 2: Write the golden test.** `test/integration/test-fwtool-host.sh`: for each available firmware `.fd` the project can locate (reuse `find_firmware` from `axl-common.sh`), run both `python3 scripts/extract-fv-shell.py <fd> -o /tmp/py.efi` and `$(FWTOOL_HOST) extract <fd> 7C04A583-9E3E-4F1C-AD65-E05268D0B4D1 -o /tmp/c.efi`, then `cmp /tmp/py.efi /tmp/c.efi`. Fail if they differ or either is missing. Mark `TEST_SKIP_RATCHET=1` (integration).

- [ ] **Step 3: Confirm it builds + passes.**

Run: `make $(FWTOOL_HOST) && ./test/integration/test-fwtool-host.sh`
Expected: PASS — C host tool emits byte-identical Shell to the Python extractor on X64 and AARCH64 firmware.

- [ ] **Step 4: Commit.**

```bash
git add tools/fwtool.c tools/fwtool-host-shim.h test/integration/test-fwtool-host.sh Makefile
git commit -m "fwtool: host build (HOSTCC) + golden byte-match vs extract-fv-shell.py"
```

---

### Task 1.7: Switch `find_shell_efi` to host `fwtool`

**Files:**
- Modify: `scripts/axl-common.sh` (`find_shell_efi`, the tier-4 block ~L375-451)

**Interfaces:**
- Consumes: host `fwtool` (Task 1.6); existing tier-4a (Python) / 4b (uefiextract).

- [ ] **Step 1: Add tier 4a′.** Before the Python block, if a host `fwtool` binary exists (locate it next to the build output / on `PATH`), loop `fw_candidates` calling `fwtool extract <fw> <shell-guid> -o "$fv_shell"` and accept on an `MZ` body. Demote Python to 4a″ and `uefiextract` to 4b (unchanged logic, just ordered after). Keep the existing `log_info`/`log_warning` messages.

- [ ] **Step 2: Verify the harness still stages a shell.**

Run: `./test/integration/test-axl.sh --arch X64` (uses `find_shell_efi`)
Expected: PASS — the suite boots a shell extracted by the C `fwtool` (check the log line names fwtool, not Python/uefiextract).

- [ ] **Step 3: Commit.**

```bash
git add scripts/axl-common.sh
git commit -m "run-qemu: prefer native fwtool for Shell extraction (tier 4a')"
```

---

### Task 1.8: Docs sync

**Files:**
- Create: `src/fw/README.md`
- Create: `docs/sphinx/modules/fw.rst`
- Modify: `docs/sphinx/index.rst` (add the fw module page)
- Modify: `CLAUDE.md` (add the `AxlFw` row to the module table)

- [ ] **Step 1: Module README + rst.** Write `src/fw/README.md` (what AxlFw is, the borrowed-`data`/owned-buffers ownership rule, the tree model, relation to runtime `AxlFv`). Create `docs/sphinx/modules/fw.rst` with a title, `.. include:: ../../../src/fw/README.md`, and `.. doxygenfile:: axl-fw.h`. Add `modules/fw` to `index.rst`.

- [ ] **Step 2: CLAUDE.md table row.** Add: `| AxlFw | src/fw/ | axl/axl-fw.h (raw .fd / SPI-image parser: FV/FFS/section tree + on-demand decompress; sibling to runtime AxlFv) |`.

- [ ] **Step 3: Doc gates.**

Run: `make check-docs && make check-ascii`
Expected: PASS — `axl-fw.h` has a doxygenfile directive; no non-ASCII.
(Optional: `./scripts/build-docs.sh` to confirm Sphinx renders without `\ref` warnings.)

- [ ] **Step 4: Commit.**

```bash
git add src/fw/README.md docs/sphinx/modules/fw.rst docs/sphinx/index.rst CLAUDE.md
git commit -m "docs: AxlFw module README + sphinx page + module-table row"
```

---

## Final integration gate (run before declaring Phase 0+1 done)

- [ ] **Full suite, both arches.**

Run: `./test/integration/test-axl.sh --arch X64` and `./test/integration/test-axl.sh --arch AARCH64`
Expected: PASS, ratchet count increased by the new `AxlTestFw` + LZMA tests, balanced SKIPs, 0 failures.

- [ ] **Integration host golden.**

Run: `./test/integration/test-fwtool-host.sh`
Expected: PASS on every locatable firmware image.

- [ ] **Pre-commit review.** Independent integration review per `feedback_code_review_before_commit` across the whole Phase 0+1 diff (LZMA codec, parser, tool, host build, find_shell_efi). Apply fixes.

---

## Self-review notes (author)

- **Spec coverage:** LZMA encode+decode (0.1-0.4) ✓; `AxlFw` materialized tree + ownership (1.1-1.4) ✓; single-node + kind enum incl. reserved REGION/NVRAM (1.1) ✓; `fwtool list/extract/find` UEFI + host (1.5-1.6) ✓; backend-free host build + shim (1.6) ✓; `find_shell_efi` tier 4a′/4a″/4b (1.7) ✓; golden byte-match vs python/uefiextract (1.6) ✓; docs (1.8) ✓. Deferred per spec: Tiano/report (Phase 2), regions/NVRAM/microcode (Phase 3-5), exhaustive LZMA stream-filter tests.
- **Forward-compat:** enum reserves REGION/NVRAM so Phase 3/4 add kinds without ABI churn.
- **Test design:** synthetic fixtures for unit (small, controlled, RED-confirmable); real-firmware byte-match at the host/integration layer (no multi-MB `.fd` embedded in a unit EFI).
- **Open items handed to the implementer (flagged in-task, not placeholders):** SDK freestanding shims (0.1 Step 4); unknown-size LZMA sentinel handling (0.2 Step 4); host I/O split in `fwtool.c` (1.6 Step 1).
