# Handoff — Rich UI G-phases + axl-utils consumer round (2026-05-29)

Session-close snapshot so a fresh session can continue without
re-deriving context.

## TL;DR state

- Branch `main`, tip **`49c158ca`**, working tree **clean**.
- **5 commits ahead of `origin/main`** (everything post-tag `v0.21.0`),
  **all local / UNPUSHED**, held pending explicit push approval
  (see `[[release-approval-gate]]` — never push/tag without an explicit
  "push" / "cut vX.Y.Z").
- Tests **4087 / 4087** both arches (X64 + AARCH64), 0 failures.
- clang-tidy **deterministically clean** (full tree, per-file form).
- `v0.21.0` is the last released tag (on `aximcode/axl-sdk-releases`).

## Unpushed commit stack (oldest → newest, `v0.21.0..HEAD`)

```
30ef3d02 AxlGfx: G5 — path + rounded-rect gradient fills (completes G5)
6b1ef7dd ci: run clang-tidy one file per process to stop ArrayBound flakiness
101b9628 AxlInput: plumb keyboard modifier + lock state through to events
673c839b AxlGfx: G6 — buffer blur (axl_gfx_buffer_blur)
49c158ca AxlGfx: G6 — drop-shadow helper (axl_gfx_draw_shadow), completes G6
```

## What shipped this session (after v0.21.0)

1. **G5 gradients — COMPLETE.** `axl_gfx_gradient_sample()` +
   `axl_gfx_fill_path_gradient()` + `axl_gfx_fill_rounded_rect_gradient()`
   (rect variant + the object shipped in v0.21.0). Path/rounded fills
   share their rasterizer with the solid versions via a `grad != NULL`
   selector; solid behavior unchanged bit-for-bit.
2. **AxlInput key modifiers — DONE.** `AxlInputEvent.modifiers` is now
   populated: L/R-distinct SHIFT/CTRL/ALT/META single bits + side-
   agnostic masks (`AXL_INPUT_MOD_SHIFT == LSHIFT|RSHIFT`) +
   CAPS/NUM/SCROLL lock. Backend `read_key_ex` translates EFI
   KeyShiftState+KeyToggleState → AXL bits (gated on the VALID bits);
   loop carries it in `AxlInputKey.modifiers`; `axl_input_attach_key`
   copies it to the event. `0` = none-or-unavailable (no ConIn-Ex /
   serial). KEY_DOWN only (UEFI has no key-up/standalone-mod events).
   **Live decode is hardware-only** (QEMU serial can't inject keys);
   only the public bit-layout constants are unit-tested.
3. **clang-tidy CI determinism fix.** Root-caused the recurring,
   intermittent `clang-analyzer-security.ArrayBound` flakiness to the
   **batched** invocation (`xargs -0 clang-tidy`, all TUs in one
   process — observed 0 then 6 then 0 findings on identical source).
   CI + RELEASING now use `xargs -0 -n1 -P"$(nproc)"` (one file per
   process): deterministic, parallel, still catches real bugs (they
   reproduce per-file — the v0.21.0 `axl_pci_addr_parse` stack-OOB did).
4. **G6 blur + shadows — COMPLETE.** New `<axl/axl-gfx-effects.h>`:
   - `axl_gfx_buffer_blur(buf, radius)` — separable triangular-kernel
     blur (Gaussian approx), clamp-to-edge, all 4 channels incl. alpha,
     normalized (energy-preserving), int64 sums (no overflow). Inner
     loop is direct O(r)/pixel; swapping in the O(1) incremental
     stack-blur later is a test-protected refactor (kernel identical).
   - `axl_gfx_draw_shadow(src, x, y, color, radius)` — soft drop shadow
     from src's alpha, tinted, blurred, composited via `fill_rect_i`.
     Temp padded by radius; RGB filled uniformly + only alpha carries
     the shape, so blur never darkens a colored shadow toward black.

## Architecture decisions still in force

- **`axl-gfx.h` is a thin umbrella** over `axl-gfx-{types,surface,draw,
  path,gradient,effects}.h` + font/truetype/pixmap. New gfx work adds a
  sub-header + one umbrella line; don't grow a monolith. `#include
  <axl/axl-gfx.h>` unchanged for consumers; `<axl.h>` surfaces the whole
  2D lib. (per `[[module-prefix-macros]]`, AxlNet-style layering.)
- **clang-tidy: run per-file** (`-n1`). Batched is flaky. When a finding
  fires, triage real-vs-FP by running that ONE file alone — reproduces
  per-file ⇒ real bug (fix it); only batched ⇒ analyzer artifact.
- **Test discipline:** test-first, exact-string/value or invariant
  assertions; QEMU-untestable paths (hardware quirks: PCI 0x0000 filter,
  key-modifier decode) are documented hardware-only, NOT faked. Both
  arches must stay balanced.

## Rich UI roadmap position (`docs/AXL-Rich-UI-Plan.md`)

- **Both viability spikes are GREEN** (branches `spike/lexbor-uefi`
  `4866085e`, `spike/quickjs-uefi` `52297020` — throwaway, NOT merged).
  Decision gate passed → **A2 (HTML/CSS, ~2-3mo) + C2 (QuickJS bindings,
  ~1-2mo) are green-lit** whenever you want the bigger bets.
- **AxlGfx 2D substrate:** G1–G4 (v0.20.0) + G5 + G6 **done**. Remaining:
  - **G7** glyph cache `(font, codepoint, px_size, subpixel)` → bitmap,
    LRU, hidden behind axl-truetype.c (no public API change). ~1wk.
  - **G8** stroke styling (width>1 Minkowski, caps/joins, dashes).
  - G9 display list / scene graph; G10 path-based clip; G11 multi-line
    text + word-wrap (`axl_ttf_draw_box`); G12 pattern fill / tile blit;
    G13 blend modes beyond source-over.
- AxlMath M1–M10 done. AxlGfx is **mid-arc, pre-1.0** — surface may
  still churn; consumers should pin an exact version, not a range.

## Open / deferred items

- **delldiags axl-utils consumer** can pin **`v0.21.0`** and drop its
  `dell_pci_tag` %04X wrapper (#1) + `-1`→`+-1` argv rewrite (#2). The
  4 consumer issues all shipped in v0.21.0.
- **PCI `0x0000` phantom filter (#4)** still needs **Spark-EVT hardware
  confirmation** — shipped on a QEMU-can't-reproduce basis (documented).
- **Drop-shadow / blur live rendering** is QEMU-buffer-testable (done),
  but the wider gfx-on-real-GOP path is hardware-verified.
- Optional: wire key modifiers + a drop-shadow into
  `sdk/examples/input-demo.c` / `gfx-demo.c` as visual checks.
- A future **release** (next minor, ~`0.22.0`) would bundle the 5
  unpushed commits — only on explicit approval.

## How to verify locally

```sh
cd ~/projects/aximcode/axl-sdk
make tests ARCH=x64 && ./test/integration/test-axl.sh --arch X64       # 4087/4087
./test/integration/test-axl.sh --arch AARCH64                          # 4087/4087
# clang-tidy (per-file, deterministic):
rm -f compile_commands.json && bear -- make tests tools
find src -name '*.c' -not -path '*/backend/*' -not -name 'axl-mbedtls-platform.c' \
  -print0 | xargs -0 -n1 -P"$(nproc)" clang-tidy -p . -quiet ; echo $?
```
