/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fs-replace.c
    axl_file_replace() -- rename(2) on filesystems that cannot rename onto
    an existing name -- and the transaction it is built on.

    The EDK2-derived FAT driver refuses a rename onto a taken name, so a
    replace is a sequence: decide every refusal, rename the destination
    ASIDE in its own directory, check nothing was two spellings of one
    entry, move (or copy) the source into place, and only then remove the
    aside. A failure before the last step puts the aside back.

    Identity is decided by STORED names: every component of both paths is
    opened and its name read back through the handle, so an 8.3 alias, a
    `.\`, the other separator or a case variant cannot make two entries look
    like one or one entry look like two.

    axl_fs_replace_begin() and axl_fs_replace_end() are the two halves a
    caller that copies (the WebDAV server's COPY) wraps its own work in;
    axl_file_replace() is one sequence over them.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include "../runtime/axl-signal-internal.h"
#include <axl/axl-fs.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "axl-file-gen.h"
#include "axl-fs-internal.h"
AXL_LOG_DOMAIN("fs");

// ---------------------------------------------------------------------------
// Internal types and macros
// ---------------------------------------------------------------------------

/* Bytes moved per read/write while copying. */
#define REPLACE_COPY_CHUNK  (64u * 1024u)

/* Asides tried before giving up: names taken by something else are skipped. */
#define REPLACE_ASIDE_TRIES  64u

/* Listings in a row that remove nothing before axl_fs_remove_tree gives
   up -- the backstop behind its check for a directory reported removed and
   still listed. Descending makes such listings, one per level, so this
   must exceed any depth a path can reach (asserted below). */
#define REPLACE_REMOVE_PASSES  1024u

/* The deepest tree a volume-relative path can name: one byte and a
   separator per level. */
_Static_assert(AXL_FS_REL_MAX / 2u < REPLACE_REMOVE_PASSES,
               "a tree deeper than REPLACE_REMOVE_PASSES could stop axl_fs_remove_tree");

/* UCS-2 units for a path built on the stack, not the AXL heap (the undo
   path). */
#define REPLACE_WIDE_MAX  (AXL_BACKEND_VOL_PATH_MAX + AXL_FS_ROOT_MAX)

/* A directory entry read into one EFI_FILE_INFO: the fixed part plus a
   name of AXL_FS_NAME_MAX units with its NUL -- any name whose UTF-8 can
   fit AXL_FS_NAME_MAX. A longer entry is a walk error, never a cut name. */
#define REPLACE_DIRENT_BUF  (80u + 2u * AXL_FS_NAME_MAX)

/* Offsets into EFI_FILE_INFO (UEFI 2.11 section 13.5.16), read here as
   axl_dir_read does, without the UEFI types. */
#define FI_FILE_SIZE_OFF   8u
#define FI_ATTRIBUTE_OFF   72u
#define FI_NAME_OFF        80u
#define FI_ATTR_DIRECTORY  0x10u

/* What resolving one path found: its volume, its normalized spelling, and
   the names the filesystem STORES for every component that exists. */
typedef struct {
    void     *volume;                 /* SimpleFileSystem handle */
    char      root[AXL_FS_ROOT_MAX];  /* "fs0:\" */
    char      rel[AXL_FS_REL_MAX];    /* normalized, as spelled; never "" */
    char      canon[AXL_FS_REL_MAX];  /* stored names, '\'-joined */
    size_t    parent_rel_len;         /* bytes of rel naming the parent */
    size_t    parent_canon_len;       /* bytes of canon naming the parent */
    bool      exists;                 /* the last component resolved */
    bool      is_dir;
    bool      read_only;              /* the entry's read-only attribute */
    uint64_t  size;
    uint64_t  mtime;
    char      stored[AXL_FS_NAME_MAX];  /* the last component's stored name */
} FsWhere;

/* What one directory read gave. */
typedef enum {
    DIRENT_ENTRY,
    DIRENT_END,
    DIRENT_ERROR
} DirentRead;

/* Is a path there? Three answers, because "could not tell" is neither. */
typedef enum {
    PATH_FOUND,
    PATH_ABSENT,
    PATH_UNKNOWN
} PathState;

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------

void (*axl_fs_replace_test_hook)(AxlFsReplaceStep step);

/* Next aside suffix, and whether it has been seeded. */
static uint16_t mAsideNext;
static bool     mAsideSeeded;

/* axl_fs_remove_tree's limit; 0 = REPLACE_REMOVE_PASSES. */
static unsigned mRemoveLimit;

// ---------------------------------------------------------------------------
// Function prototypes
// ---------------------------------------------------------------------------

static int remove_one(const char *path, const char *name);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void
axl_fs_replace_test_seed(
    uint16_t  next
    )
{
    mAsideNext   = next;
    mAsideSeeded = true;
}

void
axl_fs_remove_tree_test_limit(
    unsigned  limit
    )
{
    mRemoveLimit = limit;
}

static void
replace_step(
    AxlFsReplaceStep  step
    )
{
    if (axl_fs_replace_test_hook != NULL) {
        axl_fs_replace_test_hook(step);
    }
}

bool
axl_fs_replace_cancelled(
    const AxlFsReplaceTxn  *txn
    )
{
    _axl_poll_break();
    return !txn->was_interrupted && g_axl_interrupted;
}

/* @p a + @p b, allocated; NULL on OOM. */
static char *
str_join(
    const char  *a,
    const char  *b
    )
{
    size_t la  = axl_strlen(a);
    size_t lb  = axl_strlen(b);
    char  *out = axl_malloc(la + lb + 1);
    if (out != NULL) {
        axl_memcpy(out, a, la);
        axl_memcpy(out + la, b, lb + 1);
    }
    return out;
}

/* @p dir + '\' + @p name, allocated; a @p dir ending in '\' (a volume root)
   takes no second one. NULL on OOM. */
static char *
path_child(
    const char  *dir,
    const char  *name
    )
{
    size_t ld  = axl_strlen(dir);
    bool   sep = ld > 0 && dir[ld - 1] != '\\';
    size_t ln  = axl_strlen(name);
    char  *out = axl_malloc(ld + (sep ? 1u : 0u) + ln + 1);
    if (out != NULL) {
        axl_memcpy(out, dir, ld);
        if (sep) {
            out[ld] = '\\';
        }
        axl_memcpy(out + ld + (sep ? 1u : 0u), name, ln + 1);
    }
    return out;
}

/* The root plus the first @p n bytes of @p rel, allocated. */
static char *
where_path(
    const FsWhere  *w,
    size_t          n
    )
{
    size_t lr  = axl_strlen(w->root);
    char  *out = axl_malloc(lr + n + 1);
    if (out != NULL) {
        axl_memcpy(out, w->root, lr);
        axl_memcpy(out + lr, w->rel, n);
        out[lr + n] = '\0';
    }
    return out;
}

/* Open @p path; AXL_NOT_FOUND only for a volume that reported no such
   entry. */
static int
open_path(
    const char     *path,
    uint64_t        mode,
    AxlFileHandle  *h
    )
{
    unsigned short *wide = axl_utf8_to_ucs2(path);
    int             rc   = AXL_ERR;

    *h = NULL;
    if (wide != NULL) {
        rc = axl_backend_file_open(wide, mode, 0, h);
    }
    axl_free(wide);
    return rc;
}

/* Takes nothing from the AXL heap: it runs on the undo path, which must
   work when that has run out. (The firmware's own pool allocations behind
   the open remain; AXL cannot avoid those.) */
static PathState
path_state(
    const char  *path
    )
{
    unsigned short wide[REPLACE_WIDE_MAX];
    AxlFileHandle  h = NULL;

    if (axl_utf8_to_ucs2_buf(path, wide, REPLACE_WIDE_MAX) + 1u >= REPLACE_WIDE_MAX) {
        return PATH_UNKNOWN;   /* too long to have been cut safely */
    }
    int rc = axl_backend_file_open(wide, AXL_FILE_MODE_READ, 0, &h);
    if (rc == AXL_OK) {
        axl_backend_file_close(&h);
        return PATH_FOUND;
    }
    return (rc == AXL_NOT_FOUND) ? PATH_ABSENT : PATH_UNKNOWN;
}

/* Is the canonical path @p child, of length @p child_len, @p anc itself or
   below it? Exact: these are stored names. */
static bool
canon_within(
    const char  *child,
    size_t       child_len,
    const char  *anc
    )
{
    size_t n = axl_strlen(anc);
    return child_len >= n && axl_memcmp(child, anc, n) == 0
           && (child_len == n || child[n] == '\\');
}

/* @p name without the trailing dots and spaces FAT drops, into @p out. */
static void
name_stripped(
    const char  *name,
    char        *out,
    size_t       cap
    )
{
    size_t n = axl_strlen(name);
    while (n > 0 && (name[n - 1] == '.' || name[n - 1] == ' ')) {
        n--;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    axl_memcpy(out, name, n);
    out[n] = '\0';
}

/* Rename the entry @p h is open on to the bare name or '\'-anchored path
   @p target, and verify it reads back as @p want (the target's last
   component) through the same handle.

   @return AXL_OK verified; AXL_UNSUPPORTED when the filesystem cannot take
       the name (nothing changed); AXL_ERR otherwise -- the caller decides
       from the paths whether it moved after all. */
static int
handle_rename(
    AxlFileHandle  h,
    const char    *target,
    const char    *want,
    const char    *before
    )
{
    unsigned short wide[REPLACE_WIDE_MAX];
    char           stored[AXL_FS_NAME_MAX];
    int            rc = AXL_ERR;

    /* A stack buffer, not the AXL heap: this is also how an undo puts an
       entry back, and an undo must not fail because the AXL heap is
       exhausted. (The backend's SetInfo still uses firmware pool.) */
    if (axl_utf8_to_ucs2_buf(target, wide, REPLACE_WIDE_MAX) + 1u < REPLACE_WIDE_MAX) {
        rc = axl_backend_file_handle_rename(h, wide);
    }
    if (rc == AXL_UNSUPPORTED) {
        return AXL_UNSUPPORTED;
    }
    if (rc == AXL_OK
        && (axl_fs_handle_name(h, stored, sizeof(stored)) != AXL_FS_NAME_WHOLE
            || !axl_fs_rename_landed(stored, want, before))) {
        rc = AXL_ERR;
    }
    return (rc == AXL_OK) ? AXL_OK : AXL_ERR;
}

/* One entry of the directory @p h is open on, "." and ".." skipped. The
   name is read whole from EFI_FILE_INFO or not at all: one too long for
   the buffer or for @p cap is DIRENT_ERROR, never a cut name a walker
   would act on as another entry. A name that is not one component is
   DIRENT_ERROR too: a walker that joined it onto a path would leave the
   tree (the provider seam drops these too; this is the check behind it). */
static DirentRead
dirent_next(
    AxlFileHandle  h,
    char          *name,
    size_t         cap,
    bool          *is_dir,
    uint64_t      *size
    )
{
    uint8_t        buf[REPLACE_DIRENT_BUF];
    unsigned short wname[(REPLACE_DIRENT_BUF - FI_NAME_OFF) / 2u + 1u];

    for (;;) {
        size_t n = sizeof(buf);
        if (axl_backend_dir_read_entry(h, &n, buf) != AXL_OK) {
            return DIRENT_ERROR;
        }
        if (n == 0) {
            return DIRENT_END;
        }
        if (n <= FI_NAME_OFF) {
            return DIRENT_ERROR;
        }
        uint64_t attr;
        axl_memcpy(&attr, buf + FI_ATTRIBUTE_OFF, sizeof(attr));
        axl_memcpy(size, buf + FI_FILE_SIZE_OFF, sizeof(*size));
        *is_dir = (attr & FI_ATTR_DIRECTORY) != 0;
        /* The name, copied out so it is aligned and NUL-terminated whatever
           the entry claimed. */
        size_t units = (n - FI_NAME_OFF) / 2u;
        axl_memcpy(wname, buf + FI_NAME_OFF, units * 2u);
        wname[units] = 0;
        if (axl_fs_ucs2_utf8_len(wname) >= cap) {
            axl_debug("replace: a listed name too long to read whole; not walked");
            return DIRENT_ERROR;
        }
        axl_ucs2_to_utf8_buf(wname, name, cap);
        if (axl_strcmp(name, ".") == 0 || axl_strcmp(name, "..") == 0) {
            continue;
        }
        if (!axl_fs_name_is_one_component(name)) {
            axl_debug("replace: a listing gave the name \"%s\", which is not "
                      "one entry; not walked", name);
            return DIRENT_ERROR;
        }
        return DIRENT_ENTRY;
    }
}

/* Is the entry at @p path gone? ABSENT says so. A filesystem that answers a
   missing name with an error rather than NOT_FOUND (some providers) leaves
   it UNKNOWN; then the entry is gone when its parent's listing, read whole,
   no longer holds its name. A listing that cannot be read says nothing. */
static bool
entry_gone(
    const char  *path
    )
{
    PathState st = path_state(path);
    if (st != PATH_UNKNOWN) {
        return st == PATH_ABSENT;
    }
    const char *leaf = axl_fs_path_basename(path);
    size_t      lp   = (size_t)(leaf - path);
    char        parent[AXL_FS_ROOT_MAX + AXL_FS_REL_MAX];
    if (*leaf == '\0' || lp == 0 || lp >= sizeof(parent)) {
        return false;
    }
    axl_memcpy(parent, path, lp);
    parent[lp] = '\0';

    AxlFileHandle h = NULL;
    char          name[AXL_FS_NAME_MAX];
    bool          is_dir = false;
    uint64_t      size   = 0;
    DirentRead    got    = DIRENT_ERROR;
    if (open_path(parent, AXL_FILE_MODE_READ, &h) == AXL_OK) {
        while ((got = dirent_next(h, name, sizeof(name), &is_dir, &size)) == DIRENT_ENTRY
               && axl_strcmp(name, leaf) != 0) {
        }
    }
    axl_backend_file_close(&h);
    return got == DIRENT_END;
}

/* Does the directory at @p path hold anything? One that cannot be opened
   or listed counts as holding something: the answer decides whether it
   may be replaced. */
static bool
dir_holds_anything(
    const char  *path
    )
{
    AxlFileHandle h = NULL;
    char          name[AXL_FS_NAME_MAX];
    bool          is_dir = false;
    uint64_t      size   = 0;

    if (open_path(path, AXL_FILE_MODE_READ, &h) != AXL_OK) {
        return true;
    }
    DirentRead got = dirent_next(h, name, sizeof(name), &is_dir, &size);
    axl_backend_file_close(&h);
    return got != DIRENT_END;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

/* Resolve @p path component by component, reading each stored name through
   its own handle.

   @return AXL_OK (the last component may or may not exist); AXL_NOT_FOUND
       for a missing directory on the way; AXL_NOT_DIR for a non-directory
       on the way; AXL_ERR for a volume that does not resolve, the volume
       root, a name that cannot be read whole, or OOM. */
static int
where_resolve(
    const char  *path,
    FsWhere     *w
    )
{
    if (axl_fs_path_locate(path, &w->volume, w->rel, sizeof(w->rel)) != AXL_OK
        || axl_fs_volume_root_of(path, w->root, sizeof(w->root)) != AXL_OK
        || w->rel[0] == '\0') {
        return AXL_ERR;
    }

    size_t canon_len = 0;
    size_t start     = 0;
    w->canon[0] = '\0';
    w->exists   = false;

    for (;;) {
        size_t end = start;
        while (w->rel[end] != '\0' && w->rel[end] != '\\') {
            end++;
        }
        bool  last = w->rel[end] == '\0';
        char *p    = where_path(w, end);
        if (p == NULL) {
            return AXL_ERR;
        }
        AxlFileHandle h  = NULL;
        int           rc = open_path(p, AXL_FILE_MODE_READ, &h);
        axl_free(p);
        if (last) {
            w->parent_rel_len   = (start > 0) ? start - 1 : 0;
            w->parent_canon_len = canon_len;
        }
        if (rc == AXL_NOT_FOUND) {
            return last ? AXL_OK : AXL_NOT_FOUND;
        }
        if (rc != AXL_OK) {
            return AXL_ERR;
        }

        uint64_t       size = 0;
        uint64_t       mtime = 0;
        bool           is_dir = false;
        bool           ro = false;
        bool           cut = true;
        unsigned short wname[AXL_FS_NAME_MAX];
        rc = axl_backend_file_handle_stat(h, &size, NULL, &mtime, &is_dir, &ro,
                                          wname, AXL_FS_NAME_MAX, &cut);
        axl_backend_file_close(&h);
        char name[AXL_FS_NAME_MAX];
        if (rc != AXL_OK || cut || axl_fs_ucs2_utf8_len(wname) >= sizeof(name)) {
            return AXL_ERR;   /* a name not read whole is never compared */
        }
        axl_ucs2_to_utf8_buf(wname, name, sizeof(name));
        if (!last && !is_dir) {
            return AXL_NOT_DIR;
        }
        size_t ln = axl_strlen(name);
        if (canon_len + 1 + ln + 1 > sizeof(w->canon)) {
            return AXL_ERR;
        }
        if (canon_len > 0) {
            w->canon[canon_len++] = '\\';
        }
        axl_memcpy(w->canon + canon_len, name, ln + 1);
        canon_len += ln;

        if (last) {
            w->exists    = true;
            w->is_dir    = is_dir;
            w->read_only = ro;
            w->size      = size;
            w->mtime     = mtime;
            axl_strlcpy(w->stored, name, sizeof(w->stored));
            return AXL_OK;
        }
        start = end + 1;
    }
}

int
axl_fs_path_canonical(
    const char  *path,
    void       **volume,
    char        *out,
    size_t       cap
    )
{
    char rel[AXL_FS_REL_MAX];
    if (path == NULL || volume == NULL || out == NULL || cap == 0
        || axl_fs_path_locate(path, volume, rel, sizeof(rel)) != AXL_OK) {
        return AXL_ERR;
    }
    if (rel[0] == '\0') {
        out[0] = '\0';   /* the volume root */
        return AXL_OK;
    }
    FsWhere *w = axl_calloc(1, sizeof(*w));
    if (w == NULL) {
        return AXL_ERR;
    }
    int rc = where_resolve(path, w);
    if (rc == AXL_OK) {
        /* A last component that is not there joins as spelled. */
        size_t      cl   = axl_strlen(w->canon);
        const char *last = w->rel + ((w->parent_rel_len > 0) ? w->parent_rel_len + 1 : 0);
        int         n    = w->exists
                           ? axl_snprintf(out, cap, "%s", w->canon)
                           : axl_snprintf(out, cap, "%s%s%s", w->canon,
                                          (cl > 0) ? "\\" : "", last);
        if (n < 0 || (size_t)n >= cap) {
            rc = AXL_ERR;
        }
    }
    axl_free(w);
    return rc;
}

// ---------------------------------------------------------------------------
// The free-space pre-flight
// ---------------------------------------------------------------------------

/* Is @p c a character the FAT driver keeps in a short name as it is? The
   letters and digits, and the punctuation FAT short names allow. Anything
   else -- a space, '+', non-ASCII -- makes the driver write a long name;
   saying "no" when unsure only over-counts. */
static bool
short_name_char(
    char  c
    )
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return true;
    }
    for (const char *p = "$%'-_@~`!(){}^#&"; *p != '\0'; p++) {
        if (*p == c) {
            return true;
        }
    }
    return false;
}

