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
#include <axl/axl-io.h>
#include <axl/axl-sys.h>

AXL_LOG_DOMAIN("driver");

// ---------------------------------------------------------------------------
// Device path safety limits
// ---------------------------------------------------------------------------

#define MAX_DP_WALK  256   /* max nodes to walk before giving up */

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_driver_load(
    const char       *path,
    AxlDriverHandle  *handle
    )
{
    EFI_STATUS status;
    EFI_HANDLE image = NULL;
    void *buf = NULL;
    size_t buf_size = 0;

    if (path == NULL || handle == NULL) {
        return -1;
    }

    *handle = NULL;

    /* Read the .efi file into memory */
    if (axl_file_get_contents(path, &buf, &buf_size) != 0 || buf == NULL) {
        axl_warning("driver load: cannot read '%s'", path);
        return -1;
    }

    /* Load the image from the memory buffer */
    extern EFI_HANDLE gImageHandle;
    status = axl_bs()->LoadImage(
        FALSE,           /* BootPolicy */
        gImageHandle,    /* ParentImageHandle */
        NULL,            /* DevicePath — NULL for memory source */
        buf,             /* SourceBuffer */
        (size_t)buf_size, /* SourceSize */
        &image);

    axl_free(buf);

    if (EFI_ERROR(status)) {
        axl_warning("driver load: LoadImage failed for '%s': 0x%llx",
                   path, (unsigned long long)status);
        return -1;
    }

    *handle = (AxlDriverHandle)image;
    return 0;
}

int
axl_driver_start(
    AxlDriverHandle handle
    )
{
    EFI_STATUS status;
    size_t exit_data_size = 0;

    if (handle == NULL) {
        return -1;
    }

    status = axl_bs()->StartImage(
        (EFI_HANDLE)handle,
        &exit_data_size,
        NULL);

    if (EFI_ERROR(status)) {
        axl_warning("driver start failed: 0x%llx",
                   (unsigned long long)status);
        return -1;
    }

    return 0;
}

int
axl_driver_connect(
    AxlDriverHandle handle
    )
{
    /* If a real handle is given, connect just that handle.
       Otherwise, reconnect all controllers globally. */
    if (handle != NULL) {
        return axl_driver_connect_handle(handle);
    }

    axl_bs()->ConnectController(NULL, NULL, NULL, TRUE);
    return 0;
}

int
axl_driver_disconnect(
    AxlDriverHandle handle
    )
{
    if (handle != NULL) {
        axl_bs()->DisconnectController((EFI_HANDLE)handle, NULL, NULL);
        return 0;
    }

    /* NULL handle — disconnect all */
    axl_bs()->DisconnectController(NULL, NULL, NULL);
    return 0;
}

int
axl_driver_unload(
    AxlDriverHandle handle
    )
{
    EFI_STATUS status;

    if (handle == NULL) {
        return -1;
    }

    status = axl_bs()->UnloadImage((EFI_HANDLE)handle);
    return EFI_ERROR(status) ? -1 : 0;
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
        return -1;
    }

    status = axl_bs()->HandleProtocol(
        (EFI_HANDLE)handle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    if (EFI_ERROR(status) || img == NULL) {
        axl_warning("driver set_load_options: HandleProtocol failed");
        return -1;
    }

    /* Allow NULL data to clear load options */
    if (data == NULL || size == 0) {
        img->LoadOptions = NULL;
        img->LoadOptionsSize = 0;
        return 0;
    }

    /* Copy the data — caller's buffer may be on the stack */
    copy = axl_malloc(size);
    if (copy == NULL) {
        return -1;
    }
    axl_memcpy(copy, data, size);

    img->LoadOptions = copy;
    img->LoadOptionsSize = (UINT32)(size > 0xFFFFFFFF ? 0xFFFFFFFF : size);
    return 0;
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

    /* Walk the device path to find the file path node.
       File path nodes have Type=4 (MEDIA_DEVICE_PATH), SubType=4 (MEDIA_FILEPATH_DP).
       The node data after the 4-byte header is a UCS-2 path string.
       Limit walk to MAX_DP_WALK nodes to prevent infinite loops on
       malformed device paths. */
    EFI_DEVICE_PATH_PROTOCOL *dp = img->FilePath;
    int steps = 0;
    while (dp != NULL && !EFI_DP_IS_END(dp) && steps < MAX_DP_WALK) {
        uint16_t node_len = (uint16_t)EFI_DP_LENGTH(dp);
        if (node_len < 4) {
            break;  /* malformed node */
        }
        if (EFI_DP_TYPE(dp) == 0x04 && EFI_DP_SUBTYPE(dp) == 0x04 &&
            node_len > 4)
        {
            unsigned short *wpath = (unsigned short *)((uint8_t *)dp + 4);
            return axl_ucs2_to_utf8(wpath);
        }
        dp = EFI_DP_NEXT(dp);
        steps++;
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
        return -1;
    }

    status = axl_bs()->ConnectController(
        (EFI_HANDLE)handle, NULL, NULL, TRUE);

    /* NOT_FOUND is OK — means no new bindings, not an error */
    if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
        return -1;
    }

    return 0;
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
        return -1;
    }

    status = axl_bs()->HandleProtocol(
        gImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    if (EFI_ERROR(status) || img == NULL) {
        return -1;
    }

    img->Unload = (EFI_IMAGE_UNLOAD)unload_fn;
    return 0;
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

    extern void axl_io_init(void);
    axl_io_init();
}

