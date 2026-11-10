/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-crypto.h
 *
 * Generic public-key signature verification. A detached-signature
 * verifier over the mbedTLS that AXL links in every build.
 *
 * This is a low-level cryptographic primitive — the building block for
 * verifying signed firmware updates, signed configuration blobs, or
 * Secure-Boot-style image checks against a public key the consumer
 * ships. There is no signing side: signing is done offline by the
 * vendor and the private key never ships in the binary.
 *
 * Compiled into every build — mbedTLS is an unconditional dependency,
 * so axl_pk_available() is a guaranteed true and not worth branching
 * on. axl_pk_alg_available() answers the question that can still be
 * no — whether THIS IMAGE linked a given algorithm. Use it to
 * distinguish "algorithm not linked" from "signature invalid".
 *
 * **Every key-handle algorithm is opt-in, in code.** An image carries
 * an algorithm only if it calls that algorithm's enable function —
 * calling it is what links the provider, exactly as calling
 * axl_tls_init() is what links TLS:
 *
 * - axl_pk_enable_mbedtls() — ECDSA P-256 / P-384 and RSA;
 * - axl_pk_enable_ed25519() — Ed25519.
 *
 * Some callers make the call for you. AxlJose enables mbedTLS itself,
 * and so does every ECDSA/RSA-specific function in this header —
 * axl_pk_verify() and the example below included — because they live
 * in the provider's own object. AxlSsh enables Ed25519 itself. Only an
 * image that reaches an algorithm through the generic axl_pk_key_new() /
 * axl_pk_key_load_private() / axl_pk_key_load_public() /
 * axl_pk_key_from_raw_public() alone must call the function; without it
 * those calls return NULL and log one error line naming the function.
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

// ---------------------------------------------------------------------------
// Constant-time comparison
// ---------------------------------------------------------------------------

/**
 * @brief Compare two equal-length buffers in constant time.
 *
 * Runs in time that depends only on @p len, never on the byte values or
 * on where the first difference is — it OR-accumulates the per-byte
 * difference over the whole buffer with no early exit. Use it for EVERY
 * secret-dependent comparison (MACs, auth tags, session tokens, password
 * verifiers, SCRAM proofs); a plain `memcmp` leaks the match length
 * through timing and enables byte-at-a-time forgery.
 *
 * Both buffers MUST be at least @p len bytes. Depends on nothing but
 * the compiler.
 *
 * Length is NOT part of the comparison. For a variable- or
 * attacker-influenced secret (e.g. a session token), the caller MUST
 * confirm both lengths equal the expected secret length BEFORE calling —
 * otherwise an attacker supplying a 1-byte value has only 1 byte checked.
 * Compare against a fixed expected length, never against the attacker's.
 *
 * @return true iff the two buffers are byte-for-byte equal. A @p len of
 *     0 compares equal; a NULL buffer with @p len > 0 returns false.
 */
bool
axl_consttime_equal(
    const void *a,    ///< first buffer
    const void *b,    ///< second buffer
    size_t      len   ///< number of bytes to compare
);

// ---------------------------------------------------------------------------
// Entropy policy
// ---------------------------------------------------------------------------