/* Is the part [@p s, @p s + @p n) of one letter case (or none)? */
static bool
one_case(
    const char  *s,
    size_t       n
    )
{
    bool lower = false;
    bool upper = false;
    for (size_t i = 0; i < n; i++) {
        lower = lower || (s[i] >= 'a' && s[i] <= 'z');
        upper = upper || (s[i] >= 'A' && s[i] <= 'Z');
    }
    return !(lower && upper);
}

/* Does the FAT driver store @p name in ONE short entry? Mirrors EDK2's
   FatCheckIs8Dot3Name and FatSetCaseFlag: a main part of 1 to 8 and an
   extension of 0 to 3 short-name characters around at most one dot, no
   space or second dot, and each part of one letter case (a flag records
   which). */
static bool
fits_one_short_entry(
    const char  *name
    )
{
    size_t      len = axl_strlen(name);
    const char *dot = NULL;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '.') {
            if (dot != NULL) {
                return false;   /* a second dot */
            }
            dot = name + i;
        } else if (!short_name_char(name[i])) {
            return false;
        }
    }
    size_t main = (dot != NULL) ? (size_t)(dot - name) : len;
    size_t ext  = (dot != NULL) ? len - main - 1u : 0u;
    return main >= 1 && main <= 8 && ext <= 3
           && one_case(name, main) && (dot == NULL || one_case(dot + 1, ext));
}

