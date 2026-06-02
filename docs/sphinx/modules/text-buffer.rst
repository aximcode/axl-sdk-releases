AxlTextBuffer — Editable Text Store
===================================

See :doc:`data` for an overview of all data modules.

A growable, editable byte buffer with an integral line index, tuned for
an interactive text editor: load a file once, then many small
inserts/deletes near a moving cursor, with O(log n) byte-offset ↔ line
mapping on every keystroke and repaint. Storage is a gap buffer; the
line index is maintained incrementally (never a full rescan) and queried
by binary search. The store is byte-oriented — ``'\n'`` is the only
special byte — so the caller owns any UTF-8 / codepoint policy.

Header: ``<axl/axl-text-buffer.h>``

API Reference
-------------

.. doxygenfile:: axl-text-buffer.h
