/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-scram.h
    Server-side SCRAM-SHA-256 authentication (RFC 5802 / RFC 7677).

    SCRAM lets a server authenticate a client over a password without the
    server ever storing — or seeing on the wire — a password-equivalent.
    At enrollment the server runs `axl_scram_sha256_derive` and keeps only
    `{salt, iterations, StoredKey, ServerKey}` (an @ref AxlScramCredential);
    the password is never persisted. At login the client proves knowledge
    of the password through a two-message challenge/response that the
    server drives with `axl_scram_server_first` then
    `axl_scram_server_final`.

    This is plain **SCRAM-SHA-256** only — gs2 header `n,,`, no channel
    binding (SCRAM-PLUS). A browser client driving this from JavaScript
    cannot read the TLS channel-binding hash, so channel binding is out of
    scope; run it inside TLS for transport security.

    @code
    // Enrollment (once, when a password is set):
    uint8_t salt[16];
    axl_rng_bytes(salt, sizeof salt);
    uint8_t stored[32], server[32];
    axl_scram_sha256_derive("pencil", salt, sizeof salt, 4096, stored, server);
    // ... persist {salt, 4096, stored, server} as the user's credential ...

    // Login, request 1 (client-first arrives):
    AxlScramCredential cred = { salt, sizeof salt, 4096, ..., ... };
    AxlScramState st;
    char server_first[512];
    axl_scram_server_first(&cred, client_first, client_first_len,
                           server_first, sizeof server_first, &st);
    // ... send server_first to the client; park `st` keyed by a login id ...

    // Login, request 2 (client-final arrives; reload `st`):
    char server_final[256];
    if (axl_scram_server_final(&st, client_final, client_final_len,
                               server_final, sizeof server_final) == AXL_OK) {
        // authenticated — send server_final (proves the server too)
    }
    @endcode

    The two login messages arrive on two separate connections/requests, so
    @ref AxlScramState is a plain serializable value (no pointers, no
    heap): copy it into a small table between the two steps and back.

    Verification uses `axl_consttime_equal` (@ref axl-crypto.h) for the
    proof compare. Dependency-free except for the digest/HMAC/base64/RNG
    primitives — works in every build (no AXL_TLS required).
**/

#ifndef AXL_SCRAM_H
#define AXL_SCRAM_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max bytes of a SCRAM message this engine parses/emits (client-first-bare
    and server-first). Longer inputs are rejected with AXL_INVALID; size an
    `out_server_first` buffer at this. */
#define AXL_SCRAM_MAX_MESSAGE  512
/** Max length (bytes of printable, comma-free ASCII) of the combined
    client+server nonce. The client nonce must therefore be at most
    AXL_SCRAM_MAX_NONCE - AXL_SCRAM_SERVER_NONCE_LEN bytes, else AXL_INVALID. */
#define AXL_SCRAM_MAX_NONCE    160
/** Length of the server nonce the engine appends (base64 of 18 random bytes;
    base64's alphabet excludes ','). */
#define AXL_SCRAM_SERVER_NONCE_LEN  24
/** Minimum size for an `out_server_final` buffer: "v=" + base64(32-byte
    ServerSignature) + NUL = 2 + 44 + 1. */
#define AXL_SCRAM_SERVER_FINAL_MAX  64

/**
 * @brief A stored SCRAM credential (no password-equivalent).
 *
 * What the server persists per user. `salt` points to the per-user random
 * salt the caller stores (not copied — must outlive any
 * `axl_scram_server_first` call that reads it). `stored_key` and
 * `server_key` come from `axl_scram_sha256_derive`. None of these reveal
 * the password, and `stored_key` alone cannot forge a client proof
 * (that needs ClientKey, which the server never holds).
 */
typedef struct {
    const uint8_t *salt;          ///< per-user salt bytes
    size_t         salt_len;      ///< salt length in bytes
    uint32_t       iterations;    ///< PBKDF2 iteration count used at enrollment
    uint8_t        stored_key[32];///< SHA256(HMAC(SaltedPassword,"Client Key"))
    uint8_t        server_key[32];///< HMAC(SaltedPassword,"Server Key")
} AxlScramCredential;

/**
 * @brief Engine state carried between the two server steps.
 *
 * A plain serializable value (no pointers): `axl_scram_server_first`
 * fills it, the caller parks it between the two HTTP requests, and
 * `axl_scram_server_final` consumes it. Holds the keys, the combined
 * nonce, and the two messages that form the AuthMessage. The salt is NOT
 * retained here — it is already embedded in `server_first` (`s=...`), so
 * the final step needs no salt and the no-pointers POD guarantee holds.
 *
 * It is sensitive (contains the credential keys) but not a
 * password-equivalent. Zero it with `axl_memset(&st, 0, sizeof st)` after
 * `server_final` (and likewise any `AxlScramCredential` copy): a leaked
 * `stored_key` enables an offline attack against captured login traffic.
 */
