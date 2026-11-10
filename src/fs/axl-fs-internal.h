/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fs-internal.h
    Internal to AxlFs and the file stream (src/stream/axl-stream-file.c).

    1. The handle routines behind the path forms and the stream forms of
       truncate, stat and set-mtime: each path form opens a handle, calls
       one of these and closes it, and each stream form calls it on the
       stream's own handle, so verification and flushing live in one place.

    2. Whole-file read with a status that tells "absent" from
       "unreadable".

    The public axl_file_get_contents() collapses every failure to AXL_ERR.
    A config loader cannot live with that: a caller that treats a missing
    file as "use the defaults" must not take that branch for a file that
    exists and could not be read. This is the same read, reporting which.

    3. The path and name helpers axl-fs.c and axl-fs-replace.c share.

    4. The replace transaction axl_file_replace() is built on
       (axl-fs-replace.c): axl_fs_replace_begin() decides every refusal and
       sets an existing destination aside, the caller moves or copies the
       source into place, and axl_fs_replace_end() removes the aside or puts
       it back. axl_fs_remove_tree() removes a directory with everything
       below it, without recursion.
**/

#ifndef AXL_FS_INTERNAL_H
#define AXL_FS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-fs.h>
#include "../backend/axl-backend.h"

/// Capacity for a volume root such as "fs0:\": a map name, ':', the
/// separator and the NUL. A longer map name is refused, never cut.
#define AXL_FS_ROOT_MAX  32u

/// Capacity, in UTF-8 bytes with the NUL, for a volume-relative path:
/// AXL_BACKEND_VOL_PATH_MAX UCS-2 units at up to 3 bytes each.
#define AXL_FS_REL_MAX  (AXL_BACKEND_VOL_PATH_MAX * 3u)

/// What reading an entry's name through its handle gave. A name the backend
/// had to CUT to fit is told apart from a failed read: it is a real name,
/// just not one to compare whole.
typedef enum {
    AXL_FS_NAME_WHOLE,   ///< the whole name
    AXL_FS_NAME_CUT,     ///< a name, cut to fit
    AXL_FS_NAME_FAILED   ///< no name: the query failed
} AxlFsNameRead;

/**
 * @brief Metadata through an open handle, `name` = current basename.
 *
 * @a entry is zeroed first, so it is defined (empty) on failure.
 *
 * @return AXL_OK, or AXL_ERR on a query the filesystem failed.
 */
AXL_WARN_UNUSED int
axl_fs_handle_stat(
    AxlFileHandle  handle,  ///< open handle
    AxlFsEntry    *entry    ///< [out] metadata
);

/**
 * @brief axl_file_info() with `name` filled: the basename the filesystem
 *     STORES for @p path, whatever spelling @p path used (on FAT, another
 *     letter case or an 8.3 alias). Empty for a volume root, and for a
 *     name too long for `entry->name`.
 *
 * @return AXL_OK; AXL_NOT_FOUND when the volume holds no such entry;
 *     AXL_ERR otherwise.
 */
AXL_WARN_UNUSED int
axl_fs_path_stat_named(
    const char  *path,   ///< path (UTF-8)
    AxlFsEntry  *entry   ///< [out] metadata, `name` = stored basename
);

/**
 * @brief Resize through an open handle: refuse a directory, set the size,
 *     re-read it and flush. The contract of axl_file_truncate().
 *
 * @return AXL_OK once verified and flushed, AXL_ERR otherwise.
 */
AXL_WARN_UNUSED int
axl_fs_handle_truncate(
    AxlFileHandle  handle,  ///< open handle, write access
    uint64_t       size     ///< new length in bytes
);

/**
 * @brief Set the modification time through an open handle, verify it,
 *     restore the previous time on a mismatch, flush. The contract of
 *     axl_file_set_mtime(); the caller has range-checked @p mtime_unix.
 *
 * @return AXL_OK once verified and flushed; AXL_DENIED for an entry with
 *     the read-only attribute; AXL_ERR otherwise.
 */
AXL_WARN_UNUSED int
axl_fs_handle_set_mtime(
    AxlFileHandle  handle,      ///< open handle, write access
    uint64_t       mtime_unix   ///< Unix seconds, UTC, in FAT's range
);

