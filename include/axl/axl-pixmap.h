/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pixmap.h
    Image decoders for PNG, JPEG, GIF, and BMP into AxlGfxBuffer.

    Decodes byte buffers into AxlGfxBuffer pixel arrays for blit-style
    consumption (icons, button bitmaps, CSS `<img>` and `background-
    image`).  Backed by stb_image — public-domain single-header
    decoder vendored at `deps/stb/stb_image.h`.

    Naming note: `pixmap` to distinguish from `<axl/axl-image.h>`
    (which is for UEFI executable-image lifecycle —
    LoadImage/StartImage/UnloadImage).  `pixmap` here is "map of
    pixels" — the decoded raster data, not the format-on-disk and
    not anything executable.

    Two entry points:
      - `axl_pixmap_info` returns dimensions WITHOUT decoding — useful
        for layout-driven container sizing before committing to a
        full pixel-buffer allocation.
      - `axl_pixmap_decode` returns a fully-decoded AxlGfxBuffer.

    The public API is toolkit-neutral — no HTML/CSS-shaped
    intrinsic-sizing helpers, no DPI metadata.  Consumers wrap at
    their own layer.

    @code
    // Layout-before-decode: size the container, decide whether to
    // commit to the full buffer alloc.
    uint32_t w, h;
    if (axl_pixmap_info(bytes, len, &w, &h) == AXL_OK
        && w <= my_max_w && h <= my_max_h)
    {
        AxlGfxBuffer *buf = axl_pixmap_decode(bytes, len);
        if (buf) {
            axl_gfx_buffer_present(buf, dst_x, dst_y);
            axl_gfx_buffer_free(buf);
        }
    }
    @endcode
**/

#ifndef AXL_PIXMAP_H
#define AXL_PIXMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-gfx-surface.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Inspection — dimensions without decoding
// ===================================================================

/// Get pixel dimensions of an encoded image without allocating a
/// decode buffer.
///
/// Wraps stb_image's `stbi_info_from_memory`.  Parses just enough
/// of the format's header to extract width and height — for PNG
/// that's the 8-byte signature + 25-byte IHDR chunk; for JPEG it's
/// the SOFn marker.  Cheap compared to full decode.
///
/// Supports the same formats as `axl_pixmap_decode` (PNG, JPEG,
/// GIF, BMP).
///
/// @return AXL_OK on success.  AXL_ERR if @a bytes is NULL,
///         @a len is too small to contain any recognized format
///         header, the format is unrecognized, or the header is
///         malformed.  Output pointers untouched on error.
int
axl_pixmap_info(
    const uint8_t  *bytes,     ///< [in] encoded image byte buffer
    size_t          len,       ///< buffer length in bytes
    uint32_t       *out_w,     ///< [out] image width in pixels (NULL OK)
    uint32_t       *out_h      ///< [out] image height in pixels (NULL OK)
    );

// ===================================================================
// Decoding
// ===================================================================

/// Decode an encoded image byte buffer into a fresh AxlGfxBuffer.
///
/// Format is auto-detected from the buffer magic; supports PNG,
/// JPEG, GIF (first frame only), and BMP.  Output is 4-channel
/// BGRA matching AxlGfxPixel's layout — directly blittable via
/// `axl_gfx_buffer_present`.  Source formats that omit alpha
/// (e.g. opaque BMP, JPEG) get `alpha = 0xFF` for every pixel.
///
/// Decoded buffer dimensions match what `axl_pixmap_info` reports.
///
/// Callers receive ownership and must free with `axl_gfx_buffer_free`.
///
/// @return new AxlGfxBuffer on success (caller frees).  NULL on
///         allocation failure, unrecognized format, malformed
///         input, @a bytes NULL, or @a len 0.
AxlGfxBuffer *
axl_pixmap_decode(
    const uint8_t  *bytes,     ///< [in] encoded image byte buffer
    size_t          len        ///< buffer length in bytes
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_PIXMAP_H */
