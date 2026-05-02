# AXL SDK Design

Part of the [AximCode](https://github.com/aximcode) project.
AXL = AximCode Library. Pronounced "axle."

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
EFI-free. Spec updates are a manifest edit + regeneration; there's
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

### Consumer-time (axl-cc)

| Dependency | Purpose |
|-----------|---------|
| GCC | Compile + link |

No EDK2. No Python. No Java. No Make.

## Distribution Model

**No binaries in git.** All build artifacts are produced by
`install.sh` into `out/`, which is gitignored.

**Two ways to get the SDK:**

1. **Build from source** (developer workflow):
   ```
   git clone axl-sdk
   ./scripts/install.sh --arch x64
   # produces out/bin/axl-cc, out/lib/libaxl.a, out/include/
   ```

2. **Download a release tarball** (consumer workflow):
   ```
   tar xf axl-sdk-x64-linux.tar.gz
   ./bin/axl-cc hello.c -o hello.efi
   ```

Release tarballs are built by GitHub Actions on tagged commits
and attached to GitHub Releases.