/* Bytes of directory entries one entry named @p name takes: 32 for a name
   one short entry holds, otherwise a short entry plus a long-name entry
   per 13 UTF-16 units, 32 bytes each. */
static uint64_t
name_entry_bytes(
    const char  *name
    )
{
    if (fits_one_short_entry(name)) {
        return 32u;
    }
    unsigned short w[AXL_FS_NAME_MAX];
    size_t         units = axl_utf8_to_ucs2_buf(name, w, AXL_FS_NAME_MAX);
    return (1u + (units + 12u) / 13u) * 32u;
}

unsigned
axl_fs_name_entries(
    const char  *name
    )
{
    return (unsigned)(name_entry_bytes(name) / 32u);
}

uint64_t
axl_fs_replace_dir_bytes(
    const char  *want,
    const char  *stored
    )
{
    /* The new name (step 4), and when something is set aside, the aside
       (step 2) and the name an undo writes back. */
    uint64_t bytes = name_entry_bytes(want);
    if (stored != NULL) {
        bytes += name_entry_bytes("~AXL0000.TMP") + name_entry_bytes(stored);
    }
    return bytes;
}

/* @p bytes in whole clusters of @p cluster bytes. */
static uint64_t
whole_clusters(
    uint64_t  bytes,
    uint32_t  cluster
    )
{
    return (bytes + cluster - 1u) / cluster;
}

/* Clusters a copy of the tree at @p path takes: each directory once --
   its "." and ".." entries plus every child's entries, rounded up once --
   and each file's data rounded up. Walks without recursion. */
static int
tree_clusters(
    const char  *path,
    uint32_t     cluster,
    uint64_t    *out
    )
{
    char  **stack = NULL;
    size_t  depth = 0;
    size_t  cap   = 0;
    int     rc    = AXL_OK;

    *out = 0;
    char *first = axl_strdup(path);
    if (first == NULL) {
        return AXL_ERR;
    }
    stack = axl_malloc(sizeof(char *) * 8u);
    if (stack == NULL) {
        axl_free(first);
        return AXL_ERR;
    }
    cap = 8;
    stack[depth++] = first;

    while (rc == AXL_OK && depth > 0) {
        char         *dir = stack[--depth];
        AxlFileHandle h   = NULL;
        char          name[AXL_FS_NAME_MAX];
        bool          is_dir = false;
        uint64_t      size   = 0;
        uint64_t      dirent = 64u;   /* "." and ".." */
        if (open_path(dir, AXL_FILE_MODE_READ, &h) != AXL_OK) {
            rc = AXL_ERR;
        }
        DirentRead got = DIRENT_END;
        while (rc == AXL_OK
               && (got = dirent_next(h, name, sizeof(name), &is_dir, &size))
                      == DIRENT_ENTRY) {
            dirent += name_entry_bytes(name);
            if (!is_dir) {
                *out += whole_clusters(size, cluster);
                continue;
            }
            if (depth == cap) {
                char **grown = axl_realloc(stack, sizeof(char *) * cap * 2u);
                if (grown == NULL) {
                    rc = AXL_ERR;
                    break;
                }
                stack = grown;
                cap  *= 2u;
            }
            stack[depth] = path_child(dir, name);
            if (stack[depth] == NULL) {
                rc = AXL_ERR;
                break;
            }
            depth++;
        }
        if (got == DIRENT_ERROR) {
            rc = AXL_ERR;
        }
        *out += whole_clusters(dirent, cluster);
        axl_backend_file_close(&h);
        axl_free(dir);
    }
    while (depth > 0) {
        axl_free(stack[--depth]);
    }
    axl_free(stack);
    return rc;
}

/* AXL_NO_RESOURCES when @p d's volume reports less free space than the
   sequence may need there; AXL_OK when it reports enough or cannot say. */
static int
replace_preflight(
    const AxlFsReplaceTxn  *t,
    const FsWhere          *s,
    const FsWhere          *d,
    bool                    copies
    )
{
    AxlFsVolumeFacts vf;
    if (axl_fs_volume_facts(t->dst, &vf) != AXL_OK
        || vf.free_bytes == UINT64_MAX || vf.block_size == 0) {
        return AXL_OK;
    }
    /* Entries appended to the destination's directory, rounded up once:
       the new name (step 4), and when something is set aside, the aside
       (step 2) and the name an undo writes back. A case-only change sets
       the entry itself aside, so its own name is the one an undo writes. */
    uint32_t cl    = vf.block_size;
    uint64_t bytes = axl_fs_replace_dir_bytes(t->want, d->exists ? d->stored : NULL);
    uint64_t need = whole_clusters(bytes, cl);
    if (copies) {
        if (s->is_dir && (t->flags & AXL_FS_REPLACE_SHALLOW) != 0) {
            need += whole_clusters(64u, cl);   /* the directory alone: "." and ".." */
        } else if (s->is_dir) {
            uint64_t tree = 0;
            if (tree_clusters(t->src, cl, &tree) != AXL_OK) {
                return AXL_ERR;
            }
            need += tree;
        } else {
            need += whole_clusters(s->size, cl);
        }
    }
    return (need * cl > vf.free_bytes) ? AXL_NO_RESOURCES : AXL_OK;
}


