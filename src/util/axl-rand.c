/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-rand.c
    Deterministic PRNG: xoshiro256** seeded via SplitMix64.

    xoshiro256** is David Blackman & Sebastiano Vigna's public-domain
    generator (https://prng.di.unimi.it/). SplitMix64 expands a single
    64-bit seed into the 256-bit state, guaranteeing a non-degenerate
    state for every seed (including zero). All output paths are pinned
    to a single 64-bit word stream so results are byte-identical across
    architectures — see axl-rand.h for the stream definition.
**/

#include <axl/axl-rand.h>
#include <axl/axl-rng.h>
#include <axl/axl-time.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-macros.h>

struct AxlRand {
    uint64_t s[4];
};

// ---------------------------------------------------------------------------
// Core algorithm
// ---------------------------------------------------------------------------

static uint64_t
splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void
seed_state(AxlRand *r, uint64_t seed)
{
    uint64_t st = seed;
    for (int i = 0; i < 4; i++) {
        r->s[i] = splitmix64(&st);
    }
}

static inline uint64_t
rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

// Next 64-bit word — the single stream every other call draws from.
static uint64_t
next_word(AxlRand *r)
{
    uint64_t *s = r->s;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);

    return result;
}

// Non-reproducible seed: hardware entropy if available, else the clock.
static uint64_t
entropy_seed(void)
{
    uint64_t seed;
    if (axl_rng_bytes(&seed, sizeof(seed)) != AXL_OK) {
        seed = axl_time_get_us();
    }
    return seed;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AxlRand *
axl_rand_new(void)
{
    return axl_rand_new_seeded(entropy_seed());
}

AxlRand *
axl_rand_new_seeded(uint64_t seed)
{
    AxlRand *r = axl_calloc(1, sizeof(*r));
    seed_state(r, seed);
    return r;
}

AxlRand *
axl_rand_copy(const AxlRand *r)
{
    if (r == NULL) {
        return NULL;
    }
    AxlRand *copy = axl_calloc(1, sizeof(*copy));
    axl_memcpy(copy->s, r->s, sizeof(r->s));
    return copy;
}

void
axl_rand_set_seed(AxlRand *r, uint64_t seed)
{
    if (r != NULL) {
        seed_state(r, seed);
    }
}

void
axl_rand_free(AxlRand *r)
{
    axl_free(r);
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

uint64_t
axl_rand_uint64(AxlRand *r)
{
    return next_word(r);
}

uint32_t
axl_rand_uint32(AxlRand *r)
{
    return (uint32_t)(next_word(r) >> 32);
}

int32_t
axl_rand_int_range(AxlRand *r, int32_t begin, int32_t end)
{
    if (begin >= end) {
        return begin;  // degenerate range — also guards span==0 div-by-zero
    }
    // span is at most 2^32-1 (int32 bounds); computed in 64-bit so the
    // full [INT32_MIN, INT32_MAX) range doesn't overflow.
    uint64_t span = (uint64_t)((int64_t)end - (int64_t)begin);

    // Unbiased rejection: accept words in [0, floor(2^64/span)*span).
    // rem = 2^64 mod span; when rem == 0 (span is a power of two) every
    // word is acceptable. limit wraps to floor(2^64/span)*span mod 2^64.
    uint64_t rem = (uint64_t)(0 - span) % span;
    uint64_t limit = 0 - rem;

    uint64_t w;
    do {
        w = next_word(r);
    } while (rem != 0 && w >= limit);

    return (int32_t)((int64_t)begin + (int64_t)(w % span));
}

double
axl_rand_double(AxlRand *r)
{
    // Top 53 bits -> [0,1). 1.0/2^53 is exact, and the shifted value is
    // an integer < 2^53 (exactly representable), so the product is exact
    // and identical on every architecture.
    return (double)(next_word(r) >> 11) * (1.0 / 9007199254740992.0);
}

double
axl_rand_double_range(AxlRand *r, double begin, double end)
{
    if (begin >= end) {
        return begin;  // degenerate range
    }
    // Round the scaled product to double (named intermediate) before
    // adding the offset, so no fused multiply-add can perturb the low
    // bit — keeps the result bit-identical across architectures.
    double scaled = axl_rand_double(r) * (end - begin);
    return begin + scaled;
}

bool
axl_rand_boolean(AxlRand *r)
{
    return (next_word(r) >> 63) != 0;
}

void
axl_rand_bytes(AxlRand *r, void *out, size_t len)
{
    uint8_t *p = out;
    while (len >= 8) {
        uint64_t w = next_word(r);
        for (int j = 0; j < 8; j++) {
            p[j] = (uint8_t)(w >> (8 * j));
        }
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t w = next_word(r);
        for (size_t j = 0; j < len; j++) {
            p[j] = (uint8_t)(w >> (8 * j));
        }
    }
}

// ---------------------------------------------------------------------------
// Process-global convenience stream
//
// A file-static generator (no allocation, so nothing to leak), lazily
// seeded from a non-reproducible source on first use. Single-threaded
// UEFI BSP — no locking.
// ---------------------------------------------------------------------------

static AxlRand g_global;
static bool    g_global_seeded = false;

static AxlRand *
global_rand(void)
{
    if (!g_global_seeded) {
        seed_state(&g_global, entropy_seed());
        g_global_seeded = true;
    }
    return &g_global;
}

void
axl_random_set_seed(uint64_t seed)
{
    seed_state(&g_global, seed);
    g_global_seeded = true;
}

uint32_t
axl_random_uint32(void)
{
    return axl_rand_uint32(global_rand());
}

int32_t
axl_random_int_range(int32_t begin, int32_t end)
{
    return axl_rand_int_range(global_rand(), begin, end);
}

double
axl_random_double(void)
{
    return axl_rand_double(global_rand());
}

double
axl_random_double_range(double begin, double end)
{
    return axl_rand_double_range(global_rand(), begin, end);
}

bool
axl_random_boolean(void)
{
    return axl_rand_boolean(global_rand());
}
