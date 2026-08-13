# AXL Float Conversion — string ↔ double, correctly rounded

**Status:** IMPLEMENTED 2026-07-31, 21 commits on `3be79c4a` (Clinger +
exact fallback; Eisel-Lemire specified but not built).
**Date:** 2026-07-30, implemented 2026-07-31.

Delivered beyond the original spec: `axl_str_to_float` (the design required it
in S3.1/S7-P2 but the plan assigned it to no task -- added as task 3b);
`axl_u64_to_str`/`axl_s64_to_str`; `%f/%e/%g/%E/%G` in `axl_sscanf` including
field width; one truncation convention across all four renderers, matching
`axl_snprintf`; and a fix for an out-of-bounds `kPow10` read in the
pre-existing `axl_dtoa` that this work uncovered.

Closes a complete gap: AXL can compute with doubles and print them, but has
no way to read one. Adds a correctly-rounded parser, its symmetric printer,
and the `nan`/`inf` surface both need.

---

## 1. The gap, verified

| direction | today |
|---|---|
| string → integer | `axl_str_to_u64/u32/u16/u8`, `_s64/s32/s16/s8`, `axl_hex_parse_u64` |
| **string → double** | **nothing** |
| **integer → string** | **nothing** (only `axl_snprintf("%llu")`) |
| double → string | `axl_dtoa` (Grisu2, public, round-trippable) + `%f/%e/%g` |
| double arithmetic | `axl-math.h`, explicitly approximate |

Three independent confirmations the parse side is absent: `axl_sscanf`'s
conversion list is enumerated and contains no `%f`/`%e`/`%g`;
`grep -rln "strtod\|parse_double\|to_double\|atof" src/` returns nothing;
and `axl-json.h` has `get_int`/`get_uint`/`get_bool` but no `get_double`.

`docs/AXL-JSON-Design.md` §4 records the consequence as a deliberate current
position: *"AXL is freestanding with no libm and has no double accessor …
`NaN`/`Infinity` are retrievable only as text via `axl_json_get_number_str`."*
That accessor is the workaround for this gap. This design does not change
JSON; it gives JSON something to layer on if it later wants to.

**Consumer:** Redfish sensor data — temperatures, voltages, fan RPM, power —
are JSON reals. SoftBMC is the flagship roadmap item and `rfbrowse` cannot
read a sensor value numerically today.

---

## 2. What `axl_dtoa` already gives us

`axl_dtoa` (public, `axl-format.h`, Grisu2, ~1.3 KB table) produces a short
string of decimal digits that, when read back, reproduces the value
*exactly*. So the print half of a round trip already exists and is exact.

The *round trip* is exact; the *length* is Grisu2's best effort, not a
proven minimum. For a fraction of a percent of doubles a shorter string
also round-trips — `1e23` comes back as `9999999999999999e7` where `1e23`
would have done. Guaranteeing minimality is Grisu3/Ryu territory (bignum
fallback) and buys nothing here, so the contract to document is
"round-trips exactly, very nearly always minimal", not "shortest".

That raises the bar for the parser: a merely-close parser would make
`parse(print(x)) == x` false for a minority of inputs, silently. Correct
rounding is therefore a requirement, not a luxury — it is what makes the pair
a pair.

---

## 3. API surface

### 3.1 Parse

Identical shape to `axl_str_to_u64` — same whitespace rule, same `endptr`
contract, same `AXL_OK`/`AXL_ERR` return. No `base` parameter (meaningless
for decimal floats).

```c
int axl_str_to_double(const char *nptr, double *out, const char **endptr);
int axl_str_to_float (const char *nptr, float  *out, const char **endptr);
```

`axl_str_to_float` narrows from the double result with a range check,
mirroring how `_u32`/`_u16`/`_u8` narrow from `_u64`.

**Grammar** (POSIX `strtod` minus hex floats): optional whitespace, optional
sign, digits with an optional `.`, optional `e`/`E` exponent with optional
sign. Plus `nan`, `inf`, `infinity` — case-insensitive — which both POSIX
`strtod` and `g_ascii_strtod` accept, and which lets JSON's `ALLOW_NAN_INF`
map onto the parser instead of staying text-only. A sign on `inf` is
applied; a sign on `nan` is consumed but discarded, so `-nan` yields a
*positive* NaN. That is a deliberate divergence from glibc and costs
nothing, because AXL has no signed-NaN surface at all — every renderer
writes `nan` unsigned, so no text this library produces goes unread.

