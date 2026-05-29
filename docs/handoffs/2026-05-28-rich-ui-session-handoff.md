# Rich UI session handoff — 2026-05-28

**Tip:** `8d0159c` (16 commits ahead of `21d1e9e`'s start point; ALL
LOCAL, none pushed per [[release-approval-gate]]).

**Tests:** 3699 passed / 0 failed on x64 + aa64.  Ratchet baseline
locked at 3699 in `test/integration/.last-pass-count`.

**Working tree:** clean.

## What landed this session

Sixteen commits across three phases of the Rich UI exploration
plan plus supporting infrastructure.  In commit order, oldest first:

| Commit | Phase | What |
|---|---|---|
| `233a653` | docs | `AXL-Rich-UI-Plan.md` — Lexbor + QuickJS spike plan + AxlGfx 2D upgrade as prereq |
| `ee9fed9` | tooling | `.clangd` CompileFlags for orphan-header include resolution |
| `1478222` | **G1** | AxlTtf — vector text via stb_truetype (NULL/input-validation contract) |
| `953cacd` | **Gx** | `axl_gfx_fill_rect_i` signed-coord variant for off-screen widget rendering |
| `e4863a8` | G1+ | G1 positive tests against embedded DejaVu ASCII subset |
| `338bd79` | **G2** | AxlPixmap — PNG/JPG/GIF/BMP via stb_image |
| `ec86020` | G1 raster | `axl_ttf_draw` produces real pixels (stb_truetype glyph bitmaps + compositing) |
| `9bcaeb2` | **G3** | AxlGfxPath + scanline `fill_path` + `fill_rounded_rect` (hand-rolled, libm-free) |
| `8b98f3c` | refactor | AxlMath — lifted libm-free floor/ceil/fabs/sqrt/sin/cos/fmod into a shared module |
| `4adf2a1` | docs | Sphinx pages — `modules/math.rst`, `modules/truetype.rst`, `modules/pixmap.rst` + README updates |
| `21d1e9e` | plan | Rich UI plan — added G4–G13 (full 2D library) + M1–M10 (AxlMath follow-ups) with dependency graph |
| `e468758` | **M10** | math constants — `AXL_MATH_E`, `AXL_MATH_SQRT_2`, `AXL_MATH_LOG_2`, `AXL_MATH_GOLDEN`, deg↔rad |
| `5ac37e8` | **M5+M9** | clamp / min / max / remap / step / smoothstep + `axl_wrap` (parallel agents both swept into one commit) |
| `7f55130` | style | Module-prefix all public macros — `AXL_PI` → `AXL_MATH_PI` rename + codified in `AXL-Coding-Style.md` |
| `4dee9ac` | perf | AxlMath compile-time HW fast paths — `__builtin_*` lowers `sqrt`/`floor`/`ceil`/`fabs` to single instructions where ISA permits; FMA Horner for sin |
| `8d0159c` | docs | QEMU CPU compatibility section in `src/math/README.md` (forward-looking for bumped `-march`) |

## Current AxlSDK module state

Shipped this session (Rich UI substrate ready for Spike A1/C1):

- **`<axl/axl-truetype.h>` / AxlTtf** — vector text, full surface
  (load/free/measure/measure_prefix/draw/metrics).  Rasterization
  active.  No `axl_ttf_default()` library API yet — consumers load
  their own TTF.
- **`<axl/axl-pixmap.h>` / AxlPixmap** — PNG/JPG/GIF/BMP decode +
  dimensions-without-decode (`axl_pixmap_info`).
- **`<axl/axl-gfx.h>`** — G3 additions: AxlGfxPath
  (move_to/line_to/curve_to/arc/close), `fill_path`, `stroke_path`
  (1-px width fixed), `fill_rounded_rect` (SDF direct, bypasses
  path); plus Gx `axl_gfx_fill_rect_i` signed-coord rect.
