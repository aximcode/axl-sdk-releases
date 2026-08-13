/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-strtod.c
    Correctly-rounded decimal string -> double.

    Two tiers. Tier 1 is Clinger's exact fast path: when the mantissa
    fits in 2^53 and |exponent| <= 22, BOTH operands are exactly
    representable, so a single IEEE multiply or divide is correctly
    rounded by the hardware alone. Tier 2 is an exact decimal
    big-number fallback for everything else.

    Deliberately NOT Eisel-Lemire: its cached-power table is ~10KB of
    rodata versus tier 1's 184 bytes, and it buys only speed, which
    nothing in AXL needs at config-value and sensor-reading rates. The
    seam below is built to accept it later behind
    AXL_STRTOD_EISEL_LEMIRE=1; see docs/AXL-Float-Conversion-Design.md.

    Zero-dependency: no axl_mem / axl_str calls (AxlFormat must stay free
    of the Log<->Data cycle), so tier 2 slides its digits with a local
    loop rather than axl_memmove and writes the result bits through a
    union rather than axl_memcpy.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-math.h>
#include <axl/axl-str.h>
#include <axl/axl-types.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/* Clinger's bound. 10^22 = 2^22 * 5^22 and 5^22 ~= 2.4e15 < 2^53, so it is
   exactly representable; 10^23 needs 5^23 ~= 1.19e16 > 2^53 and is not. */
#define CLINGER_MAX_EXP   22

/* 2^53 — above this a u64 mantissa is no longer exactly a double. */
#define MAX_EXACT_MANTISSA  9007199254740992ULL

/* Digits we accumulate before declaring truncation. 19 is the most that
   always fits a u64 (UINT64_MAX has 20 digits but not all 20-digit values
   fit). */
#define MAX_SIG_DIGITS  19

/* Tier 2's decimal big-number capacity. The worst realistic case is a
   ~768-digit significand plus the digits a right shift appends at the far
   end of the exponent range; past that the value is truncated, which the
   rounding rule accounts for exactly. */
#define BIGDEC_CAP  1100

/* Largest binary shift performed in one pass. Both digit loops carry an
   accumulator bounded by 10 << k, so 60 keeps it inside a u64:
   10 * 2^60 = 1.15e19 against UINT64_MAX = 1.84e19. */
#define BIGDEC_MAX_SHIFT  60

/* Decimal-point positions that cannot possibly land in range, checked up
   front so the scaling loops never spin over a hopeless exponent. 10^311 is
   far above DBL_MAX; 10^-331 is far below half the smallest subnormal. */
#define BIGDEC_DP_MAX   310
#define BIGDEC_DP_MIN   (-330)

/* binary64 layout. */
#define F64_MANT_BITS   52
#define F64_EXP_BITS    11
#define F64_EXP_BIAS    (-1023)

/* Biased exponent that means inf/NaN, i.e. the first one that overflows. */
#define F64_EXP_INF     ((1 << F64_EXP_BITS) - 1)

/* Binary shift applied per unit of |dp| when |dp| is at or past the end of
   pow2_steps[]. Under 9 * log2(10) = 29.9, so it never overshoots. */
#define POW2_STEP_FLAT  27

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

/* The lexer's output — a decimal number in the form mantissa * 10^exp.

   CONTRACT for the (mantissa, exp10, truncated) triple: mantissa * 10^exp10
   is the input value rounded toward zero to MAX_SIG_DIGITS significant
   digits, and `truncated` says whether that rounding dropped anything. The
   magnitude is exact even when digits were dropped, so a consumer that can
   handle inexact input — tier 1 declines, but an Eisel-Lemire middle tier
   behind AXL_STRTOD_EISEL_LEMIRE=1 would not — reads the right number. */
typedef struct {
    uint64_t  mantissa;    /* up to MAX_SIG_DIGITS significant digits */
    int32_t   exp10;       /* decimal exponent applied to `mantissa`   */
    bool      negative;
    bool      truncated;   /* digits were dropped: tier 1 must decline */
    bool      is_nan;
    bool      is_inf;
    bool      valid;       /* false = syntax error, nothing consumed   */
    const char *end;       /* first unconsumed byte                    */
} DecNum;

