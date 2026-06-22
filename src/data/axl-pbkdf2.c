/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pbkdf2.c
    PBKDF2-HMAC-SHA256 (RFC 8018 §5.2) layered on the dependency-free
    AxlHmac engine — works in every build (no AXL_TLS).

    DK = T_1 ‖ T_2 ‖ ... where T_i = U_1 XOR U_2 XOR ... XOR U_c,
    U_1 = HMAC(P, S ‖ INT32BE(i)), U_k = HMAC(P, U_{k-1}).
**/

#include <axl/axl-digest.h>
#include <axl/axl-hmac.h>
#include <axl/axl-str.h>   /* axl_memcpy */

#define PBKDF2_HLEN  32    /* SHA-256 output size */

/* One HMAC-SHA256(key, a‖b) into out[32]. Returns true on success. */
static bool
hmac_sha256(const uint8_t *key, size_t key_len,
            const uint8_t *a, size_t a_len,
            const uint8_t *b, size_t b_len,
            uint8_t out[PBKDF2_HLEN])
{
    AxlHmac *h = axl_hmac_new(AXL_CHECKSUM_SHA256, key, key_len);
    if (h == NULL) {
        return false;
    }
    if (a_len > 0) {
        axl_hmac_update(h, a, a_len);
    }
    if (b_len > 0) {
        axl_hmac_update(h, b, b_len);
    }
    size_t len = PBKDF2_HLEN;
    axl_hmac_get_digest(h, out, &len);
    axl_hmac_free(h);
    return len == PBKDF2_HLEN;
}

int
axl_pbkdf2_hmac_sha256(
    const uint8_t *password,
    size_t         password_len,
    const uint8_t *salt,
    size_t         salt_len,
    uint32_t       iterations,
    uint8_t       *out,
    size_t         out_len
    )
{
    if (iterations == 0 || out == NULL || out_len == 0) {
        return AXL_INVALID;
    }
    if ((password == NULL && password_len > 0) ||
        (salt == NULL && salt_len > 0)) {
        return AXL_INVALID;
    }

    size_t blocks = (out_len + PBKDF2_HLEN - 1) / PBKDF2_HLEN;

    for (uint32_t i = 1; i <= blocks; i++) {
        uint8_t u[PBKDF2_HLEN];
        uint8_t t[PBKDF2_HLEN];
        uint8_t i_be[4] = {
            (uint8_t)(i >> 24), (uint8_t)(i >> 16),
            (uint8_t)(i >> 8),  (uint8_t)i
        };

        /* U_1 = HMAC(P, S ‖ INT32BE(i)). */
        if (!hmac_sha256(password, password_len, salt, salt_len,
                         i_be, sizeof i_be, u)) {
            return AXL_ERR;
        }
        axl_memcpy(t, u, PBKDF2_HLEN);

        /* U_k = HMAC(P, U_{k-1}); T_i ^= U_k. */
        for (uint32_t k = 2; k <= iterations; k++) {
            if (!hmac_sha256(password, password_len, u, PBKDF2_HLEN,
                             NULL, 0, u)) {
                return AXL_ERR;
            }
            for (size_t j = 0; j < PBKDF2_HLEN; j++) {
                t[j] ^= u[j];
            }
        }

        size_t off = (size_t)(i - 1) * PBKDF2_HLEN;
        size_t n = (out_len - off < PBKDF2_HLEN) ? out_len - off : PBKDF2_HLEN;
        axl_memcpy(out + off, t, n);
    }

    return AXL_OK;
}
