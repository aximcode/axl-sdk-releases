# AXL Transform Design

Status: **design, pre-implementation.** Target **axl-sdk v0.22.0**
(AGT dependency floor). This document is the contract AGT adopts and
the spec the implementation follows. Everything below is settled
unless marked *open*.

## Purpose

A single, general 2D transform type for the whole library — the
retained-mode gfx CTM, the explicit transform passed to rendering
primitives, and the matrix AGT owns. It supersedes **two** existing
types:

- `AxlMat3` (axl-math, double, 3×3) — today's internal gfx CTM.
- `AxlGfxAffine` (gfx, float, 2×3, cairo) — today's explicit
  toolkit transform (shipped `6b59fd6b`, no external adopter yet).

Both are **removed**. There is one type, `AxlTransform`, in
**axl-math**.

### Why one type, why in math

`AxlMat3` is *already* the gfx CTM (`transform_current` /
`transform_stack` in `src/gfx/axl-gfx.c`, every path point mapped
through `axl_mat3_transform_point` in `src/gfx/axl-gfx-path.c`). The
transform is therefore provably pure value math with no rendering
coupling. `AxlGfxAffine` existed in gfx only for three *conventions* —
float, cairo multiply order, pairing with `AxlGfxPointF` — none
fundamental. So we absorb the affine type into the math transform
rather than carry a parallel one. This matches Qt `QTransform` /
Skia `SkMatrix`: one transform in the core/geometry layer, consumed
by the renderer. cairo's affine/projective split is the outlier.

## Decisions (locked)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Count | One type, `AxlTransform` | Qt/Skia model; kills the dual-type / dual-multiply-order trap |
| Home | `axl-math` (`include/axl/axl-math.h`) | The transform is pure math; already runs as the CTM |
| Storage | Fixed `double m[9]`, 3×3, value type, no heap | Hot path (every path point); unrolled beats dynamic; headless-safe |
| Generality | Fixed 3×3 — **not** arbitrary N×M | No runtime-dimensioned consumer; C has no template escape; future fixed types added per the contract below |
| Precision | double | Matches AxlMath; better projective accumulation; gfx narrows to float at the sample loop |
| Multiply order | **cairo a-first**: `multiply(a, b)` applies `a` then `b` | AGT/cairo convention (the `6b59fd6b` reconcile) |
| Classification | **derived on demand** from `m[]`, never consumer-set | A mutable kind flag can contradict the entries; on-demand classify is cheap (once per draw, not per point) |
| Point type | `AxlVec2` (double); gfx adapts `AxlVec2`↔`AxlGfxPointF` at the boundary | One math point type; gfx float conversion stays at the edge |
| Projective | **Full** — type, math, *and* perspective-correct rasterization | A near-term consumer renders warped/perspective content |

### Multiply-order reconcile is low-risk

`axl_mat3_mul(current, local)` (B-first) and
`axl_transform_multiply(local, current)` (cairo a-first) produce the
**identical** matrix `current·local`. The CTM re-derivation
(`axl_gfx_translate/scale/rotate/skew`) is an argument-order swap at
the call sites, not new math. Existing gfx CTM tests are the safety
net (test-protected refactor).

## Type

```c
/// General 2D transform — 3×3 homography over [x y 1]ᵀ column
/// vectors. Subsumes identity / translate / scale / rigid /
/// similarity / affine / projective. Value type; no heap.
///
/// Flat 9-element row-major array:
///   [ m[0] m[1] m[2] ]      affine decode: [ a b tx ]
///   [ m[3] m[4] m[5] ]                     [ c d ty ]
///   [ m[6] m[7] m[8] ]      projective row: [ g h w ]
/// Identity = [1 0 0; 0 1 0; 0 0 1]. Affine ⇒ bottom row [0 0 1].
typedef struct AxlTransform {
    double  m[9];
} AxlTransform;

/// Classification of a transform, derived from its contents.
/// Ordered by generality; each kind is a superset of the prior.
typedef enum AxlTransformClass {
    AXL_TRANSFORM_IDENTITY = 0,  ///< exactly the identity
    AXL_TRANSFORM_TRANSLATE,     ///< identity linear part + translation
    AXL_TRANSFORM_SCALE,         ///< axis-aligned scale (+translate)
    AXL_TRANSFORM_AFFINE,        ///< bottom row [0 0 1], general linear
    AXL_TRANSFORM_PROJECTIVE,    ///< non-trivial bottom row (perspective)
} AxlTransformClass;
```

