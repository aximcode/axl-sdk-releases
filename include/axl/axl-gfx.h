/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx.h
    Basic graphics output (GOP framebuffer operations).

    Provides a thin abstraction over the UEFI Graphics Output Protocol
    for framebuffer rendering: fill rectangles, blit pixel buffers, and
    capture screen regions.  Falls back gracefully on headless systems
    where GOP is not available.

    @code
    if (axl_gfx_available()) {
        AxlGfxInfo info;
        axl_gfx_get_info(&info);
        printf("Display: %ux%u\n", info.width, info.height);

        AxlGfxPixel red = {0, 0, 0xFF, 0};
        axl_gfx_fill_rect(100, 100, 200, 150, red);
    }
    @endcode
**/

#ifndef AXL_GFX_H
#define AXL_GFX_H

#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

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

/// Pixel color in BGRX layout (matches GOP native pixel format).
typedef struct {
    uint8_t  blue;
    uint8_t  green;
    uint8_t  red;
    uint8_t  reserved;
} AxlGfxPixel;

// ===================================================================
// Availability
// ===================================================================

/// Check whether a graphics display is available.
///
/// @return true if GOP was found, false on headless/serial systems.
bool
axl_gfx_available(void);

/// Get framebuffer information.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_get_info(
    AxlGfxInfo  *info  ///< [out] receives display info
    );

// ===================================================================
// Drawing
// ===================================================================

/// Fill a rectangle with a solid color.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_fill_rect(
    uint32_t     x,      ///< left edge
    uint32_t     y,      ///< top edge
    uint32_t     w,      ///< width in pixels
    uint32_t     h,      ///< height in pixels
    AxlGfxPixel  color   ///< fill color
    );

/// Blit a pixel buffer to the screen.
///
/// @a buffer must contain at least @a w * @a h pixels in row-major
/// order with AxlGfxPixel (BGRX) layout.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_blit(
    const AxlGfxPixel  *buffer,  ///< [in] source pixel buffer
    uint32_t            x,       ///< destination left edge
    uint32_t            y,       ///< destination top edge
    uint32_t            w,       ///< width in pixels
    uint32_t            h        ///< height in pixels
    );

/// Capture a screen region into a pixel buffer.
///
/// @a buffer must have space for at least @a w * @a h pixels.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_capture(
    AxlGfxPixel  *buffer,  ///< [out] destination pixel buffer
    uint32_t      x,       ///< source left edge
    uint32_t      y,       ///< source top edge
    uint32_t      w,       ///< width in pixels
    uint32_t      h        ///< height in pixels
    );

// ===================================================================
// Text rendering (8x16 VGA bitmap font)
// ===================================================================

/// Draw a text string at the given position.
///
/// Uses an embedded 8x16 VGA bitmap font.  Printable ASCII only
/// (0x20-0x7E); non-printable characters are transparent (existing
/// background pixels are preserved).  Output is clamped to screen
/// bounds.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available or text is NULL.
int
axl_gfx_draw_text(
    uint32_t     x,     ///< left edge (pixels)
    uint32_t     y,     ///< top edge (pixels)
    const char  *text,  ///< UTF-8/ASCII text to render
    AxlGfxPixel  color, ///< text foreground color
    uint32_t     scale  ///< scale factor (1 = native 8x16, 2 = 16x32, etc.)
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_H */
