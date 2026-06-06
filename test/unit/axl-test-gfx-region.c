/** @file axl-test-gfx-region.c
    Unit tests for AxlGfxRegion (compositor enhancement phase E1) — exact
    rectangle-set algebra. Directed exact-value cases plus a per-pixel
    bitmap-oracle fuzz: random union/subtract/intersect sequences applied to
    both a region and a brute-force grid must agree pixel-for-pixel, with the
    region staying canonical (non-overlapping, area == covered cells).
**/

#include "axl-test.h"
#include <axl/axl-gfx-region.h>

static bool
clip_eq(AxlGfxClip a, AxlGfxClip b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* ---- lifecycle / empty ---- */
static void
test_region_empty(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    test_check(r != NULL, "region: new succeeds");
    test_check(axl_gfx_region_is_empty(r), "region: new is empty");
    test_check(!axl_gfx_region_is_lossy(r), "region: new is not lossy");
    test_check(axl_gfx_region_num_rects(r) == 0, "region: new has 0 rects");
    test_check(clip_eq(axl_gfx_region_bounds(r), (AxlGfxClip){0, 0, 0, 0}),
               "region: empty bounds are {0,0,0,0}");
    axl_gfx_region_union_rect(r, (AxlGfxClip){5, 5, 10, 10});
    axl_gfx_region_clear(r);
    test_check(axl_gfx_region_is_empty(r), "region: clear empties");
    axl_gfx_region_free(r);
}

/* ---- single rect + half-open containment ---- */
static void
test_region_single(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    test_check(axl_gfx_region_union_rect(r, (AxlGfxClip){0, 0, 100, 100}) == AXL_OK,
               "region: union_rect returns AXL_OK");
    test_check(axl_gfx_region_num_rects(r) == 1, "region: single union -> 1 rect");
    test_check(clip_eq(axl_gfx_region_get_rect(r, 0), (AxlGfxClip){0, 0, 100, 100}),
               "region: the rect is exactly the added one");
    test_check(clip_eq(axl_gfx_region_bounds(r), (AxlGfxClip){0, 0, 100, 100}),
               "region: bounds == the rect");
    test_check(axl_gfx_region_contains_point(r, 50, 50), "region: contains interior point");
    test_check(axl_gfx_region_contains_point(r, 0, 0), "region: contains top-left (inclusive)");
    test_check(axl_gfx_region_contains_point(r, 99, 99), "region: contains bottom-right interior");
    test_check(!axl_gfx_region_contains_point(r, 100, 100),
               "region: does NOT contain (x+w, y+h) — half-open");
    test_check(!axl_gfx_region_contains_point(r, 100, 50),
               "region: does NOT contain right edge x+w — half-open");
    axl_gfx_region_free(r);
}

/* ---- contained add is a no-op; disjoint stays multi-rect ---- */
static void
test_region_union(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    axl_gfx_region_union_rect(r, (AxlGfxClip){0, 0, 100, 100});
    axl_gfx_region_union_rect(r, (AxlGfxClip){10, 10, 20, 20});  /* fully inside */
    test_check(axl_gfx_region_num_rects(r) == 1, "region: contained union is a no-op");

    axl_gfx_region_clear(r);
    axl_gfx_region_union_rect(r, (AxlGfxClip){0, 0, 10, 10});       /* caret */
    axl_gfx_region_union_rect(r, (AxlGfxClip){500, 400, 40, 12});   /* clock */
    test_check(axl_gfx_region_num_rects(r) == 2,
               "region: two disjoint unions -> 2 rects (not one bbox)");
    test_check(!axl_gfx_region_contains_point(r, 250, 200),
               "region: the gap between disjoint rects is NOT covered");
    test_check(clip_eq(axl_gfx_region_bounds(r), (AxlGfxClip){0, 0, 540, 412}),
               "region: bounds span both disjoint rects");
    axl_gfx_region_free(r);
}

/* ---- abutting same-span rects coalesce to one canonical rect ---- */
static void
test_region_coalesce(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    axl_gfx_region_union_rect(r, (AxlGfxClip){0, 0, 100, 50});
    axl_gfx_region_union_rect(r, (AxlGfxClip){0, 50, 100, 50});  /* abuts below, same x-span */
    test_check(axl_gfx_region_num_rects(r) == 1, "region: abutting same-span -> 1 rect");
    test_check(clip_eq(axl_gfx_region_get_rect(r, 0), (AxlGfxClip){0, 0, 100, 100}),
               "region: coalesced into the full 100x100 rect");
    axl_gfx_region_free(r);
}

/* ---- subtract: hole punch -> canonical bands; subtract-all -> empty ---- */
static void
test_region_subtract(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    axl_gfx_region_union_rect(r, (AxlGfxClip){0, 0, 100, 100});
    test_check(axl_gfx_region_subtract_rect(r, (AxlGfxClip){25, 25, 50, 50}) == AXL_OK,
               "region: subtract_rect returns AXL_OK");
    /* canonical bands: top(100x25) + middle-left(25x50) + middle-right(25x50)
       + bottom(100x25) = 4 rects */
    test_check(axl_gfx_region_num_rects(r) == 4, "region: hole punch -> 4 canonical bands");
    test_check(!axl_gfx_region_contains_point(r, 50, 50), "region: hole center is uncovered");
    test_check(axl_gfx_region_contains_point(r, 10, 50), "region: left band still covered");
    test_check(axl_gfx_region_contains_point(r, 90, 50), "region: right band still covered");
    test_check(axl_gfx_region_contains_point(r, 50, 10), "region: top band still covered");

    axl_gfx_region_subtract_rect(r, (AxlGfxClip){0, 0, 100, 100});
    test_check(axl_gfx_region_is_empty(r), "region: subtract covering rect -> empty");
    axl_gfx_region_free(r);
}

/* ---- intersect clips to a rect ---- */
static void
test_region_intersect(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    axl_gfx_region_union_rect(r, (AxlGfxClip){0, 0, 100, 100});
    axl_gfx_region_intersect_rect(r, (AxlGfxClip){50, 50, 100, 100});  /* overlaps lower-right */
    test_check(axl_gfx_region_num_rects(r) == 1, "region: intersect_rect -> 1 rect");
    test_check(clip_eq(axl_gfx_region_get_rect(r, 0), (AxlGfxClip){50, 50, 50, 50}),
               "region: intersect clipped to the overlap {50,50,50,50}");
    axl_gfx_region_intersect_rect(r, (AxlGfxClip){200, 200, 10, 10});  /* disjoint */
    test_check(axl_gfx_region_is_empty(r), "region: intersect with disjoint rect -> empty");
    axl_gfx_region_free(r);
}

/* ---- region-region subtract (the occlusion path) ---- */
static void
test_region_region_ops(void)
{
    AxlGfxRegion *dmg = axl_gfx_region_new();
    axl_gfx_region_union_rect(dmg, (AxlGfxClip){0, 0, 100, 100});

    AxlGfxRegion *opaque = axl_gfx_region_new();
    axl_gfx_region_union_rect(opaque, (AxlGfxClip){0, 0, 100, 40});    /* top strip */
    axl_gfx_region_union_rect(opaque, (AxlGfxClip){0, 60, 100, 40});   /* bottom strip */

    test_check(axl_gfx_region_subtract(dmg, opaque) == AXL_OK, "region: subtract(region) AXL_OK");
    /* only the middle band [0,40)..[60) survives: {0,40,100,20} */
    test_check(axl_gfx_region_num_rects(dmg) == 1, "region: region-subtract leaves the gap band");
    test_check(clip_eq(axl_gfx_region_get_rect(dmg, 0), (AxlGfxClip){0, 40, 100, 20}),
               "region: surviving band is exactly {0,40,100,20}");
    axl_gfx_region_free(dmg);
    axl_gfx_region_free(opaque);
}

/* ---- copy + equal ---- */
static void
test_region_copy_equal(void)
{
    AxlGfxRegion *a = axl_gfx_region_new();
    axl_gfx_region_union_rect(a, (AxlGfxClip){0, 0, 30, 30});
    axl_gfx_region_union_rect(a, (AxlGfxClip){60, 60, 30, 30});

    AxlGfxRegion *b = axl_gfx_region_new();
    test_check(axl_gfx_region_copy(b, a) == AXL_OK, "region: copy AXL_OK");
    test_check(axl_gfx_region_equal(a, b), "region: copy equals source");

    axl_gfx_region_union_rect(b, (AxlGfxClip){200, 200, 5, 5});
    test_check(!axl_gfx_region_equal(a, b), "region: mutated copy is not equal");

    AxlGfxRegion *e1 = axl_gfx_region_new();
    AxlGfxRegion *e2 = axl_gfx_region_new();
    test_check(axl_gfx_region_equal(e1, e2), "region: two empties are equal");
    test_check(axl_gfx_region_equal(e1, NULL), "region: empty equals NULL");
    axl_gfx_region_free(a);
    axl_gfx_region_free(b);
    axl_gfx_region_free(e1);
    axl_gfx_region_free(e2);
}

/* ---- translate + intersects_rect ---- */
static void
test_region_translate_intersects(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    axl_gfx_region_union_rect(r, (AxlGfxClip){10, 10, 5, 5});
    axl_gfx_region_translate(r, 100, 200);
    test_check(axl_gfx_region_contains_point(r, 110, 210), "region: translated by (100,200)");
    test_check(!axl_gfx_region_contains_point(r, 10, 10), "region: original spot vacated");

    test_check(axl_gfx_region_intersects_rect(r, (AxlGfxClip){112, 212, 2, 2}),
               "region: intersects an overlapping rect");
    test_check(!axl_gfx_region_intersects_rect(r, (AxlGfxClip){0, 0, 50, 50}),
               "region: does not intersect a disjoint rect");
    test_check(!axl_gfx_region_intersects_rect(r, (AxlGfxClip){115, 210, 5, 5}),
               "region: edge-touching rect does NOT intersect (half-open)");
    axl_gfx_region_free(r);
}

/* ---- NULL safety ---- */
static void
test_region_null_safety(void)
{
    axl_gfx_region_free(NULL);  /* no crash */
    test_check(axl_gfx_region_is_empty(NULL), "region: is_empty(NULL) is true");
    test_check(!axl_gfx_region_is_lossy(NULL), "region: is_lossy(NULL) is false");
    test_check(axl_gfx_region_num_rects(NULL) == 0, "region: num_rects(NULL) is 0");
    test_check(axl_gfx_region_union_rect(NULL, (AxlGfxClip){0, 0, 1, 1}) == AXL_ERR,
               "region: union_rect(NULL) returns AXL_ERR");
    test_check(!axl_gfx_region_contains_point(NULL, 0, 0),
               "region: contains_point(NULL) is false");
}

/* ---- negative coordinates + half-open edges at negative origins ---- */
static void
test_region_negative_coords(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    axl_gfx_region_union_rect(r, (AxlGfxClip){-10, -10, 20, 20});  /* [-10,10)x[-10,10) */
    test_check(axl_gfx_region_num_rects(r) == 1, "neg: single negative-origin rect");
    test_check(clip_eq(axl_gfx_region_get_rect(r, 0), (AxlGfxClip){-10, -10, 20, 20}),
               "neg: negative rect stored exactly");
    test_check(axl_gfx_region_contains_point(r, -10, -10), "neg: top-left inclusive");
    test_check(axl_gfx_region_contains_point(r, -1, -1), "neg: interior negative point");
    test_check(!axl_gfx_region_contains_point(r, 10, 10),
               "neg: bottom-right exclusive (half-open at positive edge)");
    test_check(axl_gfx_region_contains_point(r, 9, 9), "neg: just inside bottom-right");
    test_check(clip_eq(axl_gfx_region_bounds(r), (AxlGfxClip){-10, -10, 20, 20}),
               "neg: bounds of negative rect");
    /* subtract the top half -> a band still at negative x, positive y */
    axl_gfx_region_subtract_rect(r, (AxlGfxClip){-10, -10, 20, 10});
    test_check(axl_gfx_region_num_rects(r) == 1, "neg: subtract leaves one band");
    test_check(clip_eq(axl_gfx_region_get_rect(r, 0), (AxlGfxClip){-10, 0, 20, 10}),
               "neg: surviving band is {-10,0,20,10}");
    axl_gfx_region_free(r);
}

/* ---- OOM degrade: exact normally, bbox-superset + lossy on alloc fail ---- */
static void
test_region_oom_degrade(void)
{
    AxlGfxRegion *r = axl_gfx_region_new();
    axl_gfx_region_union_rect(r, (AxlGfxClip){0, 0, 10, 10});
    axl_gfx_region_union_rect(r, (AxlGfxClip){100, 100, 10, 10});  /* disjoint -> 2 rects */
    AxlGfxRegion *other = axl_gfx_region_new();
    axl_gfx_region_union_rect(other, (AxlGfxClip){200, 200, 10, 10});

    /* A NULL operand is a caller bug, not a degrade: AXL_ERR, no lossy. */
    test_check(axl_gfx_region_union(r, NULL) == AXL_ERR, "oom: union(NULL) returns AXL_ERR");
    test_check(!axl_gfx_region_is_lossy(r), "oom: NULL-arg error does NOT set lossy");
    test_check(axl_gfx_region_num_rects(r) == 2, "oom: NULL-arg error leaves region unchanged");

    axl_gfx_region_free(r);
    axl_gfx_region_free(other);

    /* Sweep the injected failure across EVERY internal allocation point of
       the op (the fresh `out`, each of region_op's scratch arrays, and the
       in-sweep span appends). At whichever point OOM hits, the op must
       either complete exactly (failure point beyond its alloc count) or
       degrade to the SAME conservative bbox superset {0,0,210,210} + lossy
       — never a corrupt or under-covering region. */
    bool ok = true;
    for (size_t n = 1; n <= 16 && ok; n++) {
        AxlGfxRegion *a = axl_gfx_region_new();
        axl_gfx_region_union_rect(a, (AxlGfxClip){0, 0, 10, 10});
        axl_gfx_region_union_rect(a, (AxlGfxClip){100, 100, 10, 10});
        AxlGfxRegion *b = axl_gfx_region_new();
        axl_gfx_region_union_rect(b, (AxlGfxClip){200, 200, 10, 10});

        axl_mem_fail_next_alloc(n);
        int rc = axl_gfx_region_union(a, b);
        axl_mem_fail_next_alloc(0);

        if (rc == AXL_ERR) {
            ok = axl_gfx_region_is_lossy(a) &&
                 axl_gfx_region_num_rects(a) == 1 &&
                 clip_eq(axl_gfx_region_get_rect(a, 0), (AxlGfxClip){0, 0, 210, 210}) &&
                 axl_gfx_region_contains_point(a, 5, 5) &&     /* superset covers all */
                 axl_gfx_region_contains_point(a, 205, 205);
        } else {  /* failure point past the op's allocs: exact union, not lossy */
            ok = !axl_gfx_region_is_lossy(a) &&
                 axl_gfx_region_contains_point(a, 5, 5) &&
                 !axl_gfx_region_contains_point(a, 50, 50);   /* the gap stays uncovered */
        }
        if (!ok) {
            axl_printf("  oom degrade wrong at fail-alloc n=%zu rc=%d\n", n, rc);
        }
        axl_gfx_region_free(a);
        axl_gfx_region_free(b);
    }
    test_check(ok, "oom: failure at ANY allocation point -> valid lossy bbox superset");

    AxlGfxRegion *c = axl_gfx_region_new();
    axl_gfx_region_union_rect(c, (AxlGfxClip){0, 0, 10, 10});
    axl_mem_fail_next_alloc(1);
    (void)axl_gfx_region_union_rect(c, (AxlGfxClip){50, 50, 10, 10});
    axl_mem_fail_next_alloc(0);
    test_check(axl_gfx_region_is_lossy(c), "oom: union_rect degrade sets lossy");
    axl_gfx_region_clear(c);
    test_check(!axl_gfx_region_is_lossy(c), "oom: clear resets the lossy flag");
    axl_gfx_region_free(c);
}

/* ======================= bitmap-oracle fuzz ======================= */
#define GRID 48
#define OFF  (GRID / 2)   /* world coords span [-OFF, GRID-OFF) -> negatives covered */

static uint32_t g_rng = 0x12345678u;
static uint32_t
rng_next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static AxlGfxClip
rng_rect(void)
{
    int32_t x = (int32_t)(rng_next() % GRID) - OFF;   /* [-OFF, GRID-OFF) */
    int32_t y = (int32_t)(rng_next() % GRID) - OFF;
    uint32_t w = 1 + rng_next() % (uint32_t)(GRID - (x + OFF));  /* keeps x+w <= GRID-OFF */
    uint32_t h = 1 + rng_next() % (uint32_t)(GRID - (y + OFF));
    return (AxlGfxClip){x, y, w, h};
}

/* grid cell (gx,gy) maps to world (gx-OFF, gy-OFF) */
static void
bmp_apply(uint8_t bmp[GRID][GRID], AxlGfxClip c, int op /*0=or 1=sub 2=and*/)
{
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            int32_t wx = gx - OFF, wy = gy - OFF;
            bool in = (wx >= c.x && wx < c.x + (int)c.w &&
                       wy >= c.y && wy < c.y + (int)c.h);
            if (op == 0) { if (in) bmp[gy][gx] = 1; }
            else if (op == 1) { if (in) bmp[gy][gx] = 0; }
            else { if (!in) bmp[gy][gx] = 0; }
        }
    }
}

