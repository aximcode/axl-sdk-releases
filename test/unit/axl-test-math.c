/** @file axl-test-math.c
    Unit tests for AxlMath — libm-free floor / ceil / fabs / sqrt /
    fmod / sin / cos.

    Tests pin known values within a small tolerance.  Tolerances are
    chosen generous enough that drift from minor algorithm tweaks
    (extra Taylor terms, better range reduction) doesn't break the
    suite, but tight enough that catastrophic regressions (returning
    0, wrong sign, completely wrong magnitude) get caught.
**/

#include "axl-test.h"

#include <axl/axl-math.h>

/* Tolerance helpers — return true if @a actual is within @a tol of
 * @a expected. */
static bool
near_(double actual, double expected, double tol)
{
    double diff = actual - expected;
    if (diff < 0) diff = -diff;
    return diff <= tol;
}

// ---------------------------------------------------------------------------
// Rounding
// ---------------------------------------------------------------------------

static void
test_floor_positive(void)
{
    test_check(axl_floor(3.7) == 3.0,
               "floor: 3.7 → 3");
    test_check(axl_floor(3.0) == 3.0,
               "floor: 3.0 → 3 (integer is fixed point)");
    test_check(axl_floor(0.0) == 0.0,
               "floor: 0.0 → 0");
}

static void
test_floor_negative(void)
{
    test_check(axl_floor(-3.2) == -4.0,
               "floor: -3.2 → -4 (toward -inf, not zero)");
    test_check(axl_floor(-3.0) == -3.0,
               "floor: -3.0 → -3 (integer is fixed)");
    test_check(axl_floor(-0.5) == -1.0,
               "floor: -0.5 → -1");
}

static void
test_ceil_positive(void)
{
    test_check(axl_ceil(3.2) == 4.0,
               "ceil: 3.2 → 4");
    test_check(axl_ceil(3.0) == 3.0,
               "ceil: 3.0 → 3 (integer is fixed)");
    test_check(axl_ceil(0.5) == 1.0,
               "ceil: 0.5 → 1");
}

static void
test_ceil_negative(void)
{
    test_check(axl_ceil(-3.7) == -3.0,
               "ceil: -3.7 → -3 (toward +inf, not zero)");
    test_check(axl_ceil(-3.0) == -3.0,
               "ceil: -3.0 → -3");
}

static void
test_floori_and_ceili(void)
{
    test_check(axl_floori(3.7) == 3,
               "floori: 3.7 → 3");
    test_check(axl_floori(-3.2) == -4,
               "floori: -3.2 → -4");
    test_check(axl_ceili(3.2) == 4,
               "ceili: 3.2 → 4");
    test_check(axl_ceili(-3.7) == -3,
               "ceili: -3.7 → -3");
}

static void
test_fabs(void)
{
    test_check(axl_fabs(-5.5) == 5.5,
               "fabs: -5.5 → 5.5");
    test_check(axl_fabs(5.5) == 5.5,
               "fabs: 5.5 → 5.5");
    test_check(axl_fabs(0.0) == 0.0,
               "fabs: 0.0 → 0.0");
    test_check(axl_fabs(-0.0) == 0.0,
               "fabs: -0.0 → 0.0");
}

// ---------------------------------------------------------------------------
// sqrt / fmod
// ---------------------------------------------------------------------------

static void
test_sqrt_exact_squares(void)
{
    test_check(near_(axl_sqrt(4.0), 2.0, 1e-9),
               "sqrt: 4 → 2");
    test_check(near_(axl_sqrt(9.0), 3.0, 1e-9),
               "sqrt: 9 → 3");
    test_check(near_(axl_sqrt(64.0), 8.0, 1e-9),
               "sqrt: 64 → 8");
}

static void
test_sqrt_irrational(void)
{
    test_check(near_(axl_sqrt(2.0), 1.4142135623730951, 1e-9),
               "sqrt: 2 → √2");
    test_check(near_(axl_sqrt(3.0), 1.7320508075688772, 1e-9),
               "sqrt: 3 → √3");
}

static void
test_sqrt_edge_cases(void)
{
    test_check(axl_sqrt(0.0) == 0.0,
               "sqrt: 0 → 0");
    test_check(axl_sqrt(-1.0) == 0.0,
               "sqrt: negative input → 0 (clamped)");
    test_check(axl_sqrt(1.0) == 1.0,
               "sqrt: 1 → 1");
}

static void
test_fmod_basic(void)
{
    test_check(near_(axl_fmod(7.0, 3.0), 1.0, 1e-12),
               "fmod: 7 % 3 → 1");
    test_check(near_(axl_fmod(10.5, 2.0), 0.5, 1e-12),
               "fmod: 10.5 % 2 → 0.5");
}

static void
test_fmod_zero_divisor(void)
{
    /* Contract: fmod(x, 0) → 0 (safe-default, not undefined). */
    test_check(axl_fmod(7.0, 0.0) == 0.0,
               "fmod: divisor 0 → 0 (safe default)");
}

static void
test_fmod_sign(void)
{
    /* C99 fmod returns sign of dividend.  Pin it so consumers
     * porting from libm get expected behavior. */
    test_check(near_(axl_fmod(-7.0, 3.0), -1.0, 1e-12),
               "fmod: -7 % 3 → -1 (sign of dividend)");
    test_check(near_(axl_fmod(7.0, -3.0), 1.0, 1e-12),
               "fmod: 7 % -3 → 1 (sign of dividend, not divisor)");
}

static void
test_wrap(void)
{
    /* Positive in-range: identity. */
    test_check(near_(axl_wrap(3.5, 10.0), 3.5, 1e-12),
               "wrap: 3.5 in [0, 10) → 3.5 (identity)");

    /* Positive over-range: wraps once. */
    test_check(near_(axl_wrap(12.5, 10.0), 2.5, 1e-12),
               "wrap: 12.5 in [0, 10) → 2.5");

    /* Negative wraps cleanly to positive (unlike fmod). */
    test_check(near_(axl_wrap(-1.5, 10.0), 8.5, 1e-12),
               "wrap: -1.5 in [0, 10) → 8.5");

    /* Negative over-range. */
    test_check(near_(axl_wrap(-12.5, 10.0), 7.5, 1e-12),
               "wrap: -12.5 in [0, 10) → 7.5");

    /* Exact zero is exact zero. */
    test_check(axl_wrap(0.0, 10.0) == 0.0,
               "wrap: 0 in [0, 10) → 0 (exact)");

    /* Right boundary excluded — half-open interval. */
    test_check(axl_wrap(10.0, 10.0) == 0.0,
               "wrap: 10 in [0, 10) → 0 (boundary excluded)");

    /* Zero divisor → safe default. */
    test_check(axl_wrap(5.0, 0.0) == 0.0,
               "wrap: divisor 0 → 0 (safe default)");

    /* Angle normalization: 7π/2 wraps to 3π/2 in [0, 2π). */
    test_check(near_(axl_wrap(7.0 * AXL_MATH_PI / 2.0, AXL_MATH_TWO_PI),
                     3.0 * AXL_MATH_PI / 2.0, 1e-12),
               "wrap: 7π/2 in [0, 2π) → 3π/2 (angle norm)");
}

// ---------------------------------------------------------------------------
// sin / cos
// ---------------------------------------------------------------------------

static void
test_sin_known_values(void)
{
    /* Header documents ~1e-7 accuracy; use 1e-6 tolerance — 10x
     * margin allows for future algorithm tweaks without breaking
     * tests, but catches catastrophic regressions. */
    test_check(near_(axl_sin(0.0), 0.0, 1e-9),
               "sin: 0 → 0 (exact)");
    test_check(near_(axl_sin(AXL_MATH_HALF_PI), 1.0, 1e-6),
               "sin: π/2 → 1");
    test_check(near_(axl_sin(AXL_MATH_PI), 0.0, 1e-6),
               "sin: π → 0");
    test_check(near_(axl_sin(-AXL_MATH_HALF_PI), -1.0, 1e-6),
               "sin: -π/2 → -1");
}

static void
test_sin_periodicity(void)
{
    /* sin should repeat every 2π. */
    test_check(near_(axl_sin(AXL_MATH_TWO_PI), 0.0, 1e-6),
               "sin: 2π → 0 (periodic)");
    test_check(near_(axl_sin(AXL_MATH_TWO_PI + AXL_MATH_HALF_PI), 1.0, 1e-6),
               "sin: 5π/2 → 1 (periodic)");
}

static void
test_sin_large_magnitude(void)
{
    /* Constant-time range reduction must handle inputs far from 0
     * without hanging.  Reference values cross-checked against
     * known double-precision sin output. */
    test_check(near_(axl_sin(1.0e6), -0.34999350217, 1e-3),
               "sin(1e6): handles huge positive input");
    test_check(near_(axl_sin(-1.0e6), 0.34999350217, 1e-3),
               "sin(-1e6): handles huge negative input");
}

static void
test_sin_quadrants(void)
{
    /* sin(3π/4) = sin(π/4) ≈ 0.707. */
    test_check(near_(axl_sin(3.0 * AXL_MATH_PI / 4.0), 0.7071, 1e-3),
               "sin: 3π/4 → ≈0.707");
    /* sin(-3π/4) = -sin(π/4) ≈ -0.707. */
    test_check(near_(axl_sin(-3.0 * AXL_MATH_PI / 4.0), -0.7071, 1e-3),
               "sin: -3π/4 → ≈-0.707");
}

static void
test_cos_known_values(void)
{
    test_check(near_(axl_cos(0.0), 1.0, 1e-6),
               "cos: 0 → 1");
    test_check(near_(axl_cos(AXL_MATH_HALF_PI), 0.0, 1e-6),
               "cos: π/2 → 0");
    test_check(near_(axl_cos(AXL_MATH_PI), -1.0, 1e-6),
               "cos: π → -1");
}

