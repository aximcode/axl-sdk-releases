/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fs.h
 *
 * Filesystem operations — path-based file and directory APIs,
 * volume enumeration, file metadata. Mirrors the POSIX split:
 * `<axl/axl-stream.h>` is the `<stdio.h>` analog (FILE * / streams);
 * this header is the `<sys/stat.h>` + `<dirent.h>` + `<sys/statvfs.h>`
 * analog.
 *
 * All paths are UTF-8; backend converts to UCS-2 internally. A path is
 * limited to 511 characters relative to its volume (after `fsN:`), and by
 * the volume's own driver (FAT: 260 for the whole path); a longer one fails,
 * it is never cut short. See the module README, "Path length".
 * High-level convenience wrappers (`axl_file_get_contents`) layer on
 * top of axl_fopen — they're path-based shortcuts, not stream
 * primitives.
 *
 * Either separator works in every path this header takes, under the
 * modern shell, the EFI 1.x shell and with no shell: '/' is read as '\'.
 * No name can hold a '/' (FAT refuses it; axl_dir_read() never returns
 * one), so nothing that must keep one is lost.
 */

#ifndef AXL_FS_H
#define AXL_FS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-bytes.h>
#include <axl/axl-stream.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Whole-file convenience helpers (GLib-style)
// ---------------------------------------------------------------------------

/**
 * @brief Read entire file into memory. Like g_file_get_contents().
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
AXL_WARN_UNUSED int
axl_file_get_contents(
    const char *path,  ///< file path (UTF-8)
    void      **buf,   ///< (out): file contents (caller frees with axl_free)
    size_t     *len    ///< (out): file size in bytes
);

/**
 * @brief Read an entire file into an immutable AxlBytes.
 *
 * Like axl_file_get_contents but returns the contents as a
 * reference-counted AxlBytes — the shareable currency for passing
 * file data to parsers, hashers, or multiple readers without copying.
 * The read buffer is wrapped without an extra copy (axl_bytes_new_take).
 * An empty file yields a valid empty AxlBytes (size 0).
 *
 * @return a new AxlBytes (release with axl_bytes_unref), or NULL on
 *     read error or allocation failure.
 */
AxlBytes *
axl_file_get_bytes(
    const char *path  ///< file path (UTF-8)
);

/**
 * @brief Write entire buffer to file (creates or overwrites).
 *
 * Like g_file_set_contents().
 *
 * Flushes before returning, so `AXL_OK` means the bytes are on the
 * volume — not merely accepted by the firmware. That matters because
 * closing a file cannot report a failure at all
 * (`EFI_FILE_PROTOCOL.Close` is specified to return only
 * `EFI_SUCCESS`), so a full volume, write-protected media or device
 * error that surfaces when buffers are pushed out would otherwise be
 * invisible. A caller replacing a good file with this one can rely on
 * the status.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
AXL_WARN_UNUSED int
axl_file_set_contents(
    const char *path,  ///< file path (UTF-8)
    const void *buf,   ///< data to write
    size_t      len    ///< data size in bytes
);

/**
 * @brief Crash-safely write an entire buffer to a file.
 *
 * Writes @p buf to a temporary sibling ("<path>.tmp") and flushes it
 * through to the volume, then puts it in @p path's place with
 * axl_file_replace() (flags 0). The replace happens ONLY if that flush
 * succeeded: a temp whose bytes never landed is deleted and @p path is
 * left exactly as it was, rather than replaced by a file that may be
 * empty or short. Unlike axl_file_set_contents -- which truncates the
 * target and writes in place, so a power loss mid-write leaves the target
 * half-written -- this never modifies the target until the full, flushed
 * contents exist in the temp file.
 *
 * On UEFI/FAT a rename cannot replace an existing file, so when @p path
 * already exists axl_file_replace() first renames it aside to
 * `~AXLhhhh.TMP` in the same directory, then renames the temp into place,
 * then removes the aside. A failure puts the target back; a power loss in
 * between leaves the previous contents at the aside name and the new ones
 * complete at @p path or "<path>.tmp" -- never a half-written target, and
 * never neither.
 *
 * On failure the temp file is removed, and @p path is as it was -- except
 * in the cases axl_file_replace() lists under "What it cannot prevent":
 * when @p path existed before the call and no longer resolves after it,
 * "<path>.tmp" is kept, holding the complete new contents for recovery.
 *
 * @p path must be in a writable directory; the temp sibling is created
 * in the same directory, so the replace is a same-directory rename.
 *
 * @return #AXL_OK on success; #AXL_ERR for NULL @p path, NULL @p buf with
 *     a non-zero @p len, or a temp file that could not be written and
 *     flushed (the target untouched); otherwise what axl_file_replace()
 *     returned -- #AXL_IS_DIR for a directory at @p path, #AXL_DENIED for a
 *     read-only target or volume, #AXL_NO_RESOURCES, #AXL_CANCELLED, and
 *     the rest. Treat any negative value as failure.
 */
AXL_WARN_UNUSED int
axl_file_write_atomic(
    const char *path,  ///< target file path (UTF-8)
    const void *buf,   ///< data to write
    size_t      len    ///< data size in bytes
);

// ---------------------------------------------------------------------------
// AxlFileWriter — incremental (out-of-core) file writes
// ---------------------------------------------------------------------------

/**
 * @brief Streaming file writer — the write peer of AxlFileView.
 *
 * Writes a file incrementally without buffering the whole payload in
 * memory, the way `axl_file_view` reads out-of-core. This is what backs
 * a WebDAV PUT or any large upload that can't fit
 * `axl_file_set_contents`'s whole-buffer model (a BMC mounting a
 * multi-GB ISO, say). Each `axl_file_writer_write` goes straight to the
 * file; nothing is held in RAM between calls.
 *
 * Opaque handle; create with `axl_file_writer_open`, finalize with
 * `axl_file_writer_close` (which flushes). Not thread-safe (UEFI is
 * single-threaded for file I/O).
 */
typedef struct AxlFileWriter AxlFileWriter;

/// Append to an existing file instead of replacing it: writes go at the
/// current end of file rather than truncating it to empty first.
#define AXL_FILE_WRITER_APPEND  0x01u
/// Exclusive create: fail (return NULL) if @p path already exists.
/// Backs a PUT with `If-None-Match: *` (create-only). Ignored together
/// with APPEND would contradict; pass at most one of the two.
#define AXL_FILE_WRITER_EXCL    0x02u

/**
 * @brief Open a file for incremental writing.
 *
 * Creates @p path if it does not exist. By default (flags 0) an existing
 * file is truncated to empty first, so the writer replaces its contents
 * (WebDAV PUT semantics) with no stale tail when the new content is
 * shorter. With `AXL_FILE_WRITER_APPEND`, existing content is kept and
 * writes go at the current end of file. With `AXL_FILE_WRITER_EXCL`,
 * the open fails if @p path already exists.
 *
 * Not atomic: unlike `axl_file_write_atomic`, the target is written in
 * place (truncated up front), so an aborted stream or a power loss
 * mid-write leaves it partially written. For all-or-nothing, open the
 * writer on a temp path beside the target and replace the target with
 * it only once its bytes are known to be on the volume -- which
 * `axl_file_writer_close` returning AXL_OK says (it flushes) -- streaming
 * does not stand in the way of that. The WebDAV PUT of
 * `axl_fs_webdav_ops()` works this way, flushing with the handle still
 * open (an internal call) so it can still delete the temp through it.
 *
 * @return new writer, or NULL on open failure / OOM / EXCL-and-exists.
 *     Free with `axl_file_writer_close`.
 */
AxlFileWriter *
axl_file_writer_open(
    const char *path,   ///< file path (UTF-8)
    uint32_t    flags   ///< AXL_FILE_WRITER_* (0 = create / replace)
);

/**
 * @brief Append @p len bytes to the writer.
 *
 * Writes straight through to the file. A short write (fewer bytes
 * accepted by the firmware than requested) is reported as AXL_ERR and
 * puts the writer in a failed state: subsequent writes return AXL_ERR
 * without further I/O, and the file is left partially written — the
 * caller should stop and `axl_file_writer_close` it. A failed write may
 * already have advanced the file by some bytes.
 *
 * @return AXL_OK on success, AXL_ERR on write error, a prior failed
 *     state, or NULL args.
 */
AXL_WARN_UNUSED int
axl_file_writer_write(
    AxlFileWriter *w,    ///< writer
    const void    *buf,  ///< data to append
    size_t         len   ///< number of bytes (0 is a no-op success)
);

