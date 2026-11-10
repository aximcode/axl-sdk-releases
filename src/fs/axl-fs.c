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
#include <axl/axl-path.h>
#include <axl/axl-sys.h>
#include "axl-file-gen.h"
#include "axl-fs-internal.h"
AXL_LOG_DOMAIN("fs");

// ---------------------------------------------------------------------------
// Internal types and macros
// ---------------------------------------------------------------------------

/* Capacity for a name read back from the filesystem (see the internal
   header): any name FAT creates, in UTF-8 with its NUL. */
#define FS_NAME_MAX  AXL_FS_NAME_MAX
/* A name read back in UCS-2 units, with its NUL: as many units as bytes,
   since every unit is at least one byte of UTF-8 -- a longer name cannot
   fit FS_NAME_MAX whichever its characters. */
#define FS_NAME_UNITS  AXL_FS_NAME_MAX
/* EFI_FILE_INFO with a name of FS_NAME_UNITS units: FileName at offset 80. */
#define FS_DIR_INFO_BYTES  (80u + FS_NAME_UNITS * 2u)
/* Largest EFI_FILE_INFO a directory read will allocate to step past an entry
   too long for AxlDir.buf: a name of 32,767 units. */
#define FS_DIR_INFO_LIMIT  (80u + 65536u)

struct AxlDir {
    AxlFileHandle  handle;
    uint32_t       report_key;  /* axl_fs_report_key of the path it was opened by */
    uint8_t        buf[FS_DIR_INFO_BYTES];   /* scratch for EFI_FILE_INFO */
};

/* One open directory on axl_dir_walk's heap stack. */
typedef struct {
    AxlDir *dir;    /* open on path */
    char   *path;   /* heap; the directory's full path */
} WalkFrame;

/* Longest volume root we carry: name + ':' + separator + NUL. Shell map
   names are short ("fs0:", "RD:"); a longer one is rejected outright
   rather than truncated -- see volume_root_prefix. */
#define AXL_VOLUME_ROOT_MAX  AXL_FS_ROOT_MAX

/* AxlFsProviderVolumeInfo documents (uint64_t)-1 as "unknown", and the
   provider thunk synthesizes it for a provider with no volume_info
   callback. Real firmware can report it too. It must never reach a
   caller as a figure. */
#define AXL_VOLUME_SPACE_UNKNOWN  ((uint64_t)-1)

/* Sanity bound on the EFI_FILE_SYSTEM_INFO size a volume asks for. The
   struct plus a UCS-2 label is well under 1 KB; the figure comes from
   firmware or a third-party provider, so it is bounded before it
   reaches malloc rather than trusted. */
#define AXL_VOLUME_INFO_MAX  4096u

/* Escaped characters shown of a name in a log line before "...". */
#define FS_ESCAPED_SHOWN  48u

/* Keys of the directories most recently reported (axl_fs_report_first). */
#define FS_REPORT_RING  16u

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------

/* The directories most recently reported for a dropped name. */
static uint32_t mReportRing[FS_REPORT_RING];
static size_t   mReportCount;

// ---------------------------------------------------------------------------
// Reporting dropped names
// ---------------------------------------------------------------------------

void
axl_fs_escape_name(
    const char  *name,
    char        *out,
    size_t       cap
    )
{
    size_t w = 0;
    const unsigned char *c = (const unsigned char *)name;

    if (cap == 0) {
        return;   /* no room even for the NUL */
    }
    for (; *c != '\0'; c++) {
        char   one[5];
        size_t n;
        if (*c == '\\' || *c == '"') {
            one[0] = '\\';
            one[1] = (char)*c;
            n      = 2;
        } else if (*c < 0x20 || *c >= 0x7F) {
            axl_snprintf(one, sizeof(one), "\\x%02X", (unsigned)*c);
            n = 4;
        } else {
            one[0] = (char)*c;
            n      = 1;
        }
        if (w + n > FS_ESCAPED_SHOWN || w + n + 4 > cap) {
            break;
        }
        axl_memcpy(out + w, one, n);
        w += n;
    }
    if (*c != '\0' && w + 4 <= cap) {
        axl_memcpy(out + w, "...", 3);
        w += 3;
    }
    out[(w < cap) ? w : cap - 1] = '\0';
}

uint32_t
axl_fs_report_key(
    const void  *salt,
    const char  *dir
    )
{
    uint32_t  h = 2166136261u;
    uintptr_t s = (uintptr_t)salt;
    for (size_t i = 0; i < sizeof(s); i++) {
        h = (h ^ (uint32_t)((s >> (8u * i)) & 0xFFu)) * 16777619u;
    }
    for (const unsigned char *c = (const unsigned char *)dir; *c != '\0'; c++) {
        h = (h ^ *c) * 16777619u;
    }
    return h;
}

bool
axl_fs_report_first(
    uint32_t  key
    )
{
    size_t n = (mReportCount < FS_REPORT_RING) ? mReportCount : FS_REPORT_RING;
    for (size_t i = 0; i < n; i++) {
        if (mReportRing[i] == key) {
            return false;
        }
    }
    mReportRing[mReportCount % FS_REPORT_RING] = key;
    mReportCount++;
    return true;
}

// ---------------------------------------------------------------------------
// Whole-file helpers
// ---------------------------------------------------------------------------

AxlStatus
axl_file_get_contents_status(
    const char  *path,
    bool         refuse_dir,
    void       **buf,
    size_t      *len
    )
{
    if (path == NULL || buf == NULL || len == NULL) {
        return AXL_INVALID;
    }

    /* Firmware Read is uninterruptible, but a caller batching many
       file reads sees a yield between each. */
    _axl_poll_break();

    unsigned short *wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        axl_debug("utf8_to_ucs2 failed: %s", path);
        return AXL_NO_RESOURCES;
    }

    AxlFileHandle handle;
    int rc = axl_backend_file_open(
        (const unsigned short *)wide_path, AXL_FILE_MODE_READ, 0, &handle);
    axl_free(wide_path);
    if (rc != AXL_OK) {
        axl_debug("open failed: %s", path);
        return (rc == AXL_NOT_FOUND) ? AXL_NOT_FOUND : AXL_IO_ERROR;
    }

    if (refuse_dir) {
        bool is_dir = false;
        if (axl_backend_file_handle_is_dir(handle, &is_dir) != AXL_OK
            || is_dir) {
            axl_backend_file_close(&handle);
            return AXL_IO_ERROR;
        }
    }

    int64_t file_size_signed = axl_backend_file_get_size(handle);
    if (file_size_signed < 0) {
        axl_backend_file_close(&handle);
        return AXL_IO_ERROR;
    }
    size_t file_size = (size_t)file_size_signed;

    void *data = axl_malloc(file_size + 1);
    if (data == NULL) {
        axl_debug("allocation failed for %zu bytes", file_size);
        axl_backend_file_close(&handle);
        return AXL_NO_RESOURCES;
    }

    size_t read_size = file_size;
    rc = axl_backend_file_read(handle, &read_size, data);
    axl_backend_file_close(&handle);

    /* Fewer bytes than the file holds is a failed read, not a shorter file:
       axl_backend_file_read returns AXL_OK with the bytes it got before a
       later chunk failed (axl-backend.h), and those are not the contents. */
    if (rc != AXL_OK || read_size != file_size) {
        axl_debug("read failed: %s (%zu of %zu bytes)", path, read_size,
                  file_size);
        /* Zeroed before it goes back to the pool: the bytes that DID arrive
           may be a secret's (a host key file read over unreliable virtual
           media), and the caller never sees this buffer to clear it. On
           success the caller owns it, and clears what it must. Volatile, so
           a dead-store pass cannot drop the writes before the free. */
        volatile unsigned char *v = data;
        for (size_t k = 0; k <= file_size; k++) {
            v[k] = 0;
        }
        axl_free(data);
        return AXL_IO_ERROR;
    }

    /* NUL-terminate for convenience(not counted in len) */
    ((char *)data)[read_size] = '\0';

    *buf = data;
    *len = read_size;
    return AXL_OK;
}

