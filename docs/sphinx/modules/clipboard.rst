AxlClipboard — Cut / Copy / Paste
=================================

UEFI has no system clipboard, so an editor that wants copy/paste — even
just within its own process, across buffers — needs one. ``AxlClipboard``
is a process-global owned byte buffer with an optional MIME type string:
``axl_clipboard_set`` copies bytes in (freeing the previous contents),
``axl_clipboard_get`` borrows a pointer to them (valid until the next
set / clear), and ``axl_clipboard_clear`` empties it. Byte-oriented and
content-agnostic — store UTF-8 text, a UTF-16 region, a serialized
selection, or an image, and tag it with a MIME type the paste side can
check. Single-threaded; not persisted across a reboot.

Header: ``<axl/axl-clipboard.h>``

API Reference
-------------

.. doxygenfile:: axl-clipboard.h