/**
 * @brief Read a whole file, saying why when it cannot.
 *
 * @return AXL_OK with @p buf (NUL-terminated, not counted in @p len; free
 *     with axl_free) and @p len set; AXL_NOT_FOUND when the volume resolved
 *     and the file system reported no such file; AXL_IO_ERROR for every
 *     other failure to open or read -- an unmapped volume, and, when
 *     @p refuse_dir is true, a directory at @p path; AXL_NO_RESOURCES on
 *     OOM; AXL_INVALID on a NULL argument. A read that returns fewer bytes
 *     than the file holds is AXL_IO_ERROR, and the bytes it did return are
 *     ZEROED before their buffer is freed, so a secret's file read in part
 *     leaves nothing of it in the pool.
 */
AXL_WARN_UNUSED AxlStatus
axl_file_get_contents_status(
    const char  *path,        ///< file path (UTF-8)
    bool         refuse_dir,  ///< a directory is AXL_IO_ERROR, not its bytes
    void       **buf,         ///< [out] contents
    size_t      *len          ///< [out] byte count
);

// ---------------------------------------------------------------------------
// Path and name helpers (axl-fs.c)
// ---------------------------------------------------------------------------

/**
 * @brief The basename of @p path: what follows its last '/' or '\'
 *     (the whole of a separator-free string).
 */
const char *
axl_fs_path_basename(
    const char  *path   ///< any path
);

/**
 * @brief A copy of @p path with every '/' turned into '\'.
 *
 * @return the copy (axl_free() it), or NULL on OOM.
 */
char *
axl_fs_path_backslashed(
    const char  *path   ///< any path
);

/**
 * @brief @p name joined onto the directory part of @p path
 *     ("fs0:\d\x" + "y" -> "fs0:\d\y").
 *
 * @return the path (axl_free() it), or NULL on OOM.
 */
char *
axl_fs_path_sibling(
    const char  *path,  ///< a path whose directory is wanted
    const char  *name   ///< the name to put there
);

/**
 * @brief Where @p path lands: its volume and its normalized
 *     volume-relative path ('\' separators, no leading or trailing one,
 *     "." and ".." resolved; "" is the root).
 *
 * @return AXL_OK, or AXL_ERR when it does not resolve or climbs above the
 *     volume root.
 */
int
axl_fs_path_locate(
    const char  *path,    ///< any path
    void       **volume,  ///< [out] the volume's SimpleFileSystem handle
    char        *rel,     ///< [out] volume-relative path (UTF-8)
    size_t       cap      ///< bytes at @p rel; AXL_FS_REL_MAX holds any
);

/**
 * @brief The root of the volume @p path names ("fs0:\x" -> "fs0:\"), the
 *     current directory's volume for a path with no map name.
 *
 * @return AXL_OK, or AXL_ERR for an empty path or a map name too long for
 *     @p cap.
 */
int
axl_fs_volume_root_of(
    const char  *path,  ///< any path
    char        *out,   ///< [out] the root, with its trailing '\'
    size_t       cap    ///< bytes at @p out; AXL_FS_ROOT_MAX holds any
);

/**
 * @brief The current name of the entry @p handle is open on, in UTF-8.
 *     Only AXL_FS_NAME_WHOLE is a name to compare: a cut one compares as
 *     a prefix. A name longer than the backend reads, or whose UTF-8 does
 *     not fit @p cap, is AXL_FS_NAME_CUT -- never a cut name reported WHOLE.
 */
AxlFsNameRead
axl_fs_handle_name(
    AxlFileHandle  handle,  ///< open handle
    char          *out,     ///< [out] the name ("" on failure)
    size_t         cap      ///< bytes at @p out; AXL_FS_NAME_MAX holds any FAT name
);

/**
 * @brief UTF-8 bytes, without the NUL, axl_ucs2_to_utf8_buf() writes for
 *     @p s: 1, 2 or 3 per unit. Lets a caller tell "fits" from "cut" before
 *     converting into a fixed buffer.
 */
size_t
axl_fs_ucs2_utf8_len(
    const unsigned short *s  ///< NUL-terminated UCS-2
);

/**
 * @brief Does the stored name @p stored mean a rename to @p want happened?
 *
 * Either exactly @p want, or @p want with the trailing dots and spaces FAT
 * drops from a long name. A stored name equal to @p before -- the entry's
 * name before the rename -- is no rename at all, unless that is what was
 * asked.
 */
