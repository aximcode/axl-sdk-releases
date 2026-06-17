AxlConsoleMirror — mirror the firmware console to a remote terminal
====================================================================

Lets a loop-owning AXL application host the **real UEFI Shell** (or any
console app) with its console transparently mirrored to — and driven
from — a remote terminal, including full-screen interactive apps like
the Shell's ``edit``.

The mirror wraps the system console protocols
(``gST->ConIn``/``ConOut``/``StdErr`` and the ``ConsoleInHandle``
SimpleTextInputEx): console *output* is translated to a terminal byte
stream (UTF-8 text + ANSI/VT control sequences) handed to a caller sink;
remote *input* injected via :cpp:func:`axl_console_mirror_inject_text`
(xterm/VT decode) or :cpp:func:`axl_console_mirror_inject_key` is pushed
into the console key queue and the blocked reader is woken. It pairs with
:doc:`image` (``axl_shell_launch``) to put a real ``Shell.efi`` in the
foreground while the loop is pumped in the background
(``axl_loop_attach_driver``).

.. warning::

   A mirrored real Shell is **full pre-OS control over the wire** —
   arbitrary firmware, block-device, and filesystem access. The
   substrate ships no auth/authz; consumers **MUST** gate it hard
   (strong authentication on the carrying channel, admin-only
   authorization, transport encryption, an explicit audited action).
   See ``docs/AXL-Console-Mirror-Design.md`` §9.

The mirror is a single global resource (one console ⇒ one session) and
owns no pump or timer — the consumer drives the loop. See the design doc
for the layering (substrate owns the mechanism; the consumer owns
transport, RBAC, terminal size, and late-join policy).

API Reference
-------------

.. doxygenfile:: axl-console-mirror.h
