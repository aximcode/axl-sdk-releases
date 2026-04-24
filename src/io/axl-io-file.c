/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-io-file.c
    File stream backend using AxlBackend file API.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-runtime.h>
#include <axl/axl-str.h>
#include <axl/axl-io.h>
#include <axl/axl-sys.h>
#include "axl-io-internal.h"
AXL_LOG_DOMAIN("io");

typedef struct {
    AxlFileHandle  handle;
} FileCtx;

// ---------------------------------------------------------------------------
// File vtable
// ---------------------------------------------------------------------------

static axl_ssize_t
file_write(void *ctx, const void *data, size_t count)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc;

    rc = axl_backend_file_write(f->handle, &size, data);
    if (rc != 0) {
        axl_warning("write failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_read(void *ctx, void *data, size_t count)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t size = count;
    int rc;

    rc = axl_backend_file_read(f->handle, &size, data);
    if (rc != 0) {
        axl_warning("read failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_pread(void *ctx, void *data, size_t count, size_t offset)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t saved_pos;
    size_t size = count;
    int rc;

    /* Save current position */
    axl_backend_file_get_position(f->handle, &saved_pos);

    /* Seek to offset */
    rc = axl_backend_file_set_position(f->handle, (uint64_t)offset);
    if (rc != 0) {
        axl_warning("seek failed");
        return -1;
    }

    rc = axl_backend_file_read(f->handle, &size, data);

    /* Restore position */
    axl_backend_file_set_position(f->handle, saved_pos);

    if (rc != 0) {
        axl_warning("pread failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static axl_ssize_t
file_pwrite(void *ctx, const void *data, size_t count, size_t offset)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t saved_pos;
    size_t size = count;
    int rc;

    axl_backend_file_get_position(f->handle, &saved_pos);

    rc = axl_backend_file_set_position(f->handle, (uint64_t)offset);
    if (rc != 0) {
        axl_warning("seek failed");
        return -1;
    }

    rc = axl_backend_file_write(f->handle, &size, data);

    axl_backend_file_set_position(f->handle, saved_pos);

    if (rc != 0) {
        axl_warning("pwrite failed");
        return -1;
    }
    return (axl_ssize_t)size;
}

static int
file_seek(void *ctx, int64_t offset, int whence)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t pos;

    if (whence == AXL_SEEK_SET) {
        if (offset < 0) {
            return -1;
        }
        pos = (uint64_t)offset;
    } else if (whence == AXL_SEEK_CUR) {
        uint64_t cur;
        if (axl_backend_file_get_position(f->handle, &cur) != 0) {
            return -1;
        }
        if (offset < 0 && (uint64_t)(-offset) > cur) {
            return -1;
        }
        pos = (uint64_t)((int64_t)cur + offset);
    } else if (whence == AXL_SEEK_END) {
        int64_t file_size = axl_backend_file_get_size(f->handle);
        if (file_size < 0) {
            return -1;
        }
        if (offset < 0 && (uint64_t)(-offset) > (uint64_t)file_size) {
            return -1;
        }
        pos = (uint64_t)(file_size + offset);
    } else {
        return -1;
    }

    return axl_backend_file_set_position(f->handle, pos);
}

static int64_t
file_tell(void *ctx)
{
    FileCtx *f = (FileCtx *)ctx;
    uint64_t pos;

    if (axl_backend_file_get_position(f->handle, &pos) != 0) {
        return -1;
    }
    return (int64_t)pos;
}

static int
file_flush(void *ctx)
{
    FileCtx *f = (FileCtx *)ctx;
    size_t zero = 0;

    /* UEFI: WriteFile with size 0 flushes */
    return axl_backend_file_write(f->handle, &zero, NULL);
}

static void
file_close(void *ctx)
{
    FileCtx *f = (FileCtx *)ctx;
    axl_backend_file_close(&f->handle);
    axl_free(f);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AxlStream *
axl_fopen_internal(const char *path, const char *mode)
{
    unsigned short *wide_path;
    AxlFileHandle handle;
    uint64_t open_mode;
    int rc;
    FileCtx *f;
    AxlStream *s;

    if (path == NULL || mode == NULL) {
        return NULL;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        axl_warning("utf8_to_ucs2 failed: %s", path);
        return NULL;
    }

    if (mode[0] == 'r') {
        open_mode = AXL_FILE_MODE_READ;
    } else if (mode[0] == 'w' || mode[0] == 'a') {
        /* 'a' currently behaves the same as 'w' — no true append
           mode yet. Caller-visible semantics remain consistent. */
        open_mode = AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE | AXL_FILE_MODE_CREATE;
    } else {
        axl_free(wide_path);
        return NULL;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path, open_mode, 0, &handle);
    axl_free(wide_path);

    if (rc != 0) {
        axl_warning("open failed: %s", path);
        return NULL;
    }

    f = axl_new(FileCtx);
    if (f == NULL) {
        axl_warning("allocation failed");
        axl_backend_file_close(&handle);
        return NULL;
    }
    f->handle = handle;

    /* For append mode, seek to end */
    if (mode[0] == 'a') {
        axl_backend_file_set_position(handle, 0xFFFFFFFFFFFFFFFFULL);
    }

    s = axl_stream_new();
    if (s == NULL) {
        axl_warning("allocation failed");
        axl_backend_file_close(&handle);
        axl_free(f);
        return NULL;
    }

    s->ctx    = f;
    s->read   = file_read;
    s->write  = file_write;
    s->pread  = file_pread;
    s->pwrite = file_pwrite;
    s->seek   = file_seek;
    s->tell   = file_tell;
    s->flush  = file_flush;
    s->close  = file_close;

    return s;
}

// ---------------------------------------------------------------------------
// Whole-file helpers
// ---------------------------------------------------------------------------

int
axl_file_get_contents_internal(const char *path, void **buf, size_t *len)
{
    unsigned short *wide_path;
    AxlFileHandle handle;
    int64_t file_size_signed;
    size_t file_size;
    size_t read_size;
    void *data;
    int rc;

    if (path == NULL || buf == NULL || len == NULL) {
        return -1;
    }

    /* Firmware Read is uninterruptible, but a caller batching many
       file reads sees a yield between each. */
    axl_yield();

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        axl_warning("utf8_to_ucs2 failed: %s", path);
        return -1;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path, AXL_FILE_MODE_READ, 0, &handle);
    axl_free(wide_path);
    if (rc != 0) {
        axl_warning("open failed: %s", path);
        return -1;
    }

    file_size_signed = axl_backend_file_get_size(handle);
    if (file_size_signed < 0) {
        axl_backend_file_close(&handle);
        return -1;
    }
    file_size = (size_t)file_size_signed;

    data = axl_malloc(file_size + 1);
    if (data == NULL) {
        axl_warning("allocation failed for %zu bytes", file_size);
        axl_backend_file_close(&handle);
        return -1;
    }

    read_size = file_size;
    rc = axl_backend_file_read(handle, &read_size, data);
    axl_backend_file_close(&handle);

    if (rc != 0) {
        axl_warning("read failed: %s", path);
        axl_free(data);
        return -1;
    }

    /* NUL-terminate for convenience(not counted in len) */
    ((char *)data)[read_size] = '\0';

    *buf = data;
    *len = read_size;
    return 0;
}

int
axl_file_set_contents_internal(const char *path, const void *buf, size_t len)
{
    unsigned short *wide_path;
    AxlFileHandle handle;
    size_t write_size;
    int rc;

    if (path == NULL || buf == NULL) {
        return -1;
    }

    /* Yield at entry so a caller batching many whole-file writes
       stays Ctrl-C responsive between files. */
    axl_yield();

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        axl_warning("utf8_to_ucs2 failed: %s", path);
        return -1;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path,
        AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE | AXL_FILE_MODE_CREATE,
        0, &handle);
    axl_free(wide_path);
    if (rc != 0) {
        axl_warning("open failed: %s", path);
        return -1;
    }

    write_size = len;
    rc = axl_backend_file_write(handle, &write_size, buf);
    axl_backend_file_close(&handle);

    if (rc != 0) {
        axl_warning("write failed: %s", path);
        return -1;
    }
    return 0;
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
        return -1;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return -1;
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

    if (axl_file_info(path, &info) != 0) {
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
        return -1;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return -1;
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
        return -1;
    }

    wide_old = axl_utf8_to_ucs2(old_path);
    if (wide_old == NULL) {
        return -1;
    }

    wide_new = axl_utf8_to_ucs2(new_path);
    if (wide_new == NULL) {
        axl_free(wide_old);
        return -1;
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
        return -1;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return -1;
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
        return -1;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return -1;
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

    if (rc != 0) {
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
    if (rc != 0 || buf_size == 0) {
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
                              AXL_FILE_MODE_READ, 0, &fh) != 0) {
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
        return -1;
    }

    if (buf_size < 3) {
        return -1;  /* need room for at least "[]" + NUL */
    }

    buf[pos++] = '[';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            if (pos >= buf_size) {
                return -1;
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
            return -1;
        }
        pos += (size_t)n;
    }

    if (pos + 1 >= buf_size) {
        return -1;
    }
    buf[pos++] = ']';
    buf[pos] = '\0';

    return 0;
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
        return -1;
    }

    if (axl_service_enumerate("simple-fs", &handles, &num) != 0) {
        *count = 0;
        return 0;
    }

    size_t filled = 0;
    for (size_t i = 0; i < num; i++) {
        if (out != NULL && filled < max) {
            out[filled].handle = handles[i];
            axl_snprintf(out[filled].name, sizeof(out[filled].name),
                         "fs%zu", i);
        }
        filled++;
    }

    axl_free(handles);

    *count = (out != NULL) ? (filled < max ? filled : max) : filled;
    return 0;
}
