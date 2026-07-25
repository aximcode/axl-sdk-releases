/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-sys.h
 *
 * System operations — reset, device mapping refresh.
 * UEFI-specific, no GLib equivalent.
 */

#ifndef AXL_SYS_H
#define AXL_SYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// GUID
// ---------------------------------------------------------------------------

/**
 * @brief Opaque handle for any UEFI-tracked entity.
 *
 * Binary-compatible with `EFI_HANDLE` (both `void *`). Use in
 * public API and consumer code wherever a UEFI handle would
 * appear — image handles from `DriverEntry`, protocol handles
 * returned by `axl_protocol_register`, controller handles passed
 * to `axl_driver_connect_handle`, handles returned by
 * `axl_protocol_enumerate`. `AxlHandle` is an opaque token; never
 * dereference it. The EFI_* spelling stays available via
 * `<uefi/axl-uefi.h>` for the rare consumer that needs to call a
 * `gBS->...(EFI_HANDLE)` directly.
 */
typedef void *AxlHandle;

/**
 * @brief Opaque firmware system-table pointer.
 *
 * Forward-decl for `EFI_SYSTEM_TABLE`. Drivers receive an
 * `AxlSystemTable *` from the `AXL_DRIVER` adapter and pass it
 * straight to `axl_driver_init`; consumers never dereference it.
 * Reach for `<uefi/axl-uefi.h>`'s typed `EFI_SYSTEM_TABLE` only
 * when you need to poke at firmware internals the AXL surface
 * doesn't cover.
 */
typedef struct AxlSystemTable AxlSystemTable;

/**
 * @brief UEFI-compatible GUID in standard C types.
 *
 * Binary-compatible with EFI_GUID. Use in public API so consumer
 * apps don't need `<uefi/axl-uefi.h>` for GUID operations.
 */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} AxlGuid;

/**
 * @brief Compare two GUIDs, strcmp-style.
 *
 * Byte-lexicographic comparison over the 16-byte GUID image. The
 * ordering is stable but otherwise has no semantic meaning; it exists
 * so GUIDs can key sorted containers. For a plain equality test use
 * axl_guid_equal().
 *
 * @return 0 if equal, a negative value if @p a sorts before @p b, a
 *     positive value if @p a sorts after @p b.
 */
static inline int
axl_guid_cmp(
    const AxlGuid *a,
    const AxlGuid *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof(AxlGuid); i++) {
        if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    return 0;
}

/**
 * @brief Test two GUIDs for equality.
 *
 * Thin wrapper over axl_guid_cmp() — the readable form for the common
 * equality check.
 *
 * @return true if @p a and @p b are equal.
 */
static inline bool
axl_guid_equal(
    const AxlGuid *a,
    const AxlGuid *b)
{
    return axl_guid_cmp(a, b) == 0;
}

/**
 * @brief Initialize an AxlGuid from literal values.
 *
 * Usage: AxlGuid g = AXL_GUID(0x12345678, 0xABCD, 0xEF01,
 *                              0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01);
 */
#define AXL_GUID(d1, d2, d3, d4_0, d4_1, d4_2, d4_3, d4_4, d4_5, d4_6, d4_7) \
    { (d1), (d2), (d3), { (d4_0), (d4_1), (d4_2), (d4_3), (d4_4), (d4_5), (d4_6), (d4_7) } }

