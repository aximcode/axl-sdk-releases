AxlRBTree — Intrusive Augmented Red-Black Tree
==============================================

See :doc:`data` for an overview of all data modules.

A generic, intrusive, augmentable red-black tree. The caller embeds an
``AxlRBNode`` in its own struct (the tree never allocates nodes) and
descends to the insertion point itself, so the same tree serves ordered
maps, order-statistic trees, and the byte/newline-weighted
:doc:`piece-tree`. An optional ``recompute`` callback keeps a cached
subtree aggregate (size, byte/newline sums, …) exact across every
structural change in O(log n).

Distinct from :doc:`AxlTree <cache>`'s sibling AVL map — that is a
non-intrusive key→value container; this is intrusive and augmentable.
Reimplemented under Apache-2.0 from the textbook red-black algorithm; no
GPL source is used. See ``docs/AXL-RBTree-Design.md``.

Header: ``<axl/axl-rb-tree.h>``

API Reference
-------------

.. doxygenfile:: axl-rb-tree.h