/* Tier 2's working value: an exact decimal held as one digit per byte (0-9,
   not ASCII), most significant first, with the point tracked separately, so
   the value is 0.d[0]d[1]...d[nd-1] * 10^dp. Only assignment and binary
   shifts are needed, and both are exact in decimal because 2 divides 10 —
   which is the whole reason this representation can round correctly where
   double arithmetic cannot. */
typedef struct {
    uint8_t  d[BIGDEC_CAP];
    int32_t  nd;         /* digits held in d[]                           */
    int32_t  dp;         /* decimal exponent: value = 0.d * 10^dp        */
    bool     truncated;  /* a nonzero digit past d[] was dropped         */
} BigDec;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/* Exactly-representable powers of ten, 10^0 .. 10^22. 23 doubles = 184 bytes.
   Every entry is exact in binary64; 10^23 would not be, which is why the
   table stops here rather than at a round number. */
static const double pow10_exact[CLINGER_MAX_EXP + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* Binary shift to apply per unit of |dp| when driving a BigDec into
   [0.5, 1). Each entry is at most floor(i * log2(10)), so a step moves the
   value toward the target range without ever stepping past it. */
static const uint8_t pow2_steps[] = {1, 3, 6, 9, 13, 16, 19, 23, 26};

static bool
is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\v' || c == '\f';
}

static char
lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Case-insensitive match of @a word at @a p. Returns the length matched, or 0. */
static size_t
match_word(const char *p, const char *word)
{
    size_t i;

    for (i = 0; word[i] != '\0'; i++) {
        if (lower(p[i]) != word[i]) {
            return 0;
        }
    }
    return i;
}

/* Parse an optional decimal exponent at *pp. On success advances *pp past
   it and returns the signed value; a malformed exponent is NOT consumed —
   "1e" parses as 1 with endptr at 'e', which is what POSIX strtod does.
   The magnitude is clamped: anything that large saturates to inf or zero
   either way, and clamping keeps the accumulator inside an int32_t. */
static int32_t
parse_exponent(const char **pp)
{
    const char *e;
    bool        negative = false;
    bool        any_digit = false;
    int64_t     v = 0;

    if (**pp != 'e' && **pp != 'E') {
        return 0;
    }
    e = *pp + 1;
    if (*e == '+' || *e == '-') {
        negative = (*e == '-');
        e++;
    }
    for (; *e >= '0' && *e <= '9'; e++) {
        any_digit = true;
        if (v < 100000) {
            v = v * 10 + (*e - '0');
        }
    }
    if (!any_digit) {
        return 0;
    }
    *pp = e;
    return (int32_t)(negative ? -v : v);
}

/* Scan the POSIX strtod grammar (minus hex floats) into a DecNum.
   Consumes nothing and sets .valid = false on a syntax error. */
static DecNum
lex_decimal(const char *nptr)
{
    DecNum      n = {0};
    const char *p = nptr;
    int         ndig = 0;      /* significant digits kept in .mantissa   */
    int32_t     ndrop = 0;     /* digits dropped once precision ran out  */
    int32_t     nfrac = 0;     /* digits after the '.'                   */
    bool        saw_dot = false;
    bool        any_digit = false;

    n.end = nptr;

    while (is_space(*p)) {
        p++;
    }
    if (*p == '+' || *p == '-') {
        n.negative = (*p == '-');
        p++;
    }

    /* Specials first — they are words, not digits. */
    {
        size_t m = match_word(p, "infinity");
        if (m == 0) {
            m = match_word(p, "inf");
        }
        if (m != 0) {
            n.is_inf = true;
            n.valid  = true;
            n.end    = p + m;
            return n;
        }
        m = match_word(p, "nan");
        if (m != 0) {
            n.is_nan = true;
            n.valid  = true;
            n.end    = p + m;
            return n;
        }
    }

    for (; ; p++) {
        if (*p >= '0' && *p <= '9') {
            any_digit = true;
            if (saw_dot) {
                nfrac++;
            }
            if (ndig < MAX_SIG_DIGITS) {
                /* Leading zeros are not significant. */
                if (!(n.mantissa == 0 && *p == '0')) {
                    n.mantissa = n.mantissa * 10u + (uint64_t)(*p - '0');
                    ndig++;
                }
            } else {
                /* Past our precision: remember, and keep the magnitude. */
                n.truncated = true;
                ndrop++;
            }
        } else if (*p == '.' && !saw_dot) {
            saw_dot = true;
        } else {
            break;
        }
    }
    if (!any_digit) {
        return n;   /* .valid stays false */
    }

    /* Each dropped digit scales .mantissa up by ten, each fractional digit
       KEPT scales it down. Dropped fractional digits are in both counts and
       cancel, which is what makes this ndrop - nfrac rather than two terms
       that have to know which side of the '.' each dropped digit fell on:
         dropped_int - kept_frac
           = (ndrop - dropped_frac) - (nfrac - dropped_frac)
           = ndrop - nfrac. */
    n.exp10 = ndrop - nfrac;

    n.exp10 += parse_exponent(&p);

    n.valid = true;
    n.end   = p;
    return n;
}