/**
 * @brief Permit key material to be derived from a clock when the
 *     firmware publishes no hardware RNG.
 *
 * Every private key this SDK produces -- SSH host keys, X25519 and
 * P-256 ephemerals, RSA and ECDSA key pairs, TLS session secrets --
 * comes out of one process-global CTR-DRBG, and that DRBG is seeded
 * from a single entropy source: `EFI_RNG_PROTOCOL` (UEFI 2.11 §37.5).
 *
 * When the protocol is absent or its `GetRNG` fails, the default is to
 * FAIL: the entropy source reports failure, seeding fails, and every
 * call that needs randomness returns its error or NULL sentinel. No
 * key is produced. That is deliberate -- a key nobody can generate is
 * strictly better than a key an attacker can guess.
 *
 * Passing true opts a platform out of that. The seed then comes from a
 * xorshift generator started from the real-time clock and the
 * firmware's monotonic counter. **In plain terms: keys become
 * predictable to anyone who can bound when the machine booted.** An
 * attacker who knows the boot happened inside a one-second window
 * searches a space small enough to enumerate on a laptop -- which for
 * an SSH host key means impersonating the host, and for an ephemeral
 * means decrypting the session. Enable it only on a platform that
 * genuinely publishes no RNG, only when the operator has accepted that
 * exposure, and never for a key that outlives the boot.
 *
 * Every draw that actually falls back logs at warning level, so the
 * choice is visible in a production image rather than only under `-v`.
 *
 * ORDERING. Seeding is LAZY and the seed is cached for the lifetime of
 * the process: the DRBG is seeded by the first call that needs
 * randomness -- axl_pk_key_new(), axl_ecdh_new(), axl_aead_seal(),
 * axl_tls_init(), axl_ssh_server_new(), or any TLS handshake -- and
 * the result is reused from then on. Call this BEFORE any of them.
 * Enabling it afterwards does not retroactively rescue a seed that
 * already failed on this call, though a later seeding attempt (each
 * failed attempt is retried on the next call) will honor the new
 * setting. Disabling it afterwards does not un-taint a DRBG already
 * seeded from a clock.
 *
 * Not thread-safe against a concurrent seeding attempt; set it once
 * during startup, from the same context that will use the crypto.
 */
void
axl_crypto_set_weak_entropy_allowed(
    bool allowed   ///< true to permit the clock-seeded fallback; false
                   ///< (the default) to fail when no hardware RNG is
                   ///< available
);

/**
 * @brief Report whether the clock-seeded entropy fallback is permitted.
 *
 * Answers what axl_crypto_set_weak_entropy_allowed() was last set to,
 * NOT whether the fallback has been used or whether a hardware RNG
 * exists. A false here means seeding will fail on a platform with no
 * `EFI_RNG_PROTOCOL`; it does not promise that this platform has one.
 * To ask whether the firmware publishes an RNG, call axl_rng_bytes()
 * (@ref axl-rng.h) -- it never falls back.
 *
 * @return true if the fallback is permitted, false (the default) if
 *     the absence of a hardware RNG is a hard failure.
 */
bool
axl_crypto_is_weak_entropy_allowed(
    void
);

/**
 * @brief Public-key signature algorithm selector.
 *
 * The numeric values are part of the contract (a consumer may persist
 * the choice alongside a signed blob); new algorithms only extend the
 * range.
 */
