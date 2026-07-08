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
#include "../runtime/axl-signal-internal.h"
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-sys.h>
#include "../stream/axl-stream-internal.h"
AXL_LOG_DOMAIN("fs");

// ---------------------------------------------------------------------------
// Internal types and macros
// ---------------------------------------------------------------------------

struct AxlDir {
    AxlFileHandle  handle;
    uint8_t        buf[1024];  /* scratch for EFI_FILE_INFO */
};

#define AXL_DIR_WALK_PATH_MAX  512u

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
    _axl_poll_break();

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

AxlBytes *
axl_file_get_bytes(const char *path)
{
    void   *buf = NULL;
    size_t  len = 0;

    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        return NULL;
    }
    // Hand the read buffer straight to AxlBytes — no second copy.
    AxlBytes *b = axl_bytes_new_take(buf, len);
    if (b == NULL) {
        axl_free(buf);  // new_take failed (OOM) — don't leak the buffer
    }
    return b;
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
    _axl_poll_break();

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
axl_file_write_atomic(const char *path, const void *buf, size_t len)
{
    if (path == NULL || (buf == NULL && len > 0)) {
        return AXL_ERR;
    }

    /* Build the temp sibling "<path>.tmp" — same directory, so the
       replace below is a same-directory rename (the FAT atomic case). */
    size_t plen = axl_strlen(path);
    char  *temp = axl_malloc(plen + 5);   /* ".tmp" + NUL */
    if (temp == NULL) {
        return AXL_ERR;
    }
    axl_memcpy(temp, path, plen);
    axl_memcpy(temp + plen, ".tmp", 5);   /* copies the NUL too */

    /* Write the full contents to the temp file first; closing it flushes
       (UEFI FAT flushes on close). The target is untouched until now. */
    if (axl_file_set_contents(temp, (buf != NULL) ? buf : "", len) != AXL_OK) {
        axl_file_delete(temp);            /* best-effort cleanup */
        axl_free(temp);
        return AXL_ERR;
    }

    /* Replace the target. rename-over-existing isn't atomic on FAT, so
       try a plain rename first (atomic when the target is absent) and
       fall back to delete-then-rename. On the fallback a crash leaves
       only the complete temp file — never a half-written target. */
    int rc = axl_file_rename(temp, path);
    if (rc != AXL_OK) {
        /* Target exists: FAT can't rename-over, so remove then rename.
           If the delete succeeds but this rename then fails (effectively
           impossible on a healthy same-directory FAT volume), both files
           are gone and we report ERR below — the data is in neither. */
        axl_file_delete(path);            /* best-effort; ignore result */
        rc = axl_file_rename(temp, path);
    }
    if (rc != AXL_OK) {
        axl_file_delete(temp);            /* leave the target as it was */
        axl_free(temp);
        return AXL_ERR;
    }

    axl_free(temp);
    return AXL_OK;
}

int
axl_file_info(
    const char *path,
    AxlFsEntry *entry
    )
{
    unsigned short *wide_path;
    int rc;
    uint64_t size = 0, alloc_size = 0, mtime_unix = 0;
    bool is_dir = false, read_only = false;

    if (path == NULL || entry == NULL) {
        return AXL_ERR;
    }

    /* Zero the whole struct up-front so even a careless caller that
       skips the rc check sees a defined (empty) entry, not partial
       stack from the backend. */
    axl_memset(entry, 0, sizeof(*entry));

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    /* Backend's stat predates AxlFsEntry; bridges via temp scalars
       and merges into the bitmask below. */
    rc = axl_backend_file_stat(
        (const unsigned short *)wide_path,
        &size, &alloc_size, &mtime_unix,
        &is_dir, &read_only);
    axl_free(wide_path);
    if (rc != AXL_OK) return rc;

    entry->struct_size = sizeof(*entry);
    entry->version     = AXL_FS_ENTRY_VERSION;
    /* Path-stat doesn't populate name (caller has the path). */
    entry->size        = size;
    entry->alloc_size  = alloc_size;
    entry->mtime_unix  = mtime_unix;
    entry->attributes  = (is_dir    ? AXL_FS_ATTR_DIRECTORY : 0u)
                       | (read_only ? AXL_FS_ATTR_READ_ONLY : 0u);
    return AXL_OK;
}