/* Drop trailing zeros. They carry no information — the point is tracked
   independently — and several steps below rely on d[nd-1] being nonzero. */
static void
bigdec_trim(BigDec *b)
{
    while (b->nd > 0 && b->d[b->nd - 1] == 0) {
        b->nd--;
    }
    if (b->nd == 0) {
        b->dp = 0;
    }
}

/* Store one digit, recording truncation instead of overflowing d[]. */
static void
bigdec_put(BigDec *b, int32_t i, uint8_t dig)
{
    if (i < BIGDEC_CAP) {
        b->d[i] = dig;
    } else if (dig != 0) {
        b->truncated = true;
    }
}

/* Re-lex the significand at full precision. DecNum.mantissa has already
   truncated to MAX_SIG_DIGITS, so tier 2 must read the original text; the
   span is bounded by the lexer's endpoint so the two can never disagree
   about where the number stops. */
static void
bigdec_from_text(BigDec *b, const char *nptr, const char *end)
{
    const char *p = nptr;
    int32_t     nint = 0;    /* digits before the '.', significant or not  */
    int32_t     nlead = 0;   /* zeros before the first significant digit   */
    bool        saw_dot = false;
    bool        saw_sig = false;

    b->nd        = 0;
    b->dp        = 0;
    b->truncated = false;

    while (is_space(*p)) {
        p++;
    }
    if (*p == '+' || *p == '-') {
        p++;
    }
    for (; p < end; p++) {
        if (*p == '.' && !saw_dot) {
            saw_dot = true;
            continue;
        }
        if (*p < '0' || *p > '9') {
            break;
        }
        if (!saw_dot) {
            nint++;
        }
        if (!saw_sig) {
            if (*p == '0') {
                nlead++;
                continue;
            }
            saw_sig = true;
        }
        if (b->nd < BIGDEC_CAP) {
            b->d[b->nd] = (uint8_t)(*p - '0');
            b->nd++;
        } else if (*p != '0') {
            b->truncated = true;
        }
    }

    /* value = 0.d * 10^dp, so the point sits after the integer digits, less
       the leading zeros that were never stored. Both counts are uncapped, so
       a significand longer than d[] still keeps its magnitude. */
    b->dp = nint - nlead + parse_exponent(&p);
    bigdec_trim(b);
}

/* Divide by 2^k, exactly. k must be 1..BIGDEC_MAX_SHIFT. */
static void
bigdec_shift_right(BigDec *b, uint32_t k)
{
    const uint64_t mask = ((uint64_t)1 << k) - 1u;
    int32_t        r = 0;   /* read index  */
    int32_t        w = 0;   /* write index */
    uint64_t       n = 0;

    /* Pull in leading digits until there is enough to shift a digit out. */
    for (; (n >> k) == 0; r++) {
        if (r >= b->nd) {
            if (n == 0) {
                b->nd = 0;      /* value is zero; nothing to shift */
                b->dp = 0;
                return;
            }
            while ((n >> k) == 0) {
                n *= 10u;
                r++;
            }
            break;
        }
        n = n * 10u + b->d[r];
    }
    b->dp -= r - 1;

    /* Pick up a digit, put down a digit. w trails r, so this is in place. */
    for (; r < b->nd; r++) {
        uint64_t c = b->d[r];
        b->d[w] = (uint8_t)(n >> k);
        w++;
        n = (n & mask) * 10u + c;
    }

    /* Then drain whatever the remainder still has to give. */
    while (n > 0) {
        uint8_t dig = (uint8_t)(n >> k);
        n = (n & mask) * 10u;
        bigdec_put(b, w, dig);
        if (w < BIGDEC_CAP) {
            w++;
        }
    }

    b->nd = w;
    bigdec_trim(b);
}

