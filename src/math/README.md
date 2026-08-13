# AxlMath — Floating-point Math Primitives

Libm-free implementations of `floor` / `ceil` / `fabs` / `sqrt` /
`fmod` / `sin` / `cos`.

Header: `<axl/axl-math.h>`

## Overview

axl-sdk's freestanding UEFI build links with `-nostdlib` and cannot
rely on libm. GCC's `__builtin_floor` / `__builtin_sin` et al. usually
lower to libm calls on baseline targets (the SSE4.1 ROUND instruction
isn't in the `-march=x86-64` baseline, and there's no hardware sin/cos
on either x64 or AArch64).

AxlMath exposes a small, libm-free implementation of the math
primitives downstream consumers (axl-truetype, axl-gfx-path, future
AGT widget animation, downstream image codecs needing power-of-two
scaling, etc.) actually need. Accuracy is sufficient for UI coordinates
and animation easing — **not** for numerical analysis.

```c
#include <axl.h>

double diag    = axl_sqrt(2.0);            // 1.414...
int    pen_px  = axl_floori(p.x);          // round-down to int pixel
double t       = axl_sin(elapsed * 2.0);   // animation easing
```

All values are `double` so callers can mix integer and floating-point
inputs without precision surprises; consumers that pin `float` storage
cast at the boundary.

## API Shape

| Function | Notes |
|---|---|
| `axl_floor` / `axl_ceil` | `double → double`, libm-shape |
| `axl_floori` / `axl_ceili` | `double → int`, pixel-snap convenience |
| `axl_fabs` | absolute value |
| `axl_sqrt` | Newton's method internally; negative input clamps to 0 |
| `axl_fmod` | `x - trunc(x/y) * y`; zero-divisor returns 0 |
| `axl_sin` / `axl_cos` | 6-term Taylor with constant-time range reduction |
| `axl_isnan` / `axl_isinf` / `axl_isfinite` | IEEE-754 special-value predicates |

Constants `AXL_MATH_PI`, `AXL_MATH_HALF_PI`, `AXL_MATH_TWO_PI` are exposed as
macros for trig callers that want pinned-precision values. `AXL_MATH_INF`,
`AXL_MATH_NAN`, `AXL_MATH_DBL_MAX`, and `AXL_MATH_DBL_TRUE_MIN` expose the
IEEE-754 special values and range limits (built via division rather than
`<float.h>`/`INFINITY`, which the freestanding build has neither of).
**Never compare against `AXL_MATH_NAN` with `==`** — NaN is unequal to
itself by definition, so `x == AXL_MATH_NAN` is always false regardless of
whether `x` is NaN; use `axl_isnan(x)` instead.

## Accuracy

| Function | Error bound | Notes |
|---|---|---|
| `floor`, `ceil`, `floori`, `ceili` | exact | Integer-cast + sign correction |
| `fabs` | exact | |
| `sqrt` | ~1e-12 | 10 Newton iterations |
| `fmod` | ~1e-12 | Truncated quotient |
| `sin`, `cos` | ~1e-7 | 6-term Taylor through `x¹¹/11!` |
| `isnan`, `isinf`, `isfinite` | exact | pure comparisons, no approximation |

## Hardware fast paths

Each hardware-pathable primitive (`sqrt` / `floor` / `ceil` / `fabs`)
has both a `__builtin_*` hardware path that lowers to a single CPU
instruction AND a libm-free manual fallback that's correct on any
target. Selection is **compile-time** per `-march` via
`AXL_MATH_HAS_HW_*` flags inside `src/math/axl-math.c` — no runtime
dispatch, no per-call branch. `--gc-sections` strips the unused
branch from the link.

| `-march` baseline | sqrt | floor / ceil | fabs | sin/cos Horner |
|---|---|---|---|---|
| `x86-64` (default) | `SQRTSD` (SSE2) | manual fallback | `ANDPD`-mask (SSE2) | mul + add |
| `x86-64-v2` (Nehalem 2008+) | `SQRTSD` | `ROUNDSD` (SSE4.1) | `ANDPD` | mul + add |
| `x86-64-v3` (Haswell 2013+) | `SQRTSD` | `ROUNDSD` | `ANDPD` | `VFMADD` |
| `armv8-a` (any AArch64) | `FSQRT` | `FRINTM` / `FRINTP` | `FABS` | `FMADD` |