bool
axl_file_is_dir(const char *path)
{
    AxlFsEntry entry;

    if (axl_file_info(path, &entry) != AXL_OK) {
        return false;
    }
    return axl_fs_entry_is_dir(&entry);
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

/* Return pointer to the basename of @p path — the substring after
   the last '/' or '\' separator. Accepts mixed-separator paths
   ("fs0:\\dir/foo.txt" → "foo.txt"). For separator-free input,
   returns @p path unchanged. */
static const char *
path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
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

    /* SetFileInfo on UEFI FAT drivers expects a basename, not a
       full path, in EFI_FILE_INFO.FileName — and can't move across
       directories. Extract the basename of @p new_path; if @p
       new_path also carries a directory prefix, refuse the rename
       unless that prefix matches @p old_path's prefix (so a
       cross-directory move is rejected with AXL_ERR rather than
       silently dropping the path component).

       Caller wants cross-directory move? Use axl_file_move(), which
       falls back to copy + delete for the cross-directory case. */
    const char *new_base    = path_basename(new_path);
    if (*new_base == '\0') {
        /* "fs0:\\" — trailing separator with no name part. The
           backend would reject this anyway; catch it here so a
           well-formed AXL_ERR surfaces. */
        return AXL_ERR;
    }
    size_t      new_dir_len = (size_t)(new_base - new_path);
    if (new_dir_len > 0) {
        const char *old_base    = path_basename(old_path);
        size_t      old_dir_len = (size_t)(old_base - old_path);
        if (new_dir_len != old_dir_len ||
            axl_strncmp(old_path, new_path, old_dir_len) != 0)
        {
            return AXL_ERR;
        }
    }

    wide_old = axl_utf8_to_ucs2(old_path);
    if (wide_old == NULL) {
        return AXL_ERR;
    }

    wide_new = axl_utf8_to_ucs2(new_base);
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
axl_file_move(const char *old_path, const char *new_path)
{
    if (old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }

    /* Pre-flight: ensure the destination doesn't exist. UEFI FAT
       SetFileInfo rejects a same-directory rename if the target
       name is taken, and the copy fallback's write-open does not
       truncate past the new content's length — both paths would
       otherwise leave behind stale or hybrid content. Eagerly
       removing @p new_path gives clean POSIX-rename-style overwrite
       semantics regardless of which path runs. NOT atomic: if the
       move then fails, the destination is gone — caller can
       inspect the source and retry. Ignore the delete return
       (file may legitimately not exist). */
    axl_file_delete(new_path);

    /* Same-directory case — try the atomic rename path first. */
    if (axl_file_rename(old_path, new_path) == AXL_OK) {
        return AXL_OK;
    }

    /* Cross-directory (or rename otherwise refused). Fall back to
       chunked stream copy + source delete. NOT atomic — partial
       failures leave observable state (see header docstring). */
    AxlStream *src = axl_fopen(old_path, "r");
    if (src == NULL) {
        return AXL_ERR;
    }
    AxlStream *dst = axl_fopen(new_path, "w");
    if (dst == NULL) {
        axl_fclose(src);
        return AXL_ERR;
    }

    char    buf[4096];
    bool    ok = true;
    for (;;) {
        _axl_poll_break();  /* keep Ctrl-C responsive on large copies */
        axl_ssize_t n = axl_read(src, buf, sizeof(buf));
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            break;  /* EOF */
        }
        if (axl_write(dst, buf, (size_t)n) != n) {
            ok = false;
            break;
        }
    }

    axl_fclose(src);
    axl_fclose(dst);

    if (!ok) {
        /* Partial dest file may exist. Leave it for caller to
           inspect / retry — silently deleting it would hide
           diagnostic info and isn't always desirable. */
        return AXL_ERR;
    }
    return axl_file_delete(old_path);
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
axl_dir_read(AxlDir *dir, AxlFsEntry *entry)
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
       Layout (UEFI 2.10 §13.5): UINT64 Size, UINT64 FileSize,
       UINT64 PhysicalSize, EFI_TIME CreateTime (offset 24),
       EFI_TIME LastAccessTime (offset 40), EFI_TIME ModificationTime
       (offset 56), UINT64 Attribute (offset 72), CHAR16 FileName[]
       (offset 80). */
    uint64_t file_size;
    uint64_t attribute;
    unsigned short *filename;

    axl_memcpy(&file_size, dir->buf + 8,  sizeof(uint64_t));
    axl_memcpy(&attribute, dir->buf + 72, sizeof(uint64_t));
    filename = (unsigned short *)(dir->buf + 80);

    entry->struct_size = sizeof(*entry);
    entry->version     = AXL_FS_ENTRY_VERSION;
    entry->size        = file_size;
    entry->alloc_size  = 0;
    entry->mtime_unix  = axl_backend_efi_time_to_unix(dir->buf + 56);
    /* EFI attribute → AXL attribute bits. EFI_FILE_DIRECTORY is
       0x10, READ_ONLY 0x01, etc. — AXL constants chosen identical
       so this is a literal mask copy. */
    entry->attributes  = (uint32_t)(attribute & 0x37u);

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

    /* Join children with the separator the root already uses, so the
       composed path reopens on strict volumes. This historically
       hardcoded '/', which yields a mixed-separator path (e.g.
       "FS1:\sub/b.txt") on a backslash-rooted UEFI volume — VirtioFsDxe
       and other strict providers then fail to open it. Pick the root's
       last separator; with none present, infer '\' from a "VOL:" prefix
       (UEFI volume root), else default to '/'. */
    char sep   = '/';
    bool found = false;
    for (size_t i = root_len; i > 0 && !found; i--) {
        char c = root[i - 1];
        if (c == '/' || c == '\\') { sep = c; found = true; }
    }
    if (!found) {
        for (size_t i = 0; i < root_len; i++) {
            if (root[i] == ':') { sep = '\\'; break; }
        }
    }

    AxlFsEntry  entry;
    int         rc = 0;
    while (rc == 0 && axl_dir_read(dir, &entry)) {
        if (axl_strcmp(entry.name, ".") == 0
            || axl_strcmp(entry.name, "..") == 0) {
            continue;
        }

        char full_path[AXL_DIR_WALK_PATH_MAX];
        if (has_sep) {
            axl_snprintf(full_path, sizeof(full_path), "%s%s",
                         root, entry.name);
        } else {
            axl_snprintf(full_path, sizeof(full_path), "%s%c%s",
                         root, sep, entry.name);
        }

        rc = fn(full_path, &entry, user);
        if (rc != 0) break;

        if (axl_fs_entry_is_dir(&entry) && levels_remaining > 1) {
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
    file->GetInfo(file, &vol_label_guid, &info_size, NULL);

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
    root->GetInfo(root, &vol_label_guid, &info_size, NULL);

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
    const AxlFsEntry *entries,
    size_t            count,
    char             *buf,
    size_t            buf_size)
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
            axl_fs_entry_is_dir(&entries[i]) ? "true" : "false");

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

    if (axl_protocol_enumerate("simple-fs", &handles, &num) != AXL_OK) {
        *count = 0;
        return AXL_OK;
    }

    size_t filled = 0;
    for (size_t i = 0; i < num; i++) {
        if (out != NULL && filled < max) {
            out[filled].handle = handles[i];
            /* device_path is firmware-owned — share the pointer.
               NULL on rare handles that don't publish a DP. */
            out[filled].device_path = NULL;
            axl_handle_get_protocol(handles[i], "device-path",
                                         &out[filled].device_path);
            /* Name from the UEFI Shell's fsN map, NOT the LocateHandle
               index — the two orders diverge (a remap / mkrd / hot-plug
               inserts a handle at a different position than the shell
               assigns its alias), which previously bound the wrong
               handle/device-path to a name. Fall back to the positional
               name when there's no shell mapping (e.g. a just-created
               ramdisk the shell hasn't remapped yet, or a non-shell
               backend). The fallback shares the fsN namespace, so a
               still-unmapped volume can transiently collide with a mapped
               volume's name until the shell remaps — strictly better than
               the old always-positional naming, and self-healing. */
            if (out[filled].device_path == NULL
                || axl_backend_shell_map_name(out[filled].device_path,
                                              out[filled].name,
                                              sizeof(out[filled].name))
                       != AXL_OK) {
                axl_snprintf(out[filled].name, sizeof(out[filled].name),
                             "fs%zu", i);
            }
        }
        filled++;
    }

    axl_free(handles);

    *count = (out != NULL) ? (filled < max ? filled : max) : filled;
    return AXL_OK;
}

