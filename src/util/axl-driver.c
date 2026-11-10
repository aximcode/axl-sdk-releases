/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-driver.c
    UEFI driver lifecycle — load, start, connect, disconnect, unload.

    Uses gBS->LoadImage/StartImage directly (not Shell "load" command)
    so load options can be set between load and start.
**/

#include "../backend/axl-backend.h"
#include "../runtime/axl-atexit-internal.h"  /* _axl_atexit_init / _run_all */
#include "../runtime/axl-cxxabi-internal.h"  /* _axl_cxxabi_run_init_array */
#include "../runtime/axl-libc-heap-internal.h" /* _axl_libc_heap_release */
#include "../mem/axl-mem-internal.h"     /* _axl_mem_quarantine_drain */
#include "../runtime/axl-runtime-internal.h" /* _axl_default_loop_free */
#include "axl-image-internal.h"   /* _axl_init_image_path */
#include "axl-driver-internal.h"  /* _axl_driver_ensure_with_embedded_info */
#include "../pack/axl-pack-format.h" /* packed images: find + unpack */
#include "../fs/axl-fs-read-chunk.h"  /* AXL_FS_READ_CHUNK */
#include "../fs/axl-fs-internal.h"    /* axl_fs_path_canonical: file identity */
#include <axl/axl-driver.h>
#include <axl/axl-app.h>          /* axl_app_image_path (sibling resolution) */
#include <axl/axl-env.h>          /* axl_getenv (path-searched-launch fallback) */
#include <axl/axl-efi-status.h>
#include <axl/axl-mem.h>
#include <axl/axl-atexit.h>   /* binding teardown: app-exit safety-net hook */
#include <axl/axl-str.h>
#include <axl/axl-path.h>
#include <axl/axl-log.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-sys.h>

AXL_LOG_DOMAIN("driver");

/* Defined with the load-options table below; axl_driver_start needs it first. */
static void load_options_retire(AxlDriverHandle handle);

/* AxlEfiStatus is declared in <axl/axl-efi-status.h> as a 64-bit
   alias for EFI_STATUS so consumers writing UEFI-spec-protocol
   methods can return spec-mandated values without pulling
   <uefi/axl-uefi.h>. The contract is that the two are
   value-for-value swappable; this is the only TU that includes
   both, so it's the natural home for the build-time check. */
_Static_assert(sizeof(AxlEfiStatus) == sizeof(EFI_STATUS),
               "AxlEfiStatus must be ABI-compatible with EFI_STATUS");
_Static_assert((AxlEfiStatus)AXL_EFI_SUCCESS == (AxlEfiStatus)EFI_SUCCESS,
               "AXL_EFI_SUCCESS must equal EFI_SUCCESS");
_Static_assert((AxlEfiStatus)AXL_EFI_NOT_FOUND == (AxlEfiStatus)EFI_NOT_FOUND,
               "AXL_EFI_NOT_FOUND must equal EFI_NOT_FOUND");
_Static_assert((AxlEfiStatus)AXL_EFI_INVALID_PARAMETER == (AxlEfiStatus)EFI_INVALID_PARAMETER,
               "AXL_EFI_INVALID_PARAMETER must equal EFI_INVALID_PARAMETER");
_Static_assert((AxlEfiStatus)AXL_EFI_UNSUPPORTED == (AxlEfiStatus)EFI_UNSUPPORTED,
               "AXL_EFI_UNSUPPORTED must equal EFI_UNSUPPORTED");
_Static_assert((AxlEfiStatus)AXL_EFI_OUT_OF_RESOURCES == (AxlEfiStatus)EFI_OUT_OF_RESOURCES,
               "AXL_EFI_OUT_OF_RESOURCES must equal EFI_OUT_OF_RESOURCES");
_Static_assert((AxlEfiStatus)AXL_EFI_BUFFER_TOO_SMALL == (AxlEfiStatus)EFI_BUFFER_TOO_SMALL,
               "AXL_EFI_BUFFER_TOO_SMALL must equal EFI_BUFFER_TOO_SMALL");
_Static_assert((AxlEfiStatus)AXL_EFI_DEVICE_ERROR == (AxlEfiStatus)EFI_DEVICE_ERROR,
               "AXL_EFI_DEVICE_ERROR must equal EFI_DEVICE_ERROR");
_Static_assert((AxlEfiStatus)AXL_EFI_END_OF_FILE == (AxlEfiStatus)EFI_END_OF_FILE,
               "AXL_EFI_END_OF_FILE must equal EFI_END_OF_FILE");

// ---------------------------------------------------------------------------
// Volume-name buffer (matches DRIVER_MAX_VOLUMES so axl_volume_enumerate
// returns the same set we can address).
// ---------------------------------------------------------------------------

#define DRIVER_VOL_NAME_MAX      16
#define DRIVER_MAX_CANDIDATES    16
#define DRIVER_MAX_VOLUMES       16
#define DRIVER_PATH_BUF          256
#define DRIVER_SUB_BUF           192

#define DRIVER_DP_END_TYPE       0x7Fu
#define DRIVER_DP_MEDIA_TYPE     0x04u
#define DRIVER_DP_FILEPATH_SUB   0x04u
#define DRIVER_DP_NODE_HDR       4u

#if defined(__x86_64__)
static const char driver_arch[] = "x64";
#elif defined(__aarch64__)
static const char driver_arch[] = "aa64";
#else
#error "axl-driver: unsupported architecture"
#endif

typedef struct {
    const char       *pattern;
    const AxlAttempt *guard;   /* NULL = unprotected (the legacy contract) */
    size_t            loaded;
} DriverLoadCtx;

/* Outcome of loading an image named by a device path. REFUSED is final: AXL
   sized the file and then could not read it, or read it and the image was
   refused -- a packed one that could not be unpacked, or bytes LoadImage
   rejected. A caller must not retry by another route: for a packed file that
   could load the stub, and for a raw one the routes left are a buffer load
   of the same bytes (the same verdict) or the firmware's own large read of
   the file, the very read the loader avoids. FAILED means AXL could not
   open or size the file and the firmware's load from the path failed too;
   fallbacks stay open. */
typedef enum {
    DRIVER_DP_LOADED,
    DRIVER_DP_FAILED,
    DRIVER_DP_REFUSED,
} DriverDpResult;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/* Given a volume's own device path and a file path string ("\dir\name.efi",
 * either slash orientation), build a full file-on-volume device path: the
 * volume DP with its END node stripped, one MEDIA_FILEPATH node for the
 * (UCS-2, backslash-separated) file path, then a fresh END node. Suitable
 * for gBS->LoadImage's DevicePath argument. AllocatePool'd (caller frees
 * with gBS->FreePool), or NULL on any failure. Shared by driver_build_file_dp
 * (volume resolved from an "fsN" name) and driver_build_sibling_dp (volume
 * taken straight from the launcher's own LoadedImage->DeviceHandle). */
static EFI_DEVICE_PATH_PROTOCOL *
driver_append_file_to_vol_dp(
    EFI_DEVICE_PATH_PROTOCOL *vol_dp,
    const char               *file_part
    )
{
    if (vol_dp == NULL || file_part == NULL || *file_part == '\0') {
        return NULL;
    }
    size_t vol_dp_size = axl_device_path_size(vol_dp);
    if (vol_dp_size < 4) return NULL;
    /* Strip the volume DP's END node — we'll append our own. */
    size_t vol_dp_body = vol_dp_size - 4;

    /* Convert the file portion to UCS-2. UEFI file-path nodes store
     * the path in UCS-2, with backslash separators, NUL-terminated. */
    AXL_AUTO_FREE unsigned short *file_w = axl_utf8_to_ucs2(file_part);
    if (file_w == NULL) return NULL;
    size_t file_wlen = 0;
    while (file_w[file_wlen] != 0) {
        /* Normalize forward slashes to backslashes. */
        if (file_w[file_wlen] == (unsigned short)'/') {
            file_w[file_wlen] = (unsigned short)'\\';
        }
        file_wlen++;
    }
    /* Filepath node: 4-byte header + (wlen+1)*2 bytes of UCS-2 string.
     * Length must fit in uint16_t. */
    size_t file_node_size = 4 + (file_wlen + 1) * 2;
    if (file_node_size > 0xFFFF) return NULL;

    /* Total: stripped volume body + file node + 4-byte END node. */
    size_t total = vol_dp_body + file_node_size + 4;

    void *out = NULL;
    if (axl_bs()->AllocatePool(EfiBootServicesData, total, &out)  /* axl-pool-direct: device path for LoadImage */
        != EFI_SUCCESS || out == NULL)
    {
        return NULL;
    }
    uint8_t *p = (uint8_t *)out;
    axl_memcpy(p, vol_dp, vol_dp_body);
    p += vol_dp_body;

    /* MEDIA_FILEPATH_DP node */
    p[0] = 0x04;                                    /* MEDIA_DEVICE_PATH */
    p[1] = 0x04;                                    /* MEDIA_FILEPATH_DP */
    p[2] = (uint8_t)(file_node_size & 0xff);
    p[3] = (uint8_t)((file_node_size >> 8) & 0xff);
    axl_memcpy(p + 4, file_w, (file_wlen + 1) * 2);
    p += file_node_size;

    /* END node */
    p[0] = 0x7f; p[1] = 0xff; p[2] = 4; p[3] = 0;

    return (EFI_DEVICE_PATH_PROTOCOL *)out;
}

/* Build a full file-on-volume device path from a UEFI path string
 * like "fs0:\drivers\x64\foo.efi". The result is suitable for
 * gBS->LoadImage's DevicePath argument — the firmware will then set
 * LoadedImage->FilePath correctly, which iPXE and similar drivers
 * read at StartImage to locate themselves on disk.
 *
 * Returns a gBS->AllocatePool-allocated device path on success
 * (caller frees with gBS->FreePool), or NULL on any failure
 * (volume not mounted, malformed path, OOM, etc.).
 *
 * Failure here is non-fatal — axl_driver_load falls back to the
 * memory-buffer load path, which works for drivers that don't
 * require FilePath. */
/* Free a device path built for LoadImage (driver_build_file_dp and kin). */
static void
driver_free_dp(EFI_DEVICE_PATH_PROTOCOL *dp)
{
    axl_bs()->FreePool(dp);  /* axl-pool-direct: device path for LoadImage */
}

static EFI_DEVICE_PATH_PROTOCOL *
driver_build_file_dp(const char *path)
{
    /* Split "fsN:\..." into volume-name and file-portion. */
    const char *colon = axl_strchr(path, ':');
    if (colon == NULL || colon == path) return NULL;
    size_t name_len = (size_t)(colon - path);
    if (name_len >= DRIVER_VOL_NAME_MAX) return NULL;

    char vol_name[DRIVER_VOL_NAME_MAX];
    axl_memcpy(vol_name, path, name_len);
    vol_name[name_len] = '\0';

    const char *file_part = colon + 1;
    /* MEDIA_FILEPATH_DP wants a leading backslash; tolerate either
     * orientation in the input. */
    if (*file_part == '\0') return NULL;

    /* Find the volume handle by name. */
    AxlVolume volumes[DRIVER_VOL_NAME_MAX];
    size_t    n_vols = 0;
    if (axl_volume_enumerate(volumes, DRIVER_VOL_NAME_MAX, &n_vols) != AXL_OK
        || n_vols == 0)
    {
        return NULL;
    }
    EFI_HANDLE vol_handle = NULL;
    for (size_t i = 0; i < n_vols; i++) {
        /* Case-insensitive: UEFI shell fs aliases are case-insensitive.
           volumes[i].name is the lowercased shell alias ("fs0"), but
           `vol_name` comes from a caller path — a user's is usually lowercase
           ("fs0:") while axl_app_image_path (the source for
           axl_driver_load_sibling) carries the shell's own case ("FS0:"), so an
           exact match would miss that path. */
        if (axl_strcasecmp(volumes[i].name, vol_name) == 0) {
            vol_handle = (EFI_HANDLE)volumes[i].handle;
            break;
        }
    }
    if (vol_handle == NULL) {
        /* Not an fsN alias: any other name the shell maps (one set by
           axl_volume_set_map or `map`) names the volume whose device path
           it was set for. Without this, such a path fell to the buffer load
           and its image lost its FilePath. */
        char map[DRIVER_VOL_NAME_MAX + 1];
        axl_memcpy(map, vol_name, name_len);
        map[name_len]     = ':';
        map[name_len + 1] = '\0';
        AXL_AUTO_FREE unsigned short *mapw = axl_utf8_to_ucs2(map);
        for (size_t i = 0; mapw != NULL && i < n_vols; i++) {
            if (axl_backend_shell_map_is(mapw, volumes[i].device_path)) {
                vol_handle = (EFI_HANDLE)volumes[i].handle;
                break;
            }
        }
    }
    if (vol_handle == NULL) return NULL;

    /* Get the volume's device path. */
    EFI_DEVICE_PATH_PROTOCOL *vol_dp = NULL;
    if (axl_bs()->HandleProtocol(vol_handle,
                                 &EFI_DEVICE_PATH_PROTOCOL_GUID,
                                 (void **)&vol_dp) != EFI_SUCCESS
        || vol_dp == NULL)
    {
        return NULL;
    }

    return driver_append_file_to_vol_dp(vol_dp, file_part);
}

/* Build the sibling driver's device path straight from LoadedImage — no
 * shell. Walks gImageHandle up the ParentHandle chain (bounded to 8, to
 * cover a buffer-loaded launcher whose own FilePath is NULL) to the first
 * image with a FilePath, then rebuilds "<launcher-dir>\<sibling_name>" on
 * that image's own DeviceHandle (its volume). Because BOTH the directory
 * and the volume come from the launcher's LoadedImage, the result is
 * strictly beside the launcher the firmware actually ran — the
 * sibling-only / version-pinning contract holds with no EFI_SHELL_PROTOCOL,
 * so this resolves on the modern EDK2 shell, the old EFI 1.x shell, and BDS
 * alike. It also covers a path-searched launch WHEN the firmware recorded a
 * resolved FilePath; a shell that leaves FilePath a bare command name yields
 * a volume-root path here that LoadImage rejects, and the caller's
 * shell-`path` fallback recovers the real directory. AllocatePool'd (caller
 * frees with gBS->FreePool), or NULL on any failure. */
