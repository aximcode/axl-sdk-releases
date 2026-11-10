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

/// A destination name with this prefix has its RENAME fail outright
/// (AXL_FS_ERR_IO -> EFI_DEVICE_ERROR, not "unsupported"), which is how a
/// test proves axl_file_move does NOT copy on an ordinary rename failure.
#define FF_RENAMEFAIL_PREFIX  "rf-"

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
    bool    is_dir;                /* a directory: ff_seed_dir */
    char    listed[64];            /* a directory's first listed name, not a slot (ff_seed_listing) */
    bool    listed_dir;            /* ... listed as a directory */
    char    listed2[64];           /* its second listed name, "" for none */
    bool    listed2_dir;
    bool    alternate;             /* every other open lists the two the other way round */
    unsigned opens;                /* opens of this directory, for alternate */
    char    name[64];              /* basename; flat namespace, no subdirs */
    uint8_t data[FF_MAX_BYTES];
    size_t  len;
} FfSlot;

static FfSlot ff_slots[FF_MAX_FILES];
static void  *ff_handle;
static bool   ff_mapped;
static bool   ff_flush_ok;   /* false = the fixture's whole point; see ff_flush */

/* Fault knobs for the file stream's position bookkeeping; both off by
   default and reset by ff_fs_up. A write that would carry the cursor past
   ff_write_limit writes up to it, moves the cursor there, and fails -- a
   short write that errors. While ff_seek_fail_armed, a seek to exactly
   ff_seek_fail_pos fails. */
static size_t   ff_write_limit;
static bool     ff_seek_fail_armed;
static uint64_t ff_seek_fail_pos;
/* While set, get_info on a file (not the root) fails: a length that
   cannot be read. */
static bool     ff_info_fail;
/* The replace tests' knobs, all off by default and reset by ff_fs_up.
   ff_fold: names are found case-INSENSITIVELY (as FAT finds them).
   ff_echo: get_info reports the name a handle was opened by (or last
     renamed to), not the stored one -- a provider that echoes the request.
   ff_del_fail: every delete is refused, nothing removed.
   ff_del_truncate: a delete empties the file, then fails (the EDK2-derived
     FAT driver's order, failing between its two writes).
   ff_readdir_fail: listing a directory slot fails.
   ff_vol_ro: volume_info reports a write-protected volume.
   ff_tmp_stuck: a file whose name ends ".tmp" cannot be renamed (device
     error) -- a temp-then-promote whose promote fails while every other
     rename, the target's included, works.
   ff_no_replace: a rename onto a name another slot holds is refused (as FAT
     refuses it) instead of replacing that slot.
   ff_echo_stale: get_info reports the name a handle was OPENED by, never
     the one a rename gave it -- a rename that lands but does not verify.
   ff_rename_fail_to: a rename TO exactly this name fails (device error).
   ff_rename_lie_to: the next rename TO exactly this name answers success
     and changes nothing, though the handle then reports the new name. Once:
     the knob clears itself, so an undo to the same name is real.
   ff_rename_late_to: a rename TO exactly this name happens, then answers a
     device error.
   ff_rename_fail_once: ff_rename_fail_to clears itself once it fires.
   ff_rename_vanish_to: the next rename TO exactly this name loses the
     entry -- gone under both names -- and answers a device error. Once.
   ff_open_fail: every open of a non-root path fails with a device error --
     a path that cannot be told present or absent.
   ff_missing_is_io: opening a name that is not there fails with a device
     error instead of NOT_FOUND (some providers answer so).
   ff_del_lies: a delete answers success and keeps the entry.
   ff_del_late: a delete removes the entry, then answers a device error.
   ff_blind_after_rename_to: a rename TO exactly this name happens and
     answers success, and from then on every open of a non-root path fails
     (ff_open_fail) -- the rename landed, and nothing can see it by path.
   ff_mute_after_rename: with ff_blind_after_rename_to, also every get_info
     on a file fails from then on (ff_info_fail) -- no name can be read. */