/**
 * @brief Derive a deterministic GUID from a (namespace, name) pair.
 *
 * Name-based UUID generation in the shape of RFC 4122 §4.3 (UUIDv5):
 * the SHA-1 of `namespace_bytes || name_bytes` is truncated to 16 bytes,
 * with the version field set to 5 and the RFC-4122 variant bits set on
 * the result. Same `(namespace, name)` always yields the same GUID,
 * across binaries / arches / runs.
 *
 * Used by AxlService to derive each service's identity GUID from
 * `AxlService.name` so consumers don't have to hand-allocate a UUID
 * per service. Other AXL modules that want stable GUIDs from string
 * keys can use the same primitive.
 *
 * **Namespace bytes are fed verbatim** — AxlGuid's storage layout
 * (data1/data2/data3 in host byte order) is what hashes, NOT the
 * RFC-4122 network-byte-order serialization. The 16-byte result is
 * likewise stored as opaque AxlGuid bytes; AXL never reads its
 * data1/data2/data3 fields as host-order ints once derived. This is
 * an internal AXL convention, not strict RFC 4122 — GUIDs derived
 * here won't match what a UUIDv5 generator on another platform would
 * produce given "the same" namespace UUID written in canonical text
 * form. That's fine for AXL's use case (derivation lives entirely
 * inside AXL) and avoids a host-vs-network endian conversion that
 * would otherwise creep into every caller.
 *
 * @return AXL_OK on success (@p out populated); AXL_ERR if @p namespace
 *     or @p name is NULL or @p out is NULL.
 */
int
axl_guid_v5(
    const AxlGuid *namespace_uuid,  ///< namespace UUID (e.g. an AXL module's identity)
    const char    *name,            ///< NUL-terminated name string
    AxlGuid       *out              ///< [out] derived GUID
);

// ---------------------------------------------------------------------------
// Device path
// ---------------------------------------------------------------------------

/**
 * @brief Check if a device path contains a vendor node with the given GUID.
 *
 * Walks the device path node chain looking for a hardware vendor node
 * (type 0x01, subtype 0x04) whose GUID matches @p guid.
 *
 * @return true if a matching vendor node is found.
 */
bool
axl_device_path_has_vendor(
    void          *device_path,  ///< device path (from "device-path" protocol)
    const AxlGuid *guid          ///< vendor GUID to match
);

/**
 * @brief Per-node callback for axl_device_path_for_each.
 *
 * Return 0 to continue iteration, any non-zero value to stop —
 * the return value is propagated back from `axl_device_path_for_each`
 * so callbacks can use it as a found-flag, error code, or count.
 *
 * @p node points at the full device-path node (4-byte header
 * followed by payload); cast it to the corresponding spec struct
 * (e.g. `VENDOR_DEVICE_PATH *`) once @p type and @p subtype have
 * confirmed the shape.
 */
typedef int (*AxlDevicePathFn)(
    uint8_t      type,
    uint8_t      subtype,
    const void  *node,
    void        *user
);

/**
 * @brief Walk a device-path node chain with bounded-step safety.
 *
 * Iterates from @p device_path through the END node, calling @p fn
 * on each node with its `(type, subtype, node)` triple. Stops early
 * when @p fn returns non-zero (and propagates that value), or when
 * a malformed node is hit (length < 4 or the chain doesn't terminate
 * within an internal step cap).
 *
 * Replaces hand-rolled `while (!EFI_DP_IS_END(node)) ...` loops —
 * those used to differ on whether they bounded the walk, leaving
 * malformed firmware data able to runaway.
 *
 * @return 0 on a clean traversal to END, the callback's non-zero
 *     return value if it stopped early, or -1 on malformed input.
 */
int
axl_device_path_for_each(
    const void       *device_path,  ///< device path (from "device-path" protocol)
    AxlDevicePathFn   fn,            ///< per-node callback
    void             *user           ///< opaque user pointer for the callback
);

/**
 * @brief Find the first device-path node matching (type, subtype).
 *
 * @return pointer to the node (cast to the corresponding spec
 *     struct by the caller), or NULL if no match.
 */
const void *
axl_device_path_find(
    const void *device_path,  ///< device path (from "device-path" protocol)
    uint8_t     type,         ///< node type to match
    uint8_t     subtype       ///< node subtype to match
);

/**
 * @brief Compute the total byte length of a device path INCLUDING
 *        the END node.
 *
 * Useful when copying / appending device paths, e.g. when building
 * a LoadImage argument out of an existing volume DP plus a file
 * suffix. Bounded by the same step cap as the iterator.
 *
 * @return size in bytes, or 0 on malformed input.
 */
size_t
axl_device_path_size(
    const void *device_path  ///< device path (from "device-path" protocol)
);

