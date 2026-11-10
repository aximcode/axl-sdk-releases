/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-crypt.c
    chacha20-poly1305@openssh.com — the AEAD binary packet protocol.

    NOT the RFC 8439 AEAD, and deliberately not built on axl_aead_seal(). SSH
    needs the receiver to learn a packet's length before it can authenticate
    it, so this construction encrypts the 4-byte length field under a SECOND,
    separate key and authenticates the whole ciphertext afterwards. OpenSSH's
    PROTOCOL.chacha20poly1305 is the specification.

    The nonce is the packet sequence number and nothing else -- there is no
    explicit IV in the key material. That makes sequence handling
    security-critical rather than bookkeeping: two packets sealed under one
    sequence number reuse a keystream.

    ChaCha20 here is the original 64-bit-nonce / 64-bit-counter form, while
    mbedTLS implements RFC 8439's 96-bit nonce and 32-bit counter. The two are
    the same cipher with the state words split differently, so an 8-byte SSH
    nonce becomes a 12-byte mbedTLS nonce with four zero bytes in front. See
    ssh_nonce().
**/

#include "axl-ssh-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-rng.h>
#include <axl/axl-crypto.h>   /* axl_consttime_equal */
#include <mbedtls/chacha20.h>
#include <mbedtls/poly1305.h>

/** Poly1305's key is the first 32 bytes of the payload cipher's counter-0
    block, so that block is generated and then discarded. */
#define POLY_KEY_LEN  32u

/** The encrypted packet_length field. */
#define LEN_FIELD     4u

/* Map an SSH sequence number onto an mbedTLS (RFC 8439) nonce.

   The original ChaCha20 state is [const][key][counter64][nonce64]; RFC 8439's
   is [const][key][counter32][nonce96]. When the counter fits in 32 bits the
   two agree exactly if the RFC nonce is 4 zero bytes followed by the 8-byte
   original nonce -- the zero bytes land on the original counter's high half.
   OpenSSH writes the sequence number BIG-endian into that 8-byte nonce. */
static void
ssh_nonce(
    uint8_t  out[12],  ///< [out] mbedTLS nonce
    uint64_t seq       ///< SSH packet sequence number
)
{
    axl_memset(out, 0, 4);
    for (int i = 0; i < 8; i++) {
        out[4 + i] = (uint8_t)(seq >> (56 - 8 * i));
    }
}

/* One ChaCha20 pass. Separate from its callers because every use here is
   "key, nonce, counter, in, out" and mbedTLS needs four calls to say it. */
static int
chacha(const uint8_t key[32], uint64_t seq, uint32_t counter,
       const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t nonce[12];
    ssh_nonce(nonce, seq);

    mbedtls_chacha20_context ctx;
    mbedtls_chacha20_init(&ctx);
    int rc = mbedtls_chacha20_setkey(&ctx, key);
    if (rc == 0) {
        rc = mbedtls_chacha20_starts(&ctx, nonce, counter);
    }
    if (rc == 0) {
        rc = mbedtls_chacha20_update(&ctx, len, in, out);
    }
    mbedtls_chacha20_free(&ctx);
    return (rc == 0) ? AXL_OK : AXL_ERR;
}

/* Derive this packet's Poly1305 key: the payload cipher's counter-0 block,
   keystream only, which is why the input is zeros. */
static int
poly_key_for(const AxlSshAead *a, uint64_t seq, uint8_t out[POLY_KEY_LEN])
{
    uint8_t zeros[POLY_KEY_LEN];
    axl_memset(zeros, 0, sizeof zeros);
    return chacha(a->k_payload, seq, 0, zeros, out, sizeof zeros);
}

int
axl_ssh_aead_set_key(AxlSshAead *a, const uint8_t *key64)
{
    if (a == NULL || key64 == NULL) {
        return AXL_ERR;
    }
    /* K_2 first, K_1 second. OpenSSH's chachapoly_init does exactly this:
       main_ctx from key[0..32], header_ctx from key[32..64]. Swapping them
       yields a cipher that round-trips against itself and against nothing
       else, which no unit test of ours could tell apart -- only the
       ssh(1) conformance test can. */
    axl_memcpy(a->k_payload, key64, 32);
    axl_memcpy(a->k_length, key64 + 32, 32);
    a->ready = true;
    return AXL_OK;
}