bool
axl_fs_rename_landed(
    const char  *stored,  ///< the name read back through the handle
    const char  *want,    ///< the name asked for
    const char  *before   ///< the name before the rename
);

/**
 * @brief Can @p name be joined onto a directory path as ONE component?
 *
 * Not when it is empty, "." or "..", or holds '\\' or '/': joined,
 * `..\\x` is three components and the shell resolves the `..`, so a
 * walker that trusted the name would leave the directory it walks. A ':'
 * IS allowed -- only a path's FIRST colon names a volume, so one inside a
 * joined component is just a character (and an ordinary one in Linux
 * names a 9P server passes up). The single check axl_dir_read, the
 * provider seam and the replace engine's walkers all apply.
 */
bool
axl_fs_name_is_one_component(
    const char  *name   ///< a listed name (UTF-8)
);

/**
 * @brief How many 32-byte FAT directory entries a new entry named @p name
 *     takes, as the EDK2-derived FAT driver writes it (FatCheckIs8Dot3Name,
 *     FatSetCaseFlag): one for a valid 8.3 name whose name and extension
 *     are each of one letter case, otherwise one more per 13 UTF-16 units.
 *     Errs high when unsure.
 */
unsigned
axl_fs_name_entries(
    const char  *name   ///< the new entry's name (UTF-8)
);

/**
 * @brief The stored spelling of @p path -- each component read back as the
 *     filesystem stores it (its long name, in its stored case), '\'-joined
 *     and volume-relative: the identity axl_file_replace() decides by, so
 *     an 8.3 alias, another letter case (ASCII or not, as the volume folds
 *     it) and the stored name all give the same string. A last component
 *     that does not exist joins as spelled. "" is the volume root.
 *
 * @return AXL_OK with @p volume and @p out; AXL_NOT_FOUND when a directory
 *     on the way does not exist, AXL_NOT_DIR when one is a file (nothing
 *     can be at or below @p path then); AXL_ERR when it cannot be resolved
 *     (an entry that will not open, a name that cannot be read whole, no
 *     memory, @p out too small).
 */
AXL_WARN_UNUSED int
axl_fs_path_canonical(
    const char  *path,     ///< any path (UTF-8)
    void       **volume,   ///< [out] the volume's SimpleFileSystem handle
    char        *out,      ///< [out] the stored, volume-relative path
    size_t       cap       ///< bytes at @p out; AXL_FS_REL_MAX holds any
);

/**
 * @brief The directory room, in bytes, that a replace onto an entry named
 *     @p want appends: the new name, and -- when an entry stored as
 *     @p stored is there (NULL: nothing) -- its aside and the name an undo
 *     writes back. What axl_file_replace()'s own pre-flight counts, for a
 *     caller deciding ahead of one (a PUT choosing in place over a temp).
 */
uint64_t
axl_fs_replace_dir_bytes(
    const char  *want,     ///< the destination's last component as spelled
    const char  *stored    ///< the name the existing entry is stored under, or NULL
);

/**
 * @brief @p name made printable for a log line: '\\' and '"' escaped,
 *     control and non-ASCII bytes as \\xHH, and cut with "..." once the
 *     escaped form passes 48 characters (a 255-byte name must not make an
 *     unreadable line). A @p cap of 0 writes nothing.
 */
void
axl_fs_escape_name(
    const char  *name,  ///< UTF-8 name
    char        *out,   ///< [out] the escaped form
    size_t       cap    ///< bytes at @p out; 64 holds any
);

/**
 * @brief The key axl_fs_report_first() remembers a directory by: @p salt
 *     (a publication, or NULL) and the directory's path.
 */
uint32_t
axl_fs_report_key(
    const void  *salt,  ///< distinguishes sources, or NULL
    const char  *dir    ///< the directory's path
);

/**
 * @brief Is this the first report for @p key? Remembers it if so. A ring
 *     of the 16 most recent keys, shared by every listing that drops
 *     names: a directory pushed out of the ring may be reported once more,
 *     and a key colliding with a remembered one is not reported at warning
 *     level at all -- its lines go to debug.
 */