/* Rasterize the region and compare to the oracle bitmap exactly; also
   verify the region is canonical (non-overlapping, area == covered cells). */
static bool
region_matches(const AxlGfxRegion *r, uint8_t bmp[GRID][GRID])
{
    static uint8_t rb[GRID][GRID];
    for (int y = 0; y < GRID; y++)
        for (int x = 0; x < GRID; x++)
            rb[y][x] = 0;

    size_t nr = axl_gfx_region_num_rects(r);
    uint64_t region_area = 0;
    for (size_t i = 0; i < nr; i++) {
        AxlGfxClip c = axl_gfx_region_get_rect(r, i);
        region_area += (uint64_t)c.w * c.h;
        for (int wy = c.y; wy < c.y + (int)c.h; wy++) {
            for (int wx = c.x; wx < c.x + (int)c.w; wx++) {
                int gx = wx + OFF, gy = wy + OFF;   /* world -> grid */
                if (gx < 0 || gx >= GRID || gy < 0 || gy >= GRID) continue;
                if (rb[gy][gx]) return false;   /* OVERLAP — not canonical */
                rb[gy][gx] = 1;
            }
        }
    }
    uint64_t bmp_area = 0;
    for (int y = 0; y < GRID; y++) {
        for (int x = 0; x < GRID; x++) {
            bmp_area += bmp[y][x];
            if (rb[y][x] != bmp[y][x]) return false;   /* pixel mismatch */
        }
    }
    return region_area == bmp_area;   /* exact non-overlapping coverage */
}

