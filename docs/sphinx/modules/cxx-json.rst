axl::json — JSON for C++
========================

.. note::

   The C++ layer's JSON face — phase C6 of ``docs/AXL-Cxx-Design.md`` §9,
   deliberately built last so it inherited the naming, error and ownership
   conventions the earlier phases set rather than inventing them. See also
   :doc:`cxx` and :doc:`json` for the C API underneath.

Header: ``<axl/axl-json.hpp>``

.. code-block:: cpp

   #include <axl/axl-json.hpp>

   auto doc = axl::json_document::parse(bytes);
   if (!doc) { return doc.error(); }

   // ONE check for a three-level descent
   if (auto port = (*doc)["net"]["listen"]["port"].as<int64_t>()) {
       bind(*port);
   }

   for (axl::json_value v : (*doc)["items"].array()) { ... }
   for (auto &&[key, value] : (*doc)["headers"].object()) { ... }

   axl::json_writer w{out};
   {
       auto o = w.object();
       w.add("name", "axl");                 // picks the emitter from the type
       w.add("scale", 1.5);
       { auto a = w.array("items"); w.add(1); w.add(2); }
       w.splice("cached", (*doc)["items"]);  // a parsed subtree, verbatim
   }
   w.finish();

Four faces
----------

``docs/AXL-JSON-Design.md`` describes the C API as two engines behind four
faces — streaming and whole-document, in each direction. Each gets a C++ face,
and the bridge between them comes free:

===========  =========================  ================================
direction    streaming                  whole-document
===========  =========================  ================================
**in**       ``axl::json_scanner``      ``axl::json_document``
**out**      ``axl::json_writer``       ``json_writer::splice()``
===========  =========================  ================================

``splice()`` is ``axl_json_write_token()`` and needed no C change: a sub-reader
is *rebased*, so token 0 of ``doc["items"]`` is the array itself.

Navigation chains
-----------------

``axl::json_value`` carries the reason it is empty, so ``operator[]`` on an
already-errored value returns that error untouched. ``doc["a"]["b"]["c"]``
performs no lookups after the first failure and the first ``as_X()`` reports
what went wrong — ``AXL_NOT_FOUND`` for an absent key, ``AXL_INVALID`` for
indexing something that is not an object. **The first error survives**; a later
step never overwrites it.

This is simdjson's model, and it is the only one of the four that fits a
library where errors are values. ``nlohmann::json::operator[]``
default-constructs a missing key — mutating on read; RapidJSON asserts;
Boost.JSON throws. Returning ``result<json_value>`` at every step is honest but
forces three unwraps for a three-level lookup.

A JSON ``null`` **exists**. Conflating it with an absent key is a distinction
the C reader deliberately keeps, and the C++ layer keeps it too.

Errors are values, in both compile modes
----------------------------------------

Nothing here throws, and that is a stronger requirement than it first appears.
Exceptions genuinely work under UEFI — ``axl-c++ -fexceptions`` gives real
``try``/``catch``, pinned by ``test-cxx-exceptions-qemu.sh`` — but they are a
per-translation-unit opt-in and ``-fno-exceptions`` is the default. A header
that threw would be unusable in the default mode, so ``axl::result`` is what
works in both. It also matches the C library, where errors are queried rather
than thrown.

Lifetimes
---------

The C design's rule is *single owner, no reference counting*: a document owns
its tokens and every value handle is a non-owning view valid for its lifetime.
That carries over exactly — ``axl::json_document`` is move-only and frees on
destruction; every ``json_value``, range and iterator borrows it.

The **bytes** are a second question, and the two factories differ only in it:

``parse(std::string_view)``
   Zero-copy, borrowing the caller's buffer, matching ``axl_json_parse()``. The
   buffer must outlive the document and every value taken from it.

``parse_owning(std::string)``
   Takes the bytes by value and keeps them, for the common C++ case where they
   were a temporary. They are held behind a ``unique_ptr`` so that moving the
   document never relocates them — a short document lives in ``std::string``'s
   small-buffer, *inside* the object, and moving it would leave the reader
   pointing at the old address.

They are named rather than overloaded because getting it wrong is silent.

What C6 needed from the C side
------------------------------

Four additions, all of which serve C callers too:

``axl_json_get_string_len`` / ``axl_json_value_string_len``
   How many bytes the decoded value needs. ``axl_json_get_string()`` truncates
   silently and returns ``true``, so nothing could size a ``std::string``. The
   answer must be *decoded* — ``\uXXXX`` shrinks six bytes to one, a surrogate
   pair twelve to four, and JSON5's ``\0`` grows two to three — and it must go
   through the reader, because the standalone ``axl_json_decode_string()`` does
   not know the reader's UTF-8 mode and would produce a different string.

``axl_json_object_peek_key_len``
   The next pair's decoded key length, without consuming it. Object iteration
   truncates a key that does not fit and reports it only *afterwards*, with the
   pair already gone — so sizing first is the only way a ``std::string`` key
   can be whole.

``axl_json_double`` / ``axl_json_kv_double``
   The writer had no ``double`` atom while the reader has had ``get_double``
   since P14. Formatted ``%.17g``, which on AXL's engine is the *shortest*
   round-trippable spelling rather than 17 digits: ``axl_dtoa`` (Grisu2)
   produces at most 17 shortest digits, so the rounding is a no-op and ``%g``
   trims. ``0.1`` emits as ``0.1``. Non-finite values follow the dialect —
   refused under a strict writer, emitted as ``NaN`` / ``Infinity`` under
   ``AXL_JSON_ALLOW_NAN_INF``, which is the same bit the reader honours.

.. doxygenfile:: axl-json.hpp
   :project: axl
