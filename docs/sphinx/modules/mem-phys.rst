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
