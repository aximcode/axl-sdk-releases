/** @file axl-test-flushfail-fs.h
    A publishable filesystem whose FLUSH always fails, plus an
    independent oracle for what actually reached its backing store.

    Every write path in AXL ends the same way: write the bytes, close
    the handle, report success. The status that says whether the bytes
    reached the media is the FLUSH one -- `axl_backend_file_close`
    returns AXL_OK unconditionally, because EFI_FILE_PROTOCOL.Close is
    specified to return only EFI_SUCCESS. A path that never asks for
    the flush status therefore reports success for data that a full
    volume, write-protected media or a device error silently dropped.

    Proving that regression needs media that fails a flush and nothing
    else, which QEMU cannot produce. `AxlFsProvider` can: publish a
    tiny in-memory filesystem whose `flush` returns
    AXL_FS_ERR_NO_SPACE while every other operation succeeds, give it
    a shell map name, and the path-based `axl_file_*` APIs reach it
    exactly the way they reach fs0:. The failure is then EXACTLY the
    one that matters -- every byte accepted, the flush refused.

    Header-only, one copy per test binary (the fixture is state, not a
    library). A TU that includes this must not define its own
    `struct AxlFsProviderFile`.

    Usage:

        if (!ff_fs_up()) { ...SKIP: no shell to map through... }
        ff_seed("target", "old", 3);
        test_check(axl_file_set_contents(FF_PATH("target"), "new", 3)
                       == AXL_ERR, "...");
        test_check(ff_content_is("target", "old", 3), "...");
        ff_fs_down();

    `ff_seed` / `ff_content_is` / `ff_exists` reach the backing store
    DIRECTLY rather than through the code under test, so a test's
    "what is actually stored" oracle cannot be fooled by the same bug
    it is checking for.
**/

#ifndef AXL_TEST_FLUSHFAIL_FS_H
#define AXL_TEST_FLUSHFAIL_FS_H

#include <axl.h>
#include <axl/axl-fs-provider.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Shell map name the fixture publishes itself under.
#define FF_MAP  "AXLFF"

/// Build a path on the fixture volume: FF_PATH("t") -> "AXLFF:\t".
#define FF_PATH(name)  FF_MAP ":\\" name

/// A destination name with this prefix has its RENAME refused, which is
/// what forces axl_file_move onto its copy-and-delete fallback (the
/// same-directory rename path would otherwise never reach a stream).
#define FF_NORENAME_PREFIX  "nr-"

/* Sized for unit tests by default. A consumer that drives a TOOL through
   the fixture needs more (`tar -c` pads its archive to a 10240-byte
   record boundary, so a 1 KiB file would fail at WRITE and never reach
   the flush) -- define either before including. */
#ifndef FF_MAX_FILES
#define FF_MAX_FILES  8u
#endif
#ifndef FF_MAX_BYTES
#define FF_MAX_BYTES  1024u
#endif

typedef struct {
    bool    used;
    char    name[64];              /* basename; flat namespace, no subdirs */
    uint8_t data[FF_MAX_BYTES];
    size_t  len;
} FfSlot;

static FfSlot ff_slots[FF_MAX_FILES];
static void  *ff_handle;
static bool   ff_mapped;
static bool   ff_flush_ok;   /* false = the fixture's whole point; see ff_flush */

/// Let the flush SUCCEED. The fixture then models a healthy volume that
/// refuses only what `set_info` refuses (see FF_NORENAME_PREFIX) -- which is
/// how a promote-site's recovery policy is reached: the temp write lands,
/// and only the rename over the target fails. Reset to false by ff_fs_up.
static inline void
ff_set_flush_ok(bool ok)
{
    ff_flush_ok = ok;
}

// ---------------------------------------------------------------------------
// Backing store (the oracle side -- no provider callbacks involved)
// ---------------------------------------------------------------------------