bool
axl_fs_name_is_one_component(
    const char  *name
    )
{
    if (name == NULL || name[0] == '\0' || axl_strcmp(name, ".") == 0
        || axl_strcmp(name, "..") == 0) {
        return false;
    }
    for (const char *c = name; *c != '\0'; c++) {
        if (*c == '\\' || *c == '/') {
            return false;
        }
    }
    return true;
}

int
axl_file_get_contents(const char *path, void **buf, size_t *len)
{
    return (axl_file_get_contents_status(path, false, buf, len) == AXL_OK)
        ? AXL_OK : AXL_ERR;
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
        axl_debug("utf8_to_ucs2 failed: %s", path);
        return AXL_ERR;
    }

    rc = axl_backend_file_open(
        (const unsigned short *)wide_path,
        AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE | AXL_FILE_MODE_CREATE,
        0, &handle);
    axl_free(wide_path);
    if (rc != AXL_OK) {
        axl_debug("open failed: %s", path);
        return AXL_ERR;
    }

    write_size = len;
    rc = axl_backend_file_write(handle, &write_size, buf);
    /* The file has changed even if the write below reports a failure — a
       short write still moved bytes. Tell any open reader unconditionally;
       bumping only on success is how a reader ends up serving a file that
       is neither the old one nor the one the caller was told failed. */
    axl_file_gen_bump(path);
    /* set_contents replaces the WHOLE file, so truncate to exactly len —
       otherwise a rewrite with a shorter buffer leaves the previous file's
       tail behind (open with CREATE does not shrink an existing file). A
       no-op when the file was not longer than len. */
    if (rc == AXL_OK && axl_backend_file_set_size(handle, (uint64_t)len)
                            != AXL_OK) {
        rc = AXL_ERR;
    }
    /* Flush EXPLICITLY, before the close. The close's own status is
       unreportable — EFI_FILE_PROTOCOL.Close is specified to return only
       EFI_SUCCESS, and axl_backend_file_close answers AXL_OK regardless —
       so without this a full volume, write-protected media or device error
       that only surfaces when the firmware pushes its buffers out would be
       reported to the caller as a successful write. AXL_OK from here means
       the bytes are on the volume, which is what axl_file_write_atomic
       relies on before it replaces anything. */
    if (rc == AXL_OK && axl_backend_file_flush(handle) != AXL_OK) {
        rc = AXL_ERR;
    }
    axl_backend_file_close(&handle);

    if (rc != AXL_OK) {
        axl_debug("write failed: %s", path);
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

    /* Write the full contents to the temp file first — and axl_file_set_contents
       flushes before it reports, so AXL_OK here means the temp's bytes are
       genuinely on the volume, not merely accepted by the firmware. That is
       exactly what makes the replace below safe: promoting a temp whose write
       had not actually landed would destroy a good target file in exchange for
       one that may be empty or short. The target is untouched until now. */
    if (axl_file_set_contents(temp, (buf != NULL) ? buf : "", len) != AXL_OK) {
        axl_file_delete(temp);            /* best-effort cleanup */
        axl_free(temp);
        return AXL_ERR;
    }

    /* Put the temp in the target's place. axl_file_replace sets an existing
       target aside, renames the temp into place, and only then removes the
       aside -- and puts the target back if the rename fails -- so no failure
       it can observe costs the target. (This used to delete the target and
       then rename, ignoring the delete's result: a rename that then failed
       had already lost the target.) */
    AxlFsEntry before;
    bool       existed = axl_file_info(path, &before) == AXL_OK;
    int        rc      = axl_file_replace(temp, path, 0, NULL);
    if (rc != AXL_OK) {
        /* The temp is litter unless the target is gone although it was
           there before -- one of the failures axl_file_replace cannot
           prevent. Then the temp holds the only complete contents, so it is
           left at "<path>.tmp" for recovery, as axl_piece_tree_save leaves
           its own. */
        AxlFsEntry after;
        if (!existed || axl_file_info(path, &after) == AXL_OK) {
            axl_file_delete(temp);
        }
    }
    axl_free(temp);
    return rc;
}

/* The scalars the backend reports, as an AxlFsEntry. Shared by the path
   stat and the handle stat so the two cannot fill it differently. */
static void
fs_entry_fill(
    AxlFsEntry *entry,
    uint64_t    size,
    uint64_t    alloc_size,
    uint64_t    mtime_unix,
    bool        is_dir,
    bool        read_only
    )
{
    entry->struct_size = sizeof(*entry);
    entry->version     = AXL_FS_ENTRY_VERSION;
    entry->size        = size;
    entry->alloc_size  = alloc_size;
    entry->mtime_unix  = mtime_unix;
    entry->attributes  = (is_dir    ? AXL_FS_ATTR_DIRECTORY : 0u)
                       | (read_only ? AXL_FS_ATTR_READ_ONLY : 0u);
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

    /* Path-stat doesn't populate name (caller has the path). */
    fs_entry_fill(entry, size, alloc_size, mtime_unix, is_dir, read_only);
    return AXL_OK;
}

int
axl_fs_path_stat_named(
    const char  *path,
    AxlFsEntry  *entry
    )
{
    AxlFileHandle h = NULL;

    if (path == NULL || entry == NULL) {
        return AXL_ERR;
    }
    axl_memset(entry, 0, sizeof(*entry));
    unsigned short *wide = axl_utf8_to_ucs2(path);
    if (wide == NULL) {
        return AXL_ERR;
    }
    int rc = axl_backend_file_open(wide, AXL_FILE_MODE_READ, 0, &h);
    axl_free(wide);
    if (rc != AXL_OK) {
        return (rc == AXL_NOT_FOUND) ? AXL_NOT_FOUND : AXL_ERR;
    }
    rc = axl_fs_handle_stat(h, entry);
    axl_backend_file_close(&h);
    return rc;
}

