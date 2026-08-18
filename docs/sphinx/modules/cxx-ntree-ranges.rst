AxlNTree as ranges
==================

.. note::

   One of the C++ layer's seams over the C API — the C2, C3 and C5 phases of
   ``docs/AXL-Cxx-Design.md`` §9. None of them is a container and none is a
   C++ API *over* the C one; each exists only where C++ gives something C
   cannot, which is the rule ``AXL-Cxx-Design.md`` §6b's corollary sets. See
   also :doc:`cxx`.

Header: ``<axl/axl-ntree.hpp>``

.. code-block:: cpp

   #include <axl/axl-ntree.hpp>

   for (AxlNTree *c : axl::children(node)) { ... }
   for (const AxlNTree *p : axl::ancestors(node)) { depth++; }

   for (AxlNTree *n : axl::preorder(root)) {
       draw(axl::data_of<Item>(n));
   }

``axl_ntree_traverse()`` already visits every node, and it takes a function
pointer plus a ``void *``. That boundary is a wall: the visitor cannot inline
into the walk, capture, ``break``, or compose with anything in ``<ranges>``.
``AxlNTree``'s ``parent``, ``next`` and ``children`` links are **public**, so
every walk here is a plain pointer chase with no accessor to inline and no
change to the C side — the thing ``AxlArray`` needed :c:func:`axl_array_data`
before it could offer. The iterator shape is ``llvm::ilist``'s: intrusive
links, with a default-constructed iterator as the end sentinel.

===================  =========================================  ==========================================
Range                Yields                                     Order
===================  =========================================  ==========================================
``children(n)``      the direct children of a node              first to last
``ancestors(n)``     ``parent``, then *its* parent, …           inner to outer; excludes the node
``preorder(n)``      the node and its whole subtree             node, then each child's subtree
``postorder(n)``     the node and its whole subtree             each child's subtree, then node
``preorder_pruned``  the node and the subtrees you allow into   pre-order, skipping any rejected subtree
===================  =========================================  ==========================================

``preorder_pruned(n, descend)`` is the one that cannot be built by composition.
The rejected node is still **visited**; only its subtree is skipped — a
collapsed row in a tree view is still drawn, while everything beneath it is
not. ``preorder(n) | std::views::filter(pred)`` does not do this: ``filter``
drops a node from the *output* after the walk has already descended into it,
so the hidden subtree is still traversed and every node in it still tested.
Here the predicate governs the **descent**:

.. code-block:: cpp

   for (AxlNTree *row : axl::preorder_pruned(root, [](const AxlNTree *n) {
           return axl::data_of<Row>(n)->expanded;
       })) {
       draw(axl::data_of<Row>(row));
   }

It yields an ``input_range`` rather than a ``forward_range``, because the
predicate is stored by value in the iterator and a capturing lambda is not
default-constructible. Its end is ``std::default_sentinel`` for the same
reason.

``preorder`` and ``postorder`` include the node they are given, so
``preorder(root)`` is the whole tree. Both climb back out through the public
``parent`` link rather than keeping a stack, which is why they allocate nothing
and cannot fail.

There is **no** ``level_order`` and no ``in_order``, and that is a cost decision
rather than an oversight: breadth-first needs a queue, so a range for it would
allocate silently inside a ``for`` loop that reads like the four above.
``axl_ntree_traverse()`` implements all four orders and stays the answer for
those two.

These are **borrowed ranges**. They hold node pointers and own nothing, so an
iterator outlives the range it came from — without the
``enable_borrowed_range`` opt-in every ``std::ranges::`` algorithm taking the
range by value returns ``std::ranges::dangling`` and
``std::ranges::find_if(axl::children(n), pred)`` fails to compile at the use
site. That is what keeps them composing the same way as ``array_span``'s
``std::span``.

``iterator_concept`` is ``forward_iterator_tag`` and ``iterator_category`` is
deliberately the weaker ``input_iterator_tag``: ``operator*`` returns a prvalue
``AxlNTree *``, and the C++17 category promises a real reference that this
cannot provide. ``std::ranges`` reads the concept and gets the full forward
guarantee; a C++17-era algorithm reads the category and is told only what is
true. ``iota_view`` and ``transform_view`` do the same.

Each factory has a ``const`` overload yielding ``const AxlNTree *``, so a
``const``-qualified member function walks without casting, plus a
``nullptr``-literal overload so the documented "or NULL" actually compiles. The ranges hold node
pointers and nothing else: unlinking, moving or freeing a node invalidates any
iterator on it or below it, so the destructive
``while (AxlNTree *c = n->children) { axl_ntree_unlink(c); … }`` loop stays a
``while`` loop.

.. doxygenfile:: axl-ntree.hpp
   :project: axl
