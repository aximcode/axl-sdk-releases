/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fs.c
    Filesystem operations — path-based file ops, directory iteration
    and recursive walk, volume enumeration. The stream side
    (`axl_fopen` and the file-backed AxlStream vtable) lives in
    `src/stream/axl-stream-file.c`; this file is the path/dir/volume
    layer that calls into either streams or the AxlBackend file API
    directly.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-runtime.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-sys.h>
#include "../stream/axl-stream-internal.h"
AXL_LOG_DOMAIN("fs");


// ---------------------------------------------------------------------------
// Whole-file helpers
// ---------------------------------------------------------------------------

int
axl_file_get_contents(const char *path, void **buf, size_t *len)
{
    unsigned short *wide_path;
    AxlFileHandle handle;
    int64_t file_size_signed;
    size_t file_size;
    size_t read_size;
    void *data;
    int rc;

    if (path == NULL || buf == NULL || len == NULL) {
        return AXL_ERR;
    }

    /* Firmware Read is uninterruptible, but a caller batching many
       file reads sees a yield between each. */
    axl_yield();

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        axl_warning("utf8_to_ucs2 failed: %s", path);
        return AXL_ERR;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path, AXL_FILE_MODE_READ, 0, &handle);
    axl_free(wide_path);
    if (rc != AXL_OK) {
        axl_warning("open failed: %s", path);
        return AXL_ERR;
    }

    file_size_signed = axl_backend_file_get_size(handle);
    if (file_size_signed < 0) {
        axl_backend_file_close(&handle);
        return AXL_ERR;
    }
    file_size = (size_t)file_size_signed;

    data = axl_malloc(file_size + 1);
    if (data == NULL) {
        axl_warning("allocation failed for %zu bytes", file_size);
        axl_backend_file_close(&handle);
        return AXL_ERR;
    }

    read_size = file_size;
    rc = axl_backend_file_read(handle, &read_size, data);
    axl_backend_file_close(&handle);

    if (rc != AXL_OK) {
        axl_warning("read failed: %s", path);
        axl_free(data);
        return AXL_ERR;
    }

    /* NUL-terminate for convenience(not counted in len) */
    ((char *)data)[read_size] = '\0';

    *buf = data;
    *len = read_size;
    return AXL_OK;
}

