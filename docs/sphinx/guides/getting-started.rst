Getting Started
===============

Build and run your first UEFI application with the AXL SDK. No EDK2
source tree needed, and nothing from your distribution's toolchain.

Install the SDK
---------------

Three commands, the same on every distribution::

    curl -fsSLO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/install.sh
    sh install.sh --help
    sh install.sh

The installer resolves the latest version, verifies the download
against ``SHA256SUMS``, unpacks into a versioned directory under
``~/.local/share`` and links the commands into ``~/.local/bin``.
One archive carries both x64 and aa64 UEFI target libraries.
Afterwards ``axl update``, ``axl use <version>``, ``axl list`` and
``axl uninstall`` maintain the install from inside it.

Then add the cross toolchain for the firmware you are **building
for**::

    axl-install-toolchain x64          # or: aa64, or: all

That is a choice about your TARGET, not about your host, so the
installer does not guess it -- the wrong one is a 239 MB (x64) or
500 MB (aa64) download. ``sh install.sh --toolchain x64`` does both
in one command once you know which you want.

**AXL builds no target code with your host's gcc** -- both arches
use pinned bare-metal crosses (AXL's ``x86_64-elf`` and ARM's
``aarch64-none-elf``) under ``/opt``, which is why there is no
``apt install gcc-aarch64-linux-gnu`` step here and why a
glibc-targeted cross cannot build against these headers.

The only host commands the SDK needs are ``curl``, ``tar``,
``sha256sum``, ``awk``, ``tr`` and ``xz``.

QEMU and OVMF are needed to *run* a ``.efi``, not to build one.
The installer probes for them and prints your distribution's
install command if they are absent; it never installs them
silently.

See the `README <https://github.com/aximcode/axl-sdk-releases#install-the-sdk>`_
for pinning a version, installing from a mirror with
``--base-url``, and the host-tools-only install.

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

``run-qemu`` creates a FAT32 disk image, copies your EFI binary,
and boots QEMU with OVMF::

    axl run-qemu hello.efi

You should see ``Hello from UEFI!`` on the QEMU console. Every
host-side tool is reached as ``axl <verb>``; ``axl --help`` lists
them. From a source checkout the same script is
``./scripts/run-qemu.sh``.

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

The C++ driver bakes in the freestanding-UEFI flag set:
``-std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics``
plus per-arch additions (``-ffixed-x18`` on AArch64,
``-mno-red-zone`` on X64).  These are *defaults*, not hard limits.

**The standard containers work, with no flag.**  ``std::vector``,
``std::string``, ``std::map`` and ``std::unordered_map`` link on
both arches, because every C++ link carries the toolchain's own
``libstdc++`` / ``libsupc++``::

    axl-c++ app.cpp -o app.efi        # <vector>, <string>, <map>, ...

**Exceptions and RTTI are a per-TU opt-in**, not forbidden::

    axl-c++ -fexceptions app.cpp -o app.efi
    axl-c++ -frtti       app.cpp -o app.efi

They are off by default because they are not free: measured on
four real x64 tools, enabling exceptions cost **+153,886 to
+178,118 bytes (+28-36%)** of ``.efi``.  ``<iostream>``,
``<sstream>`` and ``<fstream>`` work too and cost roughly 734 KB of
``.text`` — which is why ``axl::cout`` (about 700 bytes over
``axl_printf``) remains the default way to print.

Because ``-fno-exceptions`` is the default, **every AXL C++ header
must compile in both modes**, which is why errors are reported as
``axl::result`` and nothing in the library throws.

.. note::

   ``--hosted`` no longer exists.  The thing it switched on is now
   unconditional, so ``axl-c++`` **refuses** the flag rather than
   accepting it as a no-op — delete it from your build; the output
   is unchanged.

``axl::string`` is kept alongside ``std::string`` deliberately:
under ``-fno-exceptions`` ``std::string`` *halts* on allocation
failure, while ``axl::string`` sets ``bad()``, which is what lets
``axl::cin`` report ``AXL_NO_RESOURCES``.  It is also smaller —
564 bytes against 1045.

**Both arches need a bare-metal cross toolchain** — AArch64
``aarch64-none-elf-g++`` and X64 ``x86_64-elf-g++``.  AXL does not
build target code with the host's gcc.  One installer covers both::

    axl-install-toolchain all              # from an installed SDK
    ./scripts/install-toolchain.sh all     # from a checkout

Takes ``aa64``, ``x64`` or ``all``.  Fetches + sha256-verifies +
extracts each pinned tarball under ``/opt``; the versions and exact
paths are in ``scripts/axl-toolchains.conf``, which is the single
source of truth for both.  Idempotent — re-running on an
already-installed system reports it and exits 0 without touching
anything.

``docs/AXL-Cxx-Design.md`` in the source tree covers the rest of the
C++ layer: ``axl::result``, ``axl::arena_allocator``,
``axl::unique_handle`` and the seam headers over the C containers.

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