/* Multiply by 2^k, exactly. k must be 1..BIGDEC_MAX_SHIFT. */
static void
bigdec_shift_left(BigDec *b, uint32_t k)
{
    int32_t  delta;
    int32_t  r;
    int32_t  w;
    int32_t  lead;
    int32_t  i;
    uint64_t n = 0;

    /* v < 10^nd, so v * 2^k < 10^(nd + k*log10(2)): the product needs at
       most `delta` more digits than v, where 30103/100000 is log10(2) to
       five places and the +1 makes it a ceiling for every k >= 1. Writing
       into exactly nd + delta slots therefore cannot lose a leading digit,
       which is why no cached-power table is needed here. */
    delta = (int32_t)((k * 30103u) / 100000u) + 1;

    /* Work from the least significant end so every destination slot
       (r + delta, always ahead of r) has already been read. */
    w = b->nd + delta;
    for (r = b->nd - 1; r >= 0; r--) {
        n += (uint64_t)b->d[r] << k;
        w--;
        bigdec_put(b, w, (uint8_t)(n % 10u));
        n /= 10u;
    }
    while (w > 0) {
        w--;
        bigdec_put(b, w, (uint8_t)(n % 10u));
        n /= 10u;
    }

    b->nd += delta;
    if (b->nd > BIGDEC_CAP) {
        b->nd = BIGDEC_CAP;
    }
    b->dp += delta;

    /* delta is an upper bound on the digits gained, so the result may be one
       digit shorter than the space reserved for it. Slide any leading zero
       out with a local loop rather than axl_memmove: AxlFormat must stay
       free of the Log<->Data cycle (see axl-dtoa.c). */
    lead = 0;
    while (lead < b->nd && b->d[lead] == 0) {
        lead++;
    }
    if (lead > 0) {
        for (i = lead; i < b->nd; i++) {
            b->d[i - lead] = b->d[i];
        }
        b->nd -= lead;
        b->dp -= lead;
    }
    bigdec_trim(b);
}

/* Multiply (k > 0) or divide (k < 0) by 2^|k|, in passes small enough that
   the digit accumulators stay inside a u64. */
static void
bigdec_shift(BigDec *b, int32_t k)
{
    while (b->nd > 0 && k > BIGDEC_MAX_SHIFT) {
        bigdec_shift_left(b, BIGDEC_MAX_SHIFT);
        k -= BIGDEC_MAX_SHIFT;
    }
    while (b->nd > 0 && k < -BIGDEC_MAX_SHIFT) {
        bigdec_shift_right(b, BIGDEC_MAX_SHIFT);
        k += BIGDEC_MAX_SHIFT;
    }
    if (b->nd == 0) {
        return;
    }
    if (k > 0) {
        bigdec_shift_left(b, (uint32_t)k);
    } else if (k < 0) {
        bigdec_shift_right(b, (uint32_t)-k);
    }
}

/* Would chopping to @a nd digits round up? This is the ONE rounding
   decision in tier 2, and it is taken on the full remaining decimal. */
static bool
bigdec_should_round_up(const BigDec *b, int32_t nd)
{
    if (nd < 0 || nd >= b->nd) {
        return false;
    }
    if (b->d[nd] == 5 && nd + 1 == b->nd) {
        /* Exactly halfway on what is held. Digits dropped past d[] make the
           true value strictly larger, so they settle the tie upward;
           otherwise it is a genuine tie and goes to even. */
        if (b->truncated) {
            return true;
        }
        return nd > 0 && (b->d[nd - 1] % 2) != 0;
    }
    return b->d[nd] >= 5;
}

/* The integer part, rounded. dp cannot exceed 16 here and so always fits a
   u64: the caller's second scaling loop only exits with the value in
   [0.5, 1), i.e. dp == 0, and the subsequent shift by F64_MANT_BITS + 1
   multiplies by 2^53, landing the value below 2^53 < 10^16. Subnormals are
   shifted down further still, so dp only gets smaller. */
