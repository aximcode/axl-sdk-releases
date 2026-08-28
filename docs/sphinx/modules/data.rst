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

Message digest checksums (MD5, SHA-1, SHA-256, SHA-512). MD5, SHA-1
and SHA-256 are AXL's own; SHA-512 forwards to mbedTLS.

.. doxygenfile:: axl-digest.h

AxlHmac
~~~~~~~

Keyed-hash message authentication (HMAC, RFC 2104) over the digest
engine — mirrors GLib's ``GHmac``. For API tokens, signed cookies,
webhook signatures. MD5, SHA-1 and SHA-256 are AXL's own; SHA-512
forwards to mbedTLS. Prefer HMAC-SHA256 for new designs.

.. doxygenfile:: axl-hmac.h

.. note::

   For the full cryptography map — hashing, HMAC, randomness, image
   verification, TLS, and public-key signature verification
   (``<axl/axl-crypto.h>``) — see :doc:`crypto`.

AxlBytes
~~~~~~~~

Immutable, reference-counted byte buffer (GLib's ``GBytes``). A
read-only ``(data, size)`` blob shared across owners without copying;
``axl_bytes_new_from_bytes`` carves a zero-copy sub-range that keeps
its parent alive. The shared currency for data flowing between
subsystems (HTTP bodies, file contents, shared-memory segments).

.. doxygenfile:: axl-bytes.h

AxlCompress
~~~~~~~~~~~

DEFLATE-family compression (RFC 1951) with gzip (RFC 1952) and zlib
(RFC 1950) framing, backed by a vendored ``sdefl``/``sinfl`` codec.
One-shot ``axl_compress`` / ``axl_decompress`` plus stream filters
(``axl_compress_writer`` / ``axl_compress_reader`` and the
``axl_gzip_*`` wrappers) over ``AxlStream`` — so ``tar.gz``, HTTP gzip,
and file compression compose for free. Integrity (CRC-32 / Adler-32)
is verified on decode via :doc:`AxlDigest <data>`.

.. doxygenfile:: axl-compress.h
