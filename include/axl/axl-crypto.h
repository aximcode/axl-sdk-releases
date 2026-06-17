/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-crypto.h:
 *
 * Generic public-key signature verification. A detached-signature
 * verifier over the mbedTLS that AXL links when built with AXL_TLS=1.
 *
 * This is a low-level cryptographic primitive — the building block for
 * verifying signed firmware updates, signed configuration blobs, or
 * Secure-Boot-style image checks against a public key the consumer
 * ships. There is no signing side: signing is done offline by the
 * vendor and the private key never ships in the binary.
 *
 * Optional — like axl-tls.h, the real implementation requires AXL_TLS=1
 * at build time. Without it, axl_pk_available() returns false and
 * axl_pk_verify() returns AXL_ERR (fail-closed). Use axl_pk_available()
 * to distinguish "verification not compiled in" from "signature
 * invalid".
 *
 * @code
 * // pubkey: DER SubjectPublicKeyInfo baked into the image at build time.
 * // sig:    detached DER ECDSA signature over msg (ECDSA-with-SHA-256).
 * if (axl_pk_verify(AXL_PK_ECDSA_P256,
 *                   pubkey, pubkey_len,
 *                   msg, msg_len,
 *                   sig, sig_len) == AXL_OK) {
 *     // signature is valid — msg is authentic
 * }
 * @endcode
 */

#ifndef AXL_CRYPTO_H
#define AXL_CRYPTO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Public-key signature algorithm selector.
 *
 * The numeric values are part of the contract (a consumer may persist
 * the choice alongside a signed blob); new algorithms only extend the
 * range.
 */
typedef enum {
    AXL_PK_ED25519    = 0,  /**< Ed25519 — reserved; NOT supported by the
                                 current mbedTLS build (requires PSA crypto,
                                 which AXL does not enable). axl_pk_verify()
                                 returns AXL_ERR for this algorithm. Because
                                 this is value 0, a zero-initialized selector
                                 fails closed (nothing verifies) — always set
                                 #AXL_PK_ECDSA_P256 explicitly. */
    AXL_PK_ECDSA_P256 = 1,  /**< ECDSA over NIST P-256 (prime256v1) with
                                 SHA-256. */
    AXL_PK_RSA        = 2,   /**< RSA with PKCS#1 v1.5 over SHA-256
                                 (rsa-sha2-256). Keygen produces a 3072-bit
                                 key. Supported by the key-handle API
                                 (axl_pk_keygen / _sign / _verify); NOT by
                                 the raw-bytes axl_pk_verify() below. */
    AXL_PK_ECDSA_P384 = 3    /**< ECDSA over NIST P-384 (secp384r1) with
                                 SHA-384 (the hash follows the curve). Like
                                 P-256 it is supported by the key-handle API
                                 (axl_pk_keygen / _sign / _verify); NOT by
                                 the raw-bytes axl_pk_verify(). Its
                                 #AXL_PK_SIG_RAW signature is r||s = 96
                                 bytes (48-byte order each). */
} AxlPkAlg;

/**
 * @brief Detached ECDSA signature byte layout.
 */
typedef enum {
    AXL_PK_SIG_DER = 0,  /**< ASN.1 DER: SEQUENCE { INTEGER r, INTEGER s }
                              (OpenSSL / X.509 form). The format the
                              raw-bytes axl_pk_verify() expects. */
    AXL_PK_SIG_RAW = 1   /**< Fixed-width r || s, each big-endian and
                              left-padded to the curve order size (64 bytes
                              for P-256, 96 for P-384). The form SSH, JWS,
                              and COSE use. Ignored for RSA (its signature
                              has a single encoding). */
} AxlPkSigFormat;

/**
 * @brief Whether public-key signature verification was compiled in.
 *
 * True only when AXL was built with AXL_TLS=1 (which links mbedTLS).
 * When false, axl_pk_verify() always returns AXL_ERR regardless of
 * input — callers that must fail closed on a missing crypto backend
 * can branch on this to log the distinction.
 *
 * @return true if axl_pk_verify() can verify signatures.
 */
bool
axl_pk_available(void);

