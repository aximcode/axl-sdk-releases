AxlAta — ATA/SATA device identity + health
============================================

ATA/SATA device identity and SMART health over the firmware's
``EFI_ATA_PASS_THRU_PROTOCOL``. Part of the storage-access family
alongside :doc:`nvme`, :doc:`scsi`, and the cross-transport
:doc:`smart` synthesis layer — see ``docs/AXL-Storage-Design.md``.

A device is addressed by (port, port-multiplier port) under a
controller; ``axl_ata_next()`` walks an opaque ``AxlAtaDev`` handle so
a consumer never juggles that triple. Scope is read-and-health:
IDENTIFY DEVICE, SMART (READ DATA + READ THRESHOLDS), and SMART
EXECUTE OFF-LINE IMMEDIATE. A raw ``axl_ata_passthru()`` entry point
covers arbitrary task-file commands; the typed surface ships no
data-destroying command.

API Reference
-------------

.. doxygenfile:: axl-ata.h
