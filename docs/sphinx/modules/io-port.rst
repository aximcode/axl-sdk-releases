AxlIoPort — x86 I/O port access
================================

Public wrappers around the x86 ``in`` / ``out`` instruction family
for legacy hardware that hasn't moved to MMIO (CMOS, SuperIO, IPMI
KCS interfaces, port-based ACPI PM blocks).

Build-gated to x86 — declarations expand to nothing on AArch64 so
wrong-arch usage fails at link time rather than silently no-op'ing.

API Reference
-------------

.. doxygenfile:: axl-io-port.h
