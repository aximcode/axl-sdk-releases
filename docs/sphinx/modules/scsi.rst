AxlScsi — SCSI/SAS device identity + health
=============================================

SCSI/SAS device identity, geometry, and health over the firmware's
``EFI_EXT_SCSI_PASS_THRU_PROTOCOL``. Part of the storage-access family
alongside :doc:`ata`, :doc:`nvme`, and the cross-transport
:doc:`smart` synthesis layer — see ``docs/AXL-Storage-Design.md``.

A device is addressed by (Target, LUN) under a controller;
``axl_scsi_next()`` walks an opaque ``AxlScsiDev`` handle so a
consumer never juggles that triple. Scope is read-and-health: INQUIRY
(standard data + Unit Serial Number VPD page), READ CAPACITY (16), and
LOG SENSE of the Informational Exceptions and Temperature pages. A raw
``axl_scsi_passthru()`` entry point carries any CDB for cases outside
the typed surface (self-test, VPD page 0x83).

API Reference
-------------

.. doxygenfile:: axl-scsi.h
