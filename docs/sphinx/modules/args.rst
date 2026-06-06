AxlArgs — Argument Parsing
==========================

Declarative command-line parsing. A static ``AxlArgsNode`` tree
describes the program — its flags, positionals, and any nested
subcommands — and ``axl_args_run`` parses ``argc`` / ``argv`` against
it and dispatches to the matching handler. The same ``AxlArgsNode``
type describes the root program **and** every subcommand, so a
multi-level CLI (``tool bios get …``) is just a tree of nodes with no
bespoke dispatch code. Parsed values arrive typed (bool / int / string)
with validation driven by each option's declared kind.

This is the current CLI parser. It replaces the deprecated
:doc:`subcommand` dispatcher (``axl_subcommand_*``) — new tools should
use ``axl_args_run``.

API Reference
-------------

.. doxygenfile:: axl-args.h
