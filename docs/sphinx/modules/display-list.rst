AxlGfxDisplayList — Retained Display List
==========================================

See :doc:`gfx` for an overview of the graphics substrate.  An
``AxlGfxDisplayList`` is a retained, replayable command buffer: record
a sequence of draw operations, then replay them against the active
draw target as many times as you like — or walk them for trace /
debug.

Header: ``<axl/axl-gfx-display-list.h>``

The recorder is explicit: every immediate-mode ``axl_gfx_*`` primitive
has an ``axl_gfx_dl_*`` analogue that appends an op instead of drawing.
``axl_gfx_display_list_replay`` then invokes the real primitive for
each recorded op, in order — byte-identical to immediate mode.

**Ownership.**  The list copies the transient data it is handed —
polyline point arrays, blit pixel buffers, text strings, and stroke
dash arrays — so the caller's originals may be freed immediately after
recording.  It does *not* copy the handle-typed objects, which have
their own lifecycles and must outlive the list: ``AxlGfxPath`` (filled
/ stroked by reference), ``AxlFont``, and ``AxlTtf``.

This is the Phase G9 "AGT-consumer" op slice — the primitives an
``AgtDrawContext`` forwards.  Gradient fills and the transform-stack
ops are a tracked follow-up.

API Reference
-------------

.. doxygenfile:: axl-gfx-display-list.h
