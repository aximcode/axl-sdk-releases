/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-clipboard.c:
 *
 * Process-spanning cut/copy/paste clipboard. See axl-clipboard.h.
 *
 * Backed by one boot-persistent shared-memory segment (axl-shm) named
 * "axl/clipboard", so the clipboard survives the app that set it and any
 * later app in the same boot can paste it — no driver, no NVRAM. The
 * segment holds a small ClipHeader (versioned via struct_size) followed by
 * the data bytes then the optional MIME string:
 *
 *   [ ClipHeader ][ data_len bytes ][ mime_len bytes ]
 *
 * set() builds the new image in a scratch buffer, snapshots the old
 * segment, then swaps — restoring the old content if creating the new
 * segment fails, so an OOM never loses the clipboard. get() borrows
 * pointers straight into the resident segment.
 */

#include <axl/axl-clipboard.h>

#include <axl/axl-shm.h>
#include <axl/axl-bytes.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>   /* axl_memcpy, axl_strlen */

#define CLIP_SHM_NAME "axl/clipboard"

typedef struct {
    uint32_t struct_size;   ///< sizeof(ClipHeader) at write time (data starts here)
    uint32_t mime_len;      ///< MIME bytes incl. NUL, 0 if none
    uint64_t data_len;      ///< clipboard byte count
} ClipHeader;

/* Locate the current clipboard segment and validate its layout against the
   segment size. Returns the header (with data/mime offsets resolved) or
   NULL if absent / malformed. */
static ClipHeader *
clip_segment(uint8_t **out_data, char **out_mime)
{
    size_t seg_size = 0;
    void  *seg = axl_shm_open(CLIP_SHM_NAME, 0, 0, &seg_size);
    if (seg == NULL || seg_size < sizeof(ClipHeader)) {
        return NULL;
    }
    ClipHeader *h = (ClipHeader *)seg;
    /* Forward-compat: data begins after the writer's (possibly larger)
       header; reject anything that doesn't fit the segment. */
    if (h->struct_size < sizeof(ClipHeader)
        || h->struct_size > seg_size
        || h->data_len > seg_size - h->struct_size
        || h->mime_len > seg_size - h->struct_size - h->data_len) {
        return NULL;
    }
    uint8_t *data = (uint8_t *)seg + h->struct_size;
    /* The MIME is consumed as a C string (paste prints it with %s), so a
       corrupt/foreign segment whose MIME window lacks a terminating NUL
       must be rejected rather than risk an out-of-bounds scan. The bounds
       checks above guarantee this index is in range. */
    if (h->mime_len > 0 && data[h->data_len + h->mime_len - 1] != '\0') {
        return NULL;
    }
    if (out_data != NULL) {
        *out_data = data;
    }
    if (out_mime != NULL) {
        *out_mime = (h->mime_len > 0) ? (char *)(data + h->data_len) : NULL;
    }
    return h;
}

int
axl_clipboard_set(const void *data, size_t len, const char *mime)
{
    if (data == NULL && len > 0) {
        return AXL_ERR;
    }

    size_t mime_len = (mime != NULL) ? axl_strlen(mime) + 1 : 0;
    if (len > SIZE_MAX - sizeof(ClipHeader)
        || mime_len > SIZE_MAX - sizeof(ClipHeader) - len) {
        return AXL_ERR;   /* image size would overflow */
    }
    size_t img_size = sizeof(ClipHeader) + len + mime_len;

    /* Build the new segment image in scratch memory (tracked alloc — fails
       cleanly under OOM, leaving the current clipboard untouched). */
    uint8_t *img = axl_malloc(img_size);
    if (img == NULL) {
        return AXL_ERR;
    }
    ClipHeader *h = (ClipHeader *)img;
    h->struct_size = (uint32_t)sizeof(ClipHeader);
    h->mime_len = (uint32_t)mime_len;
    h->data_len = (uint64_t)len;
    if (len > 0) {
        axl_memcpy(img + sizeof(ClipHeader), data, len);
    }
    if (mime_len > 0) {
        axl_memcpy(img + sizeof(ClipHeader) + len, mime, mime_len);
    }

    /* Snapshot the current clipboard so a failed swap can restore it. */
    size_t old_size = 0;
    void  *old = axl_shm_open(CLIP_SHM_NAME, 0, 0, &old_size);
    uint8_t *old_copy = NULL;
    if (old != NULL && old_size > 0) {
        old_copy = axl_malloc(old_size);
        if (old_copy == NULL) {
            axl_free(img);
            return AXL_ERR;   /* current clipboard untouched */
        }
        axl_memcpy(old_copy, old, old_size);
    }

    /* Swap: drop the old segment, create the new one, copy the image in. */
    (void)axl_shm_unlink(CLIP_SHM_NAME);   /* nodiscard: intentionally ignored */
    void *seg = axl_shm_open(CLIP_SHM_NAME, img_size, AXL_SHM_CREATE, NULL);
    if (seg == NULL) {
        /* Out of persistent memory — best-effort restore the old content. */
        if (old_copy != NULL) {
            void *r = axl_shm_open(CLIP_SHM_NAME, old_size, AXL_SHM_CREATE, NULL);
            if (r != NULL) {
                axl_memcpy(r, old_copy, old_size);
            }
        }
        axl_free(img);
        axl_free(old_copy);
        return AXL_ERR;
    }
    axl_memcpy(seg, img, img_size);
    axl_free(img);
    axl_free(old_copy);
    return AXL_OK;
}

const void *
axl_clipboard_get(size_t *out_len, const char **out_mime)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (out_mime != NULL) {
        *out_mime = NULL;
    }

    uint8_t *data = NULL;
    char    *mime = NULL;
    ClipHeader *h = clip_segment(&data, &mime);
    if (h == NULL) {
        return NULL;
    }
    if (out_len != NULL) {
        *out_len = (size_t)h->data_len;
    }
    if (out_mime != NULL) {
        *out_mime = mime;
    }
    return (h->data_len > 0) ? (const void *)data : NULL;
}

AxlBytes *
axl_clipboard_get_bytes(void)
{
    size_t      len = 0;
    const void *data = axl_clipboard_get(&len, NULL);

    if (data == NULL || len == 0) {
        return NULL;  // empty clipboard
    }
    // Snapshot copy: the resident segment can be unlinked by a later
    // set/clear, so the returned AxlBytes owns its own stable copy.
    return axl_bytes_new(data, len);
}

void
axl_clipboard_clear(void)
{
    (void)axl_shm_unlink(CLIP_SHM_NAME);   /* nodiscard: intentionally ignored */
}
