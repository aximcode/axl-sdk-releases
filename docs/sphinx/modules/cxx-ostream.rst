axl::cout / axl::cerr
=====================

Formatted output for freestanding C++, over ``axl_printf`` / ``axl_printerr``.
Costs roughly 700 bytes over an equivalent ``axl_printf`` program (x64
``--release``), with no flag of any kind. That is a selective-link delta,
not a fixed size — it moves with the library.

``axl::endl`` writes ``"\n"``, not ``"\r\n"``: the console stream translates LF
to CRLF itself, so a CRLF here would reach the wire as ``\r\r\n``. Unlike
``std::endl`` it does not flush, because AXL's standard streams are
``AXL_STREAM_BUF_NONE`` by default.

See :doc:`cxx` for why ``std::cout`` is not the answer, and :doc:`cxx-istream`
for the input side.

.. doxygenfile:: axl-ostream.hpp
   :project: axl
