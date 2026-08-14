/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-boot.c
    Boot-option management — typed wrappers over the Boot####/BootOrder/
    BootNext/BootCurrent firmware-variable family.

    The EFI_LOAD_OPTION binary codec lives entirely inside this file;
    no raw LOAD_OPTION bytes ever cross the public API boundary.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-boot.h>
#include <axl/axl-nvstore.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>     /* axl_guid_equal */
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("boot");

// ---------------------------------------------------------------------------
// DevicePathFromText protocol binding
//
// DevicePathToText is exposed publicly via axl_device_path_to_text();
// the from-text direction is private to boot-option encoding and stays
// inline here.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
typedef struct {
    void *(EFIAPI *ConvertTextToDeviceNode)(const unsigned short *Text);
    void *(EFIAPI *ConvertTextToDevicePath)(const unsigned short *Text);
} DevicePathFromTextProtocol;
#pragma pack(pop)

static const AxlGuid DEVICE_PATH_FROM_TEXT_GUID = AXL_GUID(
    0x05c99a21, 0xc70f, 0x4ad2,
    0x8a, 0x5f, 0x35, 0xdf, 0x33, 0x43, 0xf5, 0x1e);

static DevicePathFromTextProtocol *
get_from_text(
    void
    )
{
    static DevicePathFromTextProtocol *cached;
    if (cached != NULL) {
        return cached;
    }
    void *p = NULL;
    if (axl_bs()->LocateProtocol(
            (EFI_GUID *)&DEVICE_PATH_FROM_TEXT_GUID, NULL, &p) == EFI_SUCCESS) {
        cached = (DevicePathFromTextProtocol *)p;
    }
    return cached;
}

// ---------------------------------------------------------------------------
// Variable-name helpers
// ---------------------------------------------------------------------------

/// Format a Boot#### name into a 9-byte buffer ('Boot' + 4 hex + NUL).
static void
format_boot_name(
    uint16_t  index,
    char      out[9]
    )
{
    axl_snprintf(out, 9, "Boot%04X", index);
}

// ---------------------------------------------------------------------------
// Generic variable read — returns malloc'd buffer + size.
// ---------------------------------------------------------------------------

static int
read_global_variable(
    const char  *key,
    void       **out_buf,
    size_t      *out_len
    )
{
    *out_buf = NULL;
    *out_len = 0;

    /* Size query — axl_nvstore_get returns -1 on EFI_BUFFER_TOO_SMALL
       but populates *size to the required length. */
    size_t needed = 0;
    axl_nvstore_get("global", key, NULL, &needed);
    if (needed == 0) {
        return -1;
    }

    void *buf = axl_malloc(needed);
    if (buf == NULL) {
        return -1;
    }
    if (axl_nvstore_get("global", key, buf, &needed) != AXL_OK) {
        axl_free(buf);
        return -1;
    }
    *out_buf = buf;
    *out_len = needed;
    return 0;
}

// ---------------------------------------------------------------------------
// LOAD_OPTION decoder (internal — no raw bytes leak out)
// ---------------------------------------------------------------------------

/* On-wire layout (UEFI 2.11 §3.1.3):
       uint32_t attrs;
       uint16_t file_path_list_length;     // bytes in device path
       CHAR16   description[];             // NUL-terminated UCS-2
       uint8_t  file_path_list[file_path_list_length];
       uint8_t  optional_data[];           // remainder
*/

#define LOAD_OPTION_HEADER_BYTES  6u  /* attrs + path_list_length */

