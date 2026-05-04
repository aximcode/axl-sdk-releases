AxlSubcommand — Multi-command CLI dispatch
===========================================

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
