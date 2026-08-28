/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-aead.c
    Authenticated encryption (AEAD): AES-GCM and ChaCha20-Poly1305.

    When AXL_HAVE_TLS is defined, one-shot seal/open via mbedTLS.
    Otherwise the functions return AXL_ERR (fail-closed).
**/

#include <axl/axl-crypto.h>


#include <mbedtls/gcm.h>
#include <mbedtls/chachapoly.h>

/* Key length in bytes for an AEAD algorithm, or 0 if unknown. */
static size_t
aead_key_bytes(AxlAeadAlg alg)
{
    switch (alg) {
    case AXL_AEAD_AES_128_GCM:
        return 16;
    case AXL_AEAD_AES_256_GCM:
    case AXL_AEAD_CHACHA20_POLY1305:
        return 32;
    default:
        return 0;
    }
}

/* Validate the arguments common to seal and open. @p data may be NULL
   only when @p data_len is 0 (the payload). */
static bool
aead_args_ok(AxlAeadAlg alg, const uint8_t *key, size_t key_len,
             const uint8_t *nonce, size_t nonce_len,
             const uint8_t *aad, size_t aad_len,
             const uint8_t *tag, size_t tag_len,
             const uint8_t *data, size_t data_len,
             const uint8_t *out)
{
    size_t kb = aead_key_bytes(alg);
    return kb != 0 && key != NULL && key_len == kb
        && nonce != NULL && nonce_len == AXL_AEAD_NONCE_LEN
        && tag != NULL && tag_len == AXL_AEAD_TAG_LEN
        && (aad != NULL || aad_len == 0)
        && (data != NULL || data_len == 0)
        && (out != NULL || data_len == 0);
}

int
axl_aead_seal(AxlAeadAlg alg, const uint8_t *key, size_t key_len,
              const uint8_t *nonce, size_t nonce_len,
              const uint8_t *aad, size_t aad_len,
              const uint8_t *plaintext, size_t pt_len,
              uint8_t *ciphertext, uint8_t *tag, size_t tag_len)
{
    if (!aead_args_ok(alg, key, key_len, nonce, nonce_len, aad, aad_len,
                      tag, tag_len, plaintext, pt_len, ciphertext)) {
        return AXL_ERR;
    }

    if (alg == AXL_AEAD_CHACHA20_POLY1305) {
        mbedtls_chachapoly_context ctx;
        mbedtls_chachapoly_init(&ctx);
        int rc = mbedtls_chachapoly_setkey(&ctx, key);
        if (rc == 0) {
            rc = mbedtls_chachapoly_encrypt_and_tag(&ctx, pt_len, nonce,
                                                    aad, aad_len,
                                                    plaintext, ciphertext, tag);
        }
        mbedtls_chachapoly_free(&ctx);
        return (rc == 0) ? AXL_OK : AXL_ERR;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key,
                                (unsigned int)(key_len * 8));
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, pt_len,
                                       nonce, nonce_len, aad, aad_len,
                                       plaintext, ciphertext, tag_len, tag);
    }
    mbedtls_gcm_free(&ctx);
    return (rc == 0) ? AXL_OK : AXL_ERR;
}

int
axl_aead_open(AxlAeadAlg alg, const uint8_t *key, size_t key_len,
              const uint8_t *nonce, size_t nonce_len,
              const uint8_t *aad, size_t aad_len,
              const uint8_t *ciphertext, size_t ct_len,
              const uint8_t *tag, size_t tag_len,
              uint8_t *plaintext)
{
    if (!aead_args_ok(alg, key, key_len, nonce, nonce_len, aad, aad_len,
                      tag, tag_len, ciphertext, ct_len, plaintext)) {
        return AXL_ERR;
    }

    if (alg == AXL_AEAD_CHACHA20_POLY1305) {
        mbedtls_chachapoly_context ctx;
        mbedtls_chachapoly_init(&ctx);
        int rc = mbedtls_chachapoly_setkey(&ctx, key);
        if (rc == 0) {
            rc = mbedtls_chachapoly_auth_decrypt(&ctx, ct_len, nonce,
                                                 aad, aad_len, tag,
                                                 ciphertext, plaintext);
        }
        mbedtls_chachapoly_free(&ctx);
        return (rc == 0) ? AXL_OK : AXL_ERR;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key,
                                (unsigned int)(key_len * 8));
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx, ct_len, nonce, nonce_len,
                                      aad, aad_len, tag, tag_len,
                                      ciphertext, plaintext);
    }
    mbedtls_gcm_free(&ctx);
    return (rc == 0) ? AXL_OK : AXL_ERR;
}

