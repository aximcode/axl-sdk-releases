/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-env.h
 *
 * Shell environment variable access.
 *
 * @code
 * char *home = axl_getenv("path");
 * if (home != NULL) {
 *     axl_printf("path=%s\n", home);
 *     axl_free(home);
 * }
 * axl_setenv("myvar", "hello", true);
 * @endcode
 */

#ifndef AXL_ENV_H
#define AXL_ENV_H

#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get a shell environment variable.
 *
 * Returns a UTF-8 copy of the variable's value.
 * Caller frees with axl_free().
 *
 * @return value string, or NULL if not found.
 */
char *
axl_getenv(
    const char *name  ///< variable name (UTF-8)
);

/**
 * @brief Set a shell environment variable.
 *
 * On the old EFI 1.x shell (no programmatic SetEnv) this drives the shell's own
 * `set` command via Execute, which imposes limits the modern shell does not:
 * @p name must be a bare identifier (`[A-Za-z0-9_]`), and @p value cannot
 * contain `"`, `%`, or a newline (they would break or inject the command) — such
 * a value is refused with AXL_ERR rather than set incorrectly. Values with
 * spaces are fine.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_setenv(
    const char *name,      ///< variable name (UTF-8)
    const char *value,     ///< value (UTF-8)
    bool        overwrite  ///< if false, don't replace existing value
);

/**
 * @brief Remove a shell environment variable.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_unsetenv(
    const char *name  ///< variable name (UTF-8)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ENV_H */