bool
axl_fs_report_first(
    uint32_t  key   ///< from axl_fs_report_key()
);

/// What axl_fs_volume_facts() reports about a volume.
typedef struct {
    bool      read_only;    ///< the volume is write-protected
    uint32_t  block_size;   ///< allocation unit in bytes (the FAT cluster); 0 = unknown
    uint64_t  free_bytes;   ///< free bytes; UINT64_MAX = unknown
} AxlFsVolumeFacts;

/**
 * @brief Read-only flag, allocation unit and free space of the volume
 *     @p path is on, from EFI_FILE_SYSTEM_INFO.
 *
 * The EDK2-derived FAT driver reports the cluster size as BlockSize
 * (FatPkg Info.c), and FreeSpace as its count of free clusters times that
 * size -- on FAT32 the FSInfo hint.
 *
 * @return AXL_OK with @p out filled; AXL_UNSUPPORTED when the volume has no
 *     filesystem information to give; AXL_ERR otherwise.
 */
AXL_WARN_UNUSED int
axl_fs_volume_facts(
    const char        *path,  ///< any path on the volume
    AxlFsVolumeFacts  *out    ///< [out] the facts
);

// ---------------------------------------------------------------------------
// The replace transaction (axl-fs-replace.c)
// ---------------------------------------------------------------------------

/// axl_fs_replace_begin(): the caller COPIES the source rather than moving
/// it, so the source stays and is opened read-only, a read-only source or
/// volume is no refusal, and both paths naming one entry -- or the
/// destination lying below a source directory -- is AXL_INVALID: a copy
/// onto itself or into itself. Never set by axl_file_replace().
#define AXL_FS_REPLACE_KEEP_SOURCE  0x100u

/// axl_fs_replace_begin(), with AXL_FS_REPLACE_KEEP_SOURCE: the caller
/// copies a source DIRECTORY itself only, not what is below it (a WebDAV
/// COPY with Depth: 0), so the space pre-flight counts that one directory
/// rather than the whole tree.
#define AXL_FS_REPLACE_SHALLOW      0x200u

/// axl_fs_replace_begin() / axl_fs_replace_guarded(): a destination that
/// exists and is a DIFFERENT entry from the source -- by the names the
/// filesystem stores, not by how the paths are spelled -- is not replaced:
/// AXL_EXISTS, nothing changed. The source itself under another spelling
/// (RESULT_SAME, RESULT_RECASED) is no such destination. A WebDAV MOVE or
/// COPY with `Overwrite: F`, or on a no-overwrite mount.
#define AXL_FS_REPLACE_NO_CLOBBER   0x400u

/// axl_fs_replace_begin() / axl_fs_replace_guarded(): a destination that
/// exists, is a DIFFERENT entry from the source and is a DIRECTORY is not
/// replaced: AXL_DENIED, nothing changed -- a replace must not remove a
/// tree its caller may not delete (a WebDAV mount refusing DELETE).
#define AXL_FS_REPLACE_NO_DIR_DEST  0x800u

/**
 * One replace in progress, between axl_fs_replace_begin() and
 * axl_fs_replace_end(). Filled by begin; the caller reads it and passes it
 * back to end.
 */
typedef struct {
    uint32_t              flags;           ///< as given to begin
    AxlFileReplaceResult  result;          ///< what the replace does; SAME and RECASED set nothing aside
    bool                  cross_volume;    ///< source and destination on different volumes
    bool                  src_is_dir;      ///< the source is a directory
    bool                  dst_existed;     ///< something was at the destination (now at @p aside)
    bool                  dst_was_dir;     ///< ... and it was a directory
    uint64_t              src_size;        ///< the source file's length (0 for a directory)
    uint64_t              src_mtime;       ///< the source's modification time, Unix seconds (0 = unknown)
    char                 *src;             ///< the source, as a normalized path the caller can open
    char                 *src_dir;         ///< the source's directory, likewise
    char                 *dst;             ///< the destination, likewise
    char                 *dst_dir;         ///< the destination's directory, likewise
    char                 *want;            ///< the destination's last component as spelled
    char                 *target;          ///< SetInfo name for a rename onto @p dst (bare, or '\'-anchored)
    char                 *src_name;        ///< the source's stored name
    char                 *aside;           ///< where the replaced entry is now; NULL when nothing was there
    char                 *aside_name;      ///< the replaced entry's stored name, to put it back under
    AxlFileHandle         src_h;           ///< the handle begin identified the source by
    AxlFileHandle         aside_h;         ///< handle on the replaced entry, NULL when none
    bool                  keep_aside;      ///< end() neither removes nor restores the aside
    bool                  held;            ///< the Ctrl-C hold is taken
    bool                  was_interrupted; ///< Ctrl-C was already pending when begin ran
} AxlFsReplaceTxn;

