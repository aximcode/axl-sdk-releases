/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-crypto-rng.c
    Lazy process-global DRBG shared by the mbedTLS-backed crypto modules.

    Compiled into every build — mbedTLS is an unconditional dependency.
**/

#include <axl/axl-crypto.h>


#include <axl/axl-atexit.h>
#include "axl-crypto-internal.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

/* Provided by axl-mbedtls-platform.c (the entropy source TLS uses). */
int axl_mbedtls_entropy_poll(void *data, unsigned char *output,
                             size_t len, size_t *olen);

static bool                     g_rng_ready;
static mbedtls_ctr_drbg_context g_rng;
static mbedtls_entropy_context  g_rng_entropy;

static void
crypto_rng_cleanup(void *unused)
{
    (void)unused;
    if (g_rng_ready) {
        mbedtls_ctr_drbg_free(&g_rng);
        mbedtls_entropy_free(&g_rng_entropy);
        g_rng_ready = false;
    }
}

mbedtls_ctr_drbg_context *
axl_crypto_rng(void)
{
    if (g_rng_ready) {
        return &g_rng;
    }
    mbedtls_ctr_drbg_init(&g_rng);
    mbedtls_entropy_init(&g_rng_entropy);

    static const unsigned char pers[] = "axl-crypto";
    if (mbedtls_entropy_add_source(&g_rng_entropy, axl_mbedtls_entropy_poll,
                                   NULL, 32,
                                   MBEDTLS_ENTROPY_SOURCE_STRONG) != 0
        || mbedtls_ctr_drbg_seed(&g_rng, mbedtls_entropy_func, &g_rng_entropy,
                                 pers, sizeof(pers) - 1) != 0) {
        mbedtls_ctr_drbg_free(&g_rng);
        mbedtls_entropy_free(&g_rng_entropy);
        return NULL;
    }
    g_rng_ready = true;
    axl_atexit(crypto_rng_cleanup, NULL);
    return &g_rng;
}