## API surface (v0.22.0)

Builders (return by value):

```
axl_transform_identity(void)
axl_transform_translate(double tx, double ty)
axl_transform_scale(double sx, double sy)
axl_transform_rotate(double radians)            // [c -s; s c]
axl_transform_shear(double shx, double shy)     // tan-of-angle, CSS skew()
axl_transform_perspective(double px, double py)  // bottom row [px py 1]
axl_transform_quad_to_quad(const AxlVec2 src[4], const AxlVec2 dst[4],
                           AxlTransform *out)    // bool; false if degenerate
```

`quad_to_quad` corner order is **TL, TR, BR, BL** (clockwise in
y-down device space). Closed form: `square→dst ∘ (square→src)⁻¹`
via the standard unit-square homography — no general solver.

Ops:

```
axl_transform_multiply(AxlTransform a, AxlTransform b)   // apply a then b
axl_transform_invert(AxlTransform m, AxlTransform *out)  // bool; false if singular
axl_transform_determinant(AxlTransform m)                // double
```

Mapping (perspective divide on the projective path; skipped when
`is_affine`):

```
axl_transform_map_point(AxlTransform m, AxlVec2 p)   // full, divides by w
axl_transform_map_vector(AxlTransform m, AxlVec2 v)  // linear part only
axl_transform_map_rect(AxlTransform m, AxlRect r)    // AxlRect: axis-aligned bbox of mapped corners; exact when axis-aligned
axl_transform_map_quad(AxlTransform m, const AxlVec2 in[4], AxlVec2 out[4])
```

Classification (pure functions of `m[]`):

```
axl_transform_classify(AxlTransform m)         // AxlTransformClass
axl_transform_is_identity(AxlTransform m)      // bool
axl_transform_is_axis_aligned(AxlTransform m)  // bool — rect stays a rect
axl_transform_is_affine(AxlTransform m)        // bool — bottom row [0 0 1]
```

### Invariants (pinned by tests)

- `multiply(a, identity) == multiply(identity, a) == a`
- associativity: `multiply(multiply(a,b),c) == multiply(a,multiply(b,c))`
- `invert(invert(m)) == m` (incl. a projective case), within tolerance
- `map_point(quad_to_quad(s,d), s[i]) == d[i]` for each corner
- classification boundaries (identity vs translate vs scale vs affine
  vs projective), and affine/axis-aligned fast paths return the same
  result as the general path

## Rendering scope — full projective

Primitives migrate and gain perspective-correct paths:

| Removed | New |
|---------|-----|
| `axl_ttf_draw_affine(font,utf8,px,&m,color)` | `axl_ttf_draw_transform(font, utf8, px, const AxlTransform*, color)` |
| `axl_gfx_blit_affine(src,&m)` | `axl_gfx_blit_transform(src, const AxlTransform*)` |
| — | `axl_gfx_push_clip_rect_transformed(rect, const AxlTransform*)` |
| `axl_gfx_push_clip_quad(q[4])` | kept (takes `AxlGfxPointF[4]`) |

Each primitive `classify`s once, then dispatches: identity/translate
→ fastest blit; affine → existing outline-transform / bilinear path;
projective → perspective-correct outline flattening (text) and
perspective-correct sample interpolation (blit). The type never
promises perspective the renderer won't draw.

## Removals / migration

- Delete `AxlMat3` and all `axl_mat3_*` → renamed to
  `axl_transform_*`. Internal callers (gfx CTM, path) migrate;
  argument order swaps for the multiply-order reconcile.
- Delete `AxlGfxAffine`, all `axl_gfx_affine_*`, and the
  `axl_gfx_affine_to_mat3/_from_mat3` converters (one type — nothing
  to bridge).