/**
 * @brief Verify a detached signature over a message with a public key.
 *
 * Pure verification — there is no signing side in this API.
 *
 * Encodings (for #AXL_PK_ECDSA_P256):
 *   - @p pubkey is a DER-encoded SubjectPublicKeyInfo for a P-256 key
 *     (the output of `openssl ec -pubout -outform DER`). PEM is not
 *     accepted — convert to DER first.
 *   - @p sig is a DER-encoded ECDSA signature: SEQUENCE { INTEGER r,
 *     INTEGER s } (the output of `openssl dgst -sha256 -sign`). It must
 *     contain exactly the signature, with no trailing bytes.
 *   - @p msg is the raw message bytes; this function computes
 *     SHA-256(@p msg) internally (ECDSA-with-SHA-256). @p msg may be
 *     NULL only when @p msg_len is 0.
 *
 * #AXL_PK_ED25519 is reserved and unsupported by this build; it
 * returns AXL_ERR.
 *
 * Operates only on public inputs; there is no secret to leak. Any
 * parse or verify failure is reported uniformly as AXL_ERR — a caller
 * must treat every non-AXL_OK result as "not verified, untrusted" and
 * fail closed. Do NOT branch security decisions on the *reason* for
 * failure; the only distinction the API offers is axl_pk_available(),
 * which separates "verification not compiled in" from "invalid".
 *
 * @return AXL_OK if the signature is valid for @p msg under @p pubkey;
 *     AXL_ERR otherwise — invalid signature, malformed key/signature,
 *     NULL/zero-length key or signature, unsupported algorithm, or
 *     verification not compiled in (see axl_pk_available()).
 */
int
axl_pk_verify(
    AxlPkAlg       alg,         ///< signature algorithm
    const uint8_t *pubkey,      ///< public key bytes (encoding per @p alg)
    size_t         pubkey_len,  ///< public key length in bytes (> 0)
    const uint8_t *msg,         ///< message bytes (NULL iff @p msg_len == 0)
    size_t         msg_len,     ///< message length in bytes
    const uint8_t *sig,         ///< detached signature bytes
    size_t         sig_len      ///< signature length in bytes (> 0)
);

// ===================================================================
// Key handles — generation, serialization, signing, verification
// ===================================================================
//
// The functions above verify against a public key passed as raw bytes
// each call. For the signing side — generating a host/identity key,
// persisting it, and signing or verifying many messages with it — use
// an AxlPkKey handle: an opaque private or public key the rest of this
// section operates on.
//
// All of these require an AXL_TLS=1 build (mbedTLS); without it
// axl_pk_keygen / _load_* return NULL and the operations return AXL_ERR
// (see axl_pk_available()).

/**
 * @brief An opaque public-key key pair (or public-only key).
 *
 * Holds a private key (from axl_pk_keygen / axl_pk_key_load_private) or
 * a public key only (from axl_pk_key_load_public). Free with
 * axl_pk_key_free().
 */
typedef struct AxlPkKey AxlPkKey;

/**
 * @brief Generate a new key pair.
 *
 * @ref AXL_PK_ECDSA_P256 generates a NIST P-256 key; @ref AXL_PK_ECDSA_P384
 * a NIST P-384 key; @ref AXL_PK_RSA generates a 3072-bit RSA key (slower —
 * seconds on some firmware, but a one-time cost for a persisted host key).
 * @ref AXL_PK_ED25519 is unsupported and returns NULL.
 *
 * @return a new private key handle (caller frees with axl_pk_key_free),
 *     or NULL on failure / unsupported algorithm / TLS not compiled in.
 */
AxlPkKey *
axl_pk_keygen(
    AxlPkAlg  alg  ///< key algorithm to generate
);

/**
 * @brief Load a private key from its PKCS#8 DER encoding.
 *
 * @p der is an unencrypted PKCS#8 PrivateKeyInfo (the output of
 * `openssl pkcs8 -topk8 -nocrypt -outform DER`, and of
 * axl_pk_key_get_private_der()).
 *
 * @return a new key handle, or NULL on malformed input / unsupported
 *     key type / TLS not compiled in.
 */
AxlPkKey *
axl_pk_key_load_private(
    const uint8_t *der,  ///< PKCS#8 DER private key
    size_t         len   ///< length in bytes (> 0)
);

