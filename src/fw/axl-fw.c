/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* axl-fw.c — raw firmware image parser: FV → FFS file → section tree */

#include <axl/axl-fw.h>
#include <axl/axl-mem.h>      /* axl_malloc, axl_calloc, axl_free, axl_realloc */
#include <axl/axl-str.h>      /* axl_memcpy, axl_memset, axl_memcmp */
#include <axl/axl-compress.h> /* axl_decompress, AXL_COMPRESS_LZMA */
#include <axl/axl-sys.h>      /* axl_guid_equal */

/* ---------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------------
 */

struct AxlFwNode {
    AxlFwNodeKind    kind;
    int              type;        /* FFS file type or section type; 0 for containers */
    bool             has_guid;
    AxlGuid          guid;        /* FFS name GUID or GUID_DEFINED codec GUID */
    const uint8_t   *body;        /* body bytes (borrowed from image or owned buffer) */
    size_t           body_len;
    size_t           offset;      /* byte offset within the stream this node came from */
    AxlFwNode       *first_child;
    AxlFwNode       *last_child;  /* tail of child list; O(1) append (internal) */
    AxlFwNode       *next_sibling;
    /* singly-linked arena list so axl_fw_close can free every node */
    AxlFwNode       *arena_next;
};

struct AxlFwImage {
    const uint8_t   *data;        /* borrowed image (not owned) */
    size_t           len;
    AxlFwNode       *root;        /* AXL_FW_NODE_IMAGE sentinel */
    AxlFwNode       *arena_head;  /* all allocated nodes; freed by axl_fw_close */
    /* owned decompressed buffers (Task 1.3/1.4 will populate these) */
    uint8_t        **owned_bufs;
    size_t           owned_bufs_count;
};

/* ---------------------------------------------------------------------------
 * Constants ported from extract-fv-shell.py
 * ---------------------------------------------------------------------------
 */

static const uint8_t FVH_SIGNATURE[4] = { '_', 'F', 'V', 'H' };

#define FFS_ATTRIB_LARGE_FILE  0x01u
#define FFS_FILETYPE_PAD       0xF0u

/* Max recursion depth guard */
#define FW_MAX_DEPTH  32

/* LZMA codec GUID: EE4E5898-3914-4259-9D6E-DC7BD79403CF
 * bytes_le field layout: Data1 LE32, Data2 LE16, Data3 LE16, Data4[8]. */
static const AxlGuid FW_LZMA_GUID = {
    0xEE4E5898u,  /* Data1 */
    0x3914u,      /* Data2 */
    0x4259u,      /* Data3 */
    { 0x9Du, 0x6Eu, 0xDCu, 0x7Bu, 0xD7u, 0x94u, 0x03u, 0xCFu } /* Data4 */
};

/* Bitmask: if set, the section payload CANNOT be treated as a raw section
 * stream when the codec is unrecognised or decode fails. */
#define GUIDED_PROCESSING_REQUIRED  0x01u

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

/* Align @p value up to @p boundary (must be a power of two). */
static size_t
fw_align(size_t value, size_t boundary)
{
    return (value + boundary - 1u) & ~(boundary - 1u);
}

/* Read a 16-bit LE value from @p buf at byte offset @p off. */
static uint16_t
fw_read_le16(const uint8_t *buf, size_t off)
{
    return (uint16_t)((unsigned)buf[off] | ((unsigned)buf[off + 1u] << 8));
}

/* Read a 32-bit LE value. */
static uint32_t
fw_read_le32(const uint8_t *buf, size_t off)
{
    return (uint32_t)( (unsigned)buf[off]
                     | ((unsigned)buf[off + 1u] << 8)
                     | ((unsigned)buf[off + 2u] << 16)
                     | ((unsigned)buf[off + 3u] << 24));
}

