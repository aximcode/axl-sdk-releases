AxlCpu — CPU exceptions, feature detection + topology
=====================================================

Three CPU-level facilities under one ``axl_cpu_*`` namespace:

**Typed exception handling** — a backend-neutral wrapper over
``EFI_CPU_ARCH_PROTOCOL.RegisterInterruptHandler``. Register a callback
for a typed ``AxlCpuExceptionKind`` and receive a layout-stable
``AxlCpuException`` register snapshot, without spelling ``EFI_*``.

**Instruction-set feature detection + SIMD dispatch** — ``CPUID``-based
detection (cached) of the SSE family, FMA, and AVX/AVX2 on x86 (NEON on
AArch64), plus ``axl_cpu_simd_tier()`` returning a single ordered value
for kernel dispatch.

Note the firmware/UEFI specifics around SIMD state: the firmware
already enables SSE state (its calling convention passes floats in XMM),
so SSE/SSE3/SSE4 need no enabling — detection alone gates them. AVX adds
YMM register state the firmware does **not** enable, so AVX/AVX2 trap
with ``#UD`` until ``axl_cpu_enable_avx()`` performs the CPL0
``CR4.OSXSAVE`` + ``XSETBV`` sequence (a UEFI app runs at CPL0, so it
may). ``CR4``/``XCR0`` are per-logical-processor — enable on each AP
that runs AVX kernels, not just the BSP.

**Processor topology** — ``axl_cpu_topology()`` enumerates the
machine's logical processors over ``EFI_MP_SERVICES_PROTOCOL``,
reporting per-CPU physical location (package / core / thread) and
status (bootstrap, enabled, self-test healthy) into a caller-sized,
index-keyed ``AxlCpuProcessor`` array, with decoupled total / enabled
counts. On single-processor firmware that does not publish MP services
it reports the uniprocessor floor (one processor) rather than failing.
This is the headless inventory mechanism; rendering belongs to the
caller.

API Reference
-------------

.. doxygenfile:: axl-cpu.h
