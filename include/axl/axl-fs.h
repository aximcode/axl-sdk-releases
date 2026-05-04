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
 * top of @ref axl_fopen — they're path-based shortcuts, not stream
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
// File metadata (stat)
// ---------------------------------------------------------------------------

/// File metadata (UEFI EFI_FILE_INFO equivalent).
typedef struct {
    uint64_t  size;         ///< file size in bytes
    uint64_t  alloc_size;   ///< physical allocation size on disk
    bool      is_dir;       ///< true if directory
    bool      read_only;    ///< true if read-only attribute set
} AxlFileInfo;

/**
 * @brief Get file metadata. Wraps UEFI EFI_FILE_INFO.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_file_info(
    const char  *path, ///< file path (UTF-8)
    AxlFileInfo *info  ///< [out] receives file metadata
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
 * @brief Rename or move a file.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_file_rename(
    const char *old_path,  ///< current path (UTF-8)
    const char *new_path   ///< new path (UTF-8)
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

/// Directory entry returned by axl_dir_read.
typedef struct {
    char      name[256];  ///< filename (UTF-8, not full path)
    uint64_t  size;       ///< file size in bytes (0 for directories)
    bool      is_dir;     ///< true if this entry is a directory
} AxlDirEntry;

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
 * @return true if an entry was read, false at end of directory.
 */
bool
axl_dir_read(
    AxlDir      *dir,   ///< directory handle
    AxlDirEntry *entry  ///< [out] receives entry
);

/**
 * @brief Close a directory handle. NULL-safe.
 */
void
axl_dir_close(
    AxlDir *dir  ///< directory handle
);

/**
 * @brief Per-entry callback for @ref axl_dir_walk.
 *
 * @param full_path  full path to the entry (root + separator + name)
 * @param entry      the AxlDirEntry, including name, size, is_dir
 * @param user       opaque user pointer passed through from caller
 *
 * Return codes:
 *   - 0  continue walking
 *   - >0 stop (propagated as the walk's return value)
 *   - <0 stop with error (propagated)
 */
typedef int (*AxlDirWalkFn)(
    const char         *full_path,
    const AxlDirEntry  *entry,
    void               *user
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
    const AxlDirEntry *entries,  ///< array of directory entries
    size_t             count,    ///< number of entries
    char              *buf,      ///< output buffer
    size_t             buf_size  ///< output buffer size
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
 * Use with handles from axl_service_enumerate("simple-fs", ...).
 * Returns a UTF-8 copy of the label. Caller frees with axl_free().
 *
 * @return label string, or NULL on error.
 */
char *
axl_volume_get_label_by_handle(
    void *handle  ///< filesystem handle from axl_service_enumerate
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
