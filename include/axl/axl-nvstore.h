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
 * Built-in namespaces:
 *   "global"  — standard firmware variables (e.g., SecureBoot, BootOrder)
 *   "app"     — application-specific persistent settings
 *
 * Vendor namespaces (Dell, HPE, Lenovo OEM variables) plug in via
 * axl_nvstore_register_namespace() with a backend-specific token.
 * On UEFI the token is a `const AxlGuid *` (vendor-GUID pointer);
 * on a Linux backend it might be a path prefix. Access sites stay
 * UEFI-free — they reference namespaces by name only.
 *
 * @code
 * uint8_t secure_boot;
 * size_t sz = sizeof(secure_boot);
 * if (axl_nvstore_get("global", "SecureBoot", &secure_boot, &sz) == 0) {
 *     axl_printf("SecureBoot: %s\n", secure_boot ? "enabled" : "disabled");
 * }
 *
 * extern const AxlGuid AXL_DELL_VENDOR_GUID;
 * axl_nvstore_register_namespace("dell", &AXL_DELL_VENDOR_GUID);
 * axl_nvstore_get("dell", "SystemId", buf, &sz);
 * @endcode
 */

#ifndef AXL_NVSTORE_H
#define AXL_NVSTORE_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#include <axl/axl-sys.h>   /* AxlGuid — used at namespace registration */

#ifdef __cplusplus
extern "C" {
#endif

#define AXL_NV_VOLATILE    0x00  ///< lost on reboot
#define AXL_NV_PERSISTENT  0x01  ///< survives reboot (non-volatile)
#define AXL_NV_BOOT        0x02  ///< accessible during boot services
#define AXL_NV_RUNTIME     0x04  ///< accessible at runtime

/**
 * @brief Register a namespace name and bind it to a backend token.
 *
 * The backend token is opaque to consumers. On UEFI it is a
 * `const AxlGuid *` (vendor-GUID pointer); on other backends it
 * may be a path prefix or other identifier. The pointer must remain
 * valid for the lifetime of the program — the table stores the
 * pointer, not a copy.
 *
 * Built-in namespaces "global" and "app" are pre-registered and do
 * not need to be registered explicitly.
 *
 * @return AXL_OK on success, AXL_ERR if the namespace table is full or the
 *     name is already registered with a different token.
 */
int
axl_nvstore_register_namespace(
    const char *name,           ///< namespace name (UTF-8, copied)
    const void *backend_token   ///< opaque per-backend token
);

/**
 * @brief Read a value from non-volatile storage.
 *
 * @return AXL_OK on success, AXL_ERR on error (variable not found, buffer
 *     too small, namespace not registered, etc.). On
 *     buffer-too-small, @a size is updated to the required size.
 */
int
axl_nvstore_get(
    const char *ns,    ///< namespace (e.g., "global", "app")
    const char *key,   ///< variable name (UTF-8)
    void       *buf,   ///< output buffer
    size_t     *size   ///< [in/out] buffer size / bytes read
);

/**
 * @brief Read a value from non-volatile storage into a heap buffer.
 *
 * Like @ref axl_nvstore_get, but the buffer is allocated for you.
 * Useful when reading variable-length blobs (NV strings, OEM
 * settings) where the caller doesn't know the size up front and
 * picking a fixed stack buffer either over-allocates or risks
 * truncation. On success, @c *out_buf is a heap pointer of @c
 * *out_size bytes that the caller frees with @ref axl_free.
 *
 * On failure, @c *out_buf is set to NULL and @c *out_size is set
 * to 0. The buffer is allocated with one extra byte beyond
 * @c *out_size and zeroed there, so callers that read string
 * variables can dereference @c (char *)*out_buf as a NUL-terminated
 * C string when the variable's payload doesn't already include a
 * trailing NUL.
 *
 * @return AXL_OK on success, AXL_ERR on any error (variable not found,
 *     allocation failed, namespace not registered, etc.).
 */
int
axl_nvstore_get_alloc(
    const char  *ns,        ///< namespace (e.g., "global", "app")
    const char  *key,       ///< variable name (UTF-8)
    void       **out_buf,   ///< [out] heap buffer, caller frees with axl_free
    size_t      *out_size   ///< [out] payload size in bytes (excluding the trailing NUL)
);

/**
 * @brief Write a value to non-volatile storage.
 *
 * Passing @c flags == 0 (or @c AXL_NV_VOLATILE alone) defaults to
 * @c AXL_NV_BOOT — UEFI rejects SetVariable with attribute mask 0
 * for non-delete writes, so the implementation substitutes
 * boot-services access as the minimal sensible default.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_nvstore_set(
    const char *ns,    ///< namespace
    const char *key,   ///< variable name (UTF-8)
    const void *buf,   ///< data to write
    size_t      size,  ///< data size in bytes
    uint32_t    flags  ///< AXL_NV_* flags (0 → AXL_NV_BOOT)
);

/**
 * @brief Delete a variable from non-volatile storage.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_nvstore_delete(
    const char *ns,    ///< namespace
    const char *key    ///< variable name (UTF-8)
);

/**
 * @brief Get a variable's attribute flags.
 *
 * @return AXL_OK on success, AXL_ERR on error (variable not found, namespace
 *     not registered, etc.).
 */
int
axl_nvstore_get_attrs(
    const char *ns,     ///< namespace
    const char *key,    ///< variable name (UTF-8)
    uint32_t   *attrs   ///< [out] AXL_NV_* flags
);

/**
 * @brief Iterator callback for axl_nvstore_iter.
 *
 * @return 0 to continue iteration, non-zero to stop. The
 *     non-zero value is returned to the iter() caller.
 */
typedef int (*AxlNvstoreIterFn)(
    const char *key,   ///< variable name (UTF-8)
    void       *ctx    ///< caller-supplied context
);

/**
 * @brief Iterate all keys in a namespace.
 *
 * Walks all variables whose backend token matches the registered
 * namespace's token, invoking @p cb for each. Stops early if @p cb
 * returns non-zero.
 *
 * @return 0 if the walk completed, the callback's non-zero value
 *     if it stopped early, or -1 if the namespace is not registered
 *     or the iterator failed.
 */
int
axl_nvstore_iter(
    const char       *ns,   ///< namespace
    AxlNvstoreIterFn  cb,   ///< callback, called once per key
    void             *ctx   ///< passed unchanged to @p cb
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_NVSTORE_H */