static uint64_t
bigdec_rounded_integer(const BigDec *b)
{
    int32_t  i;
    uint64_t n = 0;

    for (i = 0; i < b->dp && i < b->nd; i++) {
        n = n * 10u + b->d[i];
    }
    for (; i < b->dp; i++) {
        n *= 10u;
    }
    if (bigdec_should_round_up(b, b->dp)) {
        n++;
    }
    return n;
}

/* Binary shift to take one step of |dp| toward the target range. */
static int32_t
pow2_step(int32_t dp_abs)
{
    const int32_t n = (int32_t)(sizeof(pow2_steps) / sizeof(pow2_steps[0]));

    if (dp_abs >= n) {
        return POW2_STEP_FLAT;
    }
    return pow2_steps[dp_abs];
}

/* True once the unbiased exponent has run past the largest finite one. */
static bool
f64_exp_overflows(int32_t exp)
{
    return (exp - F64_EXP_BIAS) >= F64_EXP_INF;
}

/* Tier 2. Returns the correctly-rounded magnitude; the caller applies the
   sign. Declared ahead of tier 1 so tier 1 can fall through to it. */
static double strtod_slow(const char *nptr, const DecNum *n,
                          bool *out_range_err);

/* Clinger's exact fast path. Returns false if this input is outside the
   provably-exact range and must go to tier 2. */
static bool
strtod_clinger(const DecNum *n, double *out)
{
    double m;

    if (n->truncated || n->mantissa > MAX_EXACT_MANTISSA) {
        return false;
    }
    if (n->exp10 > CLINGER_MAX_EXP || n->exp10 < -CLINGER_MAX_EXP) {
        return false;
    }

    m = (double)n->mantissa;
    /* ONE rounding operation, on two exactly-representable operands, is
       correctly rounded by IEEE 754. Two would not be. */
    if (n->exp10 >= 0) {
        m = m * pow10_exact[n->exp10];
    } else {
        m = m / pow10_exact[-n->exp10];
    }
    *out = m;
    return true;
}

/* The exact decimal fallback. Everything tier 1 declines lands here:
   mantissas past 2^53, |exponent| past 22, subnormals, and inputs longer
   than a u64 can hold. The value is carried as an exact decimal, shifted by
   powers of two until it sits in [2^52, 2^53), and rounded exactly once —
   so it is correctly rounded by construction, with no power table and no
   double arithmetic anywhere in the decision. */
static double
strtod_slow(const char *nptr, const DecNum *n, bool *out_range_err)
{
    union { double d; uint64_t u; } cvt;
    BigDec   b;
    uint64_t mant;
    int32_t  exp;

    *out_range_err = false;
    bigdec_from_text(&b, nptr, n->end);

    if (b.nd == 0) {
        return 0.0;                 /* a genuine zero, not an underflow */
    }
    if (b.dp > BIGDEC_DP_MAX) {
        *out_range_err = true;
        return AXL_MATH_INF;
    }
    if (b.dp < BIGDEC_DP_MIN) {
        *out_range_err = true;
        return 0.0;
    }

    /* Scale by powers of two until the value lands in [0.5, 1). */
    exp = 0;
    while (b.dp > 0) {
        int32_t s = pow2_step(b.dp);
        bigdec_shift(&b, -s);
        exp += s;
    }
    while (b.dp < 0 || (b.dp == 0 && b.d[0] < 5)) {
        int32_t s = pow2_step(-b.dp);
        bigdec_shift(&b, s);
        exp -= s;
    }
    exp--;      /* [0.5, 1) is one binade below IEEE's [1, 2) */

    /* Below the smallest normal the exponent is pinned and the mantissa
       gives up bits instead — that is what makes subnormals gradual. */
    if (exp < F64_EXP_BIAS + 1) {
        int32_t s = F64_EXP_BIAS + 1 - exp;
        bigdec_shift(&b, -s);
        exp += s;
    }
    if (f64_exp_overflows(exp)) {
        *out_range_err = true;
        return AXL_MATH_INF;
    }

    /* Line the mantissa up as an integer, then take the single rounding. */
    bigdec_shift(&b, F64_MANT_BITS + 1);
    mant = bigdec_rounded_integer(&b);
    if (mant == ((uint64_t)2 << F64_MANT_BITS)) {
        mant >>= 1;     /* rounding carried into a new binade */
        exp++;
        if (f64_exp_overflows(exp)) {
            *out_range_err = true;
            return AXL_MATH_INF;
        }
    }
    if (mant == 0) {
        *out_range_err = true;      /* rounded away below the smallest */
        return 0.0;
    }
    if ((mant & ((uint64_t)1 << F64_MANT_BITS)) == 0) {
        exp = F64_EXP_BIAS;         /* subnormal: no implicit leading 1 */
    }

    /* No mask on the exponent field: every path to here has cleared
       f64_exp_overflows, and a subnormal pinned exp to the bias, so
       exp - F64_EXP_BIAS is already inside [0, F64_EXP_INF - 1]. */
    cvt.u = (mant & (((uint64_t)1 << F64_MANT_BITS) - 1u))
          | ((uint64_t)(uint32_t)(exp - F64_EXP_BIAS) << F64_MANT_BITS);
    return cvt.d;
}

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

