/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-nvstore.h:
 *
 * Platform-agnostic non-volatile key-value storage.
 *
 * Provides persistent storage that survives reboots. On UEFI, this
 * maps to firmware variables (GetVariable/SetVariable). Variables
 * are organized by namespace.
 *
 * Well-known namespaces:
 *   "global"  — standard firmware variables (e.g., SecureBoot, BootOrder)
 *   "app"     — application-specific persistent settings
 *
 * @code
 * uint8_t secure_boot;
 * size_t sz = sizeof(secure_boot);
 * if (axl_nvstore_get("global", "SecureBoot", &secure_boot, &sz) == 0) {
 *     axl_printf("SecureBoot: %s\n", secure_boot ? "enabled" : "disabled");
 * }
 * @endcode
 */

#ifndef AXL_NVSTORE_H
#define AXL_NVSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AXL_NV_VOLATILE    0x00  ///< lost on reboot
#define AXL_NV_PERSISTENT  0x01  ///< survives reboot (non-volatile)
#define AXL_NV_BOOT        0x02  ///< accessible during boot services
#define AXL_NV_RUNTIME     0x04  ///< accessible at runtime

/**
 * @brief Read a value from non-volatile storage.
 *
 * @return 0 on success, -1 on error (variable not found, buffer
 *     too small, etc.). On buffer-too-small, @a size is updated
 *     to the required size.
 */
int
axl_nvstore_get(
    const char *ns,    ///< namespace (e.g., "global", "app")
    const char *key,   ///< variable name (UTF-8)
    void       *buf,   ///< output buffer
    size_t     *size   ///< [in/out] buffer size / bytes read
);

/**
 * @brief Write a value to non-volatile storage.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_nvstore_set(
    const char *ns,    ///< namespace (e.g., "global", "app")
    const char *key,   ///< variable name (UTF-8)
    const void *buf,   ///< data to write
    size_t      size,  ///< data size in bytes
    uint32_t    flags  ///< AXL_NV_* flags
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_NVSTORE_H */
