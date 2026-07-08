/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-fs.h:
 *
 * Filesystem operations — path-based file and directory APIs,
 * volume enumeration, file metadata. Mirrors the POSIX split:
 * `<axl/axl-stream.h>` is the `<stdio.h>` analog (FILE * / streams);
 * this header is the `<sys/stat.h>` + `<dirent.h>` + `<sys/statvfs.h>`
 * analog.
 *
 * All paths are UTF-8; backend converts to UCS-2 internally.
 * High-level convenience wrappers (`axl_file_get_contents`) layer on
 * top of axl_fopen — they're path-based shortcuts, not stream
 * primitives.
 */

#ifndef AXL_FS_H
#define AXL_FS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-bytes.h>

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
 * Writes @p buf to a temporary sibling ("<path>.tmp"), flushes it
 * (close implies flush on the UEFI FAT driver), then replaces @p path
 * with it via rename. Unlike axl_file_set_contents — which truncates
 * the target and writes in place, so a power loss mid-write leaves the
 * target half-written — this never modifies the target until the full,
 * flushed contents exist in the temp file.
 *
 * On UEFI/FAT a rename cannot atomically replace an existing file, so
 * when @p path already exists the replace is delete-then-rename: a power
 * loss in that window leaves the target momentarily absent but the
 * complete data still present in "<path>.tmp" (recoverable) — never a
 * half-written target. The temp file is removed on any failure.
 *
 * @p path must be in a writable directory; the temp sibling is created
 * in the same directory (same-directory rename is the FAT atomic case).
 *
 * @return AXL_OK on success, AXL_ERR on any failure (the target is left
 *     untouched if the temp write fails).
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
 * mid-write leaves it partially written. Out-of-core streaming
 * precludes the temp-file-then-rename trick — a caller that needs
 * all-or-nothing must write to a temp path and rename on close itself.
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
 * Closing flushes outstanding data to the underlying volume (UEFI FAT
 * close implies flush). The writer is freed regardless of the flush
 * result, so a PUT handler that needs durability MUST check this return
 * (a failed flush means report 5xx, not 201). There is deliberately no
 * AXL_AUTOPTR cleanup binding for AxlFileWriter: an implicit close would
 * discard this load-bearing flush status. C++ callers close explicitly.
 *
 * @return AXL_OK on success (or @p w == NULL), AXL_ERR if the final
 *     flush/close failed or the writer was already in a failed state.
 */
int
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
typedef void (*AxlProgressFunc)(uint64_t done, uint64_t total, void *ctx);

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

/// Current `AxlFsEntry.version` value emitted by the SDK. Bumped
/// when the struct gains a new field. Forward-compat: callers test
/// `entry.struct_size >= offsetof(AxlFsEntry, new_field) +
/// sizeof(new_field)` before reading anything added in version > 1.
#define AXL_FS_ENTRY_VERSION  1

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
 */
typedef struct {
    uint32_t struct_size;     ///< sizeof(AxlFsEntry) at write time
    uint32_t version;         ///< AXL_FS_ENTRY_VERSION at write time
    char     name[256];       ///< basename UTF-8; empty for path-stat / root
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
 * `entry->name` is populated with the basename; the rest of the
 * fields hold the file's stat data.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_file_info(
    const char  *path,  ///< file path (UTF-8)
    AxlFsEntry  *entry  ///< [out] receives file metadata
);

// ---------------------------------------------------------------------------
// File operations (path-based)
// ---------------------------------------------------------------------------

/**
 * @brief Delete a file.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_file_delete(
    const char *path  ///< file path (UTF-8)
);

/**
 * @brief Rename a file within its current directory.
 *
 * @p new_path may be a basename only ("bar.txt") or a full path
 * with the same directory prefix as @p old_path ("fs0:\\dir\\bar.txt"
 * given "fs0:\\dir\\foo.txt"). Cross-directory renames are refused
 * with AXL_ERR — most UEFI FAT drivers can't move a file across
 * directories via SetFileInfo. Use axl_file_move for cross-directory
 * cases (it falls back to copy + delete).
 *
 * @return AXL_OK on success, AXL_ERR on cross-directory request,
 *     missing source, or backend failure.
 */
int
axl_file_rename(
    const char *old_path,  ///< current path (UTF-8)
    const char *new_path   ///< new path or basename (UTF-8); same dir as @p old_path
);

/**
 * @brief Move a file. Same-directory case is an atomic rename;
 *     cross-directory falls back to copy + delete.
 *
 * Tries axl_file_rename first (atomic on FAT for same-directory).
 * On refusal — typically because @p new_path's directory differs
 * from @p old_path's — falls back to chunked stream copy followed
 * by source delete.
 *
 * Overwrite semantics: an existing file at @p new_path is replaced
 * (matches POSIX `rename(2)`). The implementation removes
 * @p new_path eagerly before attempting the rename / copy — this
 * is NOT atomic. If the subsequent move then fails, the prior
 * destination is gone; the source remains for retry. Callers that
 * want "fail-if-exists" must probe with axl_file_info first.
 *
 * Failure modes (the fallback is NOT atomic — no rollback):
 *
 *   - Copy fails mid-stream → partial destination file exists;
 *     source is untouched. Caller can retry or clean up @p new_path.
 *   - Copy succeeds but delete fails → both files exist. Caller
 *     can retry the delete.
 *
 * Callers needing atomicity across directories must orchestrate
 * temp-file + rename themselves at a higher layer.
 *
 * @return AXL_OK on success; AXL_ERR if either source missing,
 *     destination unwritable, copy fails, or delete fails.
 */
int
axl_file_move(
    const char *old_path,  ///< current path (UTF-8)
    const char *new_path   ///< new path (UTF-8); may be in a different directory
);

/**
 * @brief Create a directory.
 *
 * @return AXL_OK on success, AXL_ERR on error (including if it already exists).
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
);

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
 * @return 0 on a clean traversal, the callback's non-zero return
 *     value if it stopped the walk, or -1 if @p root could not be
 *     opened or arguments are invalid.
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
 * Returns a UTF-8 copy of the label. Caller frees with axl_free().
 *
 * @return label string, or NULL on error.
 */
char *
axl_volume_get_label(
    const char *path  ///< filesystem path (e.g., "fs0:", "fs1:\\")
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
 * and writes the lowercased `fsN` alias (no trailing `:`) to @p out. This is
 * the shell's *actual* mapping — unlike axl_volume_enumerate()'s `.name`,
 * it never falls back to a synthesized LocateHandle index. Use it when you
 * must publish a caller-usable `fsN` (e.g. a freshly created RAM disk): a real
 * alias, or a clean failure you can report — never a plausible-but-wrong index.
 *
 * @return AXL_OK with @p out set to the alias; AXL_ERR if @p device_path has no
 *     shell mapping (not yet remapped, or no fs alias), or an argument is
 *     invalid; AXL_UNSUPPORTED if the shell doesn't publish GetMapFromDevicePath.
 *     @p out is left unchanged on any non-AXL_OK return.
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
 * @return AXL_OK with @p out set to the alias; AXL_ERR if @p device_path has
 *     no shell mapping or an argument is invalid; AXL_UNSUPPORTED if the shell
 *     doesn't publish GetMapFromDevicePath. @p out is left unchanged on any
 *     non-AXL_OK return.
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
 * Pair with axl_volume_map_taken() to avoid clobbering an existing name.
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

#ifdef __cplusplus
}
#endif

#endif /* AXL_FS_H */