int
axl_file_set_contents(const char *path, const void *buf, size_t len)
{
    unsigned short *wide_path;
    AxlFileHandle handle;
    size_t write_size;
    int rc;

    if (path == NULL || buf == NULL) {
        return AXL_ERR;
    }

    /* Yield at entry so a caller batching many whole-file writes
       stays Ctrl-C responsive between files. */
    axl_yield();

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        axl_warning("utf8_to_ucs2 failed: %s", path);
        return AXL_ERR;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path,
        AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE | AXL_FILE_MODE_CREATE,
        0, &handle);
    axl_free(wide_path);
    if (rc != AXL_OK) {
        axl_warning("open failed: %s", path);
        return AXL_ERR;
    }

    write_size = len;
    rc = axl_backend_file_write(handle, &write_size, buf);
    axl_backend_file_close(&handle);

    if (rc != AXL_OK) {
        axl_warning("write failed: %s", path);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_file_info(
    const char  *path,
    AxlFileInfo *info
    )
{
    unsigned short *wide_path;
    int rc;

    if (path == NULL || info == NULL) {
        return AXL_ERR;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    rc = axl_backend_file_stat(
        (const unsigned short *)wide_path,
        &info->size,
        &info->alloc_size,
        &info->is_dir,
        &info->read_only
        );
    axl_free(wide_path);
    return rc;
}

bool
axl_file_is_dir(const char *path)
{
    AxlFileInfo info;

    if (axl_file_info(path, &info) != AXL_OK) {
        return false;
    }
    return info.is_dir;
}

// ---------------------------------------------------------------------------
// File operations (path-based)
// ---------------------------------------------------------------------------

int
axl_file_delete(const char *path)
{
    unsigned short *wide_path;
    int rc;

    if (path == NULL) {
        return AXL_ERR;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    rc = axl_backend_file_delete((const unsigned short *)wide_path);
    axl_free(wide_path);
    return rc;
}

int
axl_file_rename(const char *old_path, const char *new_path)
{
    unsigned short *wide_old;
    unsigned short *wide_new;
    int rc;

    if (old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }

    wide_old = axl_utf8_to_ucs2(old_path);
    if (wide_old == NULL) {
        return AXL_ERR;
    }

    wide_new = axl_utf8_to_ucs2(new_path);
    if (wide_new == NULL) {
        axl_free(wide_old);
        return AXL_ERR;
    }

    rc = axl_backend_file_rename(
        (const unsigned short *)wide_old,
        (const unsigned short *)wide_new);
    axl_free(wide_old);
    axl_free(wide_new);
    return rc;
}

int
axl_dir_mkdir(const char *path)
{
    unsigned short *wide_path;
    int rc;

    if (path == NULL) {
        return AXL_ERR;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    rc = axl_backend_file_mkdir((const unsigned short *)wide_path);
    axl_free(wide_path);
    return rc;
}

int
axl_dir_rmdir(const char *path)
{
    unsigned short *wide_path;
    int rc;

    if (path == NULL) {
        return AXL_ERR;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    rc = axl_backend_file_rmdir((const unsigned short *)wide_path);
    axl_free(wide_path);
    return rc;
}

// ---------------------------------------------------------------------------
// Directory iteration
// ---------------------------------------------------------------------------

struct AxlDir {
    AxlFileHandle  handle;
    uint8_t        buf[1024];  /* scratch for EFI_FILE_INFO */
};

AxlDir *
axl_dir_open(const char *path)
{
    unsigned short *wide_path;
    AxlFileHandle handle;
    AxlDir *dir;
    int rc;

    if (path == NULL) {
        return NULL;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return NULL;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path,
        AXL_FILE_MODE_READ, 0, &handle);
    axl_free(wide_path);

    if (rc != AXL_OK) {
        return NULL;
    }

    dir = axl_calloc(1, sizeof(AxlDir));
    if (dir == NULL) {
        axl_backend_file_close(&handle);
        return NULL;
    }

    dir->handle = handle;
    return dir;
}

bool
axl_dir_read(AxlDir *dir, AxlDirEntry *entry)
{
    size_t buf_size;
    int rc;
    size_t i;

    if (dir == NULL || entry == NULL) {
        return false;
    }

    /* Read one directory entry (EFI_FILE_INFO) */
    buf_size = sizeof(dir->buf);
    rc = axl_backend_file_read(dir->handle, &buf_size, dir->buf);
    if (rc != AXL_OK || buf_size == 0) {
        return false;
    }

    /* Parse the EFI_FILE_INFO from the buffer.
       Layout: UINT64 Size, UINT64 FileSize, UINT64 PhysicalSize,
               EFI_TIME Create, EFI_TIME LastAccess, EFI_TIME Mod,
               UINT64 Attribute, unsigned short FileName[] */
    uint64_t file_size;
    uint64_t attribute;
    unsigned short *filename;

    /* FileSize is at offset 8 */
    axl_memcpy(&file_size, dir->buf + 8, sizeof(uint64_t));
    /* Attribute is at offset 56 (8+8+8+16+16+16 = 72... actually let me
       compute: Size(8) + FileSize(8) + PhysicalSize(8) + CreateTime(16)
       + LastAccessTime(16) + ModificationTime(16) + Attribute(8) = 80,
       so Attribute is at offset 72, FileName at offset 80) */
    axl_memcpy(&attribute, dir->buf + 72, sizeof(uint64_t));
    filename = (unsigned short *)(dir->buf + 80);

    entry->size = file_size;
    entry->is_dir = (attribute & 0x10) != 0;  /* EFI_FILE_DIRECTORY */

    /* Convert UCS-2 filename to UTF-8 */
    char *utf8 = axl_ucs2_to_utf8(filename);
    if (utf8 != NULL) {
        size_t len = axl_strlen(utf8);
        if (len >= sizeof(entry->name)) {
            len = sizeof(entry->name) - 1;
        }
        for (i = 0; i < len; i++) {
            entry->name[i] = utf8[i];
        }
        entry->name[len] = '\0';
        axl_free(utf8);
    } else {
        entry->name[0] = '\0';
    }

    return true;
}

void
axl_dir_close(AxlDir *dir)
{
    if (dir == NULL) {
        return;
    }

    axl_backend_file_close(&dir->handle);
    axl_free(dir);
}

// ---------------------------------------------------------------------------
// Recursive directory walk
// ---------------------------------------------------------------------------

#define AXL_DIR_WALK_PATH_MAX  512u

/* Recursive worker. Consolidates the open-dir / skip-./.. /
   build-full-path / recurse pattern that find, grep, driver, and
   the io-demo all reinvented. Path separator dedup matches what
   those callers used.

   levels_remaining = "callback levels still allowed at or below
   here". Starts at max_depth (≥ 1). Listing root's immediate
   children consumes one level; further descent consumes more. */
static int
dir_walk_recursive(
    const char    *root,
    AxlDirWalkFn   fn,
    void          *user,
    int            levels_remaining
    )
{
    if (levels_remaining < 1) {
        return 0;  /* exhausted — no callback runs at this level */
    }

    AxlDir *dir = axl_dir_open(root);
    if (dir == NULL) {
        return -1;
    }

    size_t root_len = axl_strlen(root);
    bool   has_sep  = (root_len > 0
                      && (root[root_len - 1] == '/'
                          || root[root_len - 1] == '\\'));

    AxlDirEntry entry;
    int         rc = 0;
    while (rc == 0 && axl_dir_read(dir, &entry)) {
        if (axl_strcmp(entry.name, ".") == 0
            || axl_strcmp(entry.name, "..") == 0) {
            continue;
        }

        char full_path[AXL_DIR_WALK_PATH_MAX];
        axl_snprintf(full_path, sizeof(full_path), "%s%s%s",
                     root,
                     has_sep ? "" : "/",
                     entry.name);

        rc = fn(full_path, &entry, user);
        if (rc != 0) break;

        if (entry.is_dir && levels_remaining > 1) {
            rc = dir_walk_recursive(full_path, fn, user, levels_remaining - 1);
            /* Treat "couldn't open subdir" as a soft skip — the
               caller asked for a walk, not a strict tree-must-be-
               readable check. Hard errors from the callback still
               propagate via the rc check above. */
            if (rc == -1) rc = 0;
        }
    }

    axl_dir_close(dir);
    return rc;
}

int
axl_dir_walk(
    const char    *root,
    AxlDirWalkFn   fn,
    void          *user,
    int            max_depth
    )
{
    if (root == NULL || fn == NULL || max_depth < 1) {
        return -1;
    }
    return dir_walk_recursive(root, fn, user, max_depth);
}

// ---------------------------------------------------------------------------
// Volume operations
// ---------------------------------------------------------------------------

char *
axl_volume_get_label(
    const char *path
    )
{
    EFI_STATUS  status;
    char       *label = NULL;

    if (path == NULL) {
        return NULL;
    }

    /* Convert path to UCS-2 for comparison */
    unsigned short wpath[256];
    size_t i;
    for (i = 0; i < 255 && path[i] != '\0'; i++) {
        wpath[i] = (unsigned short)(unsigned char)path[i];
    }
    wpath[i] = 0;

    /* Open the file path — this uses the Shell protocol internally
       to resolve "fs0:" to a filesystem handle and open the root */
    AxlFileHandle fh = NULL;
    if (axl_backend_file_open(wpath,
                              AXL_FILE_MODE_READ, 0, &fh) != AXL_OK) {
        return NULL;
    }

    /* The file handle is an EFI_FILE_PROTOCOL* internally.
       Query volume label via GetInfo with FILE_SYSTEM_VOLUME_LABEL GUID */
    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)fh;

    /* EFI_FILE_SYSTEM_VOLUME_LABEL_INFO GUID */
    EFI_GUID vol_label_guid = {
        0xDB47D7D3, 0xFE81, 0x11D3,
        { 0x9A, 0x35, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }
    };

    /* First call: size probe. The EFI_BUFFER_TOO_SMALL return is
       expected; we only care that info_size was populated. */
    size_t info_size = 0;
    (void)file->GetInfo(file, &vol_label_guid, &info_size, NULL);

    if (info_size == 0) {
        axl_backend_file_close(&fh);
        return NULL;
    }

    /* Allocate and read */
    uint8_t *info_buf = (uint8_t *)axl_malloc(info_size);
    if (info_buf == NULL) {
        axl_backend_file_close(&fh);
        return NULL;
    }

    status = file->GetInfo(file, &vol_label_guid, &info_size, info_buf);
    axl_backend_file_close(&fh);

    if (EFI_ERROR(status)) {
        axl_free(info_buf);
        return NULL;
    }

    /* The buffer is an EFI_FILE_SYSTEM_VOLUME_LABEL struct:
       just a unsigned short VolumeLabel[] at offset 0 */
    unsigned short *wlabel = (unsigned short *)info_buf;
    label = axl_ucs2_to_utf8((const unsigned short *)wlabel);
    axl_free(info_buf);

    return label;
}

char *
axl_volume_get_label_by_handle(
    void *handle
    )
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL               *root = NULL;
    EFI_STATUS                       status;
    char                            *label = NULL;

    if (handle == NULL) {
        return NULL;
    }

    /* Get SimpleFileSystem protocol from the handle */
    status = axl_bs()->HandleProtocol(
        (EFI_HANDLE)handle,
        (EFI_GUID *)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
        (void **)&fs);
    if (EFI_ERROR(status) || fs == NULL) {
        return NULL;
    }

    /* Open the root directory */
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || root == NULL) {
        return NULL;
    }

    /* Volume label GUID */
    EFI_GUID vol_label_guid = {
        0xDB47D7D3, 0xFE81, 0x11D3,
        { 0x9A, 0x35, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }
    };

    /* First call: size probe. Expected EFI_BUFFER_TOO_SMALL return;
       only info_size matters. */
    size_t info_size = 0;
    (void)root->GetInfo(root, &vol_label_guid, &info_size, NULL);

    if (info_size == 0) {
        root->Close(root);
        return NULL;
    }

    /* Allocate and read */
    uint8_t *info_buf = (uint8_t *)axl_malloc(info_size);
    if (info_buf == NULL) {
        root->Close(root);
        return NULL;
    }

    status = root->GetInfo(root, &vol_label_guid, &info_size, info_buf);
    root->Close(root);

    if (EFI_ERROR(status)) {
        axl_free(info_buf);
        return NULL;
    }

    /* The buffer is unsigned short VolumeLabel[] at offset 0 */
    unsigned short *wlabel = (unsigned short *)info_buf;
    label = axl_ucs2_to_utf8((const unsigned short *)wlabel);
    axl_free(info_buf);

    return label;
}

// ---------------------------------------------------------------------------
// axl_dir_list_json
// ---------------------------------------------------------------------------

int
axl_dir_list_json(
    const AxlDirEntry *entries,
    size_t             count,
    char              *buf,
    size_t             buf_size)
{
    size_t pos = 0;
    int    n;

    if (entries == NULL || buf == NULL || buf_size == 0) {
        return AXL_ERR;
    }

    if (buf_size < 3) {
        return AXL_ERR;  /* need room for at least "[]" + NUL */
    }

    buf[pos++] = '[';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            if (pos >= buf_size) {
                return AXL_ERR;
            }
            buf[pos++] = ',';
        }

        n = axl_snprintf(
            buf + pos, buf_size - pos,
            "{\"name\":\"%s\",\"size\":%llu,\"dir\":%s}",
            entries[i].name,
            (unsigned long long)entries[i].size,
            entries[i].is_dir ? "true" : "false");

        if (n < 0 || (size_t)n >= buf_size - pos) {
            return AXL_ERR;
        }
        pos += (size_t)n;
    }

    if (pos + 1 >= buf_size) {
        return AXL_ERR;
    }
    buf[pos++] = ']';
    buf[pos] = '\0';

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_volume_enumerate
// ---------------------------------------------------------------------------

int
axl_volume_enumerate(AxlVolume *out, size_t max, size_t *count)
{
    void  **handles = NULL;
    size_t  num = 0;

    if (count == NULL) {
        return AXL_ERR;
    }

    if (axl_service_enumerate("simple-fs", &handles, &num) != AXL_OK) {
        *count = 0;
        return AXL_OK;
    }

    size_t filled = 0;
    for (size_t i = 0; i < num; i++) {
        if (out != NULL && filled < max) {
            out[filled].handle = handles[i];
            axl_snprintf(out[filled].name, sizeof(out[filled].name),
                         "fs%zu", i);
            /* device_path is firmware-owned — share the pointer.
               NULL on rare handles that don't publish a DP. */
            out[filled].device_path = NULL;
            (void)axl_handle_get_service(handles[i], "device-path",
                                         &out[filled].device_path);
        }
        filled++;
    }

    axl_free(handles);

    *count = (out != NULL) ? (filled < max ? filled : max) : filled;
    return AXL_OK;
}