typedef struct {
    uint8_t  stored_key[32];                    ///< copied from the credential
    uint8_t  server_key[32];                    ///< copied from the credential
    uint16_t client_first_bare_len;             ///< length of client_first_bare
    uint16_t server_first_len;                  ///< length of server_first
    uint16_t combined_nonce_len;                ///< length of combined_nonce
    char     client_first_bare[AXL_SCRAM_MAX_MESSAGE]; ///< "n=<user>,r=<cnonce>"
    char     server_first[AXL_SCRAM_MAX_MESSAGE];      ///< "r=...,s=...,i=..."
    char     combined_nonce[AXL_SCRAM_MAX_NONCE];      ///< client nonce ‖ server nonce
} AxlScramState;

/**
 * @brief Derive a SCRAM-SHA-256 credential from a password (enrollment).
 *
 * Computes, per RFC 5802:
 * - SaltedPassword = PBKDF2-HMAC-SHA256(password, salt, iterations, 32)
 * - ClientKey = HMAC-SHA256(SaltedPassword, "Client Key")
 * - StoredKey = SHA256(ClientKey)        (written to @p stored_key)
 * - ServerKey = HMAC-SHA256(SaltedPassword, "Server Key")  (to @p server_key)
 *
 * The caller persists `{salt, iterations, stored_key, server_key}` and
 * discards the password. Use a random per-user @p salt (>= 16 bytes) and a
 * high @p iterations (>= 4096).
 *
 * @return AXL_OK on success; AXL_INVALID if @p password is NULL,
 *     @p iterations is 0, or an output pointer is NULL; AXL_ERR on an
 *     internal failure.
 */
AXL_WARN_UNUSED int
axl_scram_sha256_derive(
    const char    *password,      ///< UTF-8 password (NUL-terminated)
    const uint8_t *salt,          ///< per-user salt (may be NULL iff salt_len==0)
    size_t         salt_len,      ///< salt length in bytes
    uint32_t       iterations,    ///< PBKDF2 iteration count (>= 1)
    uint8_t        stored_key[32],///< [out] StoredKey
    uint8_t        server_key[32] ///< [out] ServerKey
);

/**
 * @brief SCRAM step 1: consume client-first, emit server-first.
 *
 * Parses the client-first message `"n,,n=<user>,r=<client-nonce>"`,
 * appends a fresh AXL_SCRAM_SERVER_NONCE_LEN-byte random server nonce to
 * the client nonce, and writes the server-first message
 * `"r=<client-nonce‖server-nonce>,s=<base64(salt)>,i=<iterations>"` to
 * @p out_server_first (NUL-terminated). The client-first-message-bare
 * stored for the AuthMessage is `"n=<user>,r=<client-nonce>"` (the gs2
 * header is stripped). All state the final step needs is in @p out_state.
 *
 * Only the plain gs2 header `n,,` is accepted: a header requesting or
 * asserting channel binding (`y,,`, `p=...`) or carrying an authzid
 * (`n,a=...`), or a client-first mandatory-extension field (`m=...`), is
 * rejected with AXL_INVALID — no silent downgrade.
 *
 * @return AXL_OK on success; AXL_INVALID if the client-first message is
 *     malformed, the gs2 header is not exactly `n,,`, the client nonce
 *     exceeds AXL_SCRAM_MAX_NONCE - AXL_SCRAM_SERVER_NONCE_LEN, the
 *     assembled server-first would exceed AXL_SCRAM_MAX_MESSAGE, or
 *     @p out_server_first is too small; AXL_ERR on an RNG/internal failure.
 */
AXL_WARN_UNUSED int
axl_scram_server_first(
    const AxlScramCredential *cred,                ///< the user's stored credential
    const char              *client_first,         ///< client-first message
    size_t                   client_first_len,     ///< its length in bytes
    char                    *out_server_first,     ///< [out] server-first message
    size_t                   out_server_first_size,///< capacity of @p out_server_first
    AxlScramState           *out_state             ///< [out] state for step 2
);

/**
 * @brief SCRAM step 2: verify client-final, emit server-final.
 *
 * Parses the client-final message
 * `"c=biws,r=<combined-nonce>,p=<base64(ClientProof)>"` (`biws` is
 * base64 of the gs2 header `n,,`), requires its nonce to equal the
 * combined nonce from step 1, and verifies the proof:
 * - client-final-without-proof = the client-final message truncated at
 *   ",p=" — i.e. the exact bytes `"c=biws,r=<combined-nonce>"`, INCLUDING
 *   the leading `c=biws` channel-binding token.
 * - AuthMessage = client-first-bare ‖ "," ‖ server-first ‖ ","
 *   ‖ client-final-without-proof, where client-first-bare is
 *   `"n=<user>,r=<cnonce>"` (gs2 header excluded) as captured in step 1.
 * - ClientKey' = ClientProof XOR HMAC-SHA256(StoredKey, AuthMessage)
 * - accept iff SHA256(ClientKey') == StoredKey  (constant-time compare)
 *
 * On success writes the server-final message
 * `"v=<base64(ServerSignature)>"` (ServerSignature =
 * HMAC-SHA256(ServerKey, AuthMessage)) to @p out_server_final, which the
 * client checks to authenticate the server in turn.
 *
 * A wrong nonce and a wrong proof BOTH return AXL_DENIED — the engine
 * never reveals which failed (distinguishing them would leak whether the
 * login state was valid). @p out_server_final must be at least
 * AXL_SCRAM_SERVER_FINAL_MAX bytes.
 *
 * @return AXL_OK on authentication success; AXL_DENIED if the proof is
 *     wrong or the nonce does not match step 1; AXL_INVALID if the
 *     client-final message is malformed or @p out_server_final is too
 *     small; AXL_ERR on an internal failure.
 */