/* Read a 64-bit LE value. */
static uint64_t
fw_read_le64(const uint8_t *buf, size_t off)
{
    uint32_t lo = fw_read_le32(buf, off);
    uint32_t hi = fw_read_le32(buf, off + 4u);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Read a 24-bit LE size (common to FFS headers and section headers). */
static size_t
fw_read_size24(const uint8_t *buf, size_t off)
{
    return (size_t)buf[off]
         | ((size_t)buf[off + 1u] << 8)
         | ((size_t)buf[off + 2u] << 16);
}

/* Allocate a new node and link it into the image's arena. Returns NULL on OOM. */
static AxlFwNode *
fw_node_alloc(AxlFwImage *img)
{
    AxlFwNode *n = axl_calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->arena_next   = img->arena_head;
    img->arena_head = n;
    return n;
}

/* Append @p child to the end of @p parent's child list (document order).
   Uses last_child for O(1) append; iteration order is unchanged. */
static void
fw_append_child(AxlFwNode *parent, AxlFwNode *child)
{
    if (!parent->first_child) {
        parent->first_child = child;
        parent->last_child  = child;
        return;
    }
    parent->last_child->next_sibling = child;
    parent->last_child               = child;
}

/* Read 16 bytes from @p buf at @p off into an AxlGuid using the bytes_le
   (UEFI/EDK2) convention: Data1 LE32, Data2 LE16, Data3 LE16, Data4[8]. */
static void
fw_read_guid_bytes_le(const uint8_t *buf, size_t off, AxlGuid *out)
{
    out->data1 = fw_read_le32(buf, off);
    out->data2 = fw_read_le16(buf, off + 4u);
    out->data3 = fw_read_le16(buf, off + 6u);
    axl_memcpy(out->data4, buf + off + 8u, 8);
}

/* Append @p buf to the image's owned-buffer list so axl_fw_close frees it.
 * Returns true on success.  On OOM the list is left unchanged (the caller
 * must NOT have stored @p buf anywhere yet and should free it immediately
 * after this returns false). */
static bool
fw_owned_buf_push(AxlFwImage *img, uint8_t *buf)
{
    size_t   new_count = img->owned_bufs_count + 1u;
    uint8_t **arr      = axl_realloc(img->owned_bufs,
                                     new_count * sizeof(uint8_t *));
    if (!arr)
        return false;
    arr[img->owned_bufs_count] = buf;
    img->owned_bufs            = arr;
    img->owned_bufs_count      = new_count;
    return true;
}

/* Forward declaration: fw_parse_sections calls fw_parse_ffs_files for
   SECTION_FIRMWARE_VOLUME_IMAGE recursion, and fw_parse_ffs_files calls
   fw_parse_sections for file section parsing.  The two are mutually
   recursive; declare fw_parse_ffs_files here so the compiler sees it. */
static bool fw_parse_ffs_files(AxlFwImage    *img,
                               AxlFwNode     *vol_node,
                               const uint8_t *fv,
                               size_t         fv_len,
                               size_t         fv_offset,
                               int            depth);

/* ---------------------------------------------------------------------------
 * Section iterator
 * Mirrors _iter_sections in extract-fv-shell.py lines 98-113.
 *
 * @p img         : image (for node allocation)
 * @p parent      : FILE node (or SECTION for encapsulation) to attach children
 * @p blob        : section-stream bytes
 * @p blob_len    : length of @p blob
 * @p base_offset : byte offset of blob[0] within the containing parsed stream
 * @p depth       : current recursion depth guard
 * ---------------------------------------------------------------------------
 */
static bool
fw_parse_sections(AxlFwImage    *img,
                  AxlFwNode     *parent,
                  const uint8_t *blob,
                  size_t         blob_len,
                  size_t         base_offset,
                  int            depth)
{
    if (depth >= FW_MAX_DEPTH)
        return true; /* silently stop; don't fail the parse */

    size_t pos = 0;
    while (pos + 4u <= blob_len) {
        size_t  size24   = fw_read_size24(blob, pos);
        uint8_t sec_type = blob[pos + 3u];
        size_t  body_off;
        size_t  size;

        if (size24 == 0xFFFFFFu) {
            /* Extended section: next 4 bytes are uint32 total size */
            if (pos + 8u > blob_len)
                break;
            size     = (size_t)fw_read_le32(blob, pos + 4u);
            body_off = 8u;
        } else {
            size     = size24;
            body_off = 4u;
        }

        /* Overflow-safe bounds check: `pos + size` can wrap when `size`
           comes from erased/garbage bytes (e.g. an extended-section size of
           0xFFFFFFFF), so compare against the remaining span instead. The
           loop guard guarantees pos <= blob_len, so blob_len - pos is safe. */
        if (size < body_off || size > blob_len - pos)
            break;

        AxlFwNode *sec = fw_node_alloc(img);
        if (!sec)
            return false;

        sec->kind     = AXL_FW_NODE_SECTION;
        sec->type     = (int)(unsigned)sec_type;
        sec->body     = blob + pos + body_off;
        sec->body_len = size - body_off;
        sec->offset   = base_offset + pos;

        fw_append_child(parent, sec);

        /* Encapsulation sections: recurse into their payloads.
         *
         * SECTION_COMPRESSION (0x01):
         *   body layout: [0..3] UncompressedLength (LE32), [4] CompressionType
         *   mirrors Python: comp_type = section[8]; if comp_type == 0,
         *   recurse on section[9:] (i.e. blob[pos+9..pos+size-1]).
         *   CompressionType 1 (Tiano) is not handled (no decoder present).
         *
         * SECTION_FIRMWARE_VOLUME_IMAGE (0x17):
         *   body is a self-contained FV; create a VOLUME child and recurse
         *   into it via fw_parse_ffs_files, mirroring Python _search_fv.
         */
        if (sec_type == 0x01u) {
            /* SECTION_COMPRESSION: byte 8 from section start = body[4].
             * Guard against size < 9: a section with size < 9 cannot hold
             * the 4-byte UncompressedLength + 1-byte CompressionType, so
             * treat it as a leaf and skip the recursion. */
            if (size >= 9u) {
                uint8_t comp_type = blob[pos + 8u];
                if (comp_type == 0u) {
                    /* Uncompressed: inner section stream starts at blob+pos+9 */
                    const uint8_t *inner     = blob + pos + 9u;
                    size_t         inner_len = size - 9u;
                    if (!fw_parse_sections(img, sec, inner, inner_len,
                                           0u, depth + 1))
                        return false;
                }
            }
        } else if (sec_type == 0x02u) {
            /* SECTION_GUID_DEFINED: mirrors _search_sections GUID_DEFINED
             * branch (extract-fv-shell.py lines 121-132).
             *
             * Section layout (within blob[pos..pos+size-1]):
             *   [0..2]  Size24 (already parsed above)
             *   [3]     Type = 0x02 (already matched)
             *   [4..19] Codec GUID (bytes_le)
             *   [20..21] DataOffset (LE16): offset of payload from section start
             *   [22..23] Attributes (LE16)
             *   [DataOffset..size-1] payload
             *
             * For body_off==4, body = blob+pos+4; DataOffset is at blob+pos+20.
             * Need at minimum 24 bytes for the full fixed header:
             *   4 (section hdr) + 16 (GUID) + 2 (DataOffset) + 2 (Attrs).
             */
            if (size >= 24u) {
                AxlGuid  codec;
                fw_read_guid_bytes_le(blob, pos + 4u, &codec);
                uint16_t data_off = fw_read_le16(blob, pos + 20u);
                uint16_t attrs    = fw_read_le16(blob, pos + 22u);

                /* Set the GUID_DEFINED section's own guid field */
                sec->has_guid = true;
                sec->guid     = codec;

                /* Validate DataOffset: must be within [4, size) */
                if ((size_t)data_off >= 4u && (size_t)data_off < size) {
                    const uint8_t *payload     = blob + pos + (size_t)data_off;
                    size_t         payload_len = size - (size_t)data_off;

                    /* Mirrors Python _search_sections lines 121-132:
                     * try LZMA decode; if codec is unknown OR decode fails,
                     * fall through to the shared raw-stream fallback when
                     * PROCESSING_REQUIRED is clear. */
                    uint8_t *decoded     = NULL;
                    size_t   decoded_len = 0;

                    if (axl_guid_equal(&codec, &FW_LZMA_GUID)) {
                        void  *dec     = NULL;
                        size_t dec_len = 0;
                        if (axl_decompress(AXL_COMPRESS_LZMA, payload,
                                           payload_len, &dec, &dec_len)
                                == AXL_OK && dec != NULL) {
                            decoded     = (uint8_t *)dec;
                            decoded_len = dec_len;
                        }
                        /* On failure dec is NULL (axl_decompress clears *out);
                         * decoded stays NULL and we fall through. */
                    }

                    if (decoded != NULL) {
                        /* Record ownership BEFORE recursing so axl_fw_close
                         * frees the buffer even if recursion fails. */
                        if (!fw_owned_buf_push(img, decoded)) {
                            axl_free(decoded);
                            return false;
                        }
                        if (!fw_parse_sections(img, sec,
                                               decoded, decoded_len,
                                               0u, depth + 1))
                            return false;
                    } else if (!(attrs & GUIDED_PROCESSING_REQUIRED)) {
                        /* Covers BOTH unknown codec AND failed-LZMA-decode:
                         * PROCESSING_REQUIRED clear → payload is a raw section
                         * stream.  This mirrors the Python fallback exactly. */
                        if (!fw_parse_sections(img, sec, payload,
                                               payload_len, 0u, depth + 1))
                            return false;
                    }
                    /* else: PROCESSING_REQUIRED set and decode failed →
                     * opaque leaf; the node was already appended above. */
                }
            }
        } else if (sec_type == 0x17u) {
            /* SECTION_FIRMWARE_VOLUME_IMAGE: body is a nested FV */
            AxlFwNode *nested_vol = fw_node_alloc(img);
            if (!nested_vol)
                return false;
            nested_vol->kind   = AXL_FW_NODE_VOLUME;
            nested_vol->offset = 0u; /* offset within the FV_IMAGE body */
            fw_append_child(sec, nested_vol);
            if (!fw_parse_ffs_files(img, nested_vol,
                                    sec->body, sec->body_len, 0u, depth + 1))
                return false;
        }

        /* Sections are 4-byte aligned within the stream */
        pos = fw_align(pos + size, 4u);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * FFS file iterator
 * Mirrors _iter_ffs_files in extract-fv-shell.py lines 66-95.
 *
 * @p img        : image (for node allocation)
 * @p vol_node   : VOLUME node to attach FILE children to
 * @p fv         : FV bytes (starting at the FV header)
 * @p fv_len     : length of @p fv slice
 * @p fv_offset  : byte offset of fv[0] within the image (for node offsets)
 * ---------------------------------------------------------------------------
 */
static bool
fw_parse_ffs_files(AxlFwImage    *img,
                   AxlFwNode     *vol_node,
                   const uint8_t *fv,
                   size_t         fv_len,
                   size_t         fv_offset,
                   int            depth)
{
    if (depth >= FW_MAX_DEPTH)
        return true; /* silently stop; don't fail the parse */

    if (fv_len < 0x40u || axl_memcmp(fv + 40u, FVH_SIGNATURE, 4u) != 0)
        return true; /* not a valid FV header, skip gracefully */

    uint16_t header_len     = fw_read_le16(fv, 48u);
    uint16_t ext_header_off = fw_read_le16(fv, 52u);

    size_t start;
    if (ext_header_off) {
        /* EFI_FIRMWARE_VOLUME_EXT_HEADER: FvName[16] + ExtHeaderSize[4] */
        if ((size_t)ext_header_off + 20u > fv_len)
            return true;
        uint32_t ext_size = fw_read_le32(fv, (size_t)ext_header_off + 16u);
        start = (size_t)ext_header_off + (size_t)ext_size;
    } else {
        start = header_len;
    }

    size_t pos = fw_align(start, 8u);

    while (pos + 24u <= fv_len) {
        /* FFS file header layout:
         *   [0..15]  Name (GUID, bytes_le)
         *   [16..17] IntegrityCheck
         *   [18]     Type
         *   [19]     Attributes
         *   [20..22] Size (24-bit LE, includes the 24-byte header)
         *   [23]     State
         */
        uint8_t file_type = fv[pos + 18u];
        uint8_t attrib    = fv[pos + 19u];
        size_t  size24    = fw_read_size24(fv, pos + 20u);

        /* Erased free space: 0xFFFFFF without LARGE_FILE → end of files */
        if (size24 == 0xFFFFFFu && !(attrib & FFS_ATTRIB_LARGE_FILE))
            break;

        size_t header;
        size_t file_size;

        if (attrib & FFS_ATTRIB_LARGE_FILE) {
            /* 8-byte size follows the 24-byte header */
            if (pos + 32u > fv_len)
                break;
            file_size = (size_t)fw_read_le64(fv, pos + 24u);
            header    = 32u;
        } else {
            file_size = size24;
            header    = 24u;
        }

        /* Overflow-safe bounds check. Erased free space whose Attributes
           byte is 0xFF spuriously sets FFS_ATTRIB_LARGE_FILE, so the 8-byte
           LARGE_FILE size field is read as 0xFFFFFFFFFFFFFFFF. `pos +
           file_size` would then wrap past fv_len and defeat this guard,
           spinning the loop forever (the Python reference avoids this only
           because its ints are arbitrary-precision). Compare against the
           remaining span instead; the loop guard ensures pos <= fv_len. */
        if (file_size < header || file_size > fv_len - pos)
            break;

        /* Skip PAD files and fully-erased entries (name = 0xFF * 16) */
        bool erased = true;
        for (size_t i = 0; i < 16u; i++) {
            if (fv[pos + i] != 0xFFu) {
                erased = false;
                break;
            }
        }

        if (file_type != FFS_FILETYPE_PAD && !erased) {
            AxlFwNode *file = fw_node_alloc(img);
            if (!file)
                return false;

            file->kind     = AXL_FW_NODE_FILE;
            file->type     = (int)(unsigned)file_type;
            file->has_guid = true;
            fw_read_guid_bytes_le(fv, pos, &file->guid);
            file->body     = fv + pos + header;
            file->body_len = file_size - header;
            /* Absolute offset within the image */
            file->offset   = fv_offset + pos;

            fw_append_child(vol_node, file);

            /* Parse section stream within the file body; base offset accounts
               for the FV start and the FFS header */
            if (!fw_parse_sections(img, file,
                                   file->body, file->body_len,
                                   file->offset + header,
                                   depth + 1))
                return false;
        }

        pos = fw_align(pos + file_size, 8u);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Top-level FV scan
 * Mirrors _iter_top_level_fvs in extract-fv-shell.py lines 155-167.
 * ---------------------------------------------------------------------------
 */
static bool
fw_parse_top_level(AxlFwImage *img)
{
    const uint8_t *data = img->data;
    size_t         len  = img->len;
    size_t         pos  = 0;

    while (pos + 0x40u <= len) {
        /* _FVH signature lives at offset 40 within the FV header */
        if (axl_memcmp(data + pos + 40u, FVH_SIGNATURE, 4u) == 0) {
            uint64_t fv_len64   = fw_read_le64(data, pos + 32u);
            uint16_t header_len = fw_read_le16(data, pos + 48u);

            /* Overflow-safe span check (see fw_parse_ffs_files): compare
               fv_len64 against the remaining bytes rather than computing
               pos + fv_len64, which could wrap for a garbage length. The
               loop guard ensures pos <= len. */
            if ((size_t)header_len >= 0x48u
                && (size_t)fv_len64 >= (size_t)header_len
                && (size_t)fv_len64 <= len - pos) {

                AxlFwNode *vol = fw_node_alloc(img);
                if (!vol)
                    return false;

                vol->kind   = AXL_FW_NODE_VOLUME;
                vol->offset = pos;   /* offset of FV header within the image */

                fw_append_child(img->root, vol);

                if (!fw_parse_ffs_files(img, vol,
                                        data + pos, (size_t)fv_len64,
                                        pos, 0))
                    return false;

                pos = fw_align(pos + (size_t)fv_len64, 8u);
                continue;
            }
        }
        pos += 16u;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Depth-first GUID search
 * ---------------------------------------------------------------------------
 */
static AxlFwNode *
fw_find_dfs(AxlFwNode *node, const AxlGuid *guid, AxlFwNodeKind kind)
{
    /* Iterate across siblings (loop, not recursion) to keep stack depth
     * proportional to nesting depth alone — not to sibling count.
     * Recurse only into first_child to perform DFS. */
    while (node) {
        if (node->has_guid) {
            if ((kind == AXL_FW_NODE_IMAGE || kind == node->kind)
                && axl_memcmp(&node->guid, guid, sizeof(AxlGuid)) == 0)
                return node;
        }

        /* Check this node's subtree before moving to its sibling (DFS order) */
        AxlFwNode *hit = fw_find_dfs(node->first_child, guid, kind);
        if (hit)
            return hit;

        node = node->next_sibling;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------
 */

AxlFwImage *
axl_fw_open(
    const void *data,
    size_t      len)
{
    if (!data || len == 0)
        return NULL;

    AxlFwImage *img = axl_calloc(1, sizeof(*img));
    if (!img)
        return NULL;

    img->data = (const uint8_t *)data;
    img->len  = len;

    /* Allocate the IMAGE root node */
    img->root = fw_node_alloc(img);
    if (!img->root) {
        axl_free(img);
        return NULL;
    }
    img->root->kind = AXL_FW_NODE_IMAGE;

    /* Parse: scan for FVs and build the tree */
    if (!fw_parse_top_level(img)) {
        axl_fw_close(img);
        return NULL;
    }

    /* A non-NULL return requires at least one volume */
    if (!img->root->first_child) {
        axl_fw_close(img);
        return NULL;
    }

    return img;
}

void
axl_fw_close(
    AxlFwImage *img)
{
    if (!img)
        return;

    /* Free every allocated node */
    AxlFwNode *n = img->arena_head;
    while (n) {
        AxlFwNode *next = n->arena_next;
        axl_free(n);
        n = next;
    }

    /* Free owned decompressed buffers (Task 1.3/1.4) */
    for (size_t i = 0; i < img->owned_bufs_count; i++)
        axl_free(img->owned_bufs[i]);
    if (img->owned_bufs)
        axl_free(img->owned_bufs);

    axl_free(img);
}

AxlFwNode *
axl_fw_root(
    AxlFwImage *img)
{
    if (!img)
        return NULL;
    return img->root;
}

AxlFwNode *
axl_fw_node_first_child(
    AxlFwNode *node)
{
    if (!node)
        return NULL;
    return node->first_child;
}

AxlFwNode *
axl_fw_node_next_sibling(
    AxlFwNode *node)
{
    if (!node)
        return NULL;
    return node->next_sibling;
}

AxlFwNodeKind
axl_fw_node_kind(
    AxlFwNode *node)
{
    if (!node)
        return AXL_FW_NODE_IMAGE;
    return node->kind;
}

int
axl_fw_node_type(
    AxlFwNode *node)
{
    if (!node)
        return 0;
    return node->type;
}

bool
axl_fw_node_guid(
    AxlFwNode *node,
    AxlGuid   *out)
{
    if (!node || !node->has_guid)
        return false;
    if (out)
        axl_memcpy(out, &node->guid, sizeof(AxlGuid));
    return true;
}

bool
axl_fw_node_data(
    AxlFwNode   *node,
    const void **ptr,
    size_t      *len)
{
    if (!node || !node->body)
        return false;
    if (ptr)
        *ptr = node->body;
    if (len)
        *len = node->body_len;
    /* A zero-length body (body_len == 0) still returns true: the section
       exists and its body is present but empty. Task 1.3 must handle len==0. */
    return true;
}

bool
axl_fw_node_offset(
    AxlFwNode *node,
    size_t    *out)
{
    if (!node)
        return false;
    if (out)
        *out = node->offset;
    return true;
}

AxlFwNode *
axl_fw_find(
    AxlFwImage    *img,
    const AxlGuid *guid,
    AxlFwNodeKind  kind)
{
    if (!img || !guid)
        return NULL;
    /* Start DFS from root's first child (root itself has no GUID) */
    return fw_find_dfs(img->root->first_child, guid, kind);
}