static inline FfSlot *
ff_find(const char *name)
{
    for (size_t i = 0; i < FF_MAX_FILES; i++) {
        if (ff_slots[i].used && axl_strcmp(ff_slots[i].name, name) == 0) {
            return &ff_slots[i];
        }
    }
    return NULL;
}

static inline FfSlot *
ff_alloc(const char *name)
{
    for (size_t i = 0; i < FF_MAX_FILES; i++) {
        if (!ff_slots[i].used) {
            if (axl_strlcpy(ff_slots[i].name, name, sizeof(ff_slots[i].name))
                    >= sizeof(ff_slots[i].name)) {
                return NULL;
            }
            ff_slots[i].used = true;
            ff_slots[i].len  = 0;
            return &ff_slots[i];
        }
    }
    return NULL;
}

/// Put @p len bytes under @p name without going through any AXL file
/// API -- the "what the volume held before" side of a durability test.
static inline bool
ff_seed(const char *name, const void *data, size_t len)
{
    if (len > FF_MAX_BYTES) {
        return false;
    }
    FfSlot *s = ff_find(name);
    if (s == NULL) {
        s = ff_alloc(name);
    }
    if (s == NULL) {
        return false;
    }
    axl_memcpy(s->data, data, len);
    s->len = len;
    return true;
}

/// True when @p name holds exactly @p len bytes equal to @p data.
static inline bool
ff_content_is(const char *name, const void *data, size_t len)
{
    const FfSlot *s = ff_find(name);
    return s != NULL && s->len == len && axl_memcmp(s->data, data, len) == 0;
}

static inline bool
ff_exists(const char *name)
{
    return ff_find(name) != NULL;
}

/// Bytes currently stored under @p name, or SIZE_MAX if it does not exist.
/// Distinguishes "the writes were refused" from "the writes were accepted
/// and only the flush refused" -- the whole point of the fixture.
static inline size_t
ff_size(const char *name)
{
    const FfSlot *s = ff_find(name);
    return (s != NULL) ? s->len : (size_t)-1;
}

// ---------------------------------------------------------------------------
// Provider vtable
// ---------------------------------------------------------------------------

struct AxlFsProviderFile {
    FfSlot *slot;          /* NULL for the root directory */
    bool    is_dir;
    size_t  cursor;        /* file byte cursor */
    size_t  dir_index;     /* root iteration cursor */
};

/// Borrowed pointer to the basename inside @p path (absolute, '/'-separated,
/// as the thunk hands it over). "" and "/" are the root.
static inline const char *
ff_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base;
}