/**
 * @brief Steps 1 to 3 of axl_file_replace(): decide every refusal, set an
 *     existing destination aside, and check that the source is still there
 *     and the destination name free.
 *
 * Takes the Ctrl-C hold: from here until axl_fs_replace_end() an
 * interrupt is observed (axl_fs_replace_cancelled()) but does not end the
 * program. @p flags are AXL_FILE_REPLACE_* and AXL_FS_REPLACE_KEEP_SOURCE;
 * the caller has refused unknown public bits.
 *
 * @return AXL_OK with @p txn ready -- pass it to axl_fs_replace_end()
 *     whatever happens next; otherwise one of axl_file_replace()'s codes,
 *     with nothing changed and @p txn released.
 */
AXL_WARN_UNUSED int
axl_fs_replace_begin(
    const char       *old_path,  ///< entry to move or copy (UTF-8)
    const char       *new_path,  ///< bare name, or a path (UTF-8)
    uint32_t          flags,     ///< AXL_FILE_REPLACE_* | AXL_FS_REPLACE_KEEP_SOURCE
    AxlFsReplaceTxn  *txn        ///< [out] the transaction
);

/**
 * @brief Finish a transaction. With @p landed, step 5: remove the aside
 *     (with everything below it under AXL_FILE_REPLACE_TREE), logging a
 *     warning when it cannot. Without, put the aside back under its own
 *     name, logging an error naming both when it cannot. Either way close
 *     every handle, free the strings and release the Ctrl-C hold. A no-op
 *     on a released @p txn.
 */
void
axl_fs_replace_end(
    AxlFsReplaceTxn  *txn,     ///< from axl_fs_replace_begin()
    bool              landed   ///< the source is at the destination now
);

/**
 * @brief axl_file_replace() that also takes the internal policy flags
 *     AXL_FS_REPLACE_NO_CLOBBER and AXL_FS_REPLACE_NO_DIR_DEST, decided in
 *     step 1 by identity.
 *
 * @return as axl_file_replace(); AXL_INVALID for any other unknown bit.
 */
AXL_WARN_UNUSED int
axl_fs_replace_guarded(
    const char           *old_path,  ///< entry to move (UTF-8)
    const char           *new_path,  ///< bare name, or a path (UTF-8)
    uint32_t              flags,     ///< AXL_FILE_REPLACE_* | NO_CLOBBER | NO_DIR_DEST
    AxlFileReplaceResult *result     ///< [out] what happened; may be NULL
);

/**
 * @brief Has Ctrl-C been pressed since axl_fs_replace_begin()? Polls the
 *     break without exiting; the interrupt stays pending for the caller's
 *     next yield.
 */
bool
axl_fs_replace_cancelled(
    const AxlFsReplaceTxn  *txn  ///< the transaction in progress
);

/**
 * @brief Step 4 of a transaction begun with AXL_FS_REPLACE_KEEP_SOURCE:
 *     copy the source to the destination.
 *
 * A file is streamed; a directory is made alone under
 * AXL_FS_REPLACE_SHALLOW, and otherwise copied whole -- iteratively, so
 * the stack does not grow with the depth of the tree, with Ctrl-C checked
 * per entry and per chunk. Below the source, an entry whose name @p skip
 * accepts is not copied (NULL: none). A copy that fails or is cancelled
 * removes exactly what it created, newest first. Pass the result to
 * axl_fs_replace_end() as @p landed.
 *
 * @return AXL_OK; AXL_CANCELLED on Ctrl-C; AXL_ERR otherwise (also for a
 *     transaction not begun with AXL_FS_REPLACE_KEEP_SOURCE).
 */
