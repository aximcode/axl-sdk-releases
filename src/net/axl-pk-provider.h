/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pk-provider.h
    The per-algorithm vtable behind AxlPkKey.

    AxlPkKey used to *be* an mbedtls_pk_context. Ed25519 cannot be one
    — mbedTLS 3.6.3 has no twisted-Edwards curve at all — so the key
    became a tagged union and every entry point routes through the
    vtable below, selected by the key's algorithm.

    A provider is reached through a WEAK symbol so that an algorithm
    nothing calls costs an image nothing. That matters for exactly one
    algorithm today: Ed25519's precomputed base-point table is 30,720
    bytes of resident .rodata, and axl_pk_key_new() is a dispatcher
    every AxlPk consumer calls, so a strong reference would pull the
    table into every image. See the design spec §4.

    Not a public header — do not include from outside src/net/.
**/

#ifndef AXL_PK_PROVIDER_H
#define AXL_PK_PROVIDER_H

#include <axl/axl-crypto.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The weak attribute must be applied ONLY where the reference is made.
   Putting it on a declaration in the translation unit that also
   DEFINES the symbol makes the definition weak, which leaves it weak
   inside libaxl.a and makes `-u` unreliable. axl-pk-verify.c defines
   this before including the header; a provider implementation does
   not. One prototype either way, so the two cannot drift. */
#ifdef AXL_PK_PROVIDER_WEAK
#  define AXL_PK_PROVIDER_DECL __attribute__((weak))
#else
#  define AXL_PK_PROVIDER_DECL
#endif

typedef struct AxlPkKey AxlPkKey;

/**
 * Signature mode. Design spec §5 requires this on `sign`/`verify`
 * from the start: Ed25519ph and Ed25519ctx are the SAME curve and the
 * SAME key as pure Ed25519, differing only by a prehash and a
 * domain-separation string. With a mode and a context in the shape,
 * they become later enum values on an existing provider; without
 * them they are a redesign.
 *
 * Only #AXL_PK_SIG_MODE_PURE exists today. See axl_pk_sig_params_ok(),
 * the one place that enforces which (mode, ctx, ctx_len) combinations
 * are legal — a provider never has to re-check this itself.
 *
 * Internal for now. E2b decides whether this becomes public, once
 * Ed25519 gives the mode something to select between.
 */
typedef enum {
    AXL_PK_SIG_MODE_PURE = 0  /**< sign/verify the message itself
                                   (RFC 8032 §5.1). */
} AxlPkSigMode;

/**
 * The mode/context rule spec §5 requires, in one place so a provider
 * cannot forget it and so it is reachable from a unit test. The
 * dispatcher calls this before invoking a provider's `sign` or
 * `verify`; a provider may assume it has already been checked and
 * that a pure mode with an empty context is all it will ever see.
 *
 * @return true iff @a mode == #AXL_PK_SIG_MODE_PURE and @a ctx ==
 *     NULL and @a ctx_len == 0. Any other combination — including a
 *     non-NULL @a ctx with @a ctx_len == 0 — is rejected: an ignored
 *     context would silently produce a signature that verifies under
 *     the wrong domain, which is the failure this function exists to
 *     make impossible.
 */
static inline bool
axl_pk_sig_params_ok(
    AxlPkSigMode  mode,     ///< signature mode to check
    const void   *ctx,      ///< domain-separation context, or NULL
    size_t        ctx_len   ///< length of @a ctx in bytes
)
{
    return mode == AXL_PK_SIG_MODE_PURE && ctx == NULL && ctx_len == 0;
}

/**
 * Per-algorithm operations.
 *
 * A member may be NULL. Every call site treats NULL as that
 * operation's documented failure — AXL_ERR, false, or a NULL handle.
 * A provider that supports an operation supplies it; one that does
 * not simply omits it. (Do NOT rely on "every member is set": a
 * designated initializer silently zero-fills an omitted member and
 * gcc does not warn, so the missing-member bug would surface as a
 * call through address 0.)
 *
 * Const-ness follows whether the operation mutates the KEY, not what
 * a particular provider's backing library wants. mbedTLS needs a
 * mutable context for pk_sign/pk_verify (but not for the DER
 * writers), so the mbedTLS provider casts internally — one provider's
 * C API must not make every other provider declare a mutation it does
 * not perform.
 *
 * Lifecycle: the dispatcher allocates the handle, zeroes the union,
 * sets the key's `alg`, then calls `key_init` -- for every
 * construction path EXCEPT `der_private_import` / `der_public_import`.
 * DER import has no algorithm to set going in: only the provider can
 * tell, by parsing the bytes, which algorithm they encode. For those
 * two members the dispatcher leaves `alg` at an out-of-range sentinel
 * -- not a member of AxlPkAlg at all, so a key that never gets
 * classified fails closed rather than reading as some particular
 * valid algorithm, permanently and regardless of which providers a
 * given image links (see axl-crypto-internal.h's
 * AXL_PK_ALG_UNCLASSIFIED) -- and the provider sets `alg` itself, on
 * success, once it knows. `key_free` must tolerate a key whose
 * `key_init` returned false, and one it never ran on at all — the
 * zeroed arm is a legal input, and it is what every construction
 * error path passes. `key_free` is called at most once.
 *
 * The tag must identify the provider that ran `key_init`. `key_alloc`
 * sets the fail-closed out-of-range sentinel, so between allocation
 * and a successful classification the tag and the arm disagree --
 * which means a construction path that frees BEFORE classifying must
 * free through the provider it actually used, never through
 * `axl_pk_key_free()`. That function resolves the provider from the
 * tag, so on a half-built handle it would find no provider at all --
 * the sentinel cannot resolve to one, ever -- and skip freeing the
 * arm entirely: a leak, not a misdirected free, but still wrong.
 *
 * The dispatcher owns `has_private` entirely; no provider ever
 * writes it. It sets `has_private` true after a successful `keygen`
 * or `der_private_import`, and false after a successful
 * `der_public_import` or `raw_public_import`. `keygen` itself runs on
 * an initialised, empty key. A mismatched OID in a DER import is an
 * ordinary `false`, not an error, so the dispatcher may offer the
 * same bytes to each provider in turn until one accepts them. Helpers
 * that only ever apply to one provider (the JWK component pair, the
 * RSA-PSS pair) live in that provider's translation unit and touch
 * its arm directly rather than becoming vtable members.
 */
