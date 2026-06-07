AxlMemPhys — physical-memory access
====================================

Held ``_map``/``_unmap`` mappings plus one-shot
``_read{8,16,32,64}`` / ``_write{8,16,32,64}`` helpers and a
byte-pattern ``_search``. UEFI is identity-mapped so map is
effectively a no-op; the abstraction is preserved for portability
to backends where physical access requires explicit mapping
(Linux ``mmap("/dev/mem")``, AXL kernel POC explicit page-table
mapping).

API Reference
-------------

.. doxygenfile:: axl-mem-phys.h

AxlMemRegion — region map + fault-safe access
=============================================

``axl-mem-region.h`` layers a **safe, navigable** view over the raw
``axl-mem-phys`` primitives, for a tool that lets a user type an
arbitrary physical address. It classifies the physical address space
into typed regions (RAM / reserved / ACPI / MMIO / unmapped) by
overlaying the UEFI memory map with the PI **GCD** memory-space map
(so MMIO the EFI map omits — PCI BARs — is classified), gates every
access behind ``axl_mem_phys_is_accessible`` (best-effort, since there
is no recoverable pre-boot fault handler), and offers width- and
alignment-aware ``_read_range`` / ``_write_range`` so a misaligned or
unmapped address returns an error instead of faulting. An
``AxlMemAccessPolicy`` permits every mapped type by default and can be
tightened (RAM-only, read-only) as an opt-in guard.

.. doxygenfile:: axl-mem-region.h
