/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pixmap.c
    AxlPixmap — PNG/JPEG/GIF/BMP decoders backed by stb_image.

    Public API (<axl/axl-pixmap.h>) wraps stb_image with axl-sdk-
    side input validation, header pre-checks (per
    feedback_validate_untrusted_bytes_before_stb), and RGBA→BGRA
    swizzle into AxlGfxBuffer.

    No libm linkage: STBI_NO_HDR + STBI_NO_LINEAR remove the floating-
    point gamma/HDR paths so the remaining decoders (PNG, JPEG, BMP,
    GIF, PSD/TGA/PNM also linked because removing them is more code
    than letting --gc-sections drop them) use integer math only.

    No libc linkage either: STBI_MALLOC/REALLOC/FREE route to axl-sdk
    equivalents per the dogfood rule (feedback_dogfood).
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-gfx.h>
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-pixmap.h>

// ===================================================================
// stb_image configuration + memory shims
// ===================================================================

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS

#define STBI_MALLOC(sz)        axl_malloc((size_t)(sz))
#define STBI_REALLOC(p, sz)    axl_realloc((p), (size_t)(sz))
#define STBI_FREE(p)           axl_free(p)
#define STBI_ASSERT(x)         ((void)(x))

/* stb_image's BMP loader calls abs() from <stdlib.h>; provide it
 * locally since we're freestanding with no libc.  __builtin_abs is
 * compiled inline to a single instruction. */
static int abs(int x) { return __builtin_abs(x); }

#define STB_IMAGE_IMPLEMENTATION
#include "../../deps/stb/stb_image.h"

// ===================================================================
// Pre-validation — recognize the format header before handing to stb
// ===================================================================

/* Pre-validate that @a bytes begins with one of the format
 * signatures stb_image supports.  stb_image is more tolerant of
 * malformed input than stb_truetype (it does internal bounds
 * checks in many paths) but cheap up-front rejection still
 * catches the obvious "feed garbage to a parser" failure mode
 * before invoking the full decoder. */
static bool
pixmap_header_recognized(
    const uint8_t  *bytes,
    size_t          len
    )
{
    if (len < 2) {
        return false;
    }

    /* PNG: 8-byte signature. */
    if (len >= 8) {
        static const uint8_t png_sig[8] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
        };
        if (axl_memcmp(bytes, png_sig, 8) == 0) {
            return true;
        }
    }

    /* JPEG: SOI marker FF D8, followed by an FF byte starting the
     * next marker. */
    if (len >= 3
        && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
    {
        return true;
    }

    /* GIF: "GIF87a" or "GIF89a". */
    if (len >= 6
        && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == '8'
        && (bytes[4] == '7' || bytes[4] == '9')
        && bytes[5] == 'a')
    {
        return true;
    }

    /* BMP: "BM". */
    if (bytes[0] == 'B' && bytes[1] == 'M') {
        return true;
    }

    return false;
}

// ===================================================================
// Public API
// ===================================================================

int
axl_pixmap_info(
    const uint8_t  *bytes,
    size_t          len,
    uint32_t       *out_w,
    uint32_t       *out_h
    )
{
    if (!bytes || len == 0) {
        return AXL_ERR;
    }
    if (!pixmap_header_recognized(bytes, len)) {
        return AXL_ERR;
    }

    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(bytes, (int)len, &w, &h, &channels)) {
        return AXL_ERR;
    }

    if (out_w) {
        *out_w = (uint32_t)w;
    }
    if (out_h) {
        *out_h = (uint32_t)h;
    }
    return AXL_OK;
}

AxlGfxBuffer *
axl_pixmap_decode(
    const uint8_t  *bytes,
    size_t          len
    )
{
    if (!bytes || len == 0) {
        return NULL;
    }
    if (!pixmap_header_recognized(bytes, len)) {
        return NULL;
    }

    int w = 0, h = 0, channels = 0;
    /* desired_channels = 4 forces stb to produce RGBA8 regardless of
     * the source format's native channel count; opaque sources get
     * alpha = 0xFF per pixel automatically. */
    stbi_uc *rgba = stbi_load_from_memory(bytes, (int)len,
                                          &w, &h, &channels, 4);
    if (!rgba) {
        return NULL;
    }

    AxlGfxBuffer *buf = axl_gfx_buffer_new((uint32_t)w, (uint32_t)h);
    if (!buf) {
        stbi_image_free(rgba);
        return NULL;
    }

    /* Swizzle RGBA → BGRA into the AxlGfxBuffer. */
    AxlGfxPixel *pixels = axl_gfx_buffer_pixels(buf);
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        pixels[i].red   = rgba[i * 4 + 0];
        pixels[i].green = rgba[i * 4 + 1];
        pixels[i].blue  = rgba[i * 4 + 2];
        pixels[i].alpha = rgba[i * 4 + 3];
    }

    stbi_image_free(rgba);
    return buf;
}