typedef struct {
    /** Initialise @a key's union arm. Returns false on failure. */
    bool (*key_init)(AxlPkKey *key);

    /** Release everything key_init and the loaders allocated, and
        ZEROIZE any private key material first — axl_pk_key_free()
        promises that publicly and this is the only place that can
        keep it. */
    void (*key_free)(AxlPkKey *key);

    /** Generate a fresh keypair into an initialised @a key. */
    bool (*keygen)(AxlPkKey *key);

    /** Sign @a msg. The provider owns the size query: @a sig == NULL
        writes the required size to @a *sig_len and returns AXL_OK.
        This differs per algorithm — mbedTLS reports an upper bound
        for DER/RSA and an exact value for raw; Ed25519's is exactly
        64.

        @a mode / @a ctx / @a ctx_len are already validated by the
        dispatcher via axl_pk_sig_params_ok() before this is called.

        A provider whose algorithm has a single signature encoding
        ignores @a fmt, as RSA does. */
    int (*sign)(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
                AxlPkSigFormat fmt, AxlPkSigMode mode,
                const void *ctx, size_t ctx_len,
                uint8_t *sig, size_t *sig_len);

    /** Verify @a sig over @a msg. Returns AXL_OK only on a good
        signature. Same @a fmt handling as sign, and @a mode / @a ctx
        / @a ctx_len are the same dispatcher-validated values. */
    int (*verify)(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
                  AxlPkSigFormat fmt, AxlPkSigMode mode,
                  const void *ctx, size_t ctx_len,
                  const uint8_t *sig, size_t sig_len);

    /** Import a raw public key. Optional — a provider whose algorithm
        has no raw encoding (RSA) omits this. Returns false on any
        malformed input. */
    bool (*raw_public_import)(AxlPkKey *key, const uint8_t *raw, size_t len);

    /** Export the raw public key. Optional, same condition as
        raw_public_import. Same size-query protocol as sign. */
    int (*raw_public_export)(const AxlPkKey *key, uint8_t *out, size_t *len);

    /** Import a PKCS#8 PrivateKeyInfo. false if @a der is not this
        provider's key type or is malformed. The dispatcher may offer
        the same bytes to more than one provider, so a mismatched OID
        is an ordinary false, not an error. */
    bool (*der_private_import)(AxlPkKey *key, const uint8_t *der, size_t len);

    /** Import a SubjectPublicKeyInfo. Same mismatch rule as
        der_private_import. */
    bool (*der_public_import)(AxlPkKey *key, const uint8_t *der, size_t len);

    /** Export PKCS#8. Same size-query protocol as raw_public_export. */
    int  (*der_private_export)(const AxlPkKey *key, uint8_t *out, size_t *len);

    /** Export SubjectPublicKeyInfo. Same protocol. */
    int  (*der_public_export)(const AxlPkKey *key, uint8_t *out, size_t *len);
} AxlPkProvider;

/* One extern per provider. The mbedTLS provider serves ECDSA and RSA
   and is referenced strongly — every build has mbedTLS. Ed25519's is
   weak and referenced only if the image opted in with
   `-u _axl_pk_provider_ed25519`.

   When the image did not opt in, the symbol is unresolved and
   `&_axl_pk_provider_ed25519` evaluates to NULL. Test the address
   before reading any member — there is no object to read, and under
   UEFI a load from address 0 succeeds and returns zero rather than
   faulting. This is also why AxlPkProvider carries no `alg` field: a
   dispatcher that read `provider->alg` to select a provider would
   read address 0 for an unlinked weak provider and get back 0, which
   is #AXL_PK_ED25519 — selecting the very provider that is absent. */
extern const AxlPkProvider _axl_pk_provider_mbedtls;
AXL_PK_PROVIDER_DECL extern const AxlPkProvider _axl_pk_provider_ed25519;

/**
 * Resolve the provider for @a alg, or NULL if this image has none.
 * NULL is the fail-closed answer every entry point turns into a NULL
 * handle or AXL_ERR.
 *
 * Implemented as an explicit switch naming each provider symbol
 * individually and testing its address — never by walking a table
 * and reading a member off of it, which is unsafe for exactly the
 * reason documented at the externs above.
 */
const AxlPkProvider *
_axl_pk_provider_for(
    AxlPkAlg alg  ///< algorithm to resolve
);

#endif /* AXL_PK_PROVIDER_H */
