Read-only parser for raw UEFI firmware images (`.fd` files and SPI
flash dumps).

Header: `<axl/axl-fw.h>`. Where `<axl/axl-fv.h>` (AxlFv) enumerates the
*live* firmware volumes a running platform publishes over
`EFI_FIRMWARE_VOLUME2_PROTOCOL`, AxlFw parses *raw bytes* — a firmware
image you read from a file or from SPI flash — into a tree of nodes,
on host or on target. The two are siblings: AxlFv is the online
inventory probe, AxlFw is the offline byte-level parser.

`axl_fw_open(data, len)` walks the image (top-level firmware volumes ->
FFS files -> sections), decompressing GUIDED-LZMA and uncompressed
COMPRESSION sections as it goes, and materializes the result into an
owned tree. Each node is one of `AXL_FW_NODE_IMAGE` / `VOLUME` / `FILE`
/ `SECTION`; `REGION` and `NVRAM` are reserved for later phases (Intel
flash-descriptor regions and NVRAM variable stores) and are not emitted
yet.

## Ownership

The input `data` is **borrowed**: an uncompressed node's body points
directly into it, so `data` must outlive the image. The bytes a GUIDED
section decompresses to are **owned** by the image; child nodes parsed
out of a decompressed buffer point into it. `axl_fw_close` frees the
whole node tree and every decompressed buffer the image owns — free the
image before you free the `data` it borrowed.

## Walking the tree

Cursor-style children (like `axl_fv_next` / `axl_block_next`): start at
`axl_fw_root`, descend with `axl_fw_node_first_child`, iterate siblings
with `axl_fw_node_next_sibling`. Per-node accessors report the kind,
the type byte (FFS file type for FILE, section type for SECTION), the
GUID (FILE name or GUID_DEFINED codec), the body bytes, and the offset
within the enclosing parsed stream. All `axl_fw_node_*` accessors are
NULL-tolerant. `axl_fw_find(img, guid, kind)` is a depth-first search
for the first node with a matching GUID (pass `AXL_FW_NODE_IMAGE` as the
kind to match any kind).

```c
AxlFwImage *img = axl_fw_open(data, len);
if (img != NULL) {
    AxlGuid shell = /* 7C04A583-9E3E-4F1C-AD65-E05268D0B4D1 */;
    AxlFwNode *n = axl_fw_find(img, &shell, AXL_FW_NODE_IMAGE);
    const void *body; size_t body_len;
    if (n != NULL && axl_fw_node_data(n, &body, &body_len)) {
        // body is the matched node's bytes, valid until axl_fw_close
    }
    axl_fw_close(img);
}
```

A NULL return from `axl_fw_open` means no firmware volume was found (or
an allocation failed); in this phase the two are not distinguished.

## fwtool

`tools/fwtool.c` is a thin CLI over AxlFw, built both as a UEFI app
(`fwtool.efi`) and as a host binary:

- `fwtool list <image>` — print the node tree (kind, type, GUID,
  offset, body size).
- `fwtool extract <image> <guid> [-o out]` — write a matched node's
  body. When `<guid>` names an FFS FILE, `extract` descends to that
  file's PE32/TE section and emits the runnable image (the `MZ` binary)
  — matching `scripts/extract-fv-shell.py`, not the raw FFS body.
- `fwtool find <image> <guid>` — print the ancestry path of each match.

The host `fwtool` extracts the UEFI Shell from an OVMF/AAVMF image
byte-identically to `scripts/extract-fv-shell.py`, and is the preferred
Shell-extraction tier in `find_shell_efi` (shedding the python3
dependency where a C toolchain is present).

Decompression is provided by AxlCompress: `AXL_COMPRESS_LZMA` (the LZMA
"alone" format used by the EDK2 GUIDED-LZMA codec) decodes and encodes
there; AxlFw drives the decode for GUIDED-LZMA sections.
