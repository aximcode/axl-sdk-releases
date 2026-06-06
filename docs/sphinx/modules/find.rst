AxlFind — Byte-Substring Search Engine
======================================

See :doc:`data` for an overview of all data modules.

A Boyer-Moore-Horspool byte-substring search (with case-insensitive and
whole-word variants, forward and backward) that runs over an abstract
random-access byte source — an ``AxlByteReader``. The same engine drives
a flat memory block (the built-in ``AxlMemReader``), a
:doc:`AxlTextBuffer <text-buffer>` (gap buffer), and a
:doc:`AxlPieceTree <piece-tree>` (out-of-core piece table); the
``axl_text_buffer_find`` / ``axl_piece_tree_find`` wrappers build the
appropriate reader and call ``axl_find_in_source``.

The reader pulls overlapping windows so a match straddling the source's
internal boundaries (a piece boundary, the gap) is never missed; a
contiguous source that supplies the optional ``peek`` is scanned in
place with no copy. A successful find reports an ``AxlMatch`` (start +
length); length is carried explicitly so the result shape already fits
variable-length matchers.

Header: ``<axl/axl-find.h>``

API Reference
-------------

.. doxygenfile:: axl-find.h

AxlRegex — Regular-Expression Matcher
=====================================

A compiled-pattern regular-expression matcher (``<axl/axl-regex.h>``)
layered on the same ``AxlByteReader`` seam. Compile a pattern once with
``axl_regex_new``, then search many times (find-all loops, one pattern
across many lines/buffers). It is a Thompson NFA / Pike VM, **not** a
backtracker, so match time is O(pattern × input) for every pattern —
there is no catastrophic ("ReDoS") blow-up. Backreferences are
deliberately unsupported because they are not regular and would force
backtracking.

Supported: literals; ``.``; greedy and lazy quantifiers
``* + ? *? +? ??``; anchors ``^ $``; alternation ``|``; grouping and
capture ``( )``; classes ``[...]`` / ``[^...]`` with ranges; the escapes
``\d \w \s \D \W \S`` and friends. Matching is byte-oriented and
leftmost (Perl / ``grep -P`` priority). Captures via
``axl_regex_search_captures``; compile flags ``CASELESS`` / ``MULTILINE``
/ ``DOTALL``; an ``ANCHORED`` match flag. The
``axl_text_buffer_find_regex`` / ``axl_piece_tree_find_regex`` wrappers
run a compiled regex over those sources.

.. doxygenfile:: axl-regex.h