// ---------------------------------------------------------------------------
// Aside
// ---------------------------------------------------------------------------

static uint16_t
aside_next(void)
{
    if (!mAsideSeeded) {
        mAsideNext   = (uint16_t)axl_backend_get_monotonic_us();
        mAsideSeeded = true;
    }
    return mAsideNext++;
}

/* Rename the entry @p h is open on (stored name @p name, at @p path) to a
   fresh `~AXLhhhh.TMP` in @p dir. A name something else holds is skipped.

   @return AXL_OK with @p aside set (the aside's path; caller frees);
       AXL_ERR with nothing changed, or, when the rename landed without
       verifying, after putting it back. */
static int
aside_take(
    AxlFileHandle   h,
    const char     *dir,
    const char     *path,
    const char     *name,
    char          **aside
    )
{
    *aside = NULL;
    for (unsigned tries = 0; tries < REPLACE_ASIDE_TRIES; tries++) {
        char leaf[16];
        axl_snprintf(leaf, sizeof(leaf), "~AXL%04X.TMP", (unsigned)aside_next());
        char *full = path_child(dir, leaf);
        if (full == NULL) {
            return AXL_ERR;
        }
        PathState st = path_state(full);
        if (st == PATH_FOUND) {
            axl_free(full);
            continue;
        }
        if (st == PATH_UNKNOWN) {
            axl_free(full);
            return AXL_ERR;
        }
        int rc = handle_rename(h, leaf, leaf, name);
        if (rc == AXL_OK) {
            axl_file_gen_bump(path);
            *aside = full;
            return AXL_OK;
        }
        /* A rename that landed and did not verify: the entry is at the
           aside name. Put it back before reporting the failure. */
        if (path_state(full) == PATH_FOUND && path_state(path) == PATH_ABSENT
            && handle_rename(h, name, name, leaf) != AXL_OK) {
            axl_error("replace: '%s' was renamed to '%s' unverified and could "
                      "not be renamed back; it is there", path, full);
        }
        axl_free(full);
        return AXL_ERR;
    }
    return AXL_ERR;
}

/* Put the aside back under @p name, its stored name. Whether it went back
   is decided by the ASIDE: the name read through its own handle, and the
   aside name no longer resolving. The destination being occupied proves
   nothing -- after a cleanup that failed, a partial copy is what occupies
   it. Takes nothing from the AXL heap. */
static void
aside_restore(
    AxlFileHandle  h,
    const char    *aside,
    const char    *name,
    const char    *where
    )
{
    char now[AXL_FS_NAME_MAX];

    if (handle_rename(h, name, name, axl_fs_path_basename(aside)) == AXL_OK) {
        return;
    }
    if (axl_fs_handle_name(h, now, sizeof(now)) == AXL_FS_NAME_WHOLE
        && axl_strcmp(now, name) == 0 && path_state(aside) == PATH_ABSENT) {
        return;   /* it went back; only the call's own status said otherwise */
    }
    axl_error("replace: '%s' could not be renamed back to '%s' and is still "
              "there", aside, where);
}

// ---------------------------------------------------------------------------
// Begin / end
// ---------------------------------------------------------------------------

/* Free everything @p t holds and release the hold. */
static void
txn_release(
    AxlFsReplaceTxn  *t
    )
{
    axl_backend_file_close(&t->src_h);
    axl_backend_file_close(&t->aside_h);
    axl_free(t->src);
    axl_free(t->src_dir);
    axl_free(t->dst);
    axl_free(t->dst_dir);
    axl_free(t->want);
    axl_free(t->target);
    axl_free(t->src_name);
    axl_free(t->aside);
    axl_free(t->aside_name);
    if (t->held) {
        _axl_poll_break_release();
    }
    axl_memset(t, 0, sizeof(*t));
}

/* Steps 1 to 3, with both paths already resolved into @p s and @p d. */
static int
begin_resolved(
    AxlFsReplaceTxn  *t,
    FsWhere          *s,
    FsWhere          *d,
    const char       *raw_last
    )
{
    uint32_t f    = t->flags;
    bool     keep = (f & AXL_FS_REPLACE_KEEP_SOURCE) != 0;
    bool     tree = (f & AXL_FILE_REPLACE_TREE) != 0;

    t->src     = where_path(s, axl_strlen(s->rel));
    t->src_dir = where_path(s, s->parent_rel_len);
    t->dst     = where_path(d, axl_strlen(d->rel));
    t->dst_dir = where_path(d, d->parent_rel_len);
    t->want    = axl_strdup(raw_last);
    t->src_name = axl_strdup(s->stored);
    if (t->src == NULL || t->src_dir == NULL || t->dst == NULL
        || t->dst_dir == NULL || t->want == NULL || t->src_name == NULL) {
        return AXL_ERR;
    }
    t->cross_volume = s->volume != d->volume;
    t->src_is_dir   = s->is_dir;
    t->src_size     = s->is_dir ? 0 : s->size;
    t->src_mtime    = s->mtime;

    /* One entry under two spellings: rename(2) of two links to one file. */
    if (!t->cross_volume && d->exists && axl_strcmp(s->canon, d->canon) == 0) {
        if (keep) {
            return AXL_INVALID;   /* a copy onto itself */
        }
        char want[AXL_FS_NAME_MAX];
        name_stripped(raw_last, want, sizeof(want));
        if (axl_strcmp(want, s->stored) == 0 || axl_strcasecmp(want, s->stored) != 0) {
            t->result = AXL_FILE_REPLACE_RESULT_SAME;
            return AXL_OK;
        }
        t->result = AXL_FILE_REPLACE_RESULT_RECASED;
        axl_free(t->want);
        t->want = axl_strdup(want);
        if (t->want == NULL) {
            return AXL_ERR;
        }
    } else {
        /* rename(2) answers EXDEV before any rule below (Linux
           do_renameat2 compares the mounts first). Only a COPY makes a move
           across volumes possible at all; with one, the policy and kind
           rules below apply as to any move. */
        if (t->cross_volume && !keep && (f & AXL_FILE_REPLACE_COPY) == 0) {
            return AXL_UNSUPPORTED;
        }
        /* The caller's policy for a destination that is ANOTHER entry,
           decided here, by identity: a spelling that differs only in case
           names another entry on a case-sensitive volume. */
        if (d->exists && (f & AXL_FS_REPLACE_NO_CLOBBER) != 0) {
            return AXL_EXISTS;
        }
        if (d->exists && d->is_dir && (f & AXL_FS_REPLACE_NO_DIR_DEST) != 0) {
            return AXL_DENIED;
        }
        /* rename(2)'s rules, in its order. */
        if (!t->cross_volume && s->is_dir
            && canon_within(d->canon, d->parent_canon_len, s->canon)) {
            return AXL_INVALID;   /* a directory into itself */
        }
        if (!t->cross_volume && d->exists && d->is_dir
            && canon_within(s->canon, axl_strlen(s->canon), d->canon)) {
            return tree ? AXL_INVALID : AXL_NOT_EMPTY;
        }
        if (d->exists && !tree) {
            if (s->is_dir && !d->is_dir) {
                return AXL_NOT_DIR;
            }
            if (!s->is_dir && d->is_dir) {
                return AXL_IS_DIR;
            }
            if (s->is_dir && dir_holds_anything(t->dst)) {
                return AXL_NOT_EMPTY;
            }
        }
        if (t->cross_volume && !keep && s->is_dir && !tree) {
            return AXL_UNSUPPORTED;   /* a directory is copied only with TREE */
        }
        t->result = d->exists ? AXL_FILE_REPLACE_RESULT_REPLACED
                              : AXL_FILE_REPLACE_RESULT_CREATED;
    }

    /* Read-only entries and write-protected volumes. */
    AxlFsVolumeFacts vf;
    if (axl_fs_volume_facts(t->dst, &vf) == AXL_OK && vf.read_only) {
        return AXL_DENIED;
    }
    if (!keep) {
        if (s->read_only
            || (t->cross_volume && axl_fs_volume_facts(t->src, &vf) == AXL_OK
                && vf.read_only)) {
            return AXL_DENIED;
        }
    }
    if (d->exists && d->read_only && t->result != AXL_FILE_REPLACE_RESULT_RECASED) {
        return AXL_DENIED;
    }

    int rc = replace_preflight(t, s, d, t->cross_volume || keep);
    if (rc != AXL_OK) {
        return rc;
    }

    /* The handle that identifies the source from here on. */
    uint64_t mode = keep ? AXL_FILE_MODE_READ
                         : (AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE);
    if (open_path(t->src, mode, &t->src_h) != AXL_OK) {
        return AXL_ERR;
    }
    char now[AXL_FS_NAME_MAX];
    if (axl_fs_handle_name(t->src_h, now, sizeof(now)) != AXL_FS_NAME_WHOLE
        || axl_strcmp(now, s->stored) != 0) {
        return AXL_ERR;   /* not the entry step 1 resolved */
    }

    if (axl_fs_replace_cancelled(t)) {
        return AXL_CANCELLED;
    }
    if (t->result == AXL_FILE_REPLACE_RESULT_RECASED) {
        return AXL_OK;   /* axl_file_replace renames it through its own aside */
    }

    /* Step 1 ends with the SetInfo name a rename onto the destination
       takes: a bare name within one directory, else a '\'-anchored path. */
    if (!t->cross_volume) {
        bool same_dir = s->parent_canon_len == d->parent_canon_len
                        && axl_memcmp(s->canon, d->canon, s->parent_canon_len) == 0;
        t->target = same_dir ? axl_strdup(raw_last) : str_join("\\", d->rel);
        if (t->target == NULL) {
            return AXL_ERR;
        }
    }

    /* Step 2: the destination aside. */
    if (d->exists) {
        t->dst_existed = true;
        t->dst_was_dir = d->is_dir;
        t->aside_name  = axl_strdup(d->stored);
        if (t->aside_name == NULL
            || open_path(t->dst, AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE,
                         &t->aside_h) != AXL_OK) {
            return AXL_ERR;
        }
        char got[AXL_FS_NAME_MAX];
        if (axl_fs_handle_name(t->aside_h, got, sizeof(got)) != AXL_FS_NAME_WHOLE
            || axl_strcmp(got, d->stored) != 0) {
            return AXL_ERR;
        }
        if (aside_take(t->aside_h, t->dst_dir, t->dst, d->stored, &t->aside)
                != AXL_OK) {
            return AXL_ERR;
        }
    }

    /* Step 3: two spellings of one entry, or a destination that held the
       source, show here -- the source went aside with the destination. */
    if (path_state(t->src) != PATH_FOUND || path_state(t->dst) != PATH_ABSENT) {
        return AXL_ERR;   /* the caller's release puts the aside back */
    }
    replace_step(AXL_FS_REPLACE_STEP_ASIDE);
    if (axl_fs_replace_cancelled(t)) {
        return AXL_CANCELLED;
    }
    return AXL_OK;
}

