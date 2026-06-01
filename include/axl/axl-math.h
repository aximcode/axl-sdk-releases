/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-math.h
    Floating-point math primitives — libm-free.

    axl-sdk's freestanding UEFI build links with `-nostdlib` and
    cannot rely on libm.  GCC's `__builtin_floor` / `__builtin_sin`
    et al. usually lower to libm calls on baseline targets (the
    SSE4.1 ROUND instruction is not in the `-march=x86-64`
    baseline, and there's no hardware sin/cos on either x64 or
    AArch64).

    This module exposes a small, libm-free implementation of the
    math primitives downstream consumers (axl-truetype, axl-gfx-
    path, future AGT widget animations, downstream image-codec
    integrations) actually need.  Accuracy is sufficient for UI
    coordinates and animation easing — not for numerical analysis.

    Module name: AxlMath.  All values are `double` so callers can
    mix integer and floating-point inputs without precision
    surprises; consumers that pin `float` storage cast at the
    boundary.

    @code
    double a = axl_sqrt(2.0);             // 1.414...
    int    pen_pixel = axl_floori(p.x);   // round-down to int pixel
    double phase    = axl_sin(t * 2.0);   // animation easing
    @endcode
**/

#ifndef AXL_MATH_H
#define AXL_MATH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Constants
// ===================================================================

#define AXL_MATH_PI         3.141592653589793
#define AXL_MATH_TWO_PI     6.283185307179586
#define AXL_MATH_HALF_PI    1.5707963267948966

/// Euler's number — base of the natural logarithm.
#define AXL_MATH_E          2.718281828459045

/// Square root of 2 — diagonal of the unit square.
#define AXL_MATH_SQRT_2     1.4142135623730951

/// Natural log of 2 — handy for base-2 ↔ base-e conversions.
#define AXL_MATH_LOG_2      0.6931471805599453

/// Golden ratio φ = (1 + √5) / 2 — UI proportions, Fibonacci spirals.
#define AXL_MATH_GOLDEN     1.618033988749895

/// Degrees → radians conversion factor (π / 180).
#define AXL_MATH_DEG_TO_RAD 0.017453292519943295

/// Radians → degrees conversion factor (180 / π).
#define AXL_MATH_RAD_TO_DEG 57.29577951308232

// ===================================================================
// Rounding / sign
// ===================================================================

/// Floor — largest integer value not greater than @a x.
///
/// `axl_floor(3.7)  == 3.0`, `axl_floor(-3.2) == -4.0`.
/// Always returns the mathematical floor; no libm linkage.
double
axl_floor(
    double  x
    );

/// Ceiling — smallest integer value not less than @a x.
///
/// `axl_ceil(3.2)  == 4.0`, `axl_ceil(-3.7) == -3.0`.
double
axl_ceil(
    double  x
    );

/// Floor returned as `int` directly — convenience for pixel-snap
/// and bbox arithmetic where the next consumer is integer-typed.
int
axl_floori(
    double  x
    );

/// Ceiling returned as `int` directly.
int
axl_ceili(
    double  x
    );

/// Absolute value.
///
/// `axl_fabs(-3.5) == 3.5`, `axl_fabs(0.0) == 0.0`.
double
axl_fabs(
    double  x
    );

// ===================================================================
// Roots / modulus
// ===================================================================

/// Square root — Newton's method internally.
///
/// `axl_sqrt(4.0) == 2.0`, `axl_sqrt(0.0) == 0.0`.  Accurate to
/// roughly the precision of `double` for non-tiny non-huge inputs
/// (UI-scale magnitudes).
///
/// @warning Negative input clamps to 0 (NOT NaN, unlike libm).
///          Convention chosen because UI consumers usually mean
///          "absolute distance" — catch sign bugs by checking
///          input rather than result == 0.
double
axl_sqrt(
    double  x
    );

/// Floating-point modulo — `x - trunc(x/y) * y`.
///
/// `axl_fmod(7.0, 3.0) == 1.0`.  Sufficient precision for curve
/// flattening and angle wrap; not IEEE-correct for huge magnitudes.
double
axl_fmod(
    double  x,
    double  y
    );

/// Wrap @a x into the half-open range `[0, n)` regardless of sign.
///
/// Equivalent to `x - n * floor(x / n)` — same as `axl_fmod` but
/// using `floor` (toward -∞) instead of truncation (toward zero),
/// so negative inputs wrap cleanly into the positive range.
///
/// Useful for circular indices (sprite frame % N over floats) and
/// angle normalization: `axl_wrap(angle, AXL_MATH_TWO_PI)` brings any
/// angle into `[0, 2π)`.
///
/// `axl_wrap(3.5, 10.0)  == 3.5`  (positive in-range, identity)
/// `axl_wrap(12.5, 10.0) == 2.5`  (positive over-range)
/// `axl_wrap(-1.5, 10.0) == 8.5`  (negative wraps to positive)
/// `axl_wrap(10.0, 10.0) == 0.0`  (right boundary excluded)
///
/// For `n <= 0` the result is implementation-defined; the current
/// implementation returns 0 (safe-default, mirroring
/// `axl_fmod`'s zero-divisor convention).
///
/// @return Value in `[0, n)` for `n > 0`, else `0`.
double
axl_wrap(
    double  x,  ///< value to wrap
    double  n   ///< period (must be `> 0`)
    );

