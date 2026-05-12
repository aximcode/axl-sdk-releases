/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-image-verify.h
 * @brief PE Authenticode signature inspection without launching the image.
 *
 * axl_image_load runs the firmware's PE-loader signature checks
 * as a side-effect of loading, and only when Secure Boot is on. Tools
 * that want to ask "is this PE file signed and does its signature
 * validate against the current Secure Boot db?" without committing
 * to launching the image (incident-response triage, BIOS-update
 * pre-flight, bootable-media verification) reach for
 * axl_image_verify_signature.
 *
 * The check has two orthogonal axes:
 *
 *   - **Presence** (`has_signature`): does the PE file's Certificate
 *     Table data directory (PE/COFF spec §6.4) hold a non-empty
 *     WIN_CERTIFICATE blob? Detected by parsing file bytes only —
 *     works regardless of Secure Boot state and on any platform.
 *
 *   - **Validity** (`signature_valid`, `consulted_db`): if the
 *     caller asks for db validation and Secure Boot is enabled, the
 *     firmware's PE loader is asked to dry-run the same signature
 *     check it would perform on a real launch (via
 *     `LoadImage(SourceBuffer=...)` + immediate `UnloadImage`). The
 *     result is `EFI_SECURITY_VIOLATION` for a signature mismatch,
 *     `EFI_SUCCESS` for a valid one. When the caller passes
 *     `consult_db = false`, or Secure Boot is off, or the firmware
 *     refuses to load via SourceBuffer, `consulted_db` is set false
 *     and `signature_valid` mirrors `has_signature` (presence-only).
 *
 * @code
 * AxlImageSignatureInfo info = {0};
 * if (axl_image_verify_signature("fs0:\\boot.efi", true, &info) != 0) {
 *     axl_print("could not read or parse PE\n");
 * } else if (!info.has_signature) {
 *     axl_print("UNSIGNED\n");
 * } else if (info.consulted_db && !info.signature_valid) {
 *     axl_print("SIGNATURE INVALID against current Secure Boot db\n");
 * } else {
 *     axl_print("SIGNED%s\n",
 *               info.consulted_db ? " (db-validated)" : " (presence only)");
 * }
 * axl_image_signature_info_free(&info);
 * @endcode
 */

#ifndef AXL_IMAGE_VERIFY_H
#define AXL_IMAGE_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// PE Authenticode signature inspection result.
///
/// Cleared with axl_image_signature_info_free, which is
/// NULL-safe — callers that don't pass a non-NULL info pointer to
/// axl_image_verify_signature can skip the free.
typedef struct {
    bool   has_signature;     ///< PE Certificate Table directory entry holds a non-empty WIN_CERTIFICATE blob
    bool   signature_valid;   ///< signature validates (db-validated when consulted_db, presence-only otherwise)
    bool   consulted_db;      ///< Secure Boot db was actually consulted (firmware LoadImage dry-run succeeded)
    /// Subject CommonName from the first certificate in the
    /// PKCS#7 SignedData bundle. signtool.exe and most Authenticode
    /// signers emit the signer's certificate first in practice, but
    /// the format does not require it — the formal way to identify
    /// the signer is via SignerInfo's IssuerAndSerial. This field
    /// is best-effort, suitable for diagnostic output ("Signed by
    /// `<cn>`") but NOT for security decisions. NULL if
    /// has_signature is false, the cert can't be parsed, no
    /// CN attribute is present, or the CN string uses an encoding
    /// the walker doesn't support (T61String, BMPString,
    /// IA5String). Heap-allocated UTF-8; caller frees via
    /// axl_image_signature_info_free.
    char  *subject_cn;
    /// Issuer CommonName from the same certificate as subject_cn,
    /// extracted by the same parser. Same "first cert in the bundle,
    /// best-effort, diagnostic-only" caveats apply.
    char  *issuer_cn;
} AxlImageSignatureInfo;

/**
 * @brief Inspect a PE image's signature without launching it.
 *
 * Reads the file, locates the Certificate Table data-directory
 * entry, and (optionally) asks the firmware to dry-run the
 * signature check against the current Secure Boot db. See the
 * file-level overview for the per-field contract.
 *
 * @param path         Image file path (e.g. `"fs0:\\boot.efi"`).
 * @param consult_db   When true, ask the firmware to dry-run a
 *                     full db validation via `LoadImage(SourceBuffer)`
 *                     + immediate `UnloadImage`. Has no effect
 *                     beyond presence detection when Secure Boot is
 *                     off.
 *                     **Side-effect note:** the firmware's PE
 *                     loader allocates image memory, applies
 *                     relocations, and invokes any registered
 *                     `EFI_SECURITY2_ARCH_PROTOCOL` handlers as
 *                     part of the dry-run. Production firmwares
 *                     that hook those for audit logging,
 *                     PCR measurement, or `dbx` update
 *                     notifications WILL trigger those side
 *                     effects on every `consult_db = true` call;
 *                     `UnloadImage` reverses the load but not the
 *                     observability hooks. Pass `consult_db = false`
 *                     when those side effects are unacceptable.
 * @param info         [out] receives the inspection result. Must be
 *                     non-NULL. Caller frees via
 *                     axl_image_signature_info_free. When
 *                     @p info is non-NULL, every bool/pointer
 *                     field is cleared to false / NULL before any
 *                     further work — so on a -1 return the struct
 *                     is in a defined "unknown / nothing detected"
 *                     state, not arbitrary leftover bytes.
 * @return AXL_OK on success (with @p info populated), AXL_ERR if the file is
 *     missing/unreadable, the bytes are not a recognizable PE
 *     image, or @p info is NULL.
 */
int
axl_image_verify_signature(
    const char             *path,
    bool                    consult_db,
    AxlImageSignatureInfo  *info
);

/**
 * @brief Release any heap-allocated fields inside @p info.
 *
 * Frees AxlImageSignatureInfo::subject_cn and AxlImageSignatureInfo::issuer_cn
 * (each independently — either may be NULL) and clears the
 * struct's pointer fields back to NULL. NULL-safe on the @p info
 * pointer itself.
 */
void
axl_image_signature_info_free(
    AxlImageSignatureInfo  *info
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_IMAGE_VERIFY_H */
