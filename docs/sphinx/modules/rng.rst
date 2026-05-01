AxlRng — cryptographic random bytes
====================================

Thin wrapper over ``EFI_RNG_PROTOCOL`` (UEFI 2.11 §37.5). The
protocol is published by most modern firmware on platforms with an
entropy source (RDRAND on x86, an SBSA TRNG on AArch64). Returns
-1 if the protocol isn't installed; consumers that need a
deterministic fallback layer their own.

API Reference
-------------

.. doxygenfile:: axl-rng.h
