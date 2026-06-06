AxlSubcommand — Multi-command CLI dispatch
===========================================

.. warning::

   **Deprecated.** Use :doc:`args` (``axl_args_run`` from
   ``<axl/axl-args.h>``) instead — it handles flags, positionals, and
   arbitrarily nested subcommands in one declarative ``AxlArgsNode``
   tree. The ``axl_subcommand_*`` functions are marked
   ``__attribute__((deprecated))`` and remain only for existing
   consumers; new tools should not use them.

Helper for UEFI applications that expose multiple distinct operations
under a common executable (e.g. ``tool bios``, ``tool sysid``,
``tool crb`` ...). Pairs with :doc:`config` — each subcommand uses
its own AxlConfig descriptor table for flag parsing.

See :doc:`sys` for an overview of all utility modules.

Header: ``<axl/axl-subcommand.h>``

Single-purpose tools (e.g. ``mkrd``) skip this layer and use
``axl_config_*`` directly. Multi-command tool wrappers declare an
``AxlSubcommand`` table and call ``axl_subcommand_dispatch`` from
``main``.

API Reference
-------------

.. doxygenfile:: axl-subcommand.h
