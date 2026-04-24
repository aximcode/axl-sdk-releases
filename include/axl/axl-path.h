/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-path.h:
 *
 * Path manipulation: basename, dirname, extension, join, resolve.
 * Handles both '/' (Unix) and '\\' (UEFI) path separators.
 * All allocated results are freed with axl_free().
 */

#ifndef AXL_PATH_H
#define AXL_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the filename portion of a path.
 *
 * Returns everything after the last separator ('/' or '\\').
 * Returns a copy of @p path itself if no separator is found.
 * Caller frees with axl_free(). NULL-safe.
 *
 * @return newly allocated string, or NULL if @p path is NULL or
 *     allocation fails.
 */
char *
axl_path_get_basename(
    const char *path  ///< file path, or NULL
);

/**
 * @brief Return the directory portion of a path.
 *
 * Returns everything before the last separator. Returns "." if no
 * separator is found. Caller frees with axl_free(). NULL-safe.
 *
 * @return newly allocated string, or NULL if @p path is NULL or
 *     allocation fails.
 */
char *
axl_path_get_dirname(
    const char *path  ///< file path, or NULL
);

/**
 * @brief Return the file extension from a path.
 *
 * Returns a pointer to the extension after the last dot in the
 * basename portion of @p path. Ignores leading dots (e.g. ".bashrc"
 * has no extension). NULL-safe.
 *
 * @return pointer into @p path (not allocated), or NULL if @p path is
 *     NULL or has no extension.
 */
const char *
axl_path_extension(
    const char *path  ///< file path, or NULL
);

/**
 * @brief Join a directory and filename with '/'.
 *
 * Handles a trailing separator on @p dir. Caller frees with axl_free().
 * NULL-safe: returns NULL if either argument is NULL.
 *
 * @return newly allocated path, or NULL on failure.
 */
char *
axl_path_join(
    const char *dir,  ///< directory path, or NULL
    const char *name  ///< filename to append, or NULL
);

/**
 * @brief Resolve a relative path against a base directory.
 *
 * Combines @p base and @p relative, normalizing "." and ".."
 * components. Both '/' and '\\' are recognized as separators.
 * If @p relative is absolute (starts with '/' or '\\'), @p base
 * is ignored. The output always uses '/' separators.
 *
 * @return 0 on success, -1 on error (NULL args, buffer too small,
 *     or ".." underflow past root).
 */
int
axl_path_resolve(
    const char *base,      ///< base directory path
    const char *relative,  ///< relative path to resolve
    char       *out,       ///< output buffer
    size_t      size       ///< output buffer size
);

/**
 * @brief Build a UEFI-style path with volume prefix.
 *
 * Writes "VOLUME:SUBPATH" into @p out, converting forward slashes to
 * backslashes. For example, axl_path_build_uefi("fs0", "/dir/file")
 * produces "fs0:\\dir\\file".
 *
 * @return 0 on success, -1 on error (NULL args or buffer too small).
 */
int
axl_path_build_uefi(
    const char *volume,   ///< volume name (e.g. "fs0")
    const char *subpath,  ///< subpath (forward slashes OK)
    char       *out,      ///< output buffer
    size_t      size      ///< output buffer size
);

// ---------------------------------------------------------------------------
// Working directory
// ---------------------------------------------------------------------------

/**
 * @brief Get the current working directory.
 *
 * Returns a UTF-8 copy. Caller frees with axl_free().
 *
 * @return current directory path, or NULL on error.
 */
char *
axl_get_current_dir(void);

/**
 * @brief Change the current working directory.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_chdir(
    const char *path  ///< directory path (UTF-8)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PATH_H */
