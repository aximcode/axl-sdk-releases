axl::view / axl::adopt — the C-string seam
===========================================

.. note::

   One of the C++ layer's seams over the C API — the C2, C3 and C5 phases of
   ``docs/AXL-Cxx-Design.md`` §9. None of them is a container and none is a
   C++ API *over* the C one; each exists only where C++ gives something C
   cannot, which is the rule ``AXL-Cxx-Design.md`` §6b's corollary sets. See
   also :doc:`cxx`.

Header: ``<axl/axl-cstr.hpp>``

About twenty public AXL functions return a string the caller must release —
``axl_env_get()``, ``axl_fs_volume_label()``, ``axl_hmac_hex()`` and their
neighbours. In C that is an ``AXL_AUTOPTR``. In C++ it was four lines that leak
on any early return, and that get the NULL case wrong the first time somebody
writes them from memory, because ``std::string(nullptr)`` is undefined
behaviour rather than an empty string.

.. code-block:: cpp

   #include <axl/axl-cstr.hpp>

   std::string      path  = axl::adopt(axl_env_get("PATH"));   // frees for you
   std::string_view label = axl::view(axl_device_name(dev));   // NULL-safe

The two allocators stay separate and this is the boundary between them.
``axl_malloc`` reaches ``gBS->AllocatePool`` and writes an ``AXL_MEM_HEADER``
ahead of the bytes; ``operator new`` reaches libstdc++, then newlib ``malloc``,
then dlmalloc over ``_sbrk``. A pointer AXL gave you can therefore only be
released by ``axl_free()`` — handing it to ``free`` or ``delete`` makes dlmalloc
read a chunk header nobody wrote, and drops the allocation out of the leak
accounting the suite gates on. What ``axl::adopt()`` removes is not that call
but every hand-written instance of it; past the seam the value is an ordinary
C++ object on the ordinary C++ heap.

The destination must OWN its bytes, and that is enforced rather than
documented: ``adopt<std::string_view>(p)`` would build a view over ``p`` and
then free ``p``, inspecting correctly right up until the bytes are reused. A
``static_assert(!std::ranges::borrowed_range<S>)`` refuses it — the predicate
is exactly right here, true for ``string_view`` and ``span`` and false for both
intended destinations. To borrow instead, keep the pointer in an owner and read
it through ``axl::view()``.

``adopt()`` is templated on the destination and defaults to ``std::string``.
``axl::adopt<axl::string>(p)`` differs in one way, and it is the way
``AXL-Cxx-Design.md`` §9c is about: when the copy cannot be allocated
``std::string`` halts, while ``axl::string`` sets a sticky ``bad()`` the caller
can read. Either way the C buffer is released — with one honest exception: if ``S``'s
constructor halts (which is what ``std::string`` does on a failed allocation)
nothing runs after it, so the ``axl_free`` never happens. No observer survives
to see it, but "always frees" would otherwise read as a guarantee on a path
where there is no code left to make one.

The copy stops at the first NUL: ``axl::view()`` is
``std::string_view(const char *)``, so it is ``strlen``-bounded. Every C
function this seam serves returns a NUL-terminated string, so nothing is lost.

There is deliberately no ``AxlString *`` overload. The type stays exactly where
§4.5 puts it — the streaming builder behind the JSON and XML writer sinks — and
a bridge to it was scoped for this phase and dropped on a count: across every
C++ tree built on this SDK there are zero references to it. Reading one is
``axl::view(axl_string_str(s))``, which allocates nothing and is what the
overload would have done.

.. doxygenfile:: axl-cstr.hpp
   :project: axl
