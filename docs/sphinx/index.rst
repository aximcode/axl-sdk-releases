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

   modules/cxx
   modules/arena-allocator
   modules/cxx-string
   modules/cxx-ostream
   modules/cxx-istream
   modules/mem
   modules/debug
   modules/math
   modules/format
   modules/str
   modules/string
   modules/stream
   modules/log
   modules/data
   modules/json
   modules/xml
   modules/cache
   modules/page-cache
   modules/text-buffer
   modules/rb-tree
   modules/piece-tree
   modules/find
   modules/radix-tree
   modules/ring-buf
   modules/config
   modules/subcommand
   modules/args
   modules/path
   modules/clipboard
   modules/shm
   modules/sort
   modules/tar
   modules/loop
   modules/task
   modules/event
   modules/runtime
   modules/service
   modules/net
   modules/tls
   modules/crypto
   modules/scram
   modules/jose
   modules/9p
   modules/sys
   modules/cpu
   modules/port
   modules/boot
   modules/image
   modules/console-mirror
   modules/vterm
   modules/mem-phys
   modules/watchdog
   modules/crashrecord
   modules/attempt
   modules/rng
   modules/rand
   modules/gfx
   modules/edid
   modules/truetype
   modules/display-list
   modules/pixmap
   modules/input
   modules/smbios
   modules/acpi
   modules/pci
   modules/usb
   modules/block
   modules/nvme
   modules/ata
   modules/scsi
   modules/smart
   modules/serial
   modules/fv
   modules/fw
   modules/tpm
   modules/hii
   modules/ramdisk
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
   guides/shared-driver
   guides/driver-authoring
   guides/axlmm-design
   guides/display-design
   guides/porting
   guides/releasing
   guides/roadmap

.. toctree::
   :maxdepth: 1
   :caption: Reference

   types
   glossary