static inline AxlFsStatus
ff_open(
    void               *backend_ctx,
    const char         *path,
    unsigned            mode,
    unsigned            attributes,
    AxlFsProviderFile **out,
    bool               *out_is_dir
    )
{
    (void)backend_ctx;
    if (path == NULL || out == NULL || out_is_dir == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if ((mode & AXL_FS_OPEN_CREATE) != 0
        && (attributes & AXL_FS_ATTR_DIRECTORY) != 0) {
        return AXL_FS_ERR_UNSUPPORTED;   /* flat namespace, no mkdir */
    }

    const char *base = ff_basename(path);
    FfSlot     *slot = NULL;
    bool        root = (*base == '\0');

    if (!root) {
        slot = ff_find(base);
        if (slot == NULL) {
            if ((mode & AXL_FS_OPEN_CREATE) == 0) {
                return AXL_FS_ERR_NOT_FOUND;
            }
            slot = ff_alloc(base);
            if (slot == NULL) {
                return AXL_FS_ERR_NO_SPACE;
            }
        }
    }

    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->slot     = slot;
    f->is_dir   = root;
    *out        = f;
    *out_is_dir = root;
    return AXL_FS_OK;
}

static inline AxlFsStatus
ff_close(AxlFsProviderFile *file)
{
    if (file == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    axl_free(file);
    return AXL_FS_OK;
}

static inline AxlFsStatus
ff_read(AxlFsProviderFile *file, void *buf, size_t *inout_size)
{
    if (file == NULL || buf == NULL || inout_size == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->slot == NULL) {
        return AXL_FS_ERR_IS_DIR;
    }
    size_t avail = (file->cursor < file->slot->len)
                 ? (file->slot->len - file->cursor) : 0u;
    size_t n     = (*inout_size < avail) ? *inout_size : avail;
    axl_memcpy(buf, file->slot->data + file->cursor, n);
    file->cursor += n;
    *inout_size   = n;
    return AXL_FS_OK;
}

static inline AxlFsStatus
ff_read_dir(AxlFsProviderFile *file, AxlFsEntry *out, bool *out_end)
{
    if (file == NULL || out == NULL || out_end == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->slot != NULL) {
        return AXL_FS_ERR_NOT_DIR;
    }
    while (file->dir_index < FF_MAX_FILES
           && !ff_slots[file->dir_index].used) {
        file->dir_index++;
    }
    if (file->dir_index >= FF_MAX_FILES) {
        *out_end = true;
        return AXL_FS_OK;
    }
    const FfSlot *s = &ff_slots[file->dir_index];
    file->dir_index++;
    *out_end = false;

    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    axl_strlcpy(out->name, s->name, sizeof(out->name));
    out->size        = s->len;
    out->mtime_unix  = 0;
    out->attributes  = 0;
    return AXL_FS_OK;
}

static inline AxlFsStatus
ff_write(AxlFsProviderFile *file, const void *buf, size_t *inout_size)
{
    if (file == NULL || buf == NULL || inout_size == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->slot == NULL) {
        return AXL_FS_ERR_IS_DIR;
    }
    if (file->cursor + *inout_size > FF_MAX_BYTES) {
        return AXL_FS_ERR_NO_SPACE;
    }
    axl_memcpy(file->slot->data + file->cursor, buf, *inout_size);
    file->cursor += *inout_size;
    if (file->cursor > file->slot->len) {
        file->slot->len = file->cursor;
    }
    return AXL_FS_OK;
}

static inline AxlFsStatus
ff_seek(AxlFsProviderFile *file, uint64_t position)
{
    if (file == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->slot == NULL) {
        if (position != 0) {
            return AXL_FS_ERR_UNSUPPORTED;
        }
        file->dir_index = 0;
        return AXL_FS_OK;
    }
    file->cursor = (position == (uint64_t)-1)
                 ? file->slot->len : (size_t)position;
    return AXL_FS_OK;
}

static inline AxlFsStatus
ff_delete(AxlFsProviderFile *file)
{
    if (file == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->slot == NULL) {
        return AXL_FS_ERR_IS_DIR;
    }
    file->slot->used = false;
    file->slot->len  = 0;
    file->slot       = NULL;
    return AXL_FS_OK;
}

/* The whole point of the fixture: every byte was accepted, and the flush
   that says whether they reached the media refuses. AXL_FS_ERR_NO_SPACE
   is the full-volume case -- the most ordinary way real media does this. */
static inline AxlFsStatus
ff_flush(AxlFsProviderFile *file)
{
    (void)file;
    return ff_flush_ok ? AXL_FS_OK : AXL_FS_ERR_NO_SPACE;
}

static inline AxlFsStatus
ff_get_info(AxlFsProviderFile *file, AxlFsEntry *out)
{
    if (file == NULL || out == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->mtime_unix  = 0;
    if (file->slot == NULL) {
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
    } else {
        axl_strlcpy(out->name, file->slot->name, sizeof(out->name));
        out->size       = file->slot->len;
        out->attributes = 0;
    }
    return AXL_FS_OK;
}

/* Two jobs, told apart the way the thunk presents them (see
   AxlFsProviderSetInfo): a name that differs from the file's own is a
   RENAME, anything else is a size / attribute change. Renames onto a
   FF_NORENAME_PREFIX name are refused so axl_file_move's copy fallback
   is reachable; every other rename succeeds, which is what lets the
   temp-file-then-promote paths (axl_file_write_atomic,
   axl_piece_tree_save) run all the way to their promote step. */
static inline AxlFsStatus
ff_set_info(AxlFsProviderFile *file, const AxlFsEntry *in)
{
    if (file == NULL || in == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->slot == NULL) {
        return AXL_FS_ERR_IS_DIR;
    }

    if (in->name[0] != '\0'
        && axl_strcmp(in->name, file->slot->name) != 0) {
        if (axl_str_has_prefix(in->name, FF_NORENAME_PREFIX)) {
            return AXL_FS_ERR_UNSUPPORTED;
        }
        /* Reject BEFORE touching anything: dropping the victim first and
           then failing the copy would destroy a file the caller was told
           the rename never happened to. Unreachable with today's test
           names, but a fixture that lies under an edge case is worse than
           no fixture. */
        if (axl_strlen(in->name) >= sizeof(file->slot->name)) {
            return AXL_FS_ERR_INVALID;
        }
        FfSlot *victim = ff_find(in->name);
        if (victim != NULL) {
            victim->used = false;      /* rename-over replaces */
        }
        axl_strlcpy(file->slot->name, in->name, sizeof(file->slot->name));
        return AXL_FS_OK;
    }

    if (in->size > FF_MAX_BYTES) {
        return AXL_FS_ERR_NO_SPACE;
    }
    size_t want = (size_t)in->size;
    if (want > file->slot->len) {
        axl_memset(file->slot->data + file->slot->len, 0,
                   want - file->slot->len);
    }
    file->slot->len = want;
    return AXL_FS_OK;
}

static const AxlFsProvider ff_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = ff_open,
    .close         = ff_close,
    .read          = ff_read,
    .read_dir      = ff_read_dir,
    .write         = ff_write,
    .seek          = ff_seek,
    .del           = ff_delete,
    .flush         = ff_flush,
    .get_info      = ff_get_info,
    .set_info      = ff_set_info,
    .default_label = "FlushFailFs",
};

static const AxlGuid ff_guid = AXL_GUID(
    0x12345678, 0xabcd, 0x4003,
    0x90, 0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcf);

// ---------------------------------------------------------------------------
// Publish / unpublish
// ---------------------------------------------------------------------------

/// Publish the fixture and give it the FF_MAP shell mapping. False means
/// there is no shell to map through, so the volume is unreachable by path
/// and the caller must SKIP (with balancers) rather than fail.
static inline bool
ff_fs_up(void)
{
    axl_memset(ff_slots, 0, sizeof(ff_slots));
    ff_handle   = NULL;
    ff_mapped   = false;
    ff_flush_ok = false;

    if (axl_fs_provider_publish(&ff_provider, &ff_guid, &ff_handle) != AXL_OK) {
        return false;
    }

    AxlVolume   vols[16];
    size_t      n  = 0;
    const void *dp = NULL;
    if (axl_volume_enumerate(vols, 16, &n) == AXL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (vols[i].handle == ff_handle) {
                dp = vols[i].device_path;
                break;
            }
        }
    }
    if (dp == NULL || axl_volume_set_map(dp, FF_MAP) != AXL_OK) {
        if (axl_fs_provider_unpublish(ff_handle) != AXL_OK) {
            axl_printf("NOTE: flushfail fixture unpublish failed\n");
        }
        ff_handle = NULL;
        return false;
    }
    ff_mapped = true;
    return true;
}

static inline void
ff_fs_down(void)
{
    if (ff_mapped) {
        if (axl_volume_unmap(FF_MAP) != AXL_OK) {
            axl_printf("NOTE: flushfail fixture map not removed\n");
        }
        ff_mapped = false;
    }
    if (ff_handle != NULL) {
        if (axl_fs_provider_unpublish(ff_handle) != AXL_OK) {
            axl_printf("NOTE: flushfail fixture unpublish failed\n");
        }
        ff_handle = NULL;
    }
}

#endif /* AXL_TEST_FLUSHFAIL_FS_H */
