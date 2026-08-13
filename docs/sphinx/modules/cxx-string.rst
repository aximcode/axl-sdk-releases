axl::string
===========

An owning, growing UTF-8 string with ``std::string``'s interface and a
small-string optimisation, usable in a **freestanding** translation unit where
``<string>`` is unavailable.

Content up to ``sso_capacity`` (23 bytes — sized so ``\EFI\BOOT\BOOTX64.EFI``
stays inline) lives inside the object and never allocates, so such a string
cannot fail. It is deliberately **not** a wrapper over ``AxlString``:
``AXL-Cxx-Design.md`` §4.5 measured that shape at 9.2× the cost of this on
short-string construction, because a handle that is a pointer to a heap object
has nowhere to put the inline bytes. Re-measured before and after the change on
one machine: ctor 6.9× faster, copy 9.4×.

``AxlString`` keeps the job §4.5 measured it *tied* on — the streaming builder
behind the JSON and XML writer sinks.

On ``axl-c++ --hosted``, prefer ``std::string`` anyway — it is the one every
C++ programmer already knows. This exists for the configuration that cannot
have it, which is the one :doc:`cxx-ostream` and :doc:`cxx-istream` are built
for.

The search family (``find``, ``rfind``, ``find_first_of``, ``substr``,
``compare``, ``starts_with``, ...) forwards to ``std::string_view``, which IS
in the freestanding subset — so those are libstdc++'s own algorithms reading
our bytes, not reimplementations that can drift.

See :doc:`cxx` for how this fits beside the standard containers.

.. doxygenfile:: axl-string.hpp
   :project: axl