// ===================================================================
// Trigonometry — Taylor series with range reduction
// ===================================================================

/// Sine.
///
/// Range-reduces @a x to `[-π/2, π/2]` and evaluates a 6-term
/// Taylor series (through `x¹¹/11!`).  Accurate to ~1e-7 over
/// the full input range.
double
axl_sin(
    double  x
    );

/// Cosine.  Derived from `axl_sin` via the standard identity
/// `cos(x) = sin(x + π/2)`.
double
axl_cos(
    double  x
    );

// ===================================================================
// Logarithmic / exponential
// ===================================================================

/// Natural logarithm — base e.
///
/// Named `axl_ln` (not `axl_log`) to avoid clashing with the
/// logging-system function of the same name in `<axl/axl-log.h>`.
/// The math notation `ln` is unambiguous for natural log.
///
/// Range-reduces via the IEEE 754 exponent: `x = m * 2^e` with
/// `m ∈ [1, 2)`, so `ln(x) = ln(m) + e * AXL_MATH_LOG_2`.
/// Mantissa log evaluated via the substitution `s = (m-1)/(m+1)`
/// and the odd-only series
/// `ln(m) = 2 * (s + s³/3 + s⁵/5 + s⁷/7 + ...)`.
/// Accurate to ~1e-10 over the normal-double input range.
///
/// `axl_ln(AXL_MATH_E) == 1.0`, `axl_ln(1.0) == 0.0`,
/// `axl_ln(2.0) == AXL_MATH_LOG_2`.
///
/// @warning Non-positive input returns 0 (NOT -∞ or NaN as libm
///          would).  Same convention as `axl_sqrt` — catch sign
///          bugs by checking the input rather than the result.
///
/// @return `ln(x)` for `x > 0`, else `0`.
double
axl_ln(
    double  x
    );

/// Natural exponential — `e^x`.
///
/// Range-reduces via `x = k * log(2) + r` with `k` integer and
/// `r ∈ [-log(2)/2, log(2)/2]`, evaluates a 10-term Taylor series
/// on `r`, then reconstructs `2^k` by writing the IEEE 754
/// exponent field directly.  Accurate to ~1e-12 for inputs within
/// roughly `[-700, 700]` (the exponent range that fits in a
/// normal double).
///
/// `axl_exp(0.0) == 1.0`, `axl_exp(1.0) ≈ AXL_MATH_E`,
/// `axl_exp(AXL_MATH_LOG_2) == 2.0`.
///
/// Large positive inputs saturate at the maximum representable
/// double (~1.8e308) — no FP overflow trap.  Large negative
/// inputs underflow cleanly to 0.
///
/// @return `e^x`, saturated at the double range boundaries.
double
axl_exp(
    double  x
    );

/// Power — `base` raised to `exponent`.
///
/// Implemented as `exp(exponent * ln(base))` for `base > 0`.
/// Accuracy compounds the `ln` and `exp` errors but stays
/// comfortably under 1e-6 for UI-scale inputs (sRGB↔linear
/// conversion with γ=2.2, easing curves, alpha falloff, etc.).
///
/// Fast-path edge cases:
///   - `axl_pow(x, 0.0) == 1.0` for any @a base, including
///     `axl_pow(0.0, 0.0)` per IEEE 754 §9.2.1.
///   - `axl_pow(1.0, y) == 1.0` for any finite @a exponent.
///   - `axl_pow(0.0, y > 0)` == 0.
///
/// @warning Negative base returns 0 — we don't distinguish
///          integer from non-integer @a exponent (libm would
///          return a real value for integer @a exponent and NaN
///          for non-integer).  Callers that need negative-base
///          power must implement the integer-exponent case
///          themselves.
///
/// @return `base ^ exponent`.
double
axl_pow(
    double  base,      ///< must be `>= 0`
    double  exponent
    );

// ===================================================================
// Inverse trigonometry
// ===================================================================

/// Arctangent — angle whose tangent is @a x.
///
/// Range-reduces via two identities:
///   - `|x| > 1`  →  `atan(x) = sgn(x) * π/2 - atan(1/x)`.
///   - `|x| > 0.5` →  `atan(x) = π/4 * sgn(x) + atan((|x|-1)/(|x|+1))`.
/// After reductions `|x| ≤ 0.5` (in fact `|x| ≤ 1/3` if both steps
/// fire); Taylor `atan(x) = x - x³/3 + x⁵/5 - ...` then converges
/// fast.  Accurate to ~1e-9 across the full input range.
///
/// `axl_atan(0.0) == 0.0`, `axl_atan(1.0) ≈ π/4`,
/// `axl_atan(±∞)` returns `±π/2`.
///
/// @return Angle in `[-π/2, π/2]`.
double
axl_atan(
    double  x
    );