int
axl_fs_handle_stat(
    AxlFileHandle  handle,
    AxlFsEntry    *entry
    )
{
    uint64_t       size = 0, alloc_size = 0, mtime_unix = 0;
    bool           is_dir = false, read_only = false, cut = false;
    unsigned short name[FS_NAME_UNITS];

    axl_memset(entry, 0, sizeof(*entry));
    if (axl_backend_file_handle_stat(handle, &size, &alloc_size, &mtime_unix,
                                     &is_dir, &read_only, name, FS_NAME_UNITS,
                                     &cut)
            != AXL_OK) {
        return AXL_ERR;
    }
    /* A name too long for AxlFsEntry.name is left EMPTY, never cut: a cut
       name could be a sibling's (axl_fstat). */
    if (!cut && axl_fs_ucs2_utf8_len(name) < sizeof(entry->name)) {
        axl_ucs2_to_utf8_buf(name, entry->name, sizeof(entry->name));
    }
    fs_entry_fill(entry, size, alloc_size, mtime_unix, is_dir, read_only);
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
    /* Only on SUCCESS. The bump was unconditional, so a delete of a path that
       does not exist invalidated every generation-keyed cache entry for it --
       for a file that was never there and never removed. Nothing changed, so
       nothing should be dirtied. */
    if (rc == AXL_OK) {
        axl_file_gen_bump(path);
    }
    return rc;
}

/* Return pointer to the basename of @p path — the substring after
   the last '/' or '\' separator. Accepts mixed-separator paths
   ("fs0:\\dir/foo.txt" → "foo.txt"). For separator-free input,
   returns @p path unchanged. */
const char *
axl_fs_path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

/* Resolve "." and ".." in a volume-relative path ('\' separators, no
   leading one) in place. The result has no leading, trailing or doubled
   separator; "" is the root.

   @return false when ".." climbs above the root. */
static bool
vol_path_normalize(char *p)
{
    size_t r = 0;
    size_t w = 0;

    while (p[r] != '\0') {
        size_t start = r;
        while (p[r] != '\0' && p[r] != '\\') {
            r++;
        }
        size_t n = r - start;
        if (p[r] == '\\') {
            r++;
        }
        if (n == 0 || (n == 1 && p[start] == '.')) {
            continue;
        }
        if (n == 2 && p[start] == '.' && p[start + 1] == '.') {
            if (w == 0) {
                return false;
            }
            while (w > 0 && p[w - 1] != '\\') {
                w--;
            }
            if (w > 0) {
                w--;   /* the separator before the dropped component */
            }
            continue;
        }
        if (w > 0) {
            p[w++] = '\\';
        }
        axl_memmove(p + w, p + start, n);
        w += n;
    }
    p[w] = '\0';
    return true;
}

/* Where @p path lands: its volume and its normalized volume-relative path
   (UTF-8, at most @p cap bytes).

   @return AXL_OK, or AXL_ERR when it does not resolve or climbs above the
       volume root. */
int
axl_fs_path_locate(
    const char  *path,
    void       **volume,
    char        *rel,
    size_t       cap
    )
{
    unsigned short wrel[AXL_BACKEND_VOL_PATH_MAX];
    unsigned short *w = axl_utf8_to_ucs2(path);

    if (w == NULL) {
        return AXL_ERR;
    }
    int rc = axl_backend_path_locate(w, volume, wrel, AXL_BACKEND_VOL_PATH_MAX);
    axl_free(w);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }
    axl_ucs2_to_utf8_buf(wrel, rel, cap);
    return vol_path_normalize(rel) ? AXL_OK : AXL_ERR;
}

/* Length of the directory part of a normalized volume-relative path: what
   comes before its last separator ("" -- length 0 -- for the root). */
static size_t
vol_path_dir_len(const char *rel)
{
    size_t len = 0;
    for (size_t i = 0; rel[i] != '\0'; i++) {
        if (rel[i] == '\\') {
            len = i;
        }
    }
    return len;
}

/* Is @p rel @p dir itself or below it? ASCII case folded, as FAT names. */
static bool
vol_path_within(const char *rel, const char *dir)
{
    size_t n = axl_strlen(dir);
    return axl_strncasecmp(rel, dir, n) == 0
           && (rel[n] == '\0' || rel[n] == '\\');
}

/* The SetInfo FileName for a rename of @p old_path to @p new_path. A bare
   new name, or a path into
   the SAME directory, is passed as a bare name, which every filesystem --
   an AxlFsProvider included -- takes as a rename in place. Only a move to
   another directory is passed as a root-anchored path.

   @return AXL_OK with @p target filled; AXL_UNSUPPORTED when the paths are
       on different volumes; AXL_ERR on a path that does not resolve, or a
       directory moved into itself or below it. */
static int
rename_target(
    const char    *old_path,
    const char    *new_path,
    AxlFileHandle  handle,
    char          *target,
    size_t         cap
    )
{
    const char *new_base = axl_fs_path_basename(new_path);
    bool        bare     = true;
    char        rel_o[AXL_BACKEND_VOL_PATH_MAX * 3];
    char        rel_n[AXL_BACKEND_VOL_PATH_MAX * 3];
    void       *vol_o = NULL;
    void       *vol_n = NULL;

    for (const char *p = new_path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            bare = false;
        }
    }
    if (bare) {
        axl_strlcpy(target, new_base, cap);
        return AXL_OK;
    }

    if (axl_fs_path_locate(old_path, &vol_o, rel_o, sizeof(rel_o)) != AXL_OK
        || axl_fs_path_locate(new_path, &vol_n, rel_n, sizeof(rel_n)) != AXL_OK) {
        return AXL_ERR;
    }
    if (vol_o != vol_n) {
        return AXL_UNSUPPORTED;
    }

    size_t dir_o = vol_path_dir_len(rel_o);
    size_t dir_n = vol_path_dir_len(rel_n);
    if (dir_o == dir_n && axl_strncmp(rel_o, rel_n, dir_o) == 0) {
        axl_strlcpy(target, new_base, cap);
        return AXL_OK;
    }

    /* The driver would refuse a directory into itself too, but only some
       drivers check; refusing here keeps it a clean AXL_ERR everywhere. */
    bool is_dir = false;
    if (rel_o[0] == '\0'
        || axl_backend_file_handle_is_dir(handle, &is_dir) != AXL_OK
        || (is_dir && vol_path_within(rel_n, rel_o))) {
        return AXL_ERR;
    }
    if (axl_strlen(rel_n) + 2 > cap) {
        return AXL_ERR;
    }
    target[0] = '\\';
    axl_strlcpy(target + 1, rel_n, cap - 1);
    return AXL_OK;
}

/* The current name of the entry @p handle is open on, in UTF-8. Only
   AXL_FS_NAME_WHOLE is a name to compare: a cut one would compare as a prefix. */
AxlFsNameRead
axl_fs_handle_name(AxlFileHandle handle, char *out, size_t cap)
{
    unsigned short name[FS_NAME_UNITS];

    out[0] = '\0';
    bool cut = false;
    if (axl_backend_file_handle_stat(handle, NULL, NULL, NULL, NULL, NULL,
                                     name, FS_NAME_UNITS, &cut) != AXL_OK) {
        return AXL_FS_NAME_FAILED;
    }
    axl_ucs2_to_utf8_buf(name, out, cap);
    return (cut || axl_fs_ucs2_utf8_len(name) >= cap) ? AXL_FS_NAME_CUT
                                                      : AXL_FS_NAME_WHOLE;
}

size_t
axl_fs_ucs2_utf8_len(const unsigned short *s)
{
    size_t n = 0;
    for (; *s != 0; s++) {
        n += (*s < 0x80u) ? 1u : (*s < 0x800u) ? 2u : 3u;
    }
    return n;
}