static void
test_region_fuzz(void)
{
    const int seqs = 200, ops_per = 10;
    bool ok = true;
    int fail_seq = -1, fail_op = -1;

    for (int s = 0; s < seqs && ok; s++) {
        AxlGfxRegion *reg = axl_gfx_region_new();
        static uint8_t bmp[GRID][GRID];
        for (int y = 0; y < GRID; y++)
            for (int x = 0; x < GRID; x++)
                bmp[y][x] = 0;

        for (int o = 0; o < ops_per; o++) {
            AxlGfxClip c = rng_rect();
            uint32_t pick = rng_next() % 5;   /* bias to union so regions grow */
            if (pick < 2)      { axl_gfx_region_union_rect(reg, c);     bmp_apply(bmp, c, 0); }
            else if (pick < 4) { axl_gfx_region_subtract_rect(reg, c);  bmp_apply(bmp, c, 1); }
            else               { axl_gfx_region_intersect_rect(reg, c); bmp_apply(bmp, c, 2); }
            if (!region_matches(reg, bmp)) {
                ok = false; fail_seq = s; fail_op = o;
                break;
            }
        }
        axl_gfx_region_free(reg);
    }
    if (!ok) {
        axl_printf("  fuzz mismatch at seq %d op %d\n", fail_seq, fail_op);
    }
    test_check(ok, "region: 2000 fuzzed ops match per-pixel bitmap oracle (exact, canonical)");
}

int
test_gfx_region_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlGfxRegion");

    test_region_empty();
    test_region_single();
    test_region_union();
    test_region_coalesce();
    test_region_subtract();
    test_region_intersect();
    test_region_region_ops();
    test_region_copy_equal();
    test_region_translate_intersects();
    test_region_negative_coords();
    test_region_null_safety();
    test_region_oom_degrade();
    test_region_fuzz();

    return test_print_results();
}

AXL_APP(test_gfx_region_main)