- **`<axl/axl-math.h>` / AxlMath** — libm-free shared module:
  `axl_floor` / `axl_ceil` / `axl_floori` / `axl_ceili` / `axl_fabs`
  / `axl_sqrt` / `axl_fmod` / `axl_wrap` / `axl_sin` / `axl_cos` /
  `axl_clamp` / `axl_min` / `axl_max` / `axl_remap` / `axl_step` /
  `axl_smoothstep`; constants `AXL_MATH_PI` / `_HALF_PI` /
  `_TWO_PI` / `_E` / `_SQRT_2` / `_LOG_2` / `_GOLDEN` /
  `_DEG_TO_RAD` / `_RAD_TO_DEG`.  Hardware fast paths
  compile-time-selected via `AXL_MATH_HAS_HW_*` flags.

Sphinx pages exist for all three new modules.  Doxygen scans
`include/axl/*.h` so the API reference auto-includes them.

## Decisions made this session (worth remembering)

1. **Module-prefix all public macros** — codified in
   `docs/AXL-Coding-Style.md` §"Module-prefix all public macros".
   `AXL_OK` / `AXL_ERR` / `AXL_APP` are exempted as
   project-wide infrastructure.  Memory:
   [[module-prefix-macros]].

2. **Compile-time hardware-feature detection** for math intrinsics,
   not runtime.  axl-sdk consumers always build per-target;
   runtime dispatch is per-call overhead for no portability win.
   Selection happens via `AXL_MATH_HAS_HW_SQRT` / `_HW_ROUND` /
   `_HW_FABS` / `_HW_FMA` based on `__SSE2__` / `__SSE4_1__` /
   `__FMA__` / `__aarch64__` preprocessor macros.

3. **`-fno-math-errno` + `-fno-trapping-math`** added to
   `CFLAGS_BASE` — required to let GCC actually inline
   `__builtin_sqrt` etc. instead of emitting libm calls.  We have
   no errno + don't catch FP exceptions; safe for freestanding
   UEFI.

4. **Target CPU floor confirmed: post-2020 Intel + ARM only.**
   Allows `-march=x86-64-v3` (Haswell 2013+) and `-march=armv8.2-a`
   (Ampere Altra 2020+) if/when a consumer wants the perf win.
   Default build stays at `-march=x86-64` for now — no consumer
   yet asking.

5. **Parallel agents inside isolation=worktree work**, but only
   for **truly disjoint file sets**.  M5 + M9 + M10 all touched
   the same three files (`axl-math.{h,c}` + test); M9's commit was
   swept into M5's because both staged simultaneously.  The
   _output_ was correct (all functions present) but commit
   granularity lost.  Future parallel batches: split by FILE,
   not by phase.

6. **G3 implementation: hand-rolled (G3a)**, not Blend2D (G3b).
   ~590 LOC at `src/gfx/axl-gfx-path.c`.  Decision was driven by
   "do it now" pressure; G3b remains an option if Spike A2's
   fidelity targets demand it.

7. **AxlPixmap naming chosen over AxlImage** — `<axl/axl-image.h>`
   is the existing UEFI executable-image lifecycle module.
   Bitmap decode lives at `<axl/axl-pixmap.h>` / module
   `AxlPixmap` to avoid collision.

8. **QEMU CPU compatibility**: default `-march=x86-64` works under
   `qemu64` (SSE2 baseline).  Bumped `-march` would emit ROUNDSD
   (SSE4.1) / VFMADD (FMA3) which `qemu64` lacks — workaround is
   `--qemu-arg -cpu --qemu-arg Haswell` or KVM.  Documented in
   `src/math/README.md`.

## What's next — priority queue per the plan doc

### Immediate (cheap, blocking decisions)

- **Spike A1** — Lexbor compiles + parses HTML inside UEFI.
  1–2h throwaway on branch `spike/lexbor-uefi`.  Kill criteria:
  needs pthreads/mmap/dlopen.  Answers "is HTML/CSS realistic?"
- **Spike C1** — QuickJS compiles + evals ES2020 inside UEFI.
  Same shape, same length, branch `spike/quickjs-uefi`.
  Answers "is the JS runtime realistic?"

**These are not gated on anything else.**  Run them in either
order or in parallel.  The decision gate in the plan doc (see
§"Decision gate") tells you what to do based on outcomes.

### Math substrate (per [[m3-before-g4]] dependency)

- **M3** (`AxlVec2` + `AxlMat3`) — substrate for G4's transform
  stack.  Hard prereq.  Don't start G4 before M3.