void
axl_ssh_rng_pool_init(AxlSshRngPool *p)
{
    if (p == NULL) {
        return;
    }
    axl_memset(p->buf, 0, sizeof p->buf);
    p->pos     = AXL_SSH_RNG_POOL_LEN;
    p->refills = 0;
}

int
axl_ssh_rng_pool_take(AxlSshRngPool *p, uint8_t *out, size_t n)
{
    if (p == NULL || out == NULL || n > AXL_SSH_RNG_POOL_LEN) {
        return AXL_ERR;
    }
    if ((size_t)(AXL_SSH_RNG_POOL_LEN - p->pos) < n) {
        /* Fewer than n bytes remain: discard the unused tail rather than
           splice around it (simpler, and wastes at most n-1 bytes) and
           refill the WHOLE pool with one firmware call. */
        if (axl_rng_bytes(p->buf, AXL_SSH_RNG_POOL_LEN) != AXL_OK) {
            /* Left empty, never zero-filled: predictable padding is a
               chosen-plaintext gift, so a refill failure must fail the
               caller rather than hand back stale or zeroed bytes. */
            p->pos = AXL_SSH_RNG_POOL_LEN;
            return AXL_ERR;
        }
        p->pos = 0;
        p->refills++;
    }
    axl_memcpy(out, p->buf + p->pos, n);
    p->pos = (uint16_t)(p->pos + n);
    return AXL_OK;
}

void
axl_ssh_rng_pool_clear(AxlSshRngPool *p)
{
    if (p == NULL) {
        return;
    }
    axl_ssh_secure_zero(p->buf, sizeof p->buf);
    p->pos = AXL_SSH_RNG_POOL_LEN;
}

int
axl_ssh_aead_seal(const AxlSshAead *a, uint64_t seq,
                  const void *payload, size_t len,
                  AxlSshRngPool *pad, AxlString *out)
{
    if (a == NULL || !a->ready || pad == NULL || out == NULL
        || (payload == NULL && len != 0)) {
        return AXL_ERR;
    }

    /* Padding brings padding_length + payload + padding to a multiple of the
       block size. The length field is NOT counted, unlike the unencrypted
       form -- PROTOCOL.chacha20poly1305 excludes it because it is encrypted
       separately. */
    size_t body    = 1 + len;
    size_t pad_len = AXL_SSH_BLOCK - (body % AXL_SSH_BLOCK);
    if (pad_len < AXL_SSH_PAD_MIN) {
        pad_len += AXL_SSH_BLOCK;
    }
    size_t plen = body + pad_len;
    if (plen > AXL_SSH_PACKET_MAX) {
        return AXL_ERR;
    }

    uint8_t *buf = axl_malloc(LEN_FIELD + plen + AXL_SSH_AEAD_TAG_LEN);
    if (buf == NULL) {
        return AXL_ERR;
    }
    int rc = AXL_ERR;

    uint8_t *clear = axl_malloc(plen);
    if (clear == NULL) {
        axl_free(buf);
        return AXL_ERR;
    }
    clear[0] = (uint8_t)pad_len;
    if (len != 0) {
        axl_memcpy(clear + 1, payload, len);
    }
    /* Random padding, drawn from the per-connection pool rather than a
       direct firmware call per packet. A refill failure is fatal here
       exactly as a direct axl_rng_bytes failure was: predictable padding is
       a chosen-plaintext gift, and axl_ssh_rng_pool_take leaves the pool
       empty rather than handing back zeros. */
    if (axl_ssh_rng_pool_take(pad, clear + body, pad_len) != AXL_OK) {
        goto out;
    }

    uint8_t len_be[LEN_FIELD] = {
        (uint8_t)(plen >> 24), (uint8_t)(plen >> 16),
        (uint8_t)(plen >> 8),  (uint8_t)plen
    };

    uint8_t pkey[POLY_KEY_LEN];
    if (poly_key_for(a, seq, pkey) != AXL_OK
        || chacha(a->k_length, seq, 0, len_be, buf, LEN_FIELD) != AXL_OK
        || chacha(a->k_payload, seq, 1, clear, buf + LEN_FIELD, plen)
               != AXL_OK) {
        goto out;
    }
    if (mbedtls_poly1305_mac(pkey, buf, LEN_FIELD + plen,
                             buf + LEN_FIELD + plen) != 0) {
        goto out;
    }
    rc = axl_string_append_len(out, (const char *)buf,
                               LEN_FIELD + plen + AXL_SSH_AEAD_TAG_LEN);
out:
    axl_ssh_secure_zero(pkey, sizeof pkey);
    axl_ssh_secure_zero(clear, plen);
    axl_free(clear);
    axl_free(buf);
    return rc;
}