/**
 * @brief Bytes written through this writer so far.
 *
 * Total bytes accepted by successful `axl_file_writer_write` calls
 * (plus the pre-existing length when opened with APPEND). Lets a PUT
 * handler verify it received the advertised `Content-Length`.
 *
 * @return byte count, or 0 if @p w is NULL.
 */
uint64_t
axl_file_writer_tell(
    const AxlFileWriter *w   ///< writer
);

/**
 * @brief Flush and close the writer, releasing it. NULL-safe.
 *
 * Closing flushes outstanding data to the underlying volume through the
 * firmware's real flush primitive, then closes the handle. The flush is
 * explicit and its status is checked: a close alone cannot report a
 * failure, because EFI_FILE_PROTOCOL.Close is specified to return only
 * EFI_SUCCESS. The writer is freed regardless of the flush
 * result, so a PUT handler that needs durability MUST check this return
 * (a failed flush means report 5xx, not 201). There is deliberately no
 * AXL_AUTOPTR cleanup binding for AxlFileWriter: an implicit close would
 * discard this load-bearing flush status. C++ callers close explicitly.
 *
 * @return AXL_OK on success (or @p w == NULL), AXL_ERR if the final
 *     flush/close failed or the writer was already in a failed state.
 */
AXL_WARN_UNUSED int
axl_file_writer_close(
    AxlFileWriter *w   ///< writer (NULL-safe)
);

/**
 * @brief Check if a path refers to a directory.
 *
 * @return true if directory, false otherwise or on error.
 */
bool
axl_file_is_dir(
    const char *path  ///< file path (UTF-8)
);

// ---------------------------------------------------------------------------
// Progress callback
// ---------------------------------------------------------------------------

/**
 * @brief Progress callback for long-running I/O operations.
 *
 * @param done  bytes transferred so far
 * @param total total bytes (0 if unknown)
 * @param ctx   caller context pointer
 */
typedef void (*AxlProgressFunc)(uint64_t done, uint64_t total, void *ctx) AXL_CB_NOEXCEPT;

// ---------------------------------------------------------------------------
// Open-mode flags + attribute bitmask
// ---------------------------------------------------------------------------

/// Open-mode flags for `axl_fopen` (axl-stream) and the
/// `<axl/axl-fs-provider.h>` `open` callback.
///
/// READ (0x1) and WRITE (0x2) are bit-identical to the
/// corresponding `EFI_FILE_MODE_*` constants; CREATE is **renumbered**
/// (0x4 here vs `0x8000000000000000` for `EFI_FILE_MODE_CREATE`).
/// The SDK thunk at the AxlFsProvider boundary translates explicitly.
#define AXL_FS_OPEN_READ    0x1u   ///< open for reading
#define AXL_FS_OPEN_WRITE   0x2u   ///< open for writing (requires READ)
#define AXL_FS_OPEN_CREATE  0x4u   ///< create if missing (requires WRITE)

/// File / directory attribute bits — mirror EFI_FILE_* attribute
/// bits in axl shape. Used in `AxlFsEntry.attributes` and the
/// `attributes` parameter to the fs-provider open callback.
#define AXL_FS_ATTR_READ_ONLY   0x01u
#define AXL_FS_ATTR_HIDDEN      0x02u
#define AXL_FS_ATTR_SYSTEM      0x04u
#define AXL_FS_ATTR_DIRECTORY   0x10u
#define AXL_FS_ATTR_ARCHIVE     0x20u

// ---------------------------------------------------------------------------
// AxlFsEntry — file / directory metadata
// ---------------------------------------------------------------------------

/// Capacity of `AxlFsEntry.name`, NUL included: 765 UTF-8 bytes, which
/// holds any name FAT creates (255 UTF-16 units, each at most 3 bytes).
#define AXL_FS_NAME_MAX  766u

/// `AxlFsEntry.version` as this SDK writes it. 2: `name` widened from 256
/// to #AXL_FS_NAME_MAX bytes, moving every field after it.
///
/// `struct_size` and `version` record which layout filled an entry; they
/// are not a compatibility mechanism. AXL never reads them from a caller's
/// entry, and could not use them if it did. AXL links statically, so code
/// built against different versions meets only when objects built against
/// an older header are linked with a newer libaxl.a -- undefined; rebuild
/// everything. Whoever fills an entry (AXL, or a provider) sets both.
#define AXL_FS_ENTRY_VERSION  2

/**
 * @brief Canonical file / directory metadata.
 *
 * One struct, three uses:
 *  - `axl_file_info(path, &entry)` — path-based stat.
 *  - `axl_dir_read(dir, &entry)` — next directory entry; `name`
 *    populated with the basename only.
 *  - `<axl/axl-fs-provider.h>` callbacks `get_info`, `read_dir`,
 *    `set_info` — provider authors fill / read this same struct.
 *
 * Pre-Phase-C this was three different structs (`AxlFileInfo`,
 * `AxlDirEntry`, `AxlFsProviderInfo`) carrying the same data in
 * different shapes; collapsed in Phase C cleanup.
 *
 * 808 bytes. UEFI promises only 128 KiB of boot-services stack: keep
 * arrays of entries, and an entry per level of a recursive walk, on the
 * heap.
 */
typedef struct {
    uint32_t struct_size;     ///< sizeof(AxlFsEntry) at write time
    uint32_t version;         ///< AXL_FS_ENTRY_VERSION at write time
    char     name[AXL_FS_NAME_MAX]; ///< basename UTF-8, never cut; empty for path-stat / root
    uint64_t size;            ///< file size in bytes (0 for directories)
    uint64_t alloc_size;      ///< physical size on disk; 0 if unknown
    uint64_t mtime_unix;      ///< modification time, Unix epoch seconds (0 = unknown)
    uint32_t attributes;      ///< AXL_FS_ATTR_* bitmask
} AxlFsEntry;

/// Convenience: test the DIRECTORY attribute bit.
static inline bool
axl_fs_entry_is_dir(const AxlFsEntry *e)
{
    return e != NULL && (e->attributes & AXL_FS_ATTR_DIRECTORY) != 0u;
}

/// Convenience: test the READ_ONLY attribute bit.
static inline bool
axl_fs_entry_is_read_only(const AxlFsEntry *e)
{
    return e != NULL && (e->attributes & AXL_FS_ATTR_READ_ONLY) != 0u;
}

/**
 * @brief Get file metadata for a path. Wraps UEFI EFI_FILE_INFO.
 *
 * `entry->name` is left empty -- the caller has the path; axl_fstat()
 * reports a file's current name. The rest of the fields hold the
 * file's stat data.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_file_info(
    const char  *path,  ///< file path (UTF-8)
    AxlFsEntry  *entry  ///< [out] receives file metadata
);

/**
 * @brief Get metadata for the file under an open stream -- `fstat(2)`.
 *
 * axl_file_info() for a file you hold open. It describes the file @a s was
 * opened on even after that file was renamed or another file was created
 * at its old path, and, unlike axl_file_info(), it fills `entry->name`
 * with the file's CURRENT basename, which is how a caller learns where a
 * renamed file went. A name too long for `entry->name` (over 765 bytes of
 * UTF-8, which no FAT-created name is) leaves `entry->name` empty, never
 * cut: a cut name could be a sibling's.
 *
 * Read-only: nothing is written, so bytes still held in @a s's own write
 * buffer are not yet in the file and are not counted in `entry->size`.
 * Call axl_fflush() first if they must be.
 *
 * Works on any stream axl_fopen() returned, including one opened "r".
 * Other streams (buffer, console, custom, or a wrapper built over a file
 * stream) are #AXL_UNSUPPORTED. A file deleted through another handle is
 * #AXL_ERR: the FAT driver fails every query on a handle whose file is
 * gone.
 *
 * @a entry is zeroed on entry, so it is defined (empty) after any failure.
 *
 * @return #AXL_OK with @a entry filled; #AXL_UNSUPPORTED if @a s is not a
 *     file stream; #AXL_ERR on NULL arguments, a deleted file, or a device
 *     error. Treat any negative value as failure.
 */
AXL_WARN_UNUSED int
axl_fstat(
    AxlStream  *s,     ///< stream from axl_fopen()
    AxlFsEntry *entry  ///< [out] metadata, `name` = current basename
);

// ---------------------------------------------------------------------------
// File operations (path-based)
// ---------------------------------------------------------------------------

