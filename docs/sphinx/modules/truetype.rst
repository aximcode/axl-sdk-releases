AxlTtf — Vector Text (TrueType / OpenType)
============================================

See :doc:`gfx` for an overview of the graphics substrate.  AxlTtf
is the vector-text companion to ``axl-gfx``'s bitmap-font path —
loads TTF/OTF byte buffers, measures rendered strings, draws
glyphs with anti-aliased coverage into the active draw target.

Header: ``<axl/axl-truetype.h>``

Backed by `stb_truetype <https://github.com/nothings/stb>`_ vendored
at ``deps/stb/stb_truetype.h`` (dual MIT / Public Domain licensed).
The public API is toolkit-neutral — no em units, no line-box
metrics, no CSS color names; consumers (Path A2's HTML painter,
AGT widget toolkit) translate at their own layer.

API Reference
-------------

.. doxygenfile:: axl-truetype.h
