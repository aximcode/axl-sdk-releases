axl::unique_handle
==================

Unique ownership of an AXL C handle.

``AXL_AUTOPTR`` already gives C scope-exit cleanup and is unchanged. What it
cannot be is a class member, a moved value, or a factory's return type — it is
a GCC ``cleanup`` attribute on a local, so the long-lived cases fall back to a
raw pointer and a hand-written destructor. ``axl::unique_handle<T>`` is the
same ownership for exactly those three.

.. code-block:: cpp

   #include <axl/axl-handle.hpp>

   axl::unique_handle<AxlLoop> loop{axl_loop_new()};
   axl_loop_run(loop.get());          // the C API, unchanged

The handle stays the C handle: ``get()`` hands it to any ``axl_*`` function and
``release()`` gives ownership back. The deleter is stateless, so
``sizeof(axl::unique_handle<T>) == sizeof(T *)`` and the destroy call inlines to
the one a hand-written destructor would emit. There is no wrapper class per
handle and no C++ API over the C one — see ``AXL-Cxx-Design.md`` §6, which asks
this layer to earn its place only where C++ gives something C cannot.

Opt-in per type
---------------

The deleter resolves through ``axl::handle_traits<T>``, which a header emits by
invoking ``AXL_DEFINE_AUTOPTR_CLEANUP``. A type therefore acquires a C++ handle
by the same line that gives C its cleanup attribute, and the two cannot drift:
they are one macro invocation on one line. That covers 61 types, including the
three whose destructor takes a teardown mode (``AxlSocket``, ``AxlTcp``,
``AxlHttpServer``), which inherit the same ``AXL_TEARDOWN_GRACEFUL`` the C
cleanup passes.

A type with no binding is a **compile error** rather than a plausible-looking
double free. Two say why, via ``AXL_DEFINE_NO_HANDLE``:

``AxlSurface``
   ``axl_surface_new`` returns a borrowed node in a tree the compositor owns,
   and ``axl_compositor_free`` destroys every surface in it. Holding both as
   members would make correct teardown depend on declaration order.

``AxlJsonReader``
   A caller-owned value struct, normally a local. ``axl_json_free`` releases
   what the reader holds; it does not free the struct.

The same care applies to a pointer the SDK lends you. A buffer from
``axl_surface_buffer()`` is owned by the surface and has the identical type as
one from ``axl_gfx_buffer_new()``, so no template can tell them apart. Own what
a ``_new`` or ``_open`` gave you, and nothing else.

There is no shared or refcounted form: AXL's ownership is a tree. The one
refcounted type, ``AxlBytes``, binds ``axl_bytes_unref``, so a handle over it
owns one reference.

.. doxygenfile:: axl-handle.hpp
   :project: axl