/**
 * @brief Delete a file.
 *
 * Deleting a path that does not exist is #AXL_NOT_FOUND, not success, as
 * axl_file_rename() answers for a missing source.
 *
 * It also does not CREATE the file on the way to answering. That is worth
 * stating because it used to: EDK2's `EfiShellDeleteFileByName` opens its
 * target with `EfiShellCreateFile`, so on the modern-shell path a delete of an
 * absent name produced a zero-length file, removed it, and honestly reported
 * success. A pure-cleanup call therefore performed a WRITE — which can fail on
 * a read-only or nearly full volume, or one whose media just went away. The
 * no-shell path never did this, so the behaviour also depended on whether a
 * modern shell was present; both now open without `CREATE`.
 *
 * #AXL_NOT_FOUND means the volume was reached and holds no such file, and
 * nothing else: a file that is there and cannot be removed -- one with the
 * read-only attribute, a read-only volume -- and a path whose volume does
 * not resolve (an unmapped `fs9:`) are both #AXL_ERR.
 *
 * @return #AXL_OK on success; #AXL_NOT_FOUND if the volume holds no file at
 *     @a path; #AXL_ERR otherwise, including NULL @a path, a file that
 *     could not be removed and a volume that does not resolve. As everywhere
 *     in AXL, treat ANY negative value as failure; the split is there for
 *     callers that map a status onto something else (an HTTP 404 versus a
 *     500).
 */
int
axl_file_delete(
    const char *path  ///< file path (UTF-8)
);

/**
 * @brief Rename or move a file or directory within its volume --
 *     `rename(2)` with `RENAME_NOREPLACE`.
 *
 * @a new_path is either a bare name ("bar.txt"), which keeps the entry in
 * @a old_path's directory, or a path on the SAME volume as @a old_path, in
 * that directory or any other. The entry moves in one
 * `EFI_FILE_PROTOCOL.SetInfo` and no data is copied, so the cost does not
 * depend on the file's size; a directory moves with everything below it.
 * Timestamps and attributes are unchanged.
 *
 * Never replaces: an existing @a new_path is #AXL_EXISTS and both entries
 * are left as they were. (axl_file_replace() and axl_file_move() replace;
 * this does not.)
 *
 * "Same volume" means the same filesystem, decided after both paths are
 * resolved (current directory, either separator, map-name case and
 * aliases) -- not by comparing their prefixes. Another volume is
 * #AXL_UNSUPPORTED with nothing changed: the `EXDEV` case, which
 * axl_file_move answers by copying. A filesystem that cannot move an entry
 * between directories -- an `<axl/axl-fs-provider.h>` provider without a
 * `rename` callback -- is #AXL_UNSUPPORTED too, with nothing changed; a
 * rename within one directory still works there.
 *
 * #AXL_OK means VERIFIED: the entry is re-read through the same handle and
 * must carry the new name, so a filesystem that accepts the request and
 * changes nothing is #AXL_ERR. One consequence on FAT: a rename that
 * changes only letter case ("a.txt" to "A.TXT") is the same name to the
 * driver, which does nothing, so it is #AXL_ERR. A name FAT stores without
 * its trailing dots and spaces ("a." is stored as "a") verifies as that
 * stored form, so long as it differs from the entry's old name.
 *
 * Refused with nothing changed (#AXL_ERR): a directory moved into itself
 * or below itself; a destination directory that does not exist; a path
 * whose `..` climbs above the volume root; the volume root; a directory
 * with any file below it open (the EDK2-derived FAT driver refuses that
 * move, and so does a provider volume); a read-only source or volume.
 *
 * "Nothing changed" cannot extend to a failure DURING the move. The
 * EDK2-derived FAT driver removes the old directory entry before writing
 * the new one, so a destination directory that must grow on a full volume
 * (or a full fixed-size FAT12/16 root) can fail after the removal and
 * leave the entry under neither name. AXL cannot reorder the driver's
 * writes; it logs an error when it finds the entry gone from both names.
 *
 * A handle already open on the moved file stays valid and follows it. An
 * `AxlFileView` keyed by either name is invalidated, and renaming or
 * moving a directory invalidates every view, since paths below it changed
 * too.
 *
 * @return #AXL_OK once the entry is verified at @a new_path;
 *     #AXL_NOT_FOUND if @a old_path does not exist;
 *     #AXL_UNSUPPORTED if @a new_path is on another volume, or the
 *     filesystem cannot move between directories -- nothing changed in
 *     either case, and copying is the alternative (axl_file_move);
 *     #AXL_EXISTS if @a new_path already exists;
 *     #AXL_ERR otherwise, including NULL arguments and a missing
 *     destination directory. Treat any negative value as failure.
 */
AXL_WARN_UNUSED int
axl_file_rename(
    const char *old_path,  ///< current path (UTF-8)
    const char *new_path   ///< bare name, or a path on the same volume (UTF-8)
);

/// axl_file_replace(): when the filesystem cannot make the move as one
/// rename -- another volume, or a provider without a `rename` callback
/// asked to move between directories -- copy a FILE instead. A directory
/// is copied only with #AXL_FILE_REPLACE_TREE as well; without both flags
/// a directory that cannot be renamed is #AXL_UNSUPPORTED.
#define AXL_FILE_REPLACE_COPY  0x01u
/// axl_file_replace(): replace whatever is at @a new_path -- a file, or a
/// directory with everything below it -- lifting rename(2)'s kind and
/// emptiness rules (WebDAV MOVE with `Overwrite: T`, RFC 4918 section
/// 9.9.3). It never lifts the rule that a path cannot run through a
/// non-directory, and never replaces a directory that holds @a old_path.
/// With #AXL_FILE_REPLACE_COPY it also lets a directory that cannot be
/// renamed be moved by copying it, with everything below it; the copy keeps
/// a record of every entry it creates (a path each, on the order of 10 MB
/// for 100,000 entries) so a copy that fails removes exactly those.
#define AXL_FILE_REPLACE_TREE  0x02u

/// What a successful axl_file_replace() did -- for a caller that must say
/// so (WebDAV answers 201, 204 or 403 by it).
typedef enum {
    AXL_FILE_REPLACE_RESULT_CREATED  = 0, ///< nothing was at @a new_path; the entry moved there
    AXL_FILE_REPLACE_RESULT_REPLACED = 1, ///< the entry at @a new_path was replaced
    AXL_FILE_REPLACE_RESULT_SAME     = 2, ///< both paths named one entry; nothing changed
    AXL_FILE_REPLACE_RESULT_RECASED  = 3, ///< both paths named one entry; its name's letter case changed
} AxlFileReplaceResult;

