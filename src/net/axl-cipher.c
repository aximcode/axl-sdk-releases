/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cipher.c
    AES-CTR keystream cipher.

    When AXL_HAVE_TLS is defined, a stateful AES-CTR context over mbedTLS
    whose counter advances across calls. Otherwise the constructor
    returns NULL and xcrypt returns AXL_ERR (fail-closed).
**/

#include <axl/axl-crypto.h>


#include <axl/axl-mem.h>
#include <axl/axl-str.h>

#include <mbedtls/aes.h>

struct AxlCipher {
    mbedtls_aes_context aes;
    uint8_t             nonce_counter[16];  /* running counter block */
    uint8_t             stream_block[16];   /* current keystream remainder */
    size_t              nc_off;             /* offset within stream_block */
};

AxlCipher *
axl_cipher_ctr_new(AxlCipherAlg alg, const uint8_t *key, size_t key_len,
                   const uint8_t *iv)
{
    size_t kb = (alg == AXL_CIPHER_AES_128_CTR) ? 16
              : (alg == AXL_CIPHER_AES_256_CTR) ? 32 : 0;
    if (kb == 0 || key == NULL || key_len != kb || iv == NULL) {
        return NULL;
    }

    AxlCipher *c = axl_malloc(sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    mbedtls_aes_init(&c->aes);
    /* CTR uses the forward AES transform for both encrypt and decrypt. */
    if (mbedtls_aes_setkey_enc(&c->aes, key, (unsigned int)(key_len * 8)) != 0) {
        mbedtls_aes_free(&c->aes);
        axl_free(c);
        return NULL;
    }
    axl_memcpy(c->nonce_counter, iv, sizeof(c->nonce_counter));
    axl_memset(c->stream_block, 0, sizeof(c->stream_block));
    c->nc_off = 0;
    return c;
}

int
axl_cipher_ctr_xcrypt(AxlCipher *c, const uint8_t *in, size_t len, uint8_t *out)
{
    if (c == NULL || (len != 0 && (in == NULL || out == NULL))) {
        return AXL_ERR;
    }
    if (len == 0) {
        return AXL_OK;
    }
    return (mbedtls_aes_crypt_ctr(&c->aes, len, &c->nc_off, c->nonce_counter,
                                  c->stream_block, in, out) == 0)
           ? AXL_OK : AXL_ERR;
}

void
axl_cipher_free(AxlCipher *c)
{
    if (c != NULL) {
        mbedtls_aes_free(&c->aes);
        axl_memset(c->nonce_counter, 0, sizeof(c->nonce_counter));
        axl_memset(c->stream_block, 0, sizeof(c->stream_block));
        axl_free(c);
    }
}