typedef enum {
    AXL_PK_ED25519    = 0,  /**< Ed25519 (RFC 8032). Supported by the
                                 key-handle API, but **opt-in** — call
                                 axl_pk_enable_ed25519(); see also
                                 axl_pk_alg_available().
                                 NOT supported by the raw-bytes
                                 axl_pk_verify() below, which returns
                                 AXL_ERR for it. Because this is value 0,
                                 a zero-initialized selector names this
                                 algorithm rather than nothing — always
                                 set the algorithm explicitly. */
    AXL_PK_ECDSA_P256 = 1,  /**< ECDSA over NIST P-256 (prime256v1) with
                                 SHA-256. Opt-in for the key-handle API,
                                 as are P-384 and RSA — call
                                 axl_pk_enable_mbedtls(). */
    AXL_PK_RSA        = 2,   /**< RSA with PKCS#1 v1.5 over SHA-256
                                 (rsa-sha2-256). Keygen produces a 3072-bit
                                 key. Supported by the key-handle API
                                 (axl_pk_key_new / _sign / _verify); NOT by
                                 the raw-bytes axl_pk_verify() below. */
    AXL_PK_ECDSA_P384 = 3    /**< ECDSA over NIST P-384 (secp384r1) with
                                 SHA-384 (the hash follows the curve). Like
                                 P-256 it is supported by the key-handle API
                                 (axl_pk_key_new / _sign / _verify); NOT by
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
 * @brief Whether public-key signature verification is present in this build.
 *
 * Always true, guaranteed. mbedTLS is an unconditional dependency, so
 * this cannot answer no — do not branch on it. (Linking this function
 * links no algorithm.) It stays public API
 * only so existing consumer source keeps compiling.
 *
 * The question that CAN still answer no is whether this image enabled a
 * particular algorithm for the key-handle API: use
 * axl_pk_alg_available() — never this.
 *
 * @return always true.
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
 * Needs no enable call: this function lives in the mbedTLS provider's
 * object, so calling it enables ECDSA/RSA in the image by itself (see
 * axl_pk_enable_mbedtls()). #AXL_PK_ED25519 is not supported by *this*
 * entry point and returns AXL_ERR, even in an image that enabled the
 * Ed25519 provider — use the key-handle API (axl_pk_key_load_public() +
 * axl_pk_key_verify()) for it.
 *
 * Operates only on public inputs; there is no secret to leak. Any
 * parse or verify failure is reported uniformly as AXL_ERR — a caller
 * must treat every non-AXL_OK result as "not verified, untrusted" and
 * fail closed. Do NOT branch security decisions on the *reason* for
 * failure.
 *
 * @return AXL_OK if the signature is valid for @p msg under @p pubkey;
 *     AXL_ERR otherwise — invalid signature, malformed key/signature,
 *     NULL/zero-length key or signature, or an algorithm this entry
 *     point does not support (anything but #AXL_PK_ECDSA_P256, which
 *     includes the reserved #AXL_PK_ED25519).
 */
AXL_WARN_UNUSED int
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
// These need an algorithm this image actually linked. Where one is
// absent, axl_pk_key_new / _load_* return NULL and the operations
// return AXL_ERR (see axl_pk_alg_available()).

/**
 * @brief An opaque public-key key pair (or public-only key).
 *
 * Holds a private key (from axl_pk_key_new / axl_pk_key_load_private) or
 * a public key only (from axl_pk_key_load_public). Free with
 * axl_pk_key_free().
 */
typedef struct AxlPkKey AxlPkKey;

/**
 * @brief Generate a new key pair.
 *
 * @c AXL_PK_ECDSA_P256 generates a NIST P-256 key; @c AXL_PK_ECDSA_P384
 * a NIST P-384 key; @c AXL_PK_RSA generates a 3072-bit RSA key (slower —
 * seconds on some firmware, but a one-time cost for a persisted host key).
 * Every algorithm is available only in an image that enabled it (see
 * the file overview); otherwise this returns NULL and logs one error
 * line naming the enable function to call. Check
 * axl_pk_alg_available() first if the algorithm is chosen at runtime.
 *
 * @return a new private key handle (caller frees with axl_pk_key_free),
 *     or NULL on failure / unsupported algorithm / algorithm not linked.
 */
AxlPkKey *
axl_pk_key_new(
    AxlPkAlg  alg  ///< key algorithm to generate
);

/**
 * @brief Load a private key from its PKCS#8 DER encoding.
 *
 * @p der is an unencrypted PKCS#8 PrivateKeyInfo (the output of
 * `openssl pkcs8 -topk8 -nocrypt -outform DER`, and of
 * axl_pk_key_get_private_der()).
 *
 * Only the providers this image enabled are tried (see the file
 * overview). DER naming an algorithm whose provider is not enabled
 * returns NULL and logs one error line naming the function to call.
 *
 * @return a new key handle, or NULL on malformed input / unsupported
 *     key type / key type not linked.
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
 * Same provider rule as axl_pk_key_load_private().
 *
 * @return a new key handle, or NULL on malformed input / unsupported
 *     key type / key type not linked.
 */
AxlPkKey *
axl_pk_key_load_public(
    const uint8_t *der,  ///< SubjectPublicKeyInfo DER public key
    size_t         len   ///< length in bytes (> 0)
);

/**
 * @brief Import a public key from its raw, algorithm-native encoding.
 *
 * "Raw" means the encoding the algorithm's own ecosystem uses on the
 * wire, not a DER container:
 *
 * - #AXL_PK_ECDSA_P256 / #AXL_PK_ECDSA_P384 — an uncompressed SEC1
 *   point, `0x04 || X || Y`: 65 bytes for P-256, 97 for P-384.
 * - #AXL_PK_ED25519 — the 32-byte compressed point (RFC 8032 §5.1.2).
 *   Available only in an image that calls axl_pk_enable_ed25519().
 * - #AXL_PK_RSA has no meaningful raw form and is always rejected —
 *   use axl_pk_key_load_public() with DER.
 *
 * Use axl_pk_key_load_public() when you have a DER
 * `SubjectPublicKeyInfo` instead.
 *
 * @return a new public-only key handle, or NULL if @a alg has no raw
 *     form, @a raw is NULL, @a len is wrong for @a alg, the bytes are
 *     not a valid point, or the algorithm is unavailable in this build.
 */
AxlPkKey *
axl_pk_key_from_raw_public(
    AxlPkAlg       alg,  ///< algorithm the bytes encode
    const uint8_t *raw,  ///< raw public key bytes
    size_t         len   ///< length of @a raw in bytes
);

/**
 * @brief Export a public key in its raw, algorithm-native encoding.
 *
 * The inverse of axl_pk_key_from_raw_public(), and the same encodings.
 * Works on a public-only handle and on one that also holds a private
 * key; only public material is ever written.
 *
 * Uses the output-buffer protocol of axl_pk_key_get_private_der(): call
 * with @a out NULL to learn the required size in @a *len, then again
 * with a buffer at least that large.
 *
 * @return #AXL_OK on success; #AXL_ERR if @a key or @a len is NULL, the
 *     key's algorithm has no raw form, or @a out is non-NULL and
 *     @a *len is too small (in which case @a *len is set to the size
 *     needed).
 */
int
axl_pk_key_get_raw_public(
    const AxlPkKey *key,  ///< key handle
    uint8_t        *out,  ///< destination, or NULL to query the size
    size_t         *len   ///< [in/out] buffer size / bytes written
);

/**
 * @brief Whether a public-key algorithm is usable in this image.
 *
 * Distinct from axl_pk_available(), which reports whether the
 * public-key layer is present at all. An algorithm can be absent from
 * an image that has the layer, because every algorithm is opt-in (see
 * axl_pk_enable_mbedtls() and axl_pk_enable_ed25519()):
 *
 * - #AXL_PK_ED25519 — its precomputed base-point table is ~109 KB of
 *   resident `.rodata`, ~125 KB all told, measured.
 * - #AXL_PK_ECDSA_P256, #AXL_PK_ECDSA_P384 and #AXL_PK_RSA share one
 *   provider over mbedTLS's PK layer, ~18 KB of a compressed driver
 *   (RSA alone cannot be dropped: that layer reaches RSA by itself).
 *
 * This query is silent. The constructors are not: when one is asked for
 * an algorithm whose provider the image did not enable it logs, once
 * per call, an error line naming the function to call.
 *
 * Check this before axl_pk_key_new() when the algorithm is chosen at
 * runtime — it distinguishes "not linked" from "key generation
 * failed", which both otherwise surface as NULL.
 *
 * @return true if @a alg can be used in this image.
 */
bool
axl_pk_alg_available(
    AxlPkAlg alg  ///< algorithm to query
);

/**
 * @brief Enable ECDSA P-256 / P-384 and RSA for the key-handle API.
 *
 * The opt-in is the CALL, not anything it does at run time: this
 * function lives in the mbedTLS provider's object, so referencing it
 * links that provider — and mbedTLS's PK layer with it — into the
 * image. An image that never calls it (and uses neither AxlJose nor an
 * ECDSA/RSA-specific function, which call it for you) carries none of
 * that code, and axl_pk_alg_available() answers false for those
 * algorithms.
 *
 * Idempotent and safe at any point: before or after any other AxlPk
 * call, any number of times. Keys created before the call are
 * unaffected — there can be none of these algorithms, since the
 * provider was linked or it was not.
 */
void
axl_pk_enable_mbedtls(void);

/**
 * @brief Enable Ed25519 for the key-handle API.
 *
 * Same shape as axl_pk_enable_mbedtls(): calling it links the Ed25519
 * provider, whose precomputed base-point table is ~109 KB of
 * `.rodata`. AxlSsh calls it for you (its only host-key algorithm is
 * `ssh-ed25519`). Idempotent and safe at any point.
 */
void
axl_pk_enable_ed25519(void);

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
 * @warning A NULL @p key returns #AXL_PK_ED25519, because that is the
 *     enum's zero value — and it now names a REAL, usable algorithm
 *     rather than a reserved one. So this function cannot distinguish
 *     "an Ed25519 key" from "no key at all". Check the handle for NULL
 *     yourself; do not infer it from the return. The value is kept as
 *     it is deliberately: the alternatives are returning something
 *     outside AxlPkAlg's range from a function typed to return one, or
 *     adding an enumerator no caller has asked for.
 *
 * @return the AxlPkAlg of @p key — #AXL_PK_ED25519, #AXL_PK_ECDSA_P256,
 *     #AXL_PK_ECDSA_P384 or #AXL_PK_RSA. See the warning above for a
 *     NULL handle.
 */
AxlPkAlg
axl_pk_key_alg(
    const AxlPkKey *key  ///< key handle
);

/**
 * @brief Sign a message with a private key.
 *
 * Hashes @p msg with the digest the key's algorithm specifies (SHA-256
 * for P-256 and RSA, SHA-384 for P-384) and signs it. For ECDSA, @p fmt
 * selects the signature byte layout (#AXL_PK_SIG_RAW for SSH/JWS/COSE,
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
 * signature over the digest the key's algorithm specifies (SHA-256 for
 * P-256 and RSA, SHA-384 for P-384) using @p key (private or public).
 * For ECDSA, @p fmt must match how the signature was produced, and for
 * #AXL_PK_SIG_RAW @p sig_len must be exactly the curve's r||s size
 * (64 bytes for P-256). For RSA, @p fmt is ignored.
 *
 * Any non-AXL_OK result means "not verified, untrusted" — fail closed.
 * Test `!= AXL_OK`, never `== AXL_ERR`: there is more than one refusal.
 *
 * One of them also says WHY, for a caller that reports refusals: an
 * #AXL_PK_ED25519 check that could not be made because memory ran out
 * returns #AXL_NO_RESOURCES before the signature is examined at all, which
 * is a statement about this machine and not about the signer -- a server
 * that treats a bad signature as an attack must not treat this as one.
 * That allocation is 2 x (64 + @p msg_len) bytes, so it is size-driven as
 * well as pressure-driven: a caller that classifies on this code must bound
 * the message length itself, or a signer that controls it chooses the
 * answer. (A failure inside Ed25519's own hash is AXL_ERR too, although it
 * is not the signer's doing either. It is reported as a verdict because it
 * cannot happen in a supported configuration -- the build refuses the
 * mbedTLS options that would allow it -- and "not verified" is the safe
 * reading of a check that went wrong. The cost, if a future configuration
 * ever made it reachable: a caller that treats AXL_ERR as forgery, as
 * AxlSsh does, would blame the peer for the verifier's fault.)
 *
 * **The mbedTLS-backed algorithms (ECDSA, RSA) do not separate that case,
 * deliberately: every failure of theirs is AXL_ERR.** mbedTLS reports an
 * oversize integer as `MBEDTLS_ERR_MPI_ALLOC_FAILED` too (`mbedtls_mpi_grow`
 * refuses more than `MBEDTLS_MPI_MAX_LIMBS` with that code, before any
 * allocation), and the SIGNER chooses the integers' size. Mapping it to
 * #AXL_NO_RESOURCES would let a peer turn a forgery into "could not check"
 * at will, which is exactly the charge a caller distinguishing the two is
 * trying not to drop. Do not "complete" the mapping.
 *
 * @return AXL_OK if the signature is valid; #AXL_NO_RESOURCES when an
 *     Ed25519 check could not be made for lack of memory (NOT verified);
 *     AXL_ERR otherwise.
 */
AXL_WARN_UNUSED int
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
// Compiled into every build (mbedTLS).

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
AXL_WARN_UNUSED int
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
AXL_WARN_UNUSED int
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
// Compiled into every build (mbedTLS).

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
// Compiled into every build (mbedTLS).

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
