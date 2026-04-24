UEFI Glossary
=============

AXL abstracts most UEFI details behind its public API, but some
concepts surface in documentation and error messages. This glossary
defines terms you may encounter.

.. glossary::
   :sorted:

   BSP
      **Bootstrap Processor** — the CPU core that executes firmware
      initialization and runs the main application. All UEFI Boot
      Services calls must happen on the BSP. AXL functions that
      interact with firmware (I/O, networking, protocol calls) run
      on the BSP.

   AP
      **Application Processor** — any CPU core other than the BSP.
      APs can be dispatched via the MP Services protocol to run
      compute-heavy tasks (CRC, decompression, hashing) in parallel.
      APs **cannot** call Boot Services, use ``axl_printf``, or
      access UEFI protocols. Use :c:func:`axl_task_pool_submit` to
      offload work to APs.

   Boot Services
      Firmware services available before the OS takes control. Accessed
      via ``gBS`` (the Boot Services table pointer). Includes memory
      allocation, protocol management, event/timer services, and
      driver loading. AXL wraps most Boot Services behind its API --
      direct access is needed only for custom protocol calls.

   Runtime Services
      Firmware services that remain available after the OS boots.
      Accessed via ``gRT``. Includes NVRAM variable access
      (``GetVariable``/``SetVariable``), system time, and reset.
      AXL wraps these via ``axl_nvstore_*`` and ``axl_reset``.

   System Table
      The root UEFI data structure (``EFI_SYSTEM_TABLE``), passed to
      every UEFI entry point. Contains pointers to Boot Services,
      Runtime Services, console I/O, and firmware vendor info.
      Accessed via ``gST`` in AXL.

   Protocol
      A UEFI interface identified by a GUID. Protocols are installed
      on handles and discovered via ``LocateProtocol`` or
      ``LocateHandleBuffer``. Examples: TCP4, UDP4, GOP (graphics),
      SimpleNetwork, Shell. AXL's ``axl_service_find`` wraps protocol
      lookup with string names instead of GUIDs.

   Service Binding
      A protocol pattern for creating child instances. Used by
      networking protocols (TCP4, UDP4, DNS4) where each connection
      needs its own protocol instance. ``CreateChild`` allocates a
      new instance; ``DestroyChild`` frees it.

   GOP
      **Graphics Output Protocol** — the UEFI framebuffer interface.
      Provides resolution queries, rectangle fill, pixel buffer blit,
      and screen capture. AXL wraps it via ``axl_gfx_*`` functions.
      Not available on headless or serial-only systems.

   SNP
      **Simple Network Protocol** — the lowest-level NIC interface
      in UEFI. Provides raw Ethernet frame send/receive. Higher-level
      protocols (IP4, TCP4, UDP4, DHCP4) are built on top of SNP.
      ``axl_net_auto_init`` loads NIC drivers and brings up the SNP →
      IP4 → TCP4/UDP4 stack.

   ConOut
      **Console Output** — the UEFI text output protocol
      (``EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL``). ``axl_printf`` converts
      UTF-8 to UCS-2 and writes to ConOut. Available on systems with
      a display or serial console.

   EFI_STATUS
      The standard UEFI return type — a machine-word integer where
      0 = ``EFI_SUCCESS``. AXL functions return ``int`` (0 = success,
      -1 = failure) instead; ``EFI_STATUS`` values appear only in
      direct protocol calls.

   EFI_HANDLE
      An opaque pointer identifying a UEFI object (device, driver,
      application). Handles have protocols installed on them.
      ``gImageHandle`` is the handle of the running application or
      driver.

   Device Path
      A UEFI data structure describing the physical or logical path
      to a device (e.g., ``PciRoot(0x0)/Pci(0x1F,0x2)/Sata(0x0)``).
      Used internally by the firmware for boot ordering and device
      identification.

   DXE Driver
      **Driver Execution Environment** driver — a UEFI binary that
      provides services to other binaries. Built with
      ``axl-cc --type driver``. Entry point is ``DriverEntry``
      instead of ``main``. Call ``axl_driver_init`` to set up the
      AXL runtime.

   OVMF
      **Open Virtual Machine Firmware** — an open-source UEFI
      firmware implementation for virtual machines (QEMU). Used for
      testing AXL applications without real hardware.
