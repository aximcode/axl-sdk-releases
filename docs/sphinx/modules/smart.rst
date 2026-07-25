AxlSmart — normalized cross-transport storage health
=======================================================

The synthesis layer over :doc:`nvme`, :doc:`ata`, and :doc:`scsi` —
the ``smartctl``-equivalent for UEFI. The device walk lives in
``<axl/axl-storage.h>``: ``axl_storage_next()`` walks an opaque
``AxlStorageDev`` across all three transports back-to-back. Layered on
top, ``<axl/axl-smart.h>``'s ``axl_smart_health()`` reports each
device's identity and health in one uniform ``AxlSmartHealth`` record,
so a consumer asks "is this disk OK" without knowing the wire protocol.
``axl-smart.h`` includes ``axl-storage.h``, so the walk-then-read
pattern needs only the one include.

Per-field normalization is honest: a field a transport cannot supply
carries a documented sentinel (never a guessed value) rather than a
presence mask. See ``docs/AXL-Storage-Design.md`` for the family
architecture; :doc:`block` remains the logical-block/media-geometry
view this complements.

API Reference
-------------

.. doxygenfile:: axl-storage.h

.. doxygenfile:: axl-smart.h
