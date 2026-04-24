/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-env.h:
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
 * @return 0 on success, -1 on error.
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
 * @return 0 on success, -1 on error.
 */
int
axl_unsetenv(
    const char *name  ///< variable name (UTF-8)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ENV_H */
