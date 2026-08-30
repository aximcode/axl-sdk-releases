# AXL SDK Design

Part of the [AximCode](https://github.com/aximcode) project.
AXL = AximCode Library. Pronounced "axle."

## Where this doc sits — three docs, one subject, different questions

This doc covers **what the SDK contains**. Two companions cover the other
halves, and each fact below is owned by exactly one of them:

| doc | answers | owns |
|---|---|---|
| **this one** | what is in the SDK | the toolchain requirement, C++ support, the layout of what ships |
| [AXL-Distribution-Design.md](AXL-Distribution-Design.md) | how it REACHES and is USED by a consumer | packaging, `find_package` discovery, version pinning, the `out/` vs `stage/` split (§4), **install layout and the `axl` dispatcher (§12–§13)**, P1–P7 |
| [AXL-Build-System-Design.md](AXL-Build-System-Design.md) | how WE build it | the CMake port, the port surface measurements (§8.2a), `axl-cc`'s exclusion from the port, `axl-config.cmake`'s extraction (§8.4) |

**One owner per shared fact, everyone else links.** That rule exists because
of a specific failure: the two companions independently asserted that
Distribution's P2 and the port's slice 3 swept the same ~149 callers. A commit
had invalidated that months earlier, and neither noticed — each was internally
consistent, so nothing forced a re-read. Correcting it took two commits. A
claim asserted in two places goes stale in two places.

## Audience

Linux systems C developers — the glibc / GLib / systemd / libcurl
audience — who need to ship a UEFI binary without first learning
EDK2's PascalCase, `EFI_*` type universe, `.inf`/`.dsc` build files,
and gnu-efi's threadbare runtime. The SDK lets them keep the C
ergonomics they already use (snake_case, standard C types, an event
loop modeled on `GMainLoop`, hash tables modeled on `GHashTable`)
and produces a UEFI `.efi` binary at the end.

**No source-tree dependency on EDK2.** The library's internal `EFI_*`
type definitions are auto-generated from the published UEFI 2.x and
PI 1.x specifications via `scripts/generate-uefi-headers.py` +
`scripts/uefi-manifest.json5`. The SDK ships those generated headers
under `include/uefi/generated/` for any consumer that does need to
reach into raw UEFI types (driver authors, interop code), but
applications never see them — the public `axl/*.h` surface is
EFI-free. That last clause is now ENFORCED, not merely intended: the
generated headers require `AXL_ALLOW_UEFI`, granted by `axl-cc` to
`--type driver` / `--type runtime` and to an explicit `--allow-uefi`
(CMake: `axl_add_driver`, or `ALLOW_UEFI` on `axl_add_app`). Before
that guard, `uefi/` sat inside the SDK's `-isystem` directory and any
application could reach EDK2 by typing an `#include`. Spec updates are a manifest edit + regeneration; there's
no vendored EDK2 tree to merge against.

## Vision

A developer writes a standard C file with `#include <axl.h>` and
`int main(int argc, char **argv)`, runs `axl-cc app.c -o app.efi`,
and gets a working UEFI application. No EDK2 source tree, no .inf
files, no .dsc files, no PascalCase, no UEFI headers.

## Architecture

The SDK is a packaging layer on top of libaxl. It bundles the
pre-built static library with headers, a linker script, an entry
point stub, and the `axl-cc` build wrapper into a self-contained
distributable. No external dependencies — no EDK2, no gnu-efi.

```
┌─────────────────────────────────────────────┐
│  Consumer Application (hello.c)             │
│    #include <axl.h>                         │
│    int main(int argc, char **argv) { ... }  │
├─────────────────────────────────────────────┤
│  CRT0 entry stub (~17 lines)                │
│    src/crt0/axl-crt0-native.c               │
│    _AxlEntry → _axl_init → main → cleanup   │
├─────────────────────────────────────────────┤
│  libaxl.a (static library)                  │
│    AxlMem, AxlLog, AxlData, AxlStream, AxlFs,          │
│    AxlFormat, AxlLoop, AxlTask, AxlNet,     │
│    AxlRuntime (lifecycle services), …       │
├─────────────────────────────────────────────┤
│  AXL UEFI Headers (include/uefi/)           │
│    Auto-generated from UEFI/PI specs        │
├─────────────────────────────────────────────┤
│  UEFI Firmware (target system)              │
└─────────────────────────────────────────────┘
```

CRT0 and the runtime are different layers and worth keeping
straight. **CRT0** is the entry stub at the top: ~17 lines that
bridge UEFI's `_AxlEntry(ImageHandle, SystemTable)` to `int
main(argc, argv)`. **The runtime** is the lifecycle library
(`src/runtime/`) inside `libaxl.a` — it implements `_axl_init`,
`_axl_cleanup`, the default loop singleton, atexit, signal
handling, and the tier-1 resource registry. CRT0 invokes the
runtime; the runtime owns the state. Full design and the runtime-
vs-CRT0 split: [`docs/AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md).