AXL_WARN_UNUSED int
axl_scram_server_final(
    const AxlScramState *state,                  ///< state from axl_scram_server_first
    const char         *client_final,            ///< client-final message
    size_t              client_final_len,        ///< its length in bytes
    char               *out_server_final,        ///< [out] server-final message (>= AXL_SCRAM_SERVER_FINAL_MAX)
    size_t              out_server_final_size    ///< capacity of @p out_server_final
);

// ===================================================================
// Client engine (the peer of the server engine above)
// ===================================================================
//
// The client side, for an axl consumer that authenticates TO a SCRAM
// server (a client tool, agt, or a test driver) rather than serving the
// browser. It holds the password only for the duration of the exchange
// and never sends it. Three steps mirror the server: emit client-first,
// consume server-first + emit client-final, then verify the server's
// signature (mutual authentication).

/**
 * @brief Client state carried across the SCRAM exchange.
 *
 * A plain serializable value (no pointers): `axl_scram_client_first`
 * fills the nonce/message fields, `axl_scram_client_final` adds the
 * expected server signature, and `axl_scram_client_verify` checks it. It
 * holds no password-equivalent. Zero it with
 * `axl_memset(&st, 0, sizeof st)` after use.
 */
typedef struct {
    uint16_t client_first_bare_len;                     ///< length of client_first_bare
    uint16_t client_nonce_len;                          ///< length of client_nonce
    char     client_first_bare[AXL_SCRAM_MAX_MESSAGE];  ///< "n=<user>,r=<cnonce>"
    char     client_nonce[AXL_SCRAM_MAX_NONCE];         ///< the nonce we generated
    uint8_t  server_signature[32];                      ///< expected ServerSignature (set by _final)
} AxlScramClientState;

/**
 * @brief Client step 1: emit the client-first message.
 *
 * Generates a fresh random client nonce and writes
 * `"n,,n=<username>,r=<client-nonce>"` to @p out_client_first
 * (NUL-terminated). The username must contain no `,` or `=` (SCRAM
 * reserves them) and is rejected with AXL_INVALID otherwise.
 *
 * @return AXL_OK on success; AXL_INVALID if @p username is NULL, empty,
 *     contains a reserved character, or @p out_client_first is too small;
 *     AXL_ERR on an RNG failure.
 */
AXL_WARN_UNUSED int
axl_scram_client_first(
    const char          *username,         ///< login name
    char                *out_client_first, ///< [out] client-first message
    size_t               out_size,         ///< capacity of @p out_client_first
    AxlScramClientState *out_state         ///< [out] state for the next steps
);

/**
 * @brief Client step 2: consume server-first, emit client-final.
 *
 * Parses the server-first message `"r=<combined>,s=<base64(salt)>,i=<i>"`,
 * checks that @p combined begins with the nonce from step 1 (else the
 * server is not echoing our nonce), derives the proof from @p password,
 * salt and iteration count, and writes the client-final message
 * `"c=biws,r=<combined>,p=<base64(ClientProof)>"` to @p out_client_final.
 * It also stores the expected ServerSignature in @p state for
 * `axl_scram_client_verify`.
 *
 * @return AXL_OK on success; AXL_INVALID if @p password is NULL, the
 *     server-first message is malformed, its nonce does not extend ours,
 *     or @p out_client_final is too small; AXL_ERR on an internal failure.
 */
AXL_WARN_UNUSED int
axl_scram_client_final(
    AxlScramClientState *state,            ///< state from axl_scram_client_first
    const char          *password,         ///< UTF-8 password (NUL-terminated)
    const char          *server_first,     ///< server-first message
    size_t               server_first_len, ///< its length in bytes
    char                *out_client_final, ///< [out] client-final message
    size_t               out_size          ///< capacity of @p out_client_final
);

/**
 * @brief Client step 3: verify the server-final message (mutual auth).
 *
 * Parses `"v=<base64(ServerSignature)>"` and compares it, in constant
 * time, against the signature computed in step 2 — confirming the server
 * also knows the credential.
 *
 * @return AXL_OK if the server signature is correct; AXL_DENIED if it is
 *     wrong; AXL_INVALID if the server-final message is malformed.
 */
AXL_WARN_UNUSED int
axl_scram_client_verify(
    const AxlScramClientState *state,           ///< state from axl_scram_client_final
    const char               *server_final,     ///< server-final message
    size_t                    server_final_len  ///< its length in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SCRAM_H */
