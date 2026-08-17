C++ Layer
=========

The ``axl`` namespace. The standard library supplies the containers and the
algorithms; this layer supplies the error vocabulary, the runtime that lets
libstdc++ link into a firmware image at all, and an allocator for the paths
the standard containers cannot serve. See ``docs/AXL-Cxx-Design.md`` for the
measurements behind that split.

Requires a C++23 toolchain via ``axl-c++``. Every header here is C++-only and
refuses to compile as C.

Standard containers
-------------------

``std::vector``, ``std::string``, ``std::map`` and ``std::unordered_map`` all
run under UEFI, are the right default, and need **no flag**. libstdc++ used to
refuse them under ``-ffreestanding`` at ``bits/requires_hosted.h`` — that flag,
not exceptions and not the heap, was the whole gate — and the SDK no longer
passes it, so there is nothing to opt into. (The ``axl-c++ --hosted`` flag that
once lifted it per translation unit is removed and now hard-errors; delete it
from any build that still passes it.) ``libaxl-cxx.a`` supplies what the link
needs: ``operator new`` / ``delete`` over ``axl_malloc`` — every form,
including nothrow (with the
``std::nothrow`` object) and over-aligned — the five ``std::__throw_*`` entry points that
``-fno-exceptions`` calls instead of throwing, ``ceil``, and AXL's own
``_Prime_rehash_policy`` so ``std::unordered_map`` links without dragging in an
archive member that may carry AVX above the SDK's ``-march=x86-64`` baseline.

Allocation failure HALTS. That is deliberate — ``operator new`` may not return
NULL, because libstdc++ never checks it — but it means a standard container
cannot participate in AXL's recoverable-OOM contracts. See
:doc:`arena-allocator` for the paths where that matters, and for containers
that have to run on an application processor.

.. doxygenfile:: axl-cxx.hpp
   :project: axl

Owning a C handle
-----------------

``AXL_AUTOPTR`` gives C scope-exit cleanup and is unchanged. What it cannot be
is a class member, a moved value, or a factory's return type — it is a GCC
``cleanup`` attribute on a local. ``axl::unique_handle<T>`` is the same
ownership for the cases that need those three.

.. code-block:: cpp

   axl::unique_handle<AxlLoop> loop{axl_loop_new()};
   axl_loop_run(loop.get());          // the C API, unchanged

The handle stays the C handle. The deleter is stateless, so
``sizeof(axl::unique_handle<T>) == sizeof(T *)`` and the destroy call inlines
to the one a hand-written destructor would emit. There is no wrapper class per
handle and no C++ API over the C one.

It is **opt-in per type**. The deleter resolves through
``axl::handle_traits<T>``, which a header emits by invoking
``AXL_DEFINE_AUTOPTR_CLEANUP`` — so a type acquires a C++ handle by the same
line that gives C its cleanup attribute, and the two cannot drift. A type with
no binding is a compile error rather than a plausible-looking double free,
which is the point: ``AxlSurface`` is a borrowed node in a tree the compositor
owns, and ``AxlJsonReader`` is a caller-owned value struct whose ``free``
releases contents rather than the struct. Both name their reason via
``AXL_DEFINE_NO_HANDLE``, so the attempt reports the ownership rule instead of
an ``incomplete type``.

There is no shared or refcounted form. The one refcounted type, ``AxlBytes``,
binds ``axl_bytes_unref``, so a handle over it owns one reference.

The API reference and the full ownership rules live on :doc:`cxx-handle`.

Streams and strings
-------------------

``axl::string``, ``axl::cout`` and ``axl::cin`` were written for a
configuration that had no ``<string>`` at all: a freestanding translation unit,
where ``bits/requires_hosted.h`` gated it along with the containers. T3 retired
that mode, so that reason is gone — but they are kept for a better one, and it
is the layer's whole point: **out-of-memory is a value.** ``std::string`` halts
on an allocation failure under ``-fno-exceptions``; ``axl::string`` sets a
sticky ``bad()``, which is what lets ``axl::cin >> s`` report an accumulation
OOM as ``AXL_NO_RESOURCES`` instead of taking the image down. See
:doc:`cxx-string`.

``axl::cout`` alone costs roughly 700 bytes over an equivalent ``axl_printf``
program, with no flag of any kind. Pulling in ``axl::cin`` and ``axl::string``
as well brings it to about 7 KB. Both are selective-link deltas rather than
fixed sizes, so they move with the library.

They mirror the standard's spelling on purpose, so porting hosted code stays
``s/std::/axl::/``. ``axl::cout`` rather than ``axl::out`` because ``axl::err``
is already the error constructor in :doc:`this page's header <cxx>` —
declaring it as a stream object is a hard compile error.

``std::cout`` itself is not available, and the reason is measured rather than
assumed: it needs 23 libstdc++ symbols, 11 of them ``_Unwind_*``, and an image
that shims all 23 links at 155 KB and then faults inside
``std::ostream::sentry::sentry`` because ``ios_base::Init`` never ran. See
``docs/AXL-Cxx-Stdlib-Surface.md`` section 6.

.. code-block:: cpp

   axl::string host;
   uint16_t    port;

   axl::cin >> port >> host;          // chains; sticky fail state
   if (!axl::cin) {
       return axl::err(AXL_INVALID);  // one check covers the run
   }

   auto retries = axl::cin.read<uint32_t>();   // axl::result<uint32_t>
   if (!retries) {
       axl::cin.clear();
   }

   axl::cout << host << ':' << port << axl::endl;

Two deliberate divergences from ``std::``. Out-of-memory sets a sticky
``bad()`` flag and leaves the string unchanged rather than halting, because
firmware is expected to survive exhaustion and AXL treats OOM as a value
everywhere else. ``at()`` still halts, because an out-of-range index is a
program bug rather than an environmental condition, and that is precisely
``std::string::at``'s contract.

The API reference for each lives on its own page:
:doc:`cxx-string`, :doc:`cxx-ostream`, :doc:`cxx-istream`.