/**
 * @brief Render a device path as the firmware's canonical text form.
 *
 * Wraps the EFI_DEVICE_PATH_TO_TEXT_PROTOCOL the firmware exposes
 * (`ConvertDevicePathToText`) and converts the resulting UCS-2
 * string to UTF-8. The output is the same format `dh -d` and
 * `bcfg boot dump` produce — e.g.
 * `PciRoot(0x0)/Pci(0x3,0x0)/MAC(525400123456,0x1)`.
 *
 * Returns NULL when the firmware doesn't expose
 * EFI_DEVICE_PATH_TO_TEXT_PROTOCOL (some vintage UEFI 2.0 builds
 * omit it) or when @p device_path is NULL.
 *
 * @return UTF-8 string allocated with `axl_malloc`, or NULL on
 *     failure. Caller frees with `axl_free`.
 */
char *
axl_device_path_to_text(
    const void *device_path  ///< device path (from "device-path" protocol)
);

// ---------------------------------------------------------------------------
// System control
// ---------------------------------------------------------------------------

/// How the system should be reset — the @p type argument to axl_reset.
typedef enum {
    AXL_RESET_COLD     = 0,  ///< cold reset (full power cycle)
    AXL_RESET_WARM     = 1,  ///< warm reset (CPU reset, memory preserved)
    AXL_RESET_SHUTDOWN = 2   ///< power off
} AxlResetType;

/**
 * @brief Reset or shut down the system.
 *
 * Does not return on success.
 */
void
axl_reset(
    AxlResetType type  ///< AXL_RESET_COLD, AXL_RESET_WARM, or AXL_RESET_SHUTDOWN
);

/**
 * @brief Rescan device-to-filesystem mappings.
 *
 * Runs the Shell "map -r" command for its rescan side effect. Call
 * after hot-plugging a USB drive or after a driver installs a new
 * filesystem. The command's device-mapping table is redirected to nul,
 * so the rescan is silent: this is a refresh, not a listing (use the
 * `map` tool to display the table). On the old EFI 1.x shell, where the
 * command runs in-context rather than in a nested shell, that
 * redirection is what stops `map -r` from printing over a caller's own
 * output.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_map_refresh(void);

// ---------------------------------------------------------------------------
// System information
// ---------------------------------------------------------------------------

/**
 * @brief Firmware information.
 */
typedef struct {
    char      vendor[64];          ///< firmware vendor name (UTF-8)
    uint32_t  firmware_revision;   ///< vendor firmware revision
    uint16_t  spec_major;          ///< UEFI spec major version
    uint16_t  spec_minor;          ///< UEFI spec minor version
} AxlFirmwareInfo;

/**
 * @brief Get firmware information (vendor, revision, spec version).
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_sys_get_firmware_info(
    AxlFirmwareInfo *info  ///< [out] receives firmware info
);

/**
 * @brief Get total usable RAM in bytes.
 *
 * Sums the @c AXL_MEM_REGION_RAM regions of the shared physical region map
 * (`<axl/axl-mem-region.h>`) — usable system memory (conventional, loader,
 * and boot-services memory). On firmware where the GCD reports system-memory
 * ranges the EFI memory map omits, those count too, so this is the total
 * physically-present usable RAM; in the common case the two sources agree
 * (a unit test pins this equality on the test platform).
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_sys_get_memory_size(
    uint64_t *total_bytes  ///< [out] receives total usable RAM
);

/**
 * @brief Get a protocol interface from a specific handle.
 *
 * @return AXL_OK on success, AXL_ERR if not found.
 */
int
axl_handle_get_protocol(
    void        *handle,     ///< handle from axl_protocol_enumerate
    const char  *name,       ///< protocol name (e.g., "device-path", "simple-fs")
    void       **interface   ///< [out] protocol interface pointer
);

