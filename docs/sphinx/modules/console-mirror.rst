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

AxlConsoleTap — the structured console underneath
-------------------------------------------------

The firmware surgery lives in :cpp:type:`AxlConsoleTap`, and the mirror is
just one of its consumers. The tap wraps the console protocols, owns the
``SIMPLE_TEXT_OUTPUT_MODE`` the guest reads back (cursor, attribute,
visibility), overrides the reported geometry, tracks the alternate screen,
and runs the key-injection ring — then reports every console *operation*
to an :cpp:struct:`AxlConsoleOps` vtable: ``set_cell_rule``,
``clear_screen``, ``set_cursor``, ``output_text``, ``set_pen``,
``set_mode``, and ``set_term_prop``. The vtable is a two-producer contract —
the tap here, plus ``axl-vterm`` over a real VT byte stream — so it also
carries the rect/scroll ops a VT parser needs (``erase``, ``moverect``,
``bell``, ``scrollrect``) that the tap never emits.
Graphic rendition arrives as a whole-pen snapshot (``set_pen``), and cursor
visibility and the alternate screen arrive through the one extensible
``set_term_prop`` channel.

``EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`` has no wire format — a full-screen app
just calls ``SetCursorPosition`` / ``OutputString``. The VT byte stream
exists only to serialize the console to a **remote** terminal, which is
exactly what ``AxlConsoleMirror`` does as a tap consumer. A **local**
renderer binds the ops straight into its cell grid, skipping the
encode→parse round trip (and can therefore express state, like the
alternate screen, that the VT wire can only approximate).

Two flags matter for a local renderer: ``passthrough_local = false`` (the
tap becomes the only console writer, so it owns and maintains the Mode) and
``input_capture = true`` (the wrapped ``ConIn``/``ConInEx`` serve *only*
injected keys, so a consumer that drains the firmware key queue itself
can't have its keys stolen or doubled by the guest).

AxlConsoleTerm — the local cell-grid renderer
---------------------------------------------

:cpp:type:`AxlConsoleTerm` is that local renderer: the on-screen sink for
:cpp:struct:`AxlConsoleOps`, the counterpart to ``AxlConsoleMirror``. Where
the mirror serializes ops to a **remote** VT byte stream,
``AxlConsoleTerm`` binds them straight into a cell grid drawn on the GOP
framebuffer (or an off-screen :cpp:type:`AxlGfxBuffer` a compositor
composites). Hand :cpp:func:`axl_console_term_ops` to
``axl_console_device_install`` and the take-over console renders locally.

It is deliberately **mechanism, not policy**. The terminal owns the
grid + scrollback ring, the render/dirty-row machinery, reflow
(:cpp:func:`axl_console_term_set_font`,
:cpp:func:`axl_console_term_resize`,
:cpp:func:`axl_console_term_set_bounds`,
:cpp:func:`axl_console_term_set_palette`), and selection + copy
(:cpp:func:`axl_console_term_selection_start` …
:cpp:func:`axl_console_term_selection_copy` → :doc:`clipboard`). The
consumer binds *gestures* to those mechanisms —
:cpp:func:`axl_console_term_handle_pointer` (wheel → scroll, drag →
select, Ctrl+wheel → a caller ``on_zoom`` hook) and
:cpp:func:`axl_console_term_handle_hotkey` (Shift+PgUp/PgDn,
Ctrl+Shift+C) are ready-made bindings, but a richer consumer (a widget
toolkit) can ignore them and drive the mechanisms directly while keeping
its own keymap and theming. Keys reach the shell through the device's
input relay; the terminal peeks its own hotkeys via the device
``key_filter`` (its signature matches ``handle_hotkey``, so it wires as
one). The standalone ``fbcon`` tool is the thin driver that stitches
device + terminal + pointer together.

AxlConsoleScreen — server-side screen model + snapshot
------------------------------------------------------

:cpp:type:`AxlConsoleScreen` closes the **late-join** gap: a viewer that
joins a live serial / SOL session mid-stream sees a blank pane until the
guest next repaints. Feed the screen model the same VT/xterm byte stream
every viewer receives (:cpp:func:`axl_console_screen_feed`), and on a new
connection call :cpp:func:`axl_console_screen_snapshot` to serialize the
*current* screen as one self-contained VT "repaint" — clear, the visible
cells with their colours (run-length SGR, blank cells and blank rows
skipped so a mostly-empty 80×25 is a handful of bytes, not ~2 KB of
spaces), then the cursor and alt-screen state. The late joiner applies that
burst to its blank terminal and immediately sees what everyone else sees.

It is the **inverse of** ``AxlConsoleMirror``: the mirror serializes the
*tap* producer to a live VT stream, while the screen model drives
:doc:`vterm` (Layer 2) into an owned cell grid and serializes *that* back to
VT — the two share one pen→SGR encoder so the wire format cannot drift. The
model owns a **primary and an alternate grid**, swapping on the guest's
alt-screen (``DECSET/DECRST 1049``) like a real terminal, so a snapshot
taken after a full-screen app exits repaints the intact primary rather than
the stale alternate buffer. The snapshot carries cell glyphs, per-cell pen
(indexed / 24-bit RGB / styles), cursor position and visibility, the
active/alternate selection, and whole-screen reverse video; it does not set
the terminal size, so the receiving terminal must already be the right
geometry.

**Mirror consumers get late-join for free.**
:cpp:func:`axl_console_mirror_snapshot` composes an internal
``AxlConsoleScreen`` fed from the mirror's *own* emitted VT stream, so a
WebSocket server repaints a newly-connected client in one call instead of
replaying a raw byte tail (which cannot recover screen contents, the cursor,
or the alternate-screen selection). Because the model lives inside the mirror
— the console abstraction that already owns the geometry
(:cpp:func:`axl_console_mirror_set_size`) and the authoritative alt-screen
state — it stays in sync automatically: no consumer-side parallel parser to
double-maintain, and the alt-screen and resize transitions the mirror drives
flow straight into the model.

API Reference
-------------

.. doxygenfile:: axl-console-ops.h

.. doxygenfile:: axl-console-tap.h

.. doxygenfile:: axl-console-device.h

.. doxygenfile:: axl-console-term.h

.. doxygenfile:: axl-console-mirror.h

.. doxygenfile:: axl-console-screen.h