/**
 * @brief Load a public key from its SubjectPublicKeyInfo DER encoding.
 *
 * Same encoding the raw-bytes axl_pk_verify() accepts. The resulting
 * handle can verify but not sign.
 *
 * @return a new key handle, or NULL on malformed input / unsupported
 *     key type / TLS not compiled in.
 */
AxlPkKey *
axl_pk_key_load_public(
    const uint8_t *der,  ///< SubjectPublicKeyInfo DER public key
    size_t         len   ///< length in bytes (> 0)
);

/**
 * @brief Serialize a key's private half as PKCS#8 DER.
 *
 * Output-buffer protocol (shared by axl_pk_key_get_public_der and
 * axl_pk_key_sign):
 *   - @p out == NULL: a size query — writes the exact required size to
 *     @p *len and returns AXL_OK.
 *   - @p out != NULL: @p *len is the buffer capacity on entry. On
 *     success @p *len is set to the bytes written. If the buffer is too
 *     small (including @p *len == 0), returns AXL_ERR, sets @p *len to
 *     the required size, and leaves @p out untouched — so the caller can
 *     re-call with a buffer of that size (the same @p *len may be passed
 *     straight through from a prior size query).
 *
 * Fails if the handle has no private key.
 *
 * @return AXL_OK on success; AXL_ERR on a public-only key, a too-small
 *     buffer, bad args, or TLS not compiled in.
 */
int
axl_pk_key_get_private_der(
    const AxlPkKey *key,  ///< key handle (must hold a private key)
    uint8_t        *out,  ///< output buffer, or NULL to size-query
    size_t         *len   ///< [in/out] buffer capacity / bytes written
);

/**
 * @brief Serialize a key's public half as SubjectPublicKeyInfo DER.
 *
 * Output-buffer protocol as in axl_pk_key_get_private_der().
 *
 * @return AXL_OK on success; AXL_ERR on a too-small buffer, bad args, or
 *     TLS not compiled in.
 */
int
axl_pk_key_get_public_der(
    const AxlPkKey *key,  ///< key handle
    uint8_t        *out,  ///< output buffer, or NULL to size-query
    size_t         *len   ///< [in/out] buffer capacity / bytes written
);

/**
 * @brief Report a key handle's algorithm.
 *
 * @return the AxlPkAlg of @p key (AXL_PK_ECDSA_P256, AXL_PK_ECDSA_P384, or
 *     AXL_PK_RSA). Returns AXL_PK_ED25519 (the reserved zero value) for a
 *     NULL handle.
 */
AxlPkAlg
axl_pk_key_alg(
    const AxlPkKey *key  ///< key handle
);

/**
 * @brief Sign a message with a private key.
 *
 * Computes SHA-256(@p msg) and signs it. For ECDSA, @p fmt selects the
 * signature byte layout (#AXL_PK_SIG_RAW for SSH/JWS/COSE,
 * #AXL_PK_SIG_DER for X.509-style); for RSA @p fmt is ignored.
 *
 * Uses the output-buffer protocol of axl_pk_key_get_private_der() on
 * @p sig / @p sig_len (pass @p sig == NULL to size-query). Note ECDSA
 * DER signatures are variable-length (r/s leading-zero trimming), so a
 * size query reports a safe upper bound; #AXL_PK_SIG_RAW is fixed-width
 * (64 bytes for P-256).
 *
 * @return AXL_OK on success; AXL_ERR on a public-only key, a too-small
 *     buffer, bad args, or TLS not compiled in.
 */
int
axl_pk_key_sign(
    const AxlPkKey *key,      ///< signing key (must hold a private key)
    const uint8_t  *msg,      ///< message bytes (NULL iff @p msg_len == 0)
    size_t          msg_len,  ///< message length in bytes
    AxlPkSigFormat  fmt,      ///< ECDSA signature layout (ignored for RSA)
    uint8_t        *sig,      ///< output buffer, or NULL to size-query
    size_t         *sig_len   ///< [in/out] buffer capacity / bytes written
);

