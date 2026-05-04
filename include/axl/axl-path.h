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
#include <axl/axl-macros.h>

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
 * @return AXL_OK on success, AXL_ERR on error (NULL args, buffer too small,
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
 * @brief Build a path to a file alongside another (the "companion"
 *     pattern: load `jedec.json5` from the directory holding the
 *     binary that just started running, etc.).
 *
 * Equivalent to @ref axl_path_join with the dirname of @p anchor as
 * the directory portion. Returns just @p name (joined with "."/empty)
 * if @p anchor has no directory component. NULL-safe.
 *
 * Caller frees with @ref axl_free.
 *
 * @code
 * char *cfg = axl_path_companion(axl_app_argv0(), "jedec.json5");
 * // load cfg if non-NULL
 * @endcode
 *
 * @return newly allocated path, or NULL if either argument is NULL or
 *     allocation fails.
 */
char *
axl_path_companion(
    const char *anchor,  ///< reference path (e.g. argv[0])
    const char *name     ///< filename to place beside @p anchor
);

/**
 * @brief Resolve a sidecar data file by trying override → companion → cwd.
 *
 * Standard lookup order for tools that ship optional sidecar JSON
 * (jedec ID DB, vendor lists, board configs, etc.):
 *
 *   1. If @p override_path is non-NULL and the file exists, use it.
 *   2. Companion path beside the running binary. Two anchors are
 *      tried in order:
 *        a. @ref axl_app_image_path — canonical FILEPATH from
 *           EFI_LOADED_IMAGE_PROTOCOL. Reliable regardless of how
 *           the shell invoked the binary (basename vs full path,
 *           cwd-rooted vs absolute).
 *        b. @ref axl_app_argv0 — the shell-supplied invocation
 *           string. Fallback for synthetic load contexts where the
 *           image path is unavailable.
 *   3. @p name as-is (current working directory).
 *
 * Returns the first path that exists. Caller must axl_free() the
 * returned string. The check uses @ref axl_file_info, so the file
 * must be readable at lookup time.
 *
 * @code
 * char *p = axl_resolve_data_file(
 *     axl_args_get_string(a, "jedec-file"),  // CLI override (may be NULL)
 *     "jedec.json5"
 *     );
 * if (p != NULL) {
 *     load_table(p);
 *     axl_free(p);
 * }
 * @endcode
 *
 * @return newly allocated path, or NULL if no candidate exists.
 */
char *
axl_resolve_data_file(
    const char *override_path,  ///< optional explicit path (may be NULL)
    const char *name            ///< filename to resolve
);

/**
 * @brief Build a UEFI-style path with volume prefix.
 *
 * Writes "VOLUME:SUBPATH" into @p out, converting forward slashes to
 * backslashes. For example, axl_path_build_uefi("fs0", "/dir/file")
 * produces "fs0:\\dir\\file".
 *
 * @return AXL_OK on success, AXL_ERR on error (NULL args or buffer too small).
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
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_chdir(
    const char *path  ///< directory path (UTF-8)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PATH_H */
