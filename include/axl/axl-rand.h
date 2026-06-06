/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-rand.h
    Deterministic pseudo-random number generator.

    Mirrors GLib's `GRand`. A seedable, reproducible PRNG that is
    independent of any firmware entropy source — the complement to
    @ref axl-rng.h (`axl_rng_bytes`, which draws from the hardware
    `EFI_RNG_PROTOCOL` and has no deterministic mode). Reach for
    AxlRand when you want repeatable streams: test fixtures, sampling,
    retry-backoff jitter, procedural graphics (noise, particles),
    shuffling.

    The generator is xoshiro256** seeded through SplitMix64. A given
    seed produces the identical stream on every architecture — the
    output is defined as a sequence of 64-bit words w0, w1, w2, …,
    and every generator call is pinned to that sequence:

    - `axl_rand_uint64` returns the next word and advances one step.
    - `axl_rand_uint32` returns the **high 32 bits** of the next word
      (one step). It is a narrowing of the 64-bit stream, not a
      separate generator: `uint32` and `uint64` draw from the same
      sequence, one word per call.
    - `axl_rand_double` maps the next word's top 53 bits into [0,1).
    - `axl_rand_boolean` returns bit 63 of the next word.
    - `axl_rand_bytes` emits successive words in **little-endian**
      byte order (low byte first), truncating the final word.

    All of these are bit-identical across x86-64 and AArch64 (pure
    64-bit integer math, plus the standard 53-bit double
    construction). `axl_rand_double_range` is likewise bit-identical:
    the implementation rounds the scaled product to `double` before
    adding the offset, so no fused multiply-add can perturb the low
    bit. The generator is fast and statistically strong but is NOT
    cryptographically secure — use `axl_rng_bytes` for nonces, keys,
    and tokens.

    @code
    // Reproducible stream:
    AXL_AUTOPTR(AxlRand) r = axl_rand_new_seeded(0x1234);
    uint32_t a = axl_rand_uint32(r);
    int      d = axl_rand_int_range(r, 1, 7);   // dice roll, [1,7)
    double   t = axl_rand_double(r);            // [0.0, 1.0)

    // One-off without managing a handle (process-global stream):
    if (axl_random_boolean())
        axl_printf("heads\n");
    @endcode
**/

#ifndef AXL_RAND_H
#define AXL_RAND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque PRNG state (xoshiro256**).
 */