static EFI_DEVICE_PATH_PROTOCOL *
driver_build_sibling_dp(const char *sibling_name)
{
    EFI_HANDLE                 cur = gImageHandle;
    EFI_LOADED_IMAGE_PROTOCOL *li  = NULL;
    for (int depth = 0; depth < 8 && cur != NULL; depth++) {
        EFI_LOADED_IMAGE_PROTOCOL *cand = NULL;
        if (axl_bs()->HandleProtocol(cur, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                     (void **)&cand) != EFI_SUCCESS
            || cand == NULL)
        {
            return NULL;
        }
        if (cand->FilePath != NULL) {
            li = cand;
            break;
        }
        cur = (EFI_HANDLE)cand->ParentHandle;
    }
    if (li == NULL || li->DeviceHandle == NULL) {
        return NULL;
    }

    /* The launcher's own path on its volume, e.g. "\EFI\Tools\do.efi". */
    AXL_AUTO_FREE char *self = _axl_decode_image_filepath(
        (EFI_DEVICE_PATH_PROTOCOL *)li->FilePath);
    if (self == NULL) return NULL;

    /* Directory = everything up to and including the last separator. When
     * the launcher path carries no separator (a cwd-relative bare name),
     * synthesize a leading backslash so the FILEPATH node is
     * volume-absolute rather than a stray relative name. */
    size_t self_len = axl_strlen(self);
    size_t dir_len  = 0;
    for (size_t i = 0; i < self_len; i++) {
        if (self[i] == '\\' || self[i] == '/') {
            dir_len = i + 1;
        }
    }
    bool   need_lead = (dir_len == 0);
    size_t sib_len   = axl_strlen(sibling_name);
    AXL_AUTO_FREE char *full =
        axl_malloc((need_lead ? 1 : 0) + dir_len + sib_len + 1);
    if (full == NULL) return NULL;
    size_t k = 0;
    if (need_lead) {
        full[k++] = '\\';
    }
    axl_memcpy(full + k, self, dir_len);
    k += dir_len;
    axl_memcpy(full + k, sibling_name, sib_len + 1);

    EFI_DEVICE_PATH_PROTOCOL *vol_dp = NULL;
    if (axl_bs()->HandleProtocol(li->DeviceHandle,
                                 &EFI_DEVICE_PATH_PROTOCOL_GUID,
                                 (void **)&vol_dp) != EFI_SUCCESS
        || vol_dp == NULL)
    {
        return NULL;
    }
    return driver_append_file_to_vol_dp(vol_dp, full);
}

/* --------------------------------------------------------------------------
 * Embedded-image device-path synthesis
 *
 * A buffer load (gBS->LoadImage with DevicePath=NULL) leaves the image's
 * LoadedImage->FilePath and gEfiLoadedImageDevicePathProtocol interface
 * NULL. The aarch64 UEFI shell faults rendering that NULL path under `dh`.
 * After the load we synthesize
 *   [Vendor(guid)] MemoryMapped(code-type, base, base+size) FilePath("\name")
 * install it as the loaded-image device path, and point LoadedImage->FilePath
 * at the file portion. Nodes are AllocatePool'd so they outlive the loading
 * app. Best-effort: any failure leaves the driver loaded with firmware
 * defaults (only `dh -v` cosmetics degrade).
 * --------------------------------------------------------------------------
 */

/* Last path component of @p path (after any '/', '\\' or ':'). */
static const char *
driver_basename(const char *path)
{
    const char *base = path;
    for (const char *c = path; *c != '\0'; c++) {
        if (*c == '\\' || *c == '/' || *c == ':') {
            base = c + 1;
        }
    }
    return base;
}

/* Build the full device path for a memory-loaded image:
 *   [Vendor(guid)] MemoryMapped(mem_type, base, base+size-1) FilePath("\name") END
 * AllocatePool'd (caller frees with gBS->FreePool), or NULL on failure. On
 * success *filepath_offset receives the byte offset of the trailing
 * MEDIA_FILEPATH node, so LoadedImage->FilePath can point at that sub-path
 * without a second allocation. */
static EFI_DEVICE_PATH_PROTOCOL *
driver_build_image_dp(
    const AxlGuid *vendor,        /* may be NULL */
    uint32_t       mem_type,
    uint64_t       base,
    uint64_t       size,
    const char    *file_name,
    size_t        *filepath_offset
    )
{
    /* MEDIA_FILEPATH wants UCS-2 "\name" with backslash separators. */
    size_t name_len = axl_strlen(file_name);
    AXL_AUTO_FREE char *with_slash = axl_malloc(name_len + 2);
    if (with_slash == NULL) return NULL;
    size_t k = 0;
    if (file_name[0] != '\\' && file_name[0] != '/') {
        with_slash[k++] = '\\';
    }
    for (size_t i = 0; i < name_len; i++) {
        with_slash[k++] = (file_name[i] == '/') ? '\\' : file_name[i];
    }
    with_slash[k] = '\0';

    AXL_AUTO_FREE unsigned short *file_w = axl_utf8_to_ucs2(with_slash);
    if (file_w == NULL) return NULL;
    size_t file_wlen = 0;
    while (file_w[file_wlen] != 0) {
        file_wlen++;
    }
    size_t fp_node = 4 + (file_wlen + 1) * 2;
    if (fp_node > 0xFFFF) return NULL;

    size_t vendor_node = (vendor != NULL) ? (4 + 16) : 0;
    size_t mm_node     = 24;
    size_t total       = vendor_node + mm_node + fp_node + 4 /* END */;

    void *out = NULL;
    if (axl_bs()->AllocatePool(EfiBootServicesData, total, &out) != EFI_SUCCESS  /* axl-pool-direct: installed loaded-image device path */
        || out == NULL)
    {
        return NULL;
    }
    uint8_t *p = (uint8_t *)out;

    if (vendor_node != 0) {
        /* HW_VENDOR_DP: HARDWARE_DEVICE_PATH(0x01) / 0x04, 16-byte GUID body. */
        p[0] = 0x01; p[1] = 0x04;
        p[2] = (uint8_t)(vendor_node & 0xff);
        p[3] = (uint8_t)((vendor_node >> 8) & 0xff);
        axl_memcpy(p + 4, vendor, 16);   /* AxlGuid is EFI_GUID byte layout */
        p += vendor_node;
    }
    /* HW_MEMMAP_DP: HARDWARE_DEVICE_PATH(0x01) / 0x03, len 24. EndingAddress
     * is the INCLUSIVE last byte (EDK2 convention), so base + size - 1; a
     * size of 0 keeps end == base so it never underflows below the start. */
    p[0] = 0x01; p[1] = 0x03;
    p[2] = 24; p[3] = 0;
    uint32_t mt  = mem_type;
    uint64_t end = (size > 0) ? (base + size - 1) : base;
    axl_memcpy(p + 4,  &mt,   4);
    axl_memcpy(p + 8,  &base, 8);
    axl_memcpy(p + 16, &end,  8);
    p += mm_node;

    /* MEDIA_FILEPATH_DP node — record its offset for LoadedImage->FilePath. */
    *filepath_offset = (size_t)(p - (uint8_t *)out);
    p[0] = 0x04; p[1] = 0x04;
    p[2] = (uint8_t)(fp_node & 0xff);
    p[3] = (uint8_t)((fp_node >> 8) & 0xff);
    axl_memcpy(p + 4, file_w, (file_wlen + 1) * 2);
    p += fp_node;

    /* END node */
    p[0] = 0x7f; p[1] = 0xff; p[2] = 4; p[3] = 0;

    return (EFI_DEVICE_PATH_PROTOCOL *)out;
}

/* Cross-image cleanup record for a synthesized loaded-image device path.
 *
 * A buffer/embedded load has no firmware device path, so
 * driver_apply_image_identity synthesizes one, installs it as the handle's
 * EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL, and points LoadedImage->FilePath at a
 * standalone copy. The firmware's own unload matches its (NULL) internal
 * device-path pointer, not our interface, so it never removes ours: left
 * there, the handle would survive carrying only the synth device path (it
 * accumulates in `dh` on every load/unload cycle). axl_driver_unload removes
 * it AFTER a successful UnloadImage (the firmware has then freed FilePath, our
 * copy), and leaves it in place when the unload is refused, so a resident
 * image keeps its identity.
 *
 * The record lives ON the driver's image handle as a private protocol, NOT in
 * a process-local table, because the shared-driver launcher pattern loads the
 * driver in one (transient) image and unloads it from another: a per-image
 * table in the loader is gone by unload time, so image_dp_release would find
 * nothing and leak. Storing the record on the handle lets whichever image
 * unloads the driver find and release it. The record and the device paths it
 * owns are all firmware-pool allocations (AllocatePool), so they outlive the
 * loader's exit.
 *
 * {d28ca292-1eb1-4972-b6d2-669f4346a6b2} — private to axl-sdk. */
static const EFI_GUID AXL_IMAGE_DP_RECORD_GUID = {
    0xd28ca292, 0x1eb1, 0x4972,
    {0xb6, 0xd2, 0x66, 0x9f, 0x43, 0x46, 0xa6, 0xb2}
};

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL *dp;        /* installed loaded-image device path */
    EFI_DEVICE_PATH_PROTOCOL *file_path; /* standalone copy set as li->FilePath */
} AxlImageDpRecord;

/* Uninstall the synthesized LoadedImageDevicePath from @p handle and free the
 * device path + the standalone FilePath copy (@p file_path NULL: the firmware
 * already freed it), returning the handle to its pre-synthesis state. Shared by
 * image_dp_release (after an unload, or for an image the firmware unloaded
 * itself) and image_dp_track's failure rollback (a live image). For a live
 * image li->FilePath is cleared before the copy is freed, so nothing is left
 * pointing at it. */
static void
image_dp_teardown(EFI_HANDLE handle, EFI_DEVICE_PATH_PROTOCOL *dp,
                  EFI_DEVICE_PATH_PROTOCOL *file_path)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (axl_bs()->HandleProtocol(handle, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                 (void **)&li) == EFI_SUCCESS && li != NULL) {
        li->FilePath = NULL;
    }
    /* Freed only once it is no longer installed: an interface the uninstall
       refused (one still open) would otherwise point at freed pool. */
    if (axl_bs()->UninstallProtocolInterface(
            handle, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID, dp) == EFI_SUCCESS) {
        axl_bs()->FreePool(dp);  /* axl-pool-direct: installed loaded-image device path */
    }
    if (file_path != NULL) {
        axl_bs()->FreePool(file_path);  /* axl-pool-direct: LoadedImage FilePath copy */
    }
}

static void
image_dp_track(EFI_HANDLE handle, EFI_DEVICE_PATH_PROTOCOL *dp,
               EFI_DEVICE_PATH_PROTOCOL *file_path)
{
    AxlImageDpRecord *rec = NULL;
    if (axl_bs()->AllocatePool(EfiBootServicesData, sizeof(*rec),  /* axl-pool-direct: record installed as handle protocol */
                               (void **)&rec) != EFI_SUCCESS || rec == NULL) {
        rec = NULL;   /* AllocatePool may leave *rec untouched on failure */
    } else {
        rec->dp        = dp;
        rec->file_path = file_path;
        if (EFI_ERROR(axl_bs()->InstallProtocolInterface(
                &handle, (EFI_GUID *)&AXL_IMAGE_DP_RECORD_GUID,
                EFI_NATIVE_INTERFACE, rec))) {
            axl_bs()->FreePool(rec);  /* axl-pool-direct: record installed as handle protocol */
            rec = NULL;
        }
    }

    /* Could not record the cleanup (OOM / install failure). Rather than leave
     * an untracked synthesized device path installed — which would orphan the
     * handle on unload, the very leak this record prevents — roll the identity
     * back to firmware defaults. Best-effort: the handle simply loses its
     * synthesized device path (only `dh -v` cosmetics degrade). */
    if (rec == NULL) {
        axl_warning("driver load: cannot record image-dp cleanup on handle "
                    "0x%llx; rolling back synthesized device path",
                    (unsigned long long)(uintptr_t)handle);
        image_dp_teardown(handle, dp, file_path);
    }
}

/* Uninstall + free the synthesized device path for @p handle, if any, and
 * report whether the image is still loaded. Called from axl_driver_unload
 * AFTER a successful UnloadImage (the firmware never removes our interface:
 * its own record of the device path is NULL for a buffer load), and for an
 * image the firmware already unloaded -- never before an UnloadImage that
 * could be refused, which would strand a resident image without its identity.
 * Works from any image — the record is read off the handle, not a
 * process-local table.
 *
 * Liveness matters because the firmware auto-unloads some images out from
 * under us: EDK2's CoreStartImage unloads an image whose DriverEntry returned
 * an error (and unloads any application unconditionally), via
 * CoreUnloadAndCloseImage, which CoreFreePool's Image->Info.FilePath and
 * Image->LoadedImageDevicePath — the exact pool blocks our record tracks — and
 * uninstalls the LoadedImage protocols. The HANDLE survives only because our
 * private record protocol is still on it. So "the record is still here" is NOT
 * proof the image is live; freeing rec->dp / rec->file_path again would be a
 * double FreePool that corrupts the DXE pool (silent 100%-CPU wedge on RELEASE,
 * a Pool.c signature ASSERT on DEBUG). Probe the LoadedImage protocol — which
 * the firmware DID uninstall — and only tear down the device paths when the
 * image is still alive. The probe is safe: this handle is still in the DB (our
 * record keeps it there), and even a destroyed handle would only be compared
 * by value in CoreValidateHandle, never dereferenced.
 *
 * Returns true if the caller should go on to UnloadImage the handle, false if
 * there is nothing left to unload — either the firmware already unloaded the
 * image, or the handle is not in the handle database at all (a stale or bogus
 * handle is indistinguishable from an auto-unloaded one: both are simply gone).
 * The EFI_INVALID_PARAMETER / EFI_UNSUPPORTED split used below to tell "gone"
 * from "live but not an image" is an EDK2 property (CoreValidateHandle runs
 * before the protocol lookup); firmware that reported EFI_UNSUPPORTED for a
 * destroyed handle would just fall through to UnloadImage and get an error
 * back, which is a safe degradation. */
