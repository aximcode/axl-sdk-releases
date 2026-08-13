# AXL Float Conversion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give AXL a correctly-rounded `string ↔ double` conversion pair, plus the
missing integer reverse, so every numeric conversion in `axl-str.h` is symmetric.

**Architecture:** Two-tier parser — Clinger's exact fast path (hardware FP, 23-entry
table, provably correct for mantissa ≤ 2^53 and |exp| ≤ 22) with an exact decimal
big-number fallback for everything else, so every input is correctly rounded. The
print half wraps the existing Grisu2 `axl_dtoa`. Eisel–Lemire is deliberately not
built; see the design doc §4.

**Tech Stack:** C (gnu2x), freestanding UEFI — no libc, no libm, no locale. GNU Make.
Tests are `test_check` assertions in `test/unit/`, run in QEMU on both arches via
`test/integration/test-axl.sh`.

**Design doc (the contract):** `docs/AXL-Float-Conversion-Design.md`, status APPROVED.

## Global Constraints

- **Test-first, always.** Write the test, run it, SEE IT FAIL, then implement. Bucket A
  (new public API) in `CLAUDE.md`: header + docstring first, then failing tests.
- **Exact-value assertions.** `axl_strcmp(buf, "...") == 0` and exact bit comparisons.
  Never `axl_strstr`. Never `test_check(true, ...)` — there is a gate against it.
- **Both arches:** `./test/integration/test-axl.sh` and
  `./test/integration/test-axl.sh --arch AARCH64`. **`--arch AARCH64`, NOT `ARCH=aa64`**
  — the latter silently runs X64.
- **Gates before every commit:** `make check-ascii check-docs check-dogfood`,
  `./scripts/lint.sh`, `./scripts/build-docs.sh`. (`make check-tautology` does NOT
  exist — do not run it.)
- **Style:** `docs/AXL-Coding-Style.md`. `axl_snake_case`, 4-space indent, K&R braces,
  no space before parens, multi-line function signatures even for one parameter,
  `///<` inline param docs, `@brief`/`@return` block comments. Standard C types only in
  public API. No lines containing only whitespace.
- **`axl_free` is a macro** — pass `axl_free_impl` where a function pointer is needed.
- **Never `git add -A`** in this repo; there are long-standing untracked files. Stage
  named paths only, and run `git status` first.
- **Do not touch `.claude/worktrees/json-flag-redesign`** — another session owns it.
- **No `axl_json_get_double`.** JSON's surface belongs to that session; out of scope.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/axl/axl-math.h` | *Modify.* nan/inf constants + predicates; accuracy-contract doc block. |
| `src/math/axl-math.c` | *Modify.* Predicate implementations. |
| `src/format/axl-format.c` | *Modify.* Retrofit two hand-rolled nan/inf checks onto the new predicates. |
| `include/axl/axl-format.h` | *Modify.* `axl_dtoa` docstring: point at the predicates instead of telling callers to hand-roll. |
| `src/format/axl-strtod.c` | **Create.** The parser: lexer, Clinger tier 1, exact decimal tier 2. Lives beside `axl-dtoa.c` because the two halves mirror each other. |
| `include/axl/axl-str.h` | *Modify.* Declarations for all six new conversion functions. |
| `src/data/axl-str.c` | *Modify.* `axl_double_to_str` / `axl_float_to_str` / `axl_u64_to_str` / `axl_s64_to_str` — thin, next to the existing `axl_str_to_*` family. |
| `src/format/axl-scanf.c` | *Modify.* `%f/%e/%g/%E/%G` conversions. (If scanf lives elsewhere, find it with `grep -rn axl_vsscanf src/`.) |
| `test/unit/axl-test-string.c` | *Modify.* All new tests; the `axl_str_to_u64` family already lives here. |
| `Makefile` | *Modify.* Add `src/format/axl-strtod.c` to the format source list (~line 272). |

---

## Task 1: axl-math nan/inf surface

**Files:**
- Modify: `include/axl/axl-math.h`
- Modify: `src/math/axl-math.c` (confirm path: `grep -rn "axl_floor" src/ --include=*.c`)
- Modify: `src/format/axl-format.c:270` and `:285`
- Modify: `include/axl/axl-format.h` (the `axl_dtoa` docstring, ~line 124)
- Test: `test/unit/axl-test-string.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `AXL_MATH_INF`, `AXL_MATH_NAN`, `AXL_MATH_DBL_MAX`, `AXL_MATH_DBL_TRUE_MIN`,
  `bool axl_isnan(double)`, `bool axl_isinf(double)`, `bool axl_isfinite(double)`.
  Every later task uses these instead of hand-rolled checks.

- [ ] **Step 1: Add the header contract first**

In `include/axl/axl-math.h`, after the existing constants block:

```c
// ===================================================================
// IEEE-754 special values
// ===================================================================

/// Positive infinity. Built by division rather than a literal — the
/// freestanding build has no <float.h> and no INFINITY macro.
#define AXL_MATH_INF            (1.0 / 0.0)

/// A quiet NaN. Note NaN != NaN, so never compare against this;
/// use axl_isnan().
#define AXL_MATH_NAN            (0.0 / 0.0)

/// Largest finite double, 2^1024 - 2^971.
#define AXL_MATH_DBL_MAX        1.7976931348623157e308

/// Smallest positive SUBNORMAL double, 2^-1074.
#define AXL_MATH_DBL_TRUE_MIN   4.9406564584124654e-324

/**
 * @brief True if @a x is NaN (Not a Number).
 *
 * NaN is the only value that compares unequal to itself, which is
 * exactly how this is implemented — no libm, no bit inspection.
 *
 * @return true if @a x is NaN.
 */
bool
axl_isnan(
    double  x  ///< value to test
);

/**
 * @brief True if @a x is positive or negative infinity.
 *
 * @return true if @a x is +inf or -inf.
 */
bool
axl_isinf(
    double  x  ///< value to test
);

/**
 * @brief True if @a x is neither NaN nor infinite.
 *
 * @return true if @a x is a finite number (including zero and subnormals).
 */
bool
axl_isfinite(
    double  x  ///< value to test
);
```

- [ ] **Step 2: Add the accuracy-contract doc block**

Still in `include/axl/axl-math.h`, extend the `@file` block (after the existing
"Accuracy is sufficient for UI coordinates and animation easing" sentence):

```c
    ACCURACY CONTRACT — read before mixing this module with the
    conversion APIs.  AxlMath is deliberately APPROXIMATE: its
    results are good to roughly double precision for non-extreme
    inputs, which is right for UI coordinates and easing curves and
    wrong for anything that must round-trip.  The string/number
    conversions in <axl/axl-str.h> (axl_str_to_double,
    axl_double_to_str) are by contrast CORRECTLY ROUNDED and
    bit-exact.  Scaling a value you parsed exactly by axl_pow(10, n)
    silently discards the exactness you just paid for; use the
    conversion APIs for serialization and this module for geometry.
```

- [ ] **Step 3: Write the failing tests**

In `test/unit/axl-test-string.c`, add a new function and register it in the file's
`main`-equivalent runner alongside the other `test_*` calls:

```c
static void
test_math_special_values(void)
{
    double inf  = AXL_MATH_INF;
    double nan  = AXL_MATH_NAN;

    test_check(axl_isnan(nan), "isnan: NaN is NaN");
    test_check(!axl_isnan(0.0), "isnan: zero is not NaN");
    test_check(!axl_isnan(inf), "isnan: infinity is not NaN");
    test_check(!axl_isnan(AXL_MATH_DBL_MAX), "isnan: DBL_MAX is not NaN");

    test_check(axl_isinf(inf), "isinf: +inf is infinite");
    test_check(axl_isinf(-inf), "isinf: -inf is infinite");
    test_check(!axl_isinf(nan), "isinf: NaN is not infinite");
    test_check(!axl_isinf(AXL_MATH_DBL_MAX), "isinf: DBL_MAX is finite");

    test_check(axl_isfinite(0.0), "isfinite: zero is finite");
    test_check(axl_isfinite(AXL_MATH_DBL_MAX), "isfinite: DBL_MAX is finite");
    test_check(axl_isfinite(AXL_MATH_DBL_TRUE_MIN),
               "isfinite: smallest subnormal is finite");
    test_check(!axl_isfinite(inf), "isfinite: +inf is not finite");
    test_check(!axl_isfinite(-inf), "isfinite: -inf is not finite");
    test_check(!axl_isfinite(nan), "isfinite: NaN is not finite");

    /* The constants must be the values they claim. DBL_TRUE_MIN is
       subnormal, so halving it reaches zero and doubling recovers it —
       that pins it as the SMALLEST, not merely a small, double. */
    test_check(AXL_MATH_DBL_TRUE_MIN / 2.0 == 0.0,
               "DBL_TRUE_MIN: halving underflows to zero");
    test_check(AXL_MATH_DBL_MAX * 2.0 == inf,
               "DBL_MAX: doubling overflows to infinity");
}
```

- [ ] **Step 4: Run and verify it FAILS**

```bash
make -j8 tests && TEST_APPS_ONLY=AxlTestString TEST_SKIP_RATCHET=1 \
  timeout 300 ./test/integration/test-axl.sh 2>&1 | grep -E "isnan|isinf|isfinite|DBL_|Results"