/**
 * @brief Verify a detached signature with a key handle.
 *
 * The handle-based peer of the raw-bytes axl_pk_verify(): verifies a
 * signature over SHA-256(@p msg) using @p key (private or public). For
 * ECDSA, @p fmt must match how the signature was produced, and for
 * #AXL_PK_SIG_RAW @p sig_len must be exactly the curve's r||s size
 * (64 bytes for P-256). For RSA, @p fmt is ignored.
 *
 * Any non-AXL_OK result means "not verified, untrusted" — fail closed.
 *
 * @return AXL_OK if the signature is valid; AXL_ERR otherwise.
 */
int
axl_pk_key_verify(
    const AxlPkKey *key,      ///< verifying key
    const uint8_t  *msg,      ///< message bytes (NULL iff @p msg_len == 0)
    size_t          msg_len,  ///< message length in bytes
    AxlPkSigFormat  fmt,      ///< ECDSA signature layout (ignored for RSA)
    const uint8_t  *sig,      ///< detached signature bytes
    size_t          sig_len   ///< signature length in bytes (> 0)
);

/**
 * @brief Free a key handle. NULL-safe. Zeroizes private key material.
 */
void
axl_pk_key_free(
    AxlPkKey *key  ///< key handle to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlPkKey, axl_pk_key_free)
#endif

// ===================================================================
// Authenticated encryption (AEAD)
// ===================================================================
//
// One-shot authenticated encryption with associated data. The caller
// supplies a fresh nonce per message (AEAD security collapses if a
// (key, nonce) pair is ever reused) — typically a counter or random
// value the caller manages; this API does not generate nonces.
//
// Requires an AXL_TLS=1 build (mbedTLS); without it the calls return
// AXL_ERR (see axl_pk_available()).

/**
 * @brief AEAD algorithm selector.
 */
typedef enum {
    AXL_AEAD_AES_128_GCM       = 0,  /**< AES-128-GCM (16-byte key). */
    AXL_AEAD_AES_256_GCM       = 1,  /**< AES-256-GCM (32-byte key). */
    AXL_AEAD_CHACHA20_POLY1305 = 2   /**< ChaCha20-Poly1305 (32-byte key). */
} AxlAeadAlg;

#define AXL_AEAD_NONCE_LEN  12u  /**< Required nonce length, all algorithms. */
#define AXL_AEAD_TAG_LEN    16u  /**< Authentication tag length. */

/**
 * @brief Encrypt and authenticate a message (AEAD seal).
 *
 * Encrypts @p plaintext and computes an authentication tag over the
 * ciphertext and @p aad. The ciphertext is the same length as the
 * plaintext; the tag is returned separately. @p ciphertext may alias
 * @p plaintext for in-place encryption. @p plaintext / @p aad may be
 * NULL only when their length is 0.
 *
 * @p key_len must match @p alg (16 for AES-128, 32 for AES-256 and
 * ChaCha20-Poly1305). @p nonce_len must be #AXL_AEAD_NONCE_LEN and
 * @p tag_len #AXL_AEAD_TAG_LEN. The caller MUST NOT reuse a
 * (@p key, @p nonce) pair across messages.
 *
 * @return AXL_OK on success; AXL_ERR on bad args, a key/nonce/tag length
 *     mismatch, or TLS not compiled in.
 */
int
axl_aead_seal(
    AxlAeadAlg     alg,         ///< AEAD algorithm
    const uint8_t *key,         ///< key (length per @p alg)
    size_t         key_len,     ///< key length in bytes
    const uint8_t *nonce,       ///< nonce (AXL_AEAD_NONCE_LEN bytes)
    size_t         nonce_len,   ///< nonce length (must be AXL_AEAD_NONCE_LEN)
    const uint8_t *aad,         ///< associated data (NULL iff aad_len == 0)
    size_t         aad_len,     ///< associated-data length
    const uint8_t *plaintext,   ///< plaintext (NULL iff pt_len == 0)
    size_t         pt_len,      ///< plaintext length
    uint8_t       *ciphertext,  ///< [out] ciphertext, pt_len bytes (may alias plaintext)
    uint8_t       *tag,         ///< [out] tag (AXL_AEAD_TAG_LEN bytes)
    size_t         tag_len      ///< tag buffer length (must be AXL_AEAD_TAG_LEN)
);

/**
 * @brief Verify and decrypt a message (AEAD open).
 *
 * Checks the tag over @p ciphertext and @p aad and, only if it is valid,
 * writes the plaintext. On any authentication failure returns AXL_ERR
 * and writes no plaintext (fail closed) — a caller must treat AXL_ERR as
 * "not authentic, discard". @p plaintext may alias @p ciphertext.
 *
 * Length requirements match axl_aead_seal().
 *
 * @return AXL_OK if the tag is valid and decryption succeeded; AXL_ERR
 *     otherwise (bad tag, bad args, length mismatch, or TLS not built).
 */
int
axl_aead_open(
    AxlAeadAlg     alg,         ///< AEAD algorithm
    const uint8_t *key,         ///< key (length per @p alg)
    size_t         key_len,     ///< key length in bytes
    const uint8_t *nonce,       ///< nonce (AXL_AEAD_NONCE_LEN bytes)
    size_t         nonce_len,   ///< nonce length (must be AXL_AEAD_NONCE_LEN)
    const uint8_t *aad,         ///< associated data (NULL iff aad_len == 0)
    size_t         aad_len,     ///< associated-data length
    const uint8_t *ciphertext,  ///< ciphertext (NULL iff ct_len == 0)
    size_t         ct_len,      ///< ciphertext length
    const uint8_t *tag,         ///< tag (AXL_AEAD_TAG_LEN bytes)
    size_t         tag_len,     ///< tag length (must be AXL_AEAD_TAG_LEN)
    uint8_t       *plaintext    ///< [out] plaintext, ct_len bytes (may alias ciphertext)
);

// ===================================================================
// Elliptic-curve Diffie-Hellman key agreement
// ===================================================================
//
// Ephemeral ECDH: generate a key pair, send the public key to a peer,
// and combine it with the peer's public key into a shared secret. The
// secret must be run through a KDF (e.g. SHA-256) before use as a key.
// For an SSH key exchange (ecdh-sha2-nistp256, curve25519-sha256) and
// any ephemeral-DH handshake.
//
// Requires an AXL_TLS=1 build (mbedTLS); without it axl_ecdh_new()
// returns NULL and the operations return AXL_ERR.

/**
 * @brief ECDH curve selector.
 */
typedef enum {
    AXL_ECDH_P256   = 0,  /**< NIST P-256. Public key is the uncompressed
                               SEC1 point 0x04||X||Y (65 bytes); shared
                               secret is the X coordinate (32 bytes). */
    AXL_ECDH_X25519 = 1   /**< X25519 (Curve25519). Public key and shared
                               secret are 32 bytes each (RFC 7748). */
} AxlEcdhAlg;

/**
 * @brief An ephemeral ECDH key pair.
 *
 * Created by axl_ecdh_new() (which generates the key pair), used to
 * export the public key and compute the shared secret, then freed with
 * axl_ecdh_free().
 */
typedef struct AxlEcdh AxlEcdh;

/**
 * @brief Generate an ephemeral ECDH key pair.
 *
 * @return a new context (caller frees with axl_ecdh_free), or NULL on a
 *     bad algorithm, RNG failure, or TLS not compiled in.
 */
AxlEcdh *
axl_ecdh_new(
    AxlEcdhAlg  alg  ///< curve to generate a key pair on
);

/**
 * @brief Export this context's public key.
 *
 * The encoding is per @p alg (SEC1 uncompressed point for P-256, the
 * 32-byte u-coordinate for X25519). Uses the size-query / output-buffer
 * protocol: @p out == NULL writes the required size to @p *len;
 * otherwise @p *len is the capacity, set on success to the bytes
 * written, and on a too-small buffer returns AXL_ERR with @p *len set to
 * the required size.
 *
 * @return AXL_OK on success; AXL_ERR on a too-small buffer, bad args, or
 *     TLS not compiled in.
 */
int
axl_ecdh_get_public(
    AxlEcdh *e,    ///< ECDH context
    uint8_t *out,  ///< [out] public key, or NULL to size-query
    size_t  *len   ///< [in/out] buffer capacity / bytes written
);

/**
 * @brief Compute the shared secret from a peer's public key.
 *
 * @p peer_pub is the peer's public key in the same encoding
 * axl_ecdh_get_public() produces. The shared secret is 32 bytes for both
 * curves (P-256 X coordinate; X25519 shared u). Output uses the
 * size-query / output-buffer protocol of axl_ecdh_get_public().
 *
 * The raw secret MUST be passed through a KDF/hash before use as a key
 * (it is not uniformly random). Returns AXL_ERR on an invalid peer key
 * (e.g. not on the curve) — fail closed.
 *
 * @return AXL_OK on success; AXL_ERR on a bad peer key, too-small buffer,
 *     bad args, or TLS not compiled in.
 */
int
axl_ecdh_compute(
    AxlEcdh       *e,         ///< ECDH context
    const uint8_t *peer_pub,  ///< peer public key (encoding per the curve)
    size_t         peer_len,  ///< peer public key length
    uint8_t       *out,       ///< [out] shared secret, or NULL to size-query
    size_t        *len        ///< [in/out] buffer capacity / bytes written
);

/**
 * @brief Free an ECDH context. NULL-safe. Zeroizes the private key.
 */
void
axl_ecdh_free(
    AxlEcdh *e  ///< context to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlEcdh, axl_ecdh_free)
#endif

// ===================================================================
// Stream cipher (AES-CTR)
// ===================================================================
//
// A keystream cipher for a continuous byte stream (e.g. an SSH transport
// in CTR mode), where the counter advances across calls. CTR encryption
// and decryption are the same operation, so one call serves both. The
// caller supplies a fresh (key, IV) per stream and never reuses one.
//
// Requires an AXL_TLS=1 build (mbedTLS); without it axl_cipher_ctr_new()
// returns NULL and axl_cipher_ctr_xcrypt() returns AXL_ERR.

#define AXL_CIPHER_CTR_IV_LEN  16u  /**< AES-CTR initial counter block size. */

/**
 * @brief AES-CTR key size selector.
 */
typedef enum {
    AXL_CIPHER_AES_128_CTR = 0,  /**< AES-128-CTR (16-byte key). */
    AXL_CIPHER_AES_256_CTR = 1   /**< AES-256-CTR (32-byte key). */
} AxlCipherAlg;

/**
 * @brief An AES-CTR keystream context.
 *
 * Holds the running counter and keystream position so successive
 * axl_cipher_ctr_xcrypt() calls continue one keystream. Free with
 * axl_cipher_free().
 */
typedef struct AxlCipher AxlCipher;

/**
 * @brief Create an AES-CTR context.
 *
 * @p key_len must match @p alg (16 for AES-128, 32 for AES-256). @p iv
 * is the 16-byte initial counter block. A (key, IV) pair must never be
 * reused across streams.
 *
 * @return a new context (caller frees with axl_cipher_free), or NULL on
 *     a key-length mismatch, bad args, or TLS not compiled in.
 */
AxlCipher *
axl_cipher_ctr_new(
    AxlCipherAlg   alg,      ///< AES-CTR key size
    const uint8_t *key,      ///< key (length per @p alg)
    size_t         key_len,  ///< key length in bytes
    const uint8_t *iv        ///< AXL_CIPHER_CTR_IV_LEN-byte counter block
);

/**
 * @brief Encrypt or decrypt @p len bytes, advancing the keystream.
 *
 * CTR is symmetric — the same call encrypts and decrypts. @p out may
 * alias @p in for in-place operation. @p len need not be a multiple of
 * the block size; the keystream position carries across calls so a
 * stream may be processed in arbitrary chunks. @p in / @p out may be
 * NULL only when @p len is 0.
 *
 * @return AXL_OK on success; AXL_ERR on bad args or TLS not compiled in.
 */
int
axl_cipher_ctr_xcrypt(
    AxlCipher     *c,    ///< cipher context
    const uint8_t *in,   ///< input bytes (NULL iff len == 0)
    size_t         len,  ///< number of bytes
    uint8_t       *out   ///< [out] output bytes, len bytes (may alias in)
);

/**
 * @brief Free a cipher context. NULL-safe. Zeroizes key material.
 */
void
axl_cipher_free(
    AxlCipher *c  ///< context to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlCipher, axl_cipher_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_CRYPTO_H */
