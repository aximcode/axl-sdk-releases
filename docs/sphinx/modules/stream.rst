AxlStream — byte-stream abstraction
===================================

.. include:: ../../../src/stream/README.md
   :parser: myst_parser.sphinx_

API Reference
-------------

.. doxygenfile:: axl-stream.h

AxlConsole — interactive console input + text-mode control
----------------------------------------------------------

Single-keystroke input with a timeout (``axl_console_read_key``), plus the
text-output mode surface the UEFI Shell's ``mode`` command exposes:
enumerate the character-cell geometries the active console supports
(``axl_console_text_mode_count`` / ``query_mode`` / ``current_mode`` /
``find_mode`` / ``max_mode``) and switch between them
(``axl_console_text_set_mode``). The graphics-free peer of the AxlGfx
display-mode API. ``axl_console_set_page_break`` toggles the shell's own
screen-at-a-time output paging (the ``-b`` convention) — delegated to
``EFI_SHELL_PROTOCOL``, not reimplemented — and ``axl_args_run`` wires a
universal ``-b`` / ``--page`` option to it automatically.

.. doxygenfile:: axl-console.h

AxlFs — filesystem operations
-----------------------------

.. doxygenfile:: axl-fs.h

AxlFileView — mmap-like windowed view over a file
-------------------------------------------------

.. doxygenfile:: axl-file-view.h

AxlFsProvider — publish a UEFI-visible filesystem
-------------------------------------------------

.. doxygenfile:: axl-fs-provider.h

AxlDevicePath — UEFI device-path constructors
---------------------------------------------

.. doxygenfile:: axl-device-path.h