/// Two-argument arctangent — full-circle angle of vector `(x, y)`.
///
/// Returns the angle from the positive x-axis to the vector
/// `(x, y)`, in the range `(-π, π]`.  Standard quadrant handling:
///   - `x > 0`             → `atan(y/x)` in `(-π/2, π/2)`
///   - `x < 0`, `y ≥ 0`    → `atan(y/x) + π` in `(π/2, π]`
///   - `x < 0`, `y < 0`    → `atan(y/x) - π` in `(-π, -π/2)`
///   - `x == 0`, `y > 0`   → `π/2`
///   - `x == 0`, `y < 0`   → `-π/2`
///   - `x == 0`, `y == 0`  → `0` (degenerate input, safe-default)
///
/// `axl_atan2(0.0, 1.0) == 0`, `axl_atan2(1.0, 0.0) == π/2`,
/// `axl_atan2(0.0, -1.0) == π`.
///
/// Sign-of-zero is NOT distinguished: `axl_atan2(-0.0, -1.0)`
/// returns `+π`, not `-π` as IEEE 754 would.  axl-sdk has no
/// hard wire-compat bar with libm here; consumers needing IEEE
/// sign-of-zero semantics can wrap with their own signbit check.
///
/// @return Angle in `(-π, π]`.
double
axl_atan2(
    double  y,   ///< vertical component
    double  x    ///< horizontal component
    );

/// Arcsine — angle whose sine is @a x.
///
/// Computed via the identity `asin(x) = atan(x / √(1 - x²))` for
/// `|x| < 1`, with exact endpoints at `±1`.
///
/// `axl_asin(0.0) == 0.0`, `axl_asin(1.0) == π/2`,
/// `axl_asin(-1.0) == -π/2`.
///
/// @warning Out-of-domain input (`|x| > 1`) returns 0 — same
///          safe-default convention as `axl_sqrt` for negative
///          input.  Caller is expected to check the domain.
///
/// @return Angle in `[-π/2, π/2]`.
double
axl_asin(
    double  x
    );

/// Arccosine — angle whose cosine is @a x.
///
/// Implemented as `π/2 - axl_asin(x)`.
///
/// `axl_acos(1.0) == 0.0`, `axl_acos(0.0) == π/2`,
/// `axl_acos(-1.0) == π`.
///
/// @warning Out-of-domain input (`|x| > 1`) returns 0 (same
///          convention as `axl_asin`).
///
/// @return Angle in `[0, π]`.
double
axl_acos(
    double  x
    );

// ===================================================================
// Range / interpolation helpers
// ===================================================================

/// Clamp @a x to the closed interval `[lo, hi]`.
///
/// `axl_clamp(5.0, 0.0, 10.0) == 5.0`,
/// `axl_clamp(-1.0, 0.0, 10.0) == 0.0`,
/// `axl_clamp(11.0, 0.0, 10.0) == 10.0`.
///
/// @warning If `lo > hi` the result is implementation-defined; the
///          current implementation returns @a hi (i.e. the upper
///          clamp wins).  Callers are expected to pass `lo ≤ hi`.
///
/// @return @a x clamped to `[lo, hi]`.
double
axl_clamp(
    double  x,   ///< value to clamp
    double  lo,  ///< lower bound (inclusive)
    double  hi   ///< upper bound (inclusive)
    );

/// Smaller of two values.
///
/// `axl_min(3.0, 5.0) == 3.0`, `axl_min(-2.0, -5.0) == -5.0`.
///
/// @return The smaller of @a a and @a b (returns @a b on tie, but
///         either is correct for equal inputs).
double
axl_min(
    double  a,
    double  b
    );

/// Larger of two values.
///
/// `axl_max(3.0, 5.0) == 5.0`, `axl_max(-2.0, -5.0) == -2.0`.
///
/// @return The larger of @a a and @a b.
double
axl_max(
    double  a,
    double  b
    );

/// Linear remap — map @a x from input range `[in_min, in_max]` to
/// output range `[out_min, out_max]`.
///
/// `axl_remap(50, 0, 100, 0, 1) == 0.5`.  Input values outside
/// `[in_min, in_max]` extrapolate linearly (the formula is not
/// clamped — wrap with `axl_clamp` at the call site if you want
/// saturation).
///
/// Degenerate input range (`in_min == in_max`) returns @a out_min
/// to avoid division by zero.
///
/// @return The remapped value.
double
axl_remap(
    double  x,        ///< input value
    double  in_min,   ///< input range lower bound
    double  in_max,   ///< input range upper bound
    double  out_min,  ///< output range lower bound
    double  out_max   ///< output range upper bound
    );