/**
 * @brief Move an entry onto a path, replacing what is there --
 *     `rename(2)` -- and put both entries back if it fails.
 *
 * Moves the file or directory at @a old_path to @a new_path as
 * axl_file_rename() does, except that an existing @a new_path is REPLACED.
 * @a new_path is a bare name, which keeps the entry in @a old_path's
 * directory, or a path; either separator. With @a flags 0 this is
 * rename(2): no copying, rename(2)'s rules and refusals. When nothing is
 * at @a new_path, none of the aside steps below happen.
 *
 * FAT cannot rename over an existing name, so a replace is a sequence,
 * ordered so that a failure AXL can observe puts both entries back:
 *   1. every refusal below is decided before anything changes;
 *   2. the entry at @a new_path is renamed ASIDE, in its own directory, to
 *      a fresh name `~AXLhhhh.TMP` (h an uppercase hex digit), which needs
 *      no long-name entry and is never a FAT-generated alias;
 *   3. @a old_path must still resolve and @a new_path must not -- otherwise
 *      the two were one entry, or @a new_path held @a old_path, under
 *      spellings the stored names did not reveal: the aside is renamed back
 *      and the call fails;
 *   4. the source -- through the handle step 1 identified it by -- is
 *      renamed to @a new_path, or, with #AXL_FILE_REPLACE_COPY when that
 *      rename is #AXL_UNSUPPORTED, a file is copied there, flushed through
 *      to the volume, given the source's modification time where the
 *      destination can store it, and the source deleted (with
 *      #AXL_FILE_REPLACE_TREE as well, a directory is copied the same way,
 *      file by file, and the source tree then removed);
 *   5. only then is the aside removed (with everything below it, under
 *      #AXL_FILE_REPLACE_TREE). Removal acts only on the aside name, never
 *      on @a new_path.
 * If step 4 fails, the aside is renamed back to the name read through its
 * own handle in step 2 (the long name, even when @a new_path spelled an
 * 8.3 alias). A failed rename removes nothing: an entry found at
 * @a new_path after all means the move happened, and it is #AXL_OK. A
 * failed copy removes only the partial copy, through the handle that
 * created it (a partial directory copy, by removing the tree it created).
 * A copy whose source then cannot be deleted is undone the same way only
 * while the source still holds every byte copied. A source tree whose
 * removal fails part-way after a complete directory copy is not undone:
 * the move happened, what is left of the source is logged as a warning,
 * and the call returns #AXL_OK.
 *
 * A Ctrl-C during the sequence does not end the program inside it: before
 * step 5 the sequence is undone and the call returns #AXL_CANCELLED; once
 * step 5 has begun it completes. The interrupt stays pending for the
 * caller's next yield.
 *
 * Identity is decided by the names the filesystem STORES, read through a
 * handle on every component of both paths and compared exactly, on one
 * volume -- never by comparing the strings given:
 *   - @a new_path naming the same entry as @a old_path -- the same path,
 *     `.\x`, the other separator, an 8.3 alias of the entry or of any
 *     directory on the way, trailing dots FAT drops -- is #AXL_OK and
 *     changes nothing, as rename(2) of two links to one file;
 *   - except when @a new_path's LAST component differs from the entry's
 *     stored name in letter case only ("a.txt" to "A.TXT"): the entry is
 *     renamed to that spelling through an aside of itself, and #AXL_OK
 *     means it now reads back so. Letter case elsewhere in @a new_path
 *     changes nothing. (axl_file_rename() refuses a case-only change; one
 *     SetInfo cannot make it.) Only ASCII letters count: a name that
 *     differs from the stored one only in the case of other letters ("ä"
 *     to "Ä"), which FAT also finds as the same entry, is SAME and keeps
 *     its stored spelling.
 * A filesystem that reports the name it was asked for instead of the one
 * it stores (some providers) defeats this; step 3 is then what catches two
 * spellings of one entry.
 *
 * rename(2)'s rules, in rename(2)'s order, before anything changes:
 *   - a path through a non-directory: #AXL_NOT_DIR;
 *   - without #AXL_FILE_REPLACE_COPY, @a new_path on another volume:
 *     #AXL_UNSUPPORTED, whatever the kinds -- rename(2)'s `EXDEV`, which it
 *     answers before every rule below;
 *   - a directory moved into itself or below itself: #AXL_INVALID;
 *   - @a new_path a directory that holds @a old_path: #AXL_NOT_EMPTY
 *     (#AXL_INVALID with #AXL_FILE_REPLACE_TREE: replacing it would remove
 *     the source);
 *   - a directory onto a non-directory: #AXL_NOT_DIR; a non-directory onto
 *     a directory: #AXL_IS_DIR;
 *   - a directory onto a directory that holds anything: #AXL_NOT_EMPTY. A
 *     directory that cannot be listed counts as holding something. A
 *     directory onto an EMPTY directory replaces it.
 * #AXL_FILE_REPLACE_TREE lifts the last two rules and no other.
 *
 * Without #AXL_FILE_REPLACE_COPY a move the filesystem cannot make as one
 * rename is #AXL_UNSUPPORTED with nothing changed -- the `EXDEV` case.
 *
 * Also refused with nothing changed: #AXL_DENIED for an entry with the
 * read-only attribute at either path, or a write-protected volume (FAT
 * will neither rename nor remove such an entry); #AXL_NO_RESOURCES when
 * @a new_path's volume reports less free space than the sequence may
 * need -- the clusters its directory must grow by for the entries steps 2
 * and 4 and an undo append there, counted from those names' directory-
 * entry slots and the volume's cluster size, plus the source rounded up
 * to whole clusters for a copy (a directory copy: each new directory's
 * entries rounded up once, and each file's data). The replaced entry's
 * space is not counted as free: it is held until step 5, so a replace
 * across volumes needs room for both, where `mv` would remove the target
 * first. The destination directory's growth is counted as if it had no
 * free slot left -- AXL cannot see how many it has -- so a directory with
 * room to spare can be refused by up to one cluster more than it needs; a
 * fixed-size FAT12/16 root, which never grows, likewise. A volume that
 * cannot report its free space is not refused.
 *
 * What it cannot prevent:
 *   - The EDK2-derived FAT driver removes an entry's old directory entry
 *     before writing the new one, and appends the new one, growing the
 *     directory when it must. A growth that fails loses that entry under
 *     both names. The free-space check closes this only where the reported
 *     figure is exact and nothing else writes the volume meanwhile: FAT12
 *     and FAT16 count their free clusters, but FAT32 reports its FSInfo
 *     hint, which a volume last written elsewhere can overstate; and a
 *     full fixed-size FAT12/16 root directory, which cannot grow, is not
 *     covered. An entry found gone from every name is logged as an error
 *     naming them.
 *   - The same driver empties a file before removing its entry, so a
 *     source delete that fails part-way after a copy can leave the source
 *     short. The copy is then kept at @a new_path, the replaced entry
 *     stays at its aside name, an error names both, and the call fails.
 *   - Power loss or reset: from step 2 to step 5 the replaced entry is at
 *     the aside name, and nothing removes it or puts it back after a
 *     restart; a `~AXLhhhh.TMP` found later is such an entry.
 *   - Other writers: step 1's checks hold only while nothing else changes
 *     the directories involved during the call.
 *   - A filesystem that reports the name it was asked for rather than the
 *     one it stores (some providers): two spellings of one entry are then
 *     caught only at step 3, and putting the aside back renames the entry
 *     to the spelling its handle reported -- the destination's -- so the
 *     call fails with the entry under @a new_path's spelling.
 *   - An aside that cannot be renamed back is left at its aside name and
 *     logged as an error naming both. An aside that cannot be removed after
 *     a replace that succeeded is logged as a warning naming it, and the
 *     call returns #AXL_OK: the replace happened. A tree removal that fails
 *     part-way leaves the rest under the aside name, with that warning.
 *
 * A handle already open on @a old_path follows a RENAMED entry; after a
 * copy it refers to a deleted file. A handle open on the replaced entry
 * refers to a deleted entry once the call succeeds (FAT keeps no
 * unlinked-but-open files). An `AxlFileView` keyed by either name is
 * invalidated; moving or replacing a directory invalidates every view.
 *
 * @return #AXL_OK once the entry is verified at @a new_path, or when both
 *     paths named one entry -- @a result says which;
 *     #AXL_NOT_FOUND if @a old_path, or @a new_path's directory, does not
 *     exist on a volume that resolved;
 *     #AXL_INVALID for a directory into itself, an overlap
 *     #AXL_FILE_REPLACE_TREE would destroy the source by, or an unknown bit
 *     in @a flags;
 *     #AXL_NOT_DIR, #AXL_IS_DIR, #AXL_NOT_EMPTY as above;
 *     #AXL_UNSUPPORTED for a move only a copy could make, without
 *     #AXL_FILE_REPLACE_COPY, or of a directory without
 *     #AXL_FILE_REPLACE_TREE as well;
 *     #AXL_DENIED for a read-only entry at either path, or a write-protected
 *     volume;
 *     #AXL_NO_RESOURCES for a volume short of the space above;
 *     #AXL_CANCELLED for a Ctrl-C during the sequence, undone;
 *     #AXL_ERR otherwise -- NULL arguments, a volume that does not resolve,
 *     the volume root at either path, an allocation failure, a directory
 *     with a file below it open (the FAT driver refuses to move it), a name
 *     that cannot be read whole, a copy or flush that failed, one entry
 *     under two spellings found only at step 3. Except as listed under
 *     "What it cannot prevent", every failure leaves both entries as they
 *     were. Treat any negative value as failure.
 */
AXL_WARN_UNUSED int
axl_file_replace(
    const char           *old_path,  ///< entry to move (UTF-8)
    const char           *new_path,  ///< bare name, or a path (UTF-8)
    uint32_t              flags,     ///< AXL_FILE_REPLACE_* (0 = rename(2))
    AxlFileReplaceResult *result     ///< [out] what happened, written only on #AXL_OK; may be NULL
);

