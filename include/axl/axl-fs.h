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
    char   name[16];    ///< stable name ("fs0", "fs1", ...)
    void  *device_path; ///< opaque EFI_DEVICE_PATH_PROTOCOL — caller may
                        ///< pass to axl_device_path_find / _for_each.
                        ///< The pointer is firmware-owned; callers
                        ///< must not free it.
} AxlVolume;

/**
 * @brief Enumerate mounted filesystem volumes.
 *
 * Fills @p out with up to @p max descriptors, each with a stable
 * name ("fs0", "fs1", ...) and an opaque handle. On return, @p count
 * receives the number of entries filled.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_volume_enumerate(
    AxlVolume *out,    ///< output array (may be NULL to query count)
    size_t     max,    ///< capacity of @p out
    size_t    *count   ///< [out] number of volumes found
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_FS_H */
