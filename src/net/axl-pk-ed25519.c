/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pk-ed25519.c
    The Ed25519 AxlPkProvider, over the vendored ref10.

    Reached only through the WEAK symbol declared in axl-pk-provider.h,
    so an image links this translation unit -- and the ~120 KB of
    vendored curve code behind it -- only if it calls
    axl_pk_enable_ed25519(), defined below (AxlSsh's server does). This
    file therefore does NOT define AXL_PK_PROVIDER_WEAK: that macro
    belongs where the REFERENCE is made (axl-pk-verify.c), and applying
    it in the defining translation unit would make the definition itself
    weak inside libaxl.a, so linking this object would no longer resolve
    the dispatcher's reference. The header says so at
    length; this comment exists because getting it wrong here fails
    silently rather than loudly.

    The vendored code owns the cryptography and MOST of the negative
    checks -- canonical S, non-canonical public keys, the high bits of
    S. It does not own all of them: its small-order public-key check
    covers only the identity, so this file adds the rest of the
    8-torsion (see ED25519_SMALL_ORDER). Beyond that it owns adapting
    NaCl's combined signature convention to AXL's detached one, refusing
    a wrong-length signature, RFC 8410 DER, and zeroizing the secret.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>   /* SIZE_MAX, for the sm/m sizing guard */
#include <mbedtls/ctr_drbg.h>
#include <axl/axl-crypto.h>
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "axl-crypto-internal.h"
#include "axl-ed25519-glue.h"
#include "axl-pk-provider.h"
#include "crypto_api.h"

#define ED25519_SEED_LEN      32u
#define ED25519_PUB_LEN       32u
#define ED25519_SK_LEN        64u
#define ED25519_SIG_LEN       64u

/* RFC 8410 §7 / §4. Both encodings are FIXED length, which is why this
   file template-matches rather than walking ASN.1: over untrusted bytes
   a total-length check plus a fixed-prefix compare is far easier to get
   right than a parser, and it is what the design spec's §8 review reads.

   PKCS#8 PrivateKeyInfo, 48 bytes:
     30 2e                     SEQUENCE, 46 bytes
        02 01 00               INTEGER version 0
        30 05 06 03 2b 65 70   AlgorithmIdentifier: OID 1.3.101.112
        04 22                  OCTET STRING, 34 bytes (privateKey)
           04 20 <32 seed>     CurvePrivateKey: OCTET STRING, 32 bytes

   SubjectPublicKeyInfo, 44 bytes:
     30 2a                     SEQUENCE, 42 bytes
        30 05 06 03 2b 65 70   AlgorithmIdentifier: OID 1.3.101.112
        03 21 00 <32 pubkey>   BIT STRING, 33 bytes, 0 unused bits

   Note the prefixes below carry the OID *and* every length byte, so a
   wrong OID, a wrong length and a wrong tag are all one comparison --
   and the BIT STRING's unused-bits byte is INSIDE the prefix, which is
   the field a hand-written matcher most often skips because nothing
   downstream reads it. A 33rd byte of key material would live there. */
static const uint8_t ED25519_PKCS8_PREFIX[] = {
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06,
    0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20
};
static const uint8_t ED25519_SPKI_PREFIX[] = {
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65,
    0x70, 0x03, 0x21, 0x00
};

#define ED25519_PKCS8_LEN  (sizeof(ED25519_PKCS8_PREFIX) + ED25519_SEED_LEN)
#define ED25519_SPKI_LEN   (sizeof(ED25519_SPKI_PREFIX) + ED25519_PUB_LEN)

/* THE SMALL-ORDER PUBLIC KEYS -- and why this check is ours rather than
   the vendored code's.

   crypto_sign_ed25519_open rejects a public key that unpacks to the
   IDENTITY (ge25519_isneutral_vartime) but not the other seven points
   of the 8-torsion. That gap is forgeable, and not theoretically:
   under a public key A of order 2, verification reduces to
   [S]B == R + [h]A, and [h]A is the identity whenever h is even -- so
   S = 1 with R = encode(B) verifies, with no private key, for roughly
   half of all messages. Measured against this very provider before this
   table existed: the identity forgery was refused and the order-2 one
   was ACCEPTED.

   PRIOR ART, and why we do not follow it. libsodium solves this
   ALGEBRAICALLY rather than by enumeration: ge25519_has_small_order()
   takes a DECODED ge25519_p3 and tests X == 0, Y == 0, Z == 0 and
   Y*sqrt(-1) -+ X == 0. That is the better shape -- nothing to keep in
   sync -- and it is not available here. It needs the decoded point, and
   ref10 keeps `ge25519` and its field operations private to
   deps/ed25519-ref10/ed25519.c; exposing them means editing the
   vendored file, which is the one thing this vendoring exists to avoid.
   Re-deriving field arithmetic in this file to avoid a 14-entry table
   would be a far larger surface than the table is.

   A BLACKLIST, DELIBERATELY -- and src/data/axl-json-parse.c warns
   against exactly that ("a blacklist silently reclassifies anything
   added to the literal table later"). The warning does not reach this
   case, and the difference is the reason: a literal table is a design
   choice that grows, while the 8-torsion of Curve25519 is closed by
   mathematics at exactly eight points and cannot acquire a ninth.
   Enumeration is therefore exhaustive by construction, not by
   vigilance.

   All fourteen encodings were DERIVED, not copied: the 8-torsion was
   enumerated as [k]([L]Q) for a Q outside the prime-order subgroup, and
   every 32-byte encoding that decodes to one of those points -- both
   sign bits, and the non-canonical y >= p forms -- is listed. Checked
   back against this table as written: each entry decodes to a point P
   with [8]P == identity, and the fourteen together cover all eight
   points.

   THE NON-CANONICAL ROWS ARE NOT REDUNDANT, and an earlier version of
   this comment said they were. ref10 does NOT range-check y: its
   fe25519_unpack copies 32 bytes and masks bit 255, and the vendored
   source says so itself -- "Assumes input x being reduced below
   2^255". So of the four y >= p rows, the ee.. pair is caught by
   ge25519_isneutral_vartime only because fe25519_iszero freezes first,
   and THE ed.. PAIR (y = p, order 4) IS CAUGHT BY NOTHING IN REF10.
   These two rows are the only thing refusing it.

   Only A is checked, not R. libsodium also refuses a small-order R,
   which makes verification agree byte-for-byte across implementations
   -- but an honest signer produces such an R with probability about
   2^-252, and refusing it closes no forgery, so it would be pure
   divergence from OpenSSH, which is this code's interop oracle. */
static const uint8_t ED25519_SMALL_ORDER[][ED25519_PUB_LEN] = {
    /* order 4 -- y = 0, x = +-sqrt(-1) */
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80 },
    /* order 1 -- the identity, both sign bits */
    { 0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80 },
    /* order 8 */
    { 0x26,0xe8,0x95,0x8f,0xc2,0xb2,0x27,0xb0,0x45,0xc3,0xf4,0x89,0xf2,0xef,0x98,0xf0,
      0xd5,0xdf,0xac,0x05,0xd3,0xc6,0x33,0x39,0xb1,0x38,0x02,0x88,0x6d,0x53,0xfc,0x05 },
    { 0x26,0xe8,0x95,0x8f,0xc2,0xb2,0x27,0xb0,0x45,0xc3,0xf4,0x89,0xf2,0xef,0x98,0xf0,
      0xd5,0xdf,0xac,0x05,0xd3,0xc6,0x33,0x39,0xb1,0x38,0x02,0x88,0x6d,0x53,0xfc,0x85 },
    { 0xc7,0x17,0x6a,0x70,0x3d,0x4d,0xd8,0x4f,0xba,0x3c,0x0b,0x76,0x0d,0x10,0x67,0x0f,
      0x2a,0x20,0x53,0xfa,0x2c,0x39,0xcc,0xc6,0x4e,0xc7,0xfd,0x77,0x92,0xac,0x03,0x7a },
    { 0xc7,0x17,0x6a,0x70,0x3d,0x4d,0xd8,0x4f,0xba,0x3c,0x0b,0x76,0x0d,0x10,0x67,0x0f,
      0x2a,0x20,0x53,0xfa,0x2c,0x39,0xcc,0xc6,0x4e,0xc7,0xfd,0x77,0x92,0xac,0x03,0xfa },
    /* order 2 -- y = p - 1, both sign bits */
    { 0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    { 0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff },
    /* order 4, NON-canonical: y = p */
    { 0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    { 0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff },
    /* order 1, NON-canonical: y = p + 1 */
    { 0xee,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    { 0xee,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff },
};

/* True if @a pub is one of the 8-torsion encodings above. A public key
   is public, so this needs no timing defence -- it uses
   axl_consttime_equal anyway because that is the library's one buffer
   comparison for key material and reaching past it for a memcmp would
   be a reader's puzzle, not a saving. No early exit for the same
   reason. */
static bool
ed25519_is_small_order(
    const uint8_t *pub  ///< 32-byte compressed point
)
{
    bool hit = false;
    for (size_t i = 0;
         i < sizeof(ED25519_SMALL_ORDER) / sizeof(ED25519_SMALL_ORDER[0]);
         i++) {
        hit |= axl_consttime_equal(pub, ED25519_SMALL_ORDER[i],
                                   ED25519_PUB_LEN);
    }
    return hit;
}

/* A scrub the optimiser cannot elide -- see axl-ed25519-glue.c's copy
   for why this is not axl_memset. Duplicated rather than shared because
   a security primitive behind a cross-module link-time indirection is
   worse than eight lines twice. */
static void
secure_zero(
    void   *p,  ///< buffer to scrub
    size_t  n   ///< bytes to scrub
)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0) {
        *v++ = 0;
    }
}

/* The output-buffer protocol every AxlPk export shares: a NULL @a out
   is a size query; a too-small buffer reports the size it needs and
   writes nothing. Both Ed25519 encodings are fixed-length, so unlike
   ECDSA DER the "required size" here is exact rather than a bound. */
static int
export_fixed(
    const uint8_t *src,   ///< bytes to export
    size_t         need,  ///< exact size of @a src
    uint8_t       *out,   ///< destination, or NULL to size-query
    size_t        *len    ///< [in/out] capacity / bytes written
)
{
    if (len == NULL) {
        return AXL_ERR;
    }
    if (out == NULL) {
        *len = need;
        return AXL_OK;
    }
    if (*len < need) {
        *len = need;
        return AXL_ERR;
    }
    axl_memcpy(out, src, need);
    *len = need;
    return AXL_OK;
}

/* Record @a pub as the key's public half. Kept in one place because the
   arm holds it twice -- in `pk`, and in the second half of ref10's
   `sk` layout -- and letting those two disagree would mean signing with
   one key and reporting another. */
static void
set_public(
    AxlPkKey      *key,  ///< key whose Ed25519 arm to fill
    const uint8_t *pub   ///< 32-byte compressed point
)
{
    axl_memcpy(key->u.ed25519.pk, pub, ED25519_PUB_LEN);
    axl_memcpy(key->u.ed25519.sk + ED25519_SEED_LEN, pub, ED25519_PUB_LEN);
}

// ---------------------------------------------------------------------------
// Vtable members
// ---------------------------------------------------------------------------

/* Nothing to initialise: the dispatcher zeroes the union before calling
   this, and the arm is plain bytes with no library context underneath.
   A NULL member would be legal per the contract, but every construction
   path tests `key_init != NULL` before proceeding, so supplying an
   explicit true is what makes this provider reachable at all. */
static bool
ed25519_key_init(AxlPkKey *key)
{
    (void)key;
    return true;
}

/* The arm IS the secret for this provider -- there is no mbedTLS
   context below it whose own free would scrub. axl_pk_key_free()
   promises publicly that it zeroizes private key material, and this is
   the only place that can keep that promise.

   Scrubs unconditionally: this runs on public-only handles and on
   handles whose construction failed too, where the arm is already zero,
   and branching on has_private to save 96 bytes of writes would put a
   condition between the secret and its erasure. */
static void
ed25519_key_free(AxlPkKey *key)
{
    if (key != NULL) {
        secure_zero(&key->u.ed25519, sizeof(key->u.ed25519));
    }
}

/* Deliberately NOT crypto_sign_ed25519_keypair, which would reach the
   random source through the vendored code's `randombytes` -- a void
   function whose failure ref10 cannot check. Drawing the seed here
   keeps the failure checkable, and the seed is the only entropy an
   Ed25519 key has. */
static bool
ed25519_keygen(AxlPkKey *key)
{
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (rng == NULL) {
        return false;
    }

    uint8_t seed[ED25519_SEED_LEN];
    if (mbedtls_ctr_drbg_random(rng, seed, sizeof(seed)) != 0) {
        secure_zero(seed, sizeof(seed));
        return false;
    }

    uint8_t  pub[ED25519_PUB_LEN];
    uint32_t epoch = _axl_ed25519_glue_failures();
    int      rc    = crypto_sign_ed25519_keypair_from_seed(pub,
                                                           key->u.ed25519.sk,
                                                           seed);
    secure_zero(seed, sizeof(seed));
    /* ref10 returns 0 whatever its hash did; the glue's count is the only
       witness (axl-ed25519-glue.h). A key derived from a failed hash is
       not the key this seed names. */
    if (rc != 0 || _axl_ed25519_glue_failures() != epoch) {
        secure_zero(&key->u.ed25519, sizeof(key->u.ed25519));
        return false;
    }
    /* keypair_from_seed already wrote seed || pub into sk; mirror the
       public half into `pk` so both spellings agree. */
    axl_memcpy(key->u.ed25519.pk, pub, ED25519_PUB_LEN);
    return true;
}

/* NaCl's combined convention: crypto_sign_ed25519 produces
   sm = signature || message and reports its length, so the signature is
   the first 64 bytes of a buffer that must be big enough for the whole
   message as well.

   Heap, not stack: msg_len is caller-controlled and UEFI stacks are not
   generous. */
static int
ed25519_sign(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
             AxlPkSigFormat fmt, AxlPkSigMode mode,
             const void *ctx, size_t ctx_len,
             uint8_t *sig, size_t *sig_len)
{
    /* Ed25519 has exactly one signature encoding, so `fmt` is ignored --
       the contract's rule, the same one the RSA provider follows. mode /
       ctx / ctx_len are already validated by the dispatcher via
       axl_pk_sig_params_ok(), which a provider may assume, so they are
       not re-checked here. */
    (void)fmt;
    (void)mode;
    (void)ctx;
    (void)ctx_len;

    if (sig_len == NULL) {
        return AXL_ERR;
    }
    if (sig == NULL) {
        *sig_len = ED25519_SIG_LEN;
        return AXL_OK;
    }
    if (*sig_len < ED25519_SIG_LEN) {
        *sig_len = ED25519_SIG_LEN;
        return AXL_ERR;
    }

    /* msg_len is caller-controlled and nothing between the public API
       and here bounds it, so 64 + msg_len can wrap -- and the top of
       size_t is exactly where a `end - start` pointer or length
       underflow lands, which matters because the first consumer parses
       length-prefixed network fields.

       The bound is SIZE_MAX/2 rather than SIZE_MAX - 64, and that is
       deliberate: avoiding the wrap in THIS expression is not enough,
       because axl_malloc has the same defect one layer down -- its
       align_up() rounds up to a size_t boundary and wraps to 0 for a
       size above SIZE_MAX - 7, so axl_malloc(SIZE_MAX) returns a
       HEADER-SIZED buffer and reports success. Measured: a guard at
       SIZE_MAX - 64 let n == SIZE_MAX through and corrupted the heap.
       Half of size_t is far above any real message and leaves room for
       any allocator overhead, so it is correct without this file having
       to know axl-mem's layout. */
    if (msg_len > SIZE_MAX / 2) {
        return AXL_ERR;
    }
    size_t   n  = ED25519_SIG_LEN + msg_len;
    uint8_t *sm = axl_malloc(n);
    if (sm == NULL) {
        return AXL_ERR;
    }

    /* ref10 does memmove(sm + 64, m, mlen) unconditionally; a NULL m
       with mlen 0 is undefined even though it moves nothing, and the
       empty message is a real case (RFC 8032 TEST 1). */
    static const uint8_t EMPTY[1] = { 0 };
    const uint8_t       *m        = (msg != NULL) ? msg : EMPTY;

    unsigned long long smlen = 0;
    uint32_t           epoch = _axl_ed25519_glue_failures();
    int rc = crypto_sign_ed25519(sm, &smlen, m, (unsigned long long)msg_len,
                                 key->u.ed25519.sk);
    /* ref10 returns 0 whatever its three hashes did. If any failed, what it
       built is not a signature to hand out: with the NONCE hash failed,
       r = 0 and the result VERIFIES while disclosing the private scalar
       (a = S * h^-1 mod L). The glue's count is the only witness. */
    if (_axl_ed25519_glue_failures() != epoch) {
        rc = -1;
    }
    if (rc == 0 && smlen == (unsigned long long)n) {
        axl_memcpy(sig, sm, ED25519_SIG_LEN);
        *sig_len = ED25519_SIG_LEN;
    } else {
        rc = -1;
    }
    /* sm's first 64 bytes are the signature, which is public -- but the
       buffer also passed through ref10's scratch use, so scrub it
       rather than reason about which bytes are safe to leave on the
       heap. */
    secure_zero(sm, n);
    axl_free(sm);
    return (rc == 0) ? AXL_OK : AXL_ERR;
}

/* The mirror image, and the part with the sharpest edges.
   crypto_sign_ed25519_open takes sm = signature || message and writes
   the RECOVERED message into an output buffer the caller must size at
   smlen -- it does memset(m, 0, smlen) on the failure path, so `m` is
   written unconditionally and must not alias `sm`.

   A wrong-length signature is refused HERE rather than passed through.
   Ed25519 signatures are exactly 64 bytes; handing ref10 a 63-byte one
   with a 1-byte message would build a 64-byte sm whose last byte is
   message data reinterpreted as the top of S -- a well-formed-looking
   input that its own `smlen < 64` guard cannot catch, because smlen is
   64. Ours is the outer check and ref10's is the inner one. */
static int
ed25519_verify(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
               AxlPkSigFormat fmt, AxlPkSigMode mode,
               const void *ctx, size_t ctx_len,
               const uint8_t *sig, size_t sig_len)
{
    (void)fmt;
    (void)mode;
    (void)ctx;
    (void)ctx_len;

    if (sig == NULL || sig_len != ED25519_SIG_LEN) {
        return AXL_ERR;
    }
    /* Refuse a small-order public key before any signature is examined.
       See ED25519_SMALL_ORDER above: the vendored verifier catches only
       the identity, and the rest of the 8-torsion is forgeable. This is
       at VERIFY rather than at import so it covers every construction
       path -- raw, SPKI, and anything added later -- in one place. */
    if (ed25519_is_small_order(key->u.ed25519.pk)) {
        return AXL_ERR;
    }

    /* Same sizing guard as ed25519_sign() -- see its comment for why the
       bound is SIZE_MAX/2 and not SIZE_MAX - 64. */
    if (msg_len > SIZE_MAX / 2) {
        return AXL_ERR;
    }
    size_t   n  = ED25519_SIG_LEN + msg_len;
    uint8_t *sm = axl_malloc(n);
    uint8_t *m  = axl_malloc(n);
    if (sm == NULL || m == NULL) {
        axl_free(sm);
        axl_free(m);
        /* NOT AXL_ERR: nothing has examined the signature yet, so this is
           a fact about this machine, not a verdict on the signer. A caller
           that treats a bad signature as an attack must be able to tell
           the two apart (axl_pk_key_verify's contract). */
        return AXL_NO_RESOURCES;
    }

    axl_memcpy(sm, sig, ED25519_SIG_LEN);
    if (msg_len != 0) {
        axl_memcpy(sm + ED25519_SIG_LEN, msg, msg_len);
    }

    unsigned long long mlen  = 0;
    uint32_t           epoch = _axl_ed25519_glue_failures();
    int rc = crypto_sign_ed25519_open(m, &mlen, sm, (unsigned long long)n,
                                      key->u.ed25519.pk);
    /* A failed challenge hash is h = 0, under which R = encode(B), S = 1
       verifies for EVERY key -- so ref10's "0" means nothing once the
       glue has counted a failure. AXL_ERR, not AXL_NO_RESOURCES: it is
       not an allocation, and the conservative answer to a check that went
       wrong is "not verified". */
    if (_axl_ed25519_glue_failures() != epoch) {
        rc = -1;
    }

    secure_zero(m, n);
    secure_zero(sm, n);
    axl_free(m);
    axl_free(sm);
    return (rc == 0) ? AXL_OK : AXL_ERR;
}

/* No point validation here, by design. Every bad-key refusal happens on
   the verify path instead -- ed25519_is_small_order() above, plus
   ref10's own ge25519_unpackneg_vartime for a non-canonical encoding --
   so a bad point imports and then never verifies. The contract permits
   either rejection point, and one place beats two: a check here would
   have to be repeated in der_public_import and in every construction
   path added later, whereas verify is the single door every signature
   goes through. */
static bool
ed25519_raw_public_import(AxlPkKey *key, const uint8_t *raw, size_t len)
{
    if (raw == NULL || len != ED25519_PUB_LEN) {
        return false;
    }
    set_public(key, raw);
    return true;
}

static int
ed25519_raw_public_export(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    return export_fixed(key->u.ed25519.pk, ED25519_PUB_LEN, out, len);
}

/* A mismatched template is an ordinary false, NOT an error: the
   dispatcher offers the same bytes to each provider in turn, so
   "these are not mine" and "these are corrupt" must look the same to
   it. Sets `alg` on success -- the contract's one exception to the
   dispatcher owning the tag, because only the provider that parsed the
   bytes can classify them. */
static bool
ed25519_der_private_import(AxlPkKey *key, const uint8_t *der, size_t len)
{
    if (der == NULL || len != ED25519_PKCS8_LEN
        || axl_memcmp(der, ED25519_PKCS8_PREFIX,
                      sizeof(ED25519_PKCS8_PREFIX)) != 0) {
        return false;
    }

    const uint8_t *seed  = der + sizeof(ED25519_PKCS8_PREFIX);
    uint8_t        pub[ED25519_PUB_LEN];
    uint32_t       epoch = _axl_ed25519_glue_failures();
    /* The same witness as keygen: ref10 returns 0 whatever its hash did. */
    if (crypto_sign_ed25519_keypair_from_seed(pub, key->u.ed25519.sk,
                                              seed) != 0
        || _axl_ed25519_glue_failures() != epoch) {
        secure_zero(&key->u.ed25519, sizeof(key->u.ed25519));
        return false;
    }
    axl_memcpy(key->u.ed25519.pk, pub, ED25519_PUB_LEN);
    key->alg = AXL_PK_ED25519;
    return true;
}

static bool
ed25519_der_public_import(AxlPkKey *key, const uint8_t *der, size_t len)
{
    if (der == NULL || len != ED25519_SPKI_LEN
        || axl_memcmp(der, ED25519_SPKI_PREFIX,
                      sizeof(ED25519_SPKI_PREFIX)) != 0) {
        return false;
    }
    set_public(key, der + sizeof(ED25519_SPKI_PREFIX));
    key->alg = AXL_PK_ED25519;
    return true;
}

/* Encode is the exact inverse of the template match: the fixed prefix
   followed by the 32 bytes. The seed is sk's FIRST half -- writing the
   second half here would export the public key as if it were private,
   and both halves are 32 bytes, so nothing but this comment and the
   pinned vector would notice. */
static int
ed25519_der_private_export(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    uint8_t der[ED25519_PKCS8_LEN];
    axl_memcpy(der, ED25519_PKCS8_PREFIX, sizeof(ED25519_PKCS8_PREFIX));
    axl_memcpy(der + sizeof(ED25519_PKCS8_PREFIX), key->u.ed25519.sk,
               ED25519_SEED_LEN);
    int rc = export_fixed(der, sizeof(der), out, len);
    secure_zero(der, sizeof(der));
    return rc;
}

static int
ed25519_der_public_export(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    uint8_t der[ED25519_SPKI_LEN];
    axl_memcpy(der, ED25519_SPKI_PREFIX, sizeof(ED25519_SPKI_PREFIX));
    axl_memcpy(der + sizeof(ED25519_SPKI_PREFIX), key->u.ed25519.pk,
               ED25519_PUB_LEN);
    return export_fixed(der, sizeof(der), out, len);
}

/* The opt-in -- see axl_pk_enable_mbedtls() in axl-pk-mbedtls.c for why
   an empty body is the whole mechanism. */
void
axl_pk_enable_ed25519(void)
{
}

const AxlPkProvider _axl_pk_provider_ed25519 = {
    .key_init            = ed25519_key_init,
    .key_free            = ed25519_key_free,
    .keygen              = ed25519_keygen,
    .sign                = ed25519_sign,
    .verify              = ed25519_verify,
    .raw_public_import   = ed25519_raw_public_import,
    .raw_public_export   = ed25519_raw_public_export,
    .der_private_import  = ed25519_der_private_import,
    .der_public_import   = ed25519_der_public_import,
    .der_private_export  = ed25519_der_private_export,
    .der_public_export   = ed25519_der_public_export,
};