static bool     ff_fold;
static bool     ff_echo;
static bool     ff_del_fail;
static bool     ff_del_truncate;
static bool     ff_readdir_fail;
static bool     ff_vol_ro;
static bool     ff_tmp_stuck;
static bool     ff_no_replace;
static bool     ff_echo_stale;
static char     ff_rename_fail_to[64];
static char     ff_rename_lie_to[64];
static char     ff_rename_late_to[64];
static bool     ff_rename_fail_once;
static char     ff_rename_vanish_to[64];
static bool     ff_open_fail;
static bool     ff_missing_is_io;
static bool     ff_del_lies;
static bool     ff_del_late;
static char     ff_blind_after_rename_to[64];
static bool     ff_mute_after_rename;

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

/* The provider's own lookup: ff_find, or case-insensitive under ff_fold.
   The oracle side always uses the exact ff_find. */
static inline FfSlot *
ff_lookup(const char *name)
{
    for (size_t i = 0; i < FF_MAX_FILES; i++) {
        if (ff_slots[i].used
            && (ff_fold ? axl_strcasecmp(ff_slots[i].name, name)
                        : axl_strcmp(ff_slots[i].name, name)) == 0) {
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
            ff_slots[i].used   = true;
            ff_slots[i].is_dir = false;
            ff_slots[i].listed[0]  = '\0';
            ff_slots[i].listed_dir = false;
            ff_slots[i].listed2[0] = '\0';
            ff_slots[i].listed2_dir = false;
            ff_slots[i].alternate   = false;
            ff_slots[i].opens       = 0;
            ff_slots[i].len    = 0;
            return &ff_slots[i];
        }
    }
    return NULL;
}

/// Make an empty directory @p name in the fixture's root.
static inline bool
ff_seed_dir(const char *name)
{
    FfSlot *s = ff_find(name);
    if (s == NULL) {
        s = ff_alloc(name);
    }
    if (s == NULL) {
        return false;
    }
    s->is_dir = true;
    s->len    = 0;
    return true;
}

/// Make directory @p name, which lists one entry named @p listed (a
/// directory when @p as_dir) that is not a slot of its own -- what a
/// provider passing names through unchecked (a 9P mount) can produce.
static inline bool
ff_seed_listing(const char *name, const char *listed, bool as_dir)
{
    FfSlot *s = ff_find(name);
    if (!ff_seed_dir(name) || (s = ff_find(name)) == NULL) {
        return false;
    }
    axl_strlcpy(s->listed, listed, sizeof(s->listed));
    s->listed_dir = as_dir;
    return true;
}

/// Give directory @p name a second listed entry, @p listed (a directory
/// when @p as_dir), after the one ff_seed_listing gave it.
static inline bool
ff_seed_listing2(const char *name, const char *listed, bool as_dir)
{
    FfSlot *s = ff_find(name);
    if (s == NULL || !s->is_dir) {
        return false;
    }
    axl_strlcpy(s->listed2, listed, sizeof(s->listed2));
    s->listed2_dir = as_dir;
    return true;
}

/// True when the root holds a directory slot named exactly @p name.
static inline bool
ff_is_dir(const char *name)
{
    const FfSlot *s = ff_find(name);
    return s != NULL && s->is_dir;
}

