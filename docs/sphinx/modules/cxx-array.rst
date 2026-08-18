axl::array_span — AxlArray as a std::span
==========================================

.. note::

   One of the C++ layer's seams over the C API — the C2, C3 and C5 phases of
   ``docs/AXL-Cxx-Design.md`` §9. None of them is a container and none is a
   C++ API *over* the C one; each exists only where C++ gives something C
   cannot, which is the rule ``AXL-Cxx-Design.md`` §6b's corollary sets. See
   also :doc:`cxx`.

Header: ``<axl/axl-array.hpp>``

A borrowed ``AxlArray *`` becomes a ``std::span``, so ``<algorithm>`` and
``<ranges>`` apply to it with nothing else written.

.. code-block:: cpp

   #include <axl/axl-array.hpp>

   for (int start : axl::array_span<int>(rows)) { ... }

   std::span<Item *> its = axl::array_ptr_span<Item>(items);
   auto shown = its | std::views::filter([](Item *i) { return i->visible; });

This phase was scoped as a view type, ``axl::c_array_ref``, whose iterator
would call ``axl_array_get()`` per dereference. It is not here because
:c:func:`axl_array_data` made it pointless: with a base pointer the elements
are ``T *``, so ``std::span`` **is** the view and its iterators are real
pointers rather than a proxy.

Both costs of the proxy shape were measured rather than guessed. §4.1: an
out-of-line call plus a bounds check per dereference ran indexed traversal 4.2×
and a sort 19.4× slower than the base-pointer loop. §2, second trap: a
hand-rolled proxy iterator satisfied ``std::sort`` and was then *rejected* by
``views::filter`` for lacking a default constructor, because C++20's iterator
concepts refine ``std::semiregular`` where ``iterator_traits`` did not. A
``T *`` has neither problem. glibmm reached the same place from the other
direction — its ``Glib::ArrayHandle`` family wrapped C arrays for a decade and
was deleted in 2.68 in favour of standard containers plus conversion functions.

A span is a ``(pointer, length)`` pair read out of the array when you ask, and
it carries :c:func:`axl_array_data`'s invalidation rule unchanged: appending,
inserting, prepending, resizing or stealing can move the buffer. Take the span,
use it, drop it; do not store one beside the array it views.

Each factory has a ``const AxlArray *`` overload yielding ``std::span<const T>``
(and ``std::span<T *const>`` for the pointer form), because the C accessors all
take ``AxlArray *`` though none of them mutates — absorbing that is what a seam
is for, and the single ``const_cast`` lives here instead of at every call site.
A literal ``nullptr`` gets its own overload so the documented "or NULL" is true
rather than ambiguous.

An over-aligned ``T`` is refused at compile time. ``axl_malloc`` guarantees only
8 bytes (``AllocatePool`` plus an ``AXL_MEM_HEADER`` that is ``8 mod 16``), so a
``struct alignas(16) Vec4`` **matches the stride** at 16 bytes while sitting on
8-aligned storage — and the compiler is then entitled to emit an aligned vector
move over the span, which is a ``#GP`` with nothing to catch it.

A stride mismatch — ``array_span<int>`` over an array of something else —
**halts**, naming both sizes and the factory that was called. When ``sizeof(T)`` is the larger it is an
out-of-bounds read, and the alternative of returning an empty span would turn
that into a loop that silently does nothing, which in firmware reads as "the
list is empty" rather than "the code is wrong".

Only the reading half. Appending still goes through ``axl_array_append()``,
which memcpys, so §4.1's soundness verdict stands: an ``AxlArray`` is not a
place to keep non-trivially-copyable C++ objects. To *own* an array from C++,
use ``axl::unique_handle<AxlArray>`` for the C structure or ``std::vector`` for
C++ elements.

.. doxygenfile:: axl-array.hpp
   :project: axl
