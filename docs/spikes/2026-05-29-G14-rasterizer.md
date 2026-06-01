# Spike G14 — analytic path rasterizer: borrow vs build

**Branch:** `spike/g14-rasterizer` (throwaway, NOT merged).
**Date:** 2026-05-29. Time-boxed per [[spike-solutions]].

**Question (from AD1):** replace `axl_gfx_fill_path`'s 4×4-supersampled
even-odd rasterizer with analytic coverage — borrow `nanosvgrast.h`,
borrow FreeType `ftgrays.c` standalone, or hand-roll? Which actually
compiles freestanding, and which integrates cleanly with our
already-flattened edge list?

## Integration surface (current code)

`fill_path_paint_` in `src/gfx/axl-gfx-path.c` consumes the path's
flattened point list (`p->pts[]`, world-space doubles, `.is_move`
flags — arcs already sampled to line segments) and, per pixel, does a
16-subsample even-odd test → coverage 0–16 → modulates alpha →
`axl_gfx_fill_rect_i` (or per-pixel gradient sample). It recomputes
scanline intersections **per pixel column** (an O(W) redundancy on top
of the 16× sampling), so analytic wins on both quality and that factor.
A replacement needs: edges in → per-pixel (or per-span) 8-bit coverage
out → existing alpha-modulate + blit/gradient path. Add non-zero
winding alongside even-odd.

## Candidate A — nanosvgrast.h (MIT)

- Pulls in the full `nanosvg.h` SVG parser (`fopen`, heavy string
  parsing) — unwanted.
- External surface: `malloc`/`free`/`realloc` (arena), `qsort` (edge
  sort), `memset`/`memcpy`, and math `sqrtf`/`cosf`/`sinf`/`acosf`/
  `fmodf`/`ceilf`/`floorf` (mostly in the SVG arc-flattening we don't
  need, but in the same file).
- Quality: 5× **vertical supersampling** + exact horizontal coverage
  (`NSVG__SUBSAMPLES 5`) — better than our 4×4 but **not pure
  analytic**.
- **Architecturally coupled to the SVG shape/paint model.** Clean use
  means either forking `nsvg__rasterizeSortedEdges` out of the file
  (violates AD1 "don't fork vendored code") or constructing fake
  `NSVGimage`/shape/paint (wrong abstraction, drags the parser).
- **Verdict: weakest fit.** The "borrow" is really a fork, at lower
  quality, with a larger dep surface.

## Candidate B — FreeType ftgrays.c, STANDALONE_ (FTL)

Empirically tested (this is the headline result):

- Compiled with the **exact axl freestanding flags**
  (`-ffreestanding -fno-builtin -fno-math-errno -mno-red-zone
  -march=x86-64 -DSTANDALONE_`) into a clean 14KB `.o`.
- **Entire external-symbol surface (`nm -u`): `malloc`, `free`,
  `memset`.** Nothing else. → route to `axl_malloc`/`axl_free`/
  `axl_memset`, exactly the stb_truetype pattern.
- **No setjmp/longjmp at runtime** — the `#include <setjmp.h>` is
  vestigial; modern ftgrays recovers from cell-buffer overflow with a
  return code + re-banding loop (`Raster_Overflow`, `gray_convert_glyph`),
  not longjmp. (This was the feared friction point in AD1/the FreeType
  discussion — it does not exist.)
- **No libm** — pure integer / 26.6 fixed-point cell coverage.
- One controllable allocation per render (`FT_QNEW_ARRAY`/`FT_FREE`
  macros → axl, or a fixed static cell buffer to stay heap-free).
- Shim needed: ~6 trivial defs the newer standalone branch omits
  (`FALL_THROUGH`, `FT_ULong/FT_Long/FT_Error/FT_Memory`,
  `FT_QNEW_ARRAY`, `FT_FREE`) + vendor `ftimage.h` (~1.3K lines, mostly
  doc comments + the `FT_Outline`/`FT_Raster`/`FT_Span` types) and
  `ftgrays.h` (~50 lines).
- Quality: **pure analytic exact-area coverage — the reference
  implementation** ("FreeType-smooth"); same family as AGG / font-rs.
- Output: **direct span callback** (`FT_RASTER_FLAG_DIRECT` →
  `FT_Span{x, len, coverage}` per scanline run) — maps cleanly to "blit
  a run at 8-bit coverage", no intermediate bitmap. Clean fit for both
  solid and per-pixel gradient.
- Integration cost: marshal our double edge list → `FT_Outline` (26.6
  fixed-point points, all `ON` tags since pre-flattened, contour array)
  + a y-flip; adopt the span callback.