/**
 * @brief Move a file or directory -- `mv`: axl_file_replace() with
 *     #AXL_FILE_REPLACE_COPY.
 *
 * Everything axl_file_replace() says applies. Within a volume the entry
 * moves by one rename, whatever its size; only when that rename is
 * #AXL_UNSUPPORTED (another volume, or a filesystem that cannot move
 * between directories) is a FILE copied in chunks, flushed, given the
 * source's modification time, and the source deleted. A directory moves
 * only by rename. An existing @a new_path is replaced; a failed copy or
 * flush, or a source that cannot be deleted but still holds every byte,
 * removes the copy and puts the replaced entry back. A source that a
 * failed delete has EMPTIED is not undone: the copy -- then the only
 * whole one -- stays at @a new_path, the replaced entry stays at its
 * `~AXLhhhh.TMP` aside, an error names both, and the call fails (see
 * axl_file_replace(), "What it cannot prevent"). A @a new_path that names
 * the source itself -- the
 * same path, another spelling, an 8.3 alias -- changes nothing; one that
 * differs from the source's stored name in letter case only renames it to
 * that spelling.
 *
 * Use axl_file_rename() for fail-if-exists, and axl_file_replace() to
 * learn what happened (created, replaced, unchanged) or to replace a
 * directory tree.
 *
 * @return as axl_file_replace().
 */
int
axl_file_move(
    const char *old_path,  ///< current path (UTF-8)
    const char *new_path   ///< new path (UTF-8); may be in a different directory
);

/**
 * @brief Set an existing file's length — the `truncate(2)` analog.
 *
 * Makes the file at @p path exactly @p size bytes long, whether that
 * shrinks or grows it. A shrink discards the tail and leaves the
 * surviving prefix byte-exact; `size == 0` empties the file. A @p size
 * equal to the current length changes nothing, but is still fully
 * checked — a missing path, a directory, or an unwritable volume fails
 * rather than reporting a hollow success.
 *
 * axl_ftruncate() is the form for a file held open in an `AxlStream`;
 * see the invalidation note below for what a resize by path does to
 * objects already holding the file.
 *
 * The file must already exist: unlike axl_file_set_contents this never
 * creates one, and a missing @p path is AXL_ERR. Directories are
 * refused — UEFI derives a directory's size from its contents, and the
 * spec has SetInfo either ignore a directory's FileSize outright or
 * fail it.
 *
 * AXL_OK means the length was VERIFIED, not merely requested: the new
 * length is re-read from the same open handle before returning. A
 * filesystem that accepts the request and silently ignores it — a
 * spec-conformant driver on a directory, or an
 * `<axl/axl-fs-provider.h>` provider whose `set_info` only implements
 * renames and attribute changes — yields AXL_ERR rather than a success
 * that changed nothing.
 *
 * Bytes added by a grow read back as zero on the EDK2-derived FAT
 * driver shipped by essentially all UEFI firmware, which physically
 * writes the gap out. The UEFI spec does not require that, so a caller
 * needing zeros on some other volume (a non-FAT driver, a custom
 * `<axl/axl-fs-provider.h>` filesystem) must write them itself.
 *
 * That zero-fill also means growing is NOT a cheap metadata update: it
 * is a real O(@p size) disk write that consumes the volume's free
 * space. Growing by gigabytes writes gigabytes, and UEFI file I/O is
 * synchronous, so a single-threaded caller stalls for the whole
 * transfer. Bound @p size before handing this a length that came from
 * untrusted input.
 *
 * Other limits: FAT caps a file at 4 GiB - 1, so a larger @p size
 * fails; a read-only volume, a file carrying the read-only attribute,
 * and a full volume all fail. Timestamps are PRESERVED, not bumped —
 * the resize writes the file's existing (non-zero, therefore honored)
 * times back through SetInfo, so unlike POSIX truncation this does not
 * update the modification time.
 *
 * An open `AxlFileView` over the same file will USUALLY follow the resize
 * — this is an AXL write path, so a view in the same PE image re-stats
 * and drops its cached pages before its next read. That is best effort,
 * not a guarantee: `AxlFileView` promises close-to-open consistency, so a
 * view in another image, or one the caller pinned
 * (`axl_file_view_set_pinned`), keeps the length it last observed.
 * Re-open the view if it must see this resize. Raw handles opened
 * elsewhere keep their positions, which a shrink can leave past end of
 * file; there is no locking.
 *
 * @return AXL_OK once the file is verified to be @p size bytes long;
 *     AXL_ERR on NULL @p path, a missing file, a directory, a read-only
 *     or full volume, a length the filesystem cannot represent, a
 *     resize the filesystem ignored, or any backend failure. AXL_ERR
 *     does not distinguish these: a caller that must tell them apart
 *     can pre-check existence, the DIRECTORY bit and the READ_ONLY bit
 *     with axl_file_info, but cannot separate a full volume from an I/O
 *     error. After AXL_ERR the file's length is unspecified — a grow
 *     extends the file before zero-filling it, so a failure part-way
 *     through leaves it longer than it started; re-read it with
 *     axl_file_info if it matters.
 */
AXL_WARN_UNUSED int
axl_file_truncate(
    const char *path,  ///< file path (UTF-8)
    uint64_t    size   ///< new file length in bytes
);

/**
 * @brief Set the length of the file under an open stream -- `ftruncate(2)`.
 *
 * axl_file_truncate() for a file you hold open: it acts on the file @a s
 * was opened on, wherever that file now is. A rename, or a new file created
 * at the old path, does not redirect it -- which a path saved at open time
 * would.
 *
 * Everything axl_file_truncate() says about the change itself applies: grow
 * or shrink to exactly @a size; growth is zero-filled on the EDK2-derived
 * FAT driver and is a real O(@a size) write; FAT caps a file at 4 GiB - 1;
 * timestamps are preserved; #AXL_OK means the new length was re-read from
 * the same handle and flushed.
 *
 * Bytes still held in @a s's own write buffer (axl_stream_set_buffering)
 * are written out FIRST, so none of them can land after the resize. If
 * writing them out fails, this returns #AXL_ERR before the length is
 * touched.
 *
 * The stream's position is NOT moved, as with POSIX. After a shrink below
 * it, a write extends the file again from that position, and a READ there
 * fails rather than reporting end of file (the EDK2-derived FAT driver
 * treats a read past the end as a device error) -- seek first.
 *
 * @a s must be a stream axl_fopen() returned, not one built over it
 * (axl_text_stream_wrap, a compress filter): those, buffer streams, the
 * console streams and custom streams are #AXL_UNSUPPORTED, unchanged. A
 * stream opened "r" is #AXL_DENIED: UEFI refuses a size change through a
 * read-only handle.
 *
 * @return #AXL_OK once the file is verified to be @a size bytes long;
 *     #AXL_UNSUPPORTED if @a s is not a file stream;
 *     #AXL_DENIED if @a s was opened for reading only;
 *     #AXL_ERR on NULL @a s, a directory, a read-only or full volume, a
 *     length the filesystem cannot represent or ignored, a file deleted
 *     through another handle, or a failure writing out buffered bytes.
 *     After #AXL_ERR from the resize itself the length is unspecified, as
 *     for axl_file_truncate(). Treat any negative value as failure.
 */
AXL_WARN_UNUSED int
axl_ftruncate(
    AxlStream *s,     ///< stream from axl_fopen()
    uint64_t   size   ///< new file length in bytes
);

/// Earliest time axl_file_set_mtime() accepts: 1980-01-01 00:00:00 UTC.
#define AXL_FS_MTIME_MIN       315532800ULL
/// Latest: 2107-12-31 23:59:59 UTC (FAT stores it as :58).
#define AXL_FS_MTIME_MAX       4354819199ULL
/// Earliest time EVERY volume keeps: 1981-01-01 00:00:00 UTC. The
/// EDK2-derived FAT driver drops the year of a 1980 time, so a server that
/// must clamp a time it was sent clamps here, not at #AXL_FS_MTIME_MIN.
#define AXL_FS_MTIME_KEPT_MIN  347155200ULL

