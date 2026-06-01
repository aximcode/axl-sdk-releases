# Session handoff — AxlTransform consolidation, tree containers, path clip; G17+G18 next (2026-05-31, session 2)

## TL;DR state

- Branch `main`, **30 commits ahead of `origin/main`** (last release tag
  `v0.21.0`), **unpushed and untagged**. Working tree clean.
- **4673/4673 unit tests, 0 failures, both X64 + AARCH64.** clang-tidy clean.
  Docs build clean (`./scripts/build-docs.sh`, 0 warnings).
- All new symbols target **axl-sdk v0.22.0** (VERSION still `0.21.0`; bump at
  release). AGT's `AgtTreeView` + transform adoption use **v0.22.0** as the
  dependency floor.
- **DO NOT push or cut a release without explicit user approval.** Same gate
  as always — the user builds AGT against this checkout; v0.22.0 is the
  version AGT references.

## What shipped this session (10 commits, top of the unpushed stack)

| Commit | What |
|--------|------|
| `00caad71` | **AxlTransform phase 1** — consolidate `AxlMat3` + `AxlGfxAffine` → one `AxlTransform` (axl-math, 3×3 double). Rename + verb harmonize (mul→multiply, inverse→invert, skew→shear, transform_point→map_point). Flip multiply to **cairo a-first**. |
| `8b6518ca` | **AxlTransform phase 2** — projective math: `perspective`, `quad_to_quad` (Heckbert), `map_rect`/`map_quad`, `classify`+`is_*` predicates; `map_point` perspective divide (affine stays bit-exact). |
| `119a4caa` | **AxlTransform phase 3+4** — perspective-correct rasterization: `axl_gfx_blit_transform` (per-pixel inverse homography + horizon guard), `axl_ttf_draw_transform` (local-space curve flatten), `axl_gfx_push_clip_rect_transformed`. |
| `08798142` | **AxlNTree** — GLib GNode n-ary tree, public node fields, full traverse surface. `<axl/axl-ntree.h>`. |
| `04ae78f4` | **AxlTree** — GLib GTree AVL sorted map, opaque. `<axl/axl-tree.h>`. |
| `beba4d20` | **AxlNTree/AxlTree iterators** — callback-free pull cursors (`AxlNTreeIter` pre-order parent-link walk; `AxlTreeIter` in-order fixed left-spine stack). |
| `03499f6c` | docs: fix Doxygen `@a param` refs in the tree headers (bare `@cmp`/`@key` broke the Sphinx/Breathe build — see `feedback_doc_comment_param_refs`). |
| `cd58bb58` | docs: Rich UI plan — transform consolidation + perspective + containers. |
| `d78fa387` | docs: Rich UI plan audit — **G13 + the entire AxlMath M1–M10 were already shipped**, now marked. |
| `ee6a316a` | **AxlGfx G10** — `axl_gfx_push_clip_path`: arbitrary-shape (concave/holed) clip via an 8-bit coverage mask (even-odd, hard-edge), all writers honor it on buffer + screen targets. |

Spec docs created this session: `docs/AXL-Transform-Design.md` (the transform
contract + future-transform-type extensibility), plus `src/data/README.md`
and `src/gfx/README.md` updates.

## NEXT SESSION: G17 + G18 combined (the present pipeline)

The user explicitly wants **G17 + G18 done combined** next session.

### What they are (from `docs/AXL-Rich-UI-Plan.md`)
- **G17 — direct-framebuffer present.** Write the back-buffer straight to
  `FrameBufferBase` (honor `PixelFormat` BGR/RGB/bitmask + `PixelsPerScanLine`
  stride), with non-temporal streaming stores; **never read VRAM**; fall back
  to GOP `Blt` for `PixelBltOnly`/`BitMask`. Wins where firmware's `Blt` is a
  slow software copy.
- **G18 — dirty-rectangle present.** Present only the changed region (10–100×
  bandwidth cut for cursor/widget redraws). Composes with G9 display list.

### KEY FINDING — present-to-screen IS testable (don't repeat my mistake)
The **unit** runner (`test/integration/common-test.sh`) boots QEMU
`-nographic`, no GPU → no GOP. So unit gfx tests composite into in-RAM
`AxlGfxBuffer`s and assert pixel values (real pixel verification, no screen).
**BUT** `scripts/run-qemu.sh --gpu` wires a real GPU (X64 `-device VGA`,
AARCH64 `-device virtio-gpu-pci`) so **OVMF exposes a linear-framebuffer GOP**,
and `--screenshot FILE` captures it via QEMU `screendump` (PNG/PPM);
`--screenshot` implies `--gpu`.