- **Verdict: strongest borrow.** Reference quality, freestanding-clean
  (3 symbols), clean rasterizer-as-rasterizer abstraction.

## Candidate C — hand-roll signed-area rasterizer

- ~200–250 LOC; consumes our edge list directly (no `FT_Outline`
  marshalling); zero new headers, zero new license, integer fixed-point.
- Same algorithm ftgrays/stb_truetype-v2 use (signed-area coverage) —
  well documented. We already *link* stb's version for text, confirming
  the approach + rough size.
- Cost: we own correctness/robustness (cell edge cases, band overflow,
  coverage accumulation) that ftgrays has hardened over 20+ years.
- **Verdict: viable fallback.** Competitive *because our edges are
  already flattened* (the hard part — curve flattening + cell
  robustness — is where ftgrays earns its keep; with simple line edges
  the gap narrows).

## FreeType licensing + component extractability (follow-up research)

Verified against the FreeType master tree (`LICENSE.TXT` + module
scan), since ftgrays is part of FreeType and we wanted to know what we
can and can't freely pull.

- **Licensing — not a blocker.** FreeType is dual-licensed **FTL** or
  **GPLv2+**; we take **FTL**: BSD-style with one extra obligation, a
  **credit clause** (acknowledge FreeType in our product
  documentation) plus reproducing the license text. No copyleft, no
  source disclosure. Per-component licenses are all permissive +
  compatible (BDF/PCF + `base/fthash.c` = X11-style; `gzip` = zlib;
  `autofit/ft-hb-*` = Old MIT; MD5 = public domain). FTL is
  GPLv2-incompatible (the credit clause) — which is *why* the GPLv2
  alternate exists; irrelevant to us (not a GPL project). Slots into
  our existing third-party-attribution process.
- **Extractability — only the rasterizers come out cheaply.** Of 28
  `src/` modules, **only `smooth/ftgrays.c` (AA) and `raster/
  ftraster.c` (mono) declare `STANDALONE_`.** Everything else — the
  bytecode hinting VM (`truetype/ttinterp.c`), `sfnt`, drivers,
  `autofit`, `cache` (FTC), `sdf` — `#include <freetype/internal/...>`
  and is welded to the FT base object model (verified: `ttinterp.c`
  and `ftsdf.c` both pull `internal/ftobjs.h`).
- **Consequence:** no cherry-pick middle path. ftgrays for the
  *rasterizer* (cheap, this spike) **or** the whole library as the
  `AXL_FREETYPE=1` backend for hinting/formats (a real port). Hinting
  is inseparable from the TrueType driver + base. Reinforces plan AD4.

## Recommendation — DECIDED 2026-05-29: borrow ftgrays (B)

**Primary: borrow ftgrays.c standalone (B).** The freestanding result
is decisive — 3 external symbols, no setjmp, no libm, reference-quality
analytic coverage, clean span-callback output. It's the rasterizer the
peer libraries' quality is measured against, and it drops in behind
`axl_gfx_fill_path` with only a small `FT_Outline` marshalling shim.
Costs are FTL attribution (fine, BSD-style) and vendoring `ftimage.h` +
`ftgrays.h`.

**Fallback: hand-roll (C)** if, in implementation, the `FT_Outline`
marshalling or the FTL/header vendoring proves more friction than a
~200-line self-contained rasterizer that fits our edge list natively.

**Reject nanosvgrast (A)** — SVG-coupled (clean use = a fork that
violates AD1), lower quality (supersampled, not analytic), larger dep
surface.

Either way: add non-zero winding alongside even-odd, compute coverage
once per scanline (kills the per-column redundancy), and **delete the
4×4 supersample outright** (no compat mode — no consumer, per AD1).

## Evidence (reproduce)

```sh
# ftgrays freestanding compile + symbol surface
curl -sSL -o ftgrays.c  https://gitlab.freedesktop.org/freetype/freetype/-/raw/master/src/smooth/ftgrays.c
curl -sSL -o ftimage.h  https://gitlab.freedesktop.org/freetype/freetype/-/raw/master/include/freetype/ftimage.h
curl -sSL -o ftgrays.h  https://gitlab.freedesktop.org/freetype/freetype/-/raw/master/src/smooth/ftgrays.h
# wrap.c: define FALL_THROUGH, FT_ULong/Long/Error/Memory, FT_QNEW_ARRAY, FT_FREE; #include "ftgrays.c"
gcc -std=gnu2x -ffreestanding -fno-builtin -fno-math-errno -mno-red-zone \
    -march=x86-64 -DSTANDALONE_ -c wrap.c -o ftgrays.o
nm -u ftgrays.o          # => U free / U malloc / U memset  (only these)
```
