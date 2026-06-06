/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-gfx-region.h:
 *
 * `AxlGfxRegion` — an EXACT set of non-overlapping rectangles with set
 * algebra (union / subtract / intersect), the role `pixman` plays in a
 * real Wayland stack. The substrate for damage tracking (a frame's changed
 * area as disjoint rects, not one spanning bounding box), opaque-region
 * occlusion culling, and multi-rect input regions.
 *
 * Representation: rectangles are kept in a canonical form — non-overlapping,
 * organised into y-sorted horizontal bands (the X11 `miregion` model). The
 * three set operations are a band sweep: cut the y-axis at every band edge
 * of both operands, combine the two x-span sets in each strip, then coalesce
 * vertically-adjacent identical bands. (A future optimization can replace
 * the sweep with the O(n) band merge-walk if rect counts ever warrant it;
 * results are identical either way.) The canonical form is unique and
 * deterministic for a given covered pixel set:
 * any two regions covering the same pixels yield the same rectangle
 * sequence (which is what makes axl_gfx_region_equal a direct compare and
 * axl_gfx_region_get_rect's order well-defined). Backing storage is an
 * `AxlArray` that persists across operations (`axl_gfx_region_clear` resets
 * the count but keeps the allocation), so a region reused each frame does no
 * steady-state allocation.
 *
 * Coordinates: a rect (x, y, w, h) covers the HALF-OPEN pixel range
 * [x, x+w) × [y, y+h) — the right and bottom edges are exclusive. So a
 * point at (x+w, y) is NOT contained, and two rects that share only an edge
 * do not overlap (but DO coalesce in a union, being abutting).
 *
 * OOM contract: a region can always be left in a valid state. If a set
 * operation cannot allocate the rectangles its exact result needs, the
 * region degrades to its **bounding box** — a conservative SUPERSET of the
 * true result — sets the `lossy` flag, and the operation returns `AXL_ERR`.
 * The region stays usable: a damage consumer simply over-paints; an
 * occlusion consumer MUST check `axl_gfx_region_is_lossy` and skip culling
 * with a lossy operand (a superset opaque area would over-cull). This is
 * the only source of imprecision — there is no routine rectangle cap.
 *
 * A mutator returns `AXL_ERR` for exactly two reasons, distinguished by the
 * `lossy` flag: an OOM degrade (the region is valid — `is_lossy` is true —
 * just proceed conservatively) versus a NULL/invalid argument (a caller
 * bug, NO state change — treat it as a precondition violation, not a
 * runtime branch). Because a NULL arg makes no state change, it never sets
 * `lossy`; so on `AXL_ERR`, `is_lossy` true means "degraded but valid" and
 * false means "bad argument" — UNLESS the region was already lossy from an
 * earlier op. Callers that care about that distinction should reject NULL
 * arguments up front rather than rely on the flag across calls.
 *
 * Not thread-safe; a region is a single-owner value (UEFI is single
 * threaded). Rectangles use `AxlGfxClip` (x, y, w, h); an empty rect
 * (w == 0 || h == 0) contributes nothing.
 */

#ifndef AXL_GFX_REGION_H
#define AXL_GFX_REGION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-types.h>
#include <axl/axl-gfx-surface.h>   /* AxlGfxClip */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An exact, non-overlapping set of rectangles. Opaque — created with
 * axl_gfx_region_new, released with axl_gfx_region_free.
 */
typedef struct AxlGfxRegion AxlGfxRegion;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * @brief Create a new, empty region.
 *
 * @return new region, or NULL on allocation failure. Free with
 *     axl_gfx_region_free().
 */
AxlGfxRegion *
axl_gfx_region_new(
    void
);

/**
 * @brief Free a region and its backing storage. NULL-safe.
 */
void
axl_gfx_region_free(
    AxlGfxRegion *r   ///< region (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlGfxRegion, axl_gfx_region_free)
#endif

/**
 * @brief Reset a region to empty, KEEPING its backing allocation.
 *
 * The high-water-mark capacity is retained so a region reused each frame
 * does no further allocation. Also clears the `lossy` flag.
 */
void
axl_gfx_region_clear(
    AxlGfxRegion *r   ///< region to empty
);

/**
 * @brief Replace @p dst's contents with a copy of @p src.
 *
 * @return AXL_OK on success; AXL_ERR on a NULL argument, or on allocation
 *     failure (in which case @p dst degrades to @p src's bounding box and
 *     is flagged lossy — see the OOM contract).
 */
int
axl_gfx_region_copy(
    AxlGfxRegion        *dst,   ///< destination (overwritten)
    const AxlGfxRegion  *src    ///< source
);

/* -------------------------------------------------------------------------
 * Set algebra. Each mutator returns AXL_OK on an exact result, or AXL_ERR
 * if it had to degrade to the bounding box on OOM (region stays valid +
 * lossy) or a NULL argument was passed (no state change).
 * ---------------------------------------------------------------------- */

/**
 * @brief Union @p other into @p r (@p r := @p r ∪ @p other).
 * @return AXL_OK, or AXL_ERR (see the set-algebra note above).
 */
int
axl_gfx_region_union(
    AxlGfxRegion        *r,      ///< region, modified in place
    const AxlGfxRegion  *other   ///< region to add
);

/**
 * @brief Subtract @p other from @p r (@p r := @p r ∖ @p other).
 * @return AXL_OK, or AXL_ERR (see the set-algebra note above).
 */
int
axl_gfx_region_subtract(
    AxlGfxRegion        *r,      ///< region, modified in place
    const AxlGfxRegion  *other   ///< region to remove
);

/**
 * @brief Intersect @p r with @p other (@p r := @p r ∩ @p other).
 * @return AXL_OK, or AXL_ERR (see the set-algebra note above).
 */
int
axl_gfx_region_intersect(
    AxlGfxRegion        *r,      ///< region, modified in place
    const AxlGfxRegion  *other   ///< region to intersect with
);

/**
 * @brief Union a single rectangle into @p r (the damage-accumulate path).
 *
 * Convenience over axl_gfx_region_union with a one-rect operand. An empty
 * @p rect (w == 0 || h == 0) is a no-op.
 * @return AXL_OK, or AXL_ERR (see the set-algebra note above).
 */
int
axl_gfx_region_union_rect(
    AxlGfxRegion  *r,      ///< region, modified in place
    AxlGfxClip     rect    ///< rectangle to add
);

/**
 * @brief Subtract a single rectangle from @p r (the occlusion path).
 *
 * Convenience over axl_gfx_region_subtract with a one-rect operand.
 * @return AXL_OK, or AXL_ERR (see the set-algebra note above).
 */
int
axl_gfx_region_subtract_rect(
    AxlGfxRegion  *r,      ///< region, modified in place
    AxlGfxClip     rect    ///< rectangle to remove
);

/**
 * @brief Clip @p r to a single rectangle (@p r := @p r ∩ @p rect).
 *
 * Convenience over axl_gfx_region_intersect with a one-rect operand —
 * e.g. clamping accumulated damage to the output bounds.
 * @return AXL_OK, or AXL_ERR (see the set-algebra note above).
 */
int
axl_gfx_region_intersect_rect(
    AxlGfxRegion  *r,      ///< region, modified in place
    AxlGfxClip     rect    ///< rectangle to clip to
);

/**
 * @brief Translate every rectangle in @p r by (@p dx, @p dy).
 *
 * Cannot fail or allocate (offsets in place); preserves the lossy flag.
 * No overflow check — keep offsets within int32 pixel range (the caller's
 * responsibility; never an issue for real output coordinates).
 */
void
axl_gfx_region_translate(
    AxlGfxRegion  *r,      ///< region, modified in place
    int32_t        dx,     ///< horizontal offset in pixels
    int32_t        dy      ///< vertical offset in pixels
);

/* -------------------------------------------------------------------------
 * Queries (const)
 * ---------------------------------------------------------------------- */

/**
 * @brief Is the region empty (covers no pixels)?
 * @return true if empty or @p r is NULL.
 */
bool
axl_gfx_region_is_empty(
    const AxlGfxRegion  *r   ///< region
);

/**
 * @brief Did a prior operation degrade this region to its bounding box?
 *
 * Set by an OOM degrade (see the OOM contract) and stays set until
 * axl_gfx_region_clear. A lossy region is a conservative SUPERSET of the
 * exact result — safe to PAINT from, NOT safe to SUBTRACT as an opaque
 * operand (an occlusion consumer must skip culling when this is true).
 * @return true if the region is a lossy bounding-box superset.
 */
bool
axl_gfx_region_is_lossy(
    const AxlGfxRegion  *r   ///< region
);

/**
 * @brief Bounding box of the whole region.
 *
 * The zero rect {0,0,0,0} is the canonical empty rectangle (w == 0), so an
 * empty or NULL region and a (degenerate) region at the origin are not
 * distinguishable from `bounds` alone — use axl_gfx_region_is_empty to test
 * emptiness.
 * @return the enclosing rectangle, or {0,0,0,0} if empty / NULL.
 */
AxlGfxClip
axl_gfx_region_bounds(
    const AxlGfxRegion  *r   ///< region
);

/**
 * @brief Is the point (@p x, @p y) inside the region?
 * @return true if covered by some rectangle in @p r.
 */
bool
axl_gfx_region_contains_point(
    const AxlGfxRegion  *r,   ///< region
    int32_t              x,   ///< x in pixels
    int32_t              y    ///< y in pixels
);

/**
 * @brief Does @p rect overlap any part of the region?
 * @return true if @p rect intersects @p r (false for an empty rect).
 */
bool
axl_gfx_region_intersects_rect(
    const AxlGfxRegion  *r,      ///< region
    AxlGfxClip           rect    ///< rectangle to test
);

/**
 * @brief Do two regions cover exactly the same pixels?
 *
 * Exact set equality (the unique canonical form makes this a direct rect
 * compare). Useful to detect "damage unchanged since last frame". All
 * regions covering no pixels compare equal — an empty region, another empty
 * region, and NULL are all equal to each other.
 * @return true if @p a and @p b cover the same pixels.
 */
bool
axl_gfx_region_equal(
    const AxlGfxRegion  *a,   ///< first region
    const AxlGfxRegion  *b    ///< second region
);

/* -------------------------------------------------------------------------
 * Iteration — the canonical rectangles (e.g. to flush each to the GOP)
 * ---------------------------------------------------------------------- */

/**
 * @brief Number of canonical rectangles in the region.
 * @return rect count (0 if empty / NULL).
 */
size_t
axl_gfx_region_num_rects(
    const AxlGfxRegion  *r   ///< region
);

/**
 * @brief The @p i-th canonical rectangle (0-based, in band order).
 * @return the rectangle, or {0,0,0,0} if @p i is out of range.
 */
AxlGfxClip
axl_gfx_region_get_rect(
    const AxlGfxRegion  *r,   ///< region
    size_t               i    ///< index in [0, num_rects)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_REGION_H */