// ---------------------------------------------------------------------------
// Protocol registry
// ---------------------------------------------------------------------------
//
// "Protocol" here is the UEFI spec's term, not a wire protocol. A
// UEFI protocol is a C struct of function pointers (sometimes with
// inline state), identified by a 128-bit GUID, installed on a
// handle — closer in shape to a COM interface or a Java/Swift
// interface bound to a specific instance than to anything network-
// shaped. The name is awkward and we adopt it anyway because the
// spec uses it everywhere. See `src/util/README.md` § "Protocol
// Registry" for the longer explainer.
//
// All register/find/enumerate/unregister calls bottom out in UEFI Boot
// Services protocol-database operations (`InstallProtocolInterface`,
// `LocateProtocol`, `LocateHandleBuffer`, `UninstallProtocolInterface`)
// and `axl_malloc` (which calls `gBS->AllocatePool`). UEFI 2.11 §7.3
// requires those to be invoked at TPL <= `TPL_NOTIFY`, so the same
// constraint applies to every entry point in this section. Callers
// running from a timer event handler at `TPL_NOTIFY` are fine; callers
// running at `TPL_HIGH_LEVEL` must lower first.
//
// Callbacks invoked via `AxlLoop` (defer-drain, pubsub dispatch,
// source handlers) all run at `TPL_APPLICATION` — the loop itself
// calls `WaitForEvent`, which mandates that level.

/**
 * @brief Pin a stable vendor GUID to a custom protocol name.
 *
 * By default `axl_protocol_register("custom-name", ...)` synthesizes a
 * deterministic GUID from the name string via FNV-1a. That works for
 * single-image use, but the GUID is unstable across name spelling
 * (a typo gives a different GUID) and isn't usable for cross-image
 * discovery via raw `LocateProtocol` because external consumers
 * can't reproduce it without the same name string.
 *
 * Calling `axl_protocol_register_name(name, guid)` once at startup
 * pins @p name to @p guid in the per-process registry, so subsequent
 * `axl_protocol_register` / `_find` / `_enumerate` / `_unregister`
 * calls for that name install or look up against @p guid instead.
 * Other consumers can publish the GUID in their own headers and
 * `LocateProtocol` against it without going through the AXL
 * protocol-registry layer at all.
 *
 * Idempotent: re-registering the same `(name, guid)` pair returns
 * `AXL_OK`. Re-registering a name with a different GUID, or
 * registering a name already in the built-in well-known table
 * (e.g. "smbios", "simple-fs"), returns `AXL_ERR`. Names are
 * copied internally; @p name does not need to outlive the call.
 *
 * Process-lifetime: the registration persists for the lifetime of
 * the running image. There is no `axl_protocol_unregister_name`;
 * unregistering a *handle* with `axl_protocol_unregister` does not
 * remove the name pinning, since other consumers may still want to
 * reuse the same name → GUID mapping. The custom-name table is
 * fixed-capacity (16 entries per image) and statically linked into
 * each image — consumers loading and unloading drivers on a tight
 * loop should pin once at first init, not on every iteration.
 *
 * Cross-image discovery: each image has its own copy of the AXL
 * protocol-registry layer (via static linkage of libaxl.a), so a
 * consumer in image B that wants to find a protocol published by
 * image A must either (a) call `axl_protocol_register_name` itself
 * with the same `(name, guid)` pair before calling
 * `axl_protocol_find`, or (b) call `LocateProtocol` directly
 * against the published GUID without going through the AXL
 * protocol-registry layer.
 *
 * @return AXL_OK on success or idempotent re-register; AXL_ERR if
 *     @p name is NULL/empty, @p guid is NULL, the name shadows a
 *     built-in well-known name, the name is already pinned to a
 *     different GUID, or the registry is full.
 */
int
axl_protocol_register_name(
    const char    *name,    ///< protocol name (copied internally)
    const AxlGuid *guid     ///< vendor GUID to bind to @p name
);

/**
 * @brief Find a system protocol by name.
 *
 * Looks up a named protocol in the platform protocol registry.
 * Well-known names: "smbios", "shell", "simple-network", "simple-fs".
 * Custom names work too — names registered via
 * `axl_protocol_register_name` resolve to their pinned GUID;
 * unregistered custom names fall back to a deterministic FNV-1a
 * GUID derived from the name string.
 *
 * Cross-image gotcha: the name → GUID table is per-image. A
 * consumer that wants to find a custom-named protocol published by
 * a different image must first call `axl_protocol_register_name`
 * with the same `(name, guid)` pair, or skip the name layer and
 * `LocateProtocol` against the published GUID directly.
 *
 * @return AXL_OK on success, AXL_ERR if not found.
 */