// ---------------------------------------------------------------------------
// axl_driver_ensure
// ---------------------------------------------------------------------------

#define ENSURE_MAX_CANDIDATES   16
#define ENSURE_MAX_VOLUMES      16
#define ENSURE_PATH_BUF         256
#define ENSURE_SUB_BUF          192

#if defined(__x86_64__)
static const char ensure_arch[] = "x64";
#elif defined(__aarch64__)
static const char ensure_arch[] = "aa64";
#else
#error "axl_driver_ensure: unsupported architecture"
#endif

static int
ensure_protocol_registered(
    const AxlGuid *guid
    )
{
    void *iface = NULL;
    EFI_STATUS st = axl_bs()->LocateProtocol(
        (EFI_GUID *)guid, NULL, &iface);
    return EFI_ERROR(st) ? -1 : 0;
}

static int
ensure_append_candidate(
    char  **candidates,
    size_t *n_cand,
    const char *path
    )
{
    if (*n_cand >= ENSURE_MAX_CANDIDATES || path == NULL) {
        return -1;
    }
    char *copy = axl_strdup(path);
    if (copy == NULL) {
        return -1;
    }
    candidates[(*n_cand)++] = copy;
    return 0;
}

int
axl_driver_ensure(
    const AxlGuid *protocol_guid,
    const char    *driver_name
    )
{
    extern EFI_HANDLE gImageHandle;

    if (protocol_guid == NULL || driver_name == NULL) {
        return -1;
    }

    /* Step 1: short-circuit if the protocol is already registered. */
    if (ensure_protocol_registered(protocol_guid) == 0) {
        return 0;
    }

    /* Locate the volume the running image was loaded from. The handle
     * stored in EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle is the same
     * handle axl_volume_enumerate exposes for that filesystem. */
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS li_st = axl_bs()->HandleProtocol(
        gImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&img);

    AxlVolume volumes[ENSURE_MAX_VOLUMES];
    size_t    n_vols = 0;
    axl_volume_enumerate(volumes, ENSURE_MAX_VOLUMES, &n_vols);

    const char *image_fs = NULL;
    if (!EFI_ERROR(li_st) && img != NULL) {
        for (size_t i = 0; i < n_vols; i++) {
            if (volumes[i].handle == img->DeviceHandle) {
                image_fs = volumes[i].name;
                break;
            }
        }
    }

    /* Build the candidate list in priority order. */
    char  *candidates[ENSURE_MAX_CANDIDATES];
    size_t n_cand = 0;
    char   path_buf[ENSURE_PATH_BUF];
    char   sub_buf[ENSURE_SUB_BUF];

    if (image_fs != NULL) {
        /* 1: drivers/<arch>/<name> on the image's volume. */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s/%s", ensure_arch, driver_name) > 0
            && axl_path_build_uefi(image_fs, sub_buf,
                                   path_buf, sizeof(path_buf)) == 0)
        {
            ensure_append_candidate(candidates, &n_cand, path_buf);
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
                                           path_buf, sizeof(path_buf)) == 0)
                {
                    ensure_append_candidate(candidates, &n_cand, path_buf);
                }
            }
        }

        /* 3: drivers/<name> at the volume root (no arch dir). */
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s", driver_name) > 0
            && axl_path_build_uefi(image_fs, sub_buf,
                                   path_buf, sizeof(path_buf)) == 0)
        {
            ensure_append_candidate(candidates, &n_cand, path_buf);
        }
    }

    /* 4: drivers/<arch>/<name> on every other mounted volume. */
    for (size_t i = 0; i < n_vols && n_cand < ENSURE_MAX_CANDIDATES; i++) {
        if (image_fs != NULL && axl_strcmp(volumes[i].name, image_fs) == 0) {
            continue;
        }
        if (axl_snprintf(sub_buf, sizeof(sub_buf),
                         "/drivers/%s/%s", ensure_arch, driver_name) > 0
            && axl_path_build_uefi(volumes[i].name, sub_buf,
                                   path_buf, sizeof(path_buf)) == 0)
        {
            ensure_append_candidate(candidates, &n_cand, path_buf);
        }
    }

    /* Try each candidate in priority order. */
    int rc = -1;
    for (size_t i = 0; i < n_cand; i++) {
        AxlFileInfo info;
        if (axl_file_info(candidates[i], &info) != 0 || info.is_dir) {
            continue;
        }

        AxlDriverHandle drv = NULL;
        if (axl_driver_load(candidates[i], &drv) != 0 || drv == NULL) {
            continue;
        }

        /* Inline StartImage so we can recognize EFI_ALREADY_STARTED.
         * axl_driver_start collapses every error to -1. */
        size_t exit_data_size = 0;
        EFI_STATUS st = axl_bs()->StartImage(
            (EFI_HANDLE)drv, &exit_data_size, NULL);

        if (EFI_ERROR(st) && st != EFI_ALREADY_STARTED) {
            axl_warning("driver ensure: StartImage failed for '%s': 0x%llx",
                        candidates[i], (unsigned long long)st);
            axl_driver_unload(drv);
            continue;
        }

        if (ensure_protocol_registered(protocol_guid) == 0) {
            axl_info("driver ensure: loaded '%s'", candidates[i]);
            rc = 0;
            break;
        }

        /* Driver started but didn't register the expected protocol.
         * Unload to keep system state clean and try the next path. */
        axl_warning("driver ensure: '%s' did not register protocol; unloading",
                    candidates[i]);
        axl_driver_unload(drv);
    }

    if (rc != 0) {
        axl_warning("driver ensure: '%s' not found on %zu volume%s",
                    driver_name, n_vols, n_vols == 1 ? "" : "s");
    }

    for (size_t i = 0; i < n_cand; i++) {
        axl_free(candidates[i]);
    }

    return rc;
}