/* Does the name the filesystem stored mean the rename to @p want happened?
   Either exactly @p want, or -- FAT drops trailing dots and spaces from a
   long name ("a." is stored as "a") -- @p want with those stripped. But a
   stored name equal to the one the entry had BEFORE is no rename at all (a
   case-only change, a provider that ignored the request), unless that is
   what was asked. */
bool
axl_fs_rename_landed(const char *stored, const char *want, const char *before)
{
    size_t n = axl_strlen(want);
    while (n > 0 && (want[n - 1] == '.' || want[n - 1] == ' ')) {
        n--;
    }
    bool stripped = axl_strlen(stored) == n && axl_strncmp(stored, want, n) == 0;
    if (axl_strcmp(stored, want) != 0 && !stripped) {
        return false;
    }
    return axl_strcmp(stored, before) != 0 || axl_strcmp(want, before) == 0;
}

/* Does @p dest, in the source's own directory, open the entry @p handle is
   open on? The stored names read through both handles are compared
   exactly: in one directory, one stored name is one entry. */
static bool
rename_names_the_source(const char *dest, AxlFileHandle handle)
{
    unsigned short *wide = axl_utf8_to_ucs2(dest);
    AxlFileHandle   h    = NULL;
    char            a[FS_NAME_MAX];
    char            b[FS_NAME_MAX];
    bool            same = false;

    if (wide != NULL
        && axl_backend_file_open(wide, AXL_FILE_MODE_READ, 0, &h) == AXL_OK) {
        same = axl_fs_handle_name(h, a, sizeof(a)) == AXL_FS_NAME_WHOLE
               && axl_fs_handle_name(handle, b, sizeof(b)) == AXL_FS_NAME_WHOLE
               && axl_strcmp(a, b) == 0;
        axl_backend_file_close(&h);
    }
    axl_free(wide);
    return same;
}

/* axl_file_rename on paths whose separators are all '\'. */
static int
file_rename_backslashed(const char *old_path, const char *new_path)
{
    unsigned short *wide;
    AxlFileHandle   handle = NULL;
    AxlFsEntry      entry;
    char            target[AXL_BACKEND_VOL_PATH_MAX * 3];
    char            before[FS_NAME_MAX];
    char            stored[FS_NAME_MAX];
    char           *dest   = NULL;
    bool            is_dir = false;
    int             rc;

    if (old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }
    const char *new_base = axl_fs_path_basename(new_path);
    if (*new_base == '\0' || axl_strcmp(new_base, ".") == 0
        || axl_strcmp(new_base, "..") == 0) {
        return AXL_ERR;   /* "fs0:\dir\", or no name at all */
    }

    wide = axl_utf8_to_ucs2(old_path);
    if (wide == NULL) {
        return AXL_ERR;
    }
    /* No CREATE: a missing source is reported, never made. */
    rc = axl_backend_file_open(wide, AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE,
                               0, &handle);
    axl_free(wide);
    if (rc != AXL_OK) {
        return (rc == AXL_NOT_FOUND) ? AXL_NOT_FOUND : AXL_ERR;
    }

    rc = rename_target(old_path, new_path, handle, target, sizeof(target));
    if (rc == AXL_OK && axl_backend_file_handle_is_dir(handle, &is_dir) != AXL_OK) {
        rc = AXL_ERR;
    }
    if (rc != AXL_OK) {
        axl_backend_file_close(&handle);
        return rc;
    }

    /* The destination as a path: @p new_path, or for a bare name the old
       directory plus that name -- a bare name is relative to the source's
       directory, not the current one. */
    if (new_base == new_path) {
        size_t dir_len = (size_t)(axl_fs_path_basename(old_path) - old_path);
        dest = axl_malloc(dir_len + axl_strlen(new_base) + 1);
        if (dest != NULL) {
            axl_memcpy(dest, old_path, dir_len);
            axl_strlcpy(dest + dir_len, new_base, axl_strlen(new_base) + 1);
        }
    } else {
        dest = axl_strdup(new_path);
    }
    if (dest == NULL) {
        axl_backend_file_close(&handle);
        return AXL_ERR;
    }

    /* NEVER replaces. FAT refuses an existing name itself, but a provider's
       set_info may well replace it (POSIX rename does), so look first. A
       rename to the entry's own exact name is the one exception: it names
       the entry itself, and the driver treats it as the no-op it is. So is
       a name in the same directory that resolves to the source itself -- a
       case-only change on FAT -- which nothing ELSE has taken: it goes to
       the driver, which changes nothing, and the verification below reports
       that as the AXL_ERR it always was. */
    bool to_self = target[0] != '\\'
                   && axl_strcmp(target, axl_fs_path_basename(old_path)) == 0;
    if (!to_self && axl_file_info(dest, &entry) == AXL_OK
        && !(target[0] != '\\' && rename_names_the_source(dest, handle))) {
        axl_backend_file_close(&handle);
        axl_free(dest);
        return AXL_EXISTS;
    }

    if (axl_fs_handle_name(handle, before, sizeof(before)) != AXL_FS_NAME_WHOLE) {
        axl_backend_file_close(&handle);
        axl_free(dest);
        return AXL_ERR;
    }
    wide = axl_utf8_to_ucs2(target);
    rc   = (wide != NULL) ? axl_backend_file_handle_rename(handle, wide)
                          : AXL_ERR;
    axl_free(wide);
    if (rc == AXL_UNSUPPORTED) {
        axl_backend_file_close(&handle);
        axl_free(dest);
        return AXL_UNSUPPORTED;   /* the filesystem changed nothing */
    }

    /* AXL_OK means VERIFIED: re-read the entry through the same handle. A
       filesystem that accepts the request and keeps the old name -- a
       provider that ignores set_info, FAT given a case-only change -- has
       not renamed anything. */
    if (rc == AXL_OK
        && (axl_fs_handle_name(handle, stored, sizeof(stored)) != AXL_FS_NAME_WHOLE
            || !axl_fs_rename_landed(stored, new_base, before))) {
        rc = AXL_ERR;
    }
    axl_backend_file_close(&handle);

    /* BOTH names changed meaning: the old one no longer resolves and the
       new one now names different content. A renamed directory -- moved or
       renamed in place, the same thing to what lies below it -- changes
       every path below it too, which no per-name key can follow. */
    axl_file_gen_bump(old_path);
    axl_file_gen_bump(dest);
    if (is_dir) {
        axl_file_gen_bump_all();
    }

    /* The EDK2-derived FAT driver removes the old directory entry before it
       writes the new one, so a failure in between loses the entry under
       BOTH names. The return value cannot say that; this can. */
    if (rc != AXL_OK && axl_file_info(old_path, &entry) != AXL_OK
        && axl_file_info(dest, &entry) != AXL_OK) {
        axl_error("rename of '%s' to '%s' failed part-way: the entry is gone "
                  "under both names", old_path, dest);
    }
    axl_free(dest);
    return (rc == AXL_OK) ? AXL_OK : AXL_ERR;
}