- **M2** (`pow` / `exp` / `log`) — soft prereq for G5 quality
  gradients + G6 shadow falloff.  Also un-stubs stb_image's
  gamma path.

### Easy math wins (no dependencies)

- **M1** (`atan2` / `atan` / `asin` / `acos`) — most-asked-for
  math function we don't have.
- **M4** (`lerp` + easing palette) — ~5 LOC each, animation
  primitive.
- **M6** (bit math: `clz` / `ctz` / `popcount` / `log2i`) — thin
  wrappers over `__builtin_*`.
- **M7** (saturated arithmetic) — alpha blending helpers.
- **M8** (geometry: `point_in_rect` / `segment_intersect` / etc.)
- **M9** (`axl_wrap`) — **DONE** in `5ac37e8`.

### Gfx follow-ups (demand-driven, do when consumer asks)

- **G4** (transforms) — biggest leverage but needs M3 first.
- **G5** (gradients) — table stakes for modern UI.
- **G6** (Gaussian blur + shadows) — table stakes.
- **G7** (glyph cache) — perf when AGT goes text-heavy.
- **G8** (stroke styling).
- **G9** (display list / scene graph) — supersedes AGT's
  RecordingDrawContext when it lands.
- **G10** (path-based clip).
- **G11–G13** (multi-line text, pattern fill, blend modes).

See `docs/AXL-Rich-UI-Plan.md` §"Follow-up enhancements" for
effort estimates and dependency notes per phase.

## Discussion topics deferred — surface in next session

- **Compiler intrinsics direct use vs. `__builtin_*`** — the user
  raised this alongside hardware FP.  We landed on `__builtin_*`
  with `-fno-math-errno`; explicit intrinsics (`_mm_sqrt_sd`,
  ACLE `vsqrt_f64`) weren't pursued.  Worth revisiting if
  `__builtin_*` lowering ever proves unreliable on a target.
- **Whether to add a `--cpu` shorthand to `run-qemu.sh`** —
  deferred until a consumer actually bumps `-march`.  The
  `--qemu-arg` passthrough works today.
- **Default-font library API** (`axl_ttf_default()`) — deferred.
  Currently consumers load their own TTF.  AGT will eventually
  want a default for fast-prototype widgets.
- **Glyph cache (G7)** — performance concern, not correctness.
  Trigger: AGT first label-heavy panel that renders sluggishly.

## Hard rules carried forward

- **No push without explicit user approval** per
  [[release-approval-gate]].  All 16 commits stay local.
- **Bucket-A TDD for any new public API** per
  [[tdd-mandatory]]: header + docstring contract → failing
  tests → confirm RED → implement → GREEN → independent
  code-review agent pass → commit.
- **Independent code-review pass** per
  [[code-review-before-commit]] catches the bugs you can't see
  yourself.  This session it caught: rasterizer 180° arc
  collapse, axl_ttf_draw negative-coord truncate-toward-zero,
  `axl_sin` while-loop hang on large input, plus the
  test-tightenings.
- **Pre-validate untrusted bytes before stb-style libs** per
  [[validate-untrusted-bytes-before-stb]] — `sfnt_header_plausible`
  in `axl-truetype.c` and `pixmap_header_recognized` in
  `axl-pixmap.c` are the precedents.  aa64 catches the OOB-read
  bugs x64 silently passes.
- **Module-prefix public macros** per [[module-prefix-macros]].
- **Run both arches at every commit** — x64 + aa64.  Many
  arch-stricter bugs (aa64 MMU) silently pass on x64.

## Files to read first in the next session

1. `docs/AXL-Rich-UI-Plan.md` — the master plan; phase IDs (G/Gx/M/A/B/C) referenced everywhere
2. `docs/ROADMAP.md` §"Rich UI Exploration" — high-level phase tracker
3. This handoff doc (you're reading it)
4. `docs/AXL-Coding-Style.md` §"Module-prefix all public macros" — recent rule
5. `CLAUDE.md` §"Development Workflow — Test-First" — discipline matrix
6. AGT memory dir at `~/.claude/projects/-home-mgosha-projects-aximcode-agt/memory/` if any work touches AGT-supporting changes