### Entry Point Flow

```
UEFI firmware
  └→ _start                              [src/crt0/axl-crt0-gcc-{x86_64,aarch64}.S]
       └→ _AxlEntry(ImageHandle, SystemTable)  [src/crt0/axl-crt0-native.c]
            ├→ gST/gBS/gRT = ...         → set firmware table globals
            ├→ _axl_init()               → enter the runtime (default loop
            │                              lazy-init, signal notify, tier-1
            │                              registry, atexit registry, streams,
            │                              memory, console)
            ├→ _axl_get_args()           → argc/argv from Shell protocol
            ├→ main(argc, argv)          → user code
            └→ _axl_cleanup()            → re-enter the runtime (drain atexit
                                           LIFO, free default loop if any,
                                           sweep tier-1, leak report)
```

### Build Flow

```
axl-cc hello.c -o hello.efi
  │
  ├─ gcc -ffreestanding -nostdlib -fpic ...
  │    -c hello.c → hello.o
  │
  ├─ ld -nostdlib -shared -Bsymbolic
  │    -T elf_x86_64_efi.lds
  │    axl-crt0.o + hello.o + libaxl.a
  │    → hello.so (ELF shared object)
  │
  └─ objcopy --output-target=pei-x86-64 --subsystem=10
       hello.so → hello.efi (PE/COFF UEFI application)
```

## Phases

See `docs/ROADMAP.md` for the unified phase tracker with current
status. Summary:

- **Phase 1 (Core):** DONE — install.sh, axl-crt0, axl-cc, CMake
- **Phase 2 (Polish):** DONE — AARCH64, net module, error messages, --verbose, --version
- **Phase 3 (Distribution):** Pending — release tarballs, version stamp, automation
- **Phase 4 (Advanced):** DONE — multi-file, --type driver, --debug, --release, --run
- **Phase 5 (Backend):** DONE — backend abstraction layer (EDK2/gnu-efi backends
  were built then removed in Phase N7; native is the only backend)

## Key Technical Decisions

### GCC + objcopy toolchain

AXL uses GCC to compile to ELF, GNU ld to link as a shared object
with a custom linker script, and objcopy to convert to PE/COFF.
This is simpler than the EDK2 build system and requires only
standard GCC toolchain packages.

The linker scripts (`scripts/elf_x86_64_efi.lds`,
`scripts/elf_aarch64_efi.lds`) define the PE/COFF section layout.
The `_start` assembly stub (arch-specific) handles ELF entry and
calls `_AxlEntry` in C.

### No EDK2, no gnu-efi

AXL originally supported EDK2 and gnu-efi backends. Both were
removed in Phase N7 in favor of a native backend that provides its
own UEFI type definitions (auto-generated from spec HTML), CRT0,
and build toolchain. This eliminates all external firmware SDK
dependencies.

### Driver and runtime support

`axl-cc --type driver` produces DXE driver images.
`axl-cc --type runtime` produces runtime driver images.
These use a different subsystem value in the PE/COFF header and
the driver provides its own entry point (no axl-crt0).

### C++ support

axl-sdk supports C++ consumers as a first-class build target.
Shipped 2026-05-28; see commit history under `src/runtime/axl-cxxabi*`
and `scripts/axl-cc`.  Two pieces live together:

1. **Toolchain.** `axl-c++` (alias for `axl-cc -x c++`) compiles
   `.cpp` source with the freestanding-UEFI C++ flag set baked
   in: `-std=c++20 -fno-exceptions -fno-rtti
   -fno-threadsafe-statics -ffreestanding -fshort-wchar`, plus the
   per-arch additions (`-ffixed-x18` on AArch64, `-mno-red-zone`
   on X64).  `axl-cc` itself dispatches by file extension — `.c`
   → gcc, `.cpp`/`.cc`/`.cxx` → g++ — so mixed-language
   projects work with one driver.  Consumers can also use
   `axl-cc -c` for compile-only (build their own `.a` libraries)
   and pass pre-built `.o` / `.a` files to the linker.