/* A copy of @p path with every '/' turned into '\\', or NULL on OOM. */
char *
axl_fs_path_backslashed(const char *path)
{
    char *copy = axl_strdup(path);
    for (char *p = copy; p != NULL && *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    return copy;
}

int
axl_file_rename(const char *old_path, const char *new_path)
{
    if (old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }
    /* "Either separator" is part of the contract, but the modern shell's
       OpenFileByName does not take '/' -- so every path this opens, stats
       and resolves is spelled with '\\' first. */
    char *old_bs = axl_fs_path_backslashed(old_path);
    char *new_bs = axl_fs_path_backslashed(new_path);
    int   rc     = (old_bs != NULL && new_bs != NULL)
                   ? file_rename_backslashed(old_bs, new_bs) : AXL_ERR;
    axl_free(old_bs);
    axl_free(new_bs);
    return rc;
}

/* @p name joined onto the directory part of @p path ("fs0:\d\x" + "y" ->
   "fs0:\d\y"), allocated; NULL on OOM. */
char *
axl_fs_path_sibling(const char *path, const char *name)
{
    size_t dir_len  = (size_t)(axl_fs_path_basename(path) - path);
    size_t name_len = axl_strlen(name);
    char  *out      = axl_malloc(dir_len + name_len + 1);
    if (out != NULL) {
        axl_memcpy(out, path, dir_len);
        axl_memcpy(out + dir_len, name, name_len + 1);
    }
    return out;
}

int
axl_file_move(const char *old_path, const char *new_path)
{
    return axl_file_replace(old_path, new_path, AXL_FILE_REPLACE_COPY, NULL);
}

int
axl_fs_handle_truncate(
    AxlFileHandle  handle,
    uint64_t       size
    )
{
    bool is_dir = true;
    int  rc;

    /* Growing physically writes the added region on the FAT driver, so a
       large resize is real I/O — yield first, like the other whole-file
       operations, so a batching caller stays Ctrl-C responsive. */
    _axl_poll_break();

    /* A directory is refused — UEFI derives a directory's size from its
       contents, and the spec has SetInfo either ignore FileSize on a
       directory or fail it, so checking first turns a driver-dependent
       outcome into one clear refusal. */
    if (axl_backend_file_handle_is_dir(handle, &is_dir) != AXL_OK || is_dir) {
        return AXL_ERR;
    }

    rc = axl_backend_file_set_size(handle, size);
    if (rc == AXL_OK) {
        /* AXL_OK means VERIFIED, not merely requested. A filesystem may
           accept a size-carrying SetInfo and change nothing — an
           AxlFsProvider whose `set_info` only implements rename and
           attribute changes ignores the size outright. Re-read the length
           from the same open handle so a silent no-op can't pass for
           success. */
        int64_t actual = axl_backend_file_get_size(handle);
        if (actual < 0 || (uint64_t)actual != size) {
            rc = AXL_ERR;
        }
    }
    /* The re-read above proves the driver ACCEPTED the new length; it reads
       the same open handle, so it cannot prove the change reached the media.
       Flush for that, before the close whose status is unreportable
       (EFI_FILE_PROTOCOL.Close is specified to return only EFI_SUCCESS) --
       otherwise "AXL_OK means VERIFIED" stops being true on exactly the
       volumes where it matters. Same contract as axl_file_set_contents. */
    if (rc == AXL_OK && axl_backend_file_flush(handle) != AXL_OK) {
        rc = AXL_ERR;
    }
    return (rc == AXL_OK) ? AXL_OK : AXL_ERR;
}

int
axl_file_truncate(const char *path, uint64_t size)
{
    unsigned short *wide_path;
    AxlFileHandle   handle = NULL;
    int             rc;

    if (path == NULL) {
        return AXL_ERR;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    /* Deliberately no CREATE — the file must already exist, so a missing
       one fails here. */
    rc = axl_backend_file_open((const unsigned short *)wide_path,
                               AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE,
                               0, &handle);
    axl_free(wide_path);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }

    rc = axl_fs_handle_truncate(handle, size);
    axl_backend_file_close(&handle);
    axl_file_gen_bump(path);
    return rc;
}

int
axl_fs_handle_set_mtime(
    AxlFileHandle  handle,
    uint64_t       mtime_unix
    )
{
    AxlFsEntry before;
    AxlFsEntry after;

    if (axl_fs_handle_stat(handle, &before) != AXL_OK) {
        return AXL_ERR;
    }
    /* The EDK2-derived FAT driver accepts a time for a read-only entry and
       silently keeps the old one (Info.c), so refuse up front and say why
       rather than fail the verification below. */
    if (axl_fs_entry_is_read_only(&before)) {
        return AXL_DENIED;
    }
    if (axl_backend_file_set_mtime(handle, mtime_unix) != AXL_OK) {
        return AXL_ERR;
    }

    /* AXL_OK means VERIFIED: FAT stores two-second steps, so the request or
       the second below it. Anything else -- a provider that never sees the
       time, the driver dropping the year of a 1980 time -- is put back the
       way it was. 0 is "unknown", which there is no way to write back. */
    if (axl_fs_handle_stat(handle, &after) != AXL_OK
        || (after.mtime_unix != mtime_unix
            && after.mtime_unix + 1u != mtime_unix)) {
        if (before.mtime_unix != 0
            && (axl_backend_file_set_mtime(handle, before.mtime_unix) != AXL_OK
                || axl_backend_file_flush(handle) != AXL_OK)) {
            axl_debug("set_mtime: restoring the previous time failed");
        }
        return AXL_ERR;
    }
    return (axl_backend_file_flush(handle) == AXL_OK) ? AXL_OK : AXL_ERR;
}

int
axl_file_set_mtime(const char *path, uint64_t mtime_unix)
{
    unsigned short *wide_path;
    AxlFileHandle   handle = NULL;
    AxlFsEntry      entry;
    int             rc;

    if (path == NULL) {
        return AXL_ERR;
    }
    /* Before any I/O: the driver would clamp a later year to 2107 and
       refuse an earlier one, neither of which says what was wrong. */
    if (mtime_unix < AXL_FS_MTIME_MIN || mtime_unix > AXL_FS_MTIME_MAX) {
        return AXL_INVALID;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }
    /* No CREATE: the path must exist. */
    rc = axl_backend_file_open((const unsigned short *)wide_path,
                               AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE,
                               0, &handle);
    axl_free(wide_path);
    if (rc == AXL_NOT_FOUND) {
        return AXL_NOT_FOUND;
    }
    if (rc != AXL_OK) {
        /* FAT refuses a write open of a read-only FILE outright, so the
           attribute is the likely reason; a read-only volume has none. */
        return (axl_file_info(path, &entry) == AXL_OK
                && axl_fs_entry_is_read_only(&entry)) ? AXL_DENIED : AXL_ERR;
    }

    rc = axl_fs_handle_set_mtime(handle, mtime_unix);
    axl_backend_file_close(&handle);
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

    /* The UEFI FAT create-directory primitive opens an existing entry instead
       of failing, so it reports success both for an existing directory (which
       we want to treat as idempotent — mkdir-p and WebDAV COPY-overwrite rely
       on it) AND for a non-directory occupying the path (a silent conflict we
       must reject). Resolve the ambiguity up front: an existing directory is
       success, an existing non-directory is an error. */
    AxlFsEntry existing;
    if (axl_file_info(path, &existing) == AXL_OK) {
        return axl_fs_entry_is_dir(&existing) ? AXL_OK : AXL_ERR;
    }

    wide_path = axl_utf8_to_ucs2(path);
    if (wide_path == NULL) {
        return AXL_ERR;
    }

    rc = axl_backend_file_mkdir((const unsigned short *)wide_path);
    axl_free(wide_path);
    /* A directory now occupies a name that did not resolve before. Nothing
       views a directory today, but the registry keys the NAMESPACE, not
       just file bytes, and a view left open on a path that has become a
       directory must not keep serving the file that used to be there. */
    axl_file_gen_bump(path);
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
    axl_file_gen_bump(path);
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

    dir->handle     = handle;
    dir->report_key = axl_fs_report_key(NULL, path);
    return dir;
}

/* Report an entry axl_dir_read skipped: the first in a directory at
   warning, the rest at debug. @p name is shown escaped and shortened. */
static void
fs_report_skipped(const AxlDir *dir, const char *name)
{
    char esc[64];
    axl_fs_escape_name(name, esc, sizeof(esc));
    if (axl_fs_report_first(dir->report_key)) {
        /* log-level: the caller sees only a shorter listing; nothing it is
           returned says an entry was skipped. */
        axl_warning("dir_read: skipped \"%s\": not one entry name, or longer "
                    "than %zu bytes", esc, sizeof(((AxlFsEntry *)0)->name) - 1);
    } else {
        axl_debug("dir_read: skipped \"%s\": not one entry name, or longer "
                  "than %zu bytes", esc, sizeof(((AxlFsEntry *)0)->name) - 1);
    }
}

bool
axl_dir_read(AxlDir *dir, AxlFsEntry *entry)
{
    size_t buf_size;
    int rc;

    if (dir == NULL || entry == NULL) {
        return false;
    }

    /* One entry per call; an entry whose name is not ONE path component is
       not returned (see the loop's end), so the loop reads on past it. */
    for (;;) {
        /* Read one directory entry (EFI_FILE_INFO). dir->buf holds any
           entry whose name could fit AxlFsEntry.name; a larger one is read
           into a heap buffer only to step past it -- ending the listing
           there would hide every entry after it. */
        buf_size = sizeof(dir->buf);
        rc = axl_backend_dir_read_entry(dir->handle, &buf_size, dir->buf);
        if (rc == AXL_NO_RESOURCES && buf_size <= FS_DIR_INFO_LIMIT) {
            void *big = axl_malloc(buf_size);
            rc = (big != NULL)
                 ? axl_backend_dir_read_entry(dir->handle, &buf_size, big)
                 : AXL_ERR;
            axl_free(big);
            if (rc != AXL_OK || buf_size == 0) {
                return false;
            }
            fs_report_skipped(dir, "(a name longer than the entry buffer)");
            continue;
        }
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
        /* The name: units the info's size holds, NUL-terminated here --
           a driver need not have, and the bytes past the info are stale. */
        unsigned short filename[(sizeof(dir->buf) - 80u) / sizeof(unsigned short) + 1u];
        size_t         units = (buf_size > 80u) ? (buf_size - 80u) / sizeof(unsigned short) : 0u;

        axl_memcpy(&file_size, dir->buf + 8,  sizeof(uint64_t));
        axl_memcpy(&attribute, dir->buf + 72, sizeof(uint64_t));
        axl_memcpy(filename, dir->buf + 80, units * sizeof(unsigned short));
        filename[units] = 0;

        entry->struct_size = sizeof(*entry);
        entry->version     = AXL_FS_ENTRY_VERSION;
        entry->size        = file_size;
        entry->alloc_size  = 0;
        entry->mtime_unix  = axl_backend_efi_time_to_unix(dir->buf + 56);
        /* EFI attribute → AXL attribute bits. EFI_FILE_DIRECTORY is
           0x10, READ_ONLY 0x01, etc. — AXL constants chosen identical
           so this is a literal mask copy. */
        entry->attributes  = (uint32_t)(attribute & 0x37u);

        /* The one choke point every directory listing passes, whatever the
           driver. Not returned: a name too long for AxlFsEntry.name, which
           cut short could be a sibling's name -- measured BEFORE converting,
           so no conversion can cut it; and a name that cannot be joined
           onto a path as ONE component -- empty, or holding '\\' or '/' --
           which would take a walker out of the directory it walks. "." and
           ".." are returned as the driver lists them. Converted straight
           into the entry, on no heap: the AXL heap failing must not empty a
           name. */
        if (axl_fs_ucs2_utf8_len(filename) >= sizeof(entry->name)) {
            char shown[64];
            axl_ucs2_to_utf8_buf(filename, shown, sizeof(shown));
            fs_report_skipped(dir, shown);
            continue;
        }
        axl_ucs2_to_utf8_buf(filename, entry->name, sizeof(entry->name));
        bool dots = axl_strcmp(entry->name, ".") == 0
                    || axl_strcmp(entry->name, "..") == 0;
        if (!dots && !axl_fs_name_is_one_component(entry->name)) {
            fs_report_skipped(dir, entry->name);
            continue;
        }

        return true;
    }
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
// Directory walk (iterative)
// ---------------------------------------------------------------------------

/* @p dir joined with @p name, on the heap: one @p sep between them unless
   @p dir already ends in a separator. NULL on OOM. Never cut. */
static char *
walk_join(
    const char *dir,
    char        sep,
    const char *name
    )
{
    size_t dlen = axl_strlen(dir);
    size_t nlen = axl_strlen(name);
    bool   has  = dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\');
    char  *out  = axl_malloc(dlen + 1 + nlen + 1);
    if (out == NULL) {
        return NULL;
    }
    axl_memcpy(out, dir, dlen);
    if (!has) {
        out[dlen++] = sep;
    }
    axl_memcpy(out + dlen, name, nlen + 1);
    return out;
}

/* Pop the top frame of @p st, closing its directory. */
static void
walk_pop(WalkFrame *st, size_t *depth)
{
    (*depth)--;
    axl_dir_close(st[*depth].dir);
    axl_free(st[*depth].path);
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

    /* Join children with the separator the root already uses, so the
       composed path reopens on strict volumes: a mixed-separator path (e.g.
       "FS1:\sub/b.txt") on a backslash-rooted volume fails on VirtioFsDxe
       and other strict providers. The root's last separator; with none,
       '\' after a "VOL:" prefix (a UEFI volume root), else '/'. */
    size_t root_len = axl_strlen(root);
    char   sep      = '/';
    bool   found    = false;
    for (size_t i = root_len; i > 0 && !found; i--) {
        if (root[i - 1] == '/' || root[i - 1] == '\\') {
            sep   = root[i - 1];
            found = true;
        }
    }
    for (size_t i = 0; i < root_len && !found; i++) {
        if (root[i] == ':') {
            sep   = '\\';
            found = true;
        }
    }

    /* Iterative, every frame and path on the heap: a recursion's stack
       grew by a directory handle, an entry and a path per level, against
       the 128 KiB UEFI promises, and its fixed path buffer CUT a long path
       -- the callback then walked a path that names something else. */
    AxlFsEntry *entry = axl_malloc(sizeof(*entry));
    size_t      cap   = 8;
    size_t      depth = 0;
    WalkFrame  *st    = axl_malloc(cap * sizeof(*st));
    char       *rpath = axl_strdup(root);
    AxlDir     *rdir  = axl_dir_open(root);
    if (entry == NULL || st == NULL || rpath == NULL || rdir == NULL) {
        axl_dir_close(rdir);
        axl_free(rpath);
        axl_free(st);
        axl_free(entry);
        return -1;
    }
    st[depth++] = (WalkFrame){ .dir = rdir, .path = rpath };

    int rc = 0;
    while (depth > 0 && rc == 0) {
        WalkFrame *top = &st[depth - 1];
        if (!axl_dir_read(top->dir, entry)) {
            walk_pop(st, &depth);
            continue;
        }
        if (axl_strcmp(entry->name, ".") == 0 || axl_strcmp(entry->name, "..") == 0) {
            continue;
        }
        char *full = walk_join(top->path, sep, entry->name);
        if (full == NULL) {
            rc = -1;
            break;
        }
        rc = fn(full, entry, user);
        if (rc != 0 || !axl_fs_entry_is_dir(entry) || depth >= (size_t)max_depth) {
            axl_free(full);
            continue;
        }
        /* A subdirectory that cannot be opened is skipped: the caller asked
           for a walk, not a check that the whole tree is readable. */
        AxlDir *sub = axl_dir_open(full);
        if (sub == NULL) {
            axl_free(full);
            continue;
        }
        if (depth == cap) {
            WalkFrame *grown = axl_realloc(st, 2 * cap * sizeof(*st));
            if (grown == NULL) {
                axl_dir_close(sub);
                axl_free(full);
                rc = -1;
                break;
            }
            st  = grown;
            cap *= 2;
        }
        st[depth++] = (WalkFrame){ .dir = sub, .path = full };
    }

    while (depth > 0) {
        walk_pop(st, &depth);
    }
    axl_free(st);
    axl_free(entry);
    return rc;
}

// ---------------------------------------------------------------------------
// Volume operations
// ---------------------------------------------------------------------------

/* Why three outcomes and not a bool: the caller may fall back to the
   working directory ONLY when the path named no volume at all. A path
   that named a volume we cannot represent must fail, because resolving
   it against the cwd would answer confidently about a DIFFERENT volume
   -- a "will this write fit?" caller would get another volume's free
   bytes and a clean AXL_OK. */
typedef enum {
    VOLUME_PREFIX_OK,     /* @p out holds the volume root */
    VOLUME_PREFIX_NONE,   /* no ':' — the path is relative to the cwd */
    VOLUME_PREFIX_BAD     /* named a volume: empty or too long to hold */
} VolumePrefixResult;

/* "fs0:\EFI\x.efi" -> "fs0:\". The trailing separator is not cosmetic:
   the shell's OpenFileByName rejects a bare map name ("fs0:") and opens
   the root for "fs0:\". */
static VolumePrefixResult
volume_root_prefix(
    const char *path,
    char       *out,
    size_t      out_size
    )
{
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == ':') {
            if (i == 0 || i + 3 > out_size) {   /* name + ':' + '\' + NUL */
                return VOLUME_PREFIX_BAD;
            }
            axl_strlcpy(out, path, i + 2);
            out[i + 1] = '\\';
            out[i + 2] = '\0';
            return VOLUME_PREFIX_OK;
        }
    }
    return VOLUME_PREFIX_NONE;
}

