AxlEdid — Display EDID Parser
=============================

A pure decoder for the 128-byte VESA E-EDID base block a display
publishes (via ``EFI_EDID_DISCOVERED_PROTOCOL`` under UEFI — see
``axl_gfx_get_edid`` — or any transport that yields the raw bytes).
``axl_edid_parse`` validates the EDID header signature and the
base-block checksum, then decodes monitor identity (manufacturer /
product / serial / name), EDID version, physical image size, and the
panel's native timing (resolution + pixel clock) from Detailed Timing
Descriptor #1. ``axl_edid_dpi`` derives display DPI from the native
resolution and physical size.

Because it takes a caller-supplied byte buffer and never touches
firmware, the decode logic unit-tests against canned blobs with no
hardware. Extension blocks (CEA-861, DisplayID) are counted via
``extension_count`` but not decoded.

Header: ``<axl/axl-edid.h>``

API Reference
-------------

.. doxygenfile:: axl-edid.h
