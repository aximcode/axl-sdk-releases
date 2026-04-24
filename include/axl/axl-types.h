/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-types.h:
 *
 * Common callback typedefs shared across AXL data structure headers.
 * Equivalent to GLib's gtypes.h callback definitions.
 */

#ifndef AXL_TYPES_H
#define AXL_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generic per-element callback (GFunc equivalent).
 */
typedef void (*AxlFunc)(
    void *data,      ///< element data
    void *user_data  ///< caller-provided context
);

/**
 * @brief Callback to free element data (GDestroyNotify equivalent).
 */
typedef void (*AxlDestroyNotify)(
    void *data  ///< element data to free
);

/**
 * @brief Comparison function (qsort-compatible, GCompareFunc equivalent).
 *
 * @return < 0 if a < b, 0 if equal, > 0 if a > b.
 */
typedef int (*AxlCompareFunc)(
    const void *a, ///< pointer to first element
    const void *b  ///< pointer to second element
);

/**
 * @brief Context-aware comparison function (GCompareDataFunc equivalent).
 *
 * @return < 0 if a < b, 0 if equal, > 0 if a > b.
 */
typedef int (*AxlCompareDataFunc)(
    const void *a,        ///< pointer to first element
    const void *b,        ///< pointer to second element
    void       *user_data ///< caller-supplied context
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_TYPES_H */
