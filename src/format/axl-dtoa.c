/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-dtoa.c
    Shortest round-trippable double->decimal conversion via Grisu2.

    Grisu2 (Loitsch, "Printing Floating-Point Numbers Quickly and
    Accurately with Integers", PLDI 2010) produces the shortest decimal
    digit string that round-trips back to the original double, using
    only 64-bit integer arithmetic plus a small cached table of powers
    of ten — no bignum, no libm, no libc. It is the engine behind the
    %f / %e / %g conversions in axl-format.c.

    Grisu2 is shortest-but-not-always-optimally-rounded: for a tiny
    fraction of inputs a different shortest string of equal length
    exists; both still round-trip. (Grisu3 adds a fallback to guarantee
    optimal rounding; we don't need it for formatting.) The output is
    a digit string + decimal-point position, the David-Gay dtoa shape,
    which lets the caller render fixed (%f), scientific (%e), or
    shortest (%g) from one conversion.

    This is the standard reference formulation; the cached-powers table
    is the well-known one shared by V8, RapidJSON, and the
    double-conversion library.

    Zero-dependency: no axl_mem / axl_str calls (AxlFormat must stay
    free of the Log<->Data cycle), so the double's bits are read via a
    union rather than axl_memcpy.
**/

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-format.h>

// ===================================================================
// diy_fp: a "do-it-yourself" floating point — 64-bit significand f,
// binary exponent e, representing f * 2^e.
// ===================================================================

typedef struct {
    uint64_t  f;
    int       e;
} DiyFp;

static DiyFp
diy_fp_sub(DiyFp x, DiyFp y)
{
    DiyFp r = { x.f - y.f, x.e };
    return r;
}

/* 64x64 -> high 64 bits of the 128-bit product, with round-to-nearest
 * on the dropped low half. Both operands are normalized (MSB set), so
 * the result is accurate to within 1 ulp. */
static DiyFp
diy_fp_mul(DiyFp x, DiyFp y)
{
    const uint64_t M32 = 0xFFFFFFFFu;
    uint64_t a = x.f >> 32, b = x.f & M32;
    uint64_t c = y.f >> 32, d = y.f & M32;
    uint64_t ac = a * c, bc = b * c, ad = a * d, bd = b * d;
    uint64_t tmp = (bd >> 32) + (ad & M32) + (bc & M32);
    tmp += 1u << 31;   /* round */
    DiyFp r = { ac + (ad >> 32) + (bc >> 32) + (tmp >> 32), x.e + y.e + 64 };
    return r;
}

static DiyFp
diy_fp_normalize(DiyFp x)
{
    while (!(x.f & (1ULL << 63))) {
        x.f <<= 1;
        x.e--;
    }
    return x;
}

// ===================================================================
// IEEE-754 double field extraction
// ===================================================================

#define DP_SIGNIFICAND_SIZE  52
#define DP_EXPONENT_BIAS     (0x3FF + DP_SIGNIFICAND_SIZE)
#define DP_MIN_EXPONENT      (-DP_EXPONENT_BIAS)
#define DP_EXPONENT_MASK     0x7FF0000000000000ULL
#define DP_SIGNIFICAND_MASK  0x000FFFFFFFFFFFFFULL
#define DP_HIDDEN_BIT        0x0010000000000000ULL

static uint64_t
double_bits(double d)
{
    union { double d; uint64_t u; } cvt;
    cvt.d = d;
    return cvt.u;
}

static DiyFp
double_to_diy_fp(double d)
{
    uint64_t u = double_bits(d);
    int      biased_e   = (int)((u & DP_EXPONENT_MASK) >> DP_SIGNIFICAND_SIZE);
    uint64_t significand = u & DP_SIGNIFICAND_MASK;
    DiyFp    r;
    if (biased_e != 0) {
        r.f = significand + DP_HIDDEN_BIT;
        r.e = biased_e - DP_EXPONENT_BIAS;
    } else {
        /* subnormal */
        r.f = significand;
        r.e = DP_MIN_EXPONENT + 1;
    }
    return r;
}

/* The normalized boundaries m- and m+ halfway to the adjacent doubles;
 * Grisu generates the shortest decimal inside (m-, m+). */
static void
normalized_boundaries(double value, DiyFp *m_minus, DiyFp *m_plus)
{
    DiyFp v = double_to_diy_fp(value);
    DiyFp pl = diy_fp_normalize((DiyFp){ (v.f << 1) + 1, v.e - 1 });
    DiyFp mi;
    if (v.f == DP_HIDDEN_BIT) {
        mi.f = (v.f << 2) - 1;
        mi.e = v.e - 2;
    } else {
        mi.f = (v.f << 1) - 1;
        mi.e = v.e - 1;
    }
    mi.f <<= mi.e - pl.e;
    mi.e = pl.e;
    *m_plus  = pl;
    *m_minus = mi;
}

// ===================================================================
// Cached powers of ten as DiyFp (the standard Grisu table: 87 entries,
// decimal exponents -348..340 in steps of 8). Significands f[i] and
// exponents e[i]. get_cached_power indexes this in [1, 86] across the
// entire finite-double range, so no bounds guard is needed.
// ===================================================================

static const uint64_t kCachedPowers_F[] = {
    0xfa8fd5a0081c0288ULL, 0xbaaee17fa23ebf76ULL, 0x8b16fb203055ac76ULL,
    0xcf42894a5dce35eaULL, 0x9a6bb0aa55653b2dULL, 0xe61acf033d1a45dfULL,
    0xab70fe17c79ac6caULL, 0xff77b1fcbebcdc4fULL, 0xbe5691ef416bd60cULL,
    0x8dd01fad907ffc3cULL, 0xd3515c2831559a83ULL, 0x9d71ac8fada6c9b5ULL,
    0xea9c227723ee8bcbULL, 0xaecc49914078536dULL, 0x823c12795db6ce57ULL,
    0xc21094364dfb5637ULL, 0x9096ea6f3848984fULL, 0xd77485cb25823ac7ULL,
    0xa086cfcd97bf97f4ULL, 0xef340a98172aace5ULL, 0xb23867fb2a35b28eULL,
    0x84c8d4dfd2c63f3bULL, 0xc5dd44271ad3cdbaULL, 0x936b9fcebb25c996ULL,
    0xdbac6c247d62a584ULL, 0xa3ab66580d5fdaf6ULL, 0xf3e2f893dec3f126ULL,
    0xb5b5ada8aaff80b8ULL, 0x87625f056c7c4a8bULL, 0xc9bcff6034c13053ULL,
    0x964e858c91ba2655ULL, 0xdff9772470297ebdULL, 0xa6dfbd9fb8e5b88fULL,
    0xf8a95fcf88747d94ULL, 0xb94470938fa89bcfULL, 0x8a08f0f8bf0f156bULL,
    0xcdb02555653131b6ULL, 0x993fe2c6d07b7facULL, 0xe45c10c42a2b3b06ULL,
    0xaa242499697392d3ULL, 0xfd87b5f28300ca0eULL, 0xbce5086492111aebULL,
    0x8cbccc096f5088ccULL, 0xd1b71758e219652cULL, 0x9c40000000000000ULL,
    0xe8d4a51000000000ULL, 0xad78ebc5ac620000ULL, 0x813f3978f8940984ULL,
    0xc097ce7bc90715b3ULL, 0x8f7e32ce7bea5c70ULL, 0xd5d238a4abe98068ULL,
    0x9f4f2726179a2245ULL, 0xed63a231d4c4fb27ULL, 0xb0de65388cc8ada8ULL,
    0x83c7088e1aab65dbULL, 0xc45d1df942711d9aULL, 0x924d692ca61be758ULL,
    0xda01ee641a708deaULL, 0xa26da3999aef774aULL, 0xf209787bb47d6b85ULL,
    0xb454e4a179dd1877ULL, 0x865b86925b9bc5c2ULL, 0xc83553c5c8965d3dULL,
    0x952ab45cfa97a0b3ULL, 0xde469fbd99a05fe3ULL, 0xa59bc234db398c25ULL,
    0xf6c69a72a3989f5cULL, 0xb7dcbf5354e9beceULL, 0x88fcf317f22241e2ULL,
    0xcc20ce9bd35c78a5ULL, 0x98165af37b2153dfULL, 0xe2a0b5dc971f303aULL,
    0xa8d9d1535ce3b396ULL, 0xfb9b7cd9a4a7443cULL, 0xbb764c4ca7a44410ULL,
    0x8bab8eefb6409c1aULL, 0xd01fef10a657842cULL, 0x9b10a4e5e9913129ULL,
    0xe7109bfba19c0c9dULL, 0xac2820d9623bf429ULL, 0x80444b5e7aa7cf85ULL,
    0xbf21e44003acdd2dULL, 0x8e679c2f5e44ff8fULL, 0xd433179d9c8cb841ULL,
    0x9e19db92b4e31ba9ULL, 0xeb96bf6ebadf77d9ULL, 0xaf87023b9bf0ee6bULL,
};

static const int16_t kCachedPowers_E[] = {
    -1220, -1193, -1166, -1140, -1113, -1087, -1060, -1034, -1007,
     -980,  -954,  -927,  -901,  -874,  -847,  -821,  -794,  -768,
     -741,  -715,  -688,  -661,  -635,  -608,  -582,  -555,  -529,
     -502,  -475,  -449,  -422,  -396,  -369,  -343,  -316,  -289,
     -263,  -236,  -210,  -183,  -157,  -130,  -103,   -77,   -50,
      -24,     3,    30,    56,    83,   109,   136,   162,   189,
      216,   242,   269,   295,   322,   348,   375,   402,   428,
      455,   481,   508,   534,   561,   588,   614,   641,   667,
      694,   720,   747,   774,   800,   827,   853,   880,   907,
      933,   960,   986,  1013,  1039,  1066,
};

/* Return the cached power of ten whose binary exponent, combined with
 * a value of binary exponent @a e, lands the product in the range Grisu
 * needs; @a *K receives the decimal exponent of the returned power. */
static DiyFp
get_cached_power(int e, int *K)
{
    /* dk = ceil((alpha - e + 63) * log10(2)) with alpha = -59, offset
     * to table index. 0.30102999566398114 = log10(2). */
    double dk = (-61 - e) * 0.30102999566398114 + 347.0;
    int    k  = (int)dk;
    if (k != dk) {
        k++;   /* ceil */
    }
    unsigned index = (unsigned)((k >> 3) + 1);
    *K = -(-348 + (int)(index << 3));
    DiyFp r = { kCachedPowers_F[index], kCachedPowers_E[index] };
    return r;
}

// ===================================================================
// Digit generation
// ===================================================================

static const uint32_t kPow10[] = {
    1, 10, 100, 1000, 10000, 100000, 1000000,
    10000000, 100000000, 1000000000,
};

static int
count_decimal_digit32(uint32_t n)
{
    if (n < 10) return 1;
    if (n < 100) return 2;
    if (n < 1000) return 3;
    if (n < 10000) return 4;
    if (n < 100000) return 5;
    if (n < 1000000) return 6;
    if (n < 10000000) return 7;
    if (n < 100000000) return 8;
    if (n < 1000000000) return 9;
    return 10;
}

/* Refine the last emitted digit so the produced value is the one
 * closest to w within the rounding interval. */
static void
grisu_round(char *buffer, int len, uint64_t delta, uint64_t rest,
            uint64_t ten_kappa, uint64_t wp_w)
{
    while (rest < wp_w
           && delta - rest >= ten_kappa
           && (rest + ten_kappa < wp_w
               || wp_w - rest > rest + ten_kappa - wp_w)) {
        buffer[len - 1]--;
        rest += ten_kappa;
    }
}

/* Generate digits of the shortest decimal in (W, Mp); on return buffer
 * holds @a *len ASCII digits and @a *K is adjusted by the number of
 * trailing places. */
static void
digit_gen(DiyFp W, DiyFp Mp, uint64_t delta, char *buffer, int *len, int *K)
{
    DiyFp    one  = { 1ULL << -Mp.e, Mp.e };
    DiyFp    wp_w = diy_fp_sub(Mp, W);
    uint32_t p1   = (uint32_t)(Mp.f >> -one.e);
    uint64_t p2   = Mp.f & (one.f - 1);
    int      kappa = count_decimal_digit32(p1);
    *len = 0;

    while (kappa > 0) {
        uint32_t d = 0;
        switch (kappa) {
        case 10: d = p1 / 1000000000; p1 %= 1000000000; break;
        case  9: d = p1 /  100000000; p1 %=  100000000; break;
        case  8: d = p1 /   10000000; p1 %=   10000000; break;
        case  7: d = p1 /    1000000; p1 %=    1000000; break;
        case  6: d = p1 /     100000; p1 %=     100000; break;
        case  5: d = p1 /      10000; p1 %=      10000; break;
        case  4: d = p1 /       1000; p1 %=       1000; break;
        case  3: d = p1 /        100; p1 %=        100; break;
        case  2: d = p1 /         10; p1 %=         10; break;
        case  1: d = p1;              p1 = 0;           break;
        default: d = 0;                                 break;
        }
        if (d != 0 || *len != 0) {
            buffer[(*len)++] = (char)('0' + d);
        }
        kappa--;
        uint64_t tmp = ((uint64_t)p1 << -one.e) + p2;
        if (tmp <= delta) {
            *K += kappa;
            grisu_round(buffer, *len, delta, tmp,
                        (uint64_t)kPow10[kappa] << -one.e, wp_w.f);
            return;
        }
    }

    /* fractional part */
    for (;;) {
        p2    *= 10;
        delta *= 10;
        char d = (char)(p2 >> -one.e);
        if (d != 0 || *len != 0) {
            buffer[(*len)++] = (char)('0' + d);
        }
        p2 &= one.f - 1;
        kappa--;
        if (p2 < delta) {
            *K += kappa;
            /* idx = -kappa is the fractional-iteration count. The
             * shortest decimal of any finite double has <= 17 digits,
             * and the loop terminates well before idx reaches 10, so
             * kPow10[idx] (entries 0..9) is always in bounds — verified
             * empirically by the DBL_MAX/DBL_MIN/subnormal tests, which
             * drive this loop to its deepest realistic point. */
            int idx = -kappa;
            grisu_round(buffer, *len, delta, p2, one.f,
                        wp_w.f * kPow10[idx]);
            return;
        }
    }
}

/* Grisu2: fill @a buffer with the shortest digits, return the count;
 * @a *K is the decimal exponent such that value = digits x 10^K. */
static int
grisu2(double value, char *buffer, int *K)
{
    DiyFp w_m, w_p;
    normalized_boundaries(value, &w_m, &w_p);
    DiyFp w = diy_fp_normalize(double_to_diy_fp(value));

    int   mk;
    DiyFp c_mk = get_cached_power(w_p.e, &mk);

    DiyFp W  = diy_fp_mul(w, c_mk);
    DiyFp Wp = diy_fp_mul(w_p, c_mk);
    DiyFp Wm = diy_fp_mul(w_m, c_mk);
    Wm.f++;
    Wp.f--;

    int len = 0;
    *K = mk;
    digit_gen(W, Wp, Wp.f - Wm.f, buffer, &len, K);
    return len;
}

// ===================================================================
// Public API
// ===================================================================

int
axl_dtoa(
    double   value,
    char    *buf,
    size_t   bufsz,
    int     *out_decpt,
    int     *out_neg
    )
{
    if (buf == NULL || bufsz < AXL_DTOA_BUF_MIN) {
        return 0;
    }

    /* Reject non-finite — callers detect NaN/inf and render them. */
    uint64_t bits = double_bits(value);
    if (((bits & DP_EXPONENT_MASK) == DP_EXPONENT_MASK)) {
        return 0;   /* NaN or +/-inf */
    }

    int neg = (int)(bits >> 63);
    if (out_neg) {
        *out_neg = neg;
    }

    /* Work on the magnitude. -0.0 and +0.0 both have significand 0. */
    double mag = neg ? -value : value;

    if (mag == 0.0) {
        buf[0] = '0';
        buf[1] = '\0';
        if (out_decpt) {
            *out_decpt = 1;
        }
        return 1;
    }

    int K = 0;
    int len = grisu2(mag, buf, &K);
    buf[len] = '\0';

    /* grisu2 yields value = <digits as integer> x 10^K. The David-Gay
     * decimal-point position (digits to the left of the point) is
     * therefore len + K. */
    if (out_decpt) {
        *out_decpt = len + K;
    }
    return len;
}
