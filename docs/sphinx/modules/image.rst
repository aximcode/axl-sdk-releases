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

AxlImageVerify — Authenticode signature inspection
---------------------------------------------------

PE Authenticode signature inspection without launching the image —
two-axis check (presence + Secure-Boot-db validity) for offline
integrity-check tooling. See ``include/axl/axl-image-verify.h``
for the side-effect contract on the ``consult_db`` path.

.. doxygenfile:: axl-image-verify.h

AxlShell — launch a real UEFI Shell
------------------------------------

Find a ``Shell.efi`` and run it as a foreground child image
(``StartImage`` blocks until it exits), with ``-nostartup`` so a
child Shell launched *from* ``startup.nsh`` doesn't recurse. The
AXL-ified form of the EDK2 ShellLauncher; pairs with
:doc:`console-mirror` to host the real Shell over a remote terminal.

.. doxygenfile:: axl-shell.h