/**
 * @brief Set a file's or directory's modification time -- `utimensat(2)`
 *     with the access time omitted.
 *
 * @a mtime_unix is Unix seconds, UTC: the clock and encoding axl_file_info()
 * reports in `AxlFsEntry.mtime_unix`, so a time read from one file can be
 * applied to another unchanged. It is written as UTC calendar fields with
 * the time zone unspecified, which is how AXL reads every file time back.
 *
 * Only the modification time changes; size, attributes, name and creation
 * time are untouched. There is no access-time argument: UEFI's FAT driver
 * ignores a supplied access time and stamps its own.
 *
 * Range and precision:
 *   - @a mtime_unix below #AXL_FS_MTIME_MIN or above #AXL_FS_MTIME_MAX is
 *     #AXL_INVALID on EVERY volume, refused before the volume is touched:
 *     FAT's range, applied everywhere so that a time one volume accepts
 *     another does not refuse (the EDK2-derived driver would silently
 *     clamp a later year to 2107);
 *   - precision is the volume's: FAT keeps two-second steps (an odd second
 *     as the even second below it); a provider may keep the second.
 * axl_file_info() reads back the value actually stored.
 *
 * #AXL_OK means VERIFIED: the time is re-read through the same open handle
 * and must equal @a mtime_unix or lie one second below it, and the change
 * is flushed before returning. A filesystem that accepts the request but
 * keeps another time -- an `<axl/axl-fs-provider.h>` provider whose
 * `set_info` does not honour `mtime_unix` (a 9P mount does) -- is
 * #AXL_ERR. After a failed check the previous time is written back, so a
 * failure leaves the time as it was (best effort: if that write fails too,
 * the time is unspecified). Known case: the EDK2-derived FAT driver drops
 * the year of any 1980 time and keeps the old one, so a 1980 time on a
 * file stamped in another year is #AXL_ERR; #AXL_FS_MTIME_KEPT_MIN is the
 * first second every volume keeps.
 *
 * @a path must exist; nothing is created. The volume root has no directory
 * entry on FAT and is #AXL_ERR. A file with the read-only attribute is
 * #AXL_DENIED, as the UEFI Shell's `touch` refuses it: UEFI opens such a
 * file read-only, and SetInfo then silently ignores the time.
 *
 * A later write through ANY handle on the file stamps the current time
 * again when that handle is flushed or closed, as on POSIX. A caller still
 * holding unwritten data in an `AxlStream` on the same file must
 * axl_fflush() it first, or use axl_fset_mtime() on that stream.
 *
 * @return #AXL_OK once the stored time is verified;
 *     #AXL_INVALID if @a mtime_unix is outside the range above;
 *     #AXL_NOT_FOUND if @a path does not exist;
 *     #AXL_DENIED if the file is read-only;
 *     #AXL_ERR otherwise (NULL @a path, the volume root, a read-only
 *     volume, a time the filesystem did not keep, a device error). Treat
 *     any negative value as failure.
 */
AXL_WARN_UNUSED int
axl_file_set_mtime(
    const char *path,       ///< file or directory path (UTF-8)
    uint64_t    mtime_unix  ///< modification time, Unix seconds, UTC
);

/**
 * @brief Set the modification time of the file under an open stream --
 *     `futimens(2)` with the access time omitted.
 *
 * axl_file_set_mtime() for a file you hold open, acting on the file @a s
 * was opened on wherever it now is. Its encoding, range (#AXL_INVALID
 * outside #AXL_FS_MTIME_MIN..#AXL_FS_MTIME_MAX), precision, verification
 * and restore-on-failure rules all apply unchanged.
 *
 * Bytes still held in @a s's own write buffer are written out FIRST: they
 * would otherwise reach the file afterwards and stamp the current time
 * over the one set here. That makes write, axl_fset_mtime(), axl_fclose()
 * -- the order `put -p` and `scp -p` use -- keep the time. A write made
 * after this call, through any handle, stamps the current time again, as
 * on POSIX.
 *
 * @a s must be a stream axl_fopen() returned and opened for writing: a
 * read-only handle makes the FAT driver drop the time silently, so a
 * stream opened "r" is #AXL_DENIED rather than a verification failure.
 * Other streams are #AXL_UNSUPPORTED.
 *
 * @return #AXL_OK once the stored time is verified;
 *     #AXL_INVALID if @a mtime_unix is outside the range above;
 *     #AXL_UNSUPPORTED if @a s is not a file stream;
 *     #AXL_DENIED if @a s was opened for reading only, or the file has the
 *     read-only attribute;
 *     #AXL_ERR otherwise (NULL @a s, a time the filesystem did not keep, a
 *     deleted file, a failure writing out buffered bytes). Treat any
 *     negative value as failure.
 */
AXL_WARN_UNUSED int
axl_fset_mtime(
    AxlStream *s,          ///< stream from axl_fopen(), opened for writing
    uint64_t   mtime_unix  ///< modification time, Unix seconds, UTC
);

/**
 * @brief Create a directory (idempotent).
 *
 * Succeeds if @p path already exists AS A DIRECTORY (so mkdir-p and
 * copy-into-existing flows need no pre-check), but fails if a non-directory
 * already occupies @p path.
 *
 * @return AXL_OK if the directory was created or already exists; AXL_ERR if a
 *     non-directory occupies @p path, or on any other error.
 */
int
axl_dir_mkdir(
    const char *path  ///< directory path (UTF-8)
);

/**
 * @brief Remove an empty directory.
 *
 * @return AXL_OK on success, AXL_ERR on error (including if not empty).
 */
int
axl_dir_rmdir(
    const char *path  ///< directory path (UTF-8)
);

// ---------------------------------------------------------------------------
// Directory iteration
// ---------------------------------------------------------------------------

typedef struct AxlDir AxlDir;

/**
 * @brief Open a directory for iteration.
 *
 * @return directory handle, or NULL on error.
 */
AxlDir *
axl_dir_open(
    const char *path  ///< directory path (UTF-8)
);

/**
 * @brief Read the next directory entry.
 *
 * `entry->name` is populated with the entry's basename;
 * `entry->attributes` carries the kind bits
 * (`AXL_FS_ATTR_DIRECTORY` for sub-dirs, etc.).
 *
 * On every driver, an entry whose name cannot be joined onto a path as ONE
 * component -- empty, or holding `\` or `/` -- is skipped, as is one too
 * long for `entry->name` (over 765 bytes of UTF-8), which cut short could
 * be a sibling's name. A FAT name always fits (FAT creates at most 255
 * UTF-16 units); a longer one comes only from a non-FAT driver, a
 * provider, or a FAT volume written by a non-conforming tool (the
 * EDK2-derived driver reads long names of up to 260 units). The first
 * skipped in a directory is logged at
 * warning with the name escaped, the rest at debug. "." and ".." are
 * returned only from drivers that list them: FAT lists them in a
 * subdirectory but not in a volume's root, and a provider-published volume
 * lists them nowhere.
 *
 * @return true if an entry was read, false at end of directory.
 */
bool
axl_dir_read(
    AxlDir     *dir,   ///< directory handle
    AxlFsEntry *entry  ///< [out] receives entry
);

/**
 * @brief Close a directory handle. NULL-safe.
 */
void
axl_dir_close(
    AxlDir *dir  ///< directory handle
);

/**
 * @brief Per-entry callback for axl_dir_walk.
 *
 * @param full_path  full path to the entry (root + separator + name)
 * @param entry      the AxlFsEntry, including name, size, attributes
 * @param user       opaque user pointer passed through from caller
 *
 * Return codes:
 *   - 0  continue walking
 *   - >0 stop (propagated as the walk's return value)
 *   - <0 stop with error (propagated)
 */
typedef int (*AxlDirWalkFn)(
    const char        *full_path,
    const AxlFsEntry  *entry,
    void              *user
) AXL_CB_NOEXCEPT;

/**
 * @brief Recursively walk a directory tree.
 *
 * Calls @p fn on every entry (excluding `.` and `..`) under @p root,
 * descending into subdirectories automatically. Each entry's full
 * path is constructed with separator deduplication so the callback
 * sees clean paths regardless of whether @p root has a trailing
 * `/` or `\`. Recursion is post-callback — the walker invokes @p fn
 * on a directory entry first, then descends into it.
 *
 * `max_depth` matches POSIX `find -maxdepth`: it caps the deepest
 * level the callback runs at, where root's immediate children are
 * level 1, their children are level 2, and so on.
 *   - `max_depth = 1` lists root's immediate children only.
 *   - `max_depth = N` lists at most N levels of nesting below root.
 *   - `max_depth <= 0` is rejected (returns -1).
 *
 * The walk is iterative and keeps its state on the heap -- one open
 * directory and one path per level -- so a deep tree costs no stack, and
 * every path reaches @p fn whole, however long. A subdirectory that cannot
 * be opened is skipped, not an error.
 *
 * @return 0 on a clean traversal; the callback's non-zero return value,
 *     from whatever level it came, if it stopped the walk; or -1 if @p root
 *     could not be opened, arguments are invalid, or memory ran out.
 */
int
axl_dir_walk(
    const char    *root,        ///< starting directory
    AxlDirWalkFn   fn,          ///< per-entry callback
    void          *user,        ///< opaque user pointer for the callback
    int            max_depth    ///< maximum nesting level visited (>=1)
);

