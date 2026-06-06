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

// ===================================================================
// Multi-frame decoding — animated GIF
// ===================================================================

/// One decoded animation: N full-canvas frames plus per-frame delays.
///
/// Transparent so a consumer iterates directly: `anim->n_frames`,
/// `anim->frames[i]`, `anim->delays_ms[i]`.  Every member is owned by
/// the AxlPixmapAnim and freed by `axl_pixmap_anim_free` — do not free
/// an individual frame.
typedef struct {
    AxlGfxBuffer **frames;     ///< @a n_frames decoded frames (full canvas, BGRA)
    uint32_t      *delays_ms;  ///< per-frame display duration in ms (may be 0)
    uint32_t       n_frames;   ///< frame count (always >= 1 on success)
} AxlPixmapAnim;

/// Decode an image as an animation: every frame of an animated GIF plus
/// each frame's delay.
///
/// Each GIF frame is returned **fully composited to the canvas** (stb
/// applies the GIF disposal/transparency), so every frame is a complete,
/// directly-blittable image — the consumer never composites frames itself.
/// A static image (PNG/JPEG/BMP, or a single-frame GIF) decodes to a
/// **single frame** with `delays_ms[0] == 0`, so one entry point serves
/// both the animated and static cases.
///
/// Delays are the GIF's declared per-frame durations in milliseconds; a
/// 0 delay (some GIFs encode it) is passed through for the consumer to
/// clamp.  The GIF loop count is not surfaced (stb does not expose it), so
/// the repeat policy is the consumer's — typically loop forever.
///
/// Caller owns the result and frees it with `axl_pixmap_anim_free`.
///
/// @return a new AxlPixmapAnim on success (caller frees).  NULL on
///         @a bytes NULL, @a len 0, unrecognized format, malformed input,
///         or allocation failure.
AxlPixmapAnim *
axl_pixmap_decode_anim(
    const uint8_t  *bytes,     ///< [in] encoded image byte buffer
    size_t          len        ///< buffer length in bytes
    );

/// Free an AxlPixmapAnim — every frame buffer, both arrays, and the
/// struct itself.  NULL-safe.
void
axl_pixmap_anim_free(
    AxlPixmapAnim  *anim       ///< animation to free (NULL-safe)
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_PIXMAP_H */