/// GLSL-style step function.
///
/// Returns 0.0 if `x < edge`, else 1.0.  The boundary value
/// `x == edge` returns 1.0 (matches the GLSL spec).
///
/// @return 0.0 or 1.0.
double
axl_step(
    double  edge,
    double  x
    );

/// GLSL-style smoothstep — cubic Hermite interpolation.
///
/// Returns 0.0 when `x ≤ edge0`, 1.0 when `x ≥ edge1`, and a
/// smooth `t*t*(3 - 2*t)` Hermite curve in between, where
/// `t = clamp((x - edge0)/(edge1 - edge0), 0, 1)`.
///
/// Useful for anti-aliased edge transitions and easing.  The
/// midpoint `(edge0 + edge1) / 2` returns exactly 0.5.
///
/// @return Smoothly interpolated value in `[0.0, 1.0]`.
double
axl_smoothstep(
    double  edge0,
    double  edge1,
    double  x
    );

// ===================================================================
// Interpolation + easing
// ===================================================================

/// Linear interpolation: `a + (b - a) * t`.
///
/// `axl_lerp(0, 100, 0.5) == 50`.  Inputs outside `[0, 1]`
/// extrapolate linearly (no clamping — wrap with `axl_clamp` at
/// the call site if you want saturation, same shape as
/// `axl_remap`).
///
/// `t == 0` returns @a a exactly, `t == 1` returns @a b exactly.
double
axl_lerp(
    double  a,
    double  b,
    double  t
    );

/// Cubic ease-in: `t³`.  Slow start, accelerates to full speed.
///
/// `f(0) = 0`, `f(1) = 1`, `f'(0) = 0` (zero initial velocity).
/// CSS `cubic-bezier(0.55, 0.055, 0.675, 0.19)` approximation.
double
axl_ease_in_cubic(
    double  t
    );

/// Cubic ease-out: `1 - (1-t)³`.  Fast start, decelerates.
///
/// `f(0) = 0`, `f(1) = 1`, `f'(1) = 0` (zero ending velocity).
double
axl_ease_out_cubic(
    double  t
    );

/// Cubic ease-in-out: `t < 0.5 ? 4t³ : 1 - (-2t+2)³/2`.
///
/// Slow at both ends, full speed at midpoint.  Most-used default
/// for UI panel slide / fade transitions.
double
axl_ease_in_out_cubic(
    double  t
    );

/// Quintic ease-in: `t⁵`.  More aggressive slow-start than cubic.
double
axl_ease_in_quint(
    double  t
    );

/// Quintic ease-out: `1 - (1-t)⁵`.  Dramatic deceleration —
/// item snapping into place from a flick.
double
axl_ease_out_quint(
    double  t
    );

/// Quintic ease-in-out: `t < 0.5 ? 16t⁵ : 1 - (-2t+2)⁵/2`.
double
axl_ease_in_out_quint(
    double  t
    );

/// Sinusoidal ease-in: `1 - cos(t * π/2)`.  Subtle slow-start.
///
/// Clamped to `[0, 1]` at the endpoints — does NOT extrapolate
/// for `t < 0` or `t > 1` (unlike the cubic/quint variants).
/// The short-circuit guarantees exact `f(0) == 0` and
/// `f(1) == 1` so animation keyframes don't drift.
double
axl_ease_in_sine(
    double  t
    );

/// Sinusoidal ease-out: `sin(t * π/2)`.  Subtle slow-finish.
///
/// Clamped to `[0, 1]`; see `axl_ease_in_sine` for rationale.
double
axl_ease_out_sine(
    double  t
    );

/// Sinusoidal ease-in-out: `(1 - cos(t * π)) / 2`.
///
/// Symmetric S-curve; less aggressive midpoint slope than the
/// cubic/quint variants.  Clamped to `[0, 1]`.
double
axl_ease_in_out_sine(
    double  t
    );

// ===================================================================
// Bit math
// ===================================================================

/// Count leading zero bits in @a x.
///
/// Returns 64 for input 0 (`__builtin_clzll(0)` is undefined; we
/// substitute a defined value).  `axl_clz(1) == 63`,
/// `axl_clz(1ULL << 63) == 0`.
///
/// @return Number of leading zero bits, in `[0, 64]`.
int
axl_clz(
    uint64_t  x
    );

/// Count trailing zero bits in @a x.
///
/// Returns 64 for input 0 (same UB-substitution as `axl_clz`).
/// `axl_ctz(1) == 0`, `axl_ctz(1ULL << 63) == 63`,
/// `axl_ctz(6) == 1`.
///
/// @return Number of trailing zero bits, in `[0, 64]`.
int
axl_ctz(
    uint64_t  x
    );

/// Count set bits (population count) in @a x.
///
/// `axl_popcount(0) == 0`, `axl_popcount(7) == 3`,
/// `axl_popcount(~0ULL) == 64`.
///
/// @return Number of set bits, in `[0, 64]`.
int
axl_popcount(
    uint64_t  x
    );