int
axl_fs_replace_begin(
    const char       *old_path,
    const char       *new_path,
    uint32_t          flags,
    AxlFsReplaceTxn  *txn
    )
{
    if (txn == NULL) {
        return AXL_ERR;
    }
    axl_memset(txn, 0, sizeof(*txn));
    if (old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }
    txn->flags           = flags;
    txn->was_interrupted = g_axl_interrupted;
    _axl_poll_break_hold();
    txn->held = true;

    char    *old_bs = axl_fs_path_backslashed(old_path);
    char    *new_bs = axl_fs_path_backslashed(new_path);
    char    *dest   = NULL;
    FsWhere *w      = axl_calloc(2, sizeof(FsWhere));
    int      rc     = AXL_ERR;

    if (old_bs != NULL && new_bs != NULL && w != NULL) {
        /* A bare new name is in the source's directory. */
        bool bare = true;
        for (const char *p = new_bs; *p != '\0'; p++) {
            if (*p == '\\' || *p == ':') {
                bare = false;
            }
        }
        dest = bare ? axl_fs_path_sibling(old_bs, new_bs) : axl_strdup(new_bs);
    }
    if (dest != NULL) {
        const char *last = axl_fs_path_basename(dest);
        if (*last == '\0' || axl_strcmp(last, ".") == 0
            || axl_strcmp(last, "..") == 0) {
            rc = AXL_ERR;   /* "fs0:\dir\", or no name at all */
        } else {
            rc = where_resolve(old_bs, &w[0]);
            if (rc == AXL_OK && !w[0].exists) {
                rc = AXL_NOT_FOUND;
            }
            if (rc == AXL_OK) {
                rc = where_resolve(dest, &w[1]);
            }
            if (rc == AXL_OK) {
                rc = begin_resolved(txn, &w[0], &w[1], last);
            }
        }
    }

    axl_free(w);
    axl_free(dest);
    axl_free(old_bs);
    axl_free(new_bs);
    if (rc != AXL_OK) {
        if (txn->aside != NULL) {
            aside_restore(txn->aside_h, txn->aside, txn->aside_name, txn->dst);
        }
        txn_release(txn);
    }
    return rc;
}

/* Bump the view generation of @p dir + @p name (either may be NULL). */
static void
bump_stored(
    const char  *dir,
    const char  *name
    )
{
    if (dir == NULL || name == NULL) {
        return;
    }
    char *p = path_child(dir, name);
    if (p != NULL) {
        axl_file_gen_bump(p);
    }
    axl_free(p);
}

void
axl_fs_replace_end(
    AxlFsReplaceTxn  *txn,
    bool              landed
    )
{
    if (txn == NULL || !txn->held) {
        return;
    }
    if (txn->aside != NULL && !txn->keep_aside) {
        if (!landed) {
            replace_step(AXL_FS_REPLACE_STEP_UNDO);
        }
        /* Put the aside back when the exact name it goes back to is free,
           or when the source is still where it was. Otherwise -- the source
           gone or unknown, and its name taken or unknown -- a step 4 that
           failed may have moved the source onto that name all the same,
           and renaming the aside back could overwrite it (a filesystem
           whose rename replaces, 9P, would): the aside stays where it is,
           and the error says so. The name is built on the stack. */
        /* Sized for the longest directory (a root and a volume-relative
           path) plus a separator and the longest stored name, so it cannot
           be cut. Were it cut anyway, the name would be some other
           entry's: it is then never "free". */
        char back[AXL_FS_ROOT_MAX + AXL_FS_REL_MAX + 1u + AXL_FS_NAME_MAX];
        bool free_name = false;
        if (!landed && txn->dst_dir != NULL && txn->aside_name != NULL) {
            size_t ld = axl_strlen(txn->dst_dir);
            int    n  = axl_snprintf(back, sizeof(back), "%s%s%s", txn->dst_dir,
                                     (ld > 0 && txn->dst_dir[ld - 1] != '\\') ? "\\" : "",
                                     txn->aside_name);
            free_name = n >= 0 && (size_t)n < sizeof(back)
                        && path_state(back) == PATH_ABSENT;
        }
        if (!landed && !free_name && path_state(txn->src) != PATH_FOUND) {
            axl_error("replace: '%s' cannot be found after a failed move to "
                      "'%s'; what it would replace is kept at '%s'",
                      txn->src, txn->dst, txn->aside);
        } else if (!landed) {
            aside_restore(txn->aside_h, txn->aside, txn->aside_name, txn->dst);
        } else {
            /* Step 5. Acts on the aside only: through its own handle, or,
               for a tree, by its aside path. */
            int rc;
            if (txn->dst_was_dir && (txn->flags & AXL_FILE_REPLACE_TREE) != 0) {
                axl_backend_file_close(&txn->aside_h);
                rc = axl_fs_remove_tree(txn->aside);
            } else {
                rc = axl_backend_file_handle_delete(&txn->aside_h);
            }
            if (rc != AXL_OK) {
                axl_warning("replace: '%s' could not be removed; it holds the "
                            "entry '%s' replaced", txn->aside, txn->dst);
            }
        }
    }
    if (txn->src != NULL && txn->result != AXL_FILE_REPLACE_RESULT_SAME) {
        /* Views are keyed by spelling: the paths as given, and the names the
           filesystem stores (a view opened under the long name, a replace
           spelled with the 8.3 alias). */
        axl_file_gen_bump(txn->src);
        axl_file_gen_bump(txn->dst);
        bump_stored(txn->src_dir, txn->src_name);
        bump_stored(txn->dst_dir, txn->aside_name);
        if (txn->src_is_dir || txn->dst_was_dir) {
            axl_file_gen_bump_all();
        }
    }
    txn_release(txn);
}

// ---------------------------------------------------------------------------
// Step 4: rename, or copy
// ---------------------------------------------------------------------------