/**
 * Reduce any path to the root of the volume it names.
 *
 * Both the label and the free-space queries are about the VOLUME, so
 * they must answer the same for every spelling of it, and the caller is
 * often asking about a file that does not exist yet ("will this upload
 * fit?"). Opening the root rather than @p path itself gives both.
 *
 * A path with no volume prefix is resolved against the current working
 * directory, which is the volume such a path would land on.
 */
int
axl_fs_volume_root_of(
    const char *path,
    char       *out,
    size_t      out_size
    )
{
    if (path[0] == '\0') {
        return AXL_ERR;
    }
    switch (volume_root_prefix(path, out, out_size)) {
    case VOLUME_PREFIX_OK:
        return AXL_OK;
    case VOLUME_PREFIX_BAD:
        /* The path DID name a volume. Falling through to the cwd here
           would silently answer about the wrong one. */
        return AXL_ERR;
    case VOLUME_PREFIX_NONE:
        break;
    }

    AXL_AUTO_FREE char *cwd = axl_get_current_dir();
    if (cwd == NULL) {
        return AXL_ERR;
    }
    return (volume_root_prefix(cwd, out, out_size) == VOLUME_PREFIX_OK)
           ? AXL_OK : AXL_ERR;
}

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

    /* Ask the volume ROOT, not @p path itself. The label belongs to the
       volume, so every spelling of it ("fs0:", "fs0:\", "fs0:\dir\f")
       must answer the same — and the bare map name the docstring
       advertises is exactly the form the shell's OpenFileByName
       rejects, so passing @p path through verbatim returned NULL for
       the documented spelling. As a bonus the path need not exist. */
    char root_path[AXL_VOLUME_ROOT_MAX];
    if (axl_fs_volume_root_of(path, root_path, sizeof(root_path)) != AXL_OK) {
        return NULL;
    }
    unsigned short wpath[AXL_VOLUME_ROOT_MAX];
    axl_utf8_to_ucs2_buf(root_path, wpath, AXL_VOLUME_ROOT_MAX);

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
    axl_efi_call(file->GetInfo, 4, file, &vol_label_guid, &info_size, NULL);

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

    status = axl_efi_call(file->GetInfo, 4, file, &vol_label_guid,
                          &info_size, info_buf);
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
    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handle,
        (EFI_GUID *)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
        (void **)&fs);
    if (EFI_ERROR(status) || fs == NULL) {
        return NULL;
    }

    /* Open the root directory */
    status = axl_efi_call(fs->OpenVolume, 2, fs, &root);
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
    axl_efi_call(root->GetInfo, 4, root, &vol_label_guid, &info_size, NULL);

    if (info_size == 0) {
        axl_efi_call(root->Close, 1, root);
        return NULL;
    }

    /* Allocate and read */
    uint8_t *info_buf = (uint8_t *)axl_malloc(info_size);
    if (info_buf == NULL) {
        axl_efi_call(root->Close, 1, root);
        return NULL;
    }

    status = axl_efi_call(root->GetInfo, 4, root, &vol_label_guid,
                          &info_size, info_buf);
    axl_efi_call(root->Close, 1, root);

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
// Volume space
// ---------------------------------------------------------------------------