/// Integer floor-log₂: largest @c i such that `2^i ≤ x`.
///
/// `axl_log2i(1) == 0`, `axl_log2i(2) == 1`, `axl_log2i(255) == 7`,
/// `axl_log2i(256) == 8`.
///
/// @warning `axl_log2i(0)` returns 0 (safe-default; mathematically
///          log₂(0) is -∞).
///
/// @return `floor(log2(x))` for `x > 0`, else `0`.
int
axl_log2i(
    uint64_t  x
    );

/// Round @a x up to the nearest power of two.
///
/// `axl_round_up_pow2(0) == 1`, `axl_round_up_pow2(1) == 1`,
/// `axl_round_up_pow2(5) == 8`, `axl_round_up_pow2(256) == 256`
/// (powers of two are fixed points).
///
/// @warning Inputs greater than `1ULL << 63` would overflow;
///          returns 0 as the safe-default in that case.
///
/// @return The smallest power of two `≥ x`, or 0 on overflow.
uint64_t
axl_round_up_pow2(
    uint64_t  x
    );

// ===================================================================
// Saturated arithmetic
// ===================================================================
//
// Result saturates at the type's max instead of wrapping.  Useful
// for color-channel math (alpha blending, gain) where the consumer
// wants 0xFF rather than `uint8_t` wraparound on overflow.

/// Saturating add: `min(a + b, 0xFF)`.
///
/// `axl_sat_add_u8(200, 100) == 255` (instead of wrapping to 44).
uint8_t
axl_sat_add_u8(
    uint8_t  a,
    uint8_t  b
    );

/// Saturating subtract: `max(a - b, 0)`.
///
/// `axl_sat_sub_u8(50, 100) == 0` (instead of wrapping to 206).
uint8_t
axl_sat_sub_u8(
    uint8_t  a,
    uint8_t  b
    );

/// Saturating multiply: `min(a * b, 0xFFFF)`.
///
/// `axl_sat_mul_u16(1000, 1000) == 0xFFFF` (instead of wrapping).
uint16_t
axl_sat_mul_u16(
    uint16_t  a,
    uint16_t  b
    );

// ===================================================================
// Linear algebra — 2D vectors and 3x3 matrices
// ===================================================================

/// 2D vector / point — double-precision components.
///
/// Used for both directional vectors and positional points; math
/// operations don't distinguish between the two.  Doubles match
/// the rest of AxlMath; gfx consumers that store coordinates as
/// `float` convert at the call boundary.
typedef struct AxlVec2 {
    double  x;
    double  y;
} AxlVec2;

/// 2D transform — 3×3 homography over `[x y 1]ᵀ` column vectors,
/// row-major.  The single transform type for the library (the gfx CTM,
/// the matrix toolkits hand to the transform-aware primitives, etc.).
///
/// Stored as a flat 9-element array indexed:
/// @code
///   [ m[0] m[1] m[2] ]
///   [ m[3] m[4] m[5] ]
///   [ m[6] m[7] m[8] ]
/// @endcode
/// An affine transform has bottom row `[0 0 1]` and decodes as
/// `[a b tx; c d ty; 0 0 1]` (scale/rotate in the 2×2 sub-matrix,
/// translation in the last column); a non-trivial bottom row encodes
/// perspective.
typedef struct AxlTransform {
    double  m[9];
} AxlTransform;

/// Classification of a transform, derived from its contents (never
/// stored / consumer-set).  Ordered by generality: each kind is a
/// special case of the ones below it, and `axl_transform_classify`
/// returns the most specific kind that fits.  Renderers and the
/// mapping routines key fast paths off this (skip the perspective
/// divide when not `PROJECTIVE`, use an axis-aligned blit when
/// `SCALE`, etc.).
typedef enum AxlTransformClass {
    AXL_TRANSFORM_IDENTITY = 0,  ///< exactly the identity
    AXL_TRANSFORM_TRANSLATE,     ///< identity linear part + translation
    AXL_TRANSFORM_SCALE,         ///< axis-aligned scale (diagonal linear) + translation
    AXL_TRANSFORM_AFFINE,        ///< general affine (rotation / shear); bottom row [0 0 1]
    AXL_TRANSFORM_PROJECTIVE,    ///< non-trivial bottom row (perspective)
} AxlTransformClass;

/// Axis-aligned rectangle, defined by its top-left corner and
/// width/height.  Negative `w` or `h` are treated as empty rects
/// by all the helpers below — the canonical form normalizes to
/// non-negative extents.
///
/// Edge semantics are HALF-OPEN: top/left included, bottom/right
/// excluded (so adjacent rects don't both claim a shared edge).
/// This is intentionally asymmetric with `AxlCircle`, which uses
/// CLOSED intersection — rects are a tiling primitive, circles
/// aren't, and each convention matches its respective domain.
typedef struct AxlRect {
    double  x;
    double  y;
    double  w;
    double  h;
} AxlRect;

