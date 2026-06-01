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

#include <stdbool.h>
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

/// 2D point at floating-point (sub-pixel) precision.
///
/// The vocabulary type for geometry the caller supplies pre-transformed
/// into device/target space — e.g. the four corners handed to
/// `axl_gfx_push_clip_quad`.  Distinct from the integer `AxlGfxPoint`
/// (polyline vertices); use this where sub-pixel placement matters.
typedef struct {
    float  x;
    float  y;
} AxlGfxPointF;

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

/// Separable blend modes (Porter-Duff "over" plus the common PDF /
/// Canvas / SVG separable blend functions).  Select one with
/// `axl_gfx_set_blend_mode`; it affects subsequent drawing on **buffer
/// targets** (GOP screen targets cannot composite, same caveat as
/// translucent alpha).  The blend function B(Cb, Cs) is applied per
/// channel to backdrop Cb and source Cs, then composited over the
/// (opaque) backdrop by the source alpha:
/// `out = ((255 - a)*Cb + a*B(Cb, Cs) + 127) / 255`.
typedef enum {
    AXL_GFX_BLEND_OVER = 0,  ///< normal source-over (default); B = Cs
    AXL_GFX_BLEND_MULTIPLY,  ///< B = Cb*Cs/255 (darkens)
    AXL_GFX_BLEND_SCREEN,    ///< B = 255 - (255-Cb)(255-Cs)/255 (lightens)
    AXL_GFX_BLEND_OVERLAY,   ///< multiply or screen per backdrop (contrast)
    AXL_GFX_BLEND_DARKEN,    ///< B = min(Cb, Cs)
    AXL_GFX_BLEND_LIGHTEN,   ///< B = max(Cb, Cs)
    AXL_GFX_BLEND_ADD,       ///< B = min(255, Cb + Cs) (additive / "plus")
} AxlGfxBlendMode;

/// Composite @a src over @a dst using an explicit blend @a mode.
///
/// The mode-parameterized form of `axl_gfx_blend` (which is exactly
/// `axl_gfx_blend_ex(dst, src, AXL_GFX_BLEND_OVER)`).  Pure: depends
/// only on its arguments, not the active blend-mode state.  Result
/// alpha is always 0xFF (the backdrop is treated as opaque).
AxlGfxPixel
axl_gfx_blend_ex(
    AxlGfxPixel      dst,   ///< destination / backdrop pixel
    AxlGfxPixel      src,   ///< source pixel (its alpha drives compositing)
    AxlGfxBlendMode  mode   ///< blend function to apply
    );

/// Parse a CSS-style hex color string into an `AxlGfxPixel`.
///
/// The inverse of the `#RRGGBBAA` form emitted by
/// `axl_gfx_display_list_dump`, and the parser a consumer needs to
/// load colors from a JSON5 theme.  Accepts the four CSS hex forms,
/// each preceded by a required `#`:
///   - `#RGB`       — 3 nibbles; each is doubled (`#f80` → `#ff8800`),
///                    alpha defaults to opaque (0xFF).
///   - `#RGBA`      — 4 nibbles, each doubled, including alpha.
///   - `#RRGGBB`    — 6 hex digits, alpha defaults to opaque.
///   - `#RRGGBBAA`  — 8 hex digits, alpha explicit.
///
/// Hex digits are case-insensitive (`#FF6347` == `#ff6347`).  No
/// surrounding whitespace is tolerated — pass the bare token (a JSON5
/// string value already arrives trimmed).  Named colors (e.g.
/// "tomato") are intentionally NOT recognized; use the `AXL_GFX_*`
/// palette macros for those.
///
/// On failure @a out is left unmodified.
///
/// @return AXL_OK on success.  AXL_ERR if @a str or @a out is NULL,
///         @a str lacks a leading `#`, its length is not 3/4/6/8
///         nibbles, or it contains a non-hex digit.
int
axl_gfx_color_parse(
    const char   *str,   ///< [in] e.g. "#RRGGBB", "#RGB", "#RRGGBBAA"
    AxlGfxPixel  *out     ///< [out] parsed color (untouched on error)
    );

/// Decode one 8-bit sRGB channel value to linear light in `[0, 1]`.
///
/// The standard sRGB EOTF: `c/12.92` below the 0.04045 knee, else
/// `((c+0.055)/1.055)^2.4`.  Light mixing (alpha blending, anti-alias
/// coverage, filtering) is physically linear, but pixels are stored
/// gamma-encoded — converting to linear first is what
/// `axl_gfx_set_gamma_correct` does internally.  Exposed so consumers
/// doing their own color math (or gamma-correct gradient ramps) can use
/// the exact same transfer function.
///
/// @return linear-light value in `[0.0, 1.0]`.
float
axl_gfx_srgb_to_linear(
    uint8_t  srgb     ///< 8-bit sRGB-encoded channel value
    );

/// Encode a linear-light value in `[0, 1]` back to an 8-bit sRGB channel.
///
/// Inverse of `axl_gfx_srgb_to_linear` (the sRGB OETF: `l*12.92` below
/// the 0.0031308 knee, else `1.055*l^(1/2.4) - 0.055`), rounded to the
/// nearest 8-bit code.  Inputs are clamped to `[0, 1]`.
///
/// @return 8-bit sRGB-encoded channel value `[0, 255]`.
uint8_t
axl_gfx_linear_to_srgb(
    float  linear     ///< linear-light value (clamped to [0, 1])
    );

/// Framebuffer pixel byte order for the two direct-write GOP formats.
///
/// A backend-neutral spelling of the two 8-bit-per-channel GOP pixel
/// formats so the present path can pack `AxlGfxPixel`s for a direct
/// framebuffer write without leaking UEFI's `EFI_GRAPHICS_PIXEL_FORMAT`
/// into the public API.  The bitmask (`PixelBitMask`) and Blt-only
/// (`PixelBltOnly`) GOP modes have no fixed byte order and are not
/// representable here — the present path falls back to GOP `Blt` for
/// those.
typedef enum {
    AXL_GFX_PIXEL_BGRA = 0,  ///< blue, green, red, reserved (GOP "BGR")
    AXL_GFX_PIXEL_RGBA,      ///< red, green, blue, reserved (GOP "RGB")
} AxlGfxPixelOrder;

/// Pack an `AxlGfxPixel` into a 32-bit framebuffer word for @a order.
///
/// The pure pixel-format conversion at the heart of the direct
/// framebuffer present path (Phase G17).  `AxlGfxPixel` is stored BGRA
/// (blue in the low byte), matching the GOP "BGR" format exactly, so:
///   - `AXL_GFX_PIXEL_BGRA` is the identity (the bytes are already in
///     framebuffer order — present is a straight row copy).
///   - `AXL_GFX_PIXEL_RGBA` swaps the red and blue bytes.
///
/// The returned word is in the host's native byte order: byte 0 (the
/// low 8 bits on a little-endian framebuffer) holds the channel that
/// @a order names first.  The alpha/reserved byte is carried through
/// unchanged in the high byte; firmware ignores it.
///
/// Pure: depends only on its arguments.  Exposed so consumers writing
/// their own framebuffer code can reuse the exact conversion the
/// library uses.
///
/// @return the packed 32-bit framebuffer word.
uint32_t
axl_gfx_pack_pixel(
    AxlGfxPixel       px,    ///< [in] source pixel (BGRA storage)
    AxlGfxPixelOrder  order  ///< [in] target framebuffer byte order
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
