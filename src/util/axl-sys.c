/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-sys.c
    System operations — reset, device mapping, firmware/memory info.
**/

#include "../backend/axl-backend.h"
#include "axl-protocol-internal.h"
#include <axl/axl-sys.h>
#include <axl/axl-mem-region.h>
#include <axl/axl-str.h>
#include <axl/axl-mem.h>
#include <axl/axl-fs.h>
#include <axl/axl-env.h>
#include <axl/axl-log.h>
#include <axl/axl-digest.h>

AXL_LOG_DOMAIN("sys");

// ---------------------------------------------------------------------------
// File-scope macros and types
// ---------------------------------------------------------------------------

/* Cap on node count for axl_device_path_for_each — guards against
   malformed firmware data that doesn't terminate. 64 nodes is well
   above any real device path the SDK has seen (typical UEFI paths
   max at ~6-12 nodes). */
#define AXL_DP_MAX_NODES  64u

/* Walk-callback context used by axl_device_path_find. */
typedef struct {
    uint8_t      type;
    uint8_t      subtype;
    const void  *match;
} DpFindCtx;

/* Walk-callback context used by axl_device_path_has_vendor. */
typedef struct {
    const AxlGuid *guid;
    bool           found;
} VendorMatchCtx;

/* EFI_DEVICE_PATH_TO_TEXT_PROTOCOL function-pointer subset — only
   ConvertDevicePathToText is used here, so the first slot is left
   as a generic void * placeholder rather than re-declaring the
   ConvertDeviceNodeToText prototype. */
#pragma pack(push, 1)
typedef struct {
    void *ConvertDeviceNodeToText;
    unsigned short *(EFIAPI *ConvertDevicePathToText)(
        const void *DevicePath,
        BOOLEAN     DisplayOnly,
        BOOLEAN     AllowShortcuts);
} DevicePathToTextProtocol;
#pragma pack(pop)

// ---------------------------------------------------------------------------
// File-scope variables
// ---------------------------------------------------------------------------

static const AxlGuid DEVICE_PATH_TO_TEXT_GUID = AXL_GUID(
    0x8b843e20, 0x8132, 0x4852,
    0x90, 0xcc, 0x55, 0x1a, 0x4e, 0x4a, 0x7f, 0x1c);

void
axl_reset(int type)
{
    EFI_RESET_TYPE reset_type;

    switch (type) {
    case AXL_RESET_WARM:
        reset_type = EfiResetWarm;
        break;
    case AXL_RESET_SHUTDOWN:
        reset_type = EfiResetShutdown;
        break;
    case AXL_RESET_COLD:
    default:
        reset_type = EfiResetCold;
        break;
    }

    axl_rt()->ResetSystem(reset_type, 0, 0, NULL);
}

