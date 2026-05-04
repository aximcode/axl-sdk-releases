/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-driver.c
    UEFI driver lifecycle — load, start, connect, disconnect, unload.

    Uses gBS->LoadImage/StartImage directly (not Shell "load" command)
    so load options can be set between load and start.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-driver.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-path.h>
#include <axl/axl-log.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-sys.h>

AXL_LOG_DOMAIN("driver");

// ---------------------------------------------------------------------------
// Volume-name buffer (matches DRIVER_MAX_VOLUMES so axl_volume_enumerate
// returns the same set we can address).
// ---------------------------------------------------------------------------

#define DRIVER_VOL_NAME_MAX      16

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
        if (axl_strcmp(volumes[i].name, vol_name) == 0) {
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

    extern EFI_HANDLE gImageHandle;

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

int
axl_driver_unload(
    AxlDriverHandle handle
    )
{
    EFI_STATUS status;

    if (handle == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->UnloadImage((EFI_HANDLE)handle);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
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

    /* Allow NULL data to clear load options */
    if (data == NULL || size == 0) {
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

    img->LoadOptions = copy;
    img->LoadOptionsSize = (UINT32)(size > 0xFFFFFFFF ? 0xFFFFFFFF : size);
    return AXL_OK;
}

char *
axl_driver_get_load_options(void)
{
    extern EFI_HANDLE gImageHandle;
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

char *
axl_driver_get_image_path(void)
{
    extern EFI_HANDLE gImageHandle;
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
axl_driver_set_unload(
    void *unload_fn
    )
{
    extern EFI_HANDLE gImageHandle;
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
    void *image_handle,
    void *system_table
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

    extern void axl_stream_init(void);
    axl_stream_init();
}

// ---------------------------------------------------------------------------
// axl_driver_locate / axl_driver_ensure
// ---------------------------------------------------------------------------

#define DRIVER_MAX_CANDIDATES   16
#define DRIVER_MAX_VOLUMES      16
#define DRIVER_PATH_BUF         256
#define DRIVER_SUB_BUF          192

#if defined(__x86_64__)
static const char driver_arch[] = "x64";
#elif defined(__aarch64__)
static const char driver_arch[] = "aa64";
#else
#error "axl-driver: unsupported architecture"
#endif

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
    extern EFI_HANDLE gImageHandle;

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
        /* 1: drivers/<arch>/<name> on the image's volume. */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s/%s", driver_arch, driver_name) > 0
            && axl_path_build_uefi(image_fs, sub_buf,
                                   path_buf, sizeof(path_buf)) == AXL_OK)
        {
            driver_append_candidate(candidates, n_cand, path_buf);
        }

        /* 2: <image_dir>/<name> in the running image's own directory. */
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

        /* 3: drivers/<name> at the volume root (no arch dir). */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s", driver_name) > 0
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
        AxlFileInfo info;
        if (axl_file_info(candidates[i], &info) == AXL_OK && !info.is_dir) {
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
 * Caller owns @p drv; on failure (return AXL_ERR) the driver is unloaded.
 * On success the driver stays loaded. EFI_ALREADY_STARTED is treated
 * as success — some drivers DXE-Core-dispatched re-register cleanly. */
static int
driver_start_and_verify(
    AxlDriverHandle drv,
    const AxlGuid  *protocol_guid,
    const char     *source_label  ///< for diagnostics, e.g. path or "<embedded>"
    )
{
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
        axl_info("driver ensure: loaded '%s'", source_label);
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
    size_t         n_cand
    )
{
    for (size_t i = 0; i < n_cand; i++) {
        AxlFileInfo info;
        if (axl_file_info(candidates[i], &info) != AXL_OK || info.is_dir) {
            axl_debug("  miss: %s", candidates[i]);
            continue;
        }
        axl_debug("  hit:  %s — attempting load", candidates[i]);

        AxlDriverHandle drv = NULL;
        if (axl_driver_load(candidates[i], &drv) != 0 || drv == NULL) {
            continue;
        }

        if (driver_start_and_verify(drv, protocol_guid, candidates[i]) == 0) {
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

/* LoadImage from a memory buffer (no DevicePath, no FilePath). Used
 * by the embedded-driver fallback so tools work on firmware that
 * ships neither the protocol nor a user-staged copy on disk. */
static int
driver_load_embedded(
    const AxlGuid       *protocol_guid,
    const unsigned char *buf,
    size_t               len
    )
{
    extern EFI_HANDLE gImageHandle;

    EFI_HANDLE  drv_handle = NULL;
    EFI_STATUS  st = axl_bs()->LoadImage(
        FALSE,                          /* BootPolicy */
        gImageHandle,                   /* ParentImageHandle */
        NULL,                           /* DevicePath (none — pure mem load) */
        (void *)(uintptr_t)buf,         /* SourceBuffer */
        len,                            /* SourceSize */
        &drv_handle);

    if (EFI_ERROR(st) || drv_handle == NULL) {
        axl_warning("driver ensure: LoadImage(embedded, %zu bytes) failed: 0x%llx",
                    len, (unsigned long long)st);
        return AXL_ERR;
    }

    return driver_start_and_verify((AxlDriverHandle)drv_handle,
                                   protocol_guid, "<embedded>");
}

int
axl_driver_ensure_with_embedded(
    const AxlGuid       *protocol_guid,
    const char          *driver_name,
    const unsigned char *embedded_buf,
    size_t               embedded_len,
    const char          *override_name
    )
{
    if (protocol_guid == NULL || driver_name == NULL) {
        return AXL_ERR;
    }

    /* Step 1: short-circuit if the protocol is already registered.
     * On most OEM firmware the corresponding driver is in the
     * firmware volume and dispatched at DXE init, so this is the
     * common path — no disk search, no embedded fallback. */
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

    int rc = driver_try_candidates(protocol_guid, candidates, n_cand);

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
        if (driver_load_embedded(protocol_guid, embedded_buf, embedded_len) == 0) {
            return AXL_OK;
        }
    }

    axl_warning("driver ensure: '%s' not found in %zu candidate path%s%s",
                search_name, n_cand, n_cand == 1 ? "" : "s",
                (override_name == NULL && embedded_buf != NULL)
                    ? " (embedded fallback also failed)" : "");
    return AXL_ERR;
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
        NULL);          /* no override */
}

// ---------------------------------------------------------------------------
// axl_driver_load_dir
// ---------------------------------------------------------------------------

typedef struct {
    const char *pattern;
    size_t      loaded;
} DriverLoadCtx;

static int
driver_load_cb(const char *full_path, const AxlDirEntry *entry, void *user)
{
    DriverLoadCtx *c = (DriverLoadCtx *)user;
    if (entry->is_dir || !axl_fnmatch(c->pattern, entry->name)) {
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
    (void)axl_dir_walk(dir_path, driver_load_cb, &ctx, 1);

    if (loaded_count != NULL) {
        *loaded_count = ctx.loaded;
    }
    return AXL_OK;
}