static void
test_cos_identity_with_sin(void)
{
    /* sin² + cos² == 1 — fundamental identity, robust against
     * implementation drift. */
    double x = 1.0;  /* arbitrary */
    double s = axl_sin(x);
    double c = axl_cos(x);
    double sum = s * s + c * c;
    test_check(near_(sum, 1.0, 1e-6),
               "trig: sin²(x) + cos²(x) → 1 (Pythagorean identity)");
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static void
test_constants(void)
{
    /* Each constant equals its expected literal within 1e-15. */
    test_check(near_(AXL_MATH_E, 2.718281828459045, 1e-15),
               "const: AXL_MATH_E");
    test_check(near_(AXL_MATH_SQRT_2, 1.4142135623730951, 1e-15),
               "const: AXL_MATH_SQRT_2");
    test_check(near_(AXL_MATH_LOG_2, 0.6931471805599453, 1e-15),
               "const: AXL_MATH_LOG_2");
    test_check(near_(AXL_MATH_GOLDEN, 1.618033988749895, 1e-15),
               "const: AXL_MATH_GOLDEN");
    test_check(near_(AXL_MATH_DEG_TO_RAD, 0.017453292519943295, 1e-15),
               "const: AXL_MATH_DEG_TO_RAD");
    test_check(near_(AXL_MATH_RAD_TO_DEG, 57.29577951308232, 1e-15),
               "const: AXL_MATH_RAD_TO_DEG");

    /* Inverse pair: deg→rad and rad→deg are reciprocals. */
    test_check(near_(AXL_MATH_DEG_TO_RAD * AXL_MATH_RAD_TO_DEG, 1.0, 1e-15),
               "const: AXL_MATH_DEG_TO_RAD * AXL_MATH_RAD_TO_DEG → 1");

    /* Sanity: 90° in radians is π/2. */
    test_check(near_(90.0 * AXL_MATH_DEG_TO_RAD, AXL_MATH_HALF_PI, 1e-15),
               "const: 90° * AXL_MATH_DEG_TO_RAD → π/2");

    /* Defining identity for the golden ratio: φ² = φ + 1. */
    test_check(near_(AXL_MATH_GOLDEN * AXL_MATH_GOLDEN, AXL_MATH_GOLDEN + 1.0, 1e-14),
               "const: φ² → φ + 1");

    /* √2 squared is 2. */
    test_check(near_(AXL_MATH_SQRT_2 * AXL_MATH_SQRT_2, 2.0, 1e-15),
               "const: √2 * √2 → 2");
}

// ---------------------------------------------------------------------------
// ln / exp / pow — Phase M2
// ---------------------------------------------------------------------------

static void
test_ln_known_values(void)
{
    /* ln(1) == 0 exactly — s = (1-1)/(1+1) = 0, series sum is 0,
     * and e (= 0) * ln(2) is 0.  No accumulated rounding. */
    test_check(axl_ln(1.0) == 0.0,
               "ln: 1 → 0 (exact)");
    test_check(near_(axl_ln(AXL_MATH_E), 1.0, 1e-9),
               "ln: e → 1");
    test_check(near_(axl_ln(2.0), AXL_MATH_LOG_2, 1e-9),
               "ln: 2 → AXL_MATH_LOG_2");
    test_check(near_(axl_ln(AXL_MATH_E * AXL_MATH_E), 2.0, 1e-9),
               "ln: e² → 2");
    test_check(near_(axl_ln(0.5), -AXL_MATH_LOG_2, 1e-9),
               "ln: 0.5 → -ln(2)");
    /* ln of a power of 2: ln(8) == 3 * ln(2). */
    test_check(near_(axl_ln(8.0), 3.0 * AXL_MATH_LOG_2, 1e-9),
               "ln: 8 → 3 * ln(2)");
}

static void
test_ln_edge_cases(void)
{
    test_check(axl_ln(0.0) == 0.0,
               "ln: 0 → 0 (safe default)");
    test_check(axl_ln(-1.0) == 0.0,
               "ln: negative → 0 (safe default)");
}

static void
test_ln_large_and_small(void)
{
    /* Range reduction must scale: ln(1e100) = 100 * ln(10). */
    test_check(near_(axl_ln(1.0e100), 100.0 * 2.302585092994046, 1e-9),
               "ln: 1e100 → 100 * ln(10)");
    test_check(near_(axl_ln(1.0e-100), -100.0 * 2.302585092994046, 1e-9),
               "ln: 1e-100 → -100 * ln(10)");
}

static void
test_exp_known_values(void)
{
    test_check(axl_exp(0.0) == 1.0,
               "exp: 0 → 1 (exact)");
    test_check(near_(axl_exp(1.0), AXL_MATH_E, 1e-9),
               "exp: 1 → e");
    test_check(near_(axl_exp(AXL_MATH_LOG_2), 2.0, 1e-9),
               "exp: ln(2) → 2");
    test_check(near_(axl_exp(-1.0), 1.0 / AXL_MATH_E, 1e-9),
               "exp: -1 → 1/e");
    /* exp(10) ≈ 22026.465794806718. */
    test_check(near_(axl_exp(10.0), 22026.4657948, 1e-3),
               "exp: 10 → ~22026");
}

static void
test_exp_identity_with_ln(void)
{
    /* Round-trip: exp(ln(x)) == x and ln(exp(x)) == x. */
    test_check(near_(axl_exp(axl_ln(3.14159)), 3.14159, 1e-9),
               "exp(ln(π)) → π");
    test_check(near_(axl_exp(axl_ln(1000.0)), 1000.0, 1e-6),
               "exp(ln(1000)) → 1000");
    test_check(near_(axl_ln(axl_exp(1.5)), 1.5, 1e-9),
               "ln(exp(1.5)) → 1.5");
}

static void
test_exp_extremes(void)
{
    /* Huge positive input saturates at exactly DBL_MAX — pinned so a
     * future regression returning e.g. 1e301 still gets caught. */
    test_check(axl_exp(1000.0) == 1.7976931348623157e308,
               "exp: 1000 → DBL_MAX (saturated)");
    /* Huge negative input underflows cleanly to 0. */
    test_check(axl_exp(-1000.0) == 0.0,
               "exp: -1000 → 0 (underflow)");
}

static void
test_pow_known_values(void)
{
    /* pow(x, 0) == 1 for any x (including 0, per IEEE 754). */
    test_check(axl_pow(5.0, 0.0) == 1.0,
               "pow: 5^0 → 1");
    test_check(axl_pow(0.0, 0.0) == 1.0,
               "pow: 0^0 → 1 (IEEE 754)");
    /* pow(1, y) == 1 for any y. */
    test_check(axl_pow(1.0, 5.0) == 1.0,
               "pow: 1^5 → 1");
    test_check(axl_pow(1.0, -3.0) == 1.0,
               "pow: 1^-3 → 1");
    test_check(near_(axl_pow(2.0, 10.0), 1024.0, 1e-6),
               "pow: 2^10 → 1024");
    test_check(near_(axl_pow(2.0, 0.5), AXL_MATH_SQRT_2, 1e-9),
               "pow: 2^0.5 → √2");
    test_check(near_(axl_pow(4.0, -1.0), 0.25, 1e-9),
               "pow: 4^-1 → 0.25");
    /* sRGB gamma shape: 0.5^2.2 ≈ 0.21763764. */
    test_check(near_(axl_pow(0.5, 2.2), 0.21763764, 1e-6),
               "pow: 0.5^2.2 → ~0.218 (sRGB γ shape)");
}

static void
test_pow_edge_cases(void)
{
    /* Contract: negative base → 0 (safe-default). */
    test_check(axl_pow(-2.0, 3.0) == 0.0,
               "pow: negative base → 0 (safe default)");
    /* pow(0, positive) == 0. */
    test_check(axl_pow(0.0, 5.0) == 0.0,
               "pow: 0^5 → 0");
    /* pow(0, negative) — libm would give +Inf; we return 0 since
     * the `base == 0` fast-path catches it regardless of exponent
     * sign.  Pin the contract so future "smarter" handling doesn't
     * silently change shape. */
    test_check(axl_pow(0.0, -1.0) == 0.0,
               "pow: 0^-1 → 0 (safe default, not +Inf)");
}

static void
test_ln_non_normal(void)
{
    /* Subnormal input (between 0 and DBL_MIN) — safe-default 0
     * rather than the mathematically-correct large-negative value.
     * Documented as "normal-double input range" in the header. */
    test_check(axl_ln(1e-310) == 0.0,
               "ln: subnormal → 0 (safe default)");
    /* +Inf — same safe-default convention.  Without this guard the
     * implementation would compute the IEEE-encoded exponent and
     * return ≈ 709.78 instead of +Inf. */
    double inf = 1e308 * 10.0;  /* construct +Inf without <math.h> */
    test_check(axl_ln(inf) == 0.0,
               "ln: +Inf → 0 (safe default)");
}

// ---------------------------------------------------------------------------
// Range / interpolation helpers — Phase M5
// ---------------------------------------------------------------------------

static void
test_clamp_inside(void)
{
    test_check(axl_clamp(5.0, 0.0, 10.0) == 5.0,
               "clamp: 5 in [0,10] → 5 (inside, unchanged)");
    test_check(axl_clamp(0.0, 0.0, 10.0) == 0.0,
               "clamp: x == lo → lo");
    test_check(axl_clamp(10.0, 0.0, 10.0) == 10.0,
               "clamp: x == hi → hi");
}

static void
test_clamp_outside(void)
{
    test_check(axl_clamp(-1.0, 0.0, 10.0) == 0.0,
               "clamp: x < lo → lo");
    test_check(axl_clamp(11.0, 0.0, 10.0) == 10.0,
               "clamp: x > hi → hi");
    test_check(axl_clamp(-5.0, -3.0, -1.0) == -3.0,
               "clamp: negative range, x < lo → lo");
}

static void
test_clamp_degenerate(void)
{
    /* Header documents lo > hi as implementation-defined; current
     * implementation returns hi (upper clamp wins).  Pin it so
     * downstream consumers can rely on the behavior. */
    test_check(axl_clamp(5.0, 10.0, 0.0) == 0.0,
               "clamp: lo > hi → hi (implementation-defined)");
}

static void
test_min(void)
{
    test_check(axl_min(3.0, 5.0) == 3.0,
               "min: (3, 5) → 3");
    test_check(axl_min(5.0, 3.0) == 3.0,
               "min: (5, 3) → 3 (order doesn't matter)");
    test_check(axl_min(-2.0, -5.0) == -5.0,
               "min: (-2, -5) → -5 (negative)");
    test_check(axl_min(4.0, 4.0) == 4.0,
               "min: equal inputs → either");
}

static void
test_max(void)
{
    test_check(axl_max(3.0, 5.0) == 5.0,
               "max: (3, 5) → 5");
    test_check(axl_max(5.0, 3.0) == 5.0,
               "max: (5, 3) → 5 (order doesn't matter)");
    test_check(axl_max(-2.0, -5.0) == -2.0,
               "max: (-2, -5) → -2 (negative)");
    test_check(axl_max(4.0, 4.0) == 4.0,
               "max: equal inputs → either");
}

static void
test_remap_midpoint(void)
{
    test_check(near_(axl_remap(50.0, 0.0, 100.0, 0.0, 1.0), 0.5, 1e-12),
               "remap: 50 of [0,100] → 0.5 of [0,1]");
    test_check(near_(axl_remap(0.0, 0.0, 100.0, 0.0, 1.0), 0.0, 1e-12),
               "remap: in_min → out_min");
    test_check(near_(axl_remap(100.0, 0.0, 100.0, 0.0, 1.0), 1.0, 1e-12),
               "remap: in_max → out_max");
}

static void
test_remap_inverted_output(void)
{
    /* Inverted output range — perfectly valid; useful for flipping
     * Y-axis between screen and math coords. */
    test_check(near_(axl_remap(50.0, 0.0, 100.0, 1.0, 0.0), 0.5, 1e-12),
               "remap: midpoint with inverted output → 0.5 (midpoint)");
    test_check(near_(axl_remap(0.0, 0.0, 100.0, 1.0, 0.0), 1.0, 1e-12),
               "remap: in_min with inverted output → out_min (== 1)");
}

static void
test_remap_extrapolation(void)
{
    /* Header documents that values outside [in_min, in_max] are NOT
     * clamped — they extrapolate linearly. */
    test_check(near_(axl_remap(150.0, 0.0, 100.0, 0.0, 1.0), 1.5, 1e-12),
               "remap: 150 of [0,100] → 1.5 (extrapolate above)");
    test_check(near_(axl_remap(-50.0, 0.0, 100.0, 0.0, 1.0), -0.5, 1e-12),
               "remap: -50 of [0,100] → -0.5 (extrapolate below)");
}

static void
test_remap_degenerate_input(void)
{
    /* Contract: in_min == in_max → return out_min (avoid div-by-zero). */
    test_check(axl_remap(5.0, 3.0, 3.0, 100.0, 200.0) == 100.0,
               "remap: in_min == in_max → out_min (safe default)");
}

static void
test_step(void)
{
    test_check(axl_step(0.5, 0.3) == 0.0,
               "step: x < edge → 0");
    test_check(axl_step(0.5, 0.7) == 1.0,
               "step: x > edge → 1");
    /* GLSL spec: x == edge returns 1 (because the condition is x < edge). */
    test_check(axl_step(0.5, 0.5) == 1.0,
               "step: x == edge → 1 (GLSL spec)");
    test_check(axl_step(0.0, -0.1) == 0.0,
               "step: negative x below edge 0 → 0");
}

static void
test_smoothstep_boundary(void)
{
    test_check(axl_smoothstep(0.0, 1.0, 0.0) == 0.0,
               "smoothstep: x == edge0 → 0 (exact)");
    test_check(axl_smoothstep(0.0, 1.0, 1.0) == 1.0,
               "smoothstep: x == edge1 → 1 (exact)");
    test_check(axl_smoothstep(0.0, 1.0, -0.5) == 0.0,
               "smoothstep: x < edge0 → 0 (clamped)");
    test_check(axl_smoothstep(0.0, 1.0, 1.5) == 1.0,
               "smoothstep: x > edge1 → 1 (clamped)");
}

static void
test_smoothstep_midpoint(void)
{
    /* t = 0.5 → t*t*(3 - 2t) = 0.25 * 2 = 0.5 exactly. */
    test_check(axl_smoothstep(0.0, 1.0, 0.5) == 0.5,
               "smoothstep: midpoint of [0,1] → 0.5 exactly");
    test_check(axl_smoothstep(2.0, 4.0, 3.0) == 0.5,
               "smoothstep: midpoint of [2,4] → 0.5 exactly");
}

static void
test_smoothstep_shape(void)
{
    /* Smoothstep should be smoother than linear: at t=0.25 the
     * value is 0.25*0.25*(3 - 0.5) = 0.15625 (below linear 0.25). */
    test_check(near_(axl_smoothstep(0.0, 1.0, 0.25), 0.15625, 1e-12),
               "smoothstep: t=0.25 → 0.15625 (below linear)");
    /* At t=0.75: 0.75*0.75*(3 - 1.5) = 0.84375 (above linear). */
    test_check(near_(axl_smoothstep(0.0, 1.0, 0.75), 0.84375, 1e-12),
               "smoothstep: t=0.75 → 0.84375 (above linear)");
}

// ---------------------------------------------------------------------------
// Interpolation + easing — Phase M4
// ---------------------------------------------------------------------------

static void
test_lerp(void)
{
    /* Endpoints exact. */
    test_check(axl_lerp(10.0, 20.0, 0.0) == 10.0,
               "lerp: t=0 → a (exact)");
    test_check(axl_lerp(10.0, 20.0, 1.0) == 20.0,
               "lerp: t=1 → b (exact)");
    /* Midpoint. */
    test_check(near_(axl_lerp(0.0, 100.0, 0.5), 50.0, 1e-12),
               "lerp: midpoint");
    /* Extrapolation (NOT clamped — documented). */
    test_check(near_(axl_lerp(0.0, 100.0, 1.5), 150.0, 1e-12),
               "lerp: t > 1 extrapolates");
    test_check(near_(axl_lerp(0.0, 100.0, -0.5), -50.0, 1e-12),
               "lerp: t < 0 extrapolates");
    /* Negative direction (b < a). */
    test_check(near_(axl_lerp(20.0, 10.0, 0.25), 17.5, 1e-12),
               "lerp: descending range");
}

static void
test_ease_endpoints(void)
{
    /* All ease functions must satisfy f(0) == 0 and f(1) == 1
     * exactly (or within FP rounding for the sine variants). */
    test_check(axl_ease_in_cubic(0.0) == 0.0,
               "ease_in_cubic: f(0) → 0");
    test_check(axl_ease_in_cubic(1.0) == 1.0,
               "ease_in_cubic: f(1) → 1");
    test_check(axl_ease_out_cubic(0.0) == 0.0,
               "ease_out_cubic: f(0) → 0");
    test_check(axl_ease_out_cubic(1.0) == 1.0,
               "ease_out_cubic: f(1) → 1");
    test_check(axl_ease_in_out_cubic(0.0) == 0.0,
               "ease_in_out_cubic: f(0) → 0");
    test_check(axl_ease_in_out_cubic(1.0) == 1.0,
               "ease_in_out_cubic: f(1) → 1");
    test_check(axl_ease_in_quint(0.0) == 0.0,
               "ease_in_quint: f(0) → 0");
    test_check(axl_ease_in_quint(1.0) == 1.0,
               "ease_in_quint: f(1) → 1");
    test_check(axl_ease_out_quint(0.0) == 0.0,
               "ease_out_quint: f(0) → 0");
    test_check(axl_ease_out_quint(1.0) == 1.0,
               "ease_out_quint: f(1) → 1");
    test_check(axl_ease_in_out_quint(0.0) == 0.0,
               "ease_in_out_quint: f(0) → 0");
    test_check(axl_ease_in_out_quint(1.0) == 1.0,
               "ease_in_out_quint: f(1) → 1");
    /* Sine variants short-circuit at t ≤ 0 and t ≥ 1, so endpoints
     * are exact — same `==` contract as the polynomial variants. */
    test_check(axl_ease_in_sine(0.0) == 0.0,
               "ease_in_sine: f(0) → 0 (exact via short-circuit)");
    test_check(axl_ease_in_sine(1.0) == 1.0,
               "ease_in_sine: f(1) → 1 (exact via short-circuit)");
    test_check(axl_ease_out_sine(0.0) == 0.0,
               "ease_out_sine: f(0) → 0 (exact)");
    test_check(axl_ease_out_sine(1.0) == 1.0,
               "ease_out_sine: f(1) → 1 (exact)");
    test_check(axl_ease_in_out_sine(0.0) == 0.0,
               "ease_in_out_sine: f(0) → 0 (exact)");
    test_check(axl_ease_in_out_sine(1.0) == 1.0,
               "ease_in_out_sine: f(1) → 1 (exact)");
}

static void
test_ease_midpoints(void)
{
    /* in_cubic(0.5) = 0.125, out_cubic(0.5) = 0.875,
     * in_out_cubic(0.5) = 0.5 (symmetric S). */
    test_check(near_(axl_ease_in_cubic(0.5), 0.125, 1e-12),
               "ease_in_cubic: 0.5 → 0.125");
    test_check(near_(axl_ease_out_cubic(0.5), 0.875, 1e-12),
               "ease_out_cubic: 0.5 → 0.875");
    test_check(near_(axl_ease_in_out_cubic(0.5), 0.5, 1e-12),
               "ease_in_out_cubic: 0.5 → 0.5 (midpoint exact)");
    /* in_quint(0.5) = 0.03125, out_quint(0.5) = 0.96875,
     * in_out_quint(0.5) = 0.5. */
    test_check(near_(axl_ease_in_quint(0.5), 0.03125, 1e-12),
               "ease_in_quint: 0.5 → 0.03125");
    test_check(near_(axl_ease_out_quint(0.5), 0.96875, 1e-12),
               "ease_out_quint: 0.5 → 0.96875");
    test_check(near_(axl_ease_in_out_quint(0.5), 0.5, 1e-12),
               "ease_in_out_quint: 0.5 → 0.5");
    /* in_sine(0.5) = 1 - cos(π/4) = 1 - √2/2 ≈ 0.29289...
     * out_sine(0.5) = sin(π/4) = √2/2 ≈ 0.70710...
     * in_out_sine(0.5) = (1 - cos(π/2))/2 = 0.5 */
    test_check(near_(axl_ease_in_sine(0.5), 1.0 - AXL_MATH_SQRT_2 / 2.0, 1e-6),
               "ease_in_sine: 0.5 → 1 - √2/2");
    test_check(near_(axl_ease_out_sine(0.5), AXL_MATH_SQRT_2 / 2.0, 1e-6),
               "ease_out_sine: 0.5 → √2/2");
    test_check(near_(axl_ease_in_out_sine(0.5), 0.5, 1e-6),
               "ease_in_out_sine: 0.5 → 0.5 (midpoint exact)");
}

static void
test_ease_in_out_symmetry(void)
{
    /* in_out variants are point-symmetric about (0.5, 0.5):
     *   f(0.5 + d) + f(0.5 - d) == 1 for d ∈ [0, 0.5]. */
    double d_set[] = { 0.1, 0.25, 0.4 };
    for (int i = 0; i < 3; i++) {
        double d = d_set[i];
        test_check(near_(axl_ease_in_out_cubic(0.5 + d)
                         + axl_ease_in_out_cubic(0.5 - d),
                         1.0, 1e-12),
                   "ease_in_out_cubic: point-symmetric about (0.5, 0.5)");
        test_check(near_(axl_ease_in_out_quint(0.5 + d)
                         + axl_ease_in_out_quint(0.5 - d),
                         1.0, 1e-12),
                   "ease_in_out_quint: point-symmetric about (0.5, 0.5)");
        test_check(near_(axl_ease_in_out_sine(0.5 + d)
                         + axl_ease_in_out_sine(0.5 - d),
                         1.0, 1e-6),
                   "ease_in_out_sine: point-symmetric about (0.5, 0.5)");
    }
}

static void
test_ease_monotonic(void)
{
    /* All nine eases must be monotonically non-decreasing on
     * [0, 1].  A 10-step sweep per function would catch a sign
     * flip on any single formula — the kind of bug structural
     * symmetry tests would miss. */
    double (*funcs[])(double) = {
        axl_ease_in_cubic, axl_ease_out_cubic, axl_ease_in_out_cubic,
        axl_ease_in_quint, axl_ease_out_quint, axl_ease_in_out_quint,
        axl_ease_in_sine,  axl_ease_out_sine,  axl_ease_in_out_sine,
    };
    bool all_mono = true;
    for (int i = 0; i < 9; i++) {
        double prev = funcs[i](0.0);
        for (int j = 1; j <= 10; j++) {
            double t   = j / 10.0;
            double cur = funcs[i](t);
            if (cur < prev) {
                all_mono = false;
            }
            prev = cur;
        }
    }
    test_check(all_mono,
               "ease: all nine variants monotonic non-decreasing on [0,1]");
}

static void
test_ease_in_out_complementarity(void)
{
    /* in_X(t) + out_X(1-t) == 1 for the cubic and quint variants:
     *   in_cubic(t) = t³;  out_cubic(1-t) = 1 - t³ → sum = 1. */
    double t_set[] = { 0.1, 0.3, 0.7, 0.9 };
    for (int i = 0; i < 4; i++) {
        double t = t_set[i];
        test_check(near_(axl_ease_in_cubic(t) + axl_ease_out_cubic(1.0 - t),
                         1.0, 1e-12),
                   "ease: in_cubic(t) + out_cubic(1-t) == 1");
        test_check(near_(axl_ease_in_quint(t) + axl_ease_out_quint(1.0 - t),
                         1.0, 1e-12),
                   "ease: in_quint(t) + out_quint(1-t) == 1");
        test_check(near_(axl_ease_in_sine(t) + axl_ease_out_sine(1.0 - t),
                         1.0, 1e-6),
                   "ease: in_sine(t) + out_sine(1-t) == 1");
    }
}

// ---------------------------------------------------------------------------
// Inverse trigonometry — Phase M1
// ---------------------------------------------------------------------------

static void
test_atan_known_values(void)
{
    /* Documented ~1e-9 accuracy; use 1e-8 tolerance (10× margin). */
    test_check(axl_atan(0.0) == 0.0,
               "atan: 0 → 0 (exact)");
    test_check(near_(axl_atan(1.0), AXL_MATH_PI / 4.0, 1e-8),
               "atan: 1 → π/4");
    test_check(near_(axl_atan(-1.0), -AXL_MATH_PI / 4.0, 1e-8),
               "atan: -1 → -π/4 (odd function)");
    /* Canonical 30°/60° angles via √3. */
    double sqrt3 = axl_sqrt(3.0);
    test_check(near_(axl_atan(sqrt3), AXL_MATH_PI / 3.0, 1e-8),
               "atan: √3 → π/3");
    test_check(near_(axl_atan(1.0 / sqrt3), AXL_MATH_PI / 6.0, 1e-8),
               "atan: 1/√3 → π/6");
    /* Large input saturates near π/2. */
    test_check(near_(axl_atan(1.0e6), AXL_MATH_HALF_PI, 1e-5),
               "atan: 1e6 → ~π/2");
    test_check(near_(axl_atan(-1.0e6), -AXL_MATH_HALF_PI, 1e-5),
               "atan: -1e6 → ~-π/2");
}

static void
test_atan_reduction_boundaries(void)
{
    /* The 1/x reduction kicks in at |x| > 1; the addition-formula
     * reduction at |x| ≥ 0.5.  Probe right at and across each
     * boundary so a future reduction-direction sign bug shows up. */
    test_check(near_(axl_atan(0.5), 0.4636476090008061, 1e-9),
               "atan: 0.5 (boundary of addition-formula reduction)");
    test_check(near_(axl_atan(0.6), 0.5404195002705842, 1e-9),
               "atan: 0.6 (just above addition-formula boundary)");
    test_check(near_(axl_atan(2.0), 1.1071487177940904, 1e-9),
               "atan: 2.0 (Step A + Step B both fire)");
    test_check(near_(axl_atan(-2.0), -1.1071487177940904, 1e-9),
               "atan: -2.0 (sign + Step A + Step B)");
    /* x > 2 exercises Step A WITHOUT Step B (1/x < 0.5, so Step B
     * skipped after Step A). */
    test_check(near_(axl_atan(3.0), 1.2490457723982544, 1e-9),
               "atan: 3.0 (Step A only, Step B skipped)");
    test_check(near_(axl_atan(100.0), 1.5607966601082315, 1e-9),
               "atan: 100 (Step A only, Step B skipped)");
}

static void
test_atan_extreme(void)
{
    /* Construct ±Inf without <math.h>.  1/0 inside FP avoids the
     * trap flag because axl-sdk builds with -fno-trapping-math. */
    double inf = 1.0 / 0.0;
    test_check(near_(axl_atan(inf), AXL_MATH_HALF_PI, 1e-15),
               "atan: +Inf → π/2 (1/Inf=0 path lands at exact π/2)");
    test_check(near_(axl_atan(-inf), -AXL_MATH_HALF_PI, 1e-15),
               "atan: -Inf → -π/2");
    /* NaN propagates (matches axl_exp's NaN passthrough). */
    double nan = inf - inf;
    double r = axl_atan(nan);
    test_check(r != r,
               "atan: NaN → NaN (passthrough, not safe-default 0)");
}

static void
test_atan2_quadrants(void)
{
    /* All eight cardinal directions. */
    test_check(axl_atan2(0.0, 1.0) == 0.0,
               "atan2: (y=0, x=1) → 0 (east, exact)");
    test_check(near_(axl_atan2(1.0, 1.0), AXL_MATH_PI / 4.0, 1e-8),
               "atan2: (1, 1) → π/4 (NE)");
    test_check(near_(axl_atan2(1.0, 0.0), AXL_MATH_HALF_PI, 1e-15),
               "atan2: (1, 0) → π/2 (north, exact constant)");
    test_check(near_(axl_atan2(1.0, -1.0), 3.0 * AXL_MATH_PI / 4.0, 1e-8),
               "atan2: (1, -1) → 3π/4 (NW)");
    test_check(near_(axl_atan2(0.0, -1.0), AXL_MATH_PI, 1e-15),
               "atan2: (0, -1) → π (west)");
    test_check(near_(axl_atan2(-1.0, -1.0), -3.0 * AXL_MATH_PI / 4.0, 1e-8),
               "atan2: (-1, -1) → -3π/4 (SW)");
    test_check(near_(axl_atan2(-1.0, 0.0), -AXL_MATH_HALF_PI, 1e-15),
               "atan2: (-1, 0) → -π/2 (south)");
    test_check(near_(axl_atan2(-1.0, 1.0), -AXL_MATH_PI / 4.0, 1e-8),
               "atan2: (-1, 1) → -π/4 (SE)");
}

static void
test_atan2_origin(void)
{
    /* atan2(0, 0) is mathematically undefined; we return 0 as the
     * safe-default per the header contract. */
    test_check(axl_atan2(0.0, 0.0) == 0.0,
               "atan2: (0, 0) → 0 (safe default)");
}

static void
test_asin_known_values(void)
{
    test_check(axl_asin(0.0) == 0.0,
               "asin: 0 → 0 (exact)");
    test_check(near_(axl_asin(1.0), AXL_MATH_HALF_PI, 1e-15),
               "asin: 1 → π/2 (exact endpoint)");
    test_check(near_(axl_asin(-1.0), -AXL_MATH_HALF_PI, 1e-15),
               "asin: -1 → -π/2 (exact endpoint)");
    test_check(near_(axl_asin(0.5), AXL_MATH_PI / 6.0, 1e-8),
               "asin: 0.5 → π/6");
    /* asin(√2/2) = π/4. */
    test_check(near_(axl_asin(AXL_MATH_SQRT_2 / 2.0), AXL_MATH_PI / 4.0, 1e-8),
               "asin: √2/2 → π/4");
}

static void
test_asin_out_of_domain(void)
{
    test_check(axl_asin(2.0) == 0.0,
               "asin: |x| > 1 → 0 (safe default)");
    test_check(axl_asin(-1.5) == 0.0,
               "asin: |x| > 1 (negative) → 0 (safe default)");
}

static void
test_acos_known_values(void)
{
    test_check(near_(axl_acos(1.0), 0.0, 1e-15),
               "acos: 1 → 0 (exact endpoint)");
    test_check(near_(axl_acos(0.0), AXL_MATH_HALF_PI, 1e-15),
               "acos: 0 → π/2");
    test_check(near_(axl_acos(-1.0), AXL_MATH_PI, 1e-15),
               "acos: -1 → π (exact endpoint)");
    test_check(near_(axl_acos(0.5), AXL_MATH_PI / 3.0, 1e-8),
               "acos: 0.5 → π/3");
}

static void
test_acos_out_of_domain(void)
{
    test_check(axl_acos(2.0) == 0.0,
               "acos: |x| > 1 → 0 (safe default)");
}

static void
test_acos_precision_near_endpoints(void)
{
    /* The atan2-based implementation avoids the subtractive
     * cancellation that bites `π/2 - asin(x)` near x = ±1.
     * Verify via round-trip: cos(acos(x)) ≈ x.  The old
     * subtraction-based impl + sin/cos roundtrip would have
     * error of ~1e-4 here; the new factored form keeps error
     * within the documented sin/cos ~1e-6 envelope. */
    double x_set[] = { 0.9, 0.99, 0.999, 0.9999, -0.9999, -0.999, -0.9 };
    for (int i = 0; i < 7; i++) {
        double x = x_set[i];
        test_check(near_(axl_cos(axl_acos(x)), x, 1e-6),
                   "acos: cos(acos(x)) ≈ x near endpoints (loop)");
    }
    /* Sanity: the result has the right magnitude — acos near +1
     * is small, acos near -1 is close to π.  Loose bounds avoid
     * pinning a magic value we can't independently verify. */
    test_check(axl_acos(0.9999) < 0.02,
               "acos: 0.9999 → small (< 0.02)");
    test_check(axl_acos(-0.9999) > AXL_MATH_PI - 0.02,
               "acos: -0.9999 → close to π");
}

static void
test_asin_acos_identity(void)
{
    /* Fundamental identity: asin(x) + acos(x) == π/2. */
    double x_set[] = { -0.9, -0.5, -0.1, 0.0, 0.2, 0.7, 0.99 };
    for (int i = 0; i < 7; i++) {
        double x = x_set[i];
        test_check(near_(axl_asin(x) + axl_acos(x), AXL_MATH_HALF_PI, 1e-8),
                   "asin(x) + acos(x) → π/2 (loop iter)");
    }
}

// ---------------------------------------------------------------------------
// Bit math — Phase M6
// ---------------------------------------------------------------------------

static void
test_clz(void)
{
    test_check(axl_clz(0) == 64,
               "clz: 0 → 64 (defined value, not UB)");
    test_check(axl_clz(1) == 63,
               "clz: 1 → 63");
    test_check(axl_clz(2) == 62,
               "clz: 2 → 62");
    test_check(axl_clz(0xFFULL) == 56,
               "clz: 0xFF → 56");
    test_check(axl_clz(1ULL << 63) == 0,
               "clz: high bit → 0");
    test_check(axl_clz(~0ULL) == 0,
               "clz: ~0 → 0 (all bits set)");
}

static void
test_ctz(void)
{
    test_check(axl_ctz(0) == 64,
               "ctz: 0 → 64 (defined value, not UB)");
    test_check(axl_ctz(1) == 0,
               "ctz: 1 → 0");
    test_check(axl_ctz(2) == 1,
               "ctz: 2 → 1");
    test_check(axl_ctz(6) == 1,
               "ctz: 6 (binary 110) → 1");
    test_check(axl_ctz(8) == 3,
               "ctz: 8 → 3");
    test_check(axl_ctz(1ULL << 63) == 63,
               "ctz: high bit → 63");
}

static void
test_popcount(void)
{
    test_check(axl_popcount(0) == 0,
               "popcount: 0 → 0");
    test_check(axl_popcount(1) == 1,
               "popcount: 1 → 1");
    test_check(axl_popcount(7) == 3,
               "popcount: 7 (binary 111) → 3");
    test_check(axl_popcount(0xFFULL) == 8,
               "popcount: 0xFF → 8");
    test_check(axl_popcount(~0ULL) == 64,
               "popcount: ~0 → 64");
    test_check(axl_popcount(0xAAAAAAAAAAAAAAAAULL) == 32,
               "popcount: alternating bits → 32");
}

static void
test_log2i(void)
{
    test_check(axl_log2i(0) == 0,
               "log2i: 0 → 0 (safe default)");
    test_check(axl_log2i(1) == 0,
               "log2i: 1 → 0 (log₂(1) = 0)");
    test_check(axl_log2i(2) == 1,
               "log2i: 2 → 1");
    test_check(axl_log2i(255) == 7,
               "log2i: 255 → 7 (floor)");
    test_check(axl_log2i(256) == 8,
               "log2i: 256 → 8");
    test_check(axl_log2i(1ULL << 63) == 63,
               "log2i: high bit → 63");
}

static void
test_round_up_pow2(void)
{
    test_check(axl_round_up_pow2(0) == 1,
               "round_up_pow2: 0 → 1");
    test_check(axl_round_up_pow2(1) == 1,
               "round_up_pow2: 1 → 1 (already pow2)");
    test_check(axl_round_up_pow2(2) == 2,
               "round_up_pow2: 2 → 2 (already pow2)");
    test_check(axl_round_up_pow2(3) == 4,
               "round_up_pow2: 3 → 4");
    test_check(axl_round_up_pow2(5) == 8,
               "round_up_pow2: 5 → 8");
    test_check(axl_round_up_pow2(256) == 256,
               "round_up_pow2: 256 → 256 (already pow2)");
    test_check(axl_round_up_pow2(1000) == 1024,
               "round_up_pow2: 1000 → 1024");
    test_check(axl_round_up_pow2(1ULL << 63) == (1ULL << 63),
               "round_up_pow2: 2^63 → 2^63 (largest valid)");
    /* Overflow case: anything above 2^63 cannot round up. */
    test_check(axl_round_up_pow2((1ULL << 63) + 1) == 0,
               "round_up_pow2: > 2^63 → 0 (overflow safe-default)");
}

// ---------------------------------------------------------------------------
// Saturated arithmetic — Phase M7
// ---------------------------------------------------------------------------

static void
test_sat_add_u8(void)
{
    test_check(axl_sat_add_u8(0, 0) == 0,
               "sat_add_u8: 0+0 → 0");
    test_check(axl_sat_add_u8(10, 20) == 30,
               "sat_add_u8: 10+20 → 30 (no saturation)");
    test_check(axl_sat_add_u8(200, 50) == 250,
               "sat_add_u8: 200+50 → 250 (boundary just below)");
    test_check(axl_sat_add_u8(200, 55) == 255,
               "sat_add_u8: 200+55 → 255 (saturated, would wrap to 254)");
    test_check(axl_sat_add_u8(200, 100) == 255,
               "sat_add_u8: 200+100 → 255 (saturated)");
    test_check(axl_sat_add_u8(255, 255) == 255,
               "sat_add_u8: 255+255 → 255 (max saturation)");
}

static void
test_sat_sub_u8(void)
{
    test_check(axl_sat_sub_u8(0, 0) == 0,
               "sat_sub_u8: 0-0 → 0");
    test_check(axl_sat_sub_u8(100, 30) == 70,
               "sat_sub_u8: 100-30 → 70 (no saturation)");
    test_check(axl_sat_sub_u8(50, 50) == 0,
               "sat_sub_u8: 50-50 → 0 (boundary)");
    test_check(axl_sat_sub_u8(50, 100) == 0,
               "sat_sub_u8: 50-100 → 0 (saturated, would wrap to 206)");
    test_check(axl_sat_sub_u8(0, 255) == 0,
               "sat_sub_u8: 0-255 → 0 (max saturation)");
}

static void
test_sat_mul_u16(void)
{
    test_check(axl_sat_mul_u16(0, 12345) == 0,
               "sat_mul_u16: 0*x → 0");
    test_check(axl_sat_mul_u16(100, 200) == 20000,
               "sat_mul_u16: 100*200 → 20000 (no saturation)");
    test_check(axl_sat_mul_u16(255, 257) == 65535,
               "sat_mul_u16: 255*257 → 65535 (exact max, no saturation)");
    test_check(axl_sat_mul_u16(255, 258) == 65535,
               "sat_mul_u16: 255*258 → 65535 (just past max, saturated)");
    test_check(axl_sat_mul_u16(1000, 1000) == 65535,
               "sat_mul_u16: 1000*1000 → 65535 (saturated)");
    test_check(axl_sat_mul_u16(65535, 65535) == 65535,
               "sat_mul_u16: max*max → 65535");
}

// ---------------------------------------------------------------------------
// Linear algebra — Vec2 + Mat3 — Phase M3
// ---------------------------------------------------------------------------

/* Tolerance helper for Vec2 — both components within @a tol. */
static bool
vec2_near_(AxlVec2 actual, AxlVec2 expected, double tol)
{
    return near_(actual.x, expected.x, tol)
        && near_(actual.y, expected.y, tol);
}

static void
test_vec2_construct(void)
{
    AxlVec2 v = axl_vec2(3.0, -2.5);
    test_check(v.x == 3.0,
               "vec2: ctor x component");
    test_check(v.y == -2.5,
               "vec2: ctor y component");
}

static void
test_vec2_add_sub(void)
{
    AxlVec2 a = axl_vec2(1.0, 2.0);
    AxlVec2 b = axl_vec2(3.0, -1.0);
    test_check(vec2_near_(axl_vec2_add(a, b), axl_vec2(4.0, 1.0), 1e-15),
               "vec2: add componentwise");
    test_check(vec2_near_(axl_vec2_sub(a, b), axl_vec2(-2.0, 3.0), 1e-15),
               "vec2: sub componentwise");
    /* v - v == (0, 0). */
    test_check(vec2_near_(axl_vec2_sub(a, a), axl_vec2(0.0, 0.0), 1e-15),
               "vec2: v - v → (0, 0)");
}

static void
test_vec2_scale(void)
{
    AxlVec2 v = axl_vec2(2.0, -3.0);
    test_check(vec2_near_(axl_vec2_scale(v, 2.5), axl_vec2(5.0, -7.5), 1e-15),
               "vec2: scale by 2.5");
    test_check(vec2_near_(axl_vec2_scale(v, 0.0), axl_vec2(0.0, 0.0), 1e-15),
               "vec2: scale by 0 → (0, 0)");
    test_check(vec2_near_(axl_vec2_scale(v, -1.0), axl_vec2(-2.0, 3.0), 1e-15),
               "vec2: scale by -1 negates");
}

static void
test_vec2_dot(void)
{
    /* Parallel: positive product. */
    test_check(near_(axl_vec2_dot(axl_vec2(1.0, 0.0), axl_vec2(2.0, 0.0)),
                     2.0, 1e-15),
               "vec2: dot parallel → positive");
    /* Perpendicular: zero. */
    test_check(near_(axl_vec2_dot(axl_vec2(1.0, 0.0), axl_vec2(0.0, 1.0)),
                     0.0, 1e-15),
               "vec2: dot perpendicular → 0");
    /* Anti-parallel: negative. */
    test_check(near_(axl_vec2_dot(axl_vec2(1.0, 0.0), axl_vec2(-3.0, 0.0)),
                     -3.0, 1e-15),
               "vec2: dot anti-parallel → negative");
    /* Self-dot is length². */
    AxlVec2 v = axl_vec2(3.0, 4.0);
    test_check(near_(axl_vec2_dot(v, v), 25.0, 1e-15),
               "vec2: v · v == |v|² (= 25 for (3,4))");
}

static void
test_vec2_length(void)
{
    test_check(near_(axl_vec2_length(axl_vec2(3.0, 4.0)), 5.0, 1e-12),
               "vec2: length (3,4) → 5 (Pythagorean triple)");
    test_check(near_(axl_vec2_length(axl_vec2(0.0, 0.0)), 0.0, 1e-15),
               "vec2: length (0,0) → 0");
    test_check(near_(axl_vec2_length(axl_vec2(-5.0, 0.0)), 5.0, 1e-12),
               "vec2: length (-5,0) → 5 (sign-independent)");
}

static void
test_vec2_normalize(void)
{
    AxlVec2 n = axl_vec2_normalize(axl_vec2(3.0, 4.0));
    test_check(vec2_near_(n, axl_vec2(0.6, 0.8), 1e-12),
               "vec2: normalize (3,4) → (0.6, 0.8)");
    /* Normalized vector has length 1. */
    test_check(near_(axl_vec2_length(n), 1.0, 1e-12),
               "vec2: normalized vector has length 1");
    /* Zero-length input → (0, 0) safe-default. */
    test_check(vec2_near_(axl_vec2_normalize(axl_vec2(0.0, 0.0)),
                          axl_vec2(0.0, 0.0), 1e-15),
               "vec2: normalize (0,0) → (0, 0) (safe default)");
    /* Huge-magnitude input must NOT overflow to (0, 0) — the
     * pre-scaling path keeps the squared-sum in range.  (1e200,
     * 1e200) normalizes to (√½, √½) ≈ (0.707, 0.707). */
    AxlVec2 huge_n = axl_vec2_normalize(axl_vec2(1.0e200, 1.0e200));
    test_check(vec2_near_(huge_n, axl_vec2(0.70710678, 0.70710678), 1e-6),
               "vec2: normalize huge magnitude (1e200, 1e200) → (√½, √½)");
}

static void
test_vec2_geometry(void)
{
    test_check(vec2_near_(axl_vec2_lerp(axl_vec2(0, 0), axl_vec2(10, 20), 0.5),
                          axl_vec2(5, 10), 1e-15),
               "vec2: lerp midpoint → (5,10)");
    test_check(near_(axl_vec2_distance(axl_vec2(0, 0), axl_vec2(3, 4)), 5.0, 1e-12),
               "vec2: distance (0,0)-(3,4) == 5");
    test_check(vec2_near_(axl_vec2_perp(axl_vec2(1, 0)), axl_vec2(0, 1), 1e-15),
               "vec2: perp (1,0) == (0,1)");
    test_check(near_(axl_vec2_cross(axl_vec2(1, 0), axl_vec2(0, 1)), 1.0, 1e-15),
               "vec2: cross (1,0)×(0,1) == 1 (CCW)");
    test_check(near_(axl_vec2_cross(axl_vec2(1, 0), axl_vec2(2, 0)), 0.0, 1e-15),
               "vec2: cross parallel == 0");
    test_check(vec2_near_(axl_vec2_rotate(axl_vec2(1, 0), AXL_MATH_HALF_PI),
                          axl_vec2(0, 1), 1e-6),
               "vec2: rotate (1,0) by π/2 == (0,1)");
    test_check(near_(axl_vec2_angle(axl_vec2(0, 1)), AXL_MATH_HALF_PI, 1e-9),
               "vec2: angle (0,1) == π/2");
    test_check(near_(axl_vec2_angle(axl_vec2(0, 0)), 0.0, 1e-15),
               "vec2: angle (0,0) == 0 (safe)");
    /* reflect (1,-1) across the x-axis (unit normal (0,1)) → (1,1). */
    test_check(vec2_near_(axl_vec2_reflect(axl_vec2(1, -1), axl_vec2(0, 1)),
                          axl_vec2(1, 1), 1e-15),
               "vec2: reflect (1,-1) across x-axis → (1,1)");
    /* project (3,4) onto (1,0) → (3,0); onto zero → (0,0). */
    test_check(vec2_near_(axl_vec2_project(axl_vec2(3, 4), axl_vec2(1, 0)),
                          axl_vec2(3, 0), 1e-15),
               "vec2: project (3,4) onto x-axis → (3,0)");
    test_check(vec2_near_(axl_vec2_project(axl_vec2(3, 4), axl_vec2(0, 0)),
                          axl_vec2(0, 0), 1e-15),
               "vec2: project onto zero → (0,0) (safe)");
}

static void
test_transform_inverse_det_xvector(void)
{
    /* transform_vector applies the linear part only (ignores translation). */
    test_check(vec2_near_(axl_transform_map_vector(axl_transform_translate(5, 7),
                                                    axl_vec2(1, 1)),
                          axl_vec2(1, 1), 1e-15),
               "transform: transform_vector ignores translation");
    test_check(vec2_near_(axl_transform_map_vector(axl_transform_scale(2, 3),
                                                    axl_vec2(1, 1)),
                          axl_vec2(2, 3), 1e-15),
               "transform: transform_vector scales");
    /* determinant. */
    test_check(near_(axl_transform_determinant(axl_transform_scale(2, 3)), 6.0, 1e-12),
               "transform: det(scale(2,3)) == 6");
    test_check(near_(axl_transform_determinant(axl_transform_identity()), 1.0, 1e-15),
               "transform: det(identity) == 1");
    test_check(near_(axl_transform_determinant(axl_transform_scale(0.0, 5.0)), 0.0, 1e-15),
               "transform: det(singular) == 0");
    /* inverse round-trips a point: inv(m(p)) == p. */
    AxlTransform m = axl_transform_multiply(axl_transform_translate(5, 7),
                             axl_transform_rotate(AXL_MATH_HALF_PI));
    AxlTransform inv;
    test_check(axl_transform_invert(m, &inv), "transform: inverse non-singular → true");
    AxlVec2 p = axl_vec2(3.0, -2.0);
    AxlVec2 rt = axl_transform_map_point(inv, axl_transform_map_point(m, p));
    test_check(vec2_near_(rt, p, 1e-9), "transform: inverse round-trips a point");
    AxlTransform dummy;
    test_check(!axl_transform_invert(axl_transform_scale(0.0, 1.0), &dummy),
               "transform: inverse(singular) → false");

    /* Sheared inverse: full m·inv == identity (not just a single point —
       exercises the off-diagonal adjugate terms). */
    AxlTransform sh = axl_transform_multiply(axl_transform_scale(2.0, 3.0), axl_transform_shear(0.3, 0.7));
    AxlTransform shi;
    test_check(axl_transform_invert(sh, &shi), "transform: inverse(sheared) → true");
    AxlTransform sp = axl_transform_multiply(sh, shi);
    test_check(near_(sp.m[0], 1, 1e-9) && near_(sp.m[4], 1, 1e-9) &&
               near_(sp.m[8], 1, 1e-9) && near_(sp.m[1], 0, 1e-9) &&
               near_(sp.m[2], 0, 1e-9) && near_(sp.m[3], 0, 1e-9) &&
               near_(sp.m[5], 0, 1e-9) && near_(sp.m[6], 0, 1e-9) &&
               near_(sp.m[7], 0, 1e-9),
               "transform: sheared m·inv == identity (full matrix)");

    /* Non-affine matrix (non-zero bottom row) exercises the m6/m7
       adjugate terms the affine cases never reach. det = -2. */
    AxlTransform proj = { .m = { 2.0, 1.0, 3.0,  0.0, 2.0, 1.0,  1.0, 0.5, 1.0 } };
    AxlTransform pinv;
    test_check(axl_transform_invert(proj, &pinv), "transform: inverse(non-affine) → true");
    AxlTransform pp = axl_transform_multiply(proj, pinv);
    test_check(near_(pp.m[0], 1, 1e-9) && near_(pp.m[4], 1, 1e-9) &&
               near_(pp.m[8], 1, 1e-9) && near_(pp.m[1], 0, 1e-9) &&
               near_(pp.m[2], 0, 1e-9) && near_(pp.m[3], 0, 1e-9) &&
               near_(pp.m[5], 0, 1e-9) && near_(pp.m[6], 0, 1e-9) &&
               near_(pp.m[7], 0, 1e-9),
               "transform: non-affine m·inv == identity (m6/m7 adjugate)");
}

static void
test_transform_identity(void)
{
    AxlTransform I = axl_transform_identity();
    AxlVec2 p = axl_vec2(3.7, -1.2);
    test_check(vec2_near_(axl_transform_map_point(I, p), p, 1e-15),
               "transform: identity * p == p");
}

static void
test_transform_translate(void)
{
    AxlTransform T = axl_transform_translate(3.0, 4.0);
    test_check(vec2_near_(axl_transform_map_point(T, axl_vec2(0.0, 0.0)),
                          axl_vec2(3.0, 4.0), 1e-15),
               "transform: translate origin");
    test_check(vec2_near_(axl_transform_map_point(T, axl_vec2(1.0, 1.0)),
                          axl_vec2(4.0, 5.0), 1e-15),
               "transform: translate (1,1) by (3,4)");
    /* Two translations compose by addition. */
    AxlTransform U = axl_transform_translate(-1.0, 2.0);
    AxlTransform TU = axl_transform_multiply(T, U);
    test_check(vec2_near_(axl_transform_map_point(TU, axl_vec2(0.0, 0.0)),
                          axl_vec2(2.0, 6.0), 1e-15),
               "transform: translate(3,4) ∘ translate(-1,2) == translate(2,6)");
}

static void
test_transform_scale(void)
{
    AxlTransform S = axl_transform_scale(2.0, 3.0);
    test_check(vec2_near_(axl_transform_map_point(S, axl_vec2(1.0, 1.0)),
                          axl_vec2(2.0, 3.0), 1e-15),
               "transform: scale (1,1) by (2,3) → (2,3)");
    test_check(vec2_near_(axl_transform_map_point(S, axl_vec2(0.0, 0.0)),
                          axl_vec2(0.0, 0.0), 1e-15),
               "transform: scale fixes origin");
    /* Scale by (1,1) is identity. */
    AxlTransform S1 = axl_transform_scale(1.0, 1.0);
    test_check(vec2_near_(axl_transform_map_point(S1, axl_vec2(5.0, 7.0)),
                          axl_vec2(5.0, 7.0), 1e-15),
               "transform: scale(1,1) == identity");
}

static void
test_transform_rotate(void)
{
    /* Tolerance 1e-6: rotation uses axl_sin/axl_cos (documented
     * ~1e-7 accurate); 10× margin matches the existing sin/cos
     * test convention. */
    AxlVec2 x_axis = axl_vec2(1.0, 0.0);
    /* Rotate (1,0) by 0 → (1,0). */
    test_check(vec2_near_(axl_transform_map_point(axl_transform_rotate(0.0),
                                                   x_axis),
                          axl_vec2(1.0, 0.0), 1e-6),
               "transform: rotate by 0 → identity on (1,0)");
    /* Rotate (1,0) by π/2 → (0,1) (counter-clockwise). */
    test_check(vec2_near_(axl_transform_map_point(
                              axl_transform_rotate(AXL_MATH_HALF_PI),
                              x_axis),
                          axl_vec2(0.0, 1.0), 1e-6),
               "transform: rotate (1,0) by π/2 → (0,1)");
    /* Rotate (1,0) by π → (-1,0). */
    test_check(vec2_near_(axl_transform_map_point(
                              axl_transform_rotate(AXL_MATH_PI),
                              x_axis),
                          axl_vec2(-1.0, 0.0), 1e-6),
               "transform: rotate (1,0) by π → (-1,0)");
    /* Origin is fixed by rotation — exactly, since cos/sin
     * multiplied by 0 is 0 regardless of accuracy. */
    test_check(vec2_near_(axl_transform_map_point(
                              axl_transform_rotate(0.5),
                              axl_vec2(0.0, 0.0)),
                          axl_vec2(0.0, 0.0), 1e-15),
               "transform: rotation fixes origin");
}

static void
test_transform_mul(void)
{
    AxlTransform I = axl_transform_identity();
    AxlTransform T = axl_transform_translate(2.0, 3.0);
    /* I * T == T (verified via two points — origin exercises the
     * translation column, (5, 7) exercises the 2×2 sub-matrix too). */
    AxlTransform IT = axl_transform_multiply(I, T);
    test_check(vec2_near_(axl_transform_map_point(IT, axl_vec2(0.0, 0.0)),
                          axl_vec2(2.0, 3.0), 1e-15),
               "transform: I * T == T (origin transform)");
    test_check(vec2_near_(axl_transform_map_point(IT, axl_vec2(5.0, 7.0)),
                          axl_vec2(7.0, 10.0), 1e-15),
               "transform: I * T == T (off-origin transform)");
    /* T * I == T — same two probes. */
    AxlTransform TI = axl_transform_multiply(T, I);
    test_check(vec2_near_(axl_transform_map_point(TI, axl_vec2(0.0, 0.0)),
                          axl_vec2(2.0, 3.0), 1e-15),
               "transform: T * I == T (origin transform)");
    test_check(vec2_near_(axl_transform_map_point(TI, axl_vec2(5.0, 7.0)),
                          axl_vec2(7.0, 10.0), 1e-15),
               "transform: T * I == T (off-origin transform)");
    /* Compose translate-after-scale: scale by 2 first, then
     * translate by (10, 0).  Point (1, 1) → (2, 2) → (12, 2). */
    AxlTransform M = axl_transform_multiply(axl_transform_scale(2.0, 2.0),
                             axl_transform_translate(10.0, 0.0));
    test_check(vec2_near_(axl_transform_map_point(M, axl_vec2(1.0, 1.0)),
                          axl_vec2(12.0, 2.0), 1e-12),
               "transform: translate(10,0) ∘ scale(2,2) on (1,1) → (12,2)");
}

static void
test_transform_associativity(void)
{
    /* Matrix multiplication IS associative: (A * B) * C == A * (B * C).
     * Pin it as a regression guard — the property protects both the
     * index math and the composition direction in a single shot. */
    AxlTransform A = axl_transform_rotate(0.3);
    AxlTransform B = axl_transform_scale(2.0, 1.5);
    AxlTransform C = axl_transform_translate(4.0, -2.0);
    AxlTransform left  = axl_transform_multiply(axl_transform_multiply(A, B), C);
    AxlTransform right = axl_transform_multiply(A, axl_transform_multiply(B, C));
    AxlVec2 p     = axl_vec2(1.5, -0.7);
    test_check(vec2_near_(axl_transform_map_point(left,  p),
                          axl_transform_map_point(right, p),
                          1e-12),
               "transform: (A*B)*C == A*(B*C) (associativity)");
}

static void
test_transform_rotate_large_angle(void)
{
    /* Large-angle input exercises axl_sin's range reduction.  At
     * x=1e6 axl_sin is good to ~1e-3 per test_sin_large_magnitude,
     * so use that tolerance here. */
    test_check(vec2_near_(axl_transform_map_point(
                              axl_transform_rotate(1.0e6),
                              axl_vec2(1.0, 0.0)),
                          axl_vec2(0.93675212, -0.35000710), 1e-3),
               "transform: rotate(1e6) handles huge angle");
    /* Rotation by 2π should land back on the input (within sin/cos
     * accuracy).  Pins both periodicity and rotation correctness. */
    test_check(vec2_near_(axl_transform_map_point(
                              axl_transform_rotate(AXL_MATH_TWO_PI),
                              axl_vec2(1.0, 0.0)),
                          axl_vec2(1.0, 0.0), 1e-6),
               "transform: rotate(2π) on (1,0) → (1,0) (full period)");
}

static void
test_transform_rotate_then_translate(void)
{
    /* Real-world composition: rotate (1,0) by π/2 then translate
     * by (10, 0).  Should land at (10, 1).  Tolerance 1e-6 for
     * the same sin/cos accuracy reason as test_transform_rotate. */
    AxlTransform M = axl_transform_multiply(axl_transform_rotate(AXL_MATH_HALF_PI),
                             axl_transform_translate(10.0, 0.0));
    test_check(vec2_near_(axl_transform_map_point(M, axl_vec2(1.0, 0.0)),
                          axl_vec2(10.0, 1.0), 1e-6),
               "transform: translate(10,0) ∘ rotate(π/2) on (1,0) → (10,1)");
}

// ---------------------------------------------------------------------------
// Projective transforms — Phase 2 (perspective, quad_to_quad, map_rect/quad,
// classification)
// ---------------------------------------------------------------------------

static void
test_transform_perspective(void)
{
    /* perspective(0,0) is the identity. */
    test_check(axl_transform_is_identity(axl_transform_perspective(0.0, 0.0)),
               "transform: perspective(0,0) == identity");
    AxlTransform p = axl_transform_perspective(0.1, 0.2);
    test_check(near_(p.m[6], 0.1, 1e-15) && near_(p.m[7], 0.2, 1e-15) &&
               near_(p.m[8], 1.0, 1e-15),
               "transform: perspective sets bottom row [px py 1]");
    /* map_point divides by w = 0.1*x + 0.2*y + 1; at (1,1) w = 1.3. */
    AxlVec2 q = axl_transform_map_point(p, axl_vec2(1.0, 1.0));
    test_check(vec2_near_(q, axl_vec2(1.0 / 1.3, 1.0 / 1.3), 1e-12),
               "transform: perspective map_point divides by w");
    test_check(axl_transform_classify(p) == AXL_TRANSFORM_PROJECTIVE,
               "transform: perspective classifies PROJECTIVE");
    test_check(!axl_transform_is_affine(p),
               "transform: perspective is not affine");
    /* Affine map_point stays bit-exact (w == 1, divide is a no-op). */
    test_check(vec2_near_(axl_transform_map_point(axl_transform_translate(3, 4),
                                                  axl_vec2(1, 1)),
                          axl_vec2(4, 5), 0.0),
               "transform: affine map_point is exact (no divide error)");
}

static void
test_transform_classify(void)
{
    test_check(axl_transform_classify(axl_transform_identity())
               == AXL_TRANSFORM_IDENTITY, "classify: identity -> IDENTITY");
    test_check(axl_transform_classify(axl_transform_translate(3, 4))
               == AXL_TRANSFORM_TRANSLATE, "classify: translate -> TRANSLATE");
    test_check(axl_transform_classify(axl_transform_scale(2, 3))
               == AXL_TRANSFORM_SCALE, "classify: scale -> SCALE");
    /* diagonal linear + translation is still SCALE. */
    test_check(axl_transform_classify(
                   axl_transform_multiply(axl_transform_scale(2, 3),
                                          axl_transform_translate(5, 6)))
               == AXL_TRANSFORM_SCALE, "classify: scale∘translate -> SCALE");
    test_check(axl_transform_classify(axl_transform_rotate(0.5))
               == AXL_TRANSFORM_AFFINE, "classify: rotate -> AFFINE");
    test_check(axl_transform_classify(axl_transform_shear(0.3, 0.0))
               == AXL_TRANSFORM_AFFINE, "classify: shear -> AFFINE");
    test_check(axl_transform_classify(axl_transform_perspective(0.1, 0.0))
               == AXL_TRANSFORM_PROJECTIVE, "classify: perspective -> PROJECTIVE");
    /* scale(1,1) is the identity, not SCALE. */
    test_check(axl_transform_classify(axl_transform_scale(1, 1))
               == AXL_TRANSFORM_IDENTITY, "classify: scale(1,1) -> IDENTITY");
}

static void
test_transform_predicates(void)
{
    test_check(axl_transform_is_identity(axl_transform_identity()),
               "is_identity: identity -> true");
    test_check(!axl_transform_is_identity(axl_transform_translate(1, 0)),
               "is_identity: translate -> false");
    test_check(axl_transform_is_affine(axl_transform_rotate(0.7)),
               "is_affine: rotate -> true");
    test_check(!axl_transform_is_affine(axl_transform_perspective(0.2, 0.0)),
               "is_affine: perspective -> false");
    /* axis-aligned: diagonal OR anti-diagonal linear, non-perspective. */
    test_check(axl_transform_is_axis_aligned(axl_transform_scale(2, 3)),
               "is_axis_aligned: scale -> true");
    test_check(axl_transform_is_axis_aligned(axl_transform_translate(5, 7)),
               "is_axis_aligned: translate -> true");
    test_check(axl_transform_is_axis_aligned(axl_transform_scale(1, -1)),
               "is_axis_aligned: y-flip -> true");
    /* exact 90° rotation (anti-diagonal) — hand-built to avoid trig fuzz. */
    AxlTransform rot90 = { .m = { 0, -1, 0,  1, 0, 0,  0, 0, 1 } };
    test_check(axl_transform_is_axis_aligned(rot90),
               "is_axis_aligned: 90° rotation (anti-diagonal) -> true");
    test_check(!axl_transform_is_axis_aligned(axl_transform_rotate(0.5)),
               "is_axis_aligned: arbitrary rotation -> false");
    test_check(!axl_transform_is_axis_aligned(axl_transform_shear(0.3, 0.0)),
               "is_axis_aligned: shear -> false");
    test_check(!axl_transform_is_axis_aligned(axl_transform_perspective(0.1, 0.0)),
               "is_axis_aligned: perspective -> false");
}

static void
test_transform_map_rect(void)
{
    /* Pure translation: exact, dimensions preserved. */
    AxlRect t = axl_transform_map_rect(axl_transform_translate(5, 7),
                                       (AxlRect){ 10, 20, 100, 50 });
    test_check(near_(t.x, 15, 1e-12) && near_(t.y, 27, 1e-12) &&
               near_(t.w, 100, 1e-12) && near_(t.h, 50, 1e-12),
               "map_rect: translate is exact");
    /* Scale about origin: corners (1,1)-(3,3) -> (2,3)-(6,9). */
    AxlRect s = axl_transform_map_rect(axl_transform_scale(2, 3),
                                       (AxlRect){ 1, 1, 2, 2 });
    test_check(near_(s.x, 2, 1e-12) && near_(s.y, 3, 1e-12) &&
               near_(s.w, 4, 1e-12) && near_(s.h, 6, 1e-12),
               "map_rect: scale is exact");
    /* 90° rotation swaps the extents (axis-aligned -> exact bbox). */
    AxlTransform rot90 = { .m = { 0, -1, 0,  1, 0, 0,  0, 0, 1 } };
    AxlRect r = axl_transform_map_rect(rot90, (AxlRect){ 0, 0, 2, 4 });
    test_check(near_(r.x, -4, 1e-12) && near_(r.y, 0, 1e-12) &&
               near_(r.w, 4, 1e-12) && near_(r.h, 2, 1e-12),
               "map_rect: 90° rotation swaps extents, exact bbox");
    /* Arbitrary rotation grows the AABB (conservative cover). */
    AxlRect g = axl_transform_map_rect(axl_transform_rotate(0.5),
                                       (AxlRect){ 0, 0, 2, 2 });
    test_check(g.w > 2.0 && g.h > 2.0,
               "map_rect: arbitrary rotation grows the AABB");
    /* Output is normalized (non-negative w/h) even under a flip. */
    AxlRect f = axl_transform_map_rect(axl_transform_scale(-1, 1),
                                       (AxlRect){ 0, 0, 3, 5 });
    test_check(f.w >= 0 && f.h >= 0 && near_(f.x, -3, 1e-12) &&
               near_(f.w, 3, 1e-12) && near_(f.h, 5, 1e-12),
               "map_rect: x-flip stays normalized");
}

static void
test_transform_map_quad(void)
{
    AxlVec2 in[4]  = { axl_vec2(0, 0), axl_vec2(1, 0),
                       axl_vec2(1, 1), axl_vec2(0, 1) };
    AxlVec2 out[4];
    axl_transform_map_quad(axl_transform_translate(10, 20), in, out);
    test_check(vec2_near_(out[0], axl_vec2(10, 20), 1e-12) &&
               vec2_near_(out[1], axl_vec2(11, 20), 1e-12) &&
               vec2_near_(out[2], axl_vec2(11, 21), 1e-12) &&
               vec2_near_(out[3], axl_vec2(10, 21), 1e-12),
               "map_quad: translate maps all four corners");
    /* in and out may alias. */
    AxlVec2 buf[4] = { axl_vec2(0, 0), axl_vec2(2, 0),
                       axl_vec2(2, 2), axl_vec2(0, 2) };
    axl_transform_map_quad(axl_transform_scale(3, 3), buf, buf);
    test_check(vec2_near_(buf[2], axl_vec2(6, 6), 1e-12),
               "map_quad: in/out aliasing works");
}

static void
test_transform_quad_to_quad(void)
{
    AxlVec2 unit[4] = { axl_vec2(0, 0), axl_vec2(1, 0),
                        axl_vec2(1, 1), axl_vec2(0, 1) };
    AxlTransform m;

    /* Identity correspondence: an arbitrary quad onto itself. */
    AxlVec2 q[4] = { axl_vec2(2, 3), axl_vec2(9, 4),
                     axl_vec2(8, 11), axl_vec2(1, 10) };
    test_check(axl_transform_quad_to_quad(q, q, &m),
               "quad_to_quad: self-map succeeds");
    bool self_ok = true;
    for (int i = 0; i < 4; i++) {
        if (!vec2_near_(axl_transform_map_point(m, q[i]), q[i], 1e-9)) {
            self_ok = false;
        }
    }
    test_check(self_ok, "quad_to_quad: self-map fixes every corner");

    /* Unit square -> trapezoid (genuinely projective). */
    AxlVec2 trap[4] = { axl_vec2(0, 0), axl_vec2(10, 0),
                        axl_vec2(8, 10), axl_vec2(2, 10) };
    test_check(axl_transform_quad_to_quad(unit, trap, &m),
               "quad_to_quad: square->trapezoid succeeds");
    bool trap_ok = true;
    for (int i = 0; i < 4; i++) {
        if (!vec2_near_(axl_transform_map_point(m, unit[i]), trap[i], 1e-9)) {
            trap_ok = false;
        }
    }
    test_check(trap_ok, "quad_to_quad: square->trapezoid maps every corner");
    test_check(axl_transform_classify(m) == AXL_TRANSFORM_PROJECTIVE,
               "quad_to_quad: trapezoid map is PROJECTIVE");

    /* Unit square -> parallelogram (affine subset). */
    AxlVec2 para[4] = { axl_vec2(0, 0), axl_vec2(2, 0),
                        axl_vec2(3, 1), axl_vec2(1, 1) };
    test_check(axl_transform_quad_to_quad(unit, para, &m),
               "quad_to_quad: square->parallelogram succeeds");
    test_check(axl_transform_is_affine(m),
               "quad_to_quad: parallelogram map is affine");
    bool para_ok = true;
    for (int i = 0; i < 4; i++) {
        if (!vec2_near_(axl_transform_map_point(m, unit[i]), para[i], 1e-9)) {
            para_ok = false;
        }
    }
    test_check(para_ok, "quad_to_quad: square->parallelogram maps every corner");

    /* Concave (non-convex) simple quad still maps every corner — the
     * closed form needs only a non-degenerate quad, not convexity. */
    AxlVec2 concave[4] = { axl_vec2(0, 0), axl_vec2(4, 0),
                           axl_vec2(1, 1), axl_vec2(0, 4) };
    test_check(axl_transform_quad_to_quad(unit, concave, &m),
               "quad_to_quad: square->concave quad succeeds");
    bool concave_ok = true;
    for (int i = 0; i < 4; i++) {
        if (!vec2_near_(axl_transform_map_point(m, unit[i]), concave[i], 1e-9)) {
            concave_ok = false;
        }
    }
    test_check(concave_ok, "quad_to_quad: concave quad maps every corner");

    /* Degenerate (collinear) source -> false, out untouched. */
    AxlVec2 line[4] = { axl_vec2(0, 0), axl_vec2(1, 1),
                        axl_vec2(2, 2), axl_vec2(3, 3) };
    AxlTransform before = axl_transform_scale(7, 7);
    AxlTransform keep = before;
    test_check(!axl_transform_quad_to_quad(line, unit, &keep),
               "quad_to_quad: degenerate source -> false");
    test_check(keep.m[0] == before.m[0] && keep.m[4] == before.m[4],
               "quad_to_quad: out untouched on failure");
}

// ---------------------------------------------------------------------------
// Geometry helpers — Phase M8
// ---------------------------------------------------------------------------

static void
test_point_in_rect(void)
{
    AxlRect r = { 10.0, 20.0, 100.0, 50.0 };  /* x=10..110, y=20..70 */
    test_check(axl_point_in_rect(axl_vec2(50.0, 40.0), r),
               "point_in_rect: interior");
    test_check(axl_point_in_rect(axl_vec2(10.0, 20.0), r),
               "point_in_rect: top-left corner included (closed)");
    /* Half-open: right and bottom edges EXCLUDED. */
    test_check(!axl_point_in_rect(axl_vec2(110.0, 40.0), r),
               "point_in_rect: right edge excluded (half-open)");
    test_check(!axl_point_in_rect(axl_vec2(50.0, 70.0), r),
               "point_in_rect: bottom edge excluded (half-open)");
    /* Outside. */
    test_check(!axl_point_in_rect(axl_vec2(0.0, 40.0), r),
               "point_in_rect: left of rect");
    test_check(!axl_point_in_rect(axl_vec2(50.0, 0.0), r),
               "point_in_rect: above rect");
    test_check(!axl_point_in_rect(axl_vec2(200.0, 200.0), r),
               "point_in_rect: far outside");
}

static void
test_rect_intersect(void)
{
    AxlRect a = { 0.0, 0.0, 100.0, 100.0 };
    AxlRect b = { 50.0, 50.0, 100.0, 100.0 };
    AxlRect ab = axl_rect_intersect(a, b);
    test_check(ab.x == 50.0 && ab.y == 50.0
               && ab.w == 50.0 && ab.h == 50.0,
               "rect_intersect: overlap → correct intersection rect");
    /* Disjoint → empty rect. */
    AxlRect c = { 200.0, 200.0, 50.0, 50.0 };
    AxlRect ac = axl_rect_intersect(a, c);
    test_check(ac.w == 0.0 && ac.h == 0.0,
               "rect_intersect: disjoint → empty rect");
    /* a ∩ a == a (self-intersection is identity). */
    AxlRect aa = axl_rect_intersect(a, a);
    test_check(aa.x == 0.0 && aa.y == 0.0
               && aa.w == 100.0 && aa.h == 100.0,
               "rect_intersect: a ∩ a → a");
    /* a fully contains b → intersection is b. */
    AxlRect big   = { 0.0, 0.0, 200.0, 200.0 };
    AxlRect small = { 50.0, 50.0, 30.0, 30.0 };
    AxlRect bs    = axl_rect_intersect(big, small);
    test_check(bs.x == 50.0 && bs.y == 50.0
               && bs.w == 30.0 && bs.h == 30.0,
               "rect_intersect: containment → contained rect");
}

static void
test_rect_union(void)
{
    AxlRect a = { 0.0, 0.0, 100.0, 100.0 };
    AxlRect b = { 50.0, 50.0, 100.0, 100.0 };
    AxlRect u = axl_rect_union(a, b);
    test_check(u.x == 0.0 && u.y == 0.0
               && u.w == 150.0 && u.h == 150.0,
               "rect_union: overlap → bounding box");
    /* Disjoint rects union to their bounding box. */
    AxlRect c = { 200.0, 200.0, 50.0, 50.0 };
    AxlRect ac = axl_rect_union(a, c);
    test_check(ac.x == 0.0 && ac.y == 0.0
               && ac.w == 250.0 && ac.h == 250.0,
               "rect_union: disjoint → bounding box");
    /* Empty rect on either side returns the other. */
    AxlRect empty = { 100.0, 100.0, 0.0, 0.0 };
    AxlRect ae    = axl_rect_union(a, empty);
    test_check(ae.x == 0.0 && ae.y == 0.0
               && ae.w == 100.0 && ae.h == 100.0,
               "rect_union: empty + a → a");
}

static void
test_segment_intersect(void)
{
    /* Two diagonals of a unit square cross at (0.5, 0.5). */
    AxlVec2 out;
    test_check(axl_segment_intersect(axl_vec2(0.0, 0.0), axl_vec2(1.0, 1.0),
                                     axl_vec2(0.0, 1.0), axl_vec2(1.0, 0.0),
                                     &out),
               "segment_intersect: square diagonals cross");
    test_check(near_(out.x, 0.5, 1e-12) && near_(out.y, 0.5, 1e-12),
               "segment_intersect: out point at (0.5, 0.5)");
    /* T-junction: segment ends exactly on another segment. */
    test_check(axl_segment_intersect(axl_vec2(0.0, 0.0), axl_vec2(2.0, 0.0),
                                     axl_vec2(1.0, 0.0), axl_vec2(1.0, 1.0),
                                     NULL),
               "segment_intersect: T-junction (endpoint on segment)");
    /* Disjoint segments — far apart. */
    test_check(!axl_segment_intersect(axl_vec2(0.0, 0.0), axl_vec2(1.0, 0.0),
                                      axl_vec2(2.0, 1.0), axl_vec2(3.0, 1.0),
                                      NULL),
               "segment_intersect: disjoint → false");
    /* Parallel segments — false even if collinear (documented). */
    test_check(!axl_segment_intersect(axl_vec2(0.0, 0.0), axl_vec2(1.0, 0.0),
                                      axl_vec2(0.0, 1.0), axl_vec2(1.0, 1.0),
                                      NULL),
               "segment_intersect: parallel non-collinear → false");
    /* Segments whose extensions intersect, but segments themselves
     * don't. */
    test_check(!axl_segment_intersect(axl_vec2(0.0, 0.0), axl_vec2(1.0, 0.0),
                                      axl_vec2(2.0, -1.0), axl_vec2(2.0, 1.0),
                                      NULL),
               "segment_intersect: extensions meet but segments don't");
}

static void
test_distance_point_to_segment(void)
{
    /* Segment along x-axis from (0,0) to (10,0).  Point (5, 3)
     * projects to (5, 0); distance = 3. */
    test_check(near_(axl_distance_point_to_segment(
                         axl_vec2(5.0, 3.0),
                         axl_vec2(0.0, 0.0), axl_vec2(10.0, 0.0)),
                     3.0, 1e-12),
               "distance: point projects within segment → 3");
    /* Point past the segment end clamps to the endpoint. */
    test_check(near_(axl_distance_point_to_segment(
                         axl_vec2(15.0, 0.0),
                         axl_vec2(0.0, 0.0), axl_vec2(10.0, 0.0)),
                     5.0, 1e-12),
               "distance: point past end → clamps to endpoint (distance 5)");
    /* Point before the segment start clamps to the start. */
    test_check(near_(axl_distance_point_to_segment(
                         axl_vec2(-3.0, 4.0),
                         axl_vec2(0.0, 0.0), axl_vec2(10.0, 0.0)),
                     5.0, 1e-12),
               "distance: point before start → clamps (3,4,5 triple)");
    /* Point on the segment → distance 0. */
    test_check(near_(axl_distance_point_to_segment(
                         axl_vec2(5.0, 0.0),
                         axl_vec2(0.0, 0.0), axl_vec2(10.0, 0.0)),
                     0.0, 1e-12),
               "distance: point on segment → 0");
    /* Degenerate segment (a == b) → distance is just |p - a|. */
    test_check(near_(axl_distance_point_to_segment(
                         axl_vec2(3.0, 4.0),
                         axl_vec2(0.0, 0.0), axl_vec2(0.0, 0.0)),
                     5.0, 1e-12),
               "distance: degenerate segment (a==b) → |p - a|");
}

static void
test_circle_circle_intersect(void)
{
    AxlCircle a = { .center = { 0.0, 0.0 }, .radius = 1.0 };
    AxlCircle b = { .center = { 1.5, 0.0 }, .radius = 1.0 };
    test_check(axl_circle_circle_intersect(a, b),
               "circle_intersect: overlap → true");
    /* Tangent externally: distance == r_sum. */
    AxlCircle c = { .center = { 2.0, 0.0 }, .radius = 1.0 };
    test_check(axl_circle_circle_intersect(a, c),
               "circle_intersect: tangent external (d == r_sum) → true");
    /* Disjoint. */
    AxlCircle d = { .center = { 5.0, 0.0 }, .radius = 1.0 };
    test_check(!axl_circle_circle_intersect(a, d),
               "circle_intersect: disjoint → false");
    /* Concentric — smaller fits inside larger; still overlap. */
    AxlCircle inner = { .center = { 0.0, 0.0 }, .radius = 0.5 };
    test_check(axl_circle_circle_intersect(a, inner),
               "circle_intersect: containment counts as overlap");
    /* Tangent internally: one inside touching the other from
     * within (d == |r_a - r_b|).  Still counts as intersect. */
    AxlCircle big   = { .center = { 0.0, 0.0 }, .radius = 2.0 };
    AxlCircle small = { .center = { 1.0, 0.0 }, .radius = 1.0 };
    test_check(axl_circle_circle_intersect(big, small),
               "circle_intersect: tangent internal → true");
}

static void
test_geometry_edge_cases(void)
{
    /* B2: rect_intersect of touching rects (shared edge only) is
     * empty per the half-open convention.  Pin so the intentional
     * "touching is not overlap" stays the documented contract. */
    AxlRect left  = { 0.0, 0.0, 10.0, 10.0 };
    AxlRect right = { 10.0, 0.0, 10.0, 10.0 };
    AxlRect r = axl_rect_intersect(left, right);
    test_check(r.w == 0.0 && r.h == 0.0,
               "rect_intersect: shared-edge touch → empty");

    /* T2: rect_union of two empties returns the second
     * (b's short-circuit fires after a's empty short-circuit
     * passes). */
    AxlRect empty1 = { 5.0, 5.0, 0.0, 0.0 };
    AxlRect empty2 = { 9.0, 9.0, 0.0, 0.0 };
    AxlRect ee = axl_rect_union(empty1, empty2);
    test_check(ee.x == 9.0 && ee.y == 9.0 && ee.w == 0.0 && ee.h == 0.0,
               "rect_union: both empty → returns b");

    /* T2: negative-extent rect — point_in_rect returns false for
     * any point (r.x + r.w < r.x makes the `<` test always fail). */
    AxlRect neg = { 10.0, 10.0, -5.0, -5.0 };
    test_check(!axl_point_in_rect(axl_vec2(10.0, 10.0), neg),
               "point_in_rect: negative extent → false for any p");
    test_check(!axl_point_in_rect(axl_vec2(7.0, 7.0), neg),
               "point_in_rect: negative extent → false even at interior");

    /* T2: segment_intersect with both segments degenerate to the
     * same point.  denom = 0 → false (defensible). */
    AxlVec2 same = axl_vec2(3.0, 4.0);
    test_check(!axl_segment_intersect(same, same, same, same, NULL),
               "segment_intersect: degenerate point pair → false");

    /* T3: real T-junction at endpoint-of-endpoint: segment A's
     * endpoint is exactly segment B's start.  t=1, s=0 — both
     * still inside [0, 1] inclusive. */
    AxlVec2 out;
    test_check(axl_segment_intersect(
                   axl_vec2(0.0, 0.0), axl_vec2(1.0, 0.0),
                   axl_vec2(1.0, 0.0), axl_vec2(1.0, 1.0),
                   &out),
               "segment_intersect: endpoint-to-endpoint T (t=1, s=0)");
    test_check(near_(out.x, 1.0, 1e-12) && near_(out.y, 0.0, 1e-12),
               "segment_intersect: T-junction out at (1, 0)");
}

// ---------------------------------------------------------------------------
// Suite entry point
// ---------------------------------------------------------------------------

int
test_math_main(
    int    argc,
    char **argv
    )
{
    (void)argc;
    (void)argv;

    test_print_header("AxlMath");

    test_constants();

    test_floor_positive();
    test_floor_negative();
    test_ceil_positive();
    test_ceil_negative();
    test_floori_and_ceili();
    test_fabs();

    test_sqrt_exact_squares();
    test_sqrt_irrational();
    test_sqrt_edge_cases();

    test_fmod_basic();
    test_fmod_zero_divisor();
    test_fmod_sign();
    test_wrap();

    test_sin_known_values();
    test_sin_periodicity();
    test_sin_quadrants();
    test_sin_large_magnitude();
    test_cos_known_values();
    test_cos_identity_with_sin();

    test_ln_known_values();
    test_ln_edge_cases();
    test_ln_large_and_small();
    test_ln_non_normal();
    test_exp_known_values();
    test_exp_identity_with_ln();
    test_exp_extremes();
    test_pow_known_values();
    test_pow_edge_cases();

    test_lerp();
    test_ease_endpoints();
    test_ease_midpoints();
    test_ease_in_out_symmetry();
    test_ease_in_out_complementarity();
    test_ease_monotonic();

    test_atan_known_values();
    test_atan_reduction_boundaries();
    test_atan_extreme();
    test_atan2_quadrants();
    test_atan2_origin();
    test_asin_known_values();
    test_asin_out_of_domain();
    test_acos_known_values();
    test_acos_out_of_domain();
    test_acos_precision_near_endpoints();
    test_asin_acos_identity();

    test_sat_add_u8();
    test_sat_sub_u8();
    test_sat_mul_u16();

    test_clz();
    test_ctz();
    test_popcount();
    test_log2i();
    test_round_up_pow2();

    test_vec2_construct();
    test_vec2_add_sub();
    test_vec2_scale();
    test_vec2_dot();
    test_vec2_length();
    test_vec2_normalize();
    test_vec2_geometry();
    test_transform_inverse_det_xvector();
    test_transform_identity();
    test_transform_translate();
    test_transform_scale();
    test_transform_rotate();
    test_transform_mul();
    test_transform_associativity();
    test_transform_rotate_then_translate();
    test_transform_rotate_large_angle();

    test_transform_perspective();
    test_transform_classify();
    test_transform_predicates();
    test_transform_map_rect();
    test_transform_map_quad();
    test_transform_quad_to_quad();

    test_point_in_rect();
    test_rect_intersect();
    test_rect_union();
    test_segment_intersect();
    test_distance_point_to_segment();
    test_circle_circle_intersect();
    test_geometry_edge_cases();

    test_clamp_inside();
    test_clamp_outside();
    test_clamp_degenerate();
    test_min();
    test_max();
    test_remap_midpoint();
    test_remap_inverted_output();
    test_remap_extrapolation();
    test_remap_degenerate_input();
    test_step();
    test_smoothstep_boundary();
    test_smoothstep_midpoint();
    test_smoothstep_shape();

    return test_print_results();
}

AXL_APP(test_math_main)
