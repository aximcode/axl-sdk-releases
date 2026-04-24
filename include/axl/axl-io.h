/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-io.h:
 *
 * Stream-based I/O with three layers:
 *   1. Simple helpers (GLib-style): axl_print, axl_file_get_contents
 *   2. Stream I/O (POSIX-style): axl_fopen, axl_fread, axl_fprintf
 *   3. Low-level: axl_read, axl_write, axl_pread, axl_pwrite
 *
 * All strings are UTF-8. Paths are converted to UCS-2 internally.
 */

#ifndef AXL_IO_H
#define AXL_IO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlStream AxlStream;
typedef long long axl_ssize_t;

// ---------------------------------------------------------------------------
// Standard streams (call axl_io_init before use)
// ---------------------------------------------------------------------------

extern AxlStream *axl_stdout;
extern AxlStream *axl_stderr;

/**
 * @brief Initialize the I/O subsystem.
 *
 * Sets up axl_stdout and axl_stderr.
 * Call once at startup (before any axl_print/axl_fprintf).
 */
void
axl_io_init(void);

// ---------------------------------------------------------------------------
// Layer 1: Simple helpers (GLib-style)
// ---------------------------------------------------------------------------

/**
 * @brief Print to stdout. Like g_print().
 *
 * @return number of bytes written, or -1 on error.
 */
int
axl_print(
    const char *fmt,  ///< printf-style format string
    ...
) __attribute__((format(printf, 1, 2)));

/**
 * @brief Alias for axl_print. Matches the design-doc name.
 */
#define axl_printf axl_print

/**
 * @brief Print to stderr. Like g_printerr().
 *
 * @return number of bytes written, or -1 on error.
 */
int
axl_printerr(
    const char *fmt,  ///< printf-style format string
    ...
) __attribute__((format(printf, 1, 2)));

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
 * @return 0 on success, -1 on error.
 */
int
axl_file_info(
    const char  *path, ///< file path (UTF-8)
    AxlFileInfo *info  ///< [out] receives file metadata
);

// ---------------------------------------------------------------------------
// Layer 2: Stream I/O (POSIX fopen-style)
// ---------------------------------------------------------------------------

/**
 * @brief Open a file stream.
 *
 * Path is converted to UCS-2 internally.
 *
 * @return stream, or NULL on error. Close with axl_fclose().
 */
AxlStream *
axl_fopen(
    const char *path,  ///< file path (UTF-8, e.g. "fs0:/data.txt")
    const char *mode   ///< "r" (read), "w" (write/create), "a" (append)
);

/**
 * @brief Close a stream and free resources. NULL-safe.
 */
void
axl_fclose(
    AxlStream *s  ///< stream, or NULL
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlStream, axl_fclose)
#endif

/**
 * @brief Read size*count bytes from stream.
 *
 * Returns number of complete items read (may be less than count at
 * EOF or on error). Returns 0 on both EOF and error -- use axl_read()
 * if you need to distinguish them (-1 = error, 0 = EOF).
 */
size_t
axl_fread(
    void      *buf,    ///< destination buffer
    size_t     size,   ///< item size in bytes
    size_t     count,  ///< number of items
    AxlStream *s       ///< stream
);

/**
 * @brief Write size*count bytes to stream.
 *
 * Returns number of complete items written.
 */
size_t
axl_fwrite(
    const void *buf,    ///< source buffer
    size_t      size,   ///< item size in bytes
    size_t      count,  ///< number of items
    AxlStream  *s       ///< stream
);

/**
 * @brief Write formatted text to a stream.
 *
 * @return number of bytes written, or -1 on error.
 */
int
axl_fprintf(
    AxlStream  *s,    ///< stream
    const char *fmt,  ///< printf-style format string
    ...
) __attribute__((format(printf, 2, 3)));

/**
 * @brief Read one line (up to and including '\\n').
 *
 * Caller frees with axl_free(). Returns NULL at EOF or on error.
 */
char *
axl_readline(
    AxlStream *s  ///< stream
);

// ---------------------------------------------------------------------------
// Stream positioning
// ---------------------------------------------------------------------------

#define AXL_SEEK_SET  0  ///< seek from beginning
#define AXL_SEEK_CUR  1  ///< seek from current position
#define AXL_SEEK_END  2  ///< seek from end of file

/**
 * @brief Set the stream position.
 *
 * @return 0 on success, -1 on error or if not supported.
 */
int
axl_fseek(
    AxlStream *s,      ///< stream
    int64_t    offset,  ///< byte offset (may be negative for CUR/END)
    int        whence   ///< AXL_SEEK_SET, AXL_SEEK_CUR, or AXL_SEEK_END
);