/* Build a ':'-terminated UCS-2 shell mapping name from a bare/optionally
   ':'-terminated UTF-8 name (e.g. "fs2" or "RD" -> "fs2:"/"RD:"). Caller
   frees. NULL on OOM / empty. */
static unsigned short *
map_name_to_ucs2(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    size_t n = axl_strlen(name);
    if (name[n - 1] == ':') {
        return axl_utf8_to_ucs2(name);   /* already terminated */
    }
    char *tmp = axl_malloc(n + 2);
    if (tmp == NULL) {
        return NULL;
    }
    axl_memcpy(tmp, name, n);
    tmp[n]     = ':';
    tmp[n + 1] = '\0';
    unsigned short *w = axl_utf8_to_ucs2(tmp);
    axl_free(tmp);
    return w;
}

bool
axl_volume_map_taken(const char *name)
{
    unsigned short *w = map_name_to_ucs2(name);
    if (w == NULL) {
        return false;
    }
    bool taken = axl_backend_shell_map_exists(w);
    axl_free(w);
    return taken;
}

int
axl_volume_set_map(const void *device_path, const char *name)
{
    if (device_path == NULL) {
        return AXL_ERR;
    }
    unsigned short *w = map_name_to_ucs2(name);
    if (w == NULL) {
        return AXL_ERR;
    }
    int rc = axl_backend_shell_set_map((void *)device_path, w);
    axl_free(w);
    return rc;
}