int
axl_str_to_double(const char *nptr, double *out, const char **endptr)
{
    DecNum  n;
    double  v;
    bool    range_err = false;

    if (endptr != NULL) {
        *endptr = nptr;
    }
    if (nptr == NULL || out == NULL) {
        return AXL_ERR;
    }

    n = lex_decimal(nptr);
    if (!n.valid) {
        return AXL_ERR;   /* *out untouched, *endptr already == nptr */
    }
    if (endptr != NULL) {
        *endptr = n.end;
    } else if (*n.end != '\0') {
        /* Strict mode: with no endptr the entire input must be consumed,
           exactly as the eight integer parsers require -- a caller who
           wants partial parsing asks for it by passing endptr. Leading
           whitespace was already skipped by the lexer, but TRAILING
           whitespace is trailing content and fails here, matching
           axl_str_to_u64("123 ", 10, &v, NULL).

           Placed before the value is written so a strict failure leaves
           *out untouched, which is the syntax-error rule. That ordering
           IS the precedence: trailing bytes outrank the range-error rule
           below, so "1e400xyz" writes nothing even though "1e400" alone
           would have written +infinity. */
        return AXL_ERR;
    }

    if (n.is_nan) {
        *out = AXL_MATH_NAN;
        return AXL_OK;
    }
    if (n.is_inf) {
        *out = n.negative ? -AXL_MATH_INF : AXL_MATH_INF;
        return AXL_OK;
    }

    if (!strtod_clinger(&n, &v)) {
        v = strtod_slow(nptr, &n, &range_err);
    }
    *out = n.negative ? -v : v;

    /* Overflow/underflow write the IEEE result AND report the range error. */
    return range_err ? AXL_ERR : AXL_OK;
}

int
axl_str_to_float(const char *nptr, float *out, const char **endptr)
{
    const char *local_end;
    double      d;
    float       f;
    int         rc;

    if (endptr != NULL) {
        *endptr = nptr;
    }
    if (out == NULL) {
        return AXL_ERR;
    }

    rc = axl_str_to_double(nptr, &d, &local_end);
    if (endptr != NULL) {
        *endptr = local_end;
    }
    if (local_end == nptr) {
        return AXL_ERR;   /* syntax error (or NULL nptr): *out untouched */
    }
    if (endptr == NULL && *local_end != '\0') {
        /* Strict mode. The inner call always passes an endptr, so its own
           strict check never fires and this is the only one -- which is
           what keeps the two functions' rule identical. Same precedence:
           before any *out write, so trailing bytes outrank the
           range-error rule for the float form too. The NULL-nptr case
           has already returned above, so local_end is safe to
           dereference here. */
        return AXL_ERR;
    }

    f = (float)d;

    /* A double-range error (e.g. "1e400") is a float-range error too --
       the saturated +/-inf or +/-0.0 narrows to the same saturated value. */
    if (rc == AXL_ERR) {
        *out = f;
        return AXL_ERR;
    }

    /* Narrowing is a SECOND rounding: a finite double can become +/-inf
       (float overflow) or +/-0.0 (float underflow) even though the
       double form parsed it cleanly. "nan"/"inf" in the input are
       values, so axl_isfinite(d) guards the overflow check, and the
       d != 0.0 guard keeps a literal "0" from tripping underflow. */
    if (axl_isfinite(d) && axl_isinf((double)f)) {
        *out = f;
        return AXL_ERR;
    }
    if (d != 0.0 && f == 0.0f) {
        *out = f;
        return AXL_ERR;
    }

    *out = f;
    return AXL_OK;
}