```
Expected: a compile/link error for the undefined `axl_isnan`. That is the RED.

- [ ] **Step 5: Implement**

In `src/math/axl-math.c`:

```c
bool
axl_isnan(double x)
{
    return x != x;   /* the only value unequal to itself */
}

bool
axl_isinf(double x)
{
    return !axl_isnan(x) && (x > AXL_MATH_DBL_MAX || x < -AXL_MATH_DBL_MAX);
}

bool
axl_isfinite(double x)
{
    return !axl_isnan(x) && !axl_isinf(x);
}
```

- [ ] **Step 6: Run and verify PASS**

Same command as Step 4. Expected: all 15 assertions PASS.

- [ ] **Step 7: Retrofit the hand-rolled checks (dogfooding)**

`src/format/axl-format.c:270` — replace `if (value != value) {` with
`if (axl_isnan(value)) {`. Line ~285 — replace
`if (value > 1.7976931348623157e308) {` with `if (axl_isinf(value)) {`.
Add `#include <axl/axl-math.h>` if absent.

**Careful:** at `:285` the sign has already been stripped into `sign` and `value`
made positive, so `axl_isinf` is equivalent there. Verify by reading the
surrounding lines before editing.

Then in `include/axl/axl-format.h` (~line 124), replace the sentence telling callers
to hand-roll:

```c
 * @p value MUST be finite. NaN and +/-infinity are NOT handled here —
 * test with axl_isfinite() from <axl/axl-math.h> first; passing one
 * returns 0.
```

- [ ] **Step 8: Full gates, both arches**

```bash
make -j8 tests
timeout 900 ./test/integration/test-axl.sh          | tail -2
timeout 1500 ./test/integration/test-axl.sh --arch AARCH64 | tail -2
make check-ascii check-docs check-dogfood
./scripts/lint.sh | tail -1
./scripts/build-docs.sh | tail -2
```
Expected: both arches 0 failed, every gate clean.

- [ ] **Step 9: Commit**

```bash
git status
git add include/axl/axl-math.h src/math/axl-math.c src/format/axl-format.c \
        include/axl/axl-format.h test/unit/axl-test-string.c \
        test/integration/.last-pass-count
git commit -m "math: add the IEEE special-value surface (nan/inf predicates)

axl-math had zero nan/inf predicates or constants, so axl-format.c
hand-rolled 'value != value' and compared against a literal
1.7976931348623157e308, and axl_dtoa's public docstring instructed
consumers to do the same. Adds axl_isnan/isinf/isfinite plus
AXL_MATH_INF/NAN/DBL_MAX/DBL_TRUE_MIN, and retrofits both hand-rolled
sites so internals dogfood the public API.

Also records the accuracy contract: AxlMath is deliberately approximate
(UI coordinates, easing), while the string conversions landing next are
correctly rounded. Scaling a parsed value by axl_pow(10, n) throws away
the exactness -- nothing said so before."
```

---

## Task 2: the parser — lexer + Clinger tier 1

**Files:**
- Create: `src/format/axl-strtod.c`
- Modify: `include/axl/axl-str.h` (declarations, in the "Number parsing" section ~line 995)
- Modify: `Makefile` (~line 272, beside `src/format/axl-dtoa.c`)
- Test: `test/unit/axl-test-string.c`

**Interfaces:**
- Consumes: `axl_isnan`/`axl_isinf`/`AXL_MATH_INF`/`AXL_MATH_NAN` from Task 1.
- Produces: `int axl_str_to_double(const char *nptr, double *out, const char **endptr)`.
  Task 3 round-trips against it; Task 5 calls it from scanf.
  Also the file-internal `static bool strtod_slow(...)` seam that Task 2b fills.

- [ ] **Step 1: Header contract first**

In `include/axl/axl-str.h`, in the "Number parsing" section:

```c
/**
 * @brief Parse a decimal floating-point number, correctly rounded.
 *
 * Skips leading whitespace, then accepts an optional sign, decimal
 * digits with an optional '.', and an optional 'e'/'E' exponent with
 * its own optional sign. Also accepts "nan", "inf" and "infinity",
 * case-insensitively and sign-aware, matching POSIX strtod and
 * g_ascii_strtod. Hex float literals (0x1.8p3) are NOT accepted.
 *
 * The result is CORRECTLY ROUNDED: it is the double nearest the exact
 * decimal value, ties to even. That makes it the exact inverse of
 * axl_double_to_str() — parse(print(x)) reproduces x bit-for-bit for
 * every finite double.
 *
 * There is no locale to worry about: the freestanding build has none,
 * so the decimal separator is always '.'. Do not add a locale hook.
 *
 * RANGE ERRORS STILL WRITE @p out, unlike the integer members of this
 * family. Overflow yields +/-infinity and underflow yields +/-0.0 —
 * the correct IEEE results — together with AXL_ERR, because with no
 * errno that is the only way to give the caller both the value and the
 * fact that it saturated. A SYNTAX error leaves @p out untouched.
 *
 * "nan" and "inf" in the INPUT are values, not errors: AXL_OK.
 *
 * @return AXL_OK on success, AXL_ERR on syntax error or range error.
 */
int
axl_str_to_double(
    const char  *nptr,    ///< number string
    double      *out,     ///< [out] parsed value
    const char **endptr   ///< [out, optional] past last consumed char
);
```

- [ ] **Step 2: Write the failing tests — tier 1 range only**

Add to `test/unit/axl-test-string.c` and register the function in the runner:

```c
static void
test_str_to_double_basic(void)
{
    double      d;
    const char *end;

    /* Tier 1: mantissa <= 2^53 and |exp| <= 22, exactly representable. */
    test_check(axl_str_to_double("0", &d, NULL) == AXL_OK && d == 0.0,
               "str_to_double: zero");
    test_check(axl_str_to_double("1", &d, NULL) == AXL_OK && d == 1.0,
               "str_to_double: one");
    test_check(axl_str_to_double("-1", &d, NULL) == AXL_OK && d == -1.0,
               "str_to_double: negative");
    test_check(axl_str_to_double("1.5", &d, NULL) == AXL_OK && d == 1.5,
               "str_to_double: 1.5 is exact in binary");
    test_check(axl_str_to_double("36.6", &d, NULL) == AXL_OK && d == 36.6,
               "str_to_double: a sensor-style reading");
    test_check(axl_str_to_double("0.001", &d, NULL) == AXL_OK && d == 0.001,
               "str_to_double: leading zeros after the point");
    test_check(axl_str_to_double("1e10", &d, NULL) == AXL_OK && d == 1e10,
               "str_to_double: positive exponent");
    test_check(axl_str_to_double("1E10", &d, NULL) == AXL_OK && d == 1e10,
               "str_to_double: capital E exponent");
    test_check(axl_str_to_double("1e-10", &d, NULL) == AXL_OK && d == 1e-10,
               "str_to_double: negative exponent");
    test_check(axl_str_to_double("+2.5e+2", &d, NULL) == AXL_OK && d == 250.0,
               "str_to_double: explicit + on both mantissa and exponent");
    test_check(axl_str_to_double("  \t 7.25", &d, NULL) == AXL_OK && d == 7.25,
               "str_to_double: leading whitespace is skipped");

    /* endptr lands just past what was consumed. */
    test_check(axl_str_to_double("3.5abc", &d, &end) == AXL_OK
               && d == 3.5 && axl_strcmp(end, "abc") == 0,
               "str_to_double: endptr stops at the first unconsumed byte");

    /* Syntax errors leave *out untouched and reset endptr to nptr. */
    d = 99.0;
    const char *src = "abc";
    test_check(axl_str_to_double(src, &d, &end) == AXL_ERR
               && d == 99.0 && end == src,
               "str_to_double: syntax error leaves out untouched, endptr = nptr");
    d = 99.0;
    test_check(axl_str_to_double("", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: empty string is a syntax error");
    d = 99.0;
    test_check(axl_str_to_double("e5", &d, NULL) == AXL_ERR && d == 99.0,
               "str_to_double: exponent with no mantissa is a syntax error");
    test_check(axl_str_to_double(NULL, &d, NULL) == AXL_ERR,
               "str_to_double: NULL input is rejected");
    test_check(axl_str_to_double("1.0", NULL, NULL) == AXL_ERR,
               "str_to_double: NULL out is rejected");

    /* Specials are VALUES, so AXL_OK. */
    test_check(axl_str_to_double("nan", &d, NULL) == AXL_OK && axl_isnan(d),
               "str_to_double: nan");
    test_check(axl_str_to_double("NaN", &d, NULL) == AXL_OK && axl_isnan(d),
               "str_to_double: NaN is case-insensitive");
    test_check(axl_str_to_double("inf", &d, NULL) == AXL_OK
               && axl_isinf(d) && d > 0,
               "str_to_double: inf");
    test_check(axl_str_to_double("-INFINITY", &d, NULL) == AXL_OK
               && axl_isinf(d) && d < 0,
               "str_to_double: -INFINITY, long spelling, case-insensitive");

    /* Range errors are TIER 2 behaviour and are asserted in Task 2b, not
       here: they route to the tier-2 stub, so asserting them now would
       commit a knowingly-red suite and trip the pass-count ratchet. */

    /* Hex floats are NOT the hex-float grammar: "0x1.8p3" parses as 0
       and stops at 'x'. Pinning this stops someone "helpfully" adding
       partial hex support later. */
    test_check(axl_str_to_double("0x1.8p3", &d, &end) == AXL_OK
               && d == 0.0 && axl_strcmp(end, "x1.8p3") == 0,
               "str_to_double: hex float is not accepted; stops after the 0");
}
```

- [ ] **Step 3: Run and verify it FAILS**

```bash
make -j8 tests 2>&1 | tail -3
```
Expected: undefined reference to `axl_str_to_double`. That is the RED.

- [ ] **Step 4: Create the parser skeleton with tier 1**

Create `src/format/axl-strtod.c`. Structure per `AXL-Coding-Style.md`: file header,
includes, macros, types, statics, then public functions.

```c
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

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

/* The lexer's output — a decimal number in the form mantissa * 10^exp. */
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

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/* Exactly-representable powers of ten, 10^0 .. 10^22. 23 doubles = 184 bytes.
   Every entry is exact in binary64; 10^23 would not be, which is why the
   table stops here rather than at a round number. */
static const double kPow10Exact[CLINGER_MAX_EXP + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

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
```

- [ ] **Step 5: Add the lexer**

Append to `src/format/axl-strtod.c`:

```c
/* Scan the POSIX strtod grammar (minus hex floats) into a DecNum.
   Consumes nothing and sets .valid = false on a syntax error. */
static DecNum
lex_decimal(const char *nptr)
{
    DecNum      n = {0};
    const char *p = nptr;
    int         ndig = 0;      /* significant digits accumulated */
    int         frac = 0;      /* digits seen after the '.'      */
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
            if (ndig < MAX_SIG_DIGITS) {
                /* Leading zeros are not significant. */
                if (!(n.mantissa == 0 && *p == '0')) {
                    n.mantissa = n.mantissa * 10u + (uint64_t)(*p - '0');
                    ndig++;
                }
            } else {
                /* Past our precision: remember, and keep the magnitude. */
                n.truncated = true;
                if (frac == 0) {
                    n.exp10++;
                }
            }
            if (frac > 0) {
                frac++;
            }
        } else if (*p == '.' && frac == 0) {
            frac = 1;
        } else {
            break;
        }
    }
    if (!any_digit) {
        return n;   /* .valid stays false */
    }
    if (frac > 1) {
        n.exp10 -= (frac - 1);
    }

    /* Optional exponent. A malformed one is NOT consumed — "1e" parses as
       1 with endptr at 'e', which is what POSIX strtod does. */
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        bool        eneg = false;
        int64_t     ev = 0;
        bool        edig = false;

        if (*e == '+' || *e == '-') {
            eneg = (*e == '-');
            e++;
        }
        for (; *e >= '0' && *e <= '9'; e++) {
            edig = true;
            if (ev < 100000) {            /* clamp; anything this big saturates */
                ev = ev * 10 + (*e - '0');
            }
        }
        if (edig) {
            n.exp10 += (int32_t)(eneg ? -ev : ev);
            p = e;
        }
    }

    n.valid = true;
    n.end   = p;
    return n;
}
```

- [ ] **Step 6: Add tier 1 and the public entry point**

Append:

```c
/* Tier 2 seam. Filled in by the next task; declared here so tier 1 can
   fall through to it. Returns the correctly-rounded value. */
static double strtod_slow(const DecNum *n, bool *out_range_err);

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
        m = m * kPow10Exact[n->exp10];
    } else {
        m = m / kPow10Exact[-n->exp10];
    }
    *out = m;
    return true;
}

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
        v = strtod_slow(&n, &range_err);
    }
    *out = n.negative ? -v : v;

    /* Overflow/underflow write the IEEE result AND report the range error. */
    return range_err ? AXL_ERR : AXL_OK;
}
```

- [ ] **Step 7: Add a temporary tier-2 stub so it links**

```c
/* TEMPORARY — replaced in the next task. Handles nothing correctly; its
   only job is to make the tier-1 tests linkable and RED for tier-2 input. */
static double
strtod_slow(const DecNum *n, bool *out_range_err)
{
    (void)n;
    *out_range_err = false;
    return 0.0;
}
```

- [ ] **Step 8: Wire into the build**

In `Makefile` (~line 272), after `src/format/axl-dtoa.c \`, add:

```
    src/format/axl-strtod.c \
```

- [ ] **Step 9: Run and verify tier-1 tests PASS**

```bash
make -j8 tests && TEST_APPS_ONLY=AxlTestString TEST_SKIP_RATCHET=1 \
  timeout 300 ./test/integration/test-axl.sh 2>&1 | grep -E "str_to_double|Results"
```
Expected: **every** `str_to_double:` assertion PASSES. The range cases that would
route to the tier-2 stub are deliberately not asserted until Task 2b, so this task
commits a fully green suite.

If any assertion fails, fix it before moving on — do not let it ride.

- [ ] **Step 10: Commit the work-in-progress**

```bash
git status
git add include/axl/axl-str.h src/format/axl-strtod.c Makefile \
        test/unit/axl-test-string.c
git commit -m "str: axl_str_to_double lexer + Clinger exact fast path

Tier 1 of two. When the mantissa fits in 2^53 and |exp| <= 22, both
operands are exactly representable, so ONE IEEE multiply or divide is
correctly rounded by the hardware -- no table lookup, no 128-bit
arithmetic. The 23-entry table is 184 bytes and covers essentially every
input AXL sees.

Tier 2 (the exact decimal fallback for everything else) is stubbed and
lands next, along with the tests for the behaviour only it provides."
```

---

## Task 2b: the parser — tier 2 exact decimal fallback

**Files:**
- Modify: `src/format/axl-strtod.c` (replace the stub from Task 2 Step 7)
- Test: `test/unit/axl-test-string.c`

**Interfaces:**
- Consumes: `DecNum`, `strtod_slow` signature from Task 2.
- Produces: nothing new publicly — completes `axl_str_to_double`'s contract.

**Why this task is separate:** it is the half that is easy to skip and then quietly
not have. It gets its own review gate.

- [ ] **Step 1: Write the failing tests — tier-2-only inputs**

These are chosen so **only tier 2 can answer them correctly**. That is how tier-2
coverage is proven without an internal hook, which `feedback_test_public_headers`
forbids (see design doc §8 O1).

```c
static void
test_str_to_double_exact(void)
{
    double d;

    /* THE case. Naive m * pow(10,23) gives 100000000000000008388608;
       the correctly-rounded answer is 99999999999999991611392. Comparing
       against the literal 1e23 works because the COMPILER rounds
       correctly, so this asserts we agree with it. */
    test_check(axl_str_to_double("1e23", &d, NULL) == AXL_OK && d == 1e23,
               "str_to_double exact: 1e23 (exp > 22, tier 2)");

    /* Mantissa above 2^53: 2^53+1 is not representable, and must round
       to 2^53 (ties-to-even), NOT to 2^53+2. */
    test_check(axl_str_to_double("9007199254740993", &d, NULL) == AXL_OK
               && d == 9007199254740992.0,
               "str_to_double exact: 2^53+1 rounds to even");

    /* The input that hung PHP and Java. Must terminate and be exact. */
    test_check(axl_str_to_double("2.2250738585072011e-308", &d, NULL) == AXL_OK
               && d == 2.2250738585072011e-308,
               "str_to_double exact: the PHP/Java hang case terminates");

    /* Subnormals. */
    test_check(axl_str_to_double("5e-324", &d, NULL) == AXL_OK
               && d == 5e-324 && d > 0.0,
               "str_to_double exact: smallest subnormal");
    test_check(axl_str_to_double("1.7976931348623157e308", &d, NULL) == AXL_OK
               && d == 1.7976931348623157e308,
               "str_to_double exact: DBL_MAX");

    /* Long digit strings must not be truncated into a wrong answer. */
    test_check(axl_str_to_double(
                   "0.30000000000000000000000000000000000000001", &d, NULL)
               == AXL_OK && d == 0.3,
               "str_to_double exact: 41 digits rounds to the nearest double");

    /* A 768-digit input — the classic slow-path stress case. Built rather
       than pasted so the test stays readable. */
    {
        char big[800];
        size_t i;
        big[0] = '1'; big[1] = '.';
        for (i = 2; i < 770; i++) { big[i] = '0'; }
        big[770] = '1';
        big[771] = '\0';
        /* 1.000...0001 with the 1 far past double precision rounds to 1.0 */
        test_check(axl_str_to_double(big, &d, NULL) == AXL_OK && d == 1.0,
                   "str_to_double exact: 768-digit input rounds to 1.0");
    }

    /* Range errors, which only tier 2 reaches. */
    test_check(axl_str_to_double("1e400", &d, NULL) == AXL_ERR
               && axl_isinf(d) && d > 0,
               "str_to_double exact: overflow -> +inf + AXL_ERR");
    test_check(axl_str_to_double("1e-400", &d, NULL) == AXL_ERR && d == 0.0,
               "str_to_double exact: underflow -> +0.0 + AXL_ERR");
}
```

- [ ] **Step 2: Run and verify these FAIL**

```bash
make -j8 tests && TEST_APPS_ONLY=AxlTestString TEST_SKIP_RATCHET=1 \
  timeout 300 ./test/integration/test-axl.sh 2>&1 | grep -E "str_to_double exact|Results"
```
Expected: all of them FAIL against the stub (which returns 0.0).

- [ ] **Step 3: Implement the exact decimal fallback**

Replace the stub. The algorithm — a decimal big-number, shifted by powers of two
until the value lands in `[2^52, 2^53)`, then rounded once:

```c
/* A decimal big-number: digits in base 10, one per byte, most significant
   first, with a decimal point at `dp`. Sized for the worst case: 768
   significant digits plus the exponent range either side. */
#define BIGDEC_CAP  1100

typedef struct {
    uint8_t  d[BIGDEC_CAP];
    int32_t  nd;        /* number of digits in d[]                       */
    int32_t  dp;        /* decimal point position, digits from the left  */
    bool     truncated; /* digits beyond BIGDEC_CAP were dropped         */
} BigDec;
```

Then, in order: `bigdec_from_decnum()` (re-lex the ORIGINAL text so no precision is
lost — `DecNum.mantissa` has already truncated to 19 digits, so tier 2 must not use
it), `bigdec_shift_right(n)` / `bigdec_shift_left(n)` (multiply/divide by 2^n in
decimal), then the round-to-double loop.

**This step is genuinely large.** Implement it against Go's `strconv/decimal.go`,
which is the reference for this exact approach and is BSD-licensed — read it for the
algorithm, write our own code in AXL style, do not copy it verbatim.

**Critical detail that a re-lex must handle:** `strtod_slow` takes a `DecNum`, but
`DecNum.mantissa` is already truncated to 19 digits. Change the tier-2 seam to take
the original `const char *nptr` as well, so it can re-scan at full precision:

```c
static double strtod_slow(const char *nptr, const DecNum *n, bool *out_range_err);
```
and update the one call site in `axl_str_to_double`.

- [ ] **Step 4: Run and verify PASS**

Same command as Step 2. Expected: all `str_to_double exact:` assertions PASS,
including the three range cases this task introduced.

- [ ] **Step 5: Sabotage-verify that tier 2 is actually REACHED**

This is the step that turns "the fallback exists" into "the fallback runs".

```bash
cp src/format/axl-strtod.c /tmp/strtod.orig
# Make tier 2 return a wrong answer.
python3 - <<'PY'
p='src/format/axl-strtod.c'; s=open(p).read()
s=s.replace("static double\nstrtod_slow(", "static double\nstrtod_slow_real(",1)
s += "\nstatic double strtod_slow(const char *a, const DecNum *b, bool *c)\n{ (void)a;(void)b;*c=false; return -12345.0; }\n"
open(p,'w').write(s)
PY
make -j8 tests >/dev/null 2>&1
TEST_APPS_ONLY=AxlTestString TEST_SKIP_RATCHET=1 timeout 300 \
  ./test/integration/test-axl.sh 2>&1 | grep -cE "^  FAIL"
cp /tmp/strtod.orig src/format/axl-strtod.c
```
Expected: **exactly** the assertions in `test_str_to_double_exact` fail — all of
them, since every case in it is outside tier 1's range by construction — and no
tier-1 assertion does. If a tier-1
assertion fails, tier 1 is wrongly delegating and its bound is off. If fewer fail
than expected, a "tier 2" test is actually being answered by tier 1 and is not
testing what it claims. Record the exact count in the commit message.

- [ ] **Step 6: Full gates, both arches**

Same block as Task 1 Step 8.

- [ ] **Step 7: Commit**

```bash
git status
git add src/format/axl-strtod.c test/unit/axl-test-string.c \
        test/integration/.last-pass-count
git commit -m "str: exact decimal fallback completes axl_str_to_double

Tier 2 of two: a decimal big-number shifted by powers of two until it
lands in [2^52, 2^53), then rounded once. Exact by construction, needs
no power table. With tier 1 this makes every input correctly rounded,
which is what lets parse(print(x)) == x hold against axl_dtoa.

Re-lexes the original text rather than using DecNum.mantissa, which has
already truncated to 19 significant digits -- using it would silently
cap precision at exactly the inputs this tier exists to handle.

Tier-2 coverage is proven behaviourally, not with an internal hook
(feedback_test_public_headers forbids piercing the abstraction): the
inputs chosen are outside tier 1's provable range by construction, so
only tier 2 can answer them. Sabotage-verified -- stubbing tier 2 fails
exactly N assertions, all of them tier-2 ones."
```

---

## Task 3: `axl_double_to_str` + the round-trip suite

**Files:**
- Modify: `include/axl/axl-str.h`
- Modify: `src/data/axl-str.c`
- Test: `test/unit/axl-test-string.c`

**Interfaces:**
- Consumes: `axl_dtoa` (existing), `axl_str_to_double` (Task 2/2b),
  `axl_isnan`/`axl_isfinite` (Task 1).
- Produces: `AXL_DOUBLE_STR_MAX`, `size_t axl_double_to_str(double, char *, size_t)`,
  `size_t axl_float_to_str(float, char *, size_t)`.

- [ ] **Step 1: Header contract**

```c
/**
 * @brief Buffer size that always holds axl_double_to_str's output.
 *
 * Worst case is 25 bytes: sign, leading digit, '.', 16 more digits,
 * 'e', exponent sign, 3 exponent digits, NUL. 32 leaves margin.
 */
#define AXL_DOUBLE_STR_MAX  32

/**
 * @brief Render a double as its shortest round-trippable decimal text.
 *
 * The exact inverse of axl_str_to_double(): the text this writes parses
 * back to bit-identical @p value for every finite double. Uses the
 * shortest digit string that achieves that (axl_dtoa / Grisu2), so
 * trailing zeros are never emitted — 100.0 renders "100", not "100.000".
 *
 * Chooses exponential notation when the decimal exponent is < -4 or
 * >= 17, fixed notation otherwise — the same rule as printf's %g, and
 * what a human reading a config file expects. Round-trip holds either
 * way; this only affects readability.
 *
 * NaN renders "nan", infinities "inf" / "-inf", matching what
 * axl_str_to_double() accepts, so those round-trip as text too.
 *
 * Truncates with a NUL if @p bufsz is too small — pass a buffer of
 * AXL_DOUBLE_STR_MAX and it never can be.
 *
 * @return number of bytes written, excluding the NUL. 0 if @p buf is
 *     NULL or @p bufsz is 0.
 */
size_t
axl_double_to_str(
    double  value,  ///< value to render
    char   *buf,    ///< [out] destination buffer
    size_t  bufsz   ///< capacity of @p buf including the NUL
);

/// Like axl_double_to_str but for float. Renders the shortest text that
/// round-trips through axl_str_to_float, which is shorter than the
/// double form for the same value.
size_t
axl_float_to_str(
    float   value,  ///< value to render
    char   *buf,    ///< [out] destination buffer
    size_t  bufsz   ///< capacity of @p buf including the NUL
);
```

- [ ] **Step 2: Write the failing tests**

```c
static void
test_double_to_str(void)
{
    char b[AXL_DOUBLE_STR_MAX];

    #define D2S(v, want, msg)                                          \
        do {                                                           \
            axl_memset(b, 0, sizeof(b));                               \
            axl_double_to_str((v), b, sizeof(b));                      \
            test_check(axl_strcmp(b, (want)) == 0, (msg));             \
        } while (0)

    D2S(0.0,    "0",     "double_to_str: zero");
    D2S(1.0,    "1",     "double_to_str: one, no trailing .0");
    D2S(-1.0,   "-1",    "double_to_str: negative");
    D2S(1.5,    "1.5",   "double_to_str: fraction");
    D2S(100.0,  "100",   "double_to_str: shortest form, not 100.000");
    D2S(0.001,  "0.001", "double_to_str: small fixed");
    D2S(1e-5,   "1e-05", "double_to_str: exponent < -4 switches to exponential");
    D2S(1e17,   "1e+17", "double_to_str: exponent >= 17 switches to exponential");

    axl_memset(b, 0, sizeof(b));
    axl_double_to_str(AXL_MATH_NAN, b, sizeof(b));
    test_check(axl_strcmp(b, "nan") == 0, "double_to_str: nan");
    axl_memset(b, 0, sizeof(b));
    axl_double_to_str(-AXL_MATH_INF, b, sizeof(b));
    test_check(axl_strcmp(b, "-inf") == 0, "double_to_str: -inf");

    test_check(axl_double_to_str(1.0, NULL, 32) == 0,
               "double_to_str: NULL buffer returns 0");
    test_check(axl_double_to_str(1.0, b, 0) == 0,
               "double_to_str: zero size returns 0");

    /* AXL_DOUBLE_STR_MAX is fixed in the ABI, so prove the worst case fits. */
    {
        char tight[AXL_DOUBLE_STR_MAX];
        size_t n = axl_double_to_str(-1.2345678901234567e-308,
                                     tight, sizeof(tight));
        test_check(n > 0 && n < AXL_DOUBLE_STR_MAX
                   && tight[n] == '\0',
                   "double_to_str: worst case fits AXL_DOUBLE_STR_MAX");
    }
    #undef D2S
}

static void
test_double_round_trip(void)
{
    /* The headline property: parse(print(x)) == x, bit-exact. */
    static const double kCases[] = {
        0.0, 1.0, -1.0, 0.5, 1.5, 0.1, 0.3, 100.0, 1e10, 1e-10,
        1e23, 9007199254740992.0, 2.2250738585072011e-308,
        5e-324, 1.7976931348623157e308, -0.0,
        3.141592653589793, 2.718281828459045
    };
    char   b[AXL_DOUBLE_STR_MAX];
    double back;
    size_t i;
    bool   all = true;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        axl_memset(b, 0, sizeof(b));
        axl_double_to_str(kCases[i], b, sizeof(b));
        if (axl_str_to_double(b, &back, NULL) != AXL_OK || back != kCases[i]) {
            all = false;
            break;
        }
    }
    test_check(all, "round-trip: every case parses back bit-identically");

    /* -0.0 keeps its sign through the trip: 1/-0.0 is -inf, 1/+0.0 is +inf. */
    axl_memset(b, 0, sizeof(b));
    axl_double_to_str(-0.0, b, sizeof(b));
    test_check(axl_str_to_double(b, &back, NULL) == AXL_OK
               && back == 0.0 && (1.0 / back) < 0.0,
               "round-trip: negative zero keeps its sign");
}
```

- [ ] **Step 3: Run and verify FAIL**

Expected: undefined reference to `axl_double_to_str`.

- [ ] **Step 4: Implement**

In `src/data/axl-str.c`, near the `axl_str_to_*` family. Call `axl_dtoa` for the
digits, then assemble per the `%g` rule. Handle nan/inf FIRST with `axl_isnan` /
`axl_isfinite` — `axl_dtoa` returns 0 for those and its docstring says the caller
must check.

`axl_float_to_str`: render via `axl_dtoa((double)value, ...)`, then, because a float
needs fewer digits, retry with progressively shorter digit counts until the shortest
that parses back to the same float — or simply document that it uses the double form.
**Pick one and make the docstring match**; the tests must pin whichever is chosen.

- [ ] **Step 5: Run, verify PASS, then full gates both arches**

- [ ] **Step 6: Commit**

```bash
git add include/axl/axl-str.h src/data/axl-str.c test/unit/axl-test-string.c \
        test/integration/.last-pass-count
git commit -m "str: axl_double_to_str completes the round-trip pair

The symmetric counterpart to axl_str_to_double, and the ergonomic layer
over axl_dtoa -- which hands back digits + decimal point + sign rather
than a string, so every caller would otherwise reassemble it. Analogue
of GLib's g_ascii_dtostr / G_ASCII_DTOSTR_BUF_SIZE.

No allocating variant: a round-trip double is at most 25 bytes, so the
buffer form is the only sensible one and a _buf suffix would have no
sibling to distinguish from.

With both halves present, parse(print(x)) == x is now a testable
property rather than an aspiration, and it is asserted bit-exactly
across normals, subnormals, DBL_MAX, 1e23 and negative zero."
```

---

## Task 3b: `axl_str_to_float` — closing a plan gap

**Why this task exists:** the approved design requires `axl_str_to_float`
(§3.1 lists it beside `axl_str_to_double`; §7 P2 assigns it), but no task in
this plan ever produced it. Task 3's brief docstring referenced it, and its
implementer correctly refused to document a function that does not exist.
Without this, the API ships `axl_float_to_str` with no parse counterpart —
exactly the asymmetry this project exists to remove.

**Files:**
- Modify: `include/axl/axl-str.h`
- Modify: `src/format/axl-strtod.c`
- Modify: `test/unit/axl-test-string.c`

**Interfaces:**
- Consumes: `axl_str_to_double` (Task 2/2b), `axl_float_to_str` (Task 3),
  `axl_isnan`/`axl_isinf`/`AXL_MATH_INF` (Task 1).
- Produces: `int axl_str_to_float(const char *nptr, float *out, const char **endptr)`.

- [ ] **Step 1: Header contract**

```c
/**
 * @brief Parse a decimal floating-point number into a float.
 *
 * Same grammar, same whitespace rule, same @p endptr contract and same
 * range-error convention as axl_str_to_double() — parse as a double,
 * then narrow. Narrowing rounds a second time, so the result is the
 * float nearest the decimal value in all but the vanishingly rare
 * double-rounding cases; axl_float_to_str() sidesteps this on the way
 * out by verifying its candidate through this exact path.
 *
 * A value too large for float yields +/-infinity and AXL_ERR, and one
 * too small yields +/-0.0 and AXL_ERR, matching axl_str_to_double()'s
 * rule that a range error still WRITES @p out. A double-range input
 * that merely exceeds float range is a range error here even though it
 * parsed cleanly as a double.
 *
 * @return AXL_OK on success, AXL_ERR on syntax error or range error.
 */
int
axl_str_to_float(
    const char  *nptr,    ///< number string
    float       *out,     ///< [out] parsed value
    const char **endptr   ///< [out, optional] past last consumed char
);
```

- [ ] **Step 2: Write the failing tests**

```c
static void
test_str_to_float(void)
{
    float       f;
    const char *end;

    test_check(axl_str_to_float("1.5", &f, NULL) == AXL_OK && f == 1.5f,
               "str_to_float: exact binary fraction");
    test_check(axl_str_to_float("-2.25", &f, NULL) == AXL_OK && f == -2.25f,
               "str_to_float: negative");
    test_check(axl_str_to_float("0.1", &f, NULL) == AXL_OK && f == 0.1f,
               "str_to_float: 0.1 narrows to the nearest float");
    test_check(axl_str_to_float("3.4028235e38", &f, NULL) == AXL_OK
               && f == 3.4028235e38f,
               "str_to_float: FLT_MAX");
    test_check(axl_str_to_float("1.1754944e-38", &f, NULL) == AXL_OK
               && f == 1.1754944e-38f,
               "str_to_float: FLT_MIN");

    /* Doubles that are FINE as doubles but out of float range. */
    test_check(axl_str_to_float("1e39", &f, NULL) == AXL_ERR
               && axl_isinf((double)f) && f > 0,
               "str_to_float: over float range is +inf AND AXL_ERR");
    test_check(axl_str_to_float("-1e39", &f, NULL) == AXL_ERR
               && axl_isinf((double)f) && f < 0,
               "str_to_float: under -float range is -inf AND AXL_ERR");
    test_check(axl_str_to_float("1e-60", &f, NULL) == AXL_ERR && f == 0.0f,
               "str_to_float: below float range is +0.0 AND AXL_ERR");

    /* Same syntax and endptr contract as the double form. */
    f = 9.0f;
    const char *src = "abc";
    test_check(axl_str_to_float(src, &f, &end) == AXL_ERR
               && f == 9.0f && end == src,
               "str_to_float: syntax error leaves out untouched, endptr = nptr");
    test_check(axl_str_to_float("2.5xyz", &f, &end) == AXL_OK
               && f == 2.5f && axl_strcmp(end, "xyz") == 0,
               "str_to_float: endptr stops at the first unconsumed byte");
    test_check(axl_str_to_float(NULL, &f, NULL) == AXL_ERR,
               "str_to_float: NULL input is rejected");
    test_check(axl_str_to_float("1.0", NULL, NULL) == AXL_ERR,
               "str_to_float: NULL out is rejected");
    test_check(axl_str_to_float("nan", &f, NULL) == AXL_OK
               && axl_isnan((double)f),
               "str_to_float: nan");

    /* The pair round-trips: print then parse reproduces the same float. */
    {
        static const float kF[] = {
            0.0f, 1.0f, -1.0f, 0.1f, 2.5f, 1e10f, 1e-10f,
            3.4028235e38f, 1.1754944e-38f, 1.4e-45f
        };
        char  b[AXL_DOUBLE_STR_MAX];
        float back;
        bool  all = true;
        for (size_t i = 0; i < sizeof(kF) / sizeof(kF[0]); i++) {
            axl_memset(b, 0, sizeof(b));
            axl_float_to_str(kF[i], b, sizeof(b));
            if (axl_str_to_float(b, &back, NULL) != AXL_OK
                || axl_memcmp(&back, &kF[i], sizeof(float)) != 0) {
                all = false;
                break;
            }
        }
        test_check(all, "float round-trip: str_to_float inverts float_to_str");
    }
}
```

Also add the assertion Task 3's review flagged as missing, in `test_float_to_str`:

```c
    F2S(-0.0f, "-0", "float_to_str: negative zero keeps its sign");
```

- [ ] **Step 3: Run, verify FAIL** (undefined reference to `axl_str_to_float`).

- [ ] **Step 4: Implement in `src/format/axl-strtod.c`**, beside `axl_str_to_double`:
parse as a double, then narrow. Detect float overflow/underflow AFTER the cast
(a finite double can become inf or 0 as a float) and report `AXL_ERR` while still
writing the value, exactly as the double form does. Propagate the double form's
own `AXL_ERR` — a double-range error is also a float-range error.

- [ ] **Step 5: Verify PASS. Step 6: full gates both arches. Step 7: commit.**

```bash
git add include/axl/axl-str.h src/format/axl-strtod.c \
        test/unit/axl-test-string.c test/integration/.last-pass-count
git commit -m "str: axl_str_to_float completes the float pair

The approved design required this beside axl_str_to_double, but the
implementation plan never assigned it to a task -- caught when Task 3's
docstring had to reference a function that did not exist. Without it the
API shipped axl_float_to_str with no parse counterpart, which is the
exact asymmetry this work set out to remove.

Parses as a double then narrows, so a value in double range but outside
float range is a range error that still writes +/-inf or +/-0.0, matching
the double form's rule. Round-trip against axl_float_to_str is asserted
over normals, subnormals, FLT_MAX and FLT_MIN.

Also adds the -0.0f assertion Task 3's review flagged as missing."
```

---

## Task 4: integer symmetry

**Files:**
- Modify: `include/axl/axl-str.h`
- Modify: `src/data/axl-str.c`
- Test: `test/unit/axl-test-string.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `size_t axl_u64_to_str(uint64_t, int base, char *, size_t)`,
  `size_t axl_s64_to_str(int64_t, int base, char *, size_t)`.

- [ ] **Step 1: Header contract**

```c
/**
 * @brief Render an unsigned 64-bit integer in @p base.
 *
 * The reverse of axl_str_to_u64(), sharing its @p base parameter
 * (2..36, digits '0'..'9' then lowercase 'a'..'z'), so the two
 * round-trip at any radix. No "0x" prefix is emitted — add one
 * yourself if you want axl_str_to_u64's base-0 auto-detection to see
 * it on the way back.
 *
 * Truncates with a NUL if @p bufsz is too small. 65 bytes always
 * suffices (64 binary digits plus NUL).
 *
 * @return bytes written excluding the NUL, or 0 if @p buf is NULL,
 *     @p bufsz is 0, or @p base is out of range.
 */
size_t
axl_u64_to_str(
    uint64_t  value,  ///< value to render
    int       base,   ///< radix, 2..36
    char     *buf,    ///< [out] destination
    size_t    bufsz   ///< capacity including the NUL
);

/// Like axl_u64_to_str with a leading '-' for negative values. The
/// magnitude is taken in unsigned arithmetic so INT64_MIN renders
/// correctly rather than overflowing on negation.
size_t
axl_s64_to_str(
    int64_t   value,  ///< value to render
    int       base,   ///< radix, 2..36
    char     *buf,    ///< [out] destination
    size_t    bufsz   ///< capacity including the NUL
);
```

- [ ] **Step 2: Write the failing tests**

```c
static void
test_int_to_str(void)
{
    char b[72];

    #define U2S(v, base, want, msg)                                    \
        do {                                                           \
            axl_memset(b, 0, sizeof(b));                               \
            axl_u64_to_str((v), (base), b, sizeof(b));                 \
            test_check(axl_strcmp(b, (want)) == 0, (msg));             \
        } while (0)

    U2S(0,     10, "0",       "u64_to_str: zero");
    U2S(12345, 10, "12345",   "u64_to_str: decimal");
    U2S(255,   16, "ff",      "u64_to_str: hex is lowercase, no 0x prefix");
    U2S(255,    2, "11111111","u64_to_str: binary");
    U2S(8,      8, "10",      "u64_to_str: octal");
    U2S(35,    36, "z",       "u64_to_str: base 36 uses z");
    U2S(18446744073709551615ULL, 10, "18446744073709551615",
        "u64_to_str: UINT64_MAX");

    axl_memset(b, 0, sizeof(b));
    axl_s64_to_str(-12345, 10, b, sizeof(b));
    test_check(axl_strcmp(b, "-12345") == 0, "s64_to_str: negative");
    axl_memset(b, 0, sizeof(b));
    axl_s64_to_str((-9223372036854775807LL - 1), 10, b, sizeof(b));
    test_check(axl_strcmp(b, "-9223372036854775808") == 0,
               "s64_to_str: INT64_MIN does not overflow on negation");

    test_check(axl_u64_to_str(1, 1, b, sizeof(b)) == 0,
               "u64_to_str: base 1 is rejected");
    test_check(axl_u64_to_str(1, 37, b, sizeof(b)) == 0,
               "u64_to_str: base 37 is rejected");
    test_check(axl_u64_to_str(1, 10, NULL, 8) == 0,
               "u64_to_str: NULL buffer returns 0");

    /* Symmetry: the pair round-trips at every base the family accepts. */
    {
        static const int bases[] = {2, 8, 10, 16, 36};
        static const uint64_t vals[] = {0, 1, 42, 65535,
                                        18446744073709551615ULL};
        bool all = true;
        for (size_t i = 0; i < 5 && all; i++) {
            for (size_t j = 0; j < 5 && all; j++) {
                uint64_t back;
                axl_memset(b, 0, sizeof(b));
                axl_u64_to_str(vals[j], bases[i], b, sizeof(b));
                if (axl_str_to_u64(b, bases[i], &back, NULL) != AXL_OK
                    || back != vals[j]) {
                    all = false;
                }
            }
        }
        test_check(all, "int round-trip: u64 survives every base 2/8/10/16/36");
    }
    #undef U2S
}
```

- [ ] **Step 3: Run and verify FAIL. Step 4: implement. Step 5: verify PASS.**

Implementation: render digits into a scratch buffer back-to-front, then reverse.
Take the magnitude of `int64_t` as `uint64_t` — `(uint64_t)-(v + 1) + 1` — so
`INT64_MIN` does not overflow.

- [ ] **Step 6: Full gates both arches, then commit**

```bash
git add include/axl/axl-str.h src/data/axl-str.c test/unit/axl-test-string.c \
        test/integration/.last-pass-count
git commit -m "str: axl_u64_to_str / axl_s64_to_str complete the conversion surface

axl_str_to_u64 had no reverse -- the only way to render an integer was
axl_snprintf(\"%llu\"). Shares the base parameter so the pair round-trips
at any radix 2..36, which the tests assert across 2/8/10/16/36.

After this, axl-str.h has no asymmetric conversion left."
```

---

## Task 4b: fix the `axl_dtoa` `kPow10` out-of-bounds read

> **Superseded.** The clamp this task landed is gone: `kPow10` is now a
> `uint64_t` table of 10^0..10^19, which covers every reachable index, so
> the multiplier is never substituted. See Task 4 of
> `AXL-Stream-Followups-Plan.md`. The defect analysis below still stands
> — only the fix changed.

**Added mid-flight** (Mike approved 2026-07-30) after Task 4's review surfaced
it. Pre-existing and released — `src/format/axl-dtoa.c` came in at `ac7849e1`,
long before this plan — but it sits directly under the conversion surface these
tasks build on, and `axl_double_to_str` / `axl_snprintf("%g")` both reach it.

**Files:**
- Modify: `src/format/axl-dtoa.c`
- Test: `test/unit/axl-test-format.c` (or wherever `axl_dtoa` is already tested)

**The defect.** `kPow10` (line ~211) has **10** entries. `digit_gen`'s fractional
branch indexes it with `idx = -kappa` and the comment above the use asserts:

> *the loop terminates well before idx reaches 10, so kPow10[idx] (entries 0..9)
> is always in bounds — verified empirically*

That is **false**. Instrumented sweep of 4,997,559 finite doubles: `idx` ranges
`[2 .. 16]`, and **3,510,086 calls (~70%) index out of bounds** — up to 6
`uint32_t` (24 bytes) past the array. It does not crash because the adjacent
`.rodata` is inside the loaded image, and output is unaffected today, but it is
UB, and the comment actively tells the next reader not to look.

**The fix** is the standard Grisu2 guard — clamp the multiplier to 0 once the
index leaves the table:

```c
int idx = -kappa;
grisu_round(buffer, *len, delta, p2, one.f,
            wp_w.f * (idx < 10 ? kPow10[idx] : 0));
```

**Verified behavior-preserving:** guarded vs unguarded over 4,997,559 finite
doubles — **0 differences** in digits, `decpt`, or sign.

- [x] **Step 1: regression test FIRST.** The trap is that any output-only
  assertion passes both before and after, so it cannot go red. Pin the *index
  bound*, not the digits: assert `axl_dtoa` round-trips through
  `axl_str_to_double` for values that drive the loop deepest
  (`4.7112871036659575e+180` was the deepest observed, plus `1.5e300`, `1e-300`,
  `DBL_MAX`, `DBL_TRUE_MIN`, `2.2250738585072014e-308`). If you cannot make a
  test that genuinely fails against the current code, **say so plainly in your
  report rather than writing one that passes for the wrong reason** — this is a
  case where the honest answer may be "the sanitizer is the test," and the
  host-side ASan run below is then the real evidence.
- [x] **Step 2: apply the guard.**
- [x] **Step 3: prove no behavior change.** Host harness: build the current and
  guarded `axl-dtoa.c` side by side (rename one symbol), sweep >= 1M random
  finite doubles plus the boundary list, assert identical digits/decpt/neg.
  Report the count.
- [x] **Step 4: rewrite the false comment.** State the real bound and why the
  clamp is correct (past the table the multiplier's contribution is negligible
  and Grisu2 falls back to the unrounded digits, which are still valid — Grisu2
  is allowed to return non-shortest, never wrong).
- [x] **Step 5: full gates both arches, commit.**

---

## Task 4c: one truncation convention for all four `*_to_str` renderers

**Added mid-flight** (Mike chose snprintf semantics 2026-07-30). Task 4's review
established that `emit_result` returns *bytes written*, so
`axl_double_to_str(1234567.0, b, 8)` and `axl_double_to_str(12345678901.0, b, 8)`
produce an **identical buffer and identical return** — truncation is
undetectable. Task 4 avoided that by making the integer pair all-or-nothing,
which left two opposite behaviors in one header.

Decision: **all four adopt `axl_snprintf`'s convention**, already documented at
`axl-str.h:310` — *"number of bytes that would have been written (excluding
NUL), regardless of buffer size (allows truncation detection)"*. That is the
house rule for "format into a fixed buffer"; the renderers were the outliers.

None of the four are released — `axl_double_to_str` / `axl_float_to_str` landed
in `29f6ab5b` and `axl_u64_to_str` / `axl_s64_to_str` in `157dc321`, all
unpushed, and `v3.1.0` / `v3.0.0` / `v2.9.0` contain none of them. There is no
compatibility cost, and this is the last moment it is free.

**Files:**
- Modify: `include/axl/axl-str.h` (four docstrings + the `@return` of each)
- Modify: `src/data/axl-str.c` (`emit_result`, `int_emit`, callers)
- Test: `test/unit/axl-test-string.c`

**The target contract, identical for all four:**
- Always NUL-terminate when `bufsz >= 1`. Write as much as fits.
- Return the length the full output *would* have had, excluding the NUL.
- `ret >= bufsz` means truncated. `ret < bufsz` means complete.
- `buf == NULL` or `bufsz == 0`: write nothing. Return the needed length anyway
  (that is what snprintf does, and it is what makes "call once to size, once to
  fill" work) — **but check `axl_snprintf`'s own behavior at `bufsz == 0` and
  match it exactly**; if it returns 0 there, match that instead and say so.
- Invalid base still returns 0 and writes a NUL — 0 stays the "cannot render"
  signal, distinct from any real length. Keep the `AXL_U64_STR_MAX` /
  `AXL_S64_STR_MAX` / `AXL_DOUBLE_STR_MAX` guarantees (there is no separate
  float macro — `axl_float_to_str` shares `AXL_DOUBLE_STR_MAX`, which its
  docstring already says can never be too small).

- [ ] **Step 1: rewrite the four `@return` blocks and the truncation paragraphs
  first.** They are the contract. Remove the all-or-nothing wording Task 4
  added, and remove the "truncates with a NUL" wording where it now understates
  what the caller gets.
- [ ] **Step 2: update the existing tests to the new expectations, and add the
  truncation-detection assertions** — for each of the four: an exact fit
  (`ret == bufsz-1`), a one-byte-short case (`ret > bufsz-1`, buffer still
  NUL-terminated), and `bufsz == 1`. Include the case that motivated this:
  `axl_double_to_str(1e-300, b, 5)` must now be *detectably* truncated.
- [ ] **Step 3: confirm RED.** Task 4's all-or-nothing assertions must fail.
- [ ] **Step 4: implement.** One shared emit helper if the shapes allow it —
  there should not be two truncation implementations after this task.
- [ ] **Step 5: GREEN, refactor while green.**
- [ ] **Step 6: full gates both arches, commit.**

---

## Task 5: `%f/%e/%g` in `axl_sscanf`

**Files:**
- Modify: the scanf implementation (`grep -rn "axl_vsscanf" src/` to locate)
- Modify: `include/axl/axl-str.h` — the conversion-specifier list in the block
  comment above `axl_sscanf` (~line 1326), which currently enumerates the
  supported set and must stay accurate
- Test: `test/unit/axl-test-string.c`

**Interfaces:**
- Consumes: `axl_str_to_double` (Task 2/2b).
- Produces: nothing new publicly.

- [ ] **Step 1: Write the failing tests**

```c
static void
test_sscanf_float(void)
{
    double d = 0.0;
    float  f = 0.0f;
    int    n;

    n = axl_sscanf("3.5", "%lf", &d);
    test_check(n == 1 && d == 3.5, "sscanf: %lf reads a double");

    n = axl_sscanf("2.25", "%f", &f);
    test_check(n == 1 && f == 2.25f, "sscanf: %f reads a float");

    d = 0.0;
    n = axl_sscanf("1e10", "%lf", &d);
    test_check(n == 1 && d == 1e10, "sscanf: %lf reads an exponent");

    d = 0.0;
    n = axl_sscanf("-0.5", "%le", &d);
    test_check(n == 1 && d == -0.5, "sscanf: %le is accepted");
    d = 0.0;
    n = axl_sscanf("7.5", "%lg", &d);
    test_check(n == 1 && d == 7.5, "sscanf: %lg is accepted");

    /* Mixed with other conversions, and the count is right. */
    {
        int    i = 0;
        double t = 0.0;
        n = axl_sscanf("cpu 3 36.5", "cpu %d %lf", &i, &t);
        test_check(n == 2 && i == 3 && t == 36.5,
                   "sscanf: float mixes with %d and literals");
    }

    /* Assignment suppression performs the conversion but stores nothing. */
    d = 99.0;
    n = axl_sscanf("1.5 2.5", "%*lf %lf", &d);
    test_check(n == 1 && d == 2.5, "sscanf: %*lf suppresses assignment");

    /* A non-numeric input assigns nothing. */
    d = 99.0;
    n = axl_sscanf("abc", "%lf", &d);
    test_check(n == 0 && d == 99.0, "sscanf: non-numeric input assigns nothing");
}
```

- [ ] **Step 2: Run and verify FAIL** — expect 0 conversions, or -1 for an
  unrecognized specifier, depending on how the current code rejects unknowns.

- [ ] **Step 3: Implement.** Add `f e g E G` to the conversion switch, routing to
  `axl_str_to_double` on the remaining input and advancing the reader by the
  returned `endptr`. Honour the `l`/`L` length modifiers (`%f` → `float *`,
  `%lf` → `double *`) and the `*` suppression flag exactly as the integer
  conversions do.

- [ ] **Step 4: Update the specifier list in the header block comment.**
  It currently enumerates the supported conversions and omits floats — leaving it
  stale is the exact doc-drift the project's doc-sync rule targets.

- [ ] **Step 5: Verify PASS, full gates both arches, commit**

```bash
git add include/axl/axl-str.h src/format/... test/unit/axl-test-string.c \
        test/integration/.last-pass-count
git commit -m "str: axl_sscanf gains %f/%e/%g

axl_snprintf has printed %f/%e/%g since the format engine landed while
axl_sscanf accepted none of them, so the printf/scanf pair was
asymmetric. Routes to axl_str_to_double, so scanned values are correctly
rounded like every other float conversion in the SDK.

The specifier list in the header block comment is updated -- it
enumerates the supported set, so omitting floats there would have been
stale the moment this landed."
```

---

## Task 6: documentation sync

**Files:**
- Modify: `src/data/README.md`
- Modify: `src/format/README.md` (if it exists — `ls src/format/README.md`)
- Modify: `docs/AXL-Float-Conversion-Design.md` (status line)

- [ ] **Step 1: Document the new surface in `src/data/README.md`**

Add a "Number conversion" section covering both directions with a worked example,
the round-trip guarantee, the accuracy-contract split against axl-math, and the
range-error divergence (value written AND `AXL_ERR`). Match the prose style of the
existing AxlArray/AxlString sections.

- [ ] **Step 2: Mark the design doc implemented**

Change its status line to `IMPLEMENTED <date>` and add the commit range.

- [ ] **Step 3: Update `docs/ROADMAP.md`**

Change the index entry from `DRAFT:` to done, matching how other completed design
docs are marked.

- [ ] **Step 4: Run the doc gates and commit**

```bash
make check-docs
./scripts/build-docs.sh | tail -2
git add src/data/README.md docs/AXL-Float-Conversion-Design.md docs/ROADMAP.md
git commit -m "docs: record the float conversion surface

README prose is where doc drift hides -- the doxygenfile directives
render the new functions automatically, but nothing would have described
the round-trip guarantee, the accuracy split against axl-math, or the
range-error divergence from the integer family."
```

---

## Self-Review

**Spec coverage.** §3.1 parse → Tasks 2, 2b. §3.2 print → Task 3. §3.3 integer
symmetry → Task 4. §3.4 sscanf → Task 5. §3.5 axl-math → Task 1. §4 two tiers →
Tasks 2 and 2b, with §4's "reserved, not built" honoured by the seam comment in
Task 2 Step 4. §5 error/range contract → tests in Task 2 Step 2 and Task 2b Step 1.
§6 testing → every task's test step, with the round-trip suite in Task 3 and the
hard-case table in Task 2b. §8 O1 → Task 2b Step 5. §8 O2 → Task 3 Step 1 docstring
and Step 2 tests. §8 O3 → Task 3 Step 2's worst-case assertion.

**Deliberately not covered**, per the spec: Eisel–Lemire (§4, seam reserved), hex
floats (§3.1), `axl_json_get_double` (JSON's surface).

**Known soft spot.** Task 2b Step 3 is the one step whose implementation is
described rather than written out, because the exact decimal big-number is ~300
lines and depends on choices the implementer will make while reading Go's
`decimal.go`. It is called out as large, it names its reference, and it has its own
review gate and a sabotage step — but an implementer should expect it to take
longer than any other step in this plan.

**Type consistency.** `DecNum` is defined in Task 2 Step 4 and consumed in 2b.
`strtod_slow`'s signature CHANGES in Task 2b Step 3 (gains `const char *nptr`) —
this is called out explicitly there, including the call-site update, because the
truncated 19-digit mantissa cannot serve tier 2.
