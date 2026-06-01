# Session handoff — transform-aware rendering, blend modes, matrix ops (2026-05-31)

## TL;DR state

- Branch `main`, **19 commits ahead of `origin/main`** (last release tag
  `v0.21.0`), **unpushed and untagged**. Working tree clean.
- **4511/4511 unit tests, 0 failures, both X64 + AARCH64.** clang-tidy clean.
- All new public symbols target **axl-sdk v0.22.0** (VERSION is still
  `0.21.0` — bump happens at release).
- **DO NOT push or cut a release without explicit user approval.** The
  user builds AGT against this checkout via `AXL_SDK_SRC`; the version
  number (v0.22.0) is what AGT references for its dependency floor.

## What shipped this session (top of the unpushed stack)

Driven by sister project **AGT** (aximcode/agt, C++ UEFI widget toolkit)
adopting a Qt/GTK/cairo coordinate model. AGT owns the matrix and feeds
AxlGfx pre-transformed geometry; AxlGfx stays paradigm-agnostic.

| Commit | What |
|--------|------|
| `8fd16bfe` | Matrix/vector ops: AxlMat3 inverse/determinant/transform_vector; AxlVec2 lerp/distance/perp/cross/rotate/angle/reflect/project; AxlGfxAffine transform_point/vector + **AxlGfxAffine↔AxlMat3 converters** |
| `ec0ea8af` | G13 review fixes: screen-target blend-mode consistency + `axl_gfx_reset_blend_mode` |
| `6b59fd6b` | **Affine API reconcile for AGT**: `affine_multiply` flipped to cairo order, `affine_rotate(double)`, `affine_invert` |
| `9e05226b` | **G13 blend modes** (over/multiply/screen/overlay/darken/lighten/add) |
| `4d590fc6` | Deferred affine coverage tests + FreeType ftgrays bug-report draft |
| `c9a5606c` | gfx README: transform-aware rendering + quad clip docs |
| `558a0341` | **`axl_gfx_blit_affine`** — transform-aware image blit (bilinear) |
| `6e72cbf4` | Harden ftgrays `FT_QNEW_ARRAY` shim vs size_t overflow |
| `2a30041f` | **`axl_ttf_draw_affine`** + **fix ftgrays pool-estimate underflow** |
| `4986cab0` | `AxlGfxAffine`/`AxlGfxPointF` + builders + **`axl_gfx_push_clip_quad`** |

(Below these, also unpushed from prior sessions: `91a93e66`/`ac7849e1`
AxlFormat dtoa+%f/%e/%g, `0093f5c8` color_parse, `aa10634c`…`aaad84ca`
G9 display list, `a998feab` G11 boxed text.)

## Public surface added (for AGT to gate on — all v0.22.0)

- Types: `AxlGfxAffine` {xx,yx,xy,yy,x0,y0} (cairo 2×3), `AxlGfxPointF`,
  `AxlGfxBlendMode`.
- Affine builders/ops: `axl_gfx_affine_identity/translate/scale/rotate/
  multiply/invert/transform_point/transform_vector`,
  `axl_gfx_affine_to_mat3`/`_from_mat3`.
- Clip: `axl_gfx_push_clip_quad(const AxlGfxPointF q[4])`.
- Text: `axl_ttf_draw_affine(font, utf8, px_size, &m, color)`.
- Blit: `axl_gfx_blit_affine(src, &m)`.
- Blend: `axl_gfx_set/get/reset_blend_mode`, `axl_gfx_blend_ex`.
- AxlMath: `axl_mat3_inverse/determinant/transform_vector`,
  `axl_vec2_lerp/distance/perp/cross/rotate/angle/reflect/project`.

## Decisions / gotchas a future session MUST know

- **`axl_gfx_affine_multiply(a, b)` applies `a` FIRST then `b`** (cairo
  `cairo_matrix_multiply` order). This was FLIPPED from b-first this
  session. Rotate-about-a-point is `multiply(rotate(t), translate(x,y))`.
- **Two intentional 2D-affine types, kept on purpose:** `AxlMat3`
  (axl-math, double, 3×3, general/precise — the gfx CTM) and
  `AxlGfxAffine` (gfx, float, 2×3, cairo, toolkit-facing). Bridge with
  the converters. **Their multiply orders DIFFER on purpose**:
  `axl_mat3_mul` is B-first (linear algebra), `affine_multiply` is
  a-first (cairo). Converters change representation only.
- **`affine_rotate` takes `double`** (matches `axl_gfx_rotate` + the
  double `AXL_MATH_*` constants — no cast at call sites).
- **Blend modes apply on BUFFER targets only.** Screen (GOP) targets
  uniformly ignore them (render to a back-buffer). The opaque fast path
  is gated on `blend_is_overwrite` = opaque AND mode==OVER.
- **The ftgrays bug** ([[reference_ftgrays_estimate_underflow]],
  `docs/ftgrays-pool-estimate-bug.md`): an UPSTREAM FreeType typo
  (`max_ex - min_ey`, should be `-min_ex`) that hangs/corrupts
  `fill_path` on narrow-x/high-y bboxes (rotated/high-offset glyph or
  rect). FIXED + shim hardened. Local patch to vendored `ftgrays.c` —
  **re-apply on any FreeType re-vendor.** Diagnosis recipe in the
  reference memory. AGT rotated-rect rendering was also exposed to it.

## Open / next (all gated on explicit user go)

1. **Release v0.22.0** — bump-version.sh → tag → release flow, ONLY on
   explicit "cut v0.22.0". AGT's floor rises to it.
2. **File the FreeType upstream bug report** (`docs/ftgrays-pool-estimate-bug.md`
   is a ready draft; not filed — user's call; can't post to their GitLab
   from here).
3. **Deferred follow-ups (flagged in reviews, no consumer yet):**
   - G9 display list doesn't record `set_blend_mode` — wants a blend-mode
     op like the transform/clip ops it already records.
   - Transform `decompose`/`lerp` (animation between transforms) — hold
     until an animation consumer is concrete.
4. **Roadmap remainder:** G10 (path-based clip), G12 (pattern fill /
   tile blit), then **Phase G-FT** (full FreeType bundle, mbedtls-scale).
   Matrix follow-ups: NOT Vec3/Vec4/Mat4/quaternions (no 3D consumer).

## Working discipline (enforce — see CLAUDE.md + memory)

- **Test-first** for new public API; exact-string/exact-value assertions.
  Confirm RED before implementing.
- **Build + run BOTH arches** (`./test/integration/test-axl.sh --arch X64`
  / `--arch AARCH64`), wrap in `timeout 200s`. Read the "Results: N
  passed, 0 failed" line before trusting.
- **Independent code-review agent between all-green and commit** (caught
  real coverage gaps every slice this session).
- **clang-tidy** the changed `src/` files (`clang-tidy -p . -quiet <f>`;
  needs the `bear` compile DB — `bear -- make tests tools` if stale).
- Ratchet auto-bumps `.last-pass-count`; set it to the verified count.
- `TEST_APPS_ONLY="AxlTestGfx"` env override on test-axl.sh runs one
  binary (fast local iteration; added this session).
- **Do NOT push; do NOT release** without explicit approval.