static bool
image_dp_release(AxlDriverHandle handle)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS li_status = axl_efi_call(axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li);
    bool image_alive = li_status == EFI_SUCCESS && li != NULL;

    AxlImageDpRecord *rec = NULL;
    if (axl_bs()->HandleProtocol((EFI_HANDLE)handle,
                                 (EFI_GUID *)&AXL_IMAGE_DP_RECORD_GUID,
                                 (void **)&rec) != EFI_SUCCESS || rec == NULL) {
        /* No synthesized device path to release — and no evidence this handle
         * was ever an image WE loaded, so "no LoadedImage" is ambiguous here
         * and only one of its two meanings may skip UnloadImage:
         *   - the handle itself is gone (EFI_INVALID_PARAMETER out of
         *     CoreValidateHandle): the firmware auto-unloaded an image nothing
         *     was keeping alive, and there is nothing left to unload;
         *   - the handle is live but carries no LoadedImage (EFI_UNSUPPORTED):
         *     it is not an image at all. Let UnloadImage run and report the
         *     real failure — claiming success for an unload we never performed
         *     would hide a caller's bad handle.
         * (The auto-unloaded-but-still-alive handle, the double-FreePool case
         * this liveness probe exists for, always carries a record and is
         * handled below.) */
        return image_alive || li_status != EFI_INVALID_PARAMETER;
    }

    /* Uninstall our private record unconditionally — that is what lets an
     * already-orphaned handle finally be destroyed. The GUID is private and
     * never opened via OpenProtocol, so this cannot be denied; freeing rec
     * below is therefore safe. */
    axl_bs()->UninstallProtocolInterface(
        (EFI_HANDLE)handle, (EFI_GUID *)&AXL_IMAGE_DP_RECORD_GUID, rec);

    if (image_alive) {
        /* Normal path: firmware never touched either block — full teardown. */
        image_dp_teardown((EFI_HANDLE)handle, rec->dp, rec->file_path);
    } else {
        /* The firmware auto-unloaded the image. It freed Image->Info.FilePath
         * (== rec->file_path) inside CoreUnloadAndCloseImage, so freeing that
         * again would be the double-free this whole guard exists to prevent —
         * pass NULL to skip it. But it did NOT touch rec->dp: our synthesized
         * LoadedImageDevicePath was installed with (Re)InstallProtocolInterface,
         * which does not update the firmware's private Image->LoadedImageDevicePath
         * field (NULL for a buffer load), so CoreUnloadAndCloseImage's
         * `if (LoadedImageDevicePath != NULL)` guard skipped BOTH its uninstall
         * and its free. So we must still uninstall + free rec->dp here, or it —
         * and the orphaned handle it keeps alive — leak on every failed start,
         * the exact leak this record was built to prevent. teardown's own
         * li->FilePath=NULL write no-ops safely (LoadedImage is already gone). */
        image_dp_teardown((EFI_HANDLE)handle, rec->dp, NULL);
    }
    axl_bs()->FreePool(rec);  /* axl-pool-direct: record installed as handle protocol */
    return image_alive;
}

/* Give a freshly buffer-loaded image a real identity so the shell never
 * renders a NULL device path. @p info may be NULL; with no file_name in it
 * the leaf is the nearest file-loaded ancestor's basename. */
static void
driver_apply_image_identity(
    EFI_HANDLE                  drv_handle,
    const AxlEmbeddedImageInfo *info
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (axl_bs()->HandleProtocol(drv_handle, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                 (void **)&li) != EFI_SUCCESS || li == NULL)
    {
        axl_warning("driver load: cannot read LoadedImage to set identity");
        return;
    }

    const char *file_name = (info != NULL) ? info->file_name : NULL;
    if (file_name == NULL || file_name[0] == '\0') {
        /* Anchor, not axl_app_image_path: the loader may itself be a
           buffer-loaded image with no file of its own, and any ancestor
           basename beats the "driver.efi" placeholder below. */
        const char *ip = _axl_app_image_anchor();
        const char *bn = (ip != NULL) ? driver_basename(ip) : NULL;
        file_name = (bn != NULL && bn[0] != '\0') ? bn : "driver.efi";
    }

    const AxlGuid *vendor = (info != NULL) ? info->vendor_guid : NULL;
    uint64_t       base   = (uint64_t)(uintptr_t)li->ImageBase;
    uint64_t       size   = li->ImageSize;

    size_t fp_off = 0;
    EFI_DEVICE_PATH_PROTOCOL *full =
        driver_build_image_dp(vendor, (uint32_t)li->ImageCodeType,
                              base, size, file_name, &fp_off);
    if (full == NULL) {
        axl_warning("driver load: device-path synthesis failed (out of memory)");
        return;
    }

    /* The firmware installs the loaded-image-device-path protocol with a
     * NULL interface for a buffer load; replace it (else install fresh). */
    void      *existing = NULL;
    EFI_STATUS hs = axl_bs()->HandleProtocol(
        drv_handle, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID, &existing);
    EFI_STATUS is;
    if (hs == EFI_SUCCESS) {
        is = axl_bs()->ReinstallProtocolInterface(
            drv_handle, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
            existing, full);
    } else {
        is = axl_bs()->InstallProtocolInterface(
            &drv_handle, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
            EFI_NATIVE_INTERFACE, full);
    }
    if (EFI_ERROR(is)) {
        axl_bs()->FreePool(full);  /* axl-pool-direct: installed loaded-image device path */
        axl_warning("driver load: install loaded-image device path failed: 0x%llx",
                    (unsigned long long)is);
        return;
    }

    /* LoadedImage->FilePath must be its OWN pool block: the firmware may
     * FreePool it during UnloadImage, and a pointer into the middle of `full`
     * (full + fp_off) would corrupt the pool allocator (observed as a hang
     * unloading a started service driver). Duplicate the MEDIA_FILEPATH tail
     * into a standalone allocation. Best-effort: if it fails, leave FilePath
     * NULL — the device-path protocol above is what fixes the aa64 `dh -v`
     * fault; FilePath is secondary. */
    EFI_DEVICE_PATH_PROTOCOL *tail =
        (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)full + fp_off);
    size_t tail_size = axl_device_path_size(tail);
    EFI_DEVICE_PATH_PROTOCOL *file_path = NULL;
    void                     *fp_buf    = NULL;
    if (tail_size >= 4
        && axl_bs()->AllocatePool(EfiBootServicesData, tail_size, &fp_buf)  /* axl-pool-direct: LoadedImage FilePath copy (firmware may free) */
               == EFI_SUCCESS
        && fp_buf != NULL)
    {
        axl_memcpy(fp_buf, tail, tail_size);
        file_path    = (EFI_DEVICE_PATH_PROTOCOL *)fp_buf;
        li->FilePath = file_path;
    }
    if (info != NULL && info->device_handle != NULL) {
        li->DeviceHandle = (EFI_HANDLE)info->device_handle;
    }

    /* Track both allocations for uninstall + free at unload. */
    image_dp_track(drv_handle, full, file_path);
}

static void *
driver_pack_alloc(
    void   *ctx,
    size_t  n
    )
{
    (void)ctx;
    return axl_malloc(n);
}

static void
driver_pack_free(
    void *ctx,
    void *p
    )
{
    (void)ctx;
    axl_free(p);
}

/* A failed LoadImage normally creates no handle, but EFI_SECURITY_VIOLATION
 * (UEFI 2.11: the image failed authentication under the platform policy)
 * returns one the caller must UnloadImage. Both LoadImage sites refuse the
 * image on any failure, so that handle is released here. */
static void
driver_unload_refused(
    EFI_STATUS st,
    EFI_HANDLE image
    )
{
    if (st == EFI_SECURITY_VIOLATION && image != NULL) {
        axl_efi_call(axl_bs()->UnloadImage, 1, image);
    }
}

bool
_axl_image_tpl_refused(
    const char *api
    )
{
    uintptr_t tpl = axl_backend_tpl_current();
    if (tpl < TPL_CALLBACK) {
        return false;
    }
    /* log-level: the caller gets AXL_UNSUPPORTED, which says "not here" but
       not why; the why -- the TPL and the rule -- is said only here */
    axl_error("%s: refused at TPL %u -- UEFI allows LoadImage and StartImage "
              "only below TPL_CALLBACK (UEFI 2.11 Table 7.3); call it from an "
              "application, such as a Shell command", api, (unsigned)tpl);
    return true;
}

/* THE buffer choke point: every image AXL loads from bytes in memory comes
 * through here. A wrapped image (its section table names .axlpack) is
 * unpacked first and the ORIGINAL is handed to LoadImage, so the handle
 * returned is the original's and no stub is ever resident. A buffer that is
 * wrapped but cannot be unpacked is a load error, logged with the pack
 * status -- never loaded as the stub it is.
 *
 * @p file_dp, when non-NULL, is the device path of the file the bytes came
 * from: LoadImage gets it, so the image's FilePath/DeviceHandle are the
 * file's own. NULL means a true memory load, which gets the synthesized
 * MemoryMapped identity (@p info). */
static int
driver_load_from_buffer(
    const void                 *buf,
    size_t                      len,
    const void                 *file_dp,
    const AxlEmbeddedImageInfo *info,
    AxlDriverHandle            *out
    )
{
    if (out != NULL) {
        *out = NULL;
    }
    if (buf == NULL || len == 0 || out == NULL) {
        return AXL_ERR;
    }
    const char *what = (info != NULL && info->file_name != NULL)
                           ? info->file_name : "<buffer>";

    size_t        off     = 0;
    size_t        pk_len  = 0;
    void         *raw     = NULL;
    size_t        raw_len = 0;
    AxlPackStatus ps      = axl_pack_find_payload(buf, len, &off, &pk_len,
                                                  NULL);
    if (ps == AXL_PACK_OK) {
        AxlPackAlloc a = { driver_pack_alloc, driver_pack_free, NULL };
        ps = axl_pack_unpack((const uint8_t *)buf + off, pk_len,
                             AXL_PACK_MACHINE_NATIVE, &a, &raw, &raw_len);
    }
    if (ps != AXL_PACK_OK && ps != AXL_PACK_E_NOT_PACKED) {
        /* log-level: the return is a bare AXL_ERR; that the file is a packed
           image, and which check it failed, is said only here */
        axl_warning("cannot load %s: %s", what, axl_pack_status_str(ps));
        return AXL_ERR;
    }

    bool        unpacked = raw != NULL;
    const void *img      = unpacked ? raw : buf;
    size_t      img_len  = unpacked ? raw_len : len;
    EFI_HANDLE  image   = NULL;
    EFI_STATUS  st      = axl_efi_call(axl_bs()->LoadImage, 6,
        FALSE,                                         /* BootPolicy */
        gImageHandle,                                  /* ParentImageHandle */
        (EFI_DEVICE_PATH_PROTOCOL *)(uintptr_t)file_dp, /* NULL: memory load */
        (void *)(uintptr_t)img, img_len, &image);
    axl_free(raw);   /* LoadImage copied it */
    if (EFI_ERROR(st) || image == NULL) {
        driver_unload_refused(st, image);
        axl_debug("driver load: LoadImage of %s (%zu bytes%s) failed: 0x%llx",
                  what, img_len, unpacked ? ", unpacked" : "",
                  (unsigned long long)st);
        return AXL_ERR;
    }

    if (file_dp == NULL) {
        driver_apply_image_identity(image, info);
    }
    *out = (AxlDriverHandle)image;
    return AXL_OK;
}

/* Open the file a file-on-volume device path names: the volume by
 * LocateDevicePath(SimpleFileSystem), then each MEDIA_FILEPATH node in turn,
 * as the firmware's own file loader does. NULL on any failure (the caller
 * then lets the firmware try, and report). */
static EFI_FILE_PROTOCOL *
driver_dp_open_file(
    EFI_DEVICE_PATH_PROTOCOL *dp
    )
{
    EFI_DEVICE_PATH_PROTOCOL *walk = dp;   /* LocateDevicePath advances it */
    EFI_HANDLE                vol  = NULL;
    if (EFI_ERROR(axl_efi_call(axl_bs()->LocateDevicePath, 3,
                               &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
                               &walk, &vol))) {
        return NULL;
    }
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL               *f  = NULL;
    if (EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3, vol,
                               &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
                               (void **)&fs))
        || fs == NULL
        || EFI_ERROR(axl_efi_call(fs->OpenVolume, 2, fs, &f))) {
        return NULL;
    }

    const uint8_t *node  = (const uint8_t *)walk;
    size_t         opens = 0;
    while (node[0] != DRIVER_DP_END_TYPE) {
        size_t nlen = (size_t)node[2] | ((size_t)node[3] << 8);
        if (node[0] != DRIVER_DP_MEDIA_TYPE || node[1] != DRIVER_DP_FILEPATH_SUB
            || nlen <= DRIVER_DP_NODE_HDR) {
            axl_efi_call(f->Close, 1, f);
            return NULL;
        }
        /* A node's string need not be CHAR16-aligned: copy it out, and
           NUL-terminate it whatever the node held. */
        size_t  chars = (nlen - DRIVER_DP_NODE_HDR) / sizeof(CHAR16);
        CHAR16 *name  = axl_malloc((chars + 1) * sizeof(CHAR16));
        if (name == NULL) {
            axl_efi_call(f->Close, 1, f);
            return NULL;
        }
        axl_memcpy(name, node + DRIVER_DP_NODE_HDR, chars * sizeof(CHAR16));
        name[chars] = 0;
        EFI_FILE_PROTOCOL *next = NULL;
        EFI_STATUS st = axl_efi_call(f->Open, 5, f, &next, name,
                                     EFI_FILE_MODE_READ, 0);
        axl_free(name);
        axl_efi_call(f->Close, 1, f);
        if (EFI_ERROR(st) || next == NULL) {
            return NULL;
        }
        f = next;
        opens++;
        node += nlen;
    }
    if (opens == 0) {   /* the path named a volume, not a file */
        axl_efi_call(f->Close, 1, f);
        return NULL;
    }
    return f;
}

/* Read @p want bytes (or to EOF) from @p f's current position into @p buf,
   at most AXL_FS_READ_CHUNK per Read: BMC virtual media has been measured
   corrupting larger single reads and not 64 KB ones (axl-fs-read-chunk.h).
   Returns the count read, or SIZE_MAX on an I/O error. */
