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
run under UEFI, and are the right default. They need ``axl-c++ --hosted``:
libstdc++ refuses them under ``-ffreestanding`` at ``bits/requires_hosted.h``,
and that flag — not exceptions, and not the heap — is the whole gate.
``libaxl-cxx.a`` supplies what the link then needs: ``operator new`` /
``delete`` over ``axl_malloc`` — every form, including nothrow (with the
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

Streams and strings, freestanding
---------------------------------

The containers above are the right default *when you have them*. A plain
freestanding translation unit does not: ``bits/requires_hosted.h`` gates
``<string>`` along with the rest, so there is no owning string to read a token
into. ``axl::string``, ``axl::cout`` and ``axl::cin`` fill exactly that gap,
and ``axl::cout`` alone costs roughly 700 bytes over an equivalent
``axl_printf`` program, with no ``--hosted`` anywhere. Pulling in
``axl::cin`` and ``axl::string`` as well brings it to about 7 KB. Both are
selective-link deltas rather than fixed sizes, so they move with the
library.

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