/**
 * @brief Serialize directory entries to a JSON array.
 *
 * Writes a JSON array of objects into @p buf. Each object has:
 * "name" (string), "size" (uint64), "dir" (boolean).
 *
 * Example output: [{"name":"foo.txt","size":1024,"dir":false}]
 *
 * @return AXL_OK on success, AXL_ERR on error or buffer overflow.
 */
int
axl_dir_list_json(
    const AxlFsEntry *entries,  ///< array of directory entries
    size_t            count,    ///< number of entries
    char             *buf,      ///< output buffer
    size_t            buf_size  ///< output buffer size
);

// ---------------------------------------------------------------------------
// Volume operations
// ---------------------------------------------------------------------------

/**
 * @brief Get the filesystem volume label for a path.
 *
 * The label belongs to the volume, so every spelling of one volume
 * answers the same: `"fs0:"` — with or without a trailing backslash —
 * and `"fs0:\\dir\\file.txt"` are equivalent, and @p path need not
 * exist — an absent file resolves to
 * its volume's label rather than failing. A path with no volume prefix
 * is resolved against the current working directory; a path naming a
 * volume too long to resolve fails rather than falling back to it.
 *
 * Returns a UTF-8 copy of the label. Caller frees with axl_free().
 *
 * @return label string, or NULL on error.
 */
char *
axl_volume_get_label(
    const char *path  ///< any path on the volume (e.g., "fs0:", "fs1:\\")
);

/**
 * @brief Get the filesystem volume label for a handle.
 *
 * Use with handles from axl_protocol_enumerate("simple-fs", ...).
 * Returns a UTF-8 copy of the label. Caller frees with axl_free().
 *
 * @return label string, or NULL on error.
 */
char *
axl_volume_get_label_by_handle(
    void *handle  ///< filesystem handle from axl_protocol_enumerate
);

/**
 * @brief Get total and free bytes for the volume containing a path.
 *
 * The intended use is a pre-flight check — "will this write fit?" —
 * so the query is answered by the volume ROOT, not by @p path itself:
 * @p path need not exist. `"fs0:"` — with or without a trailing
 * backslash — and `"fs0:\\EFI\\BOOT\\new.efi"` all report the same
 * volume. A @p path with no volume prefix is resolved against the
 * current working directory.
 *
 * Either out-parameter may be NULL to skip it; passing NULL for both
 * asks for nothing and is an error. Only the figures you actually ask
 * for affect the result, so a volume that knows its free space but not
 * its total still answers a free-space-only query.
 *
 * Failure modes are kept distinguishable on purpose, in both
 * directions. A caller sizing an upload must be able to tell "1 KB
 * left, refuse" from "this volume cannot say, decide some other way"
 * — and must equally be able to tell either of those from "that
 * volume is broken". So a value is written **only** when it is a real
 * figure reported by the volume, and a volume that *fails* the query
 * is never reported as one that merely cannot answer. Nothing is ever
 * zero-filled or left at a sentinel for the caller to recognise: on
 * any non-AXL_OK return both out-parameters are untouched.
 *
 * A @p path that names a volume too long to resolve fails; it is never
 * silently answered from the working directory, which would report a
 * different volume's figures under a clean AXL_OK.
 *
 * @return AXL_OK with every requested figure written; AXL_UNSUPPORTED
 *     if the volume has no filesystem information to give, or reports
 *     a requested figure as unknown — out-params untouched; AXL_ERR on
 *     a NULL / empty / both-out-NULL / over-long argument, a path whose
 *     volume cannot be resolved or opened, an allocation failure, or a
 *     device / media / corrupt-volume error — out-params untouched.
 */
AXL_WARN_UNUSED int
axl_volume_get_space(
    const char *path,       ///< any path on the volume (e.g. "fs0:", "fs0:\\dir\\f")
    uint64_t   *total,      ///< [out] total bytes on the volume, or NULL
    uint64_t   *free_bytes  ///< [out] bytes available for new data, or NULL
);

/**
 * @brief Get total and free bytes for a volume by handle.
 *
 * Handle-based sibling of axl_volume_get_space, pairing with
 * @ref axl_volume_get_label_by_handle. Use it with handles from
 * @ref axl_volume_enumerate or `axl_protocol_enumerate("simple-fs", ...)`:
 * a handle always names exactly one volume, whereas AxlVolume.name can
 * fall back to a synthesized index that is not a usable shell path.
 *
 * Same contract as axl_volume_get_space in every other respect,
 * including that both out-parameters are untouched unless AXL_OK is
 * returned.
 *
 * @return AXL_OK with every requested figure written; AXL_UNSUPPORTED
 *     if the volume has no filesystem information to give, or reports
 *     a requested figure as unknown; AXL_ERR on a NULL / both-out-NULL
 *     argument, a handle carrying no filesystem, an allocation failure,
 *     or a device / media / corrupt-volume error.
 */
AXL_WARN_UNUSED int
axl_volume_get_space_by_handle(
    void     *handle,       ///< filesystem handle (AxlVolume.handle / axl_protocol_enumerate)
    uint64_t *total,        ///< [out] total bytes on the volume, or NULL
    uint64_t *free_bytes    ///< [out] bytes available for new data, or NULL
);

/// Volume descriptor for axl_volume_enumerate.
typedef struct {
    void  *handle;       ///< opaque filesystem handle
    char   name[16];    ///< the UEFI Shell's own alias for this volume,
                        ///< lowercased ("fs0", "fs1", ...) — matches what
                        ///< `map` / `vol fsN:` show, so name, handle, and
                        ///< device_path all refer to the same volume. Falls
                        ///< back to the LocateHandle index ("fs<i>") only when
                        ///< the shell has no mapping for the volume (e.g. a
                        ///< just-created ramdisk before the shell remaps).
    void  *device_path; ///< opaque EFI_DEVICE_PATH_PROTOCOL — caller may
                        ///< pass to axl_device_path_find / _for_each.
                        ///< The pointer is firmware-owned; callers
                        ///< must not free it.
} AxlVolume;

/**
 * @brief Enumerate mounted filesystem volumes.
 *
 * Fills @p out with up to @p max descriptors, each naming the volume by the
 * UEFI Shell's own fsN alias (see @ref AxlVolume) with a matching opaque
 * handle and device path. On return, @p count receives the number of entries
 * filled.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_volume_enumerate(
    AxlVolume *out,    ///< output array (may be NULL to query count)
    size_t     max,    ///< capacity of @p out
    size_t    *count   ///< [out] number of volumes found
);

/**
 * @brief Resolve the UEFI Shell's fsN alias for a device path.
 *
 * Looks @p device_path up in the shell's volume map (GetMapFromDevicePath)
 * and writes the lowercased `fsN` alias (no trailing `:`) to @p out -- `fs`
 * and digits only, so a custom name like `fs1a` is not taken for one. This is
 * the shell's *actual* mapping — unlike axl_volume_enumerate()'s `.name`,
 * it never falls back to a synthesized LocateHandle index. Use it when you
 * must publish a caller-usable `fsN` (e.g. a freshly created RAM disk): a real
 * alias, or a clean failure you can report — never a plausible-but-wrong index.
 *
 * On the old EFI 1.x shell (no GetMapFromDevicePath) it reverse-looks-up the
 * name via SHELL_ENVIRONMENT.GetMap, so it resolves once the disk is in the
 * shell's map (after `map -r`).
 *
 * @return AXL_OK with @p out set to the alias; AXL_ERR if @p device_path has no
 *     shell mapping (not yet remapped, or no fs alias), an argument is invalid,
 *     @p out_size is too small for the whole name (it is never shortened), or
 *     the backend has no shell at all. @p out is left unchanged on any
 *     non-AXL_OK return.
 */
AXL_WARN_UNUSED int
axl_volume_map_name(
    const void *device_path,   ///< device path (from AxlVolume / axl_ramdisk_create)
    char       *out,           ///< [out] receives the lowercased "fsN" alias
    size_t      out_size       ///< capacity of @p out in bytes (including NUL)
);