static int
decode_load_option(
    const uint8_t  *raw,
    size_t          raw_len,
    AxlBootOption  *out
    )
{
    if (raw_len < LOAD_OPTION_HEADER_BYTES) {
        return -1;
    }

    uint32_t attrs;
    uint16_t fp_len;
    axl_memcpy(&attrs,  raw + 0, 4);
    axl_memcpy(&fp_len, raw + 4, 2);

    /* Description starts at offset 6, runs as UCS-2 until a 16-bit zero. */
    const unsigned short *desc_w = (const unsigned short *)(raw + 6);
    size_t desc_chars_max = (raw_len - 6) / 2;
    size_t desc_chars     = 0;
    while (desc_chars < desc_chars_max && desc_w[desc_chars] != 0) {
        desc_chars++;
    }
    if (desc_chars == desc_chars_max) {
        /* No NUL terminator within the variable */
        return -1;
    }

    size_t desc_bytes = (desc_chars + 1) * 2;  /* include NUL */
    size_t consumed   = 6 + desc_bytes;
    if (consumed + fp_len > raw_len) {
        return -1;
    }
    const uint8_t *fp_bytes = raw + consumed;
    consumed += fp_len;

    size_t opt_len = raw_len - consumed;
    const uint8_t *opt_bytes = raw + consumed;

    out->attrs = attrs;

    /* Description: UCS-2 → UTF-8. axl_ucs2_to_utf8 expects NUL-term
       which we have at desc_w[desc_chars]. */
    out->description = axl_ucs2_to_utf8(desc_w);

    /* Device path: convert via DevicePathToText if the protocol is up. */
    out->device_path = (fp_len > 0) ? axl_device_path_to_text(fp_bytes) : NULL;

    /* Opt data: copy verbatim. */
    out->opt_data     = NULL;
    out->opt_data_len = 0;
    if (opt_len > 0) {
        void *blob = axl_malloc(opt_len);
        if (blob == NULL) {
            axl_free(out->description);
            axl_free(out->device_path);
            return -1;
        }
        axl_memcpy(blob, opt_bytes, opt_len);
        out->opt_data     = blob;
        out->opt_data_len = opt_len;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// LOAD_OPTION encoder (internal)
// ---------------------------------------------------------------------------

static int
encode_load_option(
    const AxlBootOption  *opt,
    void                **out_buf,
    size_t               *out_len
    )
{
    *out_buf = NULL;
    *out_len = 0;

    /* Description as UCS-2 (NUL-terminated). Always allocate so the
       cleanup path is unconditional — passing the empty string still
       yields a heap-owned single-NUL buffer. */
    unsigned short *desc_w = axl_utf8_to_ucs2(
        opt->description != NULL ? opt->description : "");
    if (desc_w == NULL) {
        return -1;
    }
    size_t desc_chars = axl_wcslen(desc_w);
    size_t desc_bytes = (desc_chars + 1) * 2;

    /* Device path text → bytes. NULL or "" yields a minimal "end
       of entire device path" node (4 bytes: 0x7F 0xFF 0x04 0x00).
       UEFI 2.11 §10.3.1 requires every FilePathList to end with
       an End-of-Path node; a zero-length list is spec-invalid and
       AArch64 OVMF rejects it with EFI_INVALID_PARAMETER (x64
       OVMF accepts it leniently). The synthesized end-node makes
       the encoded Boot#### portable. */
    void   *fp_bytes = NULL;
    size_t  fp_len   = 0;
    static const uint8_t end_of_path_only[4] = { 0x7F, 0xFF, 0x04, 0x00 };
    if (opt->device_path != NULL && opt->device_path[0] != '\0') {
        DevicePathFromTextProtocol *dpft = get_from_text();
        if (dpft == NULL || dpft->ConvertTextToDevicePath == NULL) {
            axl_free(desc_w);
            axl_debug("DevicePathFromText protocol unavailable; cannot encode boot option");
            return -1;
        }
        unsigned short *path_w = axl_utf8_to_ucs2(opt->device_path);
        if (path_w == NULL) {
            axl_free(desc_w);
            return -1;
        }
        void *dp = dpft->ConvertTextToDevicePath(path_w);
        axl_free(path_w);
        if (dp == NULL) {
            axl_free(desc_w);
            return -1;
        }
        /* Walk device-path nodes to compute total length. Each node is
           {uint8_t type; uint8_t subtype; uint16_t length}; end node
           is type=0x7f, subtype=0xff, length=4. Cap on node count
           rather than byte length — real device paths are well under
           a few dozen nodes; a malformed path that loops without
           reaching an End node would be caught by either bound, but
           the node count is the meaningful safety net. */
        #define MAX_DP_NODES  64
        const uint8_t *p = (const uint8_t *)dp;
        bool   found_end = false;
        size_t node_count = 0;
        while (node_count < MAX_DP_NODES) {
            uint16_t node_len;
            axl_memcpy(&node_len, p + 2, 2);
            if (node_len < 4) {
                break;
            }
            uint8_t t  = p[0];
            uint8_t st = p[1];
            p += node_len;
            node_count++;
            if (t == 0x7F && st == 0xFF) {
                found_end = true;
                break;
            }
        }
        #undef MAX_DP_NODES
        if (!found_end) {
            axl_bs()->FreePool(dp);  /* axl-pool-direct: ConvertTextToDevicePath result */
            axl_free(desc_w);
            return -1;
        }
        fp_len = (size_t)(p - (const uint8_t *)dp);
        fp_bytes = axl_malloc(fp_len);
        if (fp_bytes == NULL) {
            axl_bs()->FreePool(dp);  /* axl-pool-direct: ConvertTextToDevicePath result */
            axl_free(desc_w);
            return -1;
        }
        axl_memcpy(fp_bytes, dp, fp_len);
        axl_bs()->FreePool(dp);  /* axl-pool-direct: ConvertTextToDevicePath result */
    }

    /* Substitute the synthesized end-node when no real device
       path was supplied. Keep `fp_bytes` reserved for the
       heap-owned conversion result so the cleanup path stays
       simple: free fp_bytes if it's non-NULL, never free the
       static end-node. */
    const void  *fp_src = (fp_bytes != NULL) ? fp_bytes : end_of_path_only;
    size_t       fp_emit_len = (fp_bytes != NULL) ? fp_len : 4;

    size_t total = LOAD_OPTION_HEADER_BYTES
                 + desc_bytes
                 + fp_emit_len
                 + opt->opt_data_len;
    uint8_t *buf = axl_malloc(total);
    if (buf == NULL) {
        axl_free(fp_bytes);
        axl_free(desc_w);
        return -1;
    }

    axl_memcpy(buf + 0, &opt->attrs, 4);
    uint16_t fp_len16 = (uint16_t)fp_emit_len;
    axl_memcpy(buf + 4, &fp_len16, 2);
    axl_memcpy(buf + 6, desc_w, desc_bytes);
    axl_memcpy(buf + 6 + desc_bytes, fp_src, fp_emit_len);
    if (opt->opt_data_len > 0 && opt->opt_data != NULL) {
        axl_memcpy(buf + 6 + desc_bytes + fp_emit_len,
                   opt->opt_data, opt->opt_data_len);
    }

    axl_free(fp_bytes);
    axl_free(desc_w);
    *out_buf = buf;
    *out_len = total;
    return 0;
}

// ---------------------------------------------------------------------------
// Public API — boot option get/set/delete/free
// ---------------------------------------------------------------------------

void
axl_boot_option_free(
    AxlBootOption  *opt
    )
{
    if (opt == NULL) {
        return;
    }
    axl_free(opt->description);
    axl_free(opt->device_path);
    axl_free(opt->opt_data);
    opt->description   = NULL;
    opt->device_path   = NULL;
    opt->opt_data      = NULL;
    opt->opt_data_len  = 0;
    opt->attrs         = 0;
    opt->index         = 0;
}

int
axl_boot_option_get(
    uint16_t        index,
    AxlBootOption  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    char name[9];
    format_boot_name(index, name);

    void  *raw = NULL;
    size_t raw_len = 0;
    if (read_global_variable(name, &raw, &raw_len) != 0) {
        return AXL_ERR;
    }

    out->index         = index;
    out->attrs         = 0;
    out->description   = NULL;
    out->device_path   = NULL;
    out->opt_data      = NULL;
    out->opt_data_len  = 0;

    int rc = decode_load_option(raw, raw_len, out);
    axl_free(raw);
    if (rc != 0) {
        axl_boot_option_free(out);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_boot_option_set(
    uint16_t              index,
    const AxlBootOption  *opt
    )
{
    if (opt == NULL) {
        return AXL_ERR;
    }
    char name[9];
    format_boot_name(index, name);

    void  *encoded = NULL;
    size_t encoded_len = 0;
    if (encode_load_option(opt, &encoded, &encoded_len) != 0) {
        return AXL_ERR;
    }

    int rc = axl_nvstore_set(
        "global", name, encoded, encoded_len,
        AXL_NV_PERSISTENT | AXL_NV_BOOT | AXL_NV_RUNTIME);
    axl_free(encoded);
    return rc;
}

int
axl_boot_option_delete(
    uint16_t  index
    )
{
    char name[9];
    format_boot_name(index, name);
    return axl_nvstore_delete("global", name);
}

// ---------------------------------------------------------------------------
// Public API — BootOrder
// ---------------------------------------------------------------------------

int
axl_boot_order_get(
    uint16_t  **out,
    size_t     *count
    )
{
    if (out == NULL || count == NULL) {
        return AXL_ERR;
    }
    void  *raw = NULL;
    size_t raw_len = 0;
    if (read_global_variable("BootOrder", &raw, &raw_len) != 0) {
        return AXL_ERR;
    }
    if ((raw_len % 2) != 0) {
        axl_free(raw);
        return AXL_ERR;
    }
    *out   = (uint16_t *)raw;     /* aligned: malloc returns 16-byte aligned */
    *count = raw_len / 2;
    return AXL_OK;
}

int
axl_boot_order_set(
    const uint16_t  *order,
    size_t           count
    )
{
    if (count == 0) {
        return axl_nvstore_delete("global", "BootOrder");
    }
    return axl_nvstore_set(
        "global", "BootOrder", order, count * sizeof(uint16_t),
        AXL_NV_PERSISTENT | AXL_NV_BOOT | AXL_NV_RUNTIME);
}

// ---------------------------------------------------------------------------
// Public API — BootNext / BootCurrent
// ---------------------------------------------------------------------------

int
axl_boot_next_get(
    uint16_t  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    size_t sz = sizeof(uint16_t);
    if (axl_nvstore_get("global", "BootNext", out, &sz) != AXL_OK) {
        return AXL_ERR;
    }
    if (sz != sizeof(uint16_t)) {
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_boot_next_set(
    uint16_t  index
    )
{
    return axl_nvstore_set(
        "global", "BootNext", &index, sizeof(uint16_t),
        AXL_NV_PERSISTENT | AXL_NV_BOOT | AXL_NV_RUNTIME);
}

int
axl_boot_next_clear(
    void
    )
{
    return axl_nvstore_delete("global", "BootNext");
}

int
axl_boot_current_get(
    uint16_t  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    size_t sz = sizeof(uint16_t);
    if (axl_nvstore_get("global", "BootCurrent", out, &sz) != AXL_OK) {
        return AXL_ERR;
    }
    if (sz != sizeof(uint16_t)) {
        return AXL_ERR;
    }
    return AXL_OK;
}