int
axl_ssh_aead_open(const AxlSshAead *a, uint64_t seq,
                  const uint8_t *p, size_t len, size_t *consumed,
                  uint8_t *out, size_t out_cap, uint32_t *out_len)
{
    if (a == NULL || !a->ready || p == NULL || consumed == NULL
        || out == NULL || out_len == NULL) {
        return AXL_ERR;
    }
    if (len < LEN_FIELD) {
        return AXL_INCOMPLETE;
    }

    /* The length is decrypted BEFORE anything is authenticated -- that is
       forced by the construction, and it is the one attacker-controlled value
       read ahead of the tag. So it is bounded here, before it is used to size
       or index anything. A peer that sends a bogus length gets a refusal, not
       an allocation. */
    uint8_t len_clear[LEN_FIELD];
    if (chacha(a->k_length, seq, 0, p, len_clear, LEN_FIELD) != AXL_OK) {
        return AXL_ERR;
    }
    uint32_t plen = ((uint32_t)len_clear[0] << 24)
                  | ((uint32_t)len_clear[1] << 16)
                  | ((uint32_t)len_clear[2] << 8)
                  | (uint32_t)len_clear[3];
    if (plen < AXL_SSH_PAD_MIN + 1 || plen > AXL_SSH_PACKET_MAX
        || (plen % AXL_SSH_BLOCK) != 0) {
        return AXL_ERR;
    }

    size_t total = LEN_FIELD + (size_t)plen + AXL_SSH_AEAD_TAG_LEN;
    if (len < total) {
        return AXL_INCOMPLETE;
    }

    /* Authenticate before decrypting the payload. Nothing below this point
       acts on bytes the peer chose without the tag having verified. */
    /* pkey is scrubbed on EVERY exit below, not only the success one. The
       tag-mismatch return is reachable by any peer that sends a bad MAC, so
       "an error path" here means "the path an attacker drives". */
    uint8_t pkey[POLY_KEY_LEN];
    uint8_t tag[AXL_SSH_AEAD_TAG_LEN];
    if (poly_key_for(a, seq, pkey) != AXL_OK
        || mbedtls_poly1305_mac(pkey, p, LEN_FIELD + plen, tag) != 0) {
        axl_ssh_secure_zero(pkey, sizeof pkey);
        return AXL_ERR;
    }
    if (!axl_consttime_equal(tag, p + LEN_FIELD + plen,
                             AXL_SSH_AEAD_TAG_LEN)) {
        axl_ssh_secure_zero(pkey, sizeof pkey);
        return AXL_ERR;
    }

    uint8_t *clear = axl_malloc(plen);
    if (clear == NULL) {
        axl_ssh_secure_zero(pkey, sizeof pkey);
        return AXL_ERR;
    }
    int rc = AXL_ERR;
    if (chacha(a->k_payload, seq, 1, p + LEN_FIELD, clear, plen) != AXL_OK) {
        goto done;
    }
    uint32_t pad = clear[0];
    /* Padding must leave at least one payload byte (the message number) and
       must not exceed what the packet holds. Both halves matter: the first
       stops a zero-length payload reaching a dispatcher that indexes [0], the
       second stops the subtraction below wrapping. */
    if (pad < AXL_SSH_PAD_MIN || (size_t)pad + 1 > plen) {
        goto done;
    }
    uint32_t payload_len = plen - 1 - pad;
    if (payload_len == 0 || payload_len > out_cap) {
        goto done;
    }
    axl_memcpy(out, clear + 1, payload_len);
    *out_len  = payload_len;
    *consumed = total;
    rc = AXL_OK;
done:
    axl_ssh_secure_zero(clear, plen);
    axl_free(clear);
    axl_ssh_secure_zero(pkey, sizeof pkey);
    return rc;
}
