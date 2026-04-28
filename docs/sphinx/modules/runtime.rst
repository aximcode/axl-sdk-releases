AxlRuntime — lifecycle services
================================

.. include:: ../../../src/runtime/README.md
   :parser: myst_parser.sphinx_

API Reference
-------------

AxlSignal (interrupts + blessed exit)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. doxygenfile:: axl-signal.h

AxlAtexit (LIFO cleanup callbacks)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. doxygenfile:: axl-atexit.h

AxlRuntime (default loop, yield, registry inspection)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. doxygenfile:: axl-runtime.h
