/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-hexdump.h:
 *
 * Formatted hex+ASCII dump with configurable grouping.
 * Supports direct console output and log integration.
 */

#ifndef AXL_HEXDUMP_H
#define AXL_HEXDUMP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AXL_HEX_GROUP_BYTE   1
#define AXL_HEX_GROUP_WORD   2
#define AXL_HEX_GROUP_DWORD  4
#define AXL_HEX_GROUP_QWORD  8

#define AXL_HEXDUMP_MAX_SIZE  (64 * 1024)

/**
 * @brief Print a hex+ASCII dump to stdout via axl_print().
 */
void
axl_hexdump(
    const char *name,           ///< label printed above the dump (may be NULL)
    const void *data,           ///< buffer to dump
    size_t      size,           ///< number of bytes
    size_t      bytes_per_line, ///< columns (0 = default 16, max 64)
    size_t      group_size      ///< grouping width (AXL_HEX_GROUP_*)
);

/**
 * @brief Emit a hex+ASCII dump through axl_log_full().
 */
void
axl_hexdump_to_log(
    int         level,          ///< log level (AXL_LOG_ERROR..AXL_LOG_TRACE)
    const char *domain,         ///< log domain
    const char *func,           ///< __func__
    int         line,           ///< __LINE__
    const char *name,           ///< label printed above the dump (may be NULL)
    const void *data,           ///< buffer to dump
    size_t      size,           ///< number of bytes
    size_t      bytes_per_line, ///< columns (0 = default 16, max 64)
    size_t      group_size      ///< grouping width (AXL_HEX_GROUP_*)
);

/**
 * @brief Convenience macro that injects _AxlLogDomain, __func__, __LINE__.
 *
 * Requires AXL_LOG_DOMAIN() in the source file.
 */
#define axl_hexdump_log(Level, Name, Data, Size, BytesPerLine, GroupSize)  \
    axl_hexdump_to_log((Level), _AxlLogDomain, __func__, __LINE__,      \
                       (Name), (Data), (Size), (BytesPerLine), (GroupSize))

#ifdef __cplusplus
}
#endif

#endif /* AXL_HEXDUMP_H */