/* Stream @p from (positioned at 0) into @p to; flush; set @p mtime where it
   can be stored.

   @return AXL_OK, AXL_CANCELLED, or AXL_ERR. */
static int
copy_handle(
    const AxlFsReplaceTxn  *t,
    AxlFileHandle           from,
    AxlFileHandle           to,
    uint64_t                expect,
    uint64_t                mtime
    )
{
    uint8_t *buf    = axl_malloc(REPLACE_COPY_CHUNK);
    uint64_t copied = 0;
    int      rc     = (buf != NULL) ? axl_backend_file_set_position(from, 0)
                                    : AXL_ERR;

    while (rc == AXL_OK) {
        size_t n = REPLACE_COPY_CHUNK;
        if (axl_backend_file_read(from, &n, buf) != AXL_OK) {
            rc = AXL_ERR;
            break;
        }
        if (n == 0) {
            break;
        }
        size_t w = n;
        if (axl_backend_file_write(to, &w, buf) != AXL_OK || w != n) {
            rc = AXL_ERR;
            break;
        }
        copied += n;
        replace_step(AXL_FS_REPLACE_STEP_COPIED);
        if (axl_fs_replace_cancelled(t)) {
            rc = AXL_CANCELLED;
        }
    }
    axl_free(buf);
    if (rc == AXL_OK && copied != expect) {
        rc = AXL_ERR;   /* the source changed length under the copy */
    }
    if (rc == AXL_OK && mtime >= AXL_FS_MTIME_MIN && mtime <= AXL_FS_MTIME_MAX
        && axl_backend_file_set_mtime(to, mtime) != AXL_OK) {
        axl_debug("replace: modification time not kept by the destination");
    }
    if (rc == AXL_OK && axl_backend_file_flush(to) != AXL_OK) {
        rc = AXL_ERR;
    }
    return rc;
}

/* Create @p dst and copy the file @p src_h is open on into it; a failure
   removes the copy through the handle that created it. On AXL_OK @p out
   holds the copy's handle, still open. */
