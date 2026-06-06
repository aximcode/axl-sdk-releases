AxlData — Data Structures
==========================

.. include:: ../../../src/data/README.md
   :parser: myst_parser.sphinx_

API Reference
-------------

AxlHashTable
~~~~~~~~~~~~

.. doxygenfile:: axl-hash-table.h

AxlArray
~~~~~~~~

.. doxygenfile:: axl-array.h

AxlList
~~~~~~~

.. doxygenfile:: axl-list.h

AxlSList
~~~~~~~~

.. doxygenfile:: axl-slist.h

AxlQueue
~~~~~~~~

.. doxygenfile:: axl-queue.h

AxlRadixTree
~~~~~~~~~~~~

.. doxygenfile:: axl-radix-tree.h

AxlNTree
~~~~~~~~

.. doxygenfile:: axl-ntree.h

AxlTree
~~~~~~~

.. doxygenfile:: axl-tree.h

AxlRingBuf
~~~~~~~~~~

.. doxygenfile:: axl-ring-buf.h

AxlDigest
~~~~~~~~~

Message digest checksums (MD5, SHA-1, SHA-256). Standalone
implementations — available even without ``AXL_TLS=1``.

.. doxygenfile:: axl-digest.h

AxlHmac
~~~~~~~

Keyed-hash message authentication (HMAC, RFC 2104) over the digest
engine — mirrors GLib's ``GHmac``. For API tokens, signed cookies,
webhook signatures. No ``AXL_TLS=1`` required. Prefer HMAC-SHA256 for
new designs.

.. doxygenfile:: axl-hmac.h

AxlBytes
~~~~~~~~

Immutable, reference-counted byte buffer (GLib's ``GBytes``). A
read-only ``(data, size)`` blob shared across owners without copying;
``axl_bytes_new_from_bytes`` carves a zero-copy sub-range that keeps
its parent alive. The shared currency for data flowing between
subsystems (HTTP bodies, file contents, shared-memory segments).

.. doxygenfile:: axl-bytes.h