int
axl_volume_alias_to_fsn(const char *alias, const char *fsn)
{
    if (alias == NULL || fsn == NULL || alias[0] == '\0' || fsn[0] == '\0') {
        return AXL_ERR;
    }
    /* Drive the shell's own `map <alias> <fsn>:` command — the only way to add
       a named alias on a shell (like the old EFI 1.x shell) that has no
       programmatic SetMap. Unlike SetMap, this aliases an EXISTING fs mapping
       (so it inherits its resolvable device path). Runs through
       axl_backend_shell_execute, which uses EFI_SHELL_PROTOCOL.Execute on the
       modern shell or SHELL_ENVIRONMENT.Execute on the old one.

       The `> nul` redirect discards the map command's own output — including
       the error line the old shell prints when the alias can't resolve (its
       backward-compatible / startup.nsh mode, where the fsN has no device-path
       alias). Execute's return still reflects success/failure, so the caller
       can tell whether the alias actually took without the console noise. */
    char cmd[96];
    int n = axl_snprintf(cmd, sizeof(cmd), "map %s %s: > nul", alias, fsn);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return AXL_ERR;
    }
    unsigned short *w = axl_utf8_to_ucs2(cmd);
    if (w == NULL) {
        return AXL_ERR;
    }
    int rc = axl_backend_shell_execute(w);
    axl_free(w);
    return rc;
}

int
axl_volume_unmap(const char *name)
{
    unsigned short *w = map_name_to_ucs2(name);
    if (w == NULL) {
        return AXL_ERR;
    }
    int rc = axl_backend_shell_unmap(w);
    axl_free(w);
    return rc;
}

int
axl_volume_map_name(const void *device_path, char *out, size_t out_size)
{
    if (device_path == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }
    /* Delegate to the shell-map lookup (GetMapFromDevicePath). Unlike the
       enumerate path, there is NO positional fs<i> fallback here: a device
       path the shell hasn't mapped returns AXL_ERR, so callers publishing a
       usable fsN never emit a synthesized index. The backend takes a mutable
       pointer (GetMapFromDevicePath advances a local copy); the caller's path
       is not modified. */
    return axl_backend_shell_map_name((void *)device_path, out, out_size);
}

int
axl_volume_map_alias(const void *device_path, char *out, size_t out_size)
{
    if (device_path == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }
    /* Returns the FIRST alias verbatim (any form) — unlike map_name, which
       filters to fs<n>. The backend takes a mutable pointer; the caller's
       path is not modified. */
    return axl_backend_shell_map_alias((void *)device_path, out, out_size);
}