/**
 * @brief Get the current stream position.
 *
 * @return position in bytes, or -1 on error.
 */
int64_t
axl_ftell(
    AxlStream *s  ///< stream
);

/**
 * @brief Check if the stream has reached end-of-file.
 *
 * Set when read returns 0 bytes. Cleared by axl_fseek.
 *
 * @return true if at EOF.
 */
bool
axl_feof(
    AxlStream *s  ///< stream
);

/**
 * @brief Flush pending writes to the underlying file. NULL-safe.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_fflush(
    AxlStream *s  ///< stream
);

// ---------------------------------------------------------------------------
// Buffer streams (in-memory, auto-growing)
// ---------------------------------------------------------------------------

/**
 * @brief Create an in-memory buffer stream.
 *
 * Supports read, write, pread, pwrite.
 *
 * @return stream, or NULL on allocation failure.
 */
AxlStream *
axl_bufopen(void);

/**
 * @brief Peek at buffer contents without consuming.
 *
 * The returned pointer is owned by the stream and invalidated
 * by writes or close.
 */
const void *
axl_bufdata(
    AxlStream *s,     ///< buffer stream
    size_t    *size   ///< (out, optional): buffer size
);

/**
 * @brief Transfer ownership of buffer to caller.
 *
 * Stream becomes empty. Caller frees with axl_free().
 */
void *
axl_bufsteal(
    AxlStream *s,     ///< buffer stream
    size_t    *size   ///< (out, optional): buffer size
);

// ---------------------------------------------------------------------------
// Layer 3: Low-level read/write/pread/pwrite
// ---------------------------------------------------------------------------

/**
 * @brief Read up to @a count bytes from stream at current position.
 *
 * @return bytes read, 0 at EOF, -1 on error.
 */
axl_ssize_t
axl_read(
    AxlStream *s,      ///< stream
    void      *buf,    ///< destination buffer
    size_t     count   ///< max bytes to read
);

/**
 * @brief Write @a count bytes to stream at current position.
 *
 * @return bytes written, -1 on error.
 */
axl_ssize_t
axl_write(
    AxlStream  *s,     ///< stream
    const void *buf,   ///< source buffer
    size_t      count  ///< bytes to write
);

/**
 * @brief Read up to @a count bytes at @a offset without changing stream position.
 *
 * @return bytes read, -1 on error or if not supported.
 */
axl_ssize_t
axl_pread(
    AxlStream *s,       ///< stream
    void      *buf,     ///< destination buffer
    size_t     count,   ///< max bytes to read
    size_t     offset   ///< byte offset to read from
);

/**
 * @brief Write @a count bytes at @a offset without changing stream position.
 *
 * @return bytes written, -1 on error or if not supported.
 */
axl_ssize_t
axl_pwrite(
    AxlStream  *s,       ///< stream
    const void *buf,     ///< source buffer
    size_t      count,   ///< bytes to write
    size_t      offset   ///< byte offset to write at
);

// ---------------------------------------------------------------------------
// File operations (path-based)
// ---------------------------------------------------------------------------

/**
 * @brief Delete a file.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_file_delete(
    const char *path  ///< file path (UTF-8)
);

/**
 * @brief Rename or move a file.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_file_rename(
    const char *old_path,  ///< current path (UTF-8)
    const char *new_path   ///< new path (UTF-8)
);

/**
 * @brief Create a directory.
 *
 * @return 0 on success, -1 on error (including if it already exists).
 */
int
axl_dir_mkdir(
    const char *path  ///< directory path (UTF-8)
);

/**
 * @brief Remove an empty directory.
 *
 * @return 0 on success, -1 on error (including if not empty).
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
 * @brief Serialize directory entries to a JSON array.
 *
 * Writes a JSON array of objects into @p buf. Each object has:
 * "name" (string), "size" (uint64), "dir" (boolean).
 *
 * Example output: [{"name":"foo.txt","size":1024,"dir":false}]
 *
 * @return 0 on success, -1 on error or buffer overflow.
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
    void  *handle;    ///< opaque filesystem handle
    char   name[16];  ///< stable name ("fs0", "fs1", ...)
} AxlVolume;

/**
 * @brief Enumerate mounted filesystem volumes.
 *
 * Fills @p out with up to @p max descriptors, each with a stable
 * name ("fs0", "fs1", ...) and an opaque handle. On return, @p count
 * receives the number of entries filled.
 *
 * @return 0 on success, -1 on error.
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

#endif /* AXL_IO_H */
