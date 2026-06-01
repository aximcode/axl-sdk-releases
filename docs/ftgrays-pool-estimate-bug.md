# Upstream FreeType bug report (draft): ftgrays pool-estimate underflow

Draft for submission to the FreeType project
(https://gitlab.freedesktop.org/freetype/freetype/-/issues). axl-sdk
vendors `ftgrays.c` in `STANDALONE_` mode and carries a local patch for
this; filing upstream so the patch can be retired.

---

**Title:** `gray_raster_render`: pool-size `estimate` mixes x/y spans and
can underflow to a huge value

**Component:** `src/smooth/ftgrays.c`

**Version:** present on `master` (verified) and long-standing.

## Description

In `gray_raster_render`, the cell-pool size estimate reads:

```c
/* allocate memory based on empirical estimate from CJK fonts */
estimate = ( ras.max_ex - ras.min_ey + ras.max_ey - ras.min_ey ) * 10UL;
```

The first term `ras.max_ex - ras.min_ey` mixes the **x**-extent's end
with the **y**-extent's start. By symmetry with the second term
`ras.max_ey - ras.min_ey` (the y-span), it is clearly intended to be the
x-span `ras.max_ex - ras.min_ex`.

`min_ex/max_ex/min_ey/max_ey` are `TCoord` (`int`). When a clip box (or
the outline cbox in direct mode) satisfies `max_ex < min_ey` — a region
whose x-extent ends, in integer pixels, below where its y-extent starts;
e.g. a narrow shape positioned at a large y offset, such as a 90°-rotated
glyph or a thin tall span high on a large target — the first term is
negative. The subexpression is then promoted by `* 10UL` to a very large
`unsigned long`, so `estimate` is enormous.

`estimate` then exceeds `FT_MAX_GRAY_POOL` and the heap path is taken:

```c
if ( estimate > FT_MAX_GRAY_POOL )
{
  ...
  if ( FT_QNEW_ARRAY( ras.buffer, estimate ) )
```

With FreeType's size-checked allocator this fails cleanly
(`Raster_Overflow`) and the glyph simply does not render — a silent,
rare cosmetic glitch, which is presumably why it has gone unnoticed
(normal glyph rasterization uses origin-relative coordinates where
`min_ey ≈ 0`, so `max_ex < min_ey` almost never holds).

However, `estimate` is only a heuristic; driving it to a bogus huge
value is wrong regardless, and any integrator whose `STANDALONE_`
`FT_QNEW_ARRAY` is not overflow-hardened will compute a wrapped (small)
byte size, under-allocate, and corrupt memory.

## Fix

```diff
- estimate = ( ras.max_ex - ras.min_ey + ras.max_ey - ras.min_ey ) * 10UL;
+ estimate = ( ras.max_ex - ras.min_ex + ras.max_ey - ras.min_ey ) * 10UL;
```

This is safe: `estimate` is purely a sizing hint —
`gray_convert_glyph` already bands-and-retries on
`Smooth_Err_Raster_Overflow`, so any mis-estimate only affects how many
band passes are made, not correctness. With the fix the affected
content rasterizes normally.

## Reproduction (standalone)

Render any outline through `FT_RASTER_FLAG_DIRECT` with a
`clip_box` whose `xMax < yMin` (e.g. `{xMin:5, yMin:50, xMax:12,
yMax:90}`). With an overflow-checked allocator the glyph fails to
render; with an unchecked `STANDALONE_` `FT_QNEW_ARRAY` it corrupts the
heap.
