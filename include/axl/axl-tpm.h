/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tpm.h
    TPM 2.0 presence and capability readout.

    Reads the platform's TPM 2.0 capability via the firmware's TCG2
    protocol (`EFI_TCG2_PROTOCOL.GetCapability`). Unlike the other
    platform readers there is nothing to enumerate — the TCG2 protocol
    is a singleton — so this is a presence check plus one typed
    capability struct.

    @code
    if (axl_tpm_present()) {
        AxlTpmCapability cap;
        if (axl_tpm_get_capability(&cap) == AXL_OK) {
            // ... report manufacturer, banks, sizes ...
        }
    }
    @endcode

    Scope is the boot-service capability fields a diagnostic/inventory
    view reports. Measurement, the event log, and PCR extension are out
    of scope.
**/

#ifndef AXL_TPM_H
#define AXL_TPM_H

#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TPM 2.0 boot-service capability.
 *
 * Typed projection of the firmware's
 * `EFI_TCG2_BOOT_SERVICE_CAPABILITY`. `present` is the firmware's own
 * TPMPresentFlag — a TPM chip is installed and responding — which is
 * distinct from `axl_tpm_present()` reporting that the TCG2 *protocol*
 * is published.
 *
 * Note the two PCR fields are different kinds of value:
 * `number_of_pcr_banks` is a *count*, `active_pcr_banks` is a
 * *hash-algorithm bitmask* (not a count). Both are meaningful only
 * when the structure version is >= 1.1; on older firmware they read 0
 * — and since a present TPM always has at least one bank,
 * `number_of_pcr_banks == 0` on a present TPM means the firmware
 * predates struct ver 1.1, not a bankless TPM.
 *
 * The event-log format flags (SupportedEventLogs) are omitted — the
 * event log is measurement-domain (out of scope). The two
 * hash-algorithm bitmasks below are the supported/active pair an
 * inventory view reports.
 */
typedef struct {
    bool     present;                   ///< TPMPresentFlag: a TPM is installed and responding
    uint8_t  structure_version_major;   ///< capability structure version major
    uint8_t  structure_version_minor;   ///< capability structure version minor
    uint8_t  protocol_version_major;    ///< TCG2 protocol version major
    uint8_t  protocol_version_minor;    ///< TCG2 protocol version minor
    uint32_t manufacturer_id;           ///< TPM manufacturer ID (TCG vendor ID, 4 packed ASCII bytes)
    uint32_t max_command_size;          ///< max supported command buffer size in bytes
    uint32_t max_response_size;         ///< max supported response buffer size in bytes
    uint32_t number_of_pcr_banks;       ///< COUNT of PCR banks the TPM supports (struct ver >= 1.1)
    uint32_t supported_hash_algorithms; ///< hash algorithms the TCG2 stack supports, EFI_TCG2_BOOT_HASH_ALG_* BITMASK (HashAlgorithmBitmap; superset of active_pcr_banks; consumer decodes names)
    uint32_t active_pcr_banks;          ///< active PCR-bank hash-algorithm BITMASK, EFI_TCG2_BOOT_HASH_ALG_* (struct ver >= 1.1; consumer decodes names)
} AxlTpmCapability;

/**
 * @brief Report whether the firmware publishes the TCG2 protocol.
 *
 * A cheap presence gate: true means a TPM 2.0 software stack is
 * available to query (call `axl_tpm_get_capability` for the details).
 * It does not by itself guarantee a physical TPM is responding — that
 * is `AxlTpmCapability.present` (TPMPresentFlag). Result is cached
 * after the first call.
 *
 * @return true if the TCG2 protocol is published, false otherwise.
 */
bool
axl_tpm_present(void);

/**
 * @brief Read the TPM 2.0 boot-service capability.
 *
 * @return AXL_OK on success, AXL_ERR if the TCG2 protocol is not
 *     published, the GetCapability call fails, or @p out is NULL.
 *     A present-but-wedged TPM (protocol published, GetCapability
 *     fails) reports the same AXL_ERR as an absent protocol — the
 *     consumer reports the TPM as not present in both cases (the
 *     `axl_tpm_present() == false` case).
 */
