/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-types.h
    AxlGfx shared vocabulary: framebuffer info, the BGRA pixel type,
    source-over blend, and the RGB(A) literal macros + named color
    palette.  Every other AxlGfx sub-header builds on these types.
    Pulled in transitively via the <axl/axl-gfx.h> umbrella.
**/

#ifndef AXL_GFX_TYPES_H
#define AXL_GFX_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Types
// ===================================================================

/// Framebuffer information (standard C types, no UEFI types).
typedef struct {
    uint32_t  width;       ///< horizontal resolution in pixels
    uint32_t  height;      ///< vertical resolution in pixels
    uint32_t  stride;      ///< pixels per scan line (>= width)
    uint64_t  framebuffer; ///< physical address (0 if BltOnly mode)
} AxlGfxInfo;

/// Pixel color in BGRA layout (matches GOP native BGRX pixel format
/// for the BGR bytes; the 4th byte is alpha for blending operations).
///
/// alpha = 0xFF means fully opaque; alpha = 0 means fully transparent.
/// When passed to drawing primitives:
///   - alpha == 0xFF: fast opaque draw (existing behavior; for screen
///     targets uses GOP Blt directly).
///   - alpha == 0: no-op (fully transparent).
///   - 0 < alpha < 0xFF: source-over blend against the destination's
///     existing pixels.  Only supported on buffer targets (alpha-aware
///     drawing on screen targets falls back to opaque, since GOP has
///     no blending hardware — render to a back-buffer first if you
///     want semi-transparent overlays on screen).
typedef struct {
    uint8_t  blue;
    uint8_t  green;
    uint8_t  red;
    uint8_t  alpha;     ///< 0xFF = opaque, 0 = transparent
} AxlGfxPixel;

/// Source-over alpha composite: blend a source pixel over a destination.
/// Result alpha is always 0xFF (destination is treated as opaque).
/// Math (8-bit integer): out.rgb = (src.rgb * a + dst.rgb * (255 - a) + 127) / 255
/// where a = src.alpha.
AxlGfxPixel
axl_gfx_blend(
    AxlGfxPixel  dst,    ///< destination pixel (existing)
    AxlGfxPixel  src     ///< source pixel (with alpha)
    );

// -------------------------------------------------------------------
// Convenience: RGB(A) literal macros + named color palette
// -------------------------------------------------------------------
// AxlGfxPixel storage is BGRA to match the GOP framebuffer byte order
// exactly (zero conversion at present time).  These macros let callers
// write colors in the more familiar RGB notation — e.g. CSS #FF6347
// for tomato becomes AXL_GFX_RGB(0xFF, 0x63, 0x47) — without paying
// any per-pixel byte-swap cost at runtime: the macros expand to a
// compound literal with the bytes already in BGRA order.

#define AXL_GFX_RGB(r, g, b)        ((AxlGfxPixel){(b), (g), (r), 0xFF})
#define AXL_GFX_RGBA(r, g, b, a)    ((AxlGfxPixel){(b), (g), (r), (a)})

// Common named colors — all opaque.  Add more as consumers ask for them.
#define AXL_GFX_BLACK      AXL_GFX_RGB(0x00, 0x00, 0x00)
#define AXL_GFX_WHITE      AXL_GFX_RGB(0xFF, 0xFF, 0xFF)
#define AXL_GFX_RED        AXL_GFX_RGB(0xFF, 0x00, 0x00)
#define AXL_GFX_GREEN      AXL_GFX_RGB(0x00, 0xFF, 0x00)
#define AXL_GFX_BLUE       AXL_GFX_RGB(0x00, 0x00, 0xFF)
#define AXL_GFX_YELLOW     AXL_GFX_RGB(0xFF, 0xFF, 0x00)
#define AXL_GFX_CYAN       AXL_GFX_RGB(0x00, 0xFF, 0xFF)
#define AXL_GFX_MAGENTA    AXL_GFX_RGB(0xFF, 0x00, 0xFF)
#define AXL_GFX_GRAY       AXL_GFX_RGB(0x80, 0x80, 0x80)
#define AXL_GFX_TRANSPARENT AXL_GFX_RGBA(0x00, 0x00, 0x00, 0x00)

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_TYPES_H */
