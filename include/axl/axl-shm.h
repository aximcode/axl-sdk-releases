/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-shm.h:
 *
 * Boot-persistent named shared memory — the UEFI analog of POSIX
 * shm_open + mmap (or System V shmget + shmat).
 *
 * A named, fixed-size region of memory that outlives the app that created
 * it and is reachable by any later app in the same boot. UEFI is a single
 * flat, identity-mapped address space, so there is no per-process mapping:
 * axl_shm_open returns a pointer that is the region — there is no separate
 * map/attach step (mmap / shmat collapse to nothing), and detach is a
 * no-op. The region lives in boot-services RAM and is therefore volatile:
 * it is reclaimed at reboot / ExitBootServices. Not NVRAM — no flash wear,
 * and capacity is system RAM, not the tiny firmware variable store.
 *
 * Identity is a name string, hashed to a GUID (axl_guid_v5 under a fixed
 * AXL_SHM namespace) and published as a UEFI protocol, so the same name
 * resolves to the same region across images with no shared handle. Names
 * are a single global namespace (like POSIX "/name") — prefix yours to
 * avoid collisions.
 *
 * Mechanism: the region is a data-only pool allocation (it holds bytes,
 * never function pointers — a function pointer would dangle once the
 * creating image unloads) installed under its GUID; pool allocated this
 * way survives image unload. This is the same pattern the backend uses
 * internally to cache the calibrated TSC frequency across processes.
 *
 * Single-threaded (UEFI): no locking is provided. In the shell, apps run
 * one at a time, so a writer and a later reader never overlap; if you
 * nest access within one image, coordinate it yourself.
 */

#ifndef AXL_SHM_H
#define AXL_SHM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AXL_SHM_CREATE  0x1u   ///< create the segment if it does not exist
#define AXL_SHM_EXCL    0x2u   ///< with CREATE, fail if it already exists

/**
 * @brief Open or create a named shared-memory segment.
 *
 * Without @c AXL_SHM_CREATE, opens an existing segment (returns NULL if
 * none; @p size is ignored). With @c AXL_SHM_CREATE, returns the existing
 * segment if present, otherwise creates a @p size-byte zero-filled one;
 * adding @c AXL_SHM_EXCL makes "already exists" an error (NULL) instead.
 * Opening an existing segment ignores @p size and returns it at the size
 * it was created with (read @p out_size).
 *
 * The returned pointer is the region base — directly readable and
 * writable, at least 8-byte aligned, valid until axl_shm_unlink (or
 * reboot). Every open of the same name (in this or any other app) returns
 * the SAME region. Do not free it.
 *
 * @return region base pointer, or NULL (no segment and no CREATE, an
 *     EXCL conflict, allocation failure, or boot services unavailable).
 *     @p out_size (optional) receives the segment's actual byte size.
 */
void *
axl_shm_open(
    const char *name,       ///< segment name (UTF-8)
    size_t      size,       ///< bytes to create (ignored when opening existing)
    uint32_t    flags,      ///< AXL_SHM_CREATE | AXL_SHM_EXCL
    size_t     *out_size    ///< [out, optional] actual segment size
);

/**
 * @brief Destroy a named segment and free its memory.
 *
 * Uninstalls the segment's protocol and frees the region. The name
 * becomes reusable and any base pointer to the segment is now invalid.
 * An absent name is a no-op success.
 *
 * @return AXL_OK if the segment was removed or already absent, AXL_ERR if
 *     a present segment could not be uninstalled.
 */
AXL_WARN_UNUSED int
axl_shm_unlink(
    const char *name   ///< segment name (UTF-8)
);

/**
 * @brief Whether a named segment currently exists.
 *
 * @return true if present; @p out_size (optional) receives its size.
 */
bool
axl_shm_exists(
    const char *name,      ///< segment name (UTF-8)
    size_t     *out_size   ///< [out, optional] segment size if present
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SHM_H */
