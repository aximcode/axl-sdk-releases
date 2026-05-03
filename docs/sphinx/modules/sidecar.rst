AxlSidecar — JSON5 sidecar loader
==================================

Common scaffolding for JSON5 *sidecar* files — curated companion
data shipped next to a ``.efi`` binary and loaded at startup. axl-
sdk uses this for ``pci-ids.json5``, ``pci-class.json5``,
``usb-ids.json5``, and ``jedec.json5``; downstream consumers can
build their own sidecars on top of the same primitives.

Three module-side concerns recur for every sidecar consumer, all
provided by AxlSidecar:

1. **Open the file** with a useful error-code split so the consumer
   can log "deployment problem" (file missing) and "authoring
   problem" (file present but malformed) differently.
2. **Validate the schema field** — every sidecar in axl-sdk
   requires an explicit ``schema: N`` declaration so an old binary
   refuses to misparse a future-version file.
3. **Walk the typed entries** into the consumer's own table.

AxlSidecar owns concerns 1 and 2; concern 3 (schema-specific typed
walk) stays with each consumer because the table shapes differ.

Internal axl-sdk modules also get a typed singleton wrapper and a
hash-table foreach trampoline through ``axl-sidecar-internal.h``
(not part of the public API).

Quick start
-----------

.. code-block:: c

   #include <axl/axl-sidecar.h>

   AxlJsonReader  r;
   void          *raw;
   AxlSidecarStatus rc = axl_sidecar_open_file("foo.json5", &r, &raw);
   if (rc == AXL_SIDECAR_FILE_MISSING) {
       /* deployment hint */
   } else if (rc == AXL_SIDECAR_PARSE_ERROR) {
       /* authoring hint */
   } else {
       const uint64_t accepted[] = { 1u };
       uint64_t schema = 0;
       if (axl_sidecar_check_schema(&r, "foo", accepted, 1, &schema)
           == AXL_SIDECAR_OK)
       {
           /* schema 1 confirmed; walk fields and fill table */
       }
       axl_json_free(&r);
       axl_free(raw);
   }

API Reference
-------------

.. doxygenfile:: axl-sidecar.h