2. **C++ runtime glue** (four objects under `lib/axl/<arch>/`).
   Since P4 (`AXL-Libc-Substrate-Design.md` §4d) the Itanium C++
   ABI itself comes from the toolchain's own
   `libstdc++`/`libsupc++`, which every C++ link carries — that is
   what makes `<iostream>`, `<sstream>` and `<fstream>` work.  AXL
   supplies only what a firmware image lacks underneath them:
   `axl-cxxrt-alloc.o` (the `sbrk` newlib's allocator grows into),
   `axl-cxxrt-stubs.o` (the POSIX porting layer over `AxlStream`),
   `axl-cxxrt-eh.o` (unwind frame-table registration) and
   `axl-cxxrt-terminate.o` (a terminate handler that reports
   through the UEFI console).  They ship as OBJECTS rather than an
   archive because two of them work by PREEMPTION, which an
   archive member cannot do.  Companion symbols `__cxa_atexit`
   (routes through `axl_atexit`), `__dso_handle`, and the
   `.init_array` walker live in `libaxl.a`
   (`src/runtime/axl-cxxabi.c`) so they're always present even
   for pure-C apps that incidentally link a C++ helper from a
   library.  See [`AXL-Lifecycle.md` §2.1.1](AXL-Lifecycle.md)
   for static-initializer timing.

**AArch64 needs the ARM bare-metal toolchain**
(`aarch64-none-elf-g++`, ARM developer.arm.com, pinned to
14.3.Rel1).  Run `scripts/install-arm-toolchain.sh` to fetch +
verify + extract the ~96 MB tarball to `/opt/`.
`scripts/install.sh` auto-detects the toolchain at standard paths
and builds C++ support when present (no `--cpp` opt-in required).
The Linux-ABI cross (`aarch64-linux-gnu-g++`) is NOT viable —
its libstdc++ headers pull hosted typedefs.

**Single package.**  The C++ glue objects + `axl-c++` + the C++
headers ship in the regular `axl-sdk.deb` / `.rpm` (no `-cpp`
subpackage).  The package conveys NO libstdc++ — `axl-cc`
resolves the consumer's own installed copy through
`-print-file-name`, which is what keeps the GCC Runtime Library
Exception's one restriction out of scope (see
[`AXL-Cxx-Design.md` §8](AXL-Cxx-Design.md)).  That toolchain is
already a hard prerequisite for ANY link since P3 put
`libc.a`/`libm.a`/`libgcc.a` on every one, so C++ adds no install
step.  Pure-C consumers can ignore the extra files — they pay no
runtime cost.

**Forbidden C++ features in axl-sdk-targeted code:** exceptions,
RTTI (`typeid` / `dynamic_cast`), `<string>` / `<vector>` /
`<stdexcept>`, `thread_local`, `<format>`.  All require
libstdc++/libsupc++ symbols not available in our freestanding
link.  Validated end-to-end in CPP1.3–1.5; matches every serious
UEFI-C++ project's experience (the standard `-fno-exceptions
-fno-rtti` config).  Usable freestanding subset: `<array>`,
`<span>`, `<string_view>`, `<type_traits>`, `<utility>`,
`<initializer_list>`, `<new>`, `<optional>`, `<variant>`,
`<expected>` (C++23) + header-only pieces of `<algorithm>` /
`<numeric>` / `<functional>`.  See
[`AXLMM-Design.md` §"Toolchain & constraints"](AXLMM-Design.md#toolchain--constraints)
for the full list.

#### Wrapper-class library (`axlmm`) — design done, implementation deferred

A sibling C++ wrapper library — `axlmm`, modeled on glibmm —
adds ergonomic enhancements (RAII handles, sticky error chains,
`std::expected` factories, range-for adapters) on top of the C
API.  **Implementation is deferred indefinitely until a real C++
consumer surfaces usage patterns that inform the wrapper design**;
the spec is captured in [`AXLMM-Design.md`](AXLMM-Design.md) so
implementation can resume from a known starting point.
Reasoning: glibmm came after glib had four years of consumer
evolution; designing `axlmm` wrappers without a real consumer
risks wrapping the wrong things.

#### First C++ consumer: AGT

The AGT widget toolkit
([`AGT-Design.md`](https://github.com/aximcode/agt/blob/main/docs/AGT-Design.md),
separate repo `aximcode/agt`) builds in C++ on top of axl-sdk's
shipped C++ toolchain.  **AGT calls the axl-sdk C API directly**
— no `axlmm` wrapper dependency in v0.1.  `extern "C"`
declarations in axl-sdk headers make C++ → C calls zero-ceremony;
`AXL_AUTOPTR(Type)` (a GCC cleanup attribute macro) gives RAII
for owned C handles.  AGT's actual usage patterns will inform any
future axlmm implementation.

### Async-op cancellation

Async operations in AXL (`axl_tcp_connect_async`,
`axl_tcp_accept_async`, `axl_tcp_send_async`, `axl_tcp_recv_async`,
HTTP client, ...) accept an optional `AxlCancellable *`. Cancelling
it aborts every op observing it; each op's callback fires exactly
once with status `AXL_CANCELLED` (-2 in `<axl/axl-macros.h>`). The
same return code covers the shell break event (Ctrl-C), so
consumers see one status for "some external source stopped me."

Two companion primitives round out the set: `AxlEvent` (a one-shot
producer/waiter rendezvous — AXL's foundational latch, replacing the
earlier `AxlCompletion`) and the `axl_wait_*` helpers (`axl_wait_for`,
`axl_wait_for_flag`, `axl_wait_ms`, ...), all of which accept the same
`AxlCancellable`. See `src/event/README.md` for the mental model,
ownership rules, and worked patterns (timeout, subsystem shutdown,
user abort). For the full concurrency-primitive taxonomy and the
"why this model, not Python's GIL / stackful coroutines / protothreads"
discussion, see [`AXL-Concurrency.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md).

