Arena Allocator
===============

:cpp:class:`axl::arena_allocator` is a standard Allocator backed by an
``AxlArena``, for the two things ``axl_malloc`` structurally cannot do behind a
standard container.

**Out-of-memory that must not halt.** A standard container has nowhere to put
an allocation failure: under ``-fno-exceptions`` it lowers to a halt, and
``operator new`` may not soften that by returning NULL, because libstdc++ hands
the result to the container unchecked. An arena has a fixed capacity, so the
failure moves to one condition the caller tests up front — where an answer
other than "halt" is available.

**Application processors.** ``axl_malloc`` goes through boot services, which do
not exist on an AP. ``axl_arena_alloc`` is a lock-free CAS bump, so an
arena-backed container is usable from MP-dispatched code where a
default-allocator one is not.

The cost is that ``deallocate`` cannot reclaim. Size the arena for the PEAK,
including the buffers a container allocates while growing, and ``reserve()`` up
front.

.. doxygenfile:: axl-arena-allocator.hpp
   :project: axl
