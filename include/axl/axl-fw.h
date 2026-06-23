/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-fw.h
    Read-only parser for raw UEFI firmware images (.fd / SPI dumps).

    Parses an image into an owned tree of nodes — firmware volumes, FFS
    files, and sections — decompressing GUIDED-LZMA and uncompressed
    COMPRESSION sections on the way. Unlike <axl/axl-fv.h> (which
    enumerates LIVE volumes over EFI_FIRMWARE_VOLUME2_PROTOCOL), this
    works on raw bytes, on host or target.

    @note All `axl_fw_node_*` accessors are NULL-tolerant: given a NULL
    node they return the empty result — `axl_fw_node_kind` returns
    `AXL_FW_NODE_IMAGE`, `axl_fw_node_type` returns 0,
    `axl_fw_node_first_child`/`axl_fw_node_next_sibling` return NULL, and
    `axl_fw_node_guid`/`axl_fw_node_data`/`axl_fw_node_offset` return
    false. */
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
        volume is found or on allocation failure. In Phase 1 a NULL return
        does not distinguish "no firmware volume found" from an allocation
        failure. */
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
    SECTION. The value is an unsigned byte (0-255). 0 is returned for
    container kinds (IMAGE/VOLUME) AND is a legitimate raw type value, so
    `axl_fw_node_type` alone does not distinguish a container from a
    type-0 leaf — check `axl_fw_node_kind` first. */
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

/** @brief Byte offset of @p node within the stream it was parsed from.
    For top-level volumes and any node whose bytes reside in the borrowed
    input image, this is the offset within that image. For a node parsed
    out of a decompressed encapsulation (e.g. inside a GUIDED-LZMA section),
    it is the offset within that decompressed buffer, not the image — the
    bytes do not exist contiguously in the image. Combined with the node's
    ancestry this locates the node for `fwtool list`/`find`.
    @return true and sets @p out for any parsed node; false for a NULL node. */
bool
axl_fw_node_offset(
    AxlFwNode *node,   ///< any node
    size_t    *out     ///< [out] offset within the enclosing parsed stream
);

/** @brief Depth-first search for the first node whose GUID equals @p guid.
    Restrict to a kind, or pass AXL_FW_NODE_IMAGE to match any kind.
    GUIDs exist only on FILE nodes (the FFS file name) and GUID_DEFINED
    SECTION nodes (the codec GUID); the IMAGE root and VOLUME nodes have
    no GUID and are never returned. With @p kind == AXL_FW_NODE_IMAGE
    (the "any" sentinel), the first GUID-bearing node in depth-first order
    is returned.
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
