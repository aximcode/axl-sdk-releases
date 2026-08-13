/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-embed.h
 *
 * Helper macros for declaring and using arbitrary binary blobs
 * embedded into a UEFI image at link time. The framework is
 * content-agnostic — driver `.efi` images are the canonical use
 * case (paired with axl_service_start_embedded or
 * axl_driver_load_buffer) but anything works:
 *
 *   - Trust material (CA bundles, public keys) parsed at startup
 *   - Static config files (JSON5, key=value) parsed at startup
 *   - HTML / CSS / JS for an embedded HTTP server
 *   - Lookup tables, license text, calibration data — anything
 *
 * The blob is supplied at link time by `axl-cc --embed PATH[=symbol]`
 * — axl-cc generates a `.s` file with `.incbin` and links it for
 * you. (AXL's own build system uses an internal `EMBED_BLOB`
 * Makefile function for the same purpose; the symbol convention is
 * shared.)
 *
 * Either way, in C the consumer writes:
 *
 * @code
 *   AXL_EMBED_DECLARE(greeting);
 *
 *   axl_printf("%.*s",
 *              (int)AXL_EMBED_SIZE(greeting),
 *              (const char *)AXL_EMBED_DATA(greeting));
 * @endcode
 *
 * For binary blobs (e.g. an embedded driver image), pass
 * `AXL_EMBED_DATA` / `AXL_EMBED_SIZE` directly to
 * `AxlServiceDeploy.driver_blob` / `axl_driver_load_buffer` — no
 * cast needed.
 *
 * The bare `name` argument names the blob in C terms (e.g.
 * `greeting`); the macros prepend the `axl_embedded_` prefix that
 * the linker symbols actually use, so the prefix appears in
 * exactly one place (here).
 *
 * See `sdk/examples/embed-asset.c` for a non-driver worked example
 * and `sdk/examples/service-demo/launch.c` for the driver case.
 */

#ifndef AXL_EMBED_H
#define AXL_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Declare extern symbols for a link-time embedded blob.
 *
 * Emits the `axl_embedded_<name>` and `axl_embedded_<name>_end`
 * extern declarations the linker resolves against the `.incbin`
 * sidecar (or the `.S` that `axl-cc --embed` generates).
 *
 * Use at file scope. Pair with AXL_EMBED_DATA and
 * AXL_EMBED_SIZE for read access.
 */
#define AXL_EMBED_DECLARE(name)                                 \
    extern const unsigned char axl_embedded_##name[];           \
    extern const unsigned char axl_embedded_##name##_end[]

/**
 * @brief Pointer to the first byte of an embedded blob.
 *
 * Result type is `const unsigned char *`. Pair with
 * AXL_EMBED_DECLARE.
 */
#define AXL_EMBED_DATA(name) (&axl_embedded_##name[0])

/**
 * @brief Size of an embedded blob in bytes.
 *
 * Result type is `size_t`. Pair with AXL_EMBED_DECLARE.
 */
#define AXL_EMBED_SIZE(name)                                    \
    ((size_t)(axl_embedded_##name##_end - axl_embedded_##name))

#ifdef __cplusplus
}
#endif

#endif /* AXL_EMBED_H */