static size_t
driver_file_read(
    EFI_FILE_PROTOCOL *f,
    void              *buf,
    size_t             want
    )
{
    size_t got = 0;
    while (got < want) {
        UINTN n = (want - got < AXL_FS_READ_CHUNK) ? want - got
                                                   : AXL_FS_READ_CHUNK;
        if (EFI_ERROR(axl_efi_call(f->Read, 3, f, &n, (uint8_t *)buf + got))) {
            return SIZE_MAX;
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    return got;
}

/* Outcome of driver_file_read_all. NO_INFO (GetInfo failed, so AXL cannot
   size the file) is the one case where the firmware is asked to read the
   file itself; REFUSED is final and already logged. */
typedef enum {
    DRIVER_READ_OK,
    DRIVER_READ_NO_INFO,
    DRIVER_READ_REFUSED,
} DriverReadResult;

/* Read all of @p f (closing it) into an axl_malloc'd *@p buf, in
   AXL_FS_READ_CHUNK reads. The size comes from EFI_FILE_INFO, as the
   firmware's own file loader gets it: every file system implements GetInfo,
   where GetPosition is one it may leave unsupported (AXL's own AxlFsProvider
   volumes do).

   Once the size is known every failure is REFUSED, with one warning naming
   @p what, never a fallback to the firmware's own read of the path: an
   error in AXL's own read is the strongest sign the media is bad, and a raw
   image the firmware reads in one large call runs whatever bytes come back.
   A file over AXL_PACK_MAX_RAW (64 MB) is refused before anything is
   allocated: no driver, packed or raw, is that large, and the size comes
   from the media. An empty file is refused too -- LoadImage could only
   reject it. */
static DriverReadResult
driver_file_read_all(
    EFI_FILE_PROTOCOL *f,
    const char        *what,
    void             **buf,
    size_t            *len
    )
{
    EFI_GUID       guid = gEfiFileInfoGuid;
    EFI_FILE_INFO *info = NULL;
    UINTN          isz  = 0;
    UINT64         size = 0;
    bool           have = false;
    const char    *why  = NULL;

    *buf = NULL;
    *len = 0;
    if (axl_efi_call(f->GetInfo, 4, f, &guid, &isz, NULL) == EFI_BUFFER_TOO_SMALL) {
        info = axl_malloc(isz);
    }
    if (info != NULL
        && !EFI_ERROR(axl_efi_call(f->GetInfo, 4, f, &guid, &isz, info))) {
        size = info->FileSize;
        have = true;
    }
    axl_free(info);
    if (!have) {
        axl_efi_call(f->Close, 1, f);
        return DRIVER_READ_NO_INFO;
    }

    if (size == 0) {
        why = "the file is empty";
    } else if (size > AXL_PACK_MAX_RAW) {
        why = "the file is larger than 64 MB";
    } else {
        *buf = axl_malloc((size_t)size);
        if (*buf == NULL) {
            why = "not enough memory to read it";
        } else if (driver_file_read(f, *buf, (size_t)size) != (size_t)size) {
            why = "the file could not be read whole";
            axl_free(*buf);
            *buf = NULL;
        }
    }
    axl_efi_call(f->Close, 1, f);
    if (why != NULL) {
        /* log-level: the caller gets a bare AXL_ERR; why the driver was not
           loaded, and that the firmware was not asked instead, is only here */
        axl_warning("cannot load %s: %s", what, why);
        return DRIVER_READ_REFUSED;
    }
    *len = (size_t)size;
    return DRIVER_READ_OK;
}

/* Is @p path the file the RUNNING image was loaded from? Decided by the
 * identity axl_file_replace() uses: both paths resolved component by
 * component through opened handles to the names the filesystem STORES
 * (axl_fs_path_canonical), on the same volume handle. So an 8.3 alias,
 * another letter case, "." and ".." segments and FAT's trailing-dot trim
 * all name the same file, and on a case-sensitive filesystem two names
 * that differ in case stay two files -- no folding by hand.
 *
 * Best effort: the image is located by the volume its LoadedImage
 * DeviceHandle names now. The same media reached through another volume
 * instance (re-enumerated, or mapped twice) is not recognised, which only
 * means the old behaviour, never a wrong refusal. An image not loaded from
 * a file (a buffer load) has no file, so nothing is it. */
/* The running image's identity, resolved once per image (on BMC virtual
   media every component open is a USB round trip). Static, so it lives
   exactly as long as this image -- a reloaded image starts with a zeroed
   .bss -- and keeps its 1.5 KB off the shared DXE stack. A failed
   resolution is not cached: it is retried, and answers "not self". */
static struct {
    bool  ok;
    void *volume;
    char  canon[AXL_FS_REL_MAX];
} m_self_id;

static bool
driver_self_identity(void)
{
    if (m_self_id.ok) {
        return true;
    }
    const char *self_path = axl_app_image_path();
    if (self_path == NULL) {
        return false;   /* not loaded from a file: nothing is it */
    }
    /* Through the volume name it answers to NOW (a `map -r` may have
       renamed it since the image path was recorded). */
    AXL_AUTO_FREE char *self_name = axl_path_get_basename(self_path);
    AXL_AUTO_FREE char *self      = axl_malloc(AXL_FS_REL_MAX);
    if (self_name == NULL || self == NULL
        || axl_app_image_dir_path(self_name, self, AXL_FS_REL_MAX) != AXL_OK
        || axl_fs_path_canonical(self, &m_self_id.volume, m_self_id.canon,
                                 sizeof(m_self_id.canon)) != AXL_OK
        || m_self_id.canon[0] == '\0') {
        return false;
    }
    m_self_id.ok = true;
    axl_debug("driver: the running image resolved as '%s'", m_self_id.canon);
    return true;
}

static bool
driver_path_is_self(const char *path)
{
    if (path == NULL || !driver_self_identity()) {
        return false;
    }
    void               *vol   = NULL;
    AXL_AUTO_FREE char *canon = axl_malloc(AXL_FS_REL_MAX);
    return canon != NULL
           && axl_fs_path_canonical(path, &vol, canon, AXL_FS_REL_MAX) == AXL_OK
           && vol == m_self_id.volume
           && axl_strcmp(canon, m_self_id.canon) == 0;
}

/* Load the image file @p dp names. AXL reads the file itself, in
 * AXL_FS_READ_CHUNK reads, and hands LoadImage the bytes TOGETHER with @p dp
 * (driver_load_from_buffer), so the image's FilePath and DeviceHandle are
 * the file's own -- as iPXE-style drivers, which find their install
 * directory from them, need -- and the firmware never makes one large read
 * of the file: on BMC virtual media such reads come back corrupt
 * (axl-fs-read-chunk.h). A packed file is unpacked on the way; the decision
 * is the full buffer's section table. The firmware loads from the path,
 * reading the file itself, only when AXL cannot open the file or learn its
 * size; a file AXL opened and could not read is refused
 * (driver_file_read_all). @p what names the file in a log line. */
static DriverDpResult
driver_load_dp(
    EFI_DEVICE_PATH_PROTOCOL *dp,
    const char               *what,
    AxlDriverHandle          *out
    )
{
    *out = NULL;
    EFI_FILE_PROTOCOL *f   = driver_dp_open_file(dp);
    void              *buf = NULL;
    size_t             len = 0;
    DriverReadResult   rr  = (f != NULL)
                                 ? driver_file_read_all(f, what, &buf, &len)
                                 : DRIVER_READ_NO_INFO;
    if (rr == DRIVER_READ_REFUSED) {
        return DRIVER_DP_REFUSED;
    }
    if (rr == DRIVER_READ_OK) {
        AxlEmbeddedImageInfo info = { .file_name = what };
        int rc = driver_load_from_buffer(buf, len, dp, &info, out);
        axl_free(buf);
        return (rc == AXL_OK) ? DRIVER_DP_LOADED : DRIVER_DP_REFUSED;
    }

    axl_debug("driver load: cannot open or size '%s' here; the firmware "
              "loads it from its path", what);
    EFI_HANDLE image = NULL;
    EFI_STATUS st    = axl_efi_call(axl_bs()->LoadImage, 6, FALSE, gImageHandle,
                                    dp, NULL, 0, &image);
    if (EFI_ERROR(st) || image == NULL) {
        driver_unload_refused(st, image);
        axl_debug("driver load: DevicePath LoadImage of '%s' failed: 0x%llx",
                  what, (unsigned long long)st);
        return DRIVER_DP_FAILED;
    }
    *out = (AxlDriverHandle)image;
    return DRIVER_DP_LOADED;
}

/* The running image's own file, refused to a DRIVER loader: "load myself
   as a driver" has no meaning, and a search that picked it would start the
   image again inside itself. AXL_ERR, said why. */
static int
driver_refuse_self(const char *path)
{
    /* log-level: the caller gets AXL_ERR; why is said only here */
    axl_error("driver load: '%s' is the running image itself; not loading it", path);
    return AXL_ERR;
}

static int
driver_load_path(
    const char       *path,
    AxlDriverHandle  *handle,
    bool              allow_self
    );

int
axl_driver_load(
    const char       *path,
    AxlDriverHandle  *handle
    )
{
    return driver_load_path(path, handle, false);
}

int
_axl_driver_load_self_ok(
    const char       *path,
    AxlDriverHandle  *handle
    )
{
    return driver_load_path(path, handle, true);
}

static int
driver_load_path(
    const char       *path,
    AxlDriverHandle  *handle,
    bool              allow_self
    )
{
    if (_axl_image_tpl_refused("axl_driver_load")) {
        if (handle != NULL) {
            *handle = NULL;
        }
        return AXL_UNSUPPORTED;
    }
    if (path == NULL || handle == NULL) {
        return AXL_ERR;
    }

    *handle = NULL;
    if (!allow_self && driver_path_is_self(path)) {
        return driver_refuse_self(path);
    }

    /* Prefer the file's device path: the image's LoadedImage->FilePath is
     * then the full DP, which driver-binding-style drivers (notably iPXE)
     * read at StartImage to locate their own install directory. AXL reads
     * the file (in 64 KB reads) and loads the bytes with that DP; a packed
     * one is unpacked first (driver_load_dp). */
    EFI_DEVICE_PATH_PROTOCOL *file_dp = driver_build_file_dp(path);
    if (file_dp != NULL) {
        DriverDpResult r = driver_load_dp(file_dp, path, handle);
        driver_free_dp(file_dp);
        if (r == DRIVER_DP_LOADED) {
            return AXL_OK;
        }
        if (r == DRIVER_DP_REFUSED) {
            return AXL_ERR;
        }
        axl_debug("driver load: falling back to a buffer load of '%s'", path);
    }

    /* Fallback: read the file ourselves and load it from memory. Works for
     * drivers that don't read LoadedImage->FilePath at startup (e.g.
     * RamDiskDxe). Drivers that DO need it (iPXE) will fail StartImage with
     * EFI_INVALID_PARAMETER on this path; the caller already logs that case.
     * The image gets the synthesized identity, keyed off the file's basename,
     * so a path load that lands here never shows the shell a NULL path. */
    void  *buf      = NULL;
    size_t buf_size = 0;
    if (axl_file_get_contents(path, &buf, &buf_size) != AXL_OK || buf == NULL) {
        axl_debug("driver load: cannot read '%s'", path);
        return AXL_ERR;
    }
    AxlEmbeddedImageInfo info = { .file_name = driver_basename(path) };
    int rc = driver_load_from_buffer(buf, buf_size, NULL, &info, handle);
    axl_free(buf);
    return rc;
}

int
axl_driver_start(
    AxlDriverHandle handle
    )
{
    EFI_STATUS status;

    if (_axl_image_tpl_refused("axl_driver_start")) {
        return AXL_UNSUPPORTED;
    }
    if (handle == NULL) {
        return AXL_ERR;
    }

    status = (EFI_STATUS)axl_backend_start_image((void *)handle);

    /* The image's entry point has run, so it has read its LoadOptions. Our
       copy goes now rather than at axl_driver_unload: a launcher that starts a
       resident driver usually exits long before anyone unloads it, and the
       unload then runs in another image, whose table never held this copy. */
    load_options_retire(handle);
    /* After the retire: the child must not keep LoadOptions pointing into
       this image's heap if a deferred exit takes it now. */
    axl_backend_exit_if_deferred();

    if (EFI_ERROR(status)) {
        axl_debug("driver start failed: 0x%llx",
                 (unsigned long long)status);
        return AXL_ERR;
    }

    return AXL_OK;
}

int
axl_driver_connect(
    AxlDriverHandle handle
    )
{
    /* If a real handle is given, connect just that handle. */
    if (handle != NULL) {
        return axl_driver_connect_handle(handle);
    }

    /* Otherwise, reconnect every controller in the system. UEFI's
     * ConnectController() returns EFI_INVALID_PARAMETER when called
     * with ControllerHandle=NULL — the "connect everything" behavior
     * the shell's `connect -r` command implements is enumerate-all-
     * handles + ConnectController on each. We mirror that here.
     *
     * NOT_FOUND from per-handle calls is normal (means the handle has
     * no candidate driver bindings), not an error. */
    EFI_HANDLE *handles = NULL;
    UINTN       count = 0;
    EFI_STATUS  st = axl_bs()->LocateHandleBuffer(
        AllHandles, NULL, NULL, &count, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        return AXL_ERR;
    }

    /* The final TRUE (Recursive) is load-bearing, not cosmetic: it makes
     * ConnectController cascade the ENTIRE driver subtree below each handle in
     * one call, so a multi-level driver stack binds fully (e.g. a NIC:
     * virtio-net -> SnpDxe -> MnpDxe -> ArpDxe -> Ip4Dxe -> Dhcp4Dxe, or a
     * nested USB-network dependency chain). Consumers rely on this -- netload's
     * firmware-first probe and dependency co-load both assume `connect -r`
     * resolves arbitrary nesting depth. Do NOT drop it to FALSE (regression-
     * guarded by test-netload-qemu.sh's --net firmware-first lease check). */
    /* Counted as a child: a connect can start images (option ROMs, bound
       drivers' Start) -- see axl_backend_start_image. */
    axl_backend_child_image_enter();
    for (UINTN i = 0; i < count; i++) {
        axl_bs()->ConnectController(handles[i], NULL, NULL, TRUE);
    }
    axl_backend_child_image_leave();
    axl_bs()->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    axl_backend_exit_if_deferred();     /* after the FreePool: firmware pool */
    return AXL_OK;
}

int
axl_driver_disconnect(
    AxlDriverHandle handle
    )
{
    if (handle != NULL) {
        axl_bs()->DisconnectController((EFI_HANDLE)handle, NULL, NULL);
        return AXL_OK;
    }

    /* NULL handle — disconnect all */
    axl_bs()->DisconnectController(NULL, NULL, NULL);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Load-options ownership table
//
// axl_driver_set_load_options copies the caller's buffer with axl_malloc
// and hands the pointer to the firmware via LoadedImage->LoadOptions; the
// firmware retains the pointer for the lifetime of the loaded image and
// has no callback to free it. Without this side table, axl_driver_unload
// would leak the copy on every load+set+unload cycle (142 bytes per
// driver instance in the original axl-webfs reproducer).
//
// Mirrors the NotifyTimerEntry pattern in axl-backend-native-event.c:
// fixed-capacity table indexed by handle, allocated/freed only in this
// module's set/unload pair. 16 slots — sequential driver loads/unloads
// are the realistic case; a consumer holding 16+ driver instances open
// concurrently with load options is unusual for an SDK user. Out of
// slots returns AXL_ERR rather than installing-and-leaking — the
// log-and-leak alternative would silently re-introduce the bug this
// table exists to prevent.
// ---------------------------------------------------------------------------

#define LOAD_OPTIONS_TABLE_SIZE  16

typedef struct {
    AxlDriverHandle  handle;  /* NULL = slot free; non-NULL = tracked */
    void            *opts;
} LoadOptionsEntry;

static LoadOptionsEntry mLoadOptionsTable[LOAD_OPTIONS_TABLE_SIZE];

/* Find the table slot for `handle`. Returns LOAD_OPTIONS_TABLE_SIZE if
   not found. handle == NULL is rejected at the public-API boundary, so
   never reaches here. */
static size_t
load_options_find(AxlDriverHandle handle)
{
    for (size_t i = 0; i < LOAD_OPTIONS_TABLE_SIZE; i++) {
        if (mLoadOptionsTable[i].handle == handle) {
            return i;
        }
    }
    return LOAD_OPTIONS_TABLE_SIZE;
}

/* Free + clear the slot for `handle`. No-op if the handle was never
   tracked (caller never set load options). */
static void
load_options_release(AxlDriverHandle handle)
{
    size_t slot = load_options_find(handle);
    if (slot == LOAD_OPTIONS_TABLE_SIZE) {
        return;
    }
    axl_free(mLoadOptionsTable[slot].opts);
    mLoadOptionsTable[slot].handle = NULL;
    mLoadOptionsTable[slot].opts   = NULL;
}

/* Release `handle`'s tracked copy AND take it back from the firmware: while the
   image is still loaded, its LoadedImage->LoadOptions is pointed away first, so
   no reader is ever left holding the freed copy. The pointer is cleared only if
   it is still ours -- the image may have installed options of its own. An image
   the firmware already unloaded (an application, a failed DriverEntry) has no
   LoadedImage left to clear; the copy is released all the same. */
static void
load_options_retire(AxlDriverHandle handle)
{
    size_t slot = load_options_find(handle);
    if (slot == LOAD_OPTIONS_TABLE_SIZE) {
        return;
    }
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    if (!EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3, (EFI_HANDLE)handle,
                                &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&img))
        && img != NULL && img->LoadOptions == mLoadOptionsTable[slot].opts) {
        img->LoadOptions     = NULL;
        img->LoadOptionsSize = 0;
    }
    load_options_release(handle);
}

int
axl_driver_unload(
    AxlDriverHandle handle
    )
{
    EFI_STATUS status;

    if (handle == NULL) {
        return AXL_ERR;
    }

    /* The firmware auto-unloads an image whose DriverEntry returned an error
       (and any application) -- driver_start_and_verify and axl-net-driver-
       select both call us on exactly that error path. Such an image is gone:
       release what of our synthesized device path is left and do not
       UnloadImage the dead handle, which would only fail and mislead. */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    bool alive = axl_efi_call(axl_bs()->HandleProtocol, 3, (EFI_HANDLE)handle,
                              &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li) == EFI_SUCCESS
                 && li != NULL;
    if (!alive) {
        load_options_retire(handle);   /* the copy is ours; no LoadedImage left */
        if (!image_dp_release(handle)) {
            return AXL_OK;
        }
    }

    /* UnloadImage FIRST; nothing of ours is released before a call that can
       refuse. An unload the driver refuses leaves the image resident, and it
       keeps all of it: its load options (a tracked copy still in place), and
       its identity -- the installed LoadedImageDevicePath and
       LoadedImage->FilePath, which `dh` reads. Once the image is gone the
       firmware has freed Info.FilePath (our copy) and uninstalled
       LoadedImage, but not our LoadedImageDevicePath, which
       image_dp_release's "image gone" branch then uninstalls and frees,
       letting the handle go; the load-options copy is freed with no
       LoadedImage left to point at it. */
    status = axl_bs()->UnloadImage((EFI_HANDLE)handle);
    if (!EFI_ERROR(status)) {
        load_options_retire(handle);
        image_dp_release(handle);
    }
    if (EFI_ERROR(status)) {
        /* Surface the raw status so callers can triage. EFI_ACCESS_DENIED
           is the most-misdiagnosed shape — the firmware's post-callback
           refcount check refuses because the image still holds open
           protocol references — so we add a hint at that case
           specifically. Other failures (EFI_INVALID_PARAMETER, _UNSUPPORTED,
           rollback-path errors) get a generic message. */
        if (status == EFI_ACCESS_DENIED) {
            axl_warning("axl_driver_unload: UnloadImage(handle=%p) "
                        "returned EFI_ACCESS_DENIED (0x%llx) - image "
                        "still holds open protocol references; if your "
                        "service opens UEFI protocols in setup, ensure "
                        "teardown closes every one (axl_http_server_free, "
                        "axl_tcp_close, etc.) before returning",
                        (void *)handle, (unsigned long long)status);
        } else {
            axl_warning("axl_driver_unload: UnloadImage(handle=%p) "
                        "returned 0x%llx",
                        (void *)handle, (unsigned long long)status);
        }
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_driver_set_load_options(
    AxlDriverHandle  handle,
    const void      *data,
    size_t           size
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS status;
    void *copy;

    if (handle == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->HandleProtocol(
        (EFI_HANDLE)handle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    if (EFI_ERROR(status) || img == NULL) {
        axl_debug("driver set_load_options: HandleProtocol failed");
        return AXL_ERR;
    }

    /* Allow NULL data to clear load options. Drop any previous tracked
       copy so the count stays accurate. */
    if (data == NULL || size == 0) {
        load_options_release(handle);
        img->LoadOptions = NULL;
        img->LoadOptionsSize = 0;
        return AXL_OK;
    }

    /* Copy the data — caller's buffer may be on the stack */
    copy = axl_malloc(size);
    if (copy == NULL) {
        return AXL_ERR;
    }
    axl_memcpy(copy, data, size);

    /* Track for unload-time release. If the handle already has a tracked
       copy (re-set), free the old one first and reuse the slot.
       Otherwise reserve a fresh slot. Out-of-slots is fatal: the only
       alternative is to install the copy without tracking it, which
       silently re-introduces the very leak this table exists to
       prevent. */
    size_t slot = load_options_find(handle);
    if (slot < LOAD_OPTIONS_TABLE_SIZE) {
        axl_free(mLoadOptionsTable[slot].opts);
        mLoadOptionsTable[slot].opts = copy;
    } else {
        for (slot = 0; slot < LOAD_OPTIONS_TABLE_SIZE; slot++) {
            if (mLoadOptionsTable[slot].handle == NULL) {
                mLoadOptionsTable[slot].handle = handle;
                mLoadOptionsTable[slot].opts   = copy;
                break;
            }
        }
        if (slot == LOAD_OPTIONS_TABLE_SIZE) {
            axl_error("driver set_load_options: tracking table full "
                      "(%d slots) - increase LOAD_OPTIONS_TABLE_SIZE",
                      LOAD_OPTIONS_TABLE_SIZE);
            axl_free(copy);
            return AXL_ERR;
        }
    }

    img->LoadOptions = copy;
    img->LoadOptionsSize = (UINT32)(size > 0xFFFFFFFF ? 0xFFFFFFFF : size);
    return AXL_OK;
}

char *
axl_driver_get_load_options(void)
{
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS status;

    if (gImageHandle == NULL) {
        return NULL;
    }

    status = axl_bs()->HandleProtocol(
        gImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    if (EFI_ERROR(status) || img == NULL) {
        return NULL;
    }

    if (img->LoadOptions == NULL || img->LoadOptionsSize == 0) {
        return NULL;
    }

    /* LoadOptions is typically UCS-2 — convert to UTF-8 */
    return axl_ucs2_to_utf8((const unsigned short *)img->LoadOptions);
}

int
axl_driver_get_load_options_raw(
    const void **out_buf,
    size_t      *out_size
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS status;

    if (out_buf == NULL || out_size == NULL || gImageHandle == NULL) {
        return AXL_ERR;
    }

    *out_buf  = NULL;
    *out_size = 0;

    status = axl_bs()->HandleProtocol(
        gImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    if (EFI_ERROR(status) || img == NULL ||
        img->LoadOptions == NULL || img->LoadOptionsSize == 0) {
        return AXL_ERR;
    }

    *out_buf  = img->LoadOptions;
    *out_size = (size_t)img->LoadOptionsSize;
    return AXL_OK;
}

char *
axl_driver_get_image_path(void)
{
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS status;

    if (gImageHandle == NULL) {
        return NULL;
    }

    status = axl_bs()->HandleProtocol(
        gImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    /* DeviceHandle gates this for the same reason it gates
       axl_app_image_path: a buffer-loaded image has no file it came from,
       and the MEDIA_FILEPATH node below is one AXL synthesized after the
       load (driver_apply_image_identity) purely so the shell can render the
       handle. Decoding it would hand back a volume-less "\<name>" naming no
       real file. No source volume, no path. */
    if (EFI_ERROR(status) || img == NULL || img->FilePath == NULL
        || img->DeviceHandle == NULL) {
        return NULL;
    }

    /* Find the MEDIA_FILEPATH_DP node (Type=4, SubType=4); the bytes
       after its 4-byte header are a UCS-2 path string. The bounded
       traversal in axl_device_path_for_each guards against runaway
       on malformed firmware data. */
    const EFI_DEVICE_PATH_PROTOCOL *fp = axl_device_path_find(
        img->FilePath, 0x04, 0x04);
    if (fp != NULL && EFI_DP_LENGTH(fp) > 4) {
        unsigned short *wpath = (unsigned short *)((uint8_t *)fp + 4);
        return axl_ucs2_to_utf8(wpath);
    }
    return NULL;
}

int
axl_driver_connect_handle(
    void *handle
    )
{
    EFI_STATUS status;

    if (handle == NULL) {
        return AXL_ERR;
    }

    axl_backend_child_image_enter();    /* see axl_backend_start_image */
    status = axl_bs()->ConnectController(
        (EFI_HANDLE)handle, NULL, NULL, TRUE);
    axl_backend_child_image_leave();
    axl_backend_exit_if_deferred();

    /* NOT_FOUND is OK — means no new bindings, not an error */
    if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
        return AXL_ERR;
    }

    return AXL_OK;
}

int
axl_driver_disconnect_handle(
    void *handle
    )
{
    EFI_STATUS status;

    if (handle == NULL) {
        return AXL_ERR;
    }

    /* NULL DriverImageHandle / ChildHandle → disconnect all drivers and
       all children from this controller. */
    status = axl_bs()->DisconnectController(
        (EFI_HANDLE)handle, NULL, NULL);

    /* NOT_FOUND is OK — no driver was managing the handle (a no-op),
       mirroring axl_driver_connect_handle. */
    if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
        return AXL_ERR;
    }

    return AXL_OK;
}

int
axl_driver_set_unload(
    void *unload_fn
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS status;

    if (gImageHandle == NULL || unload_fn == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->HandleProtocol(
        gImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    if (EFI_ERROR(status) || img == NULL) {
        return AXL_ERR;
    }

    img->Unload = (EFI_IMAGE_UNLOAD)unload_fn;
    return AXL_OK;
}

void
axl_driver_init(
    AxlHandle        image_handle,
    AxlSystemTable  *system_table
    )
{
    extern EFI_SYSTEM_TABLE     *gST;
    extern EFI_BOOT_SERVICES    *gBS;
    extern EFI_RUNTIME_SERVICES *gRT;
    extern EFI_HANDLE            gImageHandle;

    gImageHandle = (EFI_HANDLE)image_handle;
    gST = (EFI_SYSTEM_TABLE *)system_table;
    gBS = gST->BootServices;
    gRT = gST->RuntimeServices;

    axl_stream_init();

    /* Populate the per-image sidecar-discovery anchor. Apps reach
       this via _axl_args_init from the CRT; drivers must go through
       here because the driver CRT path doesn't parse argv. Includes
       a ParentHandle fallback for buffer-loaded drivers — see
       axl-image-internal.h for the contract. */
    _axl_init_image_path((void *)image_handle);

    /* BEFORE the constructors, not after, and this order is the whole
       reason the two calls are adjacent. A C++ static destructor does
       NOT land in .fini_array — measured on both arches, the object
       carries no such section at all. It registers at RUN TIME, from
       inside its own constructor, through __cxa_atexit, which AXL
       routes to axl_atexit. And axl_atexit REFUSES registration while
       its table is NULL, returning 0 that __cxa_atexit turns into -1
       that the compiler-generated caller discards.

       So walking .init_array without initialising this table first
       would not merely leave destructors unrun: it would leave every
       one of them silently UNREGISTERED — a fresh instance of exactly
       the silent-failure bug this call pair exists to fix. The table
       frees itself in _axl_atexit_run_all (axl_driver_cleanup). */
    _axl_atexit_mark_driver_image();
    _axl_atexit_init();

    /* C++ global constructors fire last, after the rest of the driver
       runtime is up, so ctors may use axl_printf / axl_malloc / etc.
       Mirrors _axl_init's ordering and its comment.

       THIS IS THE CALL A DRIVER IMAGE DID NOT HAVE. An app reached it
       via _axl_init; a driver's entry path stopped here, so a driver
       image linked __init_array_start / __init_array_end bracketing
       real constructors and nothing that ever walked them. Silently:
       no link error, no warning, no diagnostic — and because crt0
       zeroes .bss, every unconstructed global read as all-zero rather
       than faulting, which puts the symptom nowhere near the cause.
       No-op for pure-C drivers (empty .init_array). */
    _axl_cxxabi_run_init_array();
}

void
_axl_driver_cleanup_keep_heap(void)
{
    /* The unload-side counterpart to axl_driver_init's two calls above.
       Drains in LIFO order and frees the table, so a driver that is
       unloaded and reloaded in one boot gets a clean table each time.

       Idempotent: _axl_atexit_run_all NULLs the table on the way out
       and returns immediately when it is already NULL, so a consumer
       with a hand-written DriverEntry that calls this AND uses a macro
       that also calls it does not run destructors twice. */
    _axl_atexit_run_all();

    /* And the image-path capture _axl_init_image_path made above. On the app
       path _axl_args_free does this; a driver has no argv, and these frees
       used to sit behind that function's mArgv guard where a driver could
       never reach them -- so every driver load leaked the capture, and a
       reloaded service leaked it per cycle. */
    _axl_image_path_free();

    /* And the default loop, as _axl_cleanup frees an app's: a driver (or a
       service's setup) that called axl_loop_default() would otherwise leak
       it on every unload and show it in the unload's leak verdict. After the
       atexit drain, which may still use it -- the same order as an app. */
    _axl_default_loop_free();

#ifdef AXL_MEM_DEBUG
    /* And the free quarantine, which only the leak report drained -- and a
       plain driver's unload prints none, so up to 64 KiB of freed pool
       outlived every unload (~2.9 pages per cycle, test-heap-release-qemu).
       Last of the AxlMem work: nothing above frees after it. */
    _axl_mem_quarantine_drain();
#endif
}

void
axl_driver_cleanup(void)
{
    _axl_driver_cleanup_keep_heap();

    /* LAST, as on the app path: newlib's arena back to the firmware, which
       does not reclaim it at unload -- every load/unload cycle of a driver
       that ever malloc'd (any C++ driver) used to leave 1 MiB behind. See
       axl-libc-heap-internal.h. Idempotent like the rest of this. */
    _axl_libc_heap_release();
}

// ---------------------------------------------------------------------------
// axl_driver_locate / axl_driver_ensure
// ---------------------------------------------------------------------------

static int
driver_protocol_registered(
    const AxlGuid *guid
    )
{
    void *iface = NULL;
    /* LocateProtocol's prototype is non-const for legacy EDK reasons
     * (it doesn't actually mutate). The cast is what keeps AxlGuid
     * out of the public-surface const story. */
    EFI_STATUS st = axl_bs()->LocateProtocol(
        (EFI_GUID *)guid, NULL, &iface);
    return EFI_ERROR(st) ? AXL_ERR : AXL_OK;
}

static int
driver_append_candidate(
    char  **candidates,
    size_t *n_cand,
    const char *path
    )
{
    if (*n_cand >= DRIVER_MAX_CANDIDATES || path == NULL) {
        return AXL_ERR;
    }
    /* Dedup: paths 1 and 2 collide whenever the running image lives
     * directly under drivers/<arch>/, which is the common mkrd case. */
    for (size_t i = 0; i < *n_cand; i++) {
        if (axl_strcmp(candidates[i], path) == 0) {
            return AXL_OK;
        }
    }
    /* Never the calling image: a driver named as the image would otherwise
       find the image itself (as candidate 1, its own directory) and load and
       start it -- the image running again inside itself. Skipped, so the
       search goes on to the real driver. Debug: an app and its driver may
       share a name legitimately (fs0:\foo.efi, fs0:\drivers\x64\foo.efi). */
    if (driver_path_is_self(path)) {
        axl_debug("driver search: skipping '%s', the running image itself", path);
        return AXL_OK;
    }
    char *copy = axl_strdup(path);
    if (copy == NULL) {
        return AXL_ERR;
    }
    candidates[(*n_cand)++] = copy;
    return AXL_OK;
}

/* Build the standard driver-search candidate list. Caller owns
 * candidates[0..*n_cand-1] and must axl_free each on the way out. */
static void
driver_build_candidates(
    const char  *driver_name,
    char       **candidates,
    size_t      *n_cand
    )
{

    *n_cand = 0;

    /* Locate the volume the running image was loaded from. The handle
     * stored in EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle is the same
     * handle axl_volume_enumerate exposes for that filesystem. */
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS li_st = axl_bs()->HandleProtocol(
        gImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    AxlVolume volumes[DRIVER_MAX_VOLUMES];
    size_t    n_vols = 0;
    axl_volume_enumerate(volumes, DRIVER_MAX_VOLUMES, &n_vols);

    /* The image's volume is named as axl_app_boot_path names it -- the
       shell's own alias, verbatim (axl_backend_volume_name_for_handle) --
       so a path found here is spelled exactly as that one is. Enumeration's
       .name is lowercased by its contract ("fs0" for the shell's "FS0"),
       and a path built from it named the same file a second way. */
    char        image_vol[AXL_BACKEND_VOLUME_NAME_MAX];
    const char *image_fs = NULL;
    if (!EFI_ERROR(li_st) && img != NULL
        && axl_backend_volume_name_for_handle(img->DeviceHandle, image_vol,
                                              sizeof(image_vol)) == AXL_OK) {
        image_fs = image_vol;
    }

    char path_buf[DRIVER_PATH_BUF];
    char sub_buf[DRIVER_SUB_BUF];

    if (image_fs != NULL) {
        /* 1: <image_dir>/<name> in the running image's own directory
           (the sibling). Tried first — a co-located driver is the most
           specific intent and should win over a stale /drivers/<arch>/
           copy from an older install. */
        /* axl_driver_get_image_path is VOLUME-RELATIVE ("\app\x.efi"): the
           companion is joined in its directory, so an image at the root
           gives "\name" (not "\/name"), and the volume is prefixed once. */
        AXL_AUTO_FREE char *image_path = axl_driver_get_image_path();
        if (image_path != NULL && (image_path[0] == '\\' || image_path[0] == '/')) {
            AXL_AUTO_FREE char *sibling = axl_path_companion(image_path, driver_name);
            if (sibling != NULL
                && axl_path_build_uefi(image_fs, sibling,
                                       path_buf, sizeof(path_buf)) == AXL_OK)
            {
                driver_append_candidate(candidates, n_cand, path_buf);
            }
        }

        /* 2: drivers/<arch>/<name> on the image's volume. */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s/%s", driver_arch, driver_name) > 0
            && axl_path_build_uefi(image_fs, sub_buf,
                                   path_buf, sizeof(path_buf)) == AXL_OK)
        {
            driver_append_candidate(candidates, n_cand, path_buf);
        }

        /* 3: drivers/<name> at the volume root (no arch dir). */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s", driver_name) > 0
            && axl_path_build_uefi(image_fs, sub_buf,
                                   path_buf, sizeof(path_buf)) == AXL_OK)
        {
            driver_append_candidate(candidates, n_cand, path_buf);
        }

        /* 3.5: <name> at the volume root, for an image in a subdirectory
           whose driver sits at fs0:\ (an image AT the root already found
           it as candidate 1). */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/%s", driver_name) > 0
            && axl_path_build_uefi(image_fs, sub_buf,
                                   path_buf, sizeof(path_buf)) == AXL_OK)
        {
            driver_append_candidate(candidates, n_cand, path_buf);
        }
    }

    /* 4: drivers/<arch>/<name> on every other mounted volume -- "other" by
       HANDLE: the image's volume is named by the shell's alias above and by
       enumeration's lowercased .name here, so the names need not match. */
    for (size_t i = 0; i < n_vols && *n_cand < DRIVER_MAX_CANDIDATES; i++) {
        if (image_fs != NULL && volumes[i].handle == img->DeviceHandle) {
            continue;
        }
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s/%s", driver_arch, driver_name) > 0
            && axl_path_build_uefi(volumes[i].name, sub_buf,
                                   path_buf, sizeof(path_buf)) == AXL_OK)
        {
            driver_append_candidate(candidates, n_cand, path_buf);
        }
    }
}

int
axl_driver_locate(
    const char *driver_name,
    char       *out,
    size_t      out_size
    )
{
    if (driver_name == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }

    char  *candidates[DRIVER_MAX_CANDIDATES];
    size_t n_cand = 0;
    driver_build_candidates(driver_name, candidates, &n_cand);

    int rc = -1;
    for (size_t i = 0; i < n_cand; i++) {
        AxlFsEntry info;
        if (axl_file_info(candidates[i], &info) == AXL_OK && !axl_fs_entry_is_dir(&info)) {
            size_t len = axl_strlen(candidates[i]);
            if (len + 1 > out_size) {
                /* Path doesn't fit in caller buffer; treat as error
                 * rather than silently truncate. */
                axl_warning("driver locate: '%s' exceeds %zu bytes",
                            candidates[i], out_size);
                break;
            }
            axl_memcpy(out, candidates[i], len + 1);
            rc = 0;
            break;
        }
    }

    for (size_t i = 0; i < n_cand; i++) {
        axl_free(candidates[i]);
    }
    return rc;
}

/* Try to start a loaded driver and confirm it registered protocol_guid.
 * Caller owns @p drv; on failure (return AXL_ERR) the driver is unloaded
 * (which also frees any installed load_options copy via the side-table
 * path in axl_driver_unload). On success the driver stays loaded.
 * EFI_ALREADY_STARTED is treated as success — some drivers
 * DXE-Core-dispatched re-register cleanly. */
static int
driver_start_and_verify(
    AxlDriverHandle drv,
    const AxlGuid  *protocol_guid,
    const char     *source_label,  ///< for diagnostics, e.g. path or "<embedded>"
    const void     *load_options,
    size_t          load_options_size
    )
{
    /* Install LoadOptions BEFORE StartImage so DriverEntry sees them
       in EFI_LOADED_IMAGE_PROTOCOL.LoadOptions / .LoadOptionsSize.
       Skip silently when the caller didn't pass any (every existing
       caller pre-AxlService passes NULL/0 — this is an additive arg). */
    if (load_options != NULL && load_options_size > 0) {
        if (axl_driver_set_load_options(drv, load_options,
                                        load_options_size) != AXL_OK) {
            axl_debug("driver ensure: set_load_options failed for '%s'",
                      source_label);
            axl_driver_unload(drv);
            return AXL_ERR;
        }
    }

    EFI_STATUS st = (EFI_STATUS)axl_backend_start_image((void *)drv);
    load_options_retire(drv);   /* read by now; see axl_driver_start */
    axl_backend_exit_if_deferred();     /* after the retire; see axl_driver_start */

    if (EFI_ERROR(st) && st != EFI_ALREADY_STARTED) {
        axl_debug("driver ensure: StartImage failed for '%s': 0x%llx",
                  source_label, (unsigned long long)st);
        axl_driver_unload(drv);
        return AXL_ERR;
    }

    if (driver_protocol_registered(protocol_guid) == 0) {
        /* Verbose identity line: name + handle + published protocol GUID
         * + ImageBase/ImageSize + the (now non-NULL) device-path text. */
        EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
        axl_bs()->HandleProtocol(
            (EFI_HANDLE)drv, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li);
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        axl_bs()->HandleProtocol(
            (EFI_HANDLE)drv, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
            (void **)&dp);
        AXL_AUTO_FREE char *dptext =
            (dp != NULL) ? axl_device_path_to_text(dp) : NULL;
        const AxlGuid *g = protocol_guid;
        axl_debug("driver ensure: loaded '%s' handle=0x%llx "
                  "guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x "
                  "base=0x%llx size=0x%llx path=%s",
                  source_label, (unsigned long long)(uintptr_t)drv,
                  g->data1, g->data2, g->data3,
                  g->data4[0], g->data4[1], g->data4[2], g->data4[3],
                  g->data4[4], g->data4[5], g->data4[6], g->data4[7],
                  (li != NULL) ? (unsigned long long)(uintptr_t)li->ImageBase : 0ULL,
                  (li != NULL) ? (unsigned long long)li->ImageSize : 0ULL,
                  (dptext != NULL) ? dptext : "<none>");
        return AXL_OK;
    }

    /* Driver started but didn't register the expected protocol.
     * Unload to keep system state clean. */
    axl_debug("driver ensure: '%s' did not register protocol; unloading",
              source_label);
    axl_driver_unload(drv);
    return AXL_ERR;
}

/* Walk the candidate list, attempting to load+start each in order.
 * Returns 0 on first success, -1 if every candidate misses or fails. */
static int
driver_try_candidates(
    const AxlGuid *protocol_guid,
    char         **candidates,
    size_t         n_cand,
    const void    *load_options,
    size_t         load_options_size
    )
{
    for (size_t i = 0; i < n_cand; i++) {
        AxlFsEntry info;
        if (axl_file_info(candidates[i], &info) != AXL_OK || axl_fs_entry_is_dir(&info)) {
            axl_debug("  miss: %s", candidates[i]);
            continue;
        }
        axl_debug("  hit:  %s - attempting load", candidates[i]);

        /* Already checked not to be the running image when it was listed
           (driver_append_candidate): not walked a second time. */
        AxlDriverHandle drv = NULL;
        if (driver_load_path(candidates[i], &drv, true) != 0 || drv == NULL) {
            continue;
        }

        if (driver_start_and_verify(drv, protocol_guid, candidates[i],
                                    load_options, load_options_size) == 0) {
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

int
axl_driver_load_buffer(
    const unsigned char *buf,
    size_t               len,
    AxlDriverHandle     *out_handle
    )
{
    if (_axl_image_tpl_refused("axl_driver_load_buffer")) {
        if (out_handle != NULL) {
            *out_handle = NULL;
        }
        return AXL_UNSUPPORTED;
    }
    return driver_load_from_buffer(buf, len, /* file_dp */ NULL,
                                   /* info */ NULL, out_handle);
}

int
_axl_driver_load_buffer_at(
    const char      *path,
    const void      *buf,
    size_t           len,
    AxlDriverHandle *out
    )
{
    if (out != NULL) {
        *out = NULL;
    }
    if (path == NULL) {
        return AXL_ERR;
    }
    EFI_DEVICE_PATH_PROTOCOL *dp   = driver_build_file_dp(path);
    AxlEmbeddedImageInfo      info = { .file_name = driver_basename(path) };
    int rc = driver_load_from_buffer(buf, len, dp, &info, out);
    if (dp != NULL) {
        driver_free_dp(dp);
    }
    return rc;
}

int
axl_driver_load_buffer_with_image_info(
    const unsigned char        *buf,
    size_t                      len,
    const AxlEmbeddedImageInfo *info,
    AxlDriverHandle            *out_handle
    )
{
    if (_axl_image_tpl_refused("axl_driver_load_buffer_with_image_info")) {
        if (out_handle != NULL) {
            *out_handle = NULL;
        }
        return AXL_UNSUPPORTED;
    }
    return driver_load_from_buffer(buf, len, /* file_dp */ NULL, info,
                                   out_handle);
}

/* Fallback for a path-searched launch. Some UEFI shells set
 * LoadedImage->FilePath to the bare command name (not the resolved
 * \dir\name), so axl_app_image_path() loses the launcher's directory and the
 * primary sibling lookup collapses to the volume root -> AXL_NOT_FOUND. Recover
 * the launcher's real directory by re-running the shell's own `path` search for
 * argv0, then load @p file_name from THAT directory — strictly beside the
 * launcher the shell actually ran, so the sibling-only / version-pinning
 * contract still holds (never a copy from anywhere but the launcher's own dir).
 *
 * Real-hardware-only: OVMF's EDK II Shell does not path-search .efi, so this
 * path is verified on target hardware (see the R7725 probe in
 * local/docs/handoff-sibling-locate-path-search.md), not in the QEMU suite.
 * Returns AXL_NOT_FOUND when it cannot anchor (no argv0 / no %path% / launcher
 * not on the path / driver not beside it). */
static int
load_sibling_via_shell_path(
    const char       *file_name,
    AxlDriverHandle  *out_handle
    )
{
    const char *argv0 = axl_app_argv0();
    /* axl_getenv hands back an OWNED copy, not the shell's live string —
     * every return below is an exit from this function, so the ownership
     * has to be attached to the variable rather than remembered. */
    AXL_AUTO_FREE char *path = axl_getenv("path");
    if (argv0 == NULL || argv0[0] == '\0' || path == NULL) {
        return AXL_NOT_FOUND;
    }

    /* The shell resolves a bare command by appending ".efi"; argv[0] is the
     * name as typed ("do"). Search for "<argv0>.efi" unless it already ends
     * in .efi. */
    const char        *ext = axl_path_extension(argv0);
    AXL_AUTO_FREE char *launcher_name = NULL;
    if (ext != NULL && axl_strcasecmp(ext, "efi") == 0) {
        launcher_name = axl_strdup(argv0);
    } else {
        size_t n = axl_strlen(argv0);
        launcher_name = axl_malloc(n + 5);   /* ".efi" + NUL */
        if (launcher_name != NULL) {
            axl_memcpy(launcher_name, argv0, n);
            axl_memcpy(launcher_name + n, ".efi", 5);
        }
    }
    if (launcher_name == NULL) {
        return AXL_ERR;
    }

    AXL_AUTO_FREE char *launcher_full = NULL;
    if (axl_path_search(path, launcher_name, &launcher_full) != AXL_OK) {
        return AXL_NOT_FOUND;   /* launcher not on %path% — can't anchor */
    }

    /* Driver strictly beside the launcher (same directory). */
    AXL_AUTO_FREE char *driver_full = axl_path_companion(launcher_full, file_name);
    if (driver_full == NULL) {
        return AXL_ERR;
    }
    AxlFsEntry entry;
    if (axl_file_info(driver_full, &entry) != AXL_OK
        || axl_fs_entry_is_dir(&entry)) {
        return AXL_NOT_FOUND;
    }
    return axl_driver_load(driver_full, out_handle);
}

AxlStatus
axl_driver_load_sibling(
    const char       *file_name,
    AxlDriverHandle  *out_handle
    )
{
    if (_axl_image_tpl_refused("axl_driver_load_sibling")) {
        if (out_handle != NULL) {
            *out_handle = NULL;
        }
        return AXL_UNSUPPORTED;
    }
    if (file_name == NULL || out_handle == NULL) {
        if (out_handle != NULL) {
            *out_handle = NULL;
        }
        return AXL_ERR;
    }
    *out_handle = NULL;

    /* Bare basename only — a separator or drive prefix could escape the
     * app directory, which is exactly what this entry point refuses. */
    if (file_name[0] == '\0') {
        return AXL_INVALID;
    }
    for (const char *c = file_name; *c != '\0'; c++) {
        if (*c == '/' || *c == '\\' || *c == ':') {
            return AXL_INVALID;
        }
    }

    /* The image's own name -- in any spelling the filesystem accepts -- is
       the running image, not a sibling. */
    AXL_AUTO_FREE char *self_sib = axl_malloc(AXL_FS_REL_MAX);
    if (self_sib != NULL
        && axl_app_image_dir_path(file_name, self_sib, AXL_FS_REL_MAX) == AXL_OK
        && driver_path_is_self(self_sib)) {
        return driver_refuse_self(self_sib);
    }

    /* Primary: shell-independent LoadedImage device-path load. The sibling's
       directory and volume both come from THIS image's LoadedImage, so it
       needs no EFI_SHELL_PROTOCOL — it works on the old EFI 1.x shell, BDS,
       and path-searched launches uniformly, where the shell-based text path
       below has nothing to anchor to. LoadImage validates existence, so a
       driver not actually staged beside the launcher fails here and drops to
       the fallback (preserving the sibling-only hard-fail contract). */
    EFI_DEVICE_PATH_PROTOCOL *sib_dp = driver_build_sibling_dp(file_name);
    if (sib_dp != NULL) {
        DriverDpResult r = driver_load_dp(sib_dp, file_name, out_handle);
        driver_free_dp(sib_dp);
        if (r == DRIVER_DP_LOADED) {
            return AXL_OK;
        }
        if (r == DRIVER_DP_REFUSED) {
            return AXL_ERR;   /* a packed sibling that cannot load: final */
        }
        axl_debug("sibling load: LoadedImage-DP load of '%s' failed; "
                  "falling back to shell path resolution", file_name);
    }

    /* Fallback: shell-based path resolution. Retained until the DP-primary
       is confirmed to cover the path-searched-launch case on the modern
       shell (then load_sibling_via_shell_path can retire). Anchor, not
       axl_app_image_path: "beside the launcher" is an ancestor-directory
       question, and a buffer-loaded launcher has no file of its own. */
    const char *ip = _axl_app_image_anchor();
    if (ip == NULL) {
        return AXL_ERR;  /* network / RAM-disk boot: no filesystem anchor */
    }

    /* Directory = everything up to and including the last separator. */
    size_t ip_len = axl_strlen(ip);
    size_t dir_len = 0;
    for (size_t i = 0; i < ip_len; i++) {
        if (ip[i] == '\\' || ip[i] == '/' || ip[i] == ':') {
            dir_len = i + 1;
        }
    }

    size_t fn_len = axl_strlen(file_name);
    AXL_AUTO_FREE char *full = axl_malloc(dir_len + fn_len + 1);
    if (full == NULL) {
        return AXL_ERR;
    }
    axl_memcpy(full, ip, dir_len);
    axl_memcpy(full + dir_len, file_name, fn_len + 1);

    /* Restrict to a real, present file in the app directory. */
    AxlFsEntry entry;
    if (axl_file_info(full, &entry) != AXL_OK || axl_fs_entry_is_dir(&entry)) {
        /* Primary miss. On a path-searched launch the image path can lack the
         * launcher's directory (bare LoadedImage->FilePath), collapsing `full`
         * to the volume root; recover the real directory from the shell `path`
         * and load the driver from beside the launcher there. */
        return load_sibling_via_shell_path(file_name, out_handle);
    }

    return axl_driver_load(full, out_handle);
}

/* LoadImage from a memory buffer + start + verify the protocol got
 * registered. Used by the embedded-driver fallback so tools work on
 * firmware that ships neither the protocol nor a user-staged copy on
 * disk. @p info / @p default_name give the loaded image a non-NULL device
 * path (default_name is the driver filename on the shared-driver path). */
static int
driver_load_embedded(
    const AxlGuid              *protocol_guid,
    const unsigned char        *buf,
    size_t                      len,
    const void                 *load_options,
    size_t                      load_options_size,
    const AxlEmbeddedImageInfo *info,
    const char                 *default_name
    )
{
    /* default_name stands in for a file_name the caller's info left unset. */
    AxlEmbeddedImageInfo named = { 0 };
    if (info != NULL) {
        named = *info;
    }
    if (named.file_name == NULL) {
        named.file_name = default_name;
    }
    AxlDriverHandle drv = NULL;
    if (driver_load_from_buffer(buf, len, /* file_dp */ NULL, &named, &drv)
        != AXL_OK) {
        return AXL_ERR;
    }

    return driver_start_and_verify(drv, protocol_guid, "<embedded>",
                                   load_options, load_options_size);
}

int
_axl_driver_ensure_with_embedded_info(
    const char                 *api,
    const AxlGuid              *protocol_guid,
    const char                 *driver_name,
    const unsigned char        *embedded_buf,
    size_t                      embedded_len,
    const char                 *override_name,
    const void                 *load_options,
    size_t                      load_options_size,
    const AxlEmbeddedImageInfo *info
    )
{
    if (protocol_guid == NULL || driver_name == NULL) {
        return AXL_ERR;
    }

    /* Step 1: short-circuit if the protocol is already registered.
     * On most OEM firmware the corresponding driver is in the
     * firmware volume and dispatched at DXE init, so this is the
     * common path — no disk search, no embedded fallback. The
     * already-published instance isn't ours to re-configure, so
     * load_options is silently ignored on this path. */
    if (driver_protocol_registered(protocol_guid) == 0) {
        axl_debug("driver ensure: protocol already registered, skipping search");
        return AXL_OK;
    }
    /* Published already is an answer at any TPL; loading it is not. */
    if (_axl_image_tpl_refused(api)) {
        return AXL_UNSUPPORTED;
    }

    /* Step 2/3: disk search. If override_name was passed, search for
     * that exact name and skip the embedded fallback (caller opted
     * into a specific external driver). Otherwise, search for the
     * canonical name and fall through to embedded on miss. */
    const char *search_name = (override_name != NULL) ? override_name : driver_name;

    char  *candidates[DRIVER_MAX_CANDIDATES];
    size_t n_cand = 0;
    driver_build_candidates(search_name, candidates, &n_cand);

    axl_debug("driver ensure: searching %zu candidate path%s for '%s'",
              n_cand, n_cand == 1 ? "" : "s", search_name);
    for (size_t i = 0; i < n_cand; i++) {
        axl_debug("  [%zu] %s", i, candidates[i]);
    }

    int rc = driver_try_candidates(protocol_guid, candidates, n_cand,
                                   load_options, load_options_size);

    for (size_t i = 0; i < n_cand; i++) {
        axl_free(candidates[i]);
    }

    if (rc == 0) {
        return AXL_OK;
    }

    /* Disk search failed. Try the embedded blob unless the caller
     * gave an override (in which case "user said use this specific
     * file" is a stronger signal than "fall back to whatever the
     * build embedded"). */
    if (override_name == NULL && embedded_buf != NULL && embedded_len > 0) {
        axl_debug("driver ensure: disk search exhausted, "
                  "trying embedded fallback (%zu bytes)", embedded_len);
        if (driver_load_embedded(protocol_guid, embedded_buf, embedded_len,
                                 load_options, load_options_size,
                                 info, driver_name) == 0) {
            return AXL_OK;
        }
    }

    axl_debug("driver ensure: '%s' not found in %zu candidate path%s%s",
              search_name, n_cand, n_cand == 1 ? "" : "s",
              (override_name == NULL && embedded_buf != NULL)
                  ? " (embedded fallback also failed)" : "");
    return AXL_NOT_FOUND;
}

AxlStatus
axl_driver_ensure_with_embedded(
    const AxlGuid       *protocol_guid,
    const char          *driver_name,
    const unsigned char *embedded_buf,
    size_t               embedded_len,
    const char          *override_name,
    const void          *load_options,
    size_t               load_options_size
    )
{
    return _axl_driver_ensure_with_embedded_info(
        "axl_driver_ensure_with_embedded",
        protocol_guid, driver_name, embedded_buf, embedded_len,
        override_name, load_options, load_options_size,
        /* info */ NULL);
}

AxlStatus
axl_driver_ensure_from_path(
    const AxlGuid *protocol_guid,
    const char    *driver_path,
    const void    *load_options,
    size_t         load_options_size
    )
{
    if (protocol_guid == NULL || driver_path == NULL) {
        return AXL_INVALID;
    }

    /* Same step-1 short-circuit as the searching variants: an already
     * published protocol is not ours to re-configure, so load_options is
     * deliberately ignored on this path. */
    if (driver_protocol_registered(protocol_guid) == AXL_OK) {
        axl_debug("driver ensure_from_path: protocol already registered, "
                  "skipping load of '%s'", driver_path);
        return AXL_OK;
    }
    if (_axl_image_tpl_refused("axl_driver_ensure_from_path")) {
        return AXL_UNSUPPORTED;
    }

    AxlDriverHandle drv = NULL;
    if (axl_driver_load(driver_path, &drv) != AXL_OK || drv == NULL) {
        axl_debug("driver ensure_from_path: cannot load pinned driver '%s'",
                  driver_path);
        return AXL_NOT_FOUND;
    }

    /* No search and no embedded fallback by construction: the caller named
     * the one file it wants, so a failure here is the answer, not a reason
     * to go looking. driver_start_and_verify unloads on failure. */
    if (driver_start_and_verify(drv, protocol_guid, driver_path,
                                load_options, load_options_size) != AXL_OK) {
        return AXL_NOT_FOUND;
    }
    return AXL_OK;
}

AxlStatus
axl_driver_ensure_embedded_only(
    const AxlGuid       *protocol_guid,
    const unsigned char *embedded_buf,
    size_t               embedded_len,
    const char          *driver_name,
    const void          *load_options,
    size_t               load_options_size
    )
{
    if (protocol_guid == NULL || embedded_buf == NULL || embedded_len == 0) {
        return AXL_INVALID;
    }

    /* Same step-1 short-circuit as the searching variants: an already
     * published protocol is not ours to re-configure, so load_options is
     * deliberately ignored on this path. */
    if (driver_protocol_registered(protocol_guid) == AXL_OK) {
        axl_debug("driver ensure_embedded_only: protocol already registered, "
                  "skipping embedded load");
        return AXL_OK;
    }
    if (_axl_image_tpl_refused("axl_driver_ensure_embedded_only")) {
        return AXL_UNSUPPORTED;
    }

    /* Load the blob directly — NO disk search, so a stale loose copy of
     * driver_name beside the launcher cannot shadow it. driver_load_embedded
     * synthesizes a device path from driver_name (may be NULL -> a sensible
     * default) and unloads the image on a start/verify failure. */
    if (driver_load_embedded(protocol_guid, embedded_buf, embedded_len,
                             load_options, load_options_size,
                             /* info */ NULL, driver_name) != AXL_OK) {
        axl_debug("driver ensure_embedded_only: embedded driver (%zu bytes) "
                  "failed to load or register", embedded_len);
        return AXL_NOT_FOUND;
    }
    return AXL_OK;
}

AxlStatus
axl_driver_ensure(
    const AxlGuid *protocol_guid,
    const char    *driver_name
    )
{
    return _axl_driver_ensure_with_embedded_info(
        "axl_driver_ensure",
        protocol_guid, driver_name,
        NULL, 0,        /* no embedded blob */
        NULL,           /* no override */
        NULL, 0,        /* no LoadOptions */
        NULL);          /* no image info */
}

// ---------------------------------------------------------------------------
// axl_driver_load_dir
// ---------------------------------------------------------------------------

static int
driver_load_cb(const char *full_path, const AxlFsEntry *entry, void *user)
{
    DriverLoadCtx *c = (DriverLoadCtx *)user;
    if (axl_fs_entry_is_dir(entry) || !axl_fnmatch(c->pattern, entry->name)) {
        return AXL_OK;
    }
    if (c->guard != NULL && axl_attempt_is_quarantined(c->guard, entry->name)) {
        axl_warning("skipping quarantined driver: %s", entry->name);
        return AXL_OK;
    }
    /* Breadcrumb BEFORE the load: if this image hangs or resets the box,
       the surviving breadcrumb is the only thing that names it next boot. */
    if (c->guard != NULL) {
        axl_attempt_begin(c->guard, entry->name);
    }
    AxlDriverHandle drv;
    if (axl_driver_load(full_path, &drv) == 0) {
        if (axl_driver_start(drv) == 0) {
            axl_driver_connect(drv);
            c->loaded++;
            axl_debug("loaded driver: %s", entry->name);
        } else {
            axl_driver_unload(drv);
            axl_warning("failed to start: %s", entry->name);
        }
    }
    /* Survived — load, start, connect and any unload are all inside the
       breadcrumb window, since a hang in any of them is this driver's fault. */
    if (c->guard != NULL) {
        axl_attempt_end(c->guard);
    }
    return AXL_OK;
}

static int
driver_load_dir_impl(
    const char       *api,
    const char       *dir_path,
    const char       *pattern,
    const AxlAttempt *guard,
    size_t           *loaded_count)
{
    if (_axl_image_tpl_refused(api)) {
        if (loaded_count != NULL) {
            *loaded_count = 0;
        }
        return AXL_UNSUPPORTED;
    }
    if (dir_path == NULL) {
        return AXL_ERR;
    }

    /* Heal a prior crash first, so a driver that wedged the box last boot is
       on the quarantine list before the walk below can reach it again. */
    if (guard != NULL) {
        /* Sized from the guard's own bound, not a fixed local: a breadcrumb
           longer than the read buffer is not truncated, it is unreadable —
           and an unreadable breadcrumb never recovers and never clears. */
        char *culprit = (char *)axl_calloc(1, guard->name_max);
        if (culprit == NULL) {
            axl_warning("out of memory; skipping crash recovery for this sweep");
        } else {
            if (axl_attempt_recover(guard, culprit, guard->name_max) == 1) {
                axl_warning("last run did not survive loading %s -- quarantining it",
                            culprit);
            }
            axl_free(culprit);
        }
    }

    DriverLoadCtx ctx = {
        .pattern = (pattern != NULL) ? pattern : "*.efi",
        .guard   = guard,
        .loaded  = 0,
    };

    /* max_depth=1 — list immediate children only, no recursion.
       A missing directory is "not an error, just 0 loaded" per the
       previous contract; axl_dir_walk returns -1 for that case
       which we silently translate. */
    axl_dir_walk(dir_path, driver_load_cb, &ctx, 1);

    if (loaded_count != NULL) {
        *loaded_count = ctx.loaded;
    }
    return AXL_OK;
}

int
axl_driver_load_dir(
    const char *dir_path,
    const char *pattern,
    size_t     *loaded_count)
{
    return driver_load_dir_impl("axl_driver_load_dir", dir_path, pattern, NULL,
                                loaded_count);
}

int
axl_driver_load_dir_guarded(
    const char       *dir_path,
    const char       *pattern,
    const AxlAttempt *guard,
    size_t           *loaded_count)
{
    return driver_load_dir_impl("axl_driver_load_dir_guarded", dir_path, pattern,
                                guard, loaded_count);
}

// ===================================================================
// Protocol publishing — public surface over the backend seam.
// ===================================================================

int
axl_protocol_install(
    const AxlGuid  *guid,
    void           *iface,
    AxlHandle      *handle
    )
{
    if (handle == NULL || guid == NULL || iface == NULL) {
        return AXL_ERR;
    }
    return axl_backend_install_protocol((void **)handle, guid, iface);
}

int
axl_protocol_uninstall(
    AxlHandle       handle,
    const AxlGuid  *guid,
    void           *iface
    )
{
    if (handle == NULL || guid == NULL || iface == NULL) {
        return AXL_ERR;
    }
    return axl_backend_uninstall_protocol(handle, guid, iface);
}

// ===================================================================
// Driver Model binding (Type B) — the managed thunks + install.
// ===================================================================
//
// AXL owns an AxlBindingRec per installed binding. The firmware-facing
// EFI_DRIVER_BINDING_PROTOCOL and EFI_COMPONENT_NAME2_PROTOCOL are embedded
// in it, so an EFIAPI thunk recovers the record from `This`: the binding is
// the FIRST member (cast directly); name2 is recovered by offset. The record
// is retained for the binding's lifetime (the firmware holds pointers into
// it).

#define AXL_DB_VERSION  0x10u   // default EFI_DRIVER_BINDING_PROTOCOL.Version

typedef struct {
    EFI_DRIVER_BINDING_PROTOCOL   binding;     // FIRST: thunks cast (rec *)This
    EFI_COMPONENT_NAME2_PROTOCOL  name2;
    AxlDriverBinding              db;          // copied descriptor
    EFI_GUID                      binds;       // copied GUID value (open target)
    unsigned short               *name_ucs2;   // driver name as CHAR16
} AxlBindingRec;

// Supported: OpenProtocol(BY_DRIVER) on `binds` as a test (this also reports
// EFI_ALREADY_STARTED if we already manage the controller), close it, then
// the optional consumer gate.
static EFI_STATUS EFIAPI
db_supported(EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ctrl,
             EFI_DEVICE_PATH_PROTOCOL *rdp)
{
    (void)rdp;   // v1: no RemainingDevicePath (bus drivers are v2)
    AxlBindingRec *r = (AxlBindingRec *)This;
    void *iface = NULL;
    EFI_STATUS s = axl_bs()->OpenProtocol(ctrl, &r->binds, &iface,
                                          This->DriverBindingHandle, ctrl,
                                          EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (s == EFI_ALREADY_STARTED) {
        return EFI_ALREADY_STARTED;
    }
    if (EFI_ERROR(s)) {
        return s;   // `binds` absent or not bindable here → unsupported
    }
    axl_bs()->CloseProtocol(ctrl, &r->binds, This->DriverBindingHandle, ctrl);
    if (r->db.supported != NULL
        && !r->db.supported((AxlHandle)ctrl, r->db.ctx)) {
        return EFI_UNSUPPORTED;
    }
    return EFI_SUCCESS;
}

// Start: claim `binds` BY_DRIVER (tagging ownership, getting the interface),
// hand it to the consumer; roll back the open if the consumer fails.
static EFI_STATUS EFIAPI
db_start(EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ctrl,
         EFI_DEVICE_PATH_PROTOCOL *rdp)
{
    (void)rdp;
    AxlBindingRec *r = (AxlBindingRec *)This;
    void *iface = NULL;
    EFI_STATUS s = axl_bs()->OpenProtocol(ctrl, &r->binds, &iface,
                                          This->DriverBindingHandle, ctrl,
                                          EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(s)) {
        return s;
    }
    int rc = r->db.start((AxlHandle)ctrl, iface, r->db.ctx);
    if (rc != AXL_OK) {
        axl_bs()->CloseProtocol(ctrl, &r->binds, This->DriverBindingHandle,
                                ctrl);
        return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
}

// Stop: let the consumer tear down, then release the BY_DRIVER open.
static EFI_STATUS EFIAPI
db_stop(EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ctrl,
        UINTN n_children, EFI_HANDLE *children)
{
    (void)n_children; (void)children;   // v1: device driver, no children
    AxlBindingRec *r = (AxlBindingRec *)This;
    int rc = r->db.stop((AxlHandle)ctrl, r->db.ctx);
    axl_bs()->CloseProtocol(ctrl, &r->binds, This->DriverBindingHandle, ctrl);
    return (rc == AXL_OK) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI
db_get_driver_name(EFI_COMPONENT_NAME2_PROTOCOL *This, CHAR8 *Language,
                   CHAR16 **DriverName)
{
    // Intentionally permissive: return the one name for any requested
    // language rather than gating on SupportedLanguages ("en"). The shell
    // driver listing passes the current language and tolerates this; a strict
    // CN2 probe of an unsupported language gets the name instead of
    // EFI_UNSUPPORTED. Acceptable for a query-only, single-name v1.
    (void)Language;
    if (DriverName == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    AxlBindingRec *r = (AxlBindingRec *)((char *)This
                       - __builtin_offsetof(AxlBindingRec, name2));
    *DriverName = r->name_ucs2;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
db_get_controller_name(EFI_COMPONENT_NAME2_PROTOCOL *This,
                       EFI_HANDLE ctrl, EFI_HANDLE child, CHAR8 *Language,
                       CHAR16 **ControllerName)
{
    (void)This; (void)ctrl; (void)child; (void)Language; (void)ControllerName;
    return EFI_UNSUPPORTED;   // v1: no per-controller names
}

// v1 tracks one binding per image handle. These hold the live record and its
// axl_atexit handle so axl_driver_binding_uninstall can remove it explicitly
// (the driver-unload path) and reject a duplicate install.
static AxlBindingRec *g_db_rec;
static uint32_t       g_db_atexit;

// Shared teardown: uninstall the tracked binding's protocols and free the
// record. Returns AXL_OK if the record was removed, AXL_ERR if there is none
// or the firmware still references the binding.
static int
db_teardown(void)
{
    AxlBindingRec *r = g_db_rec;
    if (r == NULL) {
        return AXL_ERR;
    }
    EFI_HANDLE image = r->binding.DriverBindingHandle;
    /* Uninstall the binding FIRST — it is the protocol the firmware can hold
     * BY_DRIVER. If it is still referenced (a controller is bound), bail with
     * BOTH protocols still installed and the record kept alive: freeing here
     * would leave the EFIAPI thunks dangling into freed memory (a far worse,
     * crash-on-next-ConnectController bug), and removing Component Name 2 first
     * would briefly leave the binding nameless in the `drivers` listing.
     * Disconnect the controller and retry. */
    if (axl_protocol_uninstall(
            (AxlHandle)image,
            (const AxlGuid *)&EFI_DRIVER_BINDING_PROTOCOL_GUID,
            &r->binding) != AXL_OK) {
        return AXL_ERR;   // keep the record alive — the firmware still points at it
    }
    axl_protocol_uninstall(
        (AxlHandle)image,
        (const AxlGuid *)&EFI_COMPONENT_NAME2_PROTOCOL_GUID, &r->name2);
    axl_free(r->name_ucs2);
    axl_free(r);
    g_db_rec = NULL;
    return AXL_OK;
}

// axl_atexit hook — the safety net, on BOTH exit paths now. It used to fire
// only at app exit, because axl_driver_init never initialised the atexit table
// and the registration below silently returned 0. Since axl_driver_init runs
// .init_array (and therefore must init that table first), a driver's unload
// drains it too, via axl_driver_cleanup.
//
// That makes this a real net for a driver rather than a dead registration, and
// it does NOT double-run: axl_driver_binding_uninstall removes the hook after a
// successful teardown, and if it is reached anyway db_teardown is idempotent —
// a NULL g_db_rec returns AXL_ERR without freeing anything.
//
// A driver should still uninstall explicitly from its unload callback: the
// consumer's unload runs BEFORE this drain, so a binding the firmware still
// references fails teardown while the consumer can still report it.
static void
db_cleanup(void *p)
{
    (void)p;            // the record is tracked in g_db_rec
    db_teardown();
}

int
axl_driver_binding_install(const AxlDriverBinding *db)
{
    if (db == NULL || db->name == NULL || db->binds == NULL
        || db->start == NULL || db->stop == NULL) {
        return AXL_ERR;
    }
    if (g_db_rec != NULL) {
        return AXL_ERR;   // v1: one binding per image (the firmware would also
                          // reject a duplicate EFI_DRIVER_BINDING_PROTOCOL)
    }
    AxlBindingRec *r = axl_calloc(1, sizeof(*r));
    if (r == NULL) {
        return AXL_ERR;
    }
    r->db = *db;
    axl_memcpy(&r->binds, db->binds, sizeof(EFI_GUID));
    r->name_ucs2 = axl_utf8_to_ucs2(db->name);
    if (r->name_ucs2 == NULL) {
        axl_free(r);
        return AXL_ERR;
    }

    EFI_HANDLE image = gImageHandle;
    r->binding.Supported           = db_supported;
    r->binding.Start               = db_start;
    r->binding.Stop                = db_stop;
    r->binding.Version             = AXL_DB_VERSION;
    r->binding.ImageHandle         = image;
    r->binding.DriverBindingHandle = image;   // single binding on the image
    r->name2.GetDriverName      = db_get_driver_name;
    r->name2.GetControllerName  = db_get_controller_name;
    r->name2.SupportedLanguages = (CHAR8 *)"en";

    AxlHandle h = (AxlHandle)image;   // install onto the existing image handle
    if (axl_protocol_install((const AxlGuid *)&EFI_DRIVER_BINDING_PROTOCOL_GUID,
                             &r->binding, &h) != AXL_OK) {
        axl_free(r->name_ucs2);
        axl_free(r);
        return AXL_ERR;
    }
    if (axl_protocol_install((const AxlGuid *)&EFI_COMPONENT_NAME2_PROTOCOL_GUID,
                             &r->name2, &h) != AXL_OK) {
        axl_protocol_uninstall(
            (AxlHandle)image,
            (const AxlGuid *)&EFI_DRIVER_BINDING_PROTOCOL_GUID, &r->binding);
        axl_free(r->name_ucs2);
        axl_free(r);
        return AXL_ERR;
    }
    // Track for explicit uninstall (driver-unload path) and register the
    // safety-net hook, which now drains on a driver unload too (see db_cleanup).
    // Uninstalling explicitly from the unload callback is still the right shape:
    // it runs before the drain, so a still-referenced binding is reportable.
    g_db_rec = r;
    g_db_atexit = axl_atexit(db_cleanup, r);
    return AXL_OK;
}

int
axl_driver_binding_uninstall(void)
{
    if (g_db_rec == NULL) {
        return AXL_ERR;   // nothing installed
    }
    uint32_t hook = g_db_atexit;
    if (db_teardown() != AXL_OK) {
        return AXL_ERR;   // still referenced — record kept alive by db_teardown
    }
    // Removed explicitly; drop the safety-net hook so it can't double-run.
    axl_atexit_remove(hook);
    g_db_atexit = 0;
    return AXL_OK;
}
