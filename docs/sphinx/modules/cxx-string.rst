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

This class was originally written for a configuration that could not have a
``std::string`` — a freestanding translation unit, where ``<string>`` was gated
by ``bits/requires_hosted.h``. T3 retired that mode, so **that reason is gone**.

It is kept for a better one: **out-of-memory is a value here.** ``std::string``
has nowhere to put an allocation failure — under ``-fno-exceptions`` it halts
the image, and ``operator new`` may not return NULL because libstdc++ hands the
result to the container unchecked. ``axl::string`` sets a sticky ``bad()`` and
leaves the contents untouched. :doc:`cxx-istream` depends on that: ``axl::cin
>> s`` reports an accumulation OOM as ``AXL_NO_RESOURCES`` by reading ``bad()``
off its accumulator, which with ``std::string`` would have been a halt below
the stream with no value left to report. It is also the cheaper of the two —
measured on x64 ``--release``, an equivalent construct-append-grow program is
**564 bytes** with this class and **1045 bytes** with ``std::string``.

**Prefer ``std::string``** on any path that can pre-size or may legitimately
halt; it is the one every C++ programmer already knows, and
:doc:`arena-allocator` gives it a pre-checked capacity where that fits. Reach
for ``axl::string`` when a path must *survive* exhaustion and cannot know the
size up front.

The search family (``find``, ``rfind``, ``find_first_of``, ``substr``,
``compare``, ``starts_with``, ...) forwards to ``std::string_view``, which IS
in the freestanding subset — so those are libstdc++'s own algorithms reading
our bytes, not reimplementations that can drift.

See :doc:`cxx` for how this fits beside the standard containers.

.. doxygenfile:: axl-string.hpp
   :project: axl
