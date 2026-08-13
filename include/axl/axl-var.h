/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-var.h
 *
 * Raw UEFI variable inspection — read-only, across every vendor GUID.
 *
 * This is the *unscoped* view of firmware variables, and it is a
 * different operation from axl-nvstore.h, not a convenience over it:
 *
 *   - axl_nvstore_iter() walks ONE namespace. The vendor GUID is an
 *     INPUT, and the namespace must have been registered first.
 *   - axl_var_enumerate() walks EVERY variable on the box. The vendor
 *     GUID is an OUTPUT, discovered during the walk.
 *
 * Reach for axl-nvstore.h to read or write your own settings by name.
 * Reach for this header to inventory what a machine actually carries —
 * boot order, Secure Boot state, OEM configuration — without knowing
 * the GUIDs in advance.
 *
 * READ-ONLY BY DESIGN. There is deliberately no SetVariable here. An
 * unrestricted variable write reachable from a network service is a
 * brick-the-box primitive; scoped writes belong in axl-nvstore.h, where
 * a namespace has to be registered first. If an unscoped write is ever
 * added it will be a separate, loudly-documented entry point rather
 * than a flag on these.
 *
 * @code
 * AxlVarInfo *vars;
 * size_t count;
 * if (axl_var_enumerate(&vars, &count) == AXL_OK) {
 *     for (size_t i = 0; i < count; i++) {
 *         axl_printf("%s (%zu bytes)\n", vars[i].name, vars[i].size);
 *     }
 *     axl_free(vars);              // one free covers the names too
 * }
 * @endcode
 */

#ifndef AXL_VAR_H
#define AXL_VAR_H

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-sys.h>
#include <axl/axl-nvstore.h>   /* AxlVarInfo.attrs carries the AXL_NV_* bits */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Inspection-only variable attributes
 *
 * Attributes a variable can carry that the portable AXL_NV_* set does
 * not model, because axl-nvstore.h is a portable key/value store and
 * these have no meaning off UEFI. They matter here: an inventory that
 * cannot tell an authenticated variable from an ordinary one cannot
 * describe Secure Boot state, and PK / KEK / db / dbx all carry
 * @ref AXL_VAR_TIME_AUTH_WRITE.
 *
 * The values deliberately mirror the EFI_VARIABLE_* bit positions, so
 * an attrs word can be read against either vocabulary.
 * @{
 */
#define AXL_VAR_HW_ERROR_RECORD  0x08  ///< hardware error record variable
#define AXL_VAR_TIME_AUTH_WRITE  0x20  ///< time-based authenticated write required
#define AXL_VAR_APPEND_WRITE     0x40  ///< writes append rather than replace
#define AXL_VAR_ENHANCED_AUTH    0x80  ///< enhanced authenticated access
/** @} */

/**
 * @brief One UEFI variable, described without reading its payload.
 *
 * @a name points into the same allocation as the array itself, so the
 * whole result is released by a single axl_free() on the array — never
 * free @a name separately.
 */
typedef struct {
    const char *name;    ///< variable name, UTF-8, NUL-terminated
    AxlGuid     vendor;  ///< vendor GUID, discovered during the walk
    uint32_t    attrs;   ///< AXL_NV_* flags, plus any AXL_VAR_* above
    size_t      size;    ///< payload size in bytes; the payload is NOT read
} AxlVarInfo;

/**
 * @brief Enumerate every UEFI variable, across all vendor GUIDs.
 *
 * Walks the firmware's whole variable store. For each variable this
 * reports the name, the vendor GUID it was found under, its attributes
 * and its payload SIZE — but never reads the payload itself. That is a
 * contract, not an optimization: a machine can carry megabytes of
 * variable data, and an inventory listing must not pull it into memory.
 * Use axl_var_read() for the one variable you actually want.
 *
 * The result is a single allocation: the @ref AxlVarInfo array with the
 * name strings packed behind it. Release it with one axl_free(*vars).
 * On failure @p vars is set to NULL and @p count to 0, so a caller that
 * frees unconditionally is safe.
 *
 * A machine with no variables at all is @ref AXL_OK with @p count 0 and
 * @p vars NULL — an empty store is not an error.
 *
 * @return AXL_OK on success; AXL_INVALID if @p vars or @p count is NULL;
 *     AXL_NO_RESOURCES if the result could not be allocated; AXL_ERR if
 *     the firmware walk failed.
 */
int
axl_var_enumerate(
    AxlVarInfo **vars,   ///< [out] receives the array (non-NULL)
    size_t      *count   ///< [out] receives the element count (non-NULL)
);

/**
 * @brief Read one variable's payload by name and vendor GUID.
 *
 * @p attrs and @p data are independently optional — pass NULL for
 * either to skip it. Passing NULL for @p data reads the attributes and
 * size without transferring the payload, which is the cheap way to
 * size a variable before deciding to read it.
 *
 * When a payload IS returned it is allocated with one extra zero byte
 * past @p size, so a variable holding text can be used as a C string
 * without copying. That extra byte is not counted in @p size.
 *
 * Out parameters are written only when this returns AXL_OK. On any
 * failure they are left cleared, so a populated @p size never appears
 * beside a NULL @p data.
 *
 * @return AXL_OK on success; AXL_INVALID if @p name is NULL or empty,
 *     if @p vendor is NULL, or if @p data is non-NULL while @p size is
 *     NULL; AXL_NOT_FOUND if no such variable exists under that GUID;
 *     AXL_NO_RESOURCES if the payload could not be allocated;
 *     AXL_ERR if the firmware read failed.
 */
int
axl_var_read(
    const char     *name,   ///< variable name, UTF-8, non-empty (non-NULL)
    const AxlGuid  *vendor, ///< vendor GUID to look under (non-NULL)
    uint32_t       *attrs,  ///< [out] AXL_NV_* / AXL_VAR_* flags, or NULL to skip
    void          **data,   ///< [out] payload, caller axl_free()s, or NULL to skip
    size_t         *size    ///< [out] payload size; required when @a data is non-NULL
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_VAR_H */