## Dependencies

### Build-time (install.sh)

| Dependency | Purpose |
|-----------|---------|
| GCC | Compiler (x86_64-linux-gnu-gcc) |
| aarch64-linux-gnu-gcc | AARCH64 cross-compiler |
| GNU ld | Linker |
| objcopy | ELF → PE/COFF conversion |
| `install -C` | Content-comparing install (see below) |

#### Reinstalling unchanged sources must be a filesystem no-op

`install.sh` is not a one-shot packaging step — a consumer building against a
checkout typically reinstalls the SDK on **every** build (an order-only
`sdk-sync` prerequisite is the common shape). That makes install-time mtime
churn a correctness problem for the consumer's build graph, not a cosmetic one:
SDK headers reach the compiler via `-isystem`, so gcc lists them in `-MD`
depfiles, and an unconditional `cp` refreshes every one of them on every run.
Measured: 145 of the 150 SDK headers in a single depfile went stale after a
no-op reinstall, so every no-op consumer build recompiled essentially its whole
tree. Consumers were pushed into hand-rolled content fingerprints to survive it.

So the contract is: **a reinstall whose inputs have not changed touches
nothing.** Every copy goes through `install -C` (compare content; leave an
identical destination completely alone), and every *generated* file — the
per-arch `.pc`, `axl-config.cmake`, `share/axl/{backend,version,build-date}` —
goes through the `write_if_changed` helper, which is the same idea for content
produced on the fly rather than copied.

`install -C` rather than `cp -u`: `cp -u` compares **mtimes**, so a destination
that is newer than its source but differs in content is silently left in place
— a hand-edited installed header, or a prefix restored from a backup, would
never be repaired. Content comparison is the semantics that matches the intent,
and both GNU and BSD `install` implement `-C` that way.

Pinned by `test/integration/test-install-idempotent.sh`, whose discriminating
gate corrupts an installed file *and* makes it newer than its source: an
over-eager skip fails it, and so does `cp -u`.

### Consumer-time (axl-cc)

| Dependency | Purpose |
|-----------|---------|
| GCC | Compile + link |

No EDK2. No Python. No Java. No Make.

## Distribution Model

**No binaries in git.** `install.sh` stages an install **prefix** into
`stage/`; `out/` holds the object trees and `out/docs/`. Those are different
things with different lifetimes — see
[AXL-Distribution-Design.md](AXL-Distribution-Design.md) §4, which owns the
split. (This paragraph said "into `out/`" until 2026-08-29, contradicting the
`stage/` paths three lines below it.)

**Ways to get the SDK:**

1. **Distro package** (the supported consumer path today):
   ```
   sudo dnf install ./axl-sdk.rpm      # or: apt install ./axl-sdk.deb
   axl-cc hello.c                      # -> hello.efi
   ```

2. **Build from source** (developer workflow):
   ```
   git clone axl-sdk
   ./scripts/install.sh --arch x64
   # produces stage/bin/axl-cc, stage/lib/axl/<arch>/libaxl.a, stage/include/
   ```

3. **Download a release tarball** (root-free, any distro):
   ```
   tar xf axl-sdk-<ver>-linux-x86_64.tar.gz
   ./bin/axl-cc hello.c -o hello.efi
   ```
   **Shipped 2026-08-29.** This doc promised the workflow in the present
   tense for years while nothing built it; it is real now, produced by
   `scripts/make-sdk-tarball.sh` and published as a release asset. The
   archive unpacks to a single `axl-sdk-<version>/` directory, so extracting
   into `/opt` gives the versioned root of
   AXL-Distribution-Design.md §12.2 with nothing to rename.