/// Construct a 2D vector / point from components.
AxlVec2
axl_vec2(
    double  x,
    double  y
    );

/// Component-wise addition: `(ax+bx, ay+by)`.
AxlVec2
axl_vec2_add(
    AxlVec2  a,
    AxlVec2  b
    );

/// Component-wise subtraction: `(ax-bx, ay-by)`.
AxlVec2
axl_vec2_sub(
    AxlVec2  a,
    AxlVec2  b
    );

/// Scalar multiplication: `(v.x * k, v.y * k)`.
AxlVec2
axl_vec2_scale(
    AxlVec2  v,
    double   k
    );

/// Dot product: `a.x*b.x + a.y*b.y`.
///
/// Equals `|a| * |b| * cos(θ)` where θ is the angle between them.
/// Negative iff the angle is obtuse, zero iff perpendicular.
double
axl_vec2_dot(
    AxlVec2  a,
    AxlVec2  b
    );

/// Euclidean length: `√(v.x² + v.y²)`.  Uses `axl_sqrt`.
double
axl_vec2_length(
    AxlVec2  v
    );

/// Normalize @a v to unit length.
///
/// `axl_vec2_normalize((3, 4))` returns `(0.6, 0.8)`.
///
/// @warning Zero-length input returns `(0, 0)` — same safe-default
///          convention as `axl_sqrt` (negative → 0).  Catch the
///          length-zero bug at the call site by checking the input.
AxlVec2
axl_vec2_normalize(
    AxlVec2  v
    );

/// Linear interpolation: `a + (b - a) * t`.  `t = 0` → @a a, `t = 1` →
/// @a b.  `t` outside `[0, 1]` extrapolates (no clamp).
AxlVec2
axl_vec2_lerp(
    AxlVec2  a,
    AxlVec2  b,
    double   t
    );

/// Euclidean distance between @a a and @a b — `|a - b|`.
double
axl_vec2_distance(
    AxlVec2  a,
    AxlVec2  b
    );

/// Left perpendicular: `(-v.y, v.x)` — @a v turned 90° (toward +y from
/// +x).  `axl_vec2_perp((1, 0)) == (0, 1)`.  Negate for the right
/// perpendicular.
AxlVec2
axl_vec2_perp(
    AxlVec2  v
    );

/// 2D cross product (scalar z-component): `a.x*b.y - a.y*b.x`.
///
/// Equals `|a| * |b| * sin(θ)`: positive iff @a b is counter-clockwise
/// from @a a, zero iff parallel.  The sign is the standard orientation
/// test (twice the signed area of the triangle `0, a, b`).
double
axl_vec2_cross(
    AxlVec2  a,
    AxlVec2  b
    );

/// Rotate @a v by @a radians about the origin (column-vector
/// `[c -s; s c]`, same convention as `axl_transform_rotate`).
AxlVec2
axl_vec2_rotate(
    AxlVec2  v,
    double   radians
    );

/// Angle of @a v from the +x axis, in radians `(-π, π]` — `atan2(v.y,
/// v.x)`.  `axl_vec2_angle((0, 1)) == π/2`.  `(0, 0)` returns 0.
double
axl_vec2_angle(
    AxlVec2  v
    );

/// Reflect @a v across the line through the origin with unit normal
/// @a n: `v - 2*(v·n)*n`.  @a n MUST be unit length (normalize first);
/// a non-unit normal scales the result.
AxlVec2
axl_vec2_reflect(
    AxlVec2  v,
    AxlVec2  n
    );

/// Vector projection of @a a onto @a b: `(a·b / b·b) * b` — the
/// component of @a a parallel to @a b.  Zero-length @a b returns
/// `(0, 0)` (safe default, matching `axl_vec2_normalize`).
AxlVec2
axl_vec2_project(
    AxlVec2  a,
    AxlVec2  b
    );

/// Identity matrix — leaves any transformed point unchanged.
///
/// `[1 0 0; 0 1 0; 0 0 1]`.
AxlTransform
axl_transform_identity(
    void
    );

/// Translation matrix.
///
/// `axl_transform_map_point(axl_transform_translate(3, 4), p)` equals
/// `(p.x + 3, p.y + 4)`.
AxlTransform
axl_transform_translate(
    double  tx,
    double  ty
    );

/// Non-uniform scale matrix.  `sx == sy == 1` is the identity scale.
AxlTransform
axl_transform_scale(
    double  sx,
    double  sy
    );

/// Rotation matrix.  Angle in radians.
///
/// Always computes `[c -s; s c]` (the standard column-vector
/// rotation matrix) — `axl_transform_rotate(AXL_MATH_HALF_PI)` applied
/// to `(1, 0)` returns `(0, 1)` regardless of any downstream
/// y-axis convention.  Whether that *visually* reads as CCW or
/// CW depends on whether the framebuffer is y-up (math/SVG
/// convention) or y-down (axl-gfx and stb-style rasterizers).
AxlTransform
axl_transform_rotate(
    double  radians
    );

