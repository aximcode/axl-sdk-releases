axl::gfx_target_scope
=====================

.. note::

   One of the C++ layer's seams over the C API — the C2, C3 and C5 phases of
   ``docs/AXL-Cxx-Design.md`` §9. None of them is a container and none is a
   C++ API *over* the C one; each exists only where C++ gives something C
   cannot, which is the rule ``AXL-Cxx-Design.md`` §6b's corollary sets. See
   also :doc:`cxx`.

Header: ``<axl/axl-gfx-surface.hpp>``

.. code-block:: cpp

   #include <axl/axl-gfx-surface.hpp>

   void Widget::render(AxlGfxBuffer *back)
   {
       axl::gfx_target_scope target{back};   // saves the caller's, installs back
       axl_gfx_fill_rect(...);
       for (Widget *c : children_) {
           c->render(c->buffer());           // nests; restores to back on exit
       }
   }                                         // restores the caller's target

Save-and-restore, not set-and-clear. :c:func:`axl_gfx_target_buffer` sets a
single global target, which makes the naive guard "set on entry,
``axl_gfx_target_buffer(NULL)`` on exit" wrong the moment anything nests:
``NULL`` is not "no target", it is *the screen*, so an inner scope resetting to
it silently redirects the rest of an outer widget's painting from its back
buffer onto the display. This captures the target active at construction and
puts that back. A ``NULL`` argument is legitimate and means the screen, so a
headless path needs no special case.

It is concrete rather than a generic ``scope_guard<T>``: the whole public API
has exactly one save-and-restore-a-global pair, so a template would be an
abstraction with a single caller — and a generic guard must be told the getter,
the setter and the value, which is longer at the call site than the thing it
abstracts.

Two lifetime requirements the class cannot enforce, both recorded as
``@warning`` on the type. The **saved target must outlive the scope**: this
holds a private copy of the outgoing buffer, which ``axl_gfx_buffer_free()``
cannot see — its own defensive clear fires only for the target currently
*installed*, and by construction that is never the saved one, so freeing an
outer buffer while an inner scope is open makes the restore install a dangling
pointer. And the **clip stack is a second global that is not saved**, so a clip
pushed against the outer buffer is silently reinterpreted in the inner buffer's
coordinate space.

The class is ``[[nodiscard]]``, so forgetting the variable name —
``axl::gfx_target_scope{back};``, which constructs and destroys inside one
full-expression and redirects nothing — is a diagnostic rather than a rendering
bug with no symptom.

Neither copyable nor movable. The restore must happen exactly once, at the end
of the scope that redirected; a movable guard could outlive its scope or be
dropped early, which is a rendering bug with no diagnostic.

.. doxygenfile:: axl-gfx-surface.hpp
   :project: axl
