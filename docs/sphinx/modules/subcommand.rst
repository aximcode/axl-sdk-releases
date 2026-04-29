AxlSubcommand — Multi-command CLI dispatch
===========================================

Helper for UEFI applications that expose multiple distinct operations
under a common executable (`do bios`, `do sysid`, `do crb` ...).
Pairs with :doc:`config` — each subcommand uses its own AxlConfig
descriptor table for flag parsing.

See :doc:`sys` for an overview of all utility modules.

Header: ``<axl/axl-subcommand.h>``

Single-purpose tools (e.g. ``mkrd``) skip this layer and use
``axl_config_*`` directly. Multi-command tools (e.g. ``do.efi``)
declare an ``AxlSubcommand`` table and call
``axl_subcommand_dispatch`` from ``main``.

API Reference
-------------

.. doxygenfile:: axl-subcommand.h
