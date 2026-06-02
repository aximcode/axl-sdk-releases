AxlPageCache — LRU Page Cache
=============================

See :doc:`data` for an overview of all data modules.

A fixed-capacity LRU cache of equal-sized pages backed by a
caller-supplied fill function. Lookups return a borrowed, zero-copy
pointer into the resident frame; on a miss the least-recently-used
frame is evicted and refilled. It is the mechanism behind
:doc:`AxlFileView <stream>` but knows nothing about files — the fill
function decides where bytes come from, so it windows any large,
randomly-addressed backing store where only the hot pages should stay
resident.

Header: ``<axl/axl-page-cache.h>``

API Reference
-------------

.. doxygenfile:: axl-page-cache.h
