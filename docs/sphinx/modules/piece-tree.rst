AxlPieceTree — Out-of-Core Editable Buffer
==========================================

See :doc:`data` for an overview of all data modules.

An out-of-core, editable text buffer for large files. The original file
is never loaded whole — its bytes are read on demand through
:doc:`AxlFileView <stream>` — while edits accumulate in an append-only
add buffer. A balanced tree of pieces (spans into either source, held in
an :doc:`rb-tree` augmented with subtree byte and newline sums) presents
one logical document with O(log n) offset↔line mapping and O(log n)
edits, so editing a multi-gigabyte file costs memory proportional to the
edits, not the file.

This is the structure VS Code calls a "piece tree." For a
memory-resident editable store use :doc:`AxlTextBuffer <text-buffer>` (a
gap buffer) instead. Line semantics match ``AxlTextBuffer`` exactly, so
the two are interchangeable for a renderer. See
``docs/AXL-PieceTree-Design.md``.

Header: ``<axl/axl-piece-tree.h>``

API Reference
-------------

.. doxygenfile:: axl-piece-tree.h
