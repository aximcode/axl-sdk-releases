AXL SDK
=======

.. list-table::
   :widths: 20 80

   * - **Version**
     - |release|
   * - **License**
     - Apache-2.0
   * - **C header**
     - ``#include <axl.h>``
   * - **Website**
     - `axl.aximcode.com <https://axl.aximcode.com>`_
   * - **Downloads**
     - `github.com/aximcode/axl-sdk-releases <https://github.com/aximcode/axl-sdk-releases/releases/latest>`_
   * - **Build**
     - ``axl-cc hello.c -o hello.efi``

.. include:: ../../README.md
   :parser: myst_parser.sphinx_

.. toctree::
   :maxdepth: 2
   :caption: Modules

   modules/mem
   modules/format
   modules/str
   modules/string
   modules/stream
   modules/log
   modules/data
   modules/json
   modules/cache
   modules/radix-tree
   modules/ring-buf
   modules/config
   modules/subcommand
   modules/path
   modules/loop
   modules/task
   modules/event
   modules/runtime
   modules/net
   modules/tls
   modules/sys
   modules/port
   modules/boot
   modules/image
   modules/mem-phys
   modules/watchdog
   modules/rng
   modules/gfx
   modules/smbios
   modules/acpi
   modules/pci
   modules/usb
   modules/smbus
   modules/ipmi
   modules/spd
   modules/sidecar

.. toctree::
   :maxdepth: 1
   :caption: Guides

   guides/getting-started
   guides/design
   guides/concurrency
   guides/lifecycle
   guides/coding-style
   guides/sdk
   guides/porting
   guides/releasing
   guides/roadmap

.. toctree::
   :maxdepth: 1
   :caption: Reference

   types
   glossary