int
axl_tpm_get_capability(
    AxlTpmCapability *out   ///< [out] populated on success
);

// ===================================================================
// Endorsement Key (EK) public part — hardware-rooted device identity
// ===================================================================
//
// The EK is the canonical per-device identity in a TPM 2.0: unique per
// chip, fixed at manufacture (derived from the Endorsement Primary
// Seed), with a freely readable public part and a private part that
// never leaves the TPM. It is stable across TPM2_Clear and OS
// reinstalls. A consumer doing attestation, device enrollment, or
// platform binding can hash the EK public bytes into a stable machine
// id. This is a generic device-identity primitive — it bakes in no
// hashing and no policy; the consumer chooses its own (domain-separated)
// hash of the returned bytes.

/**
 * @brief EK public-key algorithm.
 */
typedef enum {
    AXL_TPM_EK_RSA2048  = 1,  /**< RSA-2048 EK (TCG template L-1). The
                                   returned bytes are the modulus. */
    AXL_TPM_EK_ECC_P256 = 2   /**< ECC NIST P-256 EK (TCG template L-2).
                                   The returned bytes are the uncompressed
                                   point X||Y (64 bytes). */
} AxlTpmEkAlg;

/**
 * @brief Whether a TPM 2.0 with a readable Endorsement Key is present.
 *
 * A consumer can branch on this to fall back to a weaker machine id
 * (e.g. the SMBIOS UUID) when no TPM is available. Returns false
 * immediately when the TCG2 protocol is absent; otherwise it confirms
 * the EK can actually be derived. The result is cached after the first
 * call.
 *
 * @return true if axl_tpm_read_ek_pub() will succeed.
 */
bool
axl_tpm_ek_available(void);

/**
 * @brief Read the TPM 2.0 Endorsement Key public part.
 *
 * Derives the EK with TPM2_CreatePrimary in the endorsement hierarchy
 * using the standard TCG EK template — ECC P-256 first, falling back to
 * RSA-2048 — and returns the public key's canonical bytes: for ECC the
 * uncompressed point X||Y (64 bytes for P-256); for RSA the modulus
 * (256 bytes for RSA-2048). Bytes are in their natural big-endian order,
 * so they are deterministic for a given TPM across boots — hash them for
 * a stable machine id. Derivation is transient (the primary handle is
 * flushed); nothing is persisted in the TPM.
 *
 * Output-buffer protocol: call with @p buf == NULL to query the required
 * size (written to @p *out_len). Otherwise @p buf_size is the capacity;
 * on success @p *out_len is the byte count and @p *out_alg the key type.
 * If @p buf_size is too small, returns AXL_ERR with @p *out_len set to
 * the required size and @p buf untouched.
 *
 * @return AXL_OK on success; AXL_ERR if no TPM / no TCG2 protocol, the EK
 *     could not be derived, the buffer is too small, or @p out_len is
 *     NULL.
 */
int
axl_tpm_read_ek_pub(
    uint8_t     *buf,       ///< [out] EK public bytes, or NULL to size-query
    size_t       buf_size,  ///< capacity of @p buf in bytes
    size_t      *out_len,   ///< [out] bytes written / required size
    AxlTpmEkAlg *out_alg    ///< [out] EK algorithm (may be NULL)
);

// ===================================================================
// PCR-bound seal / unseal (secret-at-rest, gated on measured boot)
// ===================================================================
//
// Seal a small secret to the TPM so it can only be recovered when the
// selected PCRs hold the same values they had at seal time — i.e. only
// under the same measured-boot state. The sealed blob is opaque
// ciphertext the caller persists (e.g. in an EFI variable); it is
// useless on another machine or after the measured state changes.
//
// The flow is the standard TPM2 sealing chain over
// EFI_TCG2_PROTOCOL.SubmitCommand: a deterministic primary storage key
// (SRK, recreated identically each boot) parents a keyedhash sealed-data
// object whose authPolicy is a PolicyPCR digest over the chosen PCRs;
// unsealing runs a policy session that the TPM only satisfies when the
// live PCRs match. Use it for a TLS private key or similar small secret;
// the TPM caps the payload (a few dozen bytes).

/** Largest secret axl_tpm_seal accepts (TPM sealed-data limit). */
#define AXL_TPM_SEAL_MAX_SECRET  128