typedef struct AxlRand AxlRand;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Create a generator seeded from a non-reproducible source.
 *
 * Seeds from `axl_rng_bytes()` when the firmware entropy protocol is
 * available, otherwise from a monotonic clock reading. Two calls
 * therefore yield different streams. For a repeatable stream use
 * axl_rand_new_seeded(). Never returns NULL (aborts on OOM, per the
 * library's allocation contract).
 *
 * @return a new generator; free with axl_rand_free().
 */
AxlRand *
axl_rand_new(
    void
);

/**
 * @brief Create a generator with an explicit 64-bit seed.
 *
 * Deterministic: the same @p seed always produces the same stream.
 * Every seed value is valid (a zero seed is handled — the SplitMix64
 * expansion guarantees a non-degenerate xoshiro state).
 *
 * @return a new generator; free with axl_rand_free().
 */
AxlRand *
axl_rand_new_seeded(
    uint64_t seed  ///< seed value
);

/**
 * @brief Duplicate a generator, including its current position.
 *
 * The copy continues the identical stream from the point of the copy,
 * independently of the original. Useful for branching a reproducible
 * sequence. NULL-safe: returns NULL when @p r is NULL.
 *
 * @return a new generator, or NULL if @p r is NULL.
 */
AxlRand *
axl_rand_copy(
    const AxlRand *r  ///< generator to copy
);

/**
 * @brief Re-seed an existing generator in place.
 *
 * Resets the stream as if the generator had just been created with
 * axl_rand_new_seeded(@p seed). No-op when @p r is NULL.
 */
void
axl_rand_set_seed(
    AxlRand  *r,    ///< generator
    uint64_t  seed  ///< new seed value
);

/**
 * @brief Free a generator. NULL-safe.
 */
void
axl_rand_free(
    AxlRand *r  ///< generator (may be NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlRand, axl_rand_free)
#endif

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

/**
 * @brief Next 32-bit value, uniform over the full range.
 *
 * @return a value in [0, 2^32).
 */
uint32_t
axl_rand_uint32(
    AxlRand *r  ///< generator
);

/**
 * @brief Next 64-bit value, uniform over the full range.
 *
 * The generator's native word — no narrowing. Prefer this over two
 * axl_rand_uint32() calls when 64 bits are needed.
 *
 * @return a value in [0, 2^64).
 */
uint64_t
axl_rand_uint64(
    AxlRand *r  ///< generator
);

/**
 * @brief Uniform integer in the half-open range [@p begin, @p end).
 *
 * Rejection-sampled, so the result is unbiased across the whole
 * range (no modulo skew). The span is computed in 64-bit arithmetic,
 * so every @p begin < @p end pair is valid — including the full
 * [INT32_MIN, INT32_MAX) range whose width exceeds INT32_MAX.
 * A degenerate range (@p begin >= @p end) returns @p begin without
 * drawing from the stream. Note there is no 64-bit range variant —
 * for an unbiased 64-bit range, layer rejection sampling over
 * axl_rand_uint64().
 *
 * @return a value v with @p begin <= v < @p end, or @p begin if the
 *     range is empty.
 */
int32_t
axl_rand_int_range(
    AxlRand *r,      ///< generator
    int32_t  begin,  ///< inclusive lower bound
    int32_t  end     ///< exclusive upper bound
);

/**
 * @brief Uniform double in the half-open range [0.0, 1.0).
 *
 * 53 bits of resolution (one per representable double in the range).
 *
 * @return a value in [0.0, 1.0).
 */
double
axl_rand_double(
    AxlRand *r  ///< generator
);

/**
 * @brief Uniform double in the half-open range [@p begin, @p end).
 *
 * A degenerate range (@p begin >= @p end) returns @p begin.
 *
 * @return a value v with @p begin <= v < @p end, or @p begin if the
 *     range is empty.
 */
double
axl_rand_double_range(
    AxlRand *r,     ///< generator
    double   begin, ///< inclusive lower bound
    double   end    ///< exclusive upper bound
);

/**
 * @brief Fair coin flip.
 *
 * @return true or false, each with probability 0.5.
 */
bool
axl_rand_boolean(
    AxlRand *r  ///< generator
);

/**
 * @brief Fill a buffer with @p len pseudo-random bytes.
 *
 * Bytes are drawn from successive 64-bit stream words emitted
 * little-endian (see the file header). Deterministic for a given
 * seed and byte-identical across architectures — unlike
 * axl_rng_bytes(), which draws hardware entropy and returns a status.
 * This call cannot fail (returns void). @p len == 0 is a no-op.
 * Do NOT use for cryptographic material.
 */
void
axl_rand_bytes(
    AxlRand *r,    ///< generator
    void    *out,  ///< destination buffer
    size_t   len   ///< number of bytes to write
);

// ---------------------------------------------------------------------------
// Process-global convenience stream (mirrors GLib's g_random_*)
// ---------------------------------------------------------------------------
//
// Backed by a single shared generator, lazily seeded from a
// non-reproducible source on first use. Single-threaded (UEFI BSP) —
// no locking. Call axl_random_set_seed() first for a reproducible
// global stream (e.g. in tests).

/**
 * @brief Seed the process-global generator for a reproducible stream.
 *
 * May be called at any time; resets the global stream to seed @p seed
 * regardless of whether it had already been lazily seeded or used.
 * Call this before the first axl_random_* use in a test to get a
 * deterministic global sequence.
 */
void
axl_random_set_seed(
    uint64_t seed  ///< seed value
);

/**
 * @brief Next 32-bit value from the global generator. @see axl_rand_uint32.
 */
uint32_t
axl_random_uint32(
    void
);

/**
 * @brief Uniform integer in [@p begin, @p end) from the global generator.
 *        @see axl_rand_int_range.
 */
int32_t
axl_random_int_range(
    int32_t begin,  ///< inclusive lower bound
    int32_t end     ///< exclusive upper bound
);

/**
 * @brief Uniform double in [0.0, 1.0) from the global generator.
 *        @see axl_rand_double.
 */
double
axl_random_double(
    void
);

/**
 * @brief Uniform double in [@p begin, @p end) from the global generator.
 *        @see axl_rand_double_range.
 */
double
axl_random_double_range(
    double begin,  ///< inclusive lower bound
    double end     ///< exclusive upper bound
);

/**
 * @brief Fair coin flip from the global generator. @see axl_rand_boolean.
 */
bool
axl_random_boolean(
    void
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RAND_H */