AXL_WARN_UNUSED int
axl_fs_replace_copy(
    AxlFsReplaceTxn  *txn,                     ///< from axl_fs_replace_begin()
    bool            (*skip)(const char *name)  ///< entries not to copy, or NULL
);

/**
 * @brief Remove @p path: a file, or a directory with everything below it.
 *
 * Iterative, so its stack does not grow with the depth of the tree. Each
 * entry is removed through a handle whose stored name is first checked
 * against the listing. Stops at the first entry it cannot remove, leaving
 * the rest.
 *
 * @return AXL_OK once @p path is gone; AXL_NOT_FOUND if nothing was there;
 *     AXL_ERR otherwise.
 */
AXL_WARN_UNUSED int
axl_fs_remove_tree(
    const char  *path   ///< file or directory (UTF-8)
);

/// The points axl_fs_replace_test_hook is called at.
typedef enum {
    AXL_FS_REPLACE_STEP_ASIDE  = 1,  ///< end of step 3: the destination aside, the source checked
    AXL_FS_REPLACE_STEP_COPIED = 2,  ///< one more chunk of a copy written
    AXL_FS_REPLACE_STEP_MOVED  = 3,  ///< the source is at the destination; step 5 next
    AXL_FS_REPLACE_STEP_ENTRY  = 4,  ///< a tree copy is about to copy one more entry
    AXL_FS_REPLACE_STEP_UNDO   = 5   ///< end() is about to put the aside back
} AxlFsReplaceStep;

/// Test hook: when set, called at each AxlFsReplaceStep -- how a unit test
/// raises an interrupt mid-sequence. NULL in production.
extern void (*axl_fs_replace_test_hook)(AxlFsReplaceStep step);

/**
 * @brief Test hook: make the next aside name `~AXL<next>.TMP`, the one
 *     after `~AXL<next + 1>.TMP`, and so on (four uppercase hex digits).
 */
void
axl_fs_replace_test_seed(
    uint16_t  next   ///< hex value of the next aside name
);

/**
 * @brief Test hook: how many listings in a row that remove nothing
 *     axl_fs_remove_tree() makes before it stops. 0 restores the default.
 */
void
axl_fs_remove_tree_test_limit(
    unsigned  limit   ///< listings without a removal; 0 = default
);

/// Test hook: when true, the provider thunk hands every listed name up as
/// the provider gave it -- the seam's check for names that are not one
/// entry (a separator, empty, "." or "..") is skipped, so a unit test can
/// prove the directory walkers' own check behind it. False in production.
extern bool axl_fs_provider_test_unfiltered;

/// Test hook: when true, the provider thunk writes each listed name into
/// EFI_FILE_INFO WITHOUT its terminating NUL, sizing the info to the name
/// alone -- what a driver that omits it hands up. False in production.
extern bool axl_fs_provider_test_unterminated;

/**
 * @brief Flush an AxlFileWriter WITHOUT closing it.
 *
 * The same explicit flush axl_file_writer_close performs, so a caller can
 * learn the bytes are on the volume while it still holds the handle -- to
 * rename the file, then close it, or discard it through that handle.
 *
 * @return AXL_OK once flushed; AXL_ERR if the flush failed or a write
 *     already had.
 */
AXL_WARN_UNUSED int
axl_file_writer_flush(
    AxlFileWriter *w   ///< writer
);

/**
 * @brief Delete the writer's file THROUGH ITS OWN HANDLE, and free the
 *     writer.
 *
 * Never by path: whatever a path names by the time of the discard, this
 * removes only the file the writer created. NULL-safe.
 *
 * @return AXL_OK if the file was deleted; AXL_ERR otherwise (the writer is
 *     freed and its handle closed either way).
 */
int
axl_file_writer_discard(
    AxlFileWriter *w   ///< writer, or NULL
);

/**
 * @brief The name the writer's file has NOW, read through its own handle
 *     -- the name it was created under, or whatever a rename made it since.
 *
 * @return as axl_fs_handle_name(); AXL_FS_NAME_FAILED for a NULL writer.
 */
AxlFsNameRead
axl_file_writer_name(
    const AxlFileWriter *w,    ///< writer
    char                *out,  ///< [out] the name ("" on failure)
    size_t               cap   ///< bytes at @p out; AXL_FS_NAME_MAX holds any
);

#endif /* AXL_FS_INTERNAL_H */
