/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-boot.h
    Boot-option management.

    Backend-neutral interface for the firmware boot manager. On UEFI
    this is backed by the spec EFI_LOAD_OPTION wire format under
    Boot####/BootOrder/BootNext/BootCurrent variables; the binary
    codec is internal to the implementation. Consumers operate on
    typed `AxlBootOption` structs only — raw LOAD_OPTION bytes never
    cross this boundary.

    Boot-option indices are 16-bit numbers (0x0000..0xFFFF) named
    Boot0000..BootFFFF in firmware-variable storage. BootOrder is
    the active boot ordering; BootNext is a one-shot override
    (consumed and cleared on next boot); BootCurrent is the index
    the system actually booted from this run.

    @code
    AxlBootOption opt;
    if (axl_boot_option_get(0x0001, &opt) == 0) {
        axl_printf("Boot0001: %s\n", opt.description);
        axl_boot_option_free(&opt);
    }

    uint16_t *order;
    size_t    n;
    if (axl_boot_order_get(&order, &n) == 0) {
        for (size_t i = 0; i < n; i++) {
            axl_printf("  %zu: Boot%04x\n", i, order[i]);
        }
        axl_free(order);
    }
    @endcode
**/

#ifndef AXL_BOOT_H
#define AXL_BOOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Attribute flags for AxlBootOption.attrs
// ---------------------------------------------------------------------------

/// Boot option is enabled for selection by the boot manager.
#define AXL_BOOT_ATTR_ACTIVE           0x00000001u

/// Boot manager should reconnect all controllers before loading.
#define AXL_BOOT_ATTR_FORCE_RECONNECT  0x00000002u

/// Boot option is hidden from interactive boot menus.
#define AXL_BOOT_ATTR_HIDDEN           0x00000008u

/// Mask covering the option-category field (boot vs application).
#define AXL_BOOT_ATTR_CATEGORY_MASK    0x00001f00u
/// Standard boot entry (default for OS-loader entries).
#define AXL_BOOT_ATTR_CATEGORY_BOOT    0x00000000u
/// Application entry (boot-time utility, not a normal OS boot path).
#define AXL_BOOT_ATTR_CATEGORY_APP     0x00000100u

// ---------------------------------------------------------------------------
// Typed boot-option record
// ---------------------------------------------------------------------------

/**
 * @brief Decoded boot option.
 *
 * Heap-allocated string fields are owned by the struct after a
 * successful `axl_boot_option_get` and freed by
 * `axl_boot_option_free`. Pass a struct that's been zero-initialised
 * if you populate it manually for `_set`; the encoder accepts NULL
 * fields and writes empty equivalents to the variable.
 */
typedef struct {
    uint16_t  index;            ///< Boot#### index (0..0xFFFF)
    uint32_t  attrs;            ///< AXL_BOOT_ATTR_* flags
    char     *description;     ///< UTF-8 description, NULL allowed
    char     *device_path;      ///< UTF-8 device-path text, NULL if firmware lacks DevicePathToText
    void     *opt_data;         ///< OS-loader options (opaque to AXL), NULL allowed
    size_t    opt_data_len;     ///< bytes in @c opt_data (0 if no opt data)
} AxlBootOption;

/**
 * @brief Free heap-owned fields inside an AxlBootOption.
 *
 * Resets the struct to zeroed state. NULL-safe. Doesn't free the
 * struct itself — callers stack-allocate it.
 */
void
axl_boot_option_free(
    AxlBootOption  *opt   ///< option to free, or NULL
);

/**
 * @brief Read a single Boot#### option from firmware.
 *
 * Populates @p out with description, device-path text (if obtainable)
 * and any opt_data. The caller frees with axl_boot_option_free().
 *
 * @return 0 on success, -1 if the variable is missing or malformed.
 */
int
axl_boot_option_get(
    uint16_t        index,   ///< Boot#### index
    AxlBootOption  *out      ///< [out] populated on success
);

/**
 * @brief Write a Boot#### option to firmware.
 *
 * Encodes @p opt as the firmware's wire format and stores it under
 * Boot####. Requires the firmware to expose a text→device-path
 * service (DevicePathFromText on UEFI); on backends without one,
 * returns -1.
 *
 * @return 0 on success, -1 on encode or write failure.
 */
int
axl_boot_option_set(
    uint16_t              index,   ///< Boot#### index
    const AxlBootOption  *opt      ///< option to encode and write
);

/**
 * @brief Delete a Boot#### option.
 *
 * @return 0 on success (or if the variable already absent), -1 on error.
 */
int
axl_boot_option_delete(
    uint16_t  index   ///< Boot#### index
);

// ---------------------------------------------------------------------------
// BootOrder
// ---------------------------------------------------------------------------

/**
 * @brief Read the BootOrder variable.
 *
 * Allocates an array of Boot#### indices in firmware order. Caller
 * frees @c *out with axl_free().
 *
 * @return 0 on success, -1 if BootOrder is absent or malformed.
 */
int
axl_boot_order_get(
    uint16_t  **out,    ///< [out] caller-freed array of indices
    size_t     *count   ///< [out] number of entries in *out
);

/**
 * @brief Write the BootOrder variable.
 *
 * @return 0 on success, -1 on write failure.
 */
int
axl_boot_order_set(
    const uint16_t  *order,   ///< new boot order
    size_t           count    ///< entry count (0 deletes BootOrder)
);

// ---------------------------------------------------------------------------
// BootNext (one-shot) and BootCurrent
// ---------------------------------------------------------------------------

/**
 * @brief Read the BootNext one-shot override.
 *
 * @return 0 on success, -1 if BootNext is unset.
 */
int
axl_boot_next_get(
    uint16_t  *out   ///< [out] receives the Boot#### index
);

/**
 * @brief Set the BootNext one-shot override.
 *
 * Firmware will boot @p index on the next reboot, then automatically
 * delete BootNext.
 *
 * @return 0 on success, -1 on write failure.
 */
int
axl_boot_next_set(
    uint16_t  index   ///< Boot#### index to boot next
);

/**
 * @brief Clear the BootNext one-shot override.
 *
 * @return 0 on success (or if BootNext already absent), -1 on error.
 */
int
axl_boot_next_clear(
    void
);

/**
 * @brief Read the BootCurrent variable.
 *
 * BootCurrent is set by firmware to the Boot#### index that the
 * current boot used. Useful for "which entry got us here" reporting.
 *
 * @return 0 on success, -1 if BootCurrent is absent.
 */
int
axl_boot_current_get(
    uint16_t  *out   ///< [out] receives the Boot#### index
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_BOOT_H */