static int
copy_file_to(
    const AxlFsReplaceTxn  *t,
    AxlFileHandle           src_h,
    const char             *dst,
    uint64_t                size,
    uint64_t                mtime,
    AxlFileHandle          *out
    )
{
    AxlFileHandle h = NULL;
    *out = NULL;
    if (open_path(dst, AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE
                       | AXL_FILE_MODE_CREATE, &h) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = copy_handle(t, src_h, h, size, mtime);
    if (rc != AXL_OK) {
        if (axl_backend_file_handle_delete(&h) != AXL_OK) {
            axl_error("replace: a partial copy at '%s' could not be removed", dst);
        }
        return rc;
    }
    *out = h;
    return AXL_OK;
}

/* Step 4 by copying a file, then deleting the source through its handle. */
static int
move_file_by_copy(
    AxlFsReplaceTxn  *t
    )
{
    AxlFileHandle copy = NULL;
    int rc = copy_file_to(t, t->src_h, t->dst, t->src_size, t->src_mtime, &copy);
    if (rc != AXL_OK) {
        return rc;
    }
    if (axl_fs_replace_cancelled(t)) {
        rc = AXL_CANCELLED;
    } else if (axl_backend_file_handle_delete(&t->src_h) == AXL_OK) {
        axl_backend_file_close(&copy);
        return AXL_OK;
    } else {
        /* The source is still there. Undo -- but only while it still holds
           every byte: the FAT driver empties a file before removing its
           entry, so a delete that failed part-way can leave it short, and
           then the copy is the only whole one. */
        AxlFsEntry e;
        if (path_state(t->src) == PATH_ABSENT) {
            /* The delete removed it after all; the move happened. */
            axl_backend_file_close(&copy);
            return AXL_OK;
        }
        if (axl_file_info(t->src, &e) == AXL_OK && e.size == t->src_size) {
            rc = AXL_ERR;
        } else {
            axl_backend_file_close(&copy);
            t->keep_aside = true;
            if (t->aside != NULL) {
                axl_error("replace: source '%s' emptied by a failed delete; its "
                          "copy stays at '%s', the replaced entry at '%s'",
                          t->src, t->dst, t->aside);
            } else {
                axl_error("replace: source '%s' emptied by a failed delete; its "
                          "copy stays at '%s'", t->src, t->dst);
            }
            return AXL_ERR;
        }
    }
    if (axl_backend_file_handle_delete(&copy) != AXL_OK) {
        axl_error("replace: the copy at '%s' could not be removed", t->dst);
    }
    return rc;
}

/* What copy_tree created, in creation order, so a copy that fails removes
   exactly that -- newest first -- and nothing that was there before or
   appeared beside it. Each record is the path under the name the
   filesystem STORED (FAT keeps "foo." as "foo"), read back after creation,
   so the undo's name check can match it. The record costs a pointer and a
   path per created entry: on the order of 10 MB for 100,000 entries. */
typedef struct {
    char   **paths;
    size_t   len;
    size_t   cap;
} Created;

/* Make room for one more record BEFORE an entry is created, so creating
   it can never leave an entry the record has no room for. */
static int
created_reserve(
    Created  *c
    )
{
    if (c->len < c->cap) {
        return AXL_OK;
    }
    size_t  ncap  = (c->cap == 0) ? 16u : c->cap * 2u;
    char  **grown = axl_realloc(c->paths, sizeof(char *) * ncap);
    if (grown == NULL) {
        return AXL_ERR;
    }
    c->paths = grown;
    c->cap   = ncap;
    return AXL_OK;
}

/* Record the entry just created at @p made_as in directory @p parent,
   under the name read back through @p *h -- the handle that created it,
   or, when @p *h is NULL, one opened on @p made_as now. Room was reserved.
   When the record cannot be made, the entry is removed again at once,
   through that same handle (which leaves @p *h NULL).

   @return AXL_OK recorded; AXL_ERR removed (or logged when it cannot be). */
static int
created_record(
    Created         *c,
    const char      *parent,
    const char      *made_as,
    AxlFileHandle   *h
    )
{
    AxlFileHandle  own = NULL;
    AxlFileHandle *use = h;
    char           stored[AXL_FS_NAME_MAX];
    char          *rec  = NULL;

    if (*h == NULL
        && open_path(made_as, AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE, &own) == AXL_OK) {
        use = &own;
    }
    if (*use != NULL
        && axl_fs_handle_name(*use, stored, sizeof(stored)) == AXL_FS_NAME_WHOLE) {
        rec = path_child(parent, stored);
    }
    if (rec != NULL && c->len < c->cap) {
        c->paths[c->len++] = rec;
        axl_backend_file_close(&own);
        return AXL_OK;
    }
    axl_free(rec);
    if (*use == NULL || axl_backend_file_handle_delete(use) != AXL_OK) {
        axl_error("replace: '%s' was created by a copy and could not be "
                  "removed", made_as);
    }
    axl_backend_file_close(&own);
    return AXL_ERR;
}

/* Remove what @p c records, newest first, each through its own handle;
   then free the list.

   @return AXL_OK when everything recorded is gone. */
static int
created_undo(
    Created  *c
    )
{
    int rc = AXL_OK;
    while (c->len > 0) {
        char *p = c->paths[--c->len];
        if (remove_one(p, axl_fs_path_basename(p)) != AXL_OK) {
            rc = AXL_ERR;
        }
        axl_free(p);
    }
    axl_free(c->paths);
    axl_memset(c, 0, sizeof(*c));
    return rc;
}

static void
created_free(
    Created  *c
    )
{
    while (c->len > 0) {
        axl_free(c->paths[--c->len]);
    }
    axl_free(c->paths);
    axl_memset(c, 0, sizeof(*c));
}

/* Make directory @p path, which must not exist yet: axl_dir_mkdir succeeds
   on a directory that is already there, and a copy must not adopt one. */
static int
mkdir_new(
    const char  *path
    )
{
    if (path_state(path) != PATH_ABSENT) {
        return AXL_ERR;
    }
    return axl_dir_mkdir(path);
}

/* Copy the directory tree at @p src to @p dst, which must not exist,
   recording in @p made every entry it creates. Iterative: a stack of
   directories still to list. Ctrl-C is checked per entry as well as per
   chunk of each file. An entry whose name @p skip accepts is not copied
   (NULL: none). */
static int
copy_tree(
    const AxlFsReplaceTxn  *t,
    const char             *src,
    const char             *dst,
    bool                  (*skip)(const char *name),
    Created                *made
    )
{
    char *dst_parent = axl_strdup(dst);
    if (dst_parent == NULL) {
        return AXL_ERR;
    }
    dst_parent[axl_fs_path_basename(dst_parent) - dst_parent] = '\0';
    int made_top = (created_reserve(made) == AXL_OK && mkdir_new(dst) == AXL_OK)
                   ? created_record(made, dst_parent, dst, &(AxlFileHandle){ NULL }) : AXL_ERR;
    axl_free(dst_parent);
    if (made_top != AXL_OK) {
        return AXL_ERR;
    }
    /* Pairs: stack[2i] the source directory, stack[2i + 1] its copy. */
    size_t  cap   = 16;
    size_t  depth = 0;
    char  **stack = axl_malloc(sizeof(char *) * cap);
    int     rc    = AXL_OK;
    if (stack == NULL) {
        return AXL_ERR;
    }
    stack[0] = axl_strdup(src);
    stack[1] = axl_strdup(dst);
    depth    = 2;
    if (stack[0] == NULL || stack[1] == NULL) {
        rc = AXL_ERR;
    }

    while (rc == AXL_OK && depth > 0) {
        char         *to   = stack[--depth];
        char         *from = stack[--depth];
        AxlFileHandle dh   = NULL;
        char          name[AXL_FS_NAME_MAX];
        bool          is_dir = false;
        uint64_t      size   = 0;
        DirentRead    got    = DIRENT_END;

        if (open_path(from, AXL_FILE_MODE_READ, &dh) != AXL_OK) {
            rc = AXL_ERR;
        }
        while (rc == AXL_OK
               && (got = dirent_next(dh, name, sizeof(name), &is_dir, &size))
                      == DIRENT_ENTRY) {
            replace_step(AXL_FS_REPLACE_STEP_ENTRY);
            if (axl_fs_replace_cancelled(t)) {
                rc = AXL_CANCELLED;
                break;
            }
            if (skip != NULL && skip(name)) {
                continue;
            }
            char *sp = path_child(from, name);
            char *dp = path_child(to, name);
            if (sp == NULL || dp == NULL) {
                axl_free(sp);
                axl_free(dp);
                rc = AXL_ERR;
                break;
            }
            if (is_dir) {
                if (created_reserve(made) != AXL_OK || mkdir_new(dp) != AXL_OK
                    || created_record(made, to, dp, &(AxlFileHandle){ NULL }) != AXL_OK) {
                    rc = AXL_ERR;
                } else if (depth + 2 > cap) {
                    char **grown = axl_realloc(stack, sizeof(char *) * cap * 2u);
                    if (grown == NULL) {
                        rc = AXL_ERR;
                    } else {
                        stack = grown;
                        cap  *= 2u;
                    }
                }
                if (rc == AXL_OK) {
                    stack[depth++] = sp;
                    stack[depth++] = dp;
                    continue;
                }
                axl_free(sp);
                axl_free(dp);
                break;
            }
            AxlFileHandle fh   = NULL;
            AxlFileHandle copy = NULL;
            uint64_t      mt   = 0;
            if (path_state(dp) != PATH_ABSENT || created_reserve(made) != AXL_OK
                || open_path(sp, AXL_FILE_MODE_READ, &fh) != AXL_OK
                || axl_backend_file_handle_stat(fh, &size, NULL, &mt, NULL, NULL,
                                                NULL, 0, NULL) != AXL_OK) {
                rc = AXL_ERR;
            } else {
                rc = copy_file_to(t, fh, dp, size, mt, &copy);
                if (rc == AXL_OK) {
                    rc = created_record(made, to, dp, &copy);
                }
                axl_backend_file_close(&copy);
            }
            axl_backend_file_close(&fh);
            axl_free(sp);
            axl_free(dp);
        }
        if (rc == AXL_OK && got == DIRENT_ERROR) {
            rc = AXL_ERR;
        }
        axl_backend_file_close(&dh);
        axl_free(from);
        axl_free(to);
    }
    while (depth > 0) {
        axl_free(stack[--depth]);
    }
    axl_free(stack);
    return rc;
}

/* Step 4 by copying a directory tree, then removing the source tree. */
static int
move_tree_by_copy(
    AxlFsReplaceTxn  *t
    )
{
    Created made = { 0 };
    int     rc   = copy_tree(t, t->src, t->dst, NULL, &made);
    if (rc == AXL_OK && axl_fs_replace_cancelled(t)) {
        rc = AXL_CANCELLED;
    }
    if (rc != AXL_OK) {
        /* Remove exactly what the copy created, nothing by a path walk. */
        if (created_undo(&made) != AXL_OK) {
            axl_error("replace: a partial copy at '%s' could not be removed", t->dst);
        }
        return rc;
    }
    created_free(&made);
    /* The move happened once the copy is whole; what is left of a source
       that cannot be removed in full is reported, not undone. */
    axl_backend_file_close(&t->src_h);
    if (axl_fs_remove_tree(t->src) != AXL_OK) {
        axl_warning("replace: '%s' was copied to '%s', but could not be "
                    "removed in full; the rest of it is still there",
                    t->src, t->dst);
    }
    return AXL_OK;
}


int
axl_fs_replace_copy(
    AxlFsReplaceTxn  *t,
    bool            (*skip)(const char *name)
    )
{
    if (t == NULL || (t->flags & AXL_FS_REPLACE_KEEP_SOURCE) == 0) {
        return AXL_ERR;
    }
    if (!t->src_is_dir) {
        AxlFileHandle copy = NULL;
        int rc = copy_file_to(t, t->src_h, t->dst, t->src_size, t->src_mtime, &copy);
        axl_backend_file_close(&copy);
        return rc;
    }
    if ((t->flags & AXL_FS_REPLACE_SHALLOW) != 0) {
        return mkdir_new(t->dst);
    }
    Created made = { 0 };
    int     rc   = copy_tree(t, t->src, t->dst, skip, &made);
    if (rc == AXL_OK && axl_fs_replace_cancelled(t)) {
        rc = AXL_CANCELLED;
    }
    if (rc != AXL_OK) {
        if (created_undo(&made) != AXL_OK) {
            axl_error("replace: a partial copy at '%s' could not be removed", t->dst);
        }
        return rc;
    }
    created_free(&made);
    return AXL_OK;
}

/* Step 4: rename the source onto the destination, or copy it there. */
static int
replace_move(
    AxlFsReplaceTxn  *t
    )
{
    if (!t->cross_volume) {
        int rc = handle_rename(t->src_h, t->target, t->want, t->src_name);
        /* The handle's leaf name is not proof: a rename to another directory
           that keeps the leaf reads back the same whether or not anything
           moved. Step 5 deletes the replaced entry, so the landing is checked
           where it matters -- the destination resolves, and after a move
           between directories the source no longer does. */
        if (rc == AXL_OK
            && (path_state(t->dst) != PATH_FOUND
                || (t->target[0] == '\\' && path_state(t->src) != PATH_ABSENT))) {
            rc = AXL_ERR;
        }
        if (rc == AXL_OK) {
            return AXL_OK;
        }
        if (rc != AXL_UNSUPPORTED) {
            /* A failed rename removes nothing: the source at the
               destination after all means it moved. */
            PathState at_src = path_state(t->src);
            PathState at_dst = path_state(t->dst);
            if (at_dst == PATH_FOUND && at_src == PATH_ABSENT) {
                return AXL_OK;
            }
            if (at_dst == PATH_ABSENT && at_src == PATH_ABSENT) {
                axl_error("replace: rename of '%s' to '%s' failed part-way: the "
                          "entry is gone under both names", t->src, t->dst);
            }
            return AXL_ERR;
        }
    }
    if ((t->flags & AXL_FILE_REPLACE_COPY) == 0) {
        return AXL_UNSUPPORTED;
    }
    if (t->src_is_dir) {
        if ((t->flags & AXL_FILE_REPLACE_TREE) == 0) {
            return AXL_UNSUPPORTED;
        }
        return move_tree_by_copy(t);
    }
    return move_file_by_copy(t);
}

/* A case-only change: the entry renamed to a fresh aside name of its own,
   then to the new spelling; back to its old name if that fails. */
static int
replace_recase(
    AxlFsReplaceTxn  *t
    )
{
    char *aside = NULL;
    if (aside_take(t->src_h, t->src_dir, t->src, t->src_name, &aside) != AXL_OK) {
        return AXL_ERR;
    }
    replace_step(AXL_FS_REPLACE_STEP_ASIDE);
    int rc = axl_fs_replace_cancelled(t)
             ? AXL_CANCELLED
             : handle_rename(t->src_h, t->want, t->want, axl_fs_path_basename(aside));
    if (rc != AXL_OK) {
        aside_restore(t->src_h, aside, t->src_name, t->src);
        rc = (rc == AXL_CANCELLED) ? AXL_CANCELLED : AXL_ERR;
    }
    axl_free(aside);
    return rc;
}

int
axl_file_replace(
    const char           *old_path,
    const char           *new_path,
    uint32_t              flags,
    AxlFileReplaceResult *result
    )
{
    /* The public bits only; everything else, NULL arguments included, is
       axl_fs_replace_guarded's to refuse. */
    if ((flags & ~(AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE)) != 0) {
        return AXL_INVALID;
    }
    return axl_fs_replace_guarded(old_path, new_path, flags, result);
}

int
axl_fs_replace_guarded(
    const char           *old_path,
    const char           *new_path,
    uint32_t              flags,
    AxlFileReplaceResult *result
    )
{
    if (old_path == NULL || new_path == NULL) {
        return AXL_ERR;
    }
    if ((flags & ~(AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE
                   | AXL_FS_REPLACE_NO_CLOBBER | AXL_FS_REPLACE_NO_DIR_DEST)) != 0) {
        return AXL_INVALID;
    }

    AxlFsReplaceTxn t;
    int rc = axl_fs_replace_begin(old_path, new_path, flags, &t);
    if (rc != AXL_OK) {
        return rc;
    }
    AxlFileReplaceResult what = t.result;
    if (what == AXL_FILE_REPLACE_RESULT_SAME) {
        axl_fs_replace_end(&t, true);
    } else if (what == AXL_FILE_REPLACE_RESULT_RECASED) {
        rc = replace_recase(&t);
        axl_fs_replace_end(&t, rc == AXL_OK);
    } else {
        rc = replace_move(&t);
        if (rc == AXL_OK) {
            replace_step(AXL_FS_REPLACE_STEP_MOVED);
        }
        axl_fs_replace_end(&t, rc == AXL_OK);
    }
    if (rc == AXL_OK && result != NULL) {
        *result = what;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Tree removal
// ---------------------------------------------------------------------------

/* Remove the entry at @p path through its own handle, after checking that
   the handle's stored name is @p name -- the name a listing gave -- so a
   name that did not come through whole cannot remove another entry. NULL
   @p name skips the check (the path the caller was given). */
static int
remove_one(
    const char  *path,
    const char  *name
    )
{
    AxlFileHandle h = NULL;
    char          stored[AXL_FS_NAME_MAX];
    if (open_path(path, AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE, &h) != AXL_OK) {
        return AXL_ERR;
    }
    if (name != NULL
        && (axl_fs_handle_name(h, stored, sizeof(stored)) != AXL_FS_NAME_WHOLE
            || axl_strcmp(stored, name) != 0)) {
        axl_backend_file_close(&h);
        return AXL_ERR;
    }
    return axl_backend_file_handle_delete(&h);
}

int
axl_fs_remove_tree(
    const char  *path
    )
{
    if (path == NULL) {
        return AXL_ERR;
    }
    /* Work from the absolute spelling, the volume root plus the normalized
       path: a child name may hold ':', which is only a character inside a
       joined path, but would name a volume at the front of a RELATIVE one. */
    char *given = axl_fs_path_backslashed(path);
    char *rel   = axl_malloc(AXL_FS_REL_MAX);
    char  root[AXL_FS_ROOT_MAX];
    void *vol   = NULL;
    char *top   = NULL;
    /* Never a volume root: that would empty the volume. */
    if (given != NULL && rel != NULL
        && axl_fs_path_locate(given, &vol, rel, AXL_FS_REL_MAX) == AXL_OK
        && rel[0] != '\0'
        && axl_fs_volume_root_of(given, root, sizeof(root)) == AXL_OK) {
        top = str_join(root, rel);
    }
    axl_free(given);
    axl_free(rel);
    if (top == NULL) {
        return AXL_ERR;
    }
    AxlFileHandle h  = NULL;
    int           rc = open_path(top, AXL_FILE_MODE_READ, &h);
    bool          is_dir = false;
    if (rc != AXL_OK || axl_backend_file_handle_is_dir(h, &is_dir) != AXL_OK) {
        axl_backend_file_close(&h);
        axl_free(top);
        return (rc == AXL_NOT_FOUND) ? AXL_NOT_FOUND : AXL_ERR;
    }
    axl_backend_file_close(&h);
    if (!is_dir) {
        rc = axl_file_delete(top);
        axl_free(top);
        return rc;
    }

    /* Walk down to a directory with no subdirectory left, empty it, remove
       it, and climb back to its parent -- which is listed again. Each pass
       removes something, descends, or stops -- unless a delete reports
       success and keeps the entry, which is caught as a directory listed
       again right after its removal, and behind that by a pass limit. */
    size_t   top_len = axl_strlen(top);
    size_t   cap     = AXL_FS_REL_MAX + AXL_FS_ROOT_MAX;
    char    *cur     = axl_malloc(cap);
    char    *gone    = axl_malloc(cap);
    unsigned passes  = 0;
    if (cur == NULL || gone == NULL) {
        axl_free(cur);
        axl_free(gone);
        axl_free(top);
        return AXL_ERR;
    }
    axl_strlcpy(cur, top, cap);
    gone[0] = '\0';
    rc = AXL_OK;
    while (rc == AXL_OK) {
        unsigned limit = (mRemoveLimit != 0) ? mRemoveLimit : REPLACE_REMOVE_PASSES;
        if (passes >= limit) {
            axl_warning("replace: removing '%s' removed nothing in %u listings in "
                        "a row; stopped", top, limit);
            rc = AXL_ERR;
            break;
        }
        passes++;   /* reset below by every removal */
        AxlFileHandle dh = NULL;
        char          name[AXL_FS_NAME_MAX];
        bool          sub_dir = false;
        uint64_t      size    = 0;
        bool          descend = false;
        DirentRead    got     = DIRENT_END;

        if (open_path(cur, AXL_FILE_MODE_READ, &dh) != AXL_OK) {
            rc = AXL_ERR;
            break;
        }
        while ((got = dirent_next(dh, name, sizeof(name), &sub_dir, &size))
                   == DIRENT_ENTRY) {
            char *child = path_child(cur, name);
            if (child == NULL
                || axl_strlen(child) + 1 > AXL_FS_REL_MAX + AXL_FS_ROOT_MAX) {
                axl_free(child);
                rc = AXL_ERR;
                break;
            }
            if (sub_dir) {
                /* Descend only into what the listing named: the handle's
                   stored name must be the listed one. And not into the
                   directory just removed -- its delete said success and it
                   is still here. */
                AxlFileHandle  sh = NULL;
                char           stored[AXL_FS_NAME_MAX];
                bool           same = open_path(child, AXL_FILE_MODE_READ, &sh) == AXL_OK
                                      && axl_fs_handle_name(sh, stored, sizeof(stored))
                                             == AXL_FS_NAME_WHOLE
                                      && axl_strcmp(stored, name) == 0;
                axl_backend_file_close(&sh);
                if (!same) {
                    rc = AXL_ERR;
                } else if (axl_strcmp(child, gone) == 0) {
                    axl_warning("replace: '%s' was reported removed and is still "
                                "there; stopped", child);
                    rc = AXL_ERR;
                } else {
                    axl_strlcpy(cur, child, cap);
                    descend = true;
                }
                axl_free(child);
                break;
            }
            rc = remove_one(child, name);
            /* Progress only when the entry is really gone: a delete that
               says success and keeps it must not reset the count. */
            if (rc == AXL_OK && entry_gone(child)) {
                passes = 0;
            }
            axl_free(child);
            if (rc != AXL_OK) {
                break;
            }
        }
        axl_backend_file_close(&dh);
        if (rc != AXL_OK || descend) {
            continue;
        }
        if (got == DIRENT_ERROR) {
            rc = AXL_ERR;
            break;
        }
        rc = remove_one(cur, (axl_strlen(cur) <= top_len)
                             ? NULL : axl_fs_path_basename(cur));
        if (rc != AXL_OK || axl_strlen(cur) <= top_len) {
            break;
        }
        if (entry_gone(cur)) {
            passes = 0;
        }
        axl_strlcpy(gone, cur, cap);
        cur[axl_fs_path_basename(cur) - cur - 1] = '\0';
    }
    /* AXL_OK only once the top is really gone: its delete may have said
       success and kept it. */
    if (rc == AXL_OK && !entry_gone(top)) {
        rc = AXL_ERR;
    }
    axl_file_gen_bump_all();
    axl_free(gone);
    axl_free(cur);
    axl_free(top);
    return rc;
}
