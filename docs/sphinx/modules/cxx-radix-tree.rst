axl::radix_tree
===============

.. note::

   One of the C++ layer's seams over the C API — the C2, C3 and C5 phases of
   ``docs/AXL-Cxx-Design.md`` §9. None of them is a container and none is a
   C++ API *over* the C one; each exists only where C++ gives something C
   cannot, which is the rule ``AXL-Cxx-Design.md`` §6b's corollary sets. See
   also :doc:`cxx`.

Header: ``<axl/axl-radix-tree.hpp>``

.. code-block:: cpp

   #include <axl/axl-radix-tree.hpp>

   axl::radix_tree<Route> routes;
   routes.insert("/api/v1/", &v1);

   const char *tail = nullptr;
   if (Route *r = routes.lookup_prefix("/api/v1/users/7", &tail)) { ... }

   int shown = 0;                                    // a CAPTURING visitor
   routes.for_each([&](const char *key, Route *r) { if (r->visible) shown++; });

Three things C cannot do, which is the whole scope:

**Ownership is the destructor.** The tree is freed on scope exit, on
reassignment, and on the error path a hand-written ``goto out`` forgets.

**The payload is typed.** ``lookup()`` returns ``T *``; the ``void *`` round
trip and its ``static_cast`` at every call site are gone.

**The visitor can capture.** ``axl_radix_tree_foreach()`` takes a function
pointer plus a ``void *``, so every real visitor is a hand-packed context
struct. ``for_each()`` takes any callable and the lambda inlines into the walk.

Move-only, because the C handle is: ``AxlRadixTree`` has no refcount and AXL's
ownership is a tree, so copying is deleted rather than deep-copying. The
constructor allocates and can fail; with no exceptions available it leaves the
object empty instead of halting, ``valid()`` reports it, and every operation on
an empty tree is a safe no-op returning what a miss would.

A NULL value is storable and behaves like any other entry — counted once by
``size()``, visited by ``for_each()``, removable by ``remove()``. Only
``lookup()`` cannot tell it from an absent key, because its return type has no
spare value to say so; ``for_each()`` is the way to ask when the difference
matters.

``for_each()`` is the one member that can fail silently: the C walk
rebuilds each key into a heap buffer, and on exhaustion it returns mid-walk with
no report. A visit count below ``size()`` means exhaustion, not an empty tree.

The value destructor is the C ``AxlDestroyNotify`` and takes ``void *``,
deliberately rather than a typed ``void (*)(T *)``. The typed form reads better
and can only be delivered to the C tree by ``reinterpret_cast``ing the function
pointer — calling through a different type than the function was defined with.
There is nowhere to hang a trampoline instead, because the C tree calls the
destructor with the value alone and carries no user-data slot for it, unlike
``for_each()``.

.. doxygenfile:: axl-radix-tree.hpp
   :project: axl
