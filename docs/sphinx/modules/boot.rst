AxlBoot — boot-option management
=================================

Typed wrappers over the ``Boot####`` / ``BootOrder`` / ``BootNext`` /
``BootCurrent`` firmware-variable family. The ``EFI_LOAD_OPTION``
wire codec is internal — consumers operate on ``AxlBootOption``
structs, never on raw bytes.

See :doc:`sys` for the underlying ``axl_nvstore_*`` namespace machinery.

API Reference
-------------

.. doxygenfile:: axl-boot.h