int
axl_protocol_find(
    const char *name,       ///< protocol name
    void      **interface   ///< [out] service interface pointer
);

/**
 * @brief Enumerate all handles providing a named protocol.
 *
 * Caller frees the returned handles array with axl_free().
 *
 * @return AXL_OK on success (count may be 0), AXL_ERR on error.
 */
int
axl_protocol_enumerate(
    const char  *name,      ///< protocol name
    void      ***handles,   ///< [out] array of handles
    size_t      *count      ///< [out] number of handles
);

/**
 * @brief Register a protocol on a handle.
 *
 * Creates a new handle if @a *handle is NULL.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_protocol_register(
    const char *name,       ///< protocol name
    void       *interface,  ///< protocol interface to install
    void      **handle      ///< [in/out] handle (NULL to create new)
);

/**
 * @brief Unregister a protocol from a handle.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_protocol_unregister(
    const char *name,       ///< protocol name
    void       *interface,  ///< interface to remove
    void       *handle      ///< handle from axl_protocol_register
);

/**
 * @brief Locate a protocol interface by GUID directly.
 *
 * GUID-keyed counterpart to axl_protocol_find. Skips the
 * name-registry name → GUID lookup; useful for consumers that
 * already hold a GUID (notably AxlService, whose identity is the
 * name-derived GUID from axl_service_guid). Returns the first
 * interface registered for the GUID via @c LocateProtocol semantics.
 *
 * @return AXL_OK on success (@p interface populated); AXL_ERR if no
 *     handle publishes the GUID or arguments are NULL.
 */
int
axl_protocol_find_guid(
    const AxlGuid *guid,        ///< protocol GUID to look up (must be non-NULL)
    void         **interface    ///< [out] service interface pointer
);

/**
 * @brief Enumerate all handles publishing a protocol by GUID directly.
 *
 * GUID-keyed counterpart to axl_protocol_enumerate. Returns an
 * @c axl_malloc'd array of handles publishing @p guid; caller frees
 * with @c axl_free. Used by @c axl_service_stop to discover every
 * driver image that registered the service's identity GUID
 * (typically one, but the contract handles N for symmetry with the
 * underlying @c LocateHandleBuffer).
 *
 * Empty result (no handle publishes the GUID) is success: @p handles
 * is set to NULL and @p count to 0.
 *
 * @return AXL_OK on success (count may be 0); AXL_ERR on bad
 *     arguments or firmware allocation failure.
 */
int
axl_protocol_enumerate_guid(
    const AxlGuid  *guid,       ///< protocol GUID to look up (must be non-NULL)
    void         ***handles,    ///< [out] axl_malloc'd handle array (may be NULL on empty)
    size_t         *count       ///< [out] number of handles
);

/* Note: GUID-based install/uninstall (no name lookup) is the
   axl_protocol_install / axl_protocol_uninstall primitive in
   <axl/axl-driver.h> — the single surface over the backend protocol seam.
   The former axl_protocol_register_guid / _unregister_guid duplicated it and
   were removed; call axl_protocol_install / _uninstall directly. */

/**
 * @brief Register multiple protocols on a handle atomically.
 *
 * Installs one or more protocols on the same handle in one operation.
 * If any fails, none are installed. Creates a new handle if
 * @a *handle is NULL. Pass name/interface pairs followed by NULL:
 *
 * @code
 * void *h = NULL;
 * axl_protocol_register_multiple(&h,
 *     "simple-fs", &my_fs,
 *     "device-path", &my_dp,
 *     NULL);
 * @endcode
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_protocol_register_multiple(
    void      **handle,  ///< [in/out] handle (NULL to create new)
    ...                  ///< name, interface pairs, terminated by NULL
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SYS_H */
