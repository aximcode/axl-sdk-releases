/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-digest.h:
 *
 * Message digest checksums: MD5, SHA-1, SHA-256. Mirrors GLib's
 * GChecksum API. Standalone implementations with no external
 * dependencies (works without AXL_TLS=1).
 *
 * One-shot:
 * @code
 * char *hex = axl_compute_checksum(AXL_CHECKSUM_SHA1, data, len);
 * axl_printf("SHA-1: %s\n", hex);
 * axl_free(hex);
 * @endcode
 *
 * Incremental (streaming):
 * @code
 * AxlChecksum *cs = axl_checksum_new(AXL_CHECKSUM_SHA1);
 * axl_checksum_update(cs, part1, len1);
 * axl_checksum_update(cs, part2, len2);
 * const char *hex = axl_checksum_get_string(cs);
 * axl_checksum_free(cs);
 * @endcode
 */

#ifndef AXL_DIGEST_H
#define AXL_DIGEST_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Hash algorithm selector.
 */
typedef enum {
    AXL_CHECKSUM_MD5    = 0,  /**< MD5 (128-bit / 16-byte digest) */
    AXL_CHECKSUM_SHA1   = 1,  /**< SHA-1 (160-bit / 20-byte digest) */
    AXL_CHECKSUM_SHA256 = 2   /**< SHA-256 (256-bit / 32-byte digest) */
} AxlChecksumType;

typedef struct AxlChecksum AxlChecksum;

/**
 * @brief Get the digest length in bytes for the given algorithm.
 *
 * @return digest length, or 0 for unknown type.
 */
size_t
axl_checksum_type_get_length(
    AxlChecksumType type  ///< checksum algorithm
);

/**
 * @brief Create a new checksum context.
 *
 * @return new context, or NULL on failure.
 */
AxlChecksum *
axl_checksum_new(
    AxlChecksumType type  ///< checksum algorithm
);

/**
 * @brief Feed data into the checksum.
 *
 * Can be called multiple times. Must not be called after
 * axl_checksum_get_string() or axl_checksum_get_digest().
 */
void
axl_checksum_update(
    AxlChecksum *cs,    ///< checksum context
    const void  *data,  ///< input data
    size_t       len    ///< input length in bytes
);

/**
 * @brief Get the digest as a hex string.
 *
 * Returns a pointer to an internal NUL-terminated lowercase hex
 * string. The pointer is valid until axl_checksum_free(). After
 * this call, axl_checksum_update() must not be called.
 *
 * @return hex string (e.g. "a9993e36..."), or NULL on error.
 */
const char *
axl_checksum_get_string(
    AxlChecksum *cs  ///< checksum context
);

/**
 * @brief Get the raw digest bytes.
 *
 * Writes the binary digest into @p buf. On entry, @p *len is the
 * buffer size; on return, it is set to the digest length. After
 * this call, axl_checksum_update() must not be called.
 */
void
axl_checksum_get_digest(
    AxlChecksum *cs,   ///< checksum context
    uint8_t     *buf,  ///< output buffer
    size_t      *len   ///< [in/out] buffer size / digest length
);

/**
 * @brief Reset a checksum for reuse.
 *
 * Clears the state so axl_checksum_update() can be called again.
 */
void
axl_checksum_reset(
    AxlChecksum *cs  ///< checksum context
);

/**
 * @brief Free a checksum context. NULL-safe.
 */
void
axl_checksum_free(
    AxlChecksum *cs  ///< checksum context
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlChecksum, axl_checksum_free)
#endif

// ---------------------------------------------------------------------------
// Convenience one-shot functions
// ---------------------------------------------------------------------------

/**
 * @brief Compute a checksum and return it as a hex string.
 *
 * Convenience wrapper for new + update + get_string + free.
 *
 * @return newly allocated hex string (caller frees with axl_free),
 *     or NULL on error.
 */
char *
axl_compute_checksum(
    AxlChecksumType type,  ///< checksum algorithm
    const void     *data,  ///< input data
    size_t          len    ///< input length in bytes
);

/**
 * @brief Compute a checksum and write raw digest bytes.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_compute_checksum_digest(
    AxlChecksumType type,    ///< checksum algorithm
    const void     *data,    ///< input data
    size_t          len,     ///< input length in bytes
    uint8_t        *out,     ///< output buffer (must be large enough)
    size_t          out_len  ///< output buffer size
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DIGEST_H */
