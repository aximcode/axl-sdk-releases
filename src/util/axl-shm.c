/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-shm.c:
 *
 * Boot-persistent named shared memory. See axl-shm.h.
 *
 * A segment is one pool allocation: a small ShmHeader (magic + size +
 * install handle) followed by the caller's byte region. The allocation
 * comes from axl_backend_alloc (raw EfiBootServicesData pool) — NOT
 * axl_malloc, whose leak tracker frees at image exit — so it survives the
 * creating image being unloaded. The header pointer is installed under a
 * GUID derived from the name (axl_guid_v5 over a fixed namespace), so any
 * later image resolves the same region via LocateProtocol. The header
 * carries its own install handle so axl_shm_unlink can uninstall it from
 * a different image than the one that created it.
 */

#include <axl/axl-shm.h>

#include <axl/axl-sys.h>     /* AxlGuid, axl_guid_v5, axl_protocol_*_guid, AXL_GUID */
#include <axl/axl-str.h>     /* axl_memset */
#include "../backend/axl-backend.h"

/* {6b2f8e1d-9c34-4a7b-bf52-1e8d3c6a9075} — AXL shared-memory name namespace. */
static const AxlGuid AXL_SHM_NAMESPACE = AXL_GUID(
    0x6b2f8e1d, 0x9c34, 0x4a7b,
    0xbf, 0x52, 0x1e, 0x8d, 0x3c, 0x6a, 0x90, 0x75);

#define AXL_SHM_MAGIC 0x4d485341u   /* "ASHM" — validates a located interface */

typedef struct {
    uint32_t magic;      ///< AXL_SHM_MAGIC
    uint32_t reserved;
    uint64_t size;       ///< payload byte count
    void    *handle;     ///< install handle, for axl_shm_unlink (cross-image)
} ShmHeader;

#define SHM_PAYLOAD(h) ((void *)((uint8_t *)(h) + sizeof(ShmHeader)))

static int
shm_guid(const char *name, AxlGuid *out)
{
    if (name == NULL) {
        return AXL_ERR;
    }
    return axl_guid_v5(&AXL_SHM_NAMESPACE, name, out);
}

static ShmHeader *
shm_locate(const AxlGuid *guid)
{
    void *iface = NULL;
    if (axl_protocol_find_guid(guid, &iface) != AXL_OK || iface == NULL) {
        return NULL;
    }
    ShmHeader *h = (ShmHeader *)iface;
    return (h->magic == AXL_SHM_MAGIC) ? h : NULL;
}

void *
axl_shm_open(const char *name, size_t size, uint32_t flags, size_t *out_size)
{
    AxlGuid guid;
    if (shm_guid(name, &guid) != AXL_OK) {
        return NULL;
    }

    ShmHeader *h = shm_locate(&guid);
    if (h != NULL) {
        if ((flags & (AXL_SHM_CREATE | AXL_SHM_EXCL))
            == (AXL_SHM_CREATE | AXL_SHM_EXCL)) {
            return NULL;     /* CREATE|EXCL and it already exists */
        }
        if (out_size != NULL) {
            *out_size = (size_t)h->size;
        }
        return SHM_PAYLOAD(h);
    }
    if (!(flags & AXL_SHM_CREATE)) {
        return NULL;         /* open-only and absent */
    }

    if (size > SIZE_MAX - sizeof(ShmHeader)) {
        return NULL;         /* size + header would overflow */
    }
    ShmHeader *nh = axl_backend_alloc(sizeof(ShmHeader) + size);
    if (nh == NULL) {
        return NULL;
    }
    nh->magic = AXL_SHM_MAGIC;
    nh->reserved = 0;
    nh->size = (uint64_t)size;
    nh->handle = NULL;
    axl_memset(SHM_PAYLOAD(nh), 0, size);
    if (axl_protocol_register_guid(&guid, nh, &nh->handle) != AXL_OK) {
        axl_backend_free(nh);
        return NULL;
    }
    if (out_size != NULL) {
        *out_size = size;
    }
    return SHM_PAYLOAD(nh);
}

int
axl_shm_unlink(const char *name)
{
    AxlGuid guid;
    if (shm_guid(name, &guid) != AXL_OK) {
        return AXL_ERR;
    }
    ShmHeader *h = shm_locate(&guid);
    if (h == NULL) {
        return AXL_OK;       /* already absent */
    }
    if (axl_protocol_unregister_guid(h->handle, &guid, h) != AXL_OK) {
        return AXL_ERR;
    }
    axl_backend_free(h);
    return AXL_OK;
}

bool
axl_shm_exists(const char *name, size_t *out_size)
{
    AxlGuid guid;
    if (shm_guid(name, &guid) != AXL_OK) {
        return false;
    }
    ShmHeader *h = shm_locate(&guid);
    if (h == NULL) {
        return false;
    }
    if (out_size != NULL) {
        *out_size = (size_t)h->size;
    }
    return true;
}