/* The EFI_FILE_SYSTEM_INFO figures AXL reads. */
typedef struct {
    uint64_t  volume_size;
    uint64_t  free_space;
    uint32_t  block_size;
    bool      read_only;
} VolumeInfo;

/**
 * Read EFI_FILE_SYSTEM_INFO off an open EFI_FILE_PROTOCOL.
 *
 * @return AXL_OK with @p out filled (sizes as reported, "unknown"
 *     included); AXL_UNSUPPORTED when the volume has no filesystem
 *     information to give; AXL_ERR when the query failed.
 */
static int
volume_info_from_file(
    EFI_FILE_PROTOCOL *file,
    VolumeInfo        *out
    )
{
    /* Size probe. Its STATUS is the interesting half, not just the
       size: a volume that has no filesystem information to give says
       EFI_UNSUPPORTED ("cannot report"), while a sick one says
       EFI_DEVICE_ERROR / EFI_NO_MEDIA / EFI_VOLUME_CORRUPTED ("this
       failed"). Both leave info_size at 0, so keying off the size
       alone would launder a device error into "cannot report" -- the
       one distinction this API exists to preserve. */
    size_t     info_size = 0;
    EFI_STATUS status    = axl_efi_call(file->GetInfo, 4, file,
                                        &gEfiFileSystemInfoGuid,
                                        &info_size, NULL);
    if (status == EFI_UNSUPPORTED) {
        return AXL_UNSUPPORTED;
    }
    if (status != EFI_BUFFER_TOO_SMALL
        || info_size < SIZE_OF_EFI_FILE_SYSTEM_INFO
        || info_size > AXL_VOLUME_INFO_MAX) {
        /* Includes the nonsense cases: a "success" that returned no
           buffer, and a size a sane filesystem never asks for (the
           struct plus a label). Both come from firmware or a
           third-party provider, so neither is trusted into malloc. */
        return AXL_ERR;
    }

    uint8_t *info_buf = (uint8_t *)axl_malloc(info_size);
    if (info_buf == NULL) {
        return AXL_ERR;
    }
    status = axl_efi_call(file->GetInfo, 4, file,
                          &gEfiFileSystemInfoGuid,
                          &info_size, info_buf);
    if (EFI_ERROR(status)) {
        axl_free(info_buf);
        return AXL_ERR;
    }

    const EFI_FILE_SYSTEM_INFO *fsi = (const EFI_FILE_SYSTEM_INFO *)info_buf;
    out->volume_size = (uint64_t)fsi->VolumeSize;
    out->free_space  = (uint64_t)fsi->FreeSpace;
    out->block_size  = (uint32_t)fsi->BlockSize;
    out->read_only   = fsi->ReadOnly != 0;
    axl_free(info_buf);
    return AXL_OK;
}

