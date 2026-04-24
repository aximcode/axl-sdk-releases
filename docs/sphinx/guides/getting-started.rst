Getting Started
===============

Build and run your first UEFI application with the AXL SDK. No EDK2
source tree needed — just a C compiler and a linker.

Prerequisites
-------------

- **GCC** (native for x64, cross-compiler ``aarch64-linux-gnu-gcc`` for AARCH64)
- **binutils** (``ld``, ``objcopy``, ``ar``)
- **QEMU** + **OVMF** for testing (optional but recommended)

On Fedora/RHEL::

    dnf install gcc binutils qemu-system-x86 edk2-ovmf
    # For AARCH64 cross-builds:
    dnf install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu

On Ubuntu/Debian::

    apt install gcc binutils qemu-system-x86 ovmf
    # For AARCH64 cross-builds:
    apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu

Install the SDK
---------------

The quickest path is a prebuilt package from the
`latest release <https://github.com/aximcode/axl-sdk-releases/releases/latest>`_.
On Debian/Ubuntu::

    curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.deb
    sudo apt install ./axl-sdk.deb

On Fedora/RHEL::

    curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.rpm
    sudo dnf install ./axl-sdk.rpm

Each package bundles both x64 and aa64 UEFI target libraries and
installs ``axl-cc`` to ``/usr/bin/axl-cc``.

Build from source
^^^^^^^^^^^^^^^^^

Clone the release repo and run the installer to produce the
same FHS layout under a prefix of your choice::

    git clone https://github.com/aximcode/axl-sdk-releases.git
    cd axl-sdk-releases
    git checkout v0.1.3   # or any tagged release
    ./scripts/install.sh --arch x64 --prefix "$PWD/out"
    export PATH="$PWD/out/bin:$PATH"

For a tarball instead of a clone, use the **Source code
(tar.gz)** link on each
`release page <https://github.com/aximcode/axl-sdk-releases/releases>`_.

Write a Hello World
-------------------

Create ``hello.c``:

.. code-block:: c

    #include <axl.h>

    int main(int argc, char **argv) {
        axl_printf("Hello from UEFI!\n");
        return 0;
    }

Standard C entry point. No UEFI types, no wide strings, no
``EFI_STATUS``. Just ``#include <axl.h>`` and write C.

Build
-----

::

    axl-cc hello.c -o hello.efi

That's it — one command, 11KB binary, zero external dependencies.

Run in QEMU
------------

The ``run-qemu.sh`` script creates a FAT32 disk image, copies your
EFI binary, and boots QEMU with OVMF::

    ./scripts/run-qemu.sh hello.efi

You should see ``Hello from UEFI!`` on the QEMU console.

Next Steps
----------

- Browse the :doc:`../modules/mem` and :doc:`../modules/str` modules
  for memory and string utilities
- See :doc:`../modules/net` for HTTP client/server and TCP sockets
- Read the :doc:`design` document for architecture overview
- Explore ``sdk/examples/`` for more examples (JSON, collections,
  HTTP fetch, drivers)

All API documentation is generated from header comments. Include
``<axl.h>`` for everything, or individual headers like
``<axl/axl-json.h>`` for specific modules.