**Hex floats (`0x1.8p3`) are NOT accepted.** C99 and accepted by both POSIX
and GLib, but no consumer is in sight and it is a separate sub-grammar. If
added later it is purely additive.

Locale is not a concern and cannot become one: freestanding means there is no
locale, so the decimal separator is always `.`. This is `g_ascii_strtod`
semantics by construction rather than by choice — worth stating in the
docstring so nobody adds a locale hook.

### 3.2 Print — the symmetric counterpart

```c
#define AXL_DOUBLE_STR_MAX 32
size_t axl_double_to_str(double value, char *buf, size_t bufsz);
size_t axl_float_to_str (float  value, char *buf, size_t bufsz);
```

Renders the shortest round-trippable form, choosing fixed or exponential
notation the way `%g` does. Thin layer over `axl_dtoa`, which already does
the hard part; this exists because `axl_dtoa` hands back *digits + decimal
point + sign*, not a string, and every caller would otherwise reassemble it.

`axl_dtoa` stays exactly as it is — the low-level primitive.
`axl_double_to_str` is the ergonomic pair member, and the direct analogue of
GLib's `g_ascii_dtostr` / `G_ASCII_DTOSTR_BUF_SIZE`.

**No allocating variant**, deliberately, and this is the one place the naming
departs from the `axl_utf8_to_ucs2` / `axl_utf8_to_ucs2_buf` pattern: a
round-trip double is at most ~25 bytes, so a heap allocation is never the
right answer and the `_buf` suffix would have no sibling to distinguish
itself from.

### 3.3 Integer symmetry — completing the surface

`axl_str_to_u64` has no reverse. Added for the same reason:

```c
size_t axl_u64_to_str(uint64_t value, int base, char *buf, size_t bufsz);
size_t axl_s64_to_str(int64_t  value, int base, char *buf, size_t bufsz);
```

`base` mirrors `axl_str_to_u64`'s parameter (2–36), so the pair round-trips
at any radix. The digit emission already exists inside the format engine;
this exposes it.

**Correction (2026-07-31):** this section originally closed "after this,
`axl-str.h` has no asymmetric conversion", and the commit message that
shipped it (`157dc321`) said the same. That was wrong — it surveyed the
*number* conversions and generalised to the whole header. `axl_utf8_decode`
had no public reverse at the time; `axl_ucs2_to_utf8` converts whole
UCS-2 strings, not one codepoint, and `src/data/axl-json-parse.c` carried
a private `utf8_encode` reimplementing the missing half. `axl_utf8_encode`
closed that gap. The claim is quoted here rather than quietly dropped
because the lesson generalises: an absence claim about a whole header has
to be checked against the whole header, not against the family in front of
you.

### 3.4 `axl_sscanf`

Add `%f`, `%e`, `%g`, `%E`, `%G` with the `l` length modifier.
`axl_snprintf` already *prints* all of these and `axl_sscanf` accepts none —
completing printf/scanf symmetry. Falls out of the parser primitive.

**As implemented:** `L` is NOT accepted — AXL has no `long double` support, so
`%Lf` returns -1 rather than silently parsing as a double. Plain `%f` takes a
`float *` and `%lf` a `double *`, per C99 and the reverse of `printf`. An
explicit field width is honoured up to 256; beyond that `-1`, since the width
is a property of the format string rather than of the input.

### 3.5 axl-math — the surface both halves need

Currently there are **zero** nan/inf predicates or constants. `axl-format.c`
hand-rolls `value != value` and compares against the literal
`1.7976931348623157e308`, and `axl_dtoa`'s public docstring instructs
consumers to do exactly the same. Add:

```c
#define AXL_MATH_INF          /* +infinity  */
#define AXL_MATH_NAN          /* quiet NaN  */
#define AXL_MATH_DBL_MAX      /* 1.7976931348623157e308 */
#define AXL_MATH_DBL_TRUE_MIN /* 4.9406564584124654e-324, smallest subnormal */

bool axl_isnan(double x);
bool axl_isinf(double x);
bool axl_isfinite(double x);
```

and retrofit `axl-format.c`'s two hand-rolled checks to use them — AXL
internals must dogfood the public API.

