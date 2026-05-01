AxlImage — executable-image lifecycle
======================================

Backend-neutral wrapper for loading, starting, and unloading
executable images. On UEFI, maps to ``LoadImage`` / ``StartImage``
/ ``UnloadImage``; on a future Linux backend the same shape would
map to ``posix_spawn``. Consumers operate on an opaque
``AxlImage *`` handle — the underlying ``EFI_HANDLE`` never crosses
the public API.

Sibling to :doc:`sys` (``axl_driver_*`` for DXE-driver lifecycle,
which AxlImage delegates to internally for its load and unload
paths). The one place AxlImage diverges from AxlDriver is
``axl_image_start``, which captures the image's exit code —
something ``axl_driver_start`` discards because drivers aren't
expected to exit cleanly.

API Reference
-------------

.. doxygenfile:: axl-image.h