/**
 * Read VolumeSize / FreeSpace off an open EFI_FILE_PROTOCOL.
 *
 * Only writes an out-param when the volume reported a real figure, and
 * only fails on the figures the caller actually asked for — a volume
 * that knows its free space but not its total still answers a
 * free-space-only query.
 */
static int
volume_space_from_file(
    EFI_FILE_PROTOCOL *file,
    uint64_t          *total,
    uint64_t          *free_bytes
    )
{
    VolumeInfo vi;
    int        rc = volume_info_from_file(file, &vi);
    if (rc != AXL_OK) {
        return rc;
    }
    uint64_t vol_size  = vi.volume_size;
    uint64_t free_size = vi.free_space;

    if ((total != NULL && vol_size == AXL_VOLUME_SPACE_UNKNOWN)
        || (free_bytes != NULL && free_size == AXL_VOLUME_SPACE_UNKNOWN)) {
        return AXL_UNSUPPORTED;
    }
    if (total != NULL) {
        *total = vol_size;
    }
    if (free_bytes != NULL) {
        *free_bytes = free_size;
    }
    return AXL_OK;
}

int
axl_volume_get_space(
    const char *path,
    uint64_t   *total,
    uint64_t   *free_bytes
    )
{
    if (path == NULL || (total == NULL && free_bytes == NULL)) {
        return AXL_ERR;
    }

    char root[AXL_VOLUME_ROOT_MAX];
    if (axl_fs_volume_root_of(path, root, sizeof(root)) != AXL_OK) {
        return AXL_ERR;
    }

    unsigned short wroot[AXL_VOLUME_ROOT_MAX];
    axl_utf8_to_ucs2_buf(root, wroot, AXL_VOLUME_ROOT_MAX);

    AxlFileHandle fh = NULL;
    if (axl_backend_file_open(wroot,
                              AXL_FILE_MODE_READ, 0, &fh) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = volume_space_from_file((EFI_FILE_PROTOCOL *)fh,
                                    total, free_bytes);
    axl_backend_file_close(&fh);
    return rc;
}

int
axl_fs_volume_facts(
    const char        *path,
    AxlFsVolumeFacts  *out
    )
{
    char           root[AXL_VOLUME_ROOT_MAX];
    unsigned short wroot[AXL_VOLUME_ROOT_MAX];
    AxlFileHandle  fh = NULL;
    VolumeInfo     vi;

    if (path == NULL || out == NULL
        || axl_fs_volume_root_of(path, root, sizeof(root)) != AXL_OK) {
        return AXL_ERR;
    }
    axl_utf8_to_ucs2_buf(root, wroot, AXL_VOLUME_ROOT_MAX);
    if (axl_backend_file_open(wroot, AXL_FILE_MODE_READ, 0, &fh) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = volume_info_from_file((EFI_FILE_PROTOCOL *)fh, &vi);
    axl_backend_file_close(&fh);
    if (rc != AXL_OK) {
        return rc;
    }
    out->read_only  = vi.read_only;
    out->block_size = vi.block_size;
    out->free_bytes = (vi.free_space == AXL_VOLUME_SPACE_UNKNOWN)
                      ? UINT64_MAX : vi.free_space;
    return AXL_OK;
}

int
axl_volume_get_space_by_handle(
    void     *handle,
    uint64_t *total,
    uint64_t *free_bytes
    )
{
    if (handle == NULL || (total == NULL && free_bytes == NULL)) {
        return AXL_ERR;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_STATUS status = axl_efi_call(axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handle,
        (EFI_GUID *)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
        (void **)&fs);
    if (EFI_ERROR(status) || fs == NULL) {
        return AXL_ERR;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    status = axl_efi_call(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(status) || root == NULL) {
        return AXL_ERR;
    }
    int rc = volume_space_from_file(root, total, free_bytes);
    axl_efi_call(root->Close, 1, root);
    return rc;
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

int
axl_volume_pick_free_fsn(char *out, size_t out_size)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    for (uint32_t i = AXL_VOLUME_FSN_FLOOR; i < AXL_VOLUME_FSN_LIMIT; i++) {
        char cand[16];
        axl_snprintf(cand, sizeof(cand), "FS%u", i);
        if (!axl_volume_map_taken(cand)) {
            if (axl_strlen(cand) >= out_size) {
                return AXL_ERR;
            }
            axl_strlcpy(out, cand, out_size);
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

bool
axl_volume_map_is(const char *name, const void *device_path)
{
    if (device_path == NULL) {
        return false;
    }
    unsigned short *w = map_name_to_ucs2(name);
    if (w == NULL) {
        return false;
    }
    bool is = axl_backend_shell_map_is(w, (void *)device_path);
    axl_free(w);
    return is;
}

int
axl_volume_unmap_device(const void *device_path, size_t *removed)
{
    /* The backend works on the shell's own UCS-2 names, so a name of any
       length or spelling is removed as the shell stores it. */
    return axl_backend_shell_unmap_device((void *)device_path, removed);
}