**Accuracy contract split** — the real consistency risk, and it gets a doc
block in `axl-math.h`: `axl-math` is deliberately approximate ("sufficient
for UI coordinates and animation easing — not for numerical analysis"), while
these conversions are correctly rounded. Someone who parses a value exactly
and then scales it with `axl_pow(10, n)` silently discards the exactness they
just paid for. Nothing warns them today.

---

## 4. Algorithm — two tiers, always correctly rounded

### The constraint that drives everything

**You cannot use double arithmetic to correctly round to double.** Deciding
the final bit needs more precision than the target format carries.
`strtod("1e23")`:

| | value |
|---|---|
| correctly rounded | `99999999999999991611392` |
| naive `m × pow(10,23)` | `100000000000000008388608` |

Two different doubles; round-trip with `axl_dtoa` breaks. The cause is that
`10^23 = 2^23·5^23` with `5^23 ≈ 1.19e16 > 2^53`, so the multiplicand is
already inexact before the multiply. Using `axl_pow` would be worse still —
it is an approximation of an already-inexact value.

### Tier 1 — Clinger exact fast path (~184 bytes)

If the mantissa ≤ 2^53 **and** |decimal exponent| ≤ 22, both operands *are*
exactly representable, so a single hardware multiply (`m * 10^e`) or divide
(`m / 10^-e`) is correctly rounded by IEEE 754 alone.

22 is exactly where this stops: `10^22 = 2^22·5^22` with
`5^22 ≈ 2.4e15 < 2^53` ✓, and `10^23` fails ✗.

Cost is a 23-entry `double` table — **184 bytes** — and it covers
essentially every input AXL will see: `36.6`, `1.5`, `0.001`,
`3.14159265358979`, Redfish sensor readings, config values, coordinates.

### Tier 2 — exact decimal fallback (no table)

Everything else: >2^53 mantissa, |exp| > 22, subnormals. A decimal
big-number representation shifted by powers of two until it lands in the
binary exponent range, then rounded once, correctly, with the round-half-even
tie broken on the full remaining decimal — the approach Go's `strconv` uses
as its final fallback. Exact by construction, needs no power table.

### Why not Eisel–Lemire

E–L is the standard fast path in Rust, Go, simdjson and fast_float, and it
was the original plan here. It is **not** in this design because its cached
power table is ~10 KB of rodata (617 × 16 bytes) versus Clinger's 184 bytes,
and it buys only *speed* — tier 2 already provides the correctness. AXL
parses doubles at config-value and sensor-reading rates, never in a hot loop,
so µs instead of ns is invisible.

There is also a correctness argument for leaving it out: without E–L
absorbing the middle range, tier 2 runs often enough to be genuinely
exercised. A rarely-hit fallback is the classic way this feature ships subtly
broken.

**Reserved, specified, not built.** The tier seam is designed to accept E–L
as a drop-in middle tier behind `AXL_STRTOD_EISEL_LEMIRE=1` if a consumer
ever parses doubles in bulk. Deliberately not built now: an off-by-default
`#ifdef` path must have every gate run twice or it rots, and AXL already has
evidence — `AXL_TLS=1`, the existing optional-build precedent, drifted red
undetected until a release-prep audit caught it. Adding it later is purely
additive and requires no restructuring.

---

## 5. Error and range contract

With an `endptr` (partial parsing):

| input | `*out` | return | `endptr` |
|---|---|---|---|
| `"36.6"` | `36.6` | `AXL_OK` | past the digits |
| `"36.6C"` | `36.6` | `AXL_OK` | at `C` |
| `"1e400"` | `+inf` | `AXL_ERR` | past the digits |
| `"1e-400"` | `+0.0` | `AXL_ERR` | past the digits |
| `"nan"` | quiet NaN | `AXL_OK` | past `nan` |
| `"abc"` | untouched | `AXL_ERR` | `= nptr` |

Note the range rows: `endptr` lands *past* the digits, because the number
was consumed and is merely unrepresentable — C99 `strtod`'s `ERANGE`
behaviour. The integer family rewinds `endptr` to `nptr` on overflow
instead. Both shipped; the divergence is documented rather than
reconciled.

With **no** `endptr` the parse is strict — the whole string or nothing —
which is the rule the entire integer family already enforces:

| input | `*out` | return |
|---|---|---|
| `"36.6"` | `36.6` | `AXL_OK` |
| `"36.6C"` | untouched | `AXL_ERR` |
| `" 5 "` | untouched | `AXL_ERR` (leading ws OK, trailing ws is content) |
| `"1e400"` | `+inf` | `AXL_ERR` (range) |
| `"1e400xyz"` | untouched | `AXL_ERR` (strict outranks range) |

**One deliberate divergence from the integer family**, and it needs saying
loudly in the docstring: on overflow/underflow the value IS written. The
integer family leaves `*out` untouched, but for doubles ±inf and ±0 are the
correct IEEE results, and discarding them loses information the caller may
want. POSIX expresses this as "return HUGE_VAL and set ERANGE"; with no
`errno`, "value written, status says out-of-range" is the only way to give
the caller both.

`nan`/`inf` in the *input* are values, not errors — `AXL_OK`. Only a range
failure on a finite literal is `AXL_ERR`.

---

## 6. Testing

Test-first per the project workflow; new public API, so header + docstrings
first, then failing tests.

**Round-trip is the headline property** and is now checkable in both
directions: `axl_str_to_double(axl_double_to_str(x)) == x`, bit-exact, over
a corpus spanning normals, subnormals, powers of two and ten, and both
tier boundaries.

**Hard cases, exact expected bits:**

| input | why |
|---|---|
| `1e23` | the naive-scaling divergence above |
| `9007199254740993` | 2^53+1, unrepresentable, tests the tie |
| `2.2250738585072011e-308` | the PHP/Java infinite-loop bug |
| `5e-324` | smallest subnormal |
| `1.7976931348623157e308` | DBL_MAX |
| `0.1`, `0.3` | classic inexact decimals |
| 768+ digit inputs | forces tier 2, no truncation shortcuts |

**The fallback must be REACHED, not merely present.** A test asserts tier 2
is exercised — an unreached fallback is how this ships broken. Since tiers
are not observable through the public API, this needs either an internal
test hook or a counter; the plan must pick one rather than hand-wave it.

**Symmetry tests:** every `axl_X_to_str` / `axl_str_to_X` pair round-trips,
including the integer pair at bases 2, 8, 10, 16, 36.

Both arches (`--arch AARCH64`). Gates: `check-ascii`, `check-docs`,
`check-dogfood`, `scripts/lint.sh`, `scripts/build-docs.sh`.

---

## 7. Phasing

- **P1** — axl-math nan/inf surface + accuracy-contract doc block + retrofit
  `axl-format.c`. Standalone, unblocks the rest.
- **P2** — `axl_str_to_double` tier 1 + tier 2, `axl_str_to_float`.
- **P3** — `axl_double_to_str` / `axl_float_to_str` over `axl_dtoa`, and the
  round-trip test suite that only becomes possible once both halves exist.
- **P4** — `axl_u64_to_str` / `axl_s64_to_str` integer symmetry.
- **P5** — `%f/%e/%g` in `axl_sscanf`.
- **Deferred** — Eisel–Lemire middle tier (§4); hex float literals (§3.1);
  `axl_json_get_double` (JSON's surface, and that session's call).

## 8. Resolved decisions (were open questions)

**O1 — tier 2 coverage is proven BEHAVIOURALLY, with no instrumentation.**
The instinct was an internal hook or a tier counter; both are wrong here.
`feedback_test_public_headers` is explicit — *"if the public API is missing
the right hook, fix the API, don't pierce the abstraction"* — and a public
tier counter would be API surface existing only for tests.

The better answer needs no hook at all: **choose inputs only tier 2 can
answer correctly**, and assert exact bits. `1e23` (exponent > 22),
`9007199254740993` (mantissa > 2^53), `2.2250738585072011e-308`, and the
768-digit cases are all outside tier 1's provable range by construction. If
tier 2 were absent, wrong, or if tier 1 wrongly claimed them, those
assertions fail. That tests the OUTCOME rather than the mechanism, which is
both stronger and refactor-proof — tiers can be restructured without
rewriting the tests.

Backed by the usual sabotage pass: stub tier 2 to return `AXL_ERR` and
confirm exactly the expected set of assertions goes red, no more and no
fewer. That is what turns "the fallback exists" into "the fallback is
reached and correct".

**O2 — `%g`-style fixed-vs-exponential.** Exponential when the decimal
exponent is < -4 or ≥ 17, fixed otherwise; the C convention, and what a
human reading a config file or a log expects. Round-trip holds either way,
so this is purely a readability choice. Trailing zeros are not emitted
(`100.0` renders `100`, not `100.000`), matching `axl_dtoa`'s shortest form.

**O3 — `AXL_DOUBLE_STR_MAX = 32`, confirmed with headroom.** True worst case
under O2's rule: sign + leading digit + `.` + 16 more digits + `e` + exponent
sign + 3 exponent digits + NUL = **25 bytes**. 32 leaves margin and keeps the
value a round number. (GLib's `G_ASCII_DTOSTR_BUF_SIZE` is 39 because it
budgets for `%.17g` plus locale slack, neither of which applies here.) The
number is fixed in the ABI, so the plan must assert it: a test writes the
worst-case value into a buffer of exactly `AXL_DOUBLE_STR_MAX` and checks it
is not truncated.
