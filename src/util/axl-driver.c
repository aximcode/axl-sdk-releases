/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-driver.c
    UEFI driver lifecycle — load, start, connect, disconnect, unload.

    Uses gBS->LoadImage/StartImage directly (not Shell "load" command)
    so load options can be set between load and start.
**/

#include "../backend/axl-backend.h"
#include "axl-image-internal.h"   /* _axl_init_image_path */
#include "axl-driver-internal.h"  /* _axl_driver_ensure_with_embedded_info */
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

#if defined(__x86_64__)
static const char driver_arch[] = "x64";
#elif defined(__aarch64__)
static const char driver_arch[] = "aa64";
#else
#error "axl-driver: unsupported architecture"
#endif

typedef struct {
    const char *pattern;
    size_t      loaded;
} DriverLoadCtx;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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
    if (axl_bs()->AllocatePool(EfiBootServicesData, total, &out)
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
    if (axl_bs()->AllocatePool(EfiBootServicesData, total, &out) != EFI_SUCCESS
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
 * standalone copy. axl_driver_unload must uninstall + free both BEFORE
 * UnloadImage — otherwise the firmware's own unload matches its (stale/NULL)
 * internal device-path pointer, not our interface, so ours is never removed
 * and the handle survives carrying only the synth device path (it accumulates
 * in `dh` on every load/unload cycle).
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
 * device path + the standalone FilePath copy, returning the handle to its
 * pre-synthesis state. Shared by image_dp_release (normal unload) and
 * image_dp_track's failure rollback. Clears li->FilePath first so the firmware
 * can't read/free a dangling pointer during UnloadImage. */
static void
image_dp_teardown(EFI_HANDLE handle, EFI_DEVICE_PATH_PROTOCOL *dp,
                  EFI_DEVICE_PATH_PROTOCOL *file_path)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (axl_bs()->HandleProtocol(handle, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                 (void **)&li) == EFI_SUCCESS && li != NULL) {
        li->FilePath = NULL;
    }
    axl_bs()->UninstallProtocolInterface(
        handle, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID, dp);
    axl_bs()->FreePool(dp);
    if (file_path != NULL) {
        axl_bs()->FreePool(file_path);
    }
}

static void
image_dp_track(EFI_HANDLE handle, EFI_DEVICE_PATH_PROTOCOL *dp,
               EFI_DEVICE_PATH_PROTOCOL *file_path)
{
    AxlImageDpRecord *rec = NULL;
    if (axl_bs()->AllocatePool(EfiBootServicesData, sizeof(*rec),
                               (void **)&rec) != EFI_SUCCESS || rec == NULL) {
        rec = NULL;   /* AllocatePool may leave *rec untouched on failure */
    } else {
        rec->dp        = dp;
        rec->file_path = file_path;
        if (EFI_ERROR(axl_bs()->InstallProtocolInterface(
                &handle, (EFI_GUID *)&AXL_IMAGE_DP_RECORD_GUID,
                EFI_NATIVE_INTERFACE, rec))) {
            axl_bs()->FreePool(rec);
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

/* Uninstall + free the synthesized device path for @p handle, if any. Called
 * from axl_driver_unload BEFORE UnloadImage so our protocol entries are removed
 * cleanly (the firmware's own uninstall of its stale internal pointer then
 * no-ops instead of mismatching). Works from any image — the record is read
 * off the handle, not a process-local table. */
static void
image_dp_release(AxlDriverHandle handle)
{
    AxlImageDpRecord *rec = NULL;
    if (axl_bs()->HandleProtocol((EFI_HANDLE)handle,
                                 (EFI_GUID *)&AXL_IMAGE_DP_RECORD_GUID,
                                 (void **)&rec) != EFI_SUCCESS || rec == NULL) {
        return;   /* not a synthesized-device-path image — nothing to do */
    }

    /* The record GUID is private and never opened via OpenProtocol, so this
     * uninstall cannot be denied; freeing rec below is therefore safe. */
    axl_bs()->UninstallProtocolInterface(
        (EFI_HANDLE)handle, (EFI_GUID *)&AXL_IMAGE_DP_RECORD_GUID, rec);
    image_dp_teardown((EFI_HANDLE)handle, rec->dp, rec->file_path);
    axl_bs()->FreePool(rec);
}

/* Give a freshly buffer-loaded image a real identity so the shell never
 * renders a NULL device path. @p info may be NULL; @p default_name is the
 * fallback leaf when the caller named nothing (the driver/app filename). */
static void
driver_apply_image_identity(
    EFI_HANDLE                  drv_handle,
    const AxlEmbeddedImageInfo *info,
    const char                 *default_name
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (axl_bs()->HandleProtocol(drv_handle, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                 (void **)&li) != EFI_SUCCESS || li == NULL)
    {
        axl_warning("driver load: cannot read LoadedImage to set identity");
        return;
    }

    const char *file_name = (info != NULL && info->file_name != NULL)
                                ? info->file_name : default_name;
    if (file_name == NULL || file_name[0] == '\0') {
        const char *ip = axl_app_image_path();
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
        axl_bs()->FreePool(full);
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
        && axl_bs()->AllocatePool(EfiBootServicesData, tail_size, &fp_buf)
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

/* Shared core: LoadImage from a buffer, then synthesize image identity. */
static int
driver_load_buffer_apply(
    const unsigned char        *buf,
    size_t                      len,
    const AxlEmbeddedImageInfo *info,
    const char                 *default_name,
    AxlDriverHandle            *out_handle
    )
{
    if (buf == NULL || len == 0 || out_handle == NULL) {
        if (out_handle != NULL) {
            *out_handle = NULL;
        }
        return AXL_ERR;
    }

    EFI_HANDLE drv_handle = NULL;
    EFI_STATUS st = axl_bs()->LoadImage(
        FALSE,                          /* BootPolicy */
        gImageHandle,                   /* ParentImageHandle */
        NULL,                           /* DevicePath (none — pure mem load) */
        (void *)(uintptr_t)buf,         /* SourceBuffer */
        len,                            /* SourceSize */
        &drv_handle);

    if (EFI_ERROR(st) || drv_handle == NULL) {
        axl_warning("driver load_buffer: LoadImage(%zu bytes) failed: 0x%llx",
                    len, (unsigned long long)st);
        *out_handle = NULL;
        return AXL_ERR;
    }

    driver_apply_image_identity(drv_handle, info, default_name);

    *out_handle = (AxlDriverHandle)drv_handle;
    return AXL_OK;
}

int
axl_driver_load(
    const char       *path,
    AxlDriverHandle  *handle
    )
{
    EFI_STATUS status;
    EFI_HANDLE image = NULL;

    if (path == NULL || handle == NULL) {
        return AXL_ERR;
    }

    *handle = NULL;


    /* Prefer DevicePath load. The firmware reads the file from the
     * volume's filesystem itself, sets LoadedImage->FilePath to the
     * full DP we just built, and StartImage sees the FilePath that
     * driver-binding-style drivers (notably iPXE) need to locate
     * their own install directory. */
    EFI_DEVICE_PATH_PROTOCOL *file_dp = driver_build_file_dp(path);
    if (file_dp != NULL) {
        status = axl_bs()->LoadImage(
            FALSE,           /* BootPolicy */
            gImageHandle,    /* ParentImageHandle */
            file_dp,         /* DevicePath */
            NULL, 0,         /* No source buffer; firmware reads via DP */
            &image);
        axl_bs()->FreePool(file_dp);

        if (!EFI_ERROR(status)) {
            *handle = (AxlDriverHandle)image;
            return AXL_OK;
        }
        axl_debug("driver load: DevicePath LoadImage failed for '%s': 0x%llx; "
                  "falling back to buffer load",
                  path, (unsigned long long)status);
    }

    /* Fallback: read the file ourselves and load from buffer. Works
     * for drivers that don't read LoadedImage->FilePath at startup
     * (e.g. RamDiskDxe). Drivers that DO need it (iPXE) will fail
     * StartImage with EFI_INVALID_PARAMETER on this path; the caller
     * already logs that case. */
    void *buf = NULL;
    size_t buf_size = 0;
    if (axl_file_get_contents(path, &buf, &buf_size) != AXL_OK || buf == NULL) {
        axl_warning("driver load: cannot read '%s'", path);
        return AXL_ERR;
    }

    status = axl_bs()->LoadImage(
        FALSE, gImageHandle, NULL,
        buf, (size_t)buf_size,
        &image);

    axl_free(buf);

    if (EFI_ERROR(status)) {
        axl_warning("driver load: LoadImage failed for '%s': 0x%llx",
                   path, (unsigned long long)status);
        return AXL_ERR;
    }

    /* This fallback also loaded with DevicePath=NULL, so synthesize a
     * non-NULL device path the same way the buffer path does — keyed off
     * the file's basename. Otherwise a path load that falls back to buffer
     * mode would re-introduce the NULL-device-path shell fault. */
    driver_apply_image_identity(image, NULL, driver_basename(path));

    *handle = (AxlDriverHandle)image;
    return AXL_OK;
}

int
axl_driver_start(
    AxlDriverHandle handle
    )
{
    EFI_STATUS status;
    size_t exit_data_size = 0;

    if (handle == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->StartImage(
        (EFI_HANDLE)handle,
        &exit_data_size,
        NULL);

    if (EFI_ERROR(status)) {
        axl_warning("driver start failed: 0x%llx",
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

    for (UINTN i = 0; i < count; i++) {
        axl_bs()->ConnectController(handles[i], NULL, NULL, TRUE);
    }
    axl_bs()->FreePool(handles);
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

int
axl_driver_unload(
    AxlDriverHandle handle
    )
{
    EFI_STATUS status;

    if (handle == NULL) {
        return AXL_ERR;
    }

    /* Free any tracked load-options copy BEFORE UnloadImage runs the
       driver's Unload handler — once the image is unloaded the
       firmware-side LoadedImage pointer is gone, but the heap copy is
       ours regardless of UnloadImage's outcome. Releasing first means
       a UnloadImage failure still doesn't leak the copy. The synthesized
       loaded-image device path (if any) is released the same way: uninstall
       our protocol interface and free the pool while the handle is still
       valid. */
    load_options_release(handle);
    image_dp_release(handle);

    status = axl_bs()->UnloadImage((EFI_HANDLE)handle);
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
        axl_warning("driver set_load_options: HandleProtocol failed");
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

    if (EFI_ERROR(status) || img == NULL || img->FilePath == NULL) {
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

    status = axl_bs()->ConnectController(
        (EFI_HANDLE)handle, NULL, NULL, TRUE);

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

    const char *image_fs = NULL;
    if (!EFI_ERROR(li_st) && img != NULL) {
        for (size_t i = 0; i < n_vols; i++) {
            if (volumes[i].handle == img->DeviceHandle) {
                image_fs = volumes[i].name;
                break;
            }
        }
    }

    char path_buf[DRIVER_PATH_BUF];
    char sub_buf[DRIVER_SUB_BUF];

    if (image_fs != NULL) {
        /* 1: <image_dir>/<name> in the running image's own directory
           (the sibling). Tried first — a co-located driver is the most
           specific intent and should win over a stale /drivers/<arch>/
           copy from an older install. */
        AXL_AUTO_FREE char *image_path = axl_driver_get_image_path();
        if (image_path != NULL) {
            AXL_AUTO_FREE char *image_dir = axl_path_get_dirname(image_path);
            if (image_dir != NULL
                && image_dir[0] != '\0'
                && !(image_dir[0] == '.' && image_dir[1] == '\0'))
            {
                if (axl_snprintf(sub_buf, sizeof(sub_buf),
                                 "%s/%s", image_dir, driver_name) > 0
                    && axl_path_build_uefi(image_fs, sub_buf,
                                           path_buf, sizeof(path_buf)) == AXL_OK)
                {
                    driver_append_candidate(candidates, n_cand, path_buf);
                }
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

        /* 3.5: <name> at the volume root. Covers the common case where
           the user drops the app and its driver side-by-side at fs0:\.
           Candidate #1 (<image_dir>/<name>) misses this when the
           image's own directory is just "\" or "/" — the join produces
           a doubled separator that some path normalizers reject. */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/%s", driver_name) > 0
            && axl_path_build_uefi(image_fs, sub_buf,
                                   path_buf, sizeof(path_buf)) == AXL_OK)
        {
            driver_append_candidate(candidates, n_cand, path_buf);
        }
    }

    /* 4: drivers/<arch>/<name> on every other mounted volume. */
    for (size_t i = 0; i < n_vols && *n_cand < DRIVER_MAX_CANDIDATES; i++) {
        if (image_fs != NULL && axl_strcmp(volumes[i].name, image_fs) == 0) {
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
            axl_warning("driver ensure: set_load_options failed for '%s'",
                        source_label);
            axl_driver_unload(drv);
            return AXL_ERR;
        }
    }

    size_t exit_data_size = 0;
    EFI_STATUS st = axl_bs()->StartImage(
        (EFI_HANDLE)drv, &exit_data_size, NULL);

    if (EFI_ERROR(st) && st != EFI_ALREADY_STARTED) {
        axl_warning("driver ensure: StartImage failed for '%s': 0x%llx",
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
        axl_info("driver ensure: loaded '%s' handle=0x%llx "
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
    axl_warning("driver ensure: '%s' did not register protocol; unloading",
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

        AxlDriverHandle drv = NULL;
        if (axl_driver_load(candidates[i], &drv) != 0 || drv == NULL) {
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
    return driver_load_buffer_apply(buf, len, /* info */ NULL,
                                    /* default_name */ NULL, out_handle);
}

int
axl_driver_load_buffer_with_image_info(
    const unsigned char        *buf,
    size_t                      len,
    const AxlEmbeddedImageInfo *info,
    AxlDriverHandle            *out_handle
    )
{
    return driver_load_buffer_apply(buf, len, info, /* default_name */ NULL,
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
    const char *path  = axl_getenv("path");
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

int
axl_driver_load_sibling(
    const char       *file_name,
    AxlDriverHandle  *out_handle
    )
{
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

    const char *ip = axl_app_image_path();
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
    AxlDriverHandle drv = NULL;
    if (driver_load_buffer_apply(buf, len, info, default_name, &drv) != AXL_OK) {
        return AXL_ERR;
    }

    return driver_start_and_verify(drv, protocol_guid, "<embedded>",
                                   load_options, load_options_size);
}

int
_axl_driver_ensure_with_embedded_info(
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

    axl_warning("driver ensure: '%s' not found in %zu candidate path%s%s",
                search_name, n_cand, n_cand == 1 ? "" : "s",
                (override_name == NULL && embedded_buf != NULL)
                    ? " (embedded fallback also failed)" : "");
    return AXL_NOT_FOUND;
}

int
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
        protocol_guid, driver_name, embedded_buf, embedded_len,
        override_name, load_options, load_options_size,
        /* info */ NULL);
}

int
axl_driver_ensure(
    const AxlGuid *protocol_guid,
    const char    *driver_name
    )
{
    return axl_driver_ensure_with_embedded(
        protocol_guid, driver_name,
        NULL, 0,        /* no embedded blob */
        NULL,           /* no override */
        NULL, 0);       /* no LoadOptions */
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
    AxlDriverHandle drv;
    if (axl_driver_load(full_path, &drv) == 0) {
        if (axl_driver_start(drv) == 0) {
            axl_driver_connect(drv);
            c->loaded++;
            axl_info("loaded driver: %s", entry->name);
        } else {
            axl_driver_unload(drv);
            axl_warning("failed to start: %s", entry->name);
        }
    }
    return AXL_OK;
}

int
axl_driver_load_dir(
    const char *dir_path,
    const char *pattern,
    size_t     *loaded_count)
{
    if (dir_path == NULL) {
        return AXL_ERR;
    }

    DriverLoadCtx ctx = {
        .pattern = (pattern != NULL) ? pattern : "*.efi",
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

// axl_atexit hook — the app-exit safety net. Driver unload does NOT drain
// axl_atexit (only AXL_APP / CRT0 do), so a Type-B *driver* uninstalls its
// binding explicitly via axl_driver_binding_uninstall; this covers the
// app-style path where a binding outlives main.
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
    // app-exit safety-net hook. Driver unload must call
    // axl_driver_binding_uninstall — axl_atexit only drains at app exit.
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