int
axl_map_refresh(void)
{
    /* The shell's `map -r` regenerates the volume aliases — and on the old EFI
       1.x shell it also REWRITES the `path` environment variable (replacing the
       user's device-path-alias form with a fresh fsN form), silently clobbering
       a search path the user set. Snapshot `path` first (axl_getenv returns an
       OWNED copy, so it survives the rewrite — the backend's live GetEnv pointer
       would not) and restore it after, so a map refresh (e.g. mkrd's) never
       disturbs the user's environment. A no-op where map -r leaves path alone
       (restoring the same value) and best-effort if the restore can't be
       represented (path stays as map -r left it, never corrupted). */
    char *saved_path = axl_getenv("path");
    /* `map -r` reloads the map (the side effect we want) AND dumps the full
       device-mapping table (noise — a caller like mkrd already prints its own
       summary). On the modern shell EFI_SHELL_PROTOCOL.Execute runs a nested,
       off-console shell so the table never showed; on the old EFI 1.x shell
       SHELL_ENVIRONMENT.Execute runs in-context and the table WAS visible.
       Swallow the output at ConOut rather than with a `> nul` redirect: on the
       old shell a redirect pushes `map -r` into a sub-context, so its
       INTERACTIVE device-path-alias generation no longer reaches the parent and
       a later `map <label> fsN:` silently fails to resolve. The quiet variant
       runs the command verbatim and in-context, so the reload and its aliases
       are intact and only the listing is dropped — uniform on both shells. */
    int rc = axl_backend_shell_execute_quiet((const unsigned short *)L"map -r");
    if (saved_path != NULL) {
        axl_setenv("path", saved_path, true);
        axl_free(saved_path);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Handle protocol + stall
// ---------------------------------------------------------------------------

int
axl_handle_get_protocol(
    void        *handle,
    const char  *name,
    void       **interface
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;
    EFI_STATUS      status;

    if (handle == NULL || name == NULL || interface == NULL) {
        return AXL_ERR;
    }
    /* Defensive: ensure the caller sees NULL (not stale data) on
       any failure path. UEFI HandleProtocol does not guarantee
       *Interface is preserved on error — EDK2 zeroes it but other
       firmware may not. Callers like AxlVolume.device_path gate on
       NULL, so a leftover stale pointer would be a real footgun. */
    *interface = NULL;

    /* Reuse the protocol name→GUID lookup from axl-protocol.c. */
    guid = axl_protocol_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->HandleProtocol(
        (EFI_HANDLE)handle,
        (EFI_GUID *)guid,
        interface);

    if (EFI_ERROR(status)) {
        *interface = NULL;
        return AXL_ERR;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Firmware info
// ---------------------------------------------------------------------------

int
axl_sys_get_firmware_info(
    AxlFirmwareInfo *info
    )
{
    EFI_SYSTEM_TABLE *st;
    size_t i;

    if (info == NULL) {
        return AXL_ERR;
    }

    st = axl_st();
    if (st == NULL) {
        return AXL_ERR;
    }

    axl_memset(info, 0, sizeof(*info));

    /* Vendor name: UCS-2 → UTF-8 */
    if (st->FirmwareVendor != NULL) {
        unsigned short *w = st->FirmwareVendor;
        for (i = 0; i < sizeof(info->vendor) - 1 && w[i] != 0; i++) {
            info->vendor[i] = (char)(w[i] < 128 ? w[i] : '?');
        }
        info->vendor[i] = '\0';
    }

    info->firmware_revision = st->FirmwareRevision;

    /* EFI spec revision: high 16 bits = major, low 16 bits = minor*10 */
    info->spec_major = (uint16_t)(st->Hdr.Revision >> 16);
    info->spec_minor = (uint16_t)(st->Hdr.Revision & 0xFFFF);

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Memory size
// ---------------------------------------------------------------------------

int
axl_sys_get_memory_size(
    uint64_t *total_bytes
    )
{
    if (total_bytes == NULL) {
        return AXL_ERR;
    }

    /* Sum the usable-RAM regions from the shared physical region map
       (axl-mem-region) rather than walking the EFI memory map a second
       time. AXL_MEM_REGION_RAM covers the usable types this used to sum
       directly (Loader, BootServices, Conventional) plus any GCD system
       memory the EFI map omits — normally the same bytes. A unit test pins
       the total to an independent EFI-map walk on the test platform. */
    size_t count = 0;
    if (axl_mem_phys_region_count(&count) != AXL_OK) {
        return AXL_ERR;
    }

    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        AxlMemRegion region;
        if (axl_mem_phys_region_get(i, &region) != AXL_OK) {
            return AXL_ERR;
        }
        if (region.type == AXL_MEM_REGION_RAM) {
            total += region.len;
        }
    }

    *total_bytes = total;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Device-path iteration
// ---------------------------------------------------------------------------

int
axl_device_path_for_each(
    const void       *device_path,
    AxlDevicePathFn   fn,
    void             *user
    )
{
    if (device_path == NULL || fn == NULL) {
        return -1;
    }

    const EFI_DEVICE_PATH_PROTOCOL *dp =
        (const EFI_DEVICE_PATH_PROTOCOL *)device_path;

    for (unsigned steps = 0; steps < AXL_DP_MAX_NODES; steps++) {
        uint16_t node_len = (uint16_t)EFI_DP_LENGTH(dp);
        if (node_len < 4) {
            return -1;  /* malformed: each node header is 4 bytes */
        }
        if (EFI_DP_IS_END(dp)) {
            return 0;
        }
        int rc = fn((uint8_t)EFI_DP_TYPE(dp), (uint8_t)EFI_DP_SUBTYPE(dp),
                    dp, user);
        if (rc != 0) {
            return rc;
        }
        dp = EFI_DP_NEXT(dp);
    }
    return -1;  /* step cap exhausted — treat as malformed */
}

static int
dp_find_cb(uint8_t type, uint8_t subtype, const void *node, void *user)
{
    DpFindCtx *c = (DpFindCtx *)user;
    if (type == c->type && subtype == c->subtype) {
        c->match = node;
        return 1;  /* stop */
    }
    return 0;
}

const void *
axl_device_path_find(
    const void *device_path,
    uint8_t     type,
    uint8_t     subtype
    )
{
    DpFindCtx ctx = { .type = type, .subtype = subtype, .match = NULL };
    axl_device_path_for_each(device_path, dp_find_cb, &ctx);
    return ctx.match;
}

size_t
axl_device_path_size(const void *device_path)
{
    if (device_path == NULL) {
        return 0;
    }
    const EFI_DEVICE_PATH_PROTOCOL *dp =
        (const EFI_DEVICE_PATH_PROTOCOL *)device_path;
    const uint8_t *base = (const uint8_t *)dp;
    for (unsigned steps = 0; steps < AXL_DP_MAX_NODES; steps++) {
        uint16_t node_len = (uint16_t)EFI_DP_LENGTH(dp);
        if (node_len < 4) return 0;
        if (EFI_DP_IS_END(dp)) {
            return (size_t)((const uint8_t *)dp - base) + node_len;
        }
        dp = EFI_DP_NEXT(dp);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// axl_device_path_has_vendor
// ---------------------------------------------------------------------------

static int
vendor_match_cb(uint8_t type, uint8_t subtype, const void *node, void *user)
{
    if (type == HARDWARE_DEVICE_PATH && subtype == HW_VENDOR_DP) {
        VendorMatchCtx *c = (VendorMatchCtx *)user;
        const VENDOR_DEVICE_PATH *v = (const VENDOR_DEVICE_PATH *)node;
        if (axl_guid_equal((const AxlGuid *)&v->Guid, c->guid)) {
            c->found = true;
            return 1;  /* stop */
        }
    }
    return 0;
}

bool
axl_device_path_has_vendor(void *device_path, const AxlGuid *guid)
{
    if (device_path == NULL || guid == NULL) {
        return false;
    }
    VendorMatchCtx ctx = { .guid = guid, .found = false };
    axl_device_path_for_each(device_path, vendor_match_cb, &ctx);
    return ctx.found;
}

// ---------------------------------------------------------------------------
// axl_device_path_to_text
// ---------------------------------------------------------------------------

char *
axl_device_path_to_text(const void *device_path)
{
    if (device_path == NULL) {
        return NULL;
    }
    static DevicePathToTextProtocol *cached;
    if (cached == NULL) {
        void *p = NULL;
        if (axl_bs()->LocateProtocol(
                (EFI_GUID *)&DEVICE_PATH_TO_TEXT_GUID, NULL, &p) != EFI_SUCCESS
            || p == NULL)
        {
            return NULL;
        }
        cached = (DevicePathToTextProtocol *)p;
    }
    if (cached->ConvertDevicePathToText == NULL) {
        return NULL;
    }
    unsigned short *text = cached->ConvertDevicePathToText(
        device_path, FALSE, FALSE);
    if (text == NULL) {
        return NULL;
    }
    char *utf8 = axl_ucs2_to_utf8(text);
    axl_bs()->FreePool(text);
    return utf8;
}

// ---------------------------------------------------------------------------
// GUID derivation — name-based UUIDv5
// ---------------------------------------------------------------------------

int
axl_guid_v5(
    const AxlGuid *namespace_uuid,
    const char    *name,
    AxlGuid       *out
    )
{
    if (namespace_uuid == NULL || name == NULL || out == NULL) {
        return AXL_ERR;
    }

    /* Dogfood: AxlChecksum is the public SHA-1 entry point. The
       incremental form lets us hash namespace + name without
       allocating a temp concat buffer. */
    AxlChecksum *cs = axl_checksum_new(AXL_CHECKSUM_SHA1);
    if (cs == NULL) {
        return AXL_ERR;
    }
    axl_checksum_update(cs, namespace_uuid, sizeof(AxlGuid));
    axl_checksum_update(cs, name, axl_strlen(name));

    /* SHA-1 digest is 20 bytes; we keep the first 16 and overwrite
       the version + variant bits per RFC 4122 §4.3. axl_checksum_get_digest
       always sets *len to 20 for SHA-1 — no truncation/short-write
       case to defend against. */
    uint8_t digest[20];
    size_t  digest_len = sizeof(digest);
    axl_checksum_get_digest(cs, digest, &digest_len);
    axl_checksum_free(cs);

    /* RFC 4122 §4.3 sets the version (high nibble of byte 6) to 5
       and the variant (high two bits of byte 8) to 10b. We operate
       on the raw 16-byte image — the result is treated as opaque
       AxlGuid storage downstream, never read field-by-field as
       host-order ints, so endian is irrelevant for our derivation
       contract. */
    digest[6] = (uint8_t)((digest[6] & 0x0F) | 0x50);  /* version 5 */
    digest[8] = (uint8_t)((digest[8] & 0x3F) | 0x80);  /* variant 10b */

    axl_memcpy(out, digest, sizeof(AxlGuid));
    return AXL_OK;
}