/// Skew matrix.
///
/// `sx` shears in the x direction proportional to y; `sy` shears
/// in y proportional to x.  Both arguments are the tangent of the
/// shear angle (matches the CSS `skew()` convention: pass
/// `axl_sin(angle)/axl_cos(angle)` for the angle form).
///
/// `axl_transform_map_point(axl_transform_shear(0.5, 0), (0, 1))`
/// returns `(0.5, 1)` — y stays the same, x shifts by `0.5 * y`.
AxlTransform
axl_transform_shear(
    double  sx,
    double  sy
    );

/// Compose two transforms: the result applies @a a first, then @a b.
///
/// cairo `cairo_matrix_multiply` operand order — for column-vector
/// points it is the matrix product `b · a`.  So:
/// @code
///   M = axl_transform_multiply(axl_transform_rotate(AXL_MATH_HALF_PI),
///                              axl_transform_translate(10, 0));
///   // M rotates first, THEN translates by (10, 0)
/// @endcode
/// NOTE: the operand order is cairo a-first as of v0.22.0 (pre-release);
/// the predecessor `axl_mat3_mul(a, b)` applied `b` first.
AxlTransform
axl_transform_multiply(
    AxlTransform  a,
    AxlTransform  b
    );

/// Apply matrix @a m to point @a p, with perspective divide.
///
/// Treats `p` as the column vector `[p.x p.y 1]ᵀ`, computes
/// `[x' y' w]ᵀ = m·p`, and returns `(x'/w, y'/w)`.  For an affine
/// matrix the bottom row is `[0 0 1]` so `w` is exactly 1 and the
/// divide is a no-op (results are bit-exact); a non-trivial bottom row
/// (perspective) makes the divide meaningful.
///
/// `axl_transform_map_point(axl_transform_translate(3, 4), (1, 1))`
/// returns `(4, 5)`.
AxlVec2
axl_transform_map_point(
    AxlTransform  m,
    AxlVec2  p
    );

/// Apply only the linear part of @a m to @a v — the upper-left 2×2,
/// ignoring translation.  Use for directions / deltas / sizes (a drag
/// vector, a surface normal) that should rotate and scale but not
/// shift.  `(m0*x + m1*y, m3*x + m4*y)`.
AxlVec2
axl_transform_map_vector(
    AxlTransform  m,
    AxlVec2  v
    );

/// Determinant of @a m (full 3×3).  For an affine matrix
/// (`[0 0 1]` bottom row) this reduces to `m0*m4 - m1*m3`, the signed
/// area scale of the linear part; zero iff @a m is singular
/// (non-invertible / collapses to a line or point).
double
axl_transform_determinant(
    AxlTransform  m
    );

/// Invert @a m.  Writes the inverse to @a out and returns true; returns
/// false (leaving @a out unmodified) if @a m is singular (≈ zero
/// determinant).
///
/// The inverse maps results back to inputs — e.g. converting a screen
/// point to local coordinates by inverting the transform it was drawn
/// with (hit-testing).  Computed by the adjugate / determinant for the
/// general 3×3 case.
bool
axl_transform_invert(
    AxlTransform   m,    ///< [in] matrix to invert
    AxlTransform  *out   ///< [out] receives the inverse (untouched if singular)
    );

/// A perspective transform with bottom row `[px py 1]`.
///
/// Maps `(x, y)` to `(x, y) / (px*x + py*y + 1)` — the points where the
/// denominator stays positive are foreshortened toward the origin as
/// `px*x + py*y` grows.  `axl_transform_perspective(0, 0)` is the
/// identity.  Compose with the affine builders via
/// `axl_transform_multiply` to build a general projective map, or use
/// `axl_transform_quad_to_quad` to derive one from corner
/// correspondences.
AxlTransform
axl_transform_perspective(
    double  px,   ///< x-weight of the perspective denominator (m[6])
    double  py    ///< y-weight of the perspective denominator (m[7])
    );

/// Build the transform mapping one quad onto another (4 point
/// correspondences) — the general projective map, exact for any
/// non-degenerate simple quad (convex or concave; no general solver,
/// closed form via the unit square).
///
/// Corners are matched by index in the order **top-left, top-right,
/// bottom-right, bottom-left** (consistent winding for both quads):
/// `map_point(result, src[i]) == dst[i]` for each `i`.  Use it to warp
/// a source rectangle onto an arbitrary on-screen quadrilateral.
///
/// @return true on success; false (leaving @a out untouched) if @a src
///         is degenerate (collinear / zero-area — not invertible).
bool
axl_transform_quad_to_quad(
    const AxlVec2  src[4],   ///< [in] source corners (TL, TR, BR, BL)
    const AxlVec2  dst[4],   ///< [in] destination corners (same order)
    AxlTransform  *out       ///< [out] receives src→dst transform
    );