`sin` / `cos` evaluate a 6-term Horner polynomial; on targets with
hardware FMA the inner loop uses `AXL_MATH_FMA` for one
fused-multiply-add per term (~30% faster + one extra bit of
precision per term).  Without FMA, the same expression compiles to
plain `mul + add`.

Build with `make CFLAGS_EXTRA='-march=x86-64-v3'` (or higher) to
opt into the additional fast paths.  axl-sdk's `-fno-math-errno`
and `-fno-trapping-math` flags are what let GCC actually inline
`__builtin_sqrt` etc. instead of emitting libm calls — these are
on by default in our Makefile (the freestanding UEFI build has no
errno + we don't catch FP exceptions).

### QEMU CPU compatibility

For the default `-march=x86-64` baseline (the only path tested in
CI) **no QEMU configuration changes are needed**.  The two
hardware paths active at baseline — `SQRTSD` and `ANDPD`-as-fabs
— are in the SSE2 ISA which QEMU's default `qemu64` model
exposes, and AArch64's `-cpu cortex-a57` (set by
`scripts/axl-common.sh`'s `build_qemu_base_cmd`) covers every
ARMv8-A baseline instruction we emit.

If a consumer bumps to `-march=x86-64-v2` or higher, the binary
will contain `ROUNDSD` (SSE4.1) or `VFMADD132SD` (FMA3) which
are NOT in `qemu64`.  Two options to test such a build:

1. **Run with `-cpu host` and KVM available.**  `scripts/run-qemu.sh`
   already does this automatically when `/dev/kvm` is readable +
   writable.  Modern hosts (post-2008 Intel, post-Bulldozer AMD)
   have all the features axl-sdk can request.
2. **Pass an explicit CPU model via `--qemu-arg`.**  Example for
   a Nehalem+ build:

   ```bash
   ./scripts/run-qemu.sh --qemu-arg -cpu --qemu-arg Nehalem hello.efi
   ./scripts/run-qemu.sh --qemu-arg -cpu --qemu-arg Haswell hello.efi
   ```

   QEMU's CPU model names match the `-march` levels closely
   (`Nehalem` ≈ `x86-64-v2`, `Haswell` ≈ `x86-64-v3`).

Without one of these, a bumped-`-march` binary will hit a
`#UD` (illegal opcode) trap on the first hardware-only
instruction.  The symptom is a QEMU `EXCEPTION` log line and
firmware death, NOT a graceful error — the CPU literally doesn't
know the instruction.

This isn't documented elsewhere — flag for AGT / downstream
consumers if they ever bump `-march` for perf.

UI consumers (widget rendering, layout, animation easing) get more
than enough precision. Numerical-analysis consumers should look
elsewhere.

## Range Reduction (sin/cos)

`axl_sin` reduces the input to `[-π/2, π/2]` before evaluating the
Taylor series. The reduction is **constant time** —
`x -= 2π * floor((x + π) / 2π)` lands `x` in one period regardless
of magnitude. The previous file-static duplicates (in axl-truetype
and axl-gfx-path) used `while (x > π) x -= 2π;` which hung
multi-second on inputs like `1e9`. The shared module removes that
footgun.

Behavior on NaN or `±∞` is implementation-defined (the floor cast
hits int64 overflow). UI inputs are bounded well below that.

## Linking and Selective Pull-in

Each function is its own translation unit's `.text.axl_*` section,
so `--gc-sections` drops everything you don't reference. A tool that
only calls `axl_floor` doesn't pay for `axl_sin`.

## Substrate Discipline

AxlMath is a foundational primitive. Higher-level modules (axl-gfx,
axl-truetype, axl-gfx-path) dogfood it; toolkits on top (AGT,
downstream codecs) should too. The rule per
[CLAUDE.md](../../CLAUDE.md): no more file-static `floor` clones.
