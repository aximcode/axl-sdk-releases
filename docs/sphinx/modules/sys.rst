AxlSys — System Utilities
==========================

.. include:: ../../../src/util/README.md
   :parser: myst_parser.sphinx_

API Reference
-------------

AxlSys
~~~~~~

.. doxygenfile:: axl-sys.h

AxlEnv
~~~~~~

.. doxygenfile:: axl-env.h

AxlTime
~~~~~~~

.. doxygenfile:: axl-time.h

AxlNvStore
~~~~~~~~~~

.. doxygenfile:: axl-nvstore.h

AxlDriver
~~~~~~~~~

.. doxygenfile:: axl-driver.h

AxlDriverInfo (driver / device discovery)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Read-only discovery — the shell ``drivers`` / ``devices`` / "is this
controller bound?" views as an API: enumerate loaded
``EFI_DRIVER_BINDING_PROTOCOL`` drivers with names/versions/managed-device
counts, name any handle (ComponentName2 → device-path), map a PCI address
to its controller handle, query the bound driver, and bind a specific
driver to a controller.

.. doxygenfile:: axl-driver-info.h

AxlEfiStatus (UEFI-spec return type + constants)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``<axl/axl-efi-status.h>`` — the ``EFI_STATUS`` return type and common
Appendix-D constants for implementing UEFI-spec protocols without
pulling in all of ``<uefi/axl-uefi.h>``.

.. doxygenfile:: axl-efi-status.h

AxlEmbed
~~~~~~~~

.. doxygenfile:: axl-embed.h

AxlDiag
~~~~~~~

.. doxygenfile:: axl-diag.h

AxlHexdump
~~~~~~~~~~

.. doxygenfile:: axl-hexdump.h