/**
 * @brief Diagnostic detail for a failed axl_tpm_seal / axl_tpm_unseal.
 *
 * A seal/unseal chains several TPM2 commands; on AXL_ERR the return code alone
 * cannot say which one failed or why. Pass a pointer to one of these to learn
 * the failing command and its raw TPM responseCode — enough to tell a
 * hierarchy/auth problem (e.g. an authorized owner hierarchy failing an
 * empty-auth `TPM2_CreatePrimary`) from a transport failure, in one boot.
 *
 * Both functions clear this on entry and fill it only on failure; on AXL_OK
 * (or any AXL_INVALID caught before a TPM command — a NULL argument or a
 * malformed blob) it is left `{ NULL, 0 }`.
 */
typedef struct {
    /** Failing TPM2 command name — a static string such as
     *  "TPM2_CreatePrimary" / "TPM2_Create" / "TPM2_Load" / "TPM2_Unseal", or
     *  NULL if no TPM command reported a failure (a success, an argument/blob
     *  rejection, or a rare local command-build/response-parse error). */
    const char *stage;
    /** The failing command's raw TPM responseCode. 0 when the failure was
     *  local (command building or response parsing, no TPM-level code);
     *  0xFFFFFFFF when the firmware's SubmitCommand itself failed (no TPM
     *  response at all). Otherwise the verbatim TPM2 responseCode. */
    uint32_t    tpm_rc;
} AxlTpmError;

/**
 * @brief Seal a secret under a PCR policy.
 *
 * Binds @p secret to the current values of the PCRs listed in @p pcrs
 * (SHA-256 bank, indices 0..23) and returns an opaque sealed blob in
 * @p out_blob that the caller persists and later passes to
 * `axl_tpm_unseal`. The blob carries everything unseal needs (the sealed
 * object and the PCR selection); the secret is never in it in the clear.
 *
 * @p out_blob is allocated with axl_malloc — free it with `axl_free`.
 *
 * @return AXL_OK on success; AXL_INVALID if @p secret / @p out_blob /
 *     @p out_blob_len is NULL, @p secret_len is 0 or exceeds
 *     AXL_TPM_SEAL_MAX_SECRET, or @p pcr_count is 0 or names a PCR > 23;
 *     AXL_ERR if no TPM (`!axl_tpm_present()`) or a TPM command fails (see
 *     @p err for which command and its responseCode).
 */
AXL_WARN_UNUSED AxlStatus
axl_tpm_seal(
    const uint8_t  *secret,        ///< secret bytes to seal
    size_t          secret_len,    ///< secret length (1..AXL_TPM_SEAL_MAX_SECRET)
    const uint32_t *pcrs,          ///< PCR indices to bind to (each 0..23)
    size_t          pcr_count,     ///< number of PCRs in @p pcrs
    uint8_t       **out_blob,      ///< [out] sealed blob (free with axl_free)
    size_t         *out_blob_len,  ///< [out] sealed blob length
    AxlTpmError    *err            ///< [out] failing stage + TPM rc; NULL to ignore
);

/**
 * @brief Unseal a blob produced by axl_tpm_seal.
 *
 * Recovers the secret only if the current PCR values satisfy the policy
 * baked into the blob at seal time. The recovered secret is returned in
 * @p out_secret, allocated with axl_malloc — free it with `axl_free`.
 *
 * @return AXL_OK on success; AXL_INVALID if an argument is NULL or the
 *     blob is malformed; AXL_DENIED if the current PCRs do not satisfy the
 *     seal-time policy (the firmware/measured state changed); AXL_ERR if
 *     no TPM or a TPM command fails (see @p err for which command and its
 *     responseCode).
 */
AXL_WARN_UNUSED AxlStatus
axl_tpm_unseal(
    const uint8_t *blob,            ///< sealed blob from axl_tpm_seal
    size_t         blob_len,        ///< blob length
    uint8_t      **out_secret,      ///< [out] recovered secret (free with axl_free)
    size_t        *out_secret_len,  ///< [out] recovered secret length
    AxlTpmError   *err              ///< [out] failing stage + TPM rc; NULL to ignore
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_TPM_H */
