axl::cin
========

Formatted input for freestanding C++, over ``axl_readline``.

Extraction follows libstdc++: ``>>`` skips leading whitespace, extracts to the
next whitespace or end of input, and sets a sticky fail state if it extracts
nothing. Newline is ordinary whitespace, so a chained ``>>`` spans lines. On a
parse failure the cursor is left AT the offending character and the target is
not modified.

Both failure spellings observe the same state, so they mix freely — ``if
(!axl::cin)`` after a chain, or ``read<T>()`` returning an ``axl::result``
where the failure has to be handled as a value.

Input is read a line at a time through ``axl_stdin_text()``. That is what makes
``<`` redirection, both pipe forms, and an interactive console all work through
one path: the shell's default ``|`` transcodes to UCS-2, and reading raw
``axl_stdin`` would make ``echo 42 | prog`` parse as ``4``.

See :doc:`cxx-string` for the type ``>>`` fills, and :doc:`cxx-ostream` for
output.

.. doxygenfile:: axl-istream.hpp
   :project: axl