/// Map the four corners of axis-aligned rect @a r through @a m and
/// return the **axis-aligned bounding box** of the result.
///
/// Exact when @a m is axis-aligned (`axl_transform_is_axis_aligned`);
/// otherwise it is the tight AABB enclosing the (rotated / sheared /
/// projected) image — a conservative cover, the usual input to a clip
/// or dirty-region test.  A normalized rect (non-negative w/h) is
/// returned.
///
/// Defined when @a r does not cross @a m's horizon — i.e. all four
/// corners map with the same sign of `w` (always true for affine maps
/// and for the rect→on-screen-quad warps `axl_transform_quad_to_quad`
/// produces).  A projective @a m whose horizon line passes through
/// @a r has no finite enclosing box, and the returned rect is
/// meaningless; clip @a r to the front of the horizon first.
AxlRect
axl_transform_map_rect(
    AxlTransform  m,
    AxlRect       r
    );

/// Map four points through @a m (full perspective divide each), writing
/// the images to @a out.  @a in and @a out may alias.  For clip-region
/// and quad-corner work where the bounding box of `map_rect` is too
/// loose.
void
axl_transform_map_quad(
    AxlTransform   m,
    const AxlVec2  in[4],    ///< [in] four source points
    AxlVec2        out[4]    ///< [out] four mapped points
    );

/// Classify @a m by its contents (see `AxlTransformClass`).  Pure
/// function of the matrix; uses a small tolerance so composed
/// transforms classify as expected despite floating-point drift.
AxlTransformClass
axl_transform_classify(
    AxlTransform  m
    );

/// True iff @a m is (within tolerance) the identity.
bool
axl_transform_is_identity(
    AxlTransform  m
    );

/// True iff @a m maps every axis-aligned rectangle to an axis-aligned
/// rectangle — i.e. non-perspective with a diagonal **or** anti-diagonal
/// linear part (axis scales, 90° rotations, flips).  The condition under
/// which `axl_transform_map_rect` is exact.
bool
axl_transform_is_axis_aligned(
    AxlTransform  m
    );

/// True iff @a m is affine (bottom row `[0 0 1]`, no perspective) —
/// equivalently `axl_transform_classify(m) != AXL_TRANSFORM_PROJECTIVE`.
bool
axl_transform_is_affine(
    AxlTransform  m
    );

// ===================================================================
// Geometry helpers — rect, segment, circle
// ===================================================================

/// Circle, defined by center and radius.  Negative radius is
/// treated as a degenerate (always-false-intersect) circle.
///
/// Intersection semantics are CLOSED — a tangent contact counts
/// as an intersection.  See `AxlRect` for the rationale on why
/// rects and circles use different boundary conventions.
typedef struct AxlCircle {
    AxlVec2  center;
    double   radius;
} AxlCircle;

/// Half-open hit test: point is "inside" `r` iff `r.x ≤ p.x <
/// r.x + r.w` and the same for y.  Right and bottom edges are
/// EXCLUDED (the standard convention so adjacent rects don't
/// both claim a shared edge).
bool
axl_point_in_rect(
    AxlVec2  p,
    AxlRect  r
    );

/// Intersection of two rects.  Returns the empty rect
/// `{0, 0, 0, 0}` if they don't overlap.
AxlRect
axl_rect_intersect(
    AxlRect  a,
    AxlRect  b
    );

/// Smallest rect containing both inputs.  If either is empty
/// (`w <= 0` or `h <= 0`), returns the other unchanged.
AxlRect
axl_rect_union(
    AxlRect  a,
    AxlRect  b
    );

/// Segment-segment intersection test.  Returns `true` iff the
/// segments cross at a single point on both segments
/// (parameters `t, s ∈ [0, 1]`).  If `out` is non-NULL the
/// intersection coordinates are written there.
///
/// @warning Parallel segments — including collinear overlap that
///          shares infinitely many points — return `false`.  The
///          algorithm tests for a unique intersection only, not
///          for any-shared-point.  Document at the caller when
///          collinear-overlap matters.
bool
axl_segment_intersect(
    AxlVec2   a1,
    AxlVec2   a2,
    AxlVec2   b1,
    AxlVec2   b2,
    AxlVec2  *out  ///< optional, may be NULL
    );

/// Shortest distance from point @a p to the closed segment from
/// @a a to @a b.  Projects @a p onto the segment line, clamps
/// the parameter to `[0, 1]`, then measures Euclidean distance
/// from @a p to the clamped point.
///
/// Degenerate segment (`a == b`) returns `|p - a|`.
double
axl_distance_point_to_segment(
    AxlVec2  p,
    AxlVec2  a,
    AxlVec2  b
    );

/// Circle-circle overlap test.  Returns `true` iff the two
/// closed disks share at least one point — i.e., the distance
/// between centers is `≤ a.radius + b.radius`.
bool
axl_circle_circle_intersect(
    AxlCircle  a,
    AxlCircle  b
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_MATH_H */