/**
 * @brief Resolve the shell's current alias for a device path — ANY form.
 *
 * Like axl_volume_map_name, but returns the FIRST alias the shell lists for
 * @p device_path (GetMapFromDevicePath) verbatim, minus the trailing `:`,
 * whether it is an `fsN` name or a custom SetMap name (e.g. `RD`). Use it to
 * ask "is this device already mapped, and as what?" — e.g. to make a
 * re-create idempotent by reusing the existing mapping instead of adding a
 * second alias. When you specifically need a usable `fsN` (and want a clean
 * failure otherwise), use axl_volume_map_name instead.
 *
 * If a device path carries several aliases (e.g. both a custom name and an
 * `fsN` after a `map -r`), which one is "first" is firmware-defined; a device
 * with a single alias (the common case, e.g. a freshly SetMap'd RAM disk)
 * always resolves to that alias. An alias longer than @p out_size is reported
 * as AXL_ERR, never truncated.
 *
 * On the old EFI 1.x shell (no GetMapFromDevicePath) it reverse-looks-up the
 * name via SHELL_ENVIRONMENT.GetMap (fs<n> form).
 *
 * @return AXL_OK with @p out set to the alias; AXL_ERR if @p device_path has
 *     no shell mapping, an argument is invalid, or the backend has no shell.
 *     @p out is left unchanged on any non-AXL_OK return.
 */
AXL_WARN_UNUSED int
axl_volume_map_alias(
    const void *device_path,   ///< device path (from AxlVolume / axl_ramdisk_create)
    char       *out,           ///< [out] receives the alias, verbatim, no `:`
    size_t      out_size       ///< capacity of @p out in bytes (including NUL)
);

/**
 * @brief Is a UEFI Shell map name currently in use?
 *
 * @p name is the mapping name with or without a trailing `:` (e.g. `"fs2"` or
 * `"fs2:"`, `"RD"` / `"RD:"`). Useful to reject a caller-requested name that
 * is taken, or to scan for the next free `fsN`.
 *
 * @return true if the shell has a mapping for @p name; false otherwise (also
 *     false with no shell, or @p name NULL/empty).
 */
bool
axl_volume_map_taken(
    const char *name   ///< mapping name, `:` optional
);

/**
 * @brief Assign a UEFI Shell map name to a device path (SetMap).
 *
 * Adds @p name -> @p device_path to the shell's map. Unlike a firmware
 * ConnectController + `map -r` cycle, this is usable by the launching
 * shell/script **immediately, without `map -r`** — even when called from a
 * child image (SetMap targets the shell's global map, not a nested shell).
 * @p name may omit the trailing `:`. The device path must carry a filesystem
 * (i.e. be connected — e.g. a RAM disk from @ref axl_ramdisk_create, which
 * connects it) for the name to be usable as a volume.
 *
 * If @p name is already mapped, the volume that held it loses it: the shell
 * removes the old mapping before adding the new one. Pair with
 * axl_volume_map_taken() to avoid that, or use axl_volume_pick_free_fsn().
 *
 * @return AXL_OK on success; AXL_ERR on bad args / SetMap failure;
 *     AXL_UNSUPPORTED when there is no shell (SetMap unavailable).
 */
AXL_WARN_UNUSED int
axl_volume_set_map(
    const void *device_path,   ///< device path (e.g. from axl_ramdisk_create)
    const char *name           ///< mapping name to assign, `:` optional
);

/**
 * @brief Add a named alias for an already-mapped volume via the shell's `map`.
 *
 * Runs the shell's own `map <alias> <fsn>:` command (through the shell's
 * Execute service), aliasing @p alias to the EXISTING mapping @p fsn (e.g.
 * `"fs1"`). This is the path for shells with no programmatic SetMap — notably
 * the old EFI 1.x shell — where @ref axl_volume_set_map is unavailable but the
 * `map` command can alias one map name to another. Because it points at an
 * existing `fsN` (which already has a resolvable device path), it succeeds
 * where SetMap-by-device-path or aliasing an unresolvable handle would not.
 *
 * @p fsn must already be a live filesystem mapping (e.g. after `map -r` /
 * @ref axl_ramdisk_create). @p alias may omit the trailing `:`. Both are
 * interpolated into a `map` command line, so they must be plain map tokens —
 * no whitespace, quotes, or redirection characters.
 *
 * @return AXL_OK if the `map` command was accepted; AXL_ERR on bad args or if
 *     the shell rejected it (e.g. the mapping mode doesn't expose a resolvable
 *     device path for @p fsn).
 */
AXL_WARN_UNUSED int
axl_volume_alias_to_fsn(
    const char *alias,   ///< new alias name to add, `:` optional
    const char *fsn      ///< existing fs mapping to alias (e.g. "fs1"), no `:`
);

/**
 * @brief Remove a UEFI Shell map name (SetMap with a NULL device path).
 *
 * Deletes @p name from the shell's global map. Use it to drop a mapping whose
 * backing device is going away — e.g. after destroying a RAM disk — so a later
 * `<name>:` doesn't resolve to a freed device path. @p name may omit the
 * trailing `:`. Removing a name that isn't mapped is reported by the shell as
 * an error (harmless for best-effort cleanup).
 *
 * @return AXL_OK on success; AXL_ERR on bad args / not-mapped / SetMap failure;
 *     AXL_UNSUPPORTED when there is no shell (SetMap unavailable).
 */
AXL_WARN_UNUSED int
axl_volume_unmap(
    const char *name           ///< mapping name to delete, `:` optional
);

/// Lowest `FS<n>` number axl_volume_pick_free_fsn() hands out. The shell's
/// `map -u` names each volume that has no name `FS<count++>` from a counter of
/// its own, which starts at the number of volumes named at boot, is reset only
/// by `map -r`, and never checks the map; SetMap then takes the name from
/// whatever held it. The lowest free number is exactly that counter's next
/// value. Numbers from this floor up stay clear of it until `map -u` has named
/// enough new volumes to get there, or at once on a system that boots with
/// this many volumes.
#define AXL_VOLUME_FSN_FLOOR  20

/// End (exclusive) of the axl_volume_pick_free_fsn() scan.
#define AXL_VOLUME_FSN_LIMIT  256

/**
 * @brief Pick a free `FS<n>` name for a volume a program names itself.
 *
 * Returns the lowest `FS<n>` the shell has not mapped, from
 * @ref AXL_VOLUME_FSN_FLOOR up to (not including) @ref AXL_VOLUME_FSN_LIMIT,
 * in the shell's own uppercase spelling (e.g. `"FS20"`, no `:`). Starting at
 * the floor, not at 0, keeps the name out of the numbers the shell's `map -u`
 * hands out (see @ref AXL_VOLUME_FSN_FLOOR). Assign it with
 * axl_volume_set_map(). Keeps no state: each call asks the shell's live map,
 * so a name freed with axl_volume_unmap() is picked again.
 *
 * With no shell every name reads as free, so this returns the floor and the
 * following axl_volume_set_map() reports AXL_UNSUPPORTED.
 *
 * @return AXL_OK with @p out set; AXL_ERR if every name in the range is
 *     taken, @p out is NULL, or @p out_size is too small for the name.
 */
AXL_WARN_UNUSED int
axl_volume_pick_free_fsn(
    char   *out,        ///< [out] receives the name, e.g. "FS20"
    size_t  out_size    ///< capacity of @p out in bytes (including NUL)
);

/**
 * @brief Is @p name one of the shell's names for @p device_path?
 *
 * Unlike axl_volume_map_taken(), which only says the name is in use, this
 * says whether it is in use by this device — e.g. to tell "the label is
 * already our alias" from "the label belongs to another volume".
 *
 * Works on the modern shell (GetDevicePathFromMap) and on the old EFI 1.x
 * shell (SHELL_ENVIRONMENT.GetMap), comparing the device paths byte for byte.
 *
 * @return true if the shell maps @p name to exactly @p device_path; false if
 *     not, on bad args, or with no shell at all.
 */
bool
axl_volume_map_is(
    const char *name,          ///< mapping name, `:` optional
    const void *device_path    ///< device path to compare against
);

/**
 * @brief Remove every shell name that maps to @p device_path.
 *
 * For a volume that is going away: drops the names its owner set, and any a
 * user or a `map -r` added since, so none of them is left pointing at a
 * device that no longer exists. Only names that currently map to
 * @p device_path are touched — a name that has since moved to another volume
 * stays with it (unlike axl_volume_unmap() on a remembered name).
 *
 * Names are taken whole from the shell's own list for the device, so a name of
 * any length or spelling is removed exactly as the shell stores it.
 *
 * @return AXL_OK when no name maps to @p device_path any more (including when
 *     none did, and with no shell at all); AXL_ERR when one still does — the
 *     shell refused to remove it, or has no programmatic unmap (the old EFI
 *     1.x shell, where only `map -r` clears it) — or @p device_path is NULL.
 */
AXL_WARN_UNUSED int
axl_volume_unmap_device(
    const void *device_path,   ///< device path whose names to remove
    size_t     *removed        ///< [out, optional] how many names were removed
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_FS_H */
