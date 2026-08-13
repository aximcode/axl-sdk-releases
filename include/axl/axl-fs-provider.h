/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fs-provider.h
 *
 * Filesystem-publisher abstraction. Lets a consumer publish a
 * UEFI-visible filesystem (Shell `dir fsN:`, LoadImage from
 * `fsN:\\foo.efi`, the Boot Manager's volume picker) without
 * writing a single `EFI_*` identifier.
 *
 * The consumer fills an `AxlFsProvider` vtable in pure UTF-8 /
 * snake_case / `AxlFsStatus` terms. `axl_fs_provider_publish`
 * synthesizes the matching `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` +
 * `EFI_FILE_PROTOCOL` vtables, marshals UCS-2 ↔ UTF-8 at the
 * boundary, lays out `EFI_FILE_INFO` / `EFI_FILE_SYSTEM_INFO`
 * trailers in caller-supplied buffers (with the spec's
 * probe-then-resize `EFI_BUFFER_TOO_SMALL` semantics), maps
 * `AxlFsStatus` → spec-mandated `EFI_STATUS` codes, and installs
 * both protocols on a freshly-created handle.
 *
 * Design choices documented in `docs/AXL-EFI-Encapsulation-Plan.md`
 * (Phase C, kickoff deltas #1–#9).
 *
 * @code
 * static AxlFsStatus my_open(void *ctx, const char *path,
 *                            unsigned mode, AxlFsProviderFile **out,
 *                            bool *is_dir) { ... }
 * static AxlFsStatus my_read(AxlFsProviderFile *f, void *buf,
 *                            size_t *inout) { ... }
 * // ... rest of the vtable ...
 *
 * static const AxlFsProvider provider = {
 *     .struct_size   = sizeof(AxlFsProvider),
 *     .version       = AXL_FS_PROVIDER_VERSION,
 *     .open          = my_open,
 *     .close         = my_close,
 *     .read          = my_read,
 *     .read_dir      = my_read_dir,
 *     .get_info      = my_get_info,
 *     .default_label = "MyFs",
 *     .backend_ctx   = &my_state,
 * };
 *
 * static AxlGuid my_guid = AXL_GUID(0x..., 0x..., 0x...,
 *                                   0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..);
 * void *handle = NULL;
 * axl_fs_provider_publish(&provider, &my_guid, &handle);
 * // ... later, on driver unload:
 * axl_fs_provider_unpublish(handle);
 * @endcode
 */

#ifndef AXL_FS_PROVIDER_H
#define AXL_FS_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-fs.h>       /* AxlFsEntry, AXL_FS_OPEN_*, AXL_FS_ATTR_* */
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>      /* AxlGuid */

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------

/// Current `AxlFsProvider.version` and `AxlFsEntry.version`
/// value emitted by the SDK. Bumped when either struct gains a new
/// field. Forward-compat: consumers and providers test
/// `instance.struct_size >= offsetof(struct, new_field) +
/// sizeof(new_field)` before reading any field added in a version
/// later than the one they were built against.
#define AXL_FS_PROVIDER_VERSION  1

// ---------------------------------------------------------------------------
// Status codes
// ---------------------------------------------------------------------------

/**
 * @brief Typed status returned by every provider callback.
 *
 * The SDK thunks map these onto the spec-mandated `EFI_STATUS` for
 * each `EFI_FILE_PROTOCOL` entry point. Providers stay in pure
 * snake_case-land; UEFI consumers (Shell, Boot Manager) still see
 * the right error code (`EFI_NOT_FOUND` vs `EFI_DEVICE_ERROR` vs
 * `EFI_WRITE_PROTECTED` etc.).
 *
 * Follows the `Axl<Module>Status` convention: `AXL_FS_OK` is 0 and
 * every error member is negative (mirrors `AxlStatus`), so
 * `status != AXL_FS_OK` and `status < 0` both read as failure.
 */
typedef enum {
    AXL_FS_OK                   =   0,
    AXL_FS_ERR_NOT_FOUND        =  -1,  ///< → EFI_NOT_FOUND
    AXL_FS_ERR_ACCESS_DENIED    =  -2,  ///< → EFI_ACCESS_DENIED
    AXL_FS_ERR_WRITE_PROTECTED  =  -3,  ///< → EFI_WRITE_PROTECTED
    AXL_FS_ERR_NO_SPACE         =  -4,  ///< → EFI_VOLUME_FULL
    AXL_FS_ERR_NOT_DIR          =  -5,  ///< → EFI_INVALID_PARAMETER (open mode mismatch)
    AXL_FS_ERR_IS_DIR           =  -6,  ///< → EFI_INVALID_PARAMETER (write to dir)
    AXL_FS_ERR_INVALID          =  -7,  ///< → EFI_INVALID_PARAMETER
    AXL_FS_ERR_NO_MEMORY        =  -8,  ///< → EFI_OUT_OF_RESOURCES
    AXL_FS_ERR_IO               =  -9,  ///< → EFI_DEVICE_ERROR
    AXL_FS_ERR_UNSUPPORTED      = -10,  ///< → EFI_UNSUPPORTED
    AXL_FS_ERR_END_OF_FILE      = -11,  ///< → EFI_END_OF_FILE
    AXL_FS_ERR_VOLUME_CORRUPTED = -12,  ///< → EFI_VOLUME_CORRUPTED
} AxlFsStatus;

/* Open-mode flags (`AXL_FS_OPEN_READ` / `_WRITE` / `_CREATE`) and
   attribute bits (`AXL_FS_ATTR_DIRECTORY` etc.) live in
   `<axl/axl-fs.h>` — they're shared between consumer and publisher
   sides of the filesystem API. The fs-provider open callback uses
   them as documented at `AxlFsProviderOpen` below. */

// ---------------------------------------------------------------------------
// Per-volume info
// ---------------------------------------------------------------------------

/* Per-file metadata travels in `AxlFsEntry` (defined in
   `<axl/axl-fs.h>`). The provider's `get_info` / `read_dir` /
   `set_info` callbacks all populate / read that struct directly. */

/**
 * @brief Volume-level metadata.
 *
 * Returned by the optional `AxlFsProviderVolumeInfoFn` callback.
 * If the provider doesn't supply one, the thunk synthesizes a
 * default with `(uint64_t)-1` sizes and the static
 * `AxlFsProvider.default_label` string.
 */
typedef struct {
    uint32_t struct_size;     ///< sizeof(AxlFsProviderVolumeInfo) at write time
    uint32_t version;         ///< AXL_FS_PROVIDER_VERSION at write time
    bool     read_only;       ///< true if volume is read-only
    uint64_t volume_size;     ///< total bytes; (uint64_t)-1 if unknown
    uint64_t free_space;      ///< free bytes; (uint64_t)-1 if unknown
    uint32_t block_size;      ///< usually 512
    char     label[64];       ///< UTF-8 volume label
} AxlFsProviderVolumeInfo;

// ---------------------------------------------------------------------------
// Provider vtable callback signatures
// ---------------------------------------------------------------------------

/// Opaque per-open-file handle owned by the provider.
typedef struct AxlFsProviderFile AxlFsProviderFile;

/**
 * @brief Open a path on the provider's filesystem.
 *
 * The thunk has already resolved the EFI-supplied UCS-2 path to
 * absolute UTF-8 with `/` separators, including "." / ".." /
 * "" (open-self) handling and `\\` ↔ `/` translation. Providers
 * always see an absolute path rooted at `/`.
 *
 * @p mode is an `AXL_FS_OPEN_*` bitmask. @p attributes is an
 * `AXL_FS_ATTR_*` bitmask interpreted only when
 * `AXL_FS_OPEN_CREATE` is set: pass `AXL_FS_ATTR_DIRECTORY` to
 * request mkdir, otherwise the provider creates a regular file
 * (matches UEFI 2.11 §13.5.2's `Attributes` parameter to
 * `EFI_FILE_OPEN`). Providers that don't support directory
 * creation should return `AXL_FS_ERR_UNSUPPORTED` for that case.
 *
 * @p out_is_dir tells the thunk whether to dispatch subsequent
 * `Read` calls to `read` (file bytes) or `read_dir` (entries).
 * The provider already knows because it just looked up the entry,
 * so reporting it here saves a stat round trip.
 *
 * **Lifetime.** @p utf8_path is owned by the SDK thunk and is
 * valid only for the duration of this call. Providers that need to
 * retain the path (most do — for child Open resolution, GetInfo
 * "is root?" checks, rename source path) must copy it into the
 * `AxlFsProviderFile` they return.
 *
 * @return AXL_FS_OK on success; AXL_FS_ERR_* otherwise.
 */
typedef AxlFsStatus (*AxlFsProviderOpen)(
    void               *backend_ctx,    ///< AxlFsProvider.backend_ctx
    const char         *utf8_path,      ///< absolute UTF-8, '/' separators
    unsigned            mode,           ///< AXL_FS_OPEN_* bitmask
    unsigned            attributes,     ///< AXL_FS_ATTR_* bitmask (CREATE only)
    AxlFsProviderFile **out,            ///< [out] new file handle
    bool               *out_is_dir      ///< [out] true if @p utf8_path is a directory
) AXL_CB_NOEXCEPT;

/**
 * @brief Close a previously-opened file handle.
 *
 * The thunk calls @p close exactly once per successful @p open
 * (and during `axl_fs_provider_unpublish` on every still-open
 * handle, per the force-close-on-unpublish contract).
 *
 * Errors are logged but otherwise ignored — the caller's
 * `EFI_FILE_CLOSE` returns `EFI_SUCCESS` regardless, per UEFI 2.11
 * §13.5.4.
 */
typedef AxlFsStatus (*AxlFsProviderClose)(AxlFsProviderFile *file) AXL_CB_NOEXCEPT;

/**
 * @brief Read bytes from a regular-file handle.
 *
 * @p inout_size is the requested byte count on entry, the
 * actually-read byte count on exit. A successful read of zero
 * bytes signals EOF (mirrors `EFI_FILE_READ` spec).
 *
 * Not called for directory handles — see `read_dir`.
 */
typedef AxlFsStatus (*AxlFsProviderRead)(
    AxlFsProviderFile *file,
    void              *buf,             ///< caller-supplied buffer
    size_t            *inout_size       ///< [in] requested / [out] read; 0 = EOF
) AXL_CB_NOEXCEPT;

/**
 * @brief Read one directory entry.
 *
 * Sets `*out_end = true` (with status `AXL_FS_OK`) when there are
 * no more entries. Otherwise populates @p out and sets
 * `*out_end = false`. Iteration order is implementation-defined
 * but stable across calls within a single open.
 *
 * The thunk re-marshals the populated `AxlFsEntry` into
 * `EFI_FILE_INFO` (header + UCS-2 trailer) per call.
 */
typedef AxlFsStatus (*AxlFsProviderReadDir)(
    AxlFsProviderFile *file,
    AxlFsEntry *out,             ///< [out] entry; valid only if !*out_end
    bool              *out_end          ///< [out] true if no more entries
) AXL_CB_NOEXCEPT;

/**
 * @brief Write bytes to a regular-file handle.
 *
 * @p inout_size is the requested byte count on entry, the
 * actually-written byte count on exit. NULL in the vtable means
 * the filesystem is read-only (thunk returns `EFI_WRITE_PROTECTED`
 * for all writes).
 */
typedef AxlFsStatus (*AxlFsProviderWrite)(
    AxlFsProviderFile *file,
    const void        *buf,
    size_t            *inout_size
) AXL_CB_NOEXCEPT;

/**
 * @brief Seek to an absolute byte offset.
 *
 * `(uint64_t)-1` means seek-to-EOF (mirrors EFI's
 * `0xFFFFFFFFFFFFFFFF` convention). Directory `seek(0)` resets
 * iteration to the start; other directory seeks are
 * `AXL_FS_ERR_UNSUPPORTED`.
 */
typedef AxlFsStatus (*AxlFsProviderSeek)(
    AxlFsProviderFile *file,
    uint64_t           position
) AXL_CB_NOEXCEPT;

/**
 * @brief Delete the file referenced by @p file.
 *
 * Per UEFI 2.11 §13.5.5, the file handle is closed regardless of
 * whether the delete succeeded. The thunk takes care of calling
 * @c close after this returns; the provider just deletes the
 * backing object.
 *
 * NULL in the vtable means delete is unsupported (thunk returns
 * `EFI_WARN_DELETE_FAILURE`).
 */
typedef AxlFsStatus (*AxlFsProviderDelete)(AxlFsProviderFile *file) AXL_CB_NOEXCEPT;

/**
 * @brief Flush pending writes for @p file.
 *
 * NULL in the vtable means flush is a no-op (thunk returns
 * `EFI_SUCCESS`).
 */
typedef AxlFsStatus (*AxlFsProviderFlush)(AxlFsProviderFile *file) AXL_CB_NOEXCEPT;

/**
 * @brief Populate @p out with this file's metadata.
 *
 * The thunk converts to `EFI_FILE_INFO` for UEFI consumers,
 * including the UCS-2 trailer with the UTF-8-decoded
 * `AxlFsEntry.name`.
 */
typedef AxlFsStatus (*AxlFsProviderGetInfo)(
    AxlFsProviderFile *file,
    AxlFsEntry *out
) AXL_CB_NOEXCEPT;

/**
 * @brief Apply changes to this file's metadata.
 *
 * EFI consumers use `SetInfo(EFI_FILE_INFO)` for two purposes:
 * (1) renaming (the trailing UCS-2 name in the buffer differs
 * from the file's current basename), and (2) attribute changes.
 * The thunk decodes both and presents them as a normalized
 * `AxlFsEntry` with the new name and attributes already
 * in axl form.
 *
 * NULL in the vtable means SetInfo is unsupported (thunk returns
 * `EFI_WRITE_PROTECTED`).
 */
typedef AxlFsStatus (*AxlFsProviderSetInfo)(
    AxlFsProviderFile       *file,
    const AxlFsEntry *in
) AXL_CB_NOEXCEPT;

/**
 * @brief Optional volume-level info callback.
 *
 * UEFI's `EFI_FILE_GET_INFO` accepts both per-file
 * (`gEfiFileInfoGuid`) and per-volume
 * (`gEfiFileSystemInfoGuid` / `gEfiFileSystemVolumeLabelInfoIdGuid`)
 * GUIDs. Per-file goes through `get_info`; per-volume goes through
 * this callback. NULL means "use AxlFsProvider.default_label and
 * report (uint64_t)-1 for volume_size / free_space".
 */
typedef AxlFsStatus (*AxlFsProviderVolumeInfoFn)(
    void                    *backend_ctx,
    AxlFsProviderVolumeInfo *out
) AXL_CB_NOEXCEPT;

// ---------------------------------------------------------------------------
// The provider vtable
// ---------------------------------------------------------------------------

/**
 * @brief A filesystem-provider vtable.
 *
 * The consumer fills this once and passes a pointer to
 * `axl_fs_provider_publish`. The pointer must remain valid for as
 * long as the publication lives — typically `static const` in the
 * driver image.
 *
 * The struct_size + version prefix lets the SDK extend the vtable
 * without breaking existing providers. Always set:
 *   `.struct_size = sizeof(AxlFsProvider)`,
 *   `.version     = AXL_FS_PROVIDER_VERSION`.
 *
 * Optional callbacks (set to NULL to opt out):
 *   - `write`       — NULL → EFI_WRITE_PROTECTED for all writes
 *   - `del`         — NULL → EFI_WARN_DELETE_FAILURE for all deletes
 *   - `set_info`    — NULL → EFI_WRITE_PROTECTED for SetInfo
 *   - `flush`       — NULL → EFI_SUCCESS no-op
 *   - `volume_info` — NULL → thunk synthesizes from `default_label`
 *
 * Required callbacks: `open`, `close`, `read`, `read_dir`, `seek`,
 * `get_info`. Missing any of these is a publish-time validation
 * error (returns AXL_ERR).
 */
typedef struct {
    uint32_t struct_size;     ///< sizeof(AxlFsProvider)
    uint32_t version;         ///< AXL_FS_PROVIDER_VERSION

    AxlFsProviderOpen         open;
    AxlFsProviderClose        close;
    AxlFsProviderRead         read;
    AxlFsProviderReadDir      read_dir;
    AxlFsProviderWrite        write;        ///< optional
    AxlFsProviderSeek         seek;
    AxlFsProviderDelete       del;          ///< optional ('delete' is a C++ keyword)
    AxlFsProviderFlush        flush;        ///< optional
    AxlFsProviderGetInfo      get_info;
    AxlFsProviderSetInfo      set_info;     ///< optional
    AxlFsProviderVolumeInfoFn volume_info;  ///< optional
    const char               *default_label; ///< used when volume_info NULL; "" allowed

    void                     *backend_ctx;  ///< passed to open / volume_info
} AxlFsProvider;

// ---------------------------------------------------------------------------
// Publish / unpublish
// ---------------------------------------------------------------------------

/**
 * @brief Publish a filesystem on a new UEFI handle.
 *
 * Synthesizes `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` and
 * `EFI_FILE_PROTOCOL` vtables that forward into @p provider,
 * marshals UCS-2 ↔ UTF-8 at the boundary, builds a vendor
 * device-path with @p vendor_guid, and installs the protocols on
 * a freshly-created handle. The returned @p out_handle is opaque
 * to consumers (treat as a token); internally it is the
 * `EFI_HANDLE` the protocols were installed on, so AXL primitives
 * like `axl_protocol_find_guid` / `axl_driver_connect_handle`
 * work against it without further bookkeeping. Pass it to
 * `axl_fs_provider_unpublish` to tear down.
 *
 * UEFI consumers (Shell, Boot Manager, LoadImage) see a
 * spec-conformant filesystem they can `dir` / `cd` / `LoadImage`
 * against without knowing it's a thunked provider.
 *
 * @p vendor_guid should be unique to the provider kind so device
 * paths from multiple instances don't collide. Multiple
 * concurrent publish calls with the same GUID are allowed (each
 * gets its own handle).
 *
 * @return AXL_OK on success, AXL_ERR if @p provider is malformed
 *     (missing required callback, wrong struct_size, unsupported
 *     version) or the protocol install fails.
 */
AXL_WARN_UNUSED int
axl_fs_provider_publish(
    const AxlFsProvider *provider,    ///< caller-owned vtable
    const AxlGuid       *vendor_guid, ///< identifies provider kind
    void               **out_handle   ///< [out] opaque, for unpublish
);

/**
 * @brief Tear down a previously-published filesystem.
 *
 * Force-closes every still-open `AxlFsProviderFile *` (calling
 * the provider's `close` callback on each), uninstalls both
 * protocols from the published handle, and frees all SDK-side
 * thunk state.
 *
 * UEFI consumers that hold a stale `EFI_FILE_PROTOCOL *` after
 * unpublish receive `EFI_DEVICE_ERROR` from the next call (the
 * thunk retains a small "dead handle" record so the deref doesn't
 * fault). This is the right shape for AXL_SERVICE_DRIVER teardown,
 * where the driver image (and the vtable function pointers in it)
 * is going away — the alternative ("refuse if files open") would
 * leak the protocols permanently.
 *
 * NULL @p handle is a no-op.
 *
 * @return AXL_OK on success, AXL_ERR if @p handle was never
 *     returned by `axl_fs_provider_publish` or has already been
 *     unpublished.
 */
int
axl_fs_provider_unpublish(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* AXL_FS_PROVIDER_H */
