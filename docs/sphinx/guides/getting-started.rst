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
    export PATH="$PWD/stage/bin:$PATH"

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

C++ Quickstart
--------------

axl-sdk ships first-class C++ support: pass ``.cpp`` to
``axl-cc`` and it dispatches to ``g++`` automatically, or use the
``axl-c++`` alias (which forces C++ mode for every source
regardless of extension).

Create ``hello.cpp``:

.. code-block:: cpp

    #include <axl.h>

    int main(int, char **) {
        AXL_AUTOPTR(AxlLoop) loop = axl_loop_new();   // RAII; freed at scope exit
        axl_printf("Hello from C++!\n");
        return 0;
    }

Build::

    axl-c++ hello.cpp -o hello.efi
    # or equivalently:
    axl-cc hello.cpp -o hello.efi

The C++ toolchain bakes in the freestanding-UEFI flag set:
``-std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics``
plus per-arch additions (``-ffixed-x18`` on AArch64,
``-mno-red-zone`` on X64).  These are hard defaults — consumers
can't opt in to exceptions or RTTI because the freestanding link
won't satisfy libsupc++ symbols.

**AArch64 needs the ARM bare-metal toolchain**
(``aarch64-none-elf-g++``).  axl-sdk ships an installer::

    ./scripts/install-arm-toolchain.sh

Fetches + sha256-verifies + extracts the pinned tarball to
``/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/``.
Idempotent — re-running on an already-installed system exits
cleanly.  ``install.sh`` auto-detects this path and builds C++
support when the toolchain is present.

**Usable libstdc++ subset** (all header-only — no link dependency):

- Always-freestanding: ``<type_traits>``, ``<utility>``,
  ``<initializer_list>``, ``<new>``, ``<bit>``, ``<concepts>``,
  ``<compare>``, ``<source_location>``, ``<cstddef>``,
  ``<cstdint>``, ``<limits>``
- Practically usable: ``<array>``, ``<span>``, ``<string_view>``,
  ``<tuple>``, ``<optional>``, ``<variant>``, ``<expected>``
  (C++23), header-only subsets of ``<algorithm>`` /
  ``<numeric>`` / ``<functional>``

**Forbidden** (require libsupc++/libstdc++ symbols we don't have):
exceptions (``throw`` / ``catch``), RTTI (``typeid`` /
``dynamic_cast``), ``<string>`` / ``<vector>`` / ``<unordered_map>``
/ ``<stdexcept>``, ``thread_local``, ``<format>``.  See
:doc:`axlmm-design` for the full constraint list + rationale.

Mixed C/C++ projects work naturally — pass any combination of
``.c`` and ``.cpp`` sources to ``axl-cc`` and the right compiler
is invoked per file::

    axl-cc app.cpp legacy.c helpers.cpp -o app.efi

For staged builds where you ship your own static library, use
``axl-cc -c`` for compile-only mode + ``ar rcs`` to archive +
pass the ``.a`` to a final ``axl-cc`` link::

    axl-cc -c lib1.cpp -o lib1.o
    axl-cc -c lib2.cpp -o lib2.o
    ar rcs libfoo.a lib1.o lib2.o
    axl-cc app.cpp libfoo.a -o app.efi

The `AGT widget toolkit <https://github.com/aximcode/agt>`_ is
the first C++ consumer using this pattern (ships ``libagt.a``
that consumer apps link against via ``axl-c++``).

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
