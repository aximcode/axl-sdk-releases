/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-digest-internal.h
    Per-algorithm state structs and init/update/final prototypes
    for the AXL digest subsystem. Included by the three algorithm
    implementation files (axl-digest-md5.c, axl-digest-sha1.c,
    axl-digest-sha256.c) and by the public dispatcher (axl-digest.c),
    which embeds the state structs in a union inside AxlChecksum.

    Not a public header — do not include from outside src/data/.
**/

#ifndef AXL_DIGEST_INTERNAL_H
#define AXL_DIGEST_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} Md5State;

void md5_init(Md5State *s);
void md5_update(Md5State *s, const uint8_t *data, size_t len);
void md5_final(Md5State *s, uint8_t out[16]);

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} Sha1State;

void sha1_init(Sha1State *s);
void sha1_update(Sha1State *s, const uint8_t *data, size_t len);
void sha1_final(Sha1State *s, uint8_t out[20]);

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} Sha256State;

void sha256_init(Sha256State *s);
void sha256_update(Sha256State *s, const uint8_t *data, size_t len);
void sha256_final(Sha256State *s, uint8_t out[32]);

#endif /* AXL_DIGEST_INTERNAL_H */
