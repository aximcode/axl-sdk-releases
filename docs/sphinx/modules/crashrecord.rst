AxlCrashRecord — UEFI crash record format
============================================

The binary crash-record format the CrashHandler driver writes to
``CrashDump<N>`` NVRAM variables on an unhandled exception, and that
the host-side ``rsod-decode`` reader parses back into a symbolized
report. Standard C types only, so the same header is safe to include
from on-device driver code and host-side tooling alike.

The layout covers a fixed header (magic, version, architecture,
timestamp), the full X64 or AArch64 register file, the loaded-image
table (faulting image plus every other loaded image, for symbol
resolution), and a captured stack-frame return-address trail — plus
the companion ``.sym`` file format used for offline function-name
resolution.

API Reference
-------------

.. doxygenfile:: axl-crashrecord.h
