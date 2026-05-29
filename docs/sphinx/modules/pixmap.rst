AxlPixmap — Image Decode (PNG / JPEG / GIF / BMP)
==================================================

See :doc:`gfx` for an overview of the graphics substrate.
AxlPixmap decodes encoded image byte buffers into
``AxlGfxBuffer`` pixel arrays for blit-style consumption (icons,
button bitmaps, CSS ``<img>`` and ``background-image``).

Header: ``<axl/axl-pixmap.h>``

Backed by `stb_image <https://github.com/nothings/stb>`_ vendored
at ``deps/stb/stb_image.h`` (dual MIT / Public Domain licensed).
Two entry points:

- ``axl_pixmap_info`` returns dimensions without decoding — for
  layout-driven container sizing before committing to a full
  pixel-buffer allocation.
- ``axl_pixmap_decode`` returns a fully-decoded
  ``AxlGfxBuffer``; output is 4-channel BGRA matching
  ``AxlGfxPixel`` layout, so the returned buffer is directly
  blittable via ``axl_gfx_buffer_present``.

Naming note: ``pixmap`` (map of pixels) — distinct from
:doc:`image` (UEFI executable-image lifecycle).

API Reference
-------------

.. doxygenfile:: axl-pixmap.h
