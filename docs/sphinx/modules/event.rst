Event Primitives — AxlEvent, AxlCancellable, AxlWait
=====================================================

The foundational synchronization primitives. All three compose: an
``AxlCancellable`` is an ``AxlEvent`` with stop-token semantics; the
wait helpers drive a throwaway ``AxlLoop`` until an event fires, a
condition holds, a timeout elapses, or Ctrl-C is received.

- ``AxlEvent`` — one-shot latch wrapping a UEFI event. Replaces the
  older ``AxlCompletion`` (same mechanical behavior, UEFI-native name).
- ``AxlCancellable`` — typed stop token shared across async ops;
  cancel it once and every op observing it aborts with
  ``AXL_CANCELLED``.
- ``AxlWait`` — interruptible wait helpers (``axl_wait_for``,
  ``axl_wait_for_flag``, ``axl_wait_ms``, ...) built on ``AxlLoop``.

A note on naming: "event" appears three times in AXL docs — the
event **loop** (the dispatcher), an event **source** (a thing
registered with the loop), and ``AxlEvent`` (one kind of source).
UEFI carries the same overload; an ``AxlEvent`` is a one-shot latch
backed by a UEFI event, and the event loop dispatches them.

Headers:
``<axl/axl-event.h>``,
``<axl/axl-cancellable.h>``,
``<axl/axl-wait.h>``.

API Reference
-------------

AxlEvent
~~~~~~~~

.. doxygenfile:: axl-event.h

AxlCancellable
~~~~~~~~~~~~~~

.. doxygenfile:: axl-cancellable.h

AxlWait
~~~~~~~

.. doxygenfile:: axl-wait.h
