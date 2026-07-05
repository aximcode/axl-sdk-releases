/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * compositor-bench.c — profile the AxlCompositor present path (E6 follow-on).
 *
 * Resolves the one profile-gated optimization left after E6: is hoisting
 * occlusion further — from once-per-present (E6) to a PERSISTENT per-node
 * visible region updated only on geometry change (wlroots `node->visible`,
 * §10) — worth the caching/invalidation complexity?
 *
 * The metric is the cost of an ANIMATION frame: a present that changes only a
 * small region of content while the scene GEOMETRY is unchanged (a caret
 * blink, a spinner). After E6 such a present still does a full O(N) occlusion
 * REBUILD (collect order + per-surface region ops) even though every visible
 * region is identical to the previous frame. A persistent visible region would
 * skip that rebuild; this benchmark measures exactly what it would save.
 *
 * For each surface count N it builds a realistic 1080p scene with overlap and
 * partial occlusion, then times:
 *   - per-frame: a present whose damage is one tiny (4x4) rect — so the cost is
 *     ~ the occlusion rebuild + a near-zero blit (== the persistent-region
 *     saving).
 *   - full: one axl_compositor_composite (rebuild + full repaint), for scale.
 * It also prints composited_count and occlusion_passes to confirm 1 pass/frame.
 *
 * QEMU caveat: absolute microseconds are emulated, not real-hardware time. The
 * load-bearing evidence is (a) the rebuild's FRACTION of a frame and (b) how
 * per-frame cost SCALES with N — both robust to a constant emulation factor.
 *
 * Run via: scripts/run-qemu.sh compositor-bench.efi   (headless — no GOP)
 * Emits "COMPOSITOR-BENCH: done" as the final line.
 */

#include <axl.h>
#include <axl/axl-compositor.h>

#define OUT_W   1920
#define OUT_H   1080
#define ITERS   100        /* presents timed per N (averaged) */
#define MAX_N   256

/* Build N overlapping surfaces over the output, recording them in @surfs;
   ~half flagged opaque so the occlusion walk does real region subtraction.
   Deterministic placement. Returns the number actually created. */
static uint32_t
build_scene(AxlCompositor *c, uint32_t n, AxlSurface **surfs)
{
    AxlSurface *root = axl_compositor_root(c);
    uint32_t made = 0;
    for (uint32_t i = 0; i < n; i++) {
        AxlSurface *s = axl_surface_create(root, 320, 240);
        if (s == NULL) {
            axl_printf("  (alloc failed at surface %u)\n", i);
            break;
        }
        int32_t x = (int32_t)((i * 137u) % 1600u);
        int32_t y = (int32_t)((i * 89u) % 840u);
        axl_surface_move(s, x, y);
        axl_surface_set_opaque(s, (i & 1u) == 0u);   /* every other one occludes */
        surfs[made++] = s;
    }
    return made;
}

static void
bench_n(uint32_t n)
{
    static AxlSurface *surfs[MAX_N];
    AxlCompositor *c = axl_compositor_new(OUT_W, OUT_H);
    if (c == NULL) {
        axl_printf("  N=%u: compositor alloc failed\n", n);
        return;
    }
    uint32_t made = build_scene(c, n, surfs);

    /* Prime: one full present consumes the create/move damage. */
    axl_compositor_present(c);

    /* Full composite (rebuild + full repaint) for scale. */
    uint64_t t0 = axl_time_get_us();
    axl_compositor_composite(c);
    uint64_t t1 = axl_time_get_us();
    uint64_t full_us = t1 - t0;
    uint32_t full_blits = axl_compositor_composited_count(c);

    /* Per animation frame: tiny-damage present (rebuild dominates the blit). */
    AxlSurface *first = made > 0 ? surfs[0] : NULL;
    t0 = axl_time_get_us();
    for (uint32_t k = 0; k < ITERS; k++) {
        axl_surface_damage(first, (AxlGfxClip){2, 2, 4, 4});
        axl_compositor_present(c);
    }
    t1 = axl_time_get_us();
    uint64_t frame_us_x100 = ((t1 - t0) * 100u) / ITERS;   /* hundredths of us */
    uint32_t passes = axl_compositor_occlusion_passes(c);
    uint32_t tiny_blits = axl_compositor_composited_count(c);

    axl_printf("  N=%4u  per-frame=%lu.%02lu us  full-composite=%lu us  "
               "(tiny-blits=%u full-blits=%u passes/frame=%u)\n",
               n,
               (unsigned long)(frame_us_x100 / 100u),
               (unsigned long)(frame_us_x100 % 100u),
               (unsigned long)full_us, tiny_blits, full_blits, passes);

    axl_compositor_free(c);   /* frees the whole surface tree */
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    axl_printf("COMPOSITOR-BENCH: present-path profile "
               "(%ux%u, %u presents/N)\n", OUT_W, OUT_H, ITERS);
    if (axl_time_get_us() == 0) {
        axl_printf("  WARNING: no usable cycle counter — timings are 0\n");
    }
    axl_printf("  per-frame = tiny-damage present = occlusion rebuild + ~0 blit "
               "(== the persistent-visible-region saving)\n");

    const uint32_t counts[] = {8, 32, 128, 256};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        bench_n(counts[i]);
    }

    axl_printf("COMPOSITOR-BENCH: done\n");
    return 0;
}