// ---------------------------------------------------------------------------
// axl_driver_load_dir
// ---------------------------------------------------------------------------

int
axl_driver_load_dir(
    const char *dir_path,
    const char *pattern,
    size_t     *loaded_count)
{
    AxlDir      *dir;
    AxlDirEntry  entry;
    size_t       loaded = 0;
    const char  *pat;

    if (dir_path == NULL) {
        return -1;
    }

    pat = (pattern != NULL) ? pattern : "*.efi";

    dir = axl_dir_open(dir_path);
    if (dir == NULL) {
        /* Directory doesn't exist — not an error, just 0 loaded */
        if (loaded_count != NULL) {
            *loaded_count = 0;
        }
        return 0;
    }

    while (axl_dir_read(dir, &entry)) {
        if (entry.is_dir) {
            continue;
        }

        if (!axl_fnmatch(pat, entry.name)) {
            continue;
        }

        /* Build full path: dir_path + "/" + entry.name */
        char *full_path = axl_path_join(dir_path, entry.name);
        if (full_path == NULL) {
            continue;
        }

        AxlDriverHandle drv;
        if (axl_driver_load(full_path, &drv) == 0) {
            if (axl_driver_start(drv) == 0) {
                axl_driver_connect(drv);
                loaded++;
                axl_info("loaded driver: %s", entry.name);
            } else {
                axl_driver_unload(drv);
                axl_warning("failed to start: %s", entry.name);
            }
        }

        axl_free(full_path);
    }

    axl_dir_close(dir);

    if (loaded_count != NULL) {
        *loaded_count = loaded;
    }
    return 0;
}