/// Number of slots whose name begins with @p prefix.
static inline size_t
ff_count_prefix(const char *prefix)
{
    size_t n = 0;
    for (size_t i = 0; i < FF_MAX_FILES; i++) {
        if (ff_slots[i].used && axl_str_has_prefix(ff_slots[i].name, prefix)) {
            n++;
        }
    }
    return n;
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
    bool    lied;          /* a lying rename gave this handle its name */
    char    name[64];      /* the name this handle was opened by or renamed to (ff_echo) */
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

    if (ff_open_fail && !root) {
        return AXL_FS_ERR_IO;
    }

    if (!root) {
        slot = ff_lookup(base);
        if (slot == NULL) {
            if ((mode & AXL_FS_OPEN_CREATE) == 0) {
                return ff_missing_is_io ? AXL_FS_ERR_IO : AXL_FS_ERR_NOT_FOUND;
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
    if (slot != NULL) {
        slot->opens++;
    }
    f->slot     = slot;
    f->is_dir   = root || slot->is_dir;
    axl_strlcpy(f->name, base, sizeof(f->name));
    *out        = f;
    *out_is_dir = f->is_dir;
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
    if (file->slot == NULL || file->slot->is_dir) {
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
        if (!file->slot->is_dir) {
            return AXL_FS_ERR_NOT_DIR;
        }
        if (ff_readdir_fail) {
            return AXL_FS_ERR_IO;
        }
        size_t      at  = file->dir_index;
        if (file->slot->alternate && (file->slot->opens % 2u) == 0u && at < 2) {
            at = 1u - at;   /* the second open, the fourth, ...: reversed */
        }
        const char *ln  = (at == 0) ? file->slot->listed
                        : (at == 1) ? file->slot->listed2 : "";
        bool        ldr = (at == 0) ? file->slot->listed_dir
                                    : file->slot->listed2_dir;
        if (file->dir_index >= 2) {
            ln = "";
        }
        if (ln[0] != '\0') {
            file->dir_index++;
            *out_end = false;
            out->struct_size = sizeof(*out);
            out->version     = AXL_FS_ENTRY_VERSION;
            axl_strlcpy(out->name, ln, sizeof(out->name));
            out->size        = 0;
            out->mtime_unix  = 0;
            out->attributes  = ldr ? AXL_FS_ATTR_DIRECTORY : 0u;
            return AXL_FS_OK;
        }
        *out_end = true;   /* a directory slot is otherwise empty */
        return AXL_FS_OK;
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
    out->attributes  = s->is_dir ? AXL_FS_ATTR_DIRECTORY : 0u;
    return AXL_FS_OK;
}

static inline AxlFsStatus
ff_write(AxlFsProviderFile *file, const void *buf, size_t *inout_size)
{
    if (file == NULL || buf == NULL || inout_size == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    if (file->slot == NULL || file->slot->is_dir) {
        return AXL_FS_ERR_IS_DIR;
    }
    if (file->cursor + *inout_size > FF_MAX_BYTES) {
        return AXL_FS_ERR_NO_SPACE;
    }
    if (ff_write_limit != 0 && file->cursor + *inout_size > ff_write_limit) {
        size_t part = (file->cursor < ff_write_limit)
                    ? ff_write_limit - file->cursor : 0;
        axl_memcpy(file->slot->data + file->cursor, buf, part);
        file->cursor += part;
        if (file->cursor > file->slot->len) {
            file->slot->len = file->cursor;
        }
        *inout_size = part;
        return AXL_FS_ERR_IO;
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
    if (file->slot == NULL || file->slot->is_dir) {
        if (position != 0) {
            return AXL_FS_ERR_UNSUPPORTED;
        }
        file->dir_index = 0;
        return AXL_FS_OK;
    }
    if (ff_seek_fail_armed && position == ff_seek_fail_pos) {
        return AXL_FS_ERR_IO;
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
    if (ff_del_truncate) {
        file->slot->len = 0;
        return AXL_FS_ERR_IO;
    }
    if (ff_del_fail) {
        return AXL_FS_ERR_ACCESS_DENIED;
    }
    if (ff_del_lies) {
        return AXL_FS_OK;
    }
    file->slot->used = false;
    file->slot->len  = 0;
    file->slot       = NULL;
    return ff_del_late ? AXL_FS_ERR_IO : AXL_FS_OK;
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
    if (ff_info_fail && file->slot != NULL) {
        return AXL_FS_ERR_IO;
    }
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->mtime_unix  = 0;
    if (file->slot == NULL) {
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
    } else {
        axl_strlcpy(out->name,
                    (ff_echo || ff_echo_stale || file->lied) ? file->name
                                                             : file->slot->name,
                    sizeof(out->name));
        out->size       = file->slot->len;
        out->attributes = file->slot->is_dir ? AXL_FS_ATTR_DIRECTORY : 0u;
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
        if (axl_str_has_prefix(in->name, FF_RENAMEFAIL_PREFIX)) {
            return AXL_FS_ERR_IO;
        }
        if (ff_tmp_stuck && axl_str_has_suffix(file->slot->name, ".tmp")) {
            return AXL_FS_ERR_IO;
        }
        if (axl_strcmp(in->name, ff_rename_vanish_to) == 0) {
            ff_rename_vanish_to[0] = '\0';   /* once: an undo to the name is real */
            file->slot->used = false;
            file->slot->len  = 0;
            file->slot       = NULL;
            return AXL_FS_ERR_IO;
        }
        if (axl_strcmp(in->name, ff_rename_fail_to) == 0) {
            if (ff_rename_fail_once) {
                ff_rename_fail_to[0] = '\0';
            }
            return AXL_FS_ERR_IO;
        }
        if (axl_strcmp(in->name, ff_rename_lie_to) == 0) {
            ff_rename_lie_to[0] = '\0';
            axl_strlcpy(file->name, in->name, sizeof(file->name));
            file->lied = true;
            return AXL_FS_OK;
        }
        /* Reject BEFORE touching anything: dropping the victim first and
           then failing the copy would destroy a file the caller was told
           the rename never happened to. Unreachable with today's test
           names, but a fixture that lies under an edge case is worse than
           no fixture. */
        if (axl_strlen(in->name) >= sizeof(file->slot->name)) {
            return AXL_FS_ERR_INVALID;
        }
        FfSlot *victim = ff_lookup(in->name);
        if (victim != NULL && victim != file->slot) {
            if (ff_no_replace) {
                return AXL_FS_ERR_ACCESS_DENIED;
            }
            victim->used = false;      /* rename-over replaces */
        }
        axl_strlcpy(file->slot->name, in->name, sizeof(file->slot->name));
        if (!ff_echo_stale) {
            axl_strlcpy(file->name, in->name, sizeof(file->name));
            file->lied = false;
        }
        if (axl_strcmp(in->name, ff_rename_late_to) == 0) {
            return AXL_FS_ERR_IO;
        }
        if (axl_strcmp(in->name, ff_blind_after_rename_to) == 0) {
            ff_blind_after_rename_to[0] = '\0';
            ff_open_fail = true;
            ff_info_fail = ff_mute_after_rename;
        }
        return AXL_FS_OK;
    }

    if (file->slot->is_dir) {
        return AXL_FS_OK;              /* a directory has no length to set */
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

/* What the thunk would synthesize with no callback (unknown sizes, 512-byte
   blocks, the default label), plus ff_vol_ro. */
static inline AxlFsStatus
ff_volume_info(void *backend_ctx, AxlFsProviderVolumeInfo *out)
{
    (void)backend_ctx;
    if (out == NULL) {
        return AXL_FS_ERR_INVALID;
    }
    out->read_only   = ff_vol_ro;
    out->volume_size = (uint64_t)-1;
    out->free_space  = (uint64_t)-1;
    out->block_size  = 512;
    axl_strlcpy(out->label, "FlushFailFs", sizeof(out->label));
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
    .volume_info   = ff_volume_info,
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
    ff_write_limit     = 0;
    ff_seek_fail_armed = false;
    ff_info_fail       = false;
    ff_fold            = false;
    ff_echo            = false;
    ff_del_fail        = false;
    ff_del_truncate    = false;
    ff_readdir_fail    = false;
    ff_vol_ro          = false;
    ff_tmp_stuck       = false;
    ff_no_replace      = false;
    ff_echo_stale      = false;
    ff_rename_fail_to[0] = '\0';
    ff_rename_lie_to[0]  = '\0';
    ff_rename_late_to[0] = '\0';
    ff_rename_fail_once = false;
    ff_rename_vanish_to[0] = '\0';
    ff_open_fail       = false;
    ff_missing_is_io   = false;
    ff_del_lies        = false;
    ff_del_late        = false;
    ff_blind_after_rename_to[0] = '\0';
    ff_mute_after_rename     = false;

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