⇒ **G17+G18 present-to-screen is testable via a GOP-enabled integration test**
(the `test-*-qemu.sh` family, which opts out of the unit ratchet via
`TEST_SKIP_RATCHET=1`). Cleanest approach: a test EFI that **presents a known
pattern, then reads it back in-guest via `axl_gfx_capture`** (GOP
`VideoToBltBuffer`) and asserts pixels to serial — run under
`run-qemu.sh --gpu`. The round-trip tests the direct-FB write, the BGR/RGB
format conversion (capture returns BGRA, so a wrong swap shows as swapped
colors), and the region/damage present, on a real linear-FB GOP, both arches.
Only the *perf* claim (direct-FB beats Blt) stays hardware-specific;
correctness is fully testable.

### Agreed design (confirm with user, then implement)
- **G18 API (user CONFIRMED scope):** `axl_gfx_buffer_present_rect(buf, dst_x,
  dst_y, src_x, src_y, w, h)` to push a sub-region, PLUS **per-buffer damage
  tracking**: `axl_gfx_buffer_add_damage(buf, rect)` unions into a damage bbox
  on the buffer, `axl_gfx_buffer_present_damage(buf, dst_x, dst_y)` flushes only
  the changed bbox + clears, `axl_gfx_buffer_clear_damage(buf)`,
  `axl_gfx_buffer_get_damage(buf, &rect)` (for tests). `AxlGfxBuffer`
  (`src/gfx/axl-gfx.c:33`, `{w,h,pixels}`) gains a damage bbox field.
- **G17:** `axl_gfx_buffer_present` (+ the region variant) gains a direct-FB
  path: BGR → memcpy rows (AxlGfxPixel is already BGRA), RGB → per-pixel R/B
  swap, NT stores (`__builtin_nontemporal_store`, clang, x64+aa64), Blt
  fallback for BitMask/BltOnly. Factor a **testable** `present_pack_pixel(pixel,
  fmt)` pure helper. GOP fields: `gop->Mode->FrameBufferBase`,
  `gop->Mode->Info->PixelFormat` / `->PixelsPerScanLine` (pixel-format enum in
  `include/uefi/generated/console.h`: PixelRedGreenBlue.../PixelBlueGreenRed.../
  PixelBitMask/PixelBltOnly).
- **Build order:** they share the present path — implement the direct-FB write
  + region present together; `present()` = `present_rect(whole buffer)`;
  `present_damage` = `present_rect(damage bbox)` then clear. G17 just swaps the
  copy mechanism under the G18 API, so no rework.
- **Tests:** unit-test the pure logic (`present_pack_pixel` BGRA→RGB/identity;
  damage bbox union/clamp; region clamping). Add a GOP integration test
  (`test/integration/test-gfx-present-qemu.sh`, run under `--gpu`) doing
  present + `axl_gfx_capture` readback assertions.

### Current present code
`axl_gfx_buffer_present` is at `src/gfx/axl-gfx.c` (~just before the blend-mode
section) — currently a single GOP `Blt(EfiBltBufferToVideo)`. `axl_gfx_capture`
(GOP `VideoToBltBuffer`) already exists for readback. `AxlGfxInfo.framebuffer`
exposes the FB physical address (0 if BltOnly).

## Discipline reminders (per CLAUDE.md + auto-memory)
- Test-first (bucket A for new API); build + run **both arches** via
  `timeout 200s ./test/integration/test-axl.sh --arch X64` (and `--arch
  AARCH64`); read the `Results: N passed, 0 failed` line; **independent
  code-review agent before every commit**; clang-tidy changed src; set ratchet
  to the verified count (auto-ratchets on success).
- **Run `./scripts/build-docs.sh` after touching any public header** — bare
  `@param` (not `@a param`) silently breaks the Sphinx build (bit this session).
- Direct commits to `main` for solo work; **do not push without approval**.

## Genuinely-remaining gfx phases after G17+G18
G12 (pattern fill — fully buffer-testable), G15 (gamma), G16 (LCD subpixel),
G19 (MP parallel raster, spike-gated), G-FT (full FreeType bundle; only
`ftgrays` vendored). All AxlMath (M1–M10) and G1–G11/G13/G14 are done.
