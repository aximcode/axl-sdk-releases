AxlSort — In-Place Sorting
==========================

``axl_qsort`` is a drop-in for ``qsort(3)`` with a guaranteed
O(n log n) worst case and no heap allocation. The engine is an
introsort: median-of-three quicksort, insertion sort for small runs,
and a heapsort fallback when recursion depth exceeds ``2*log2(n)``.
It is not stable. ``axl_array_sort`` delegates to it.

Header: ``<axl/axl-sort.h>``

API Reference
-------------

.. doxygenfile:: axl-sort.h