- `AxlGfxPointF` stays (gfx float point for clip corners); adapt
  to/from `AxlVec2` at the gfx boundary.
- Update `src/math/README.md`, `src/gfx/README.md`, and the Sphinx
  pages that referenced `AxlMat3` / `AxlGfxAffine`.

## Extensibility — adding a future transform type

We deliberately chose fixed-size types over an arbitrary-dimension
matrix library. C has no templates, so "easy to extend" here means a
**stamped contract**, not a vtable or generic: every transform type
follows the same naming, semantics, and integration pattern, so a new
one drops in mechanically and reads identically to `AxlTransform`.

A future type is justified only by a **concrete consumer** (no
speculative types). Candidates and their triggers:

| Type | Shape | Trigger |
|------|-------|---------|
| `AxlMat4` + `AxlVec3`/`AxlVec4` | 4×4 double | a real 3D consumer (e.g. AGT card-flip / perspective UI in 3D) |
| `AxlColorMatrix` | 4×5 double | a color-filter effect (SVG `feColorMatrix` / Skia `SkColorMatrix`) |
| `AxlMat2` | 2×2 double | a pure-linear (no-translate) consumer that warrants its own type |

### The contract every transform type follows

1. **Value type, fixed storage, no heap.** Flat `double m[]`,
   row-major, documented index layout. Headless-safe.
2. **Naming**: `Axl<Name>` type; `axl_<name>_<verb>` free functions.
   No module prefix on the type (matches `AxlVec2`/`AxlTransform`) —
   these are math types.
3. **Standard verbs, same semantics**: `identity`, the relevant
   builders, `multiply(a, b)` = **apply `a` then `b`** (cairo
   a-first, uniform across all types), `invert → bool` (false if
   singular, `out` untouched), `determinant`, the relevant
   `map_*`/`apply_*`.
4. **Derived classification, never set.** If the type has meaningful
   sub-kinds (as `AxlTransform` does), expose a `classify` returning a
   generality-ordered enum plus `is_*` predicates, all pure functions
   of the storage. No stored/settable kind field.
5. **Rasterizer/consumer integration is by `classify` dispatch**, not
   by type-switching at call sites: a primitive classifies once and
   takes the cheapest correct path. New types and new classes extend
   the lattice without touching unrelated call sites.
6. **Tests pin the same invariant set**: identity, associativity,
   invert round-trip, builder round-trips, classification boundaries,
   fast-path-equals-general-path.

### Extending the classification lattice

`AxlTransformClass` is generality-ordered and `0`-based; new kinds
insert in generality order (a non-breaking recompile pre-1.0). Fast
paths test `<=`/`>=` against a class, so a new intermediate kind (e.g.
a `SIMILARITY` between `SCALE` and `AFFINE`) slots in by adding the
enumerator and one classify branch — existing `is_affine` /
`is_axis_aligned` predicates keep working.

Adding `AxlMat4` does **not** generalize `AxlTransform` into it (no
N×M base type, no shared storage). They are independent types bound by
the same written contract — which is what keeps them consistent
without C++ machinery.

## Phased implementation

1. Rename `AxlMat3`→`AxlTransform`, swap CTM call-site order, remove
   `AxlGfxAffine` + converters. Behavior-preserving; existing tests
   stay green (test-protected refactor).
2. Projective type + math: `perspective`, `quad_to_quad`, `map_point`
   divide, `map_rect`/`map_quad`, `classify` + predicates. Strict TDD.
3. Migrate primitives: `draw_transform` / `blit_transform` /
   `push_clip_rect_transformed`; affine paths exact.
4. Perspective-correct rasterization (the hard half): projective
   outline flattening + perspective-correct sampling + projective
   clip. Exact-value/checksum render tests. *(Recommend a
   throwaway `spike/perspective-raster` to settle the approach
   before writing committed tests.)*
5. Docs/README, clang-tidy changed files, both arches green, ratchet
   bump, independent review, commit. Publish final symbol list for
   AGT.

## Open / deferred

- Deferred (no consumer): `AxlMat4`, `AxlColorMatrix`, transform
  decompose/lerp (animation), G9 display-list transform op.
