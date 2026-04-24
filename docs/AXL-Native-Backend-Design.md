# AXL Native UEFI Backend — Design Document

## Goal

Remove EDK2 and gnu-efi dependencies entirely. AXL provides its own
UEFI type definitions, CRT0, and build toolchain. The SDK becomes a
self-contained UEFI development kit — just GCC or Clang, no external
UEFI projects needed.

The SDK supports all three UEFI image types: applications, boot
service drivers, and runtime drivers.

## UEFI Image Types

| Type | Subsystem | Entry | Shell | Resident |
|------|-----------|-------|-------|----------|
| Application | 10 | `int main(argc, argv)` via CRT0 | Yes | No |
| Boot Service Driver | 11 | User's `DriverEntry(ImageHandle, ST)` | No | Until ExitBootServices |
| Runtime Driver | 12 | User's `DriverEntry(ImageHandle, ST)` | No | Persistent |

**Applications** are the primary target: shell-launched programs with
`int main()` entry, argc/argv, and automatic cleanup on exit.

**Boot service drivers** (DXE drivers) install protocols, manage
hardware, or extend firmware during the boot phase. They persist
until ExitBootServices is called by the OS loader.

**Runtime drivers** persist after ExitBootServices. They provide
services to the OS (e.g., variable storage, RTC). They must handle
virtual address remapping and cannot use boot services after
ExitBootServices.

### Entry point design

Library init is separated from application init so all image types
can use the AXL library:

- `_axl_init(ImageHandle, SystemTable)` — sets gST/gBS/gRT globals,
  locates shell protocol. Called by CRT0 (apps) or user code (drivers).
- `_axl_get_args(&argc, &argv)` — shell argument conversion. App-only.
- `_axl_cleanup()` — frees arg memory. App-only.

**Application entry** (via CRT0):
```c
EFI_STATUS EFIAPI _AxlEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    _axl_init(image, systab);
    int argc; char **argv;
    _axl_get_args(&argc, &argv);
    int rc = main(argc, argv);
    _axl_cleanup();
    return rc == 0 ? EFI_SUCCESS : EFI_ABORTED;
}
```

**Driver entry** (no CRT0 — user writes entry directly):
```c
#include <uefi/axl-uefi.h>
#include <axl.h>

EFI_STATUS EFIAPI
DriverEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    _axl_init(ImageHandle, SystemTable);
    axl_printf("MyDriver loaded\n");
    // install protocols, set up driver binding, etc.
    return EFI_SUCCESS;
}
```

The `axl-cc` wrapper selects the right entry point and PE subsystem:
```
axl-cc hello.c -o hello.efi                  # default: application
axl-cc --type driver mydrv.c -o mydrv.efi    # boot service driver
axl-cc --type runtime myrt.c -o myrt.efi     # runtime driver
```

## What AXL Already Has

- `axl-backend.h` — backend abstraction isolating ALL UEFI interactions
- `axl-crt0.c` — defines minimal UEFI types for the entry point
- `axl-elf2efi.c` — ELF→PE/COFF converter (WIP — not yet functional, needs finishing for GCC path)
- `axl-net-compat-gnuefi.h` — TCP4/DNS4/IP4Config2 protocol structs from spec
- `compat/` headers — BaseLib, PrintLib, etc. with inline implementations
- Clang toolchain path — compiles directly to PE/COFF, no ELF conversion

## Architecture

```
AXL_BACKEND_NATIVE  (new — zero external UEFI deps)
AXL_BACKEND_EDK2    (existing — kept for EDK2 integration)
AXL_BACKEND_GNUEFI  (existing — kept until native reaches parity)
```

### Header and code layers

```
include/axl.h + include/axl/    Public AXL API (standard C, no UEFI types)
                                 App developers use only this layer.

include/uefi/                    UEFI type definitions (public)
                                 AXL's own UEFI headers — replaces EDK2's
                                 <Uefi.h> and gnu-efi's <efi.h>.
                                 Driver authors use this + the layer above.

src/backend/                     Backend implementations (private)
                                 One per backend. Implements the 37
                                 axl_backend_* functions from axl-backend.h
                                 using the UEFI types from include/uefi/.
```

The `include/uefi/` headers are **public** because they serve two
audiences: the AXL library internals and SDK consumers who need raw
UEFI access (driver authors, protocol implementers). They are NOT
private to the native backend.

The native backend itself (`src/backend/native/`) is intentionally
thin (~815 lines) because it delegates to the UEFI firmware. It
doesn't reimplement memory management, event dispatch, or file I/O —
it calls `gBS->AllocatePool`, `gBS->CreateEvent`, `Shell->ReadFile`
etc. through the function pointers in the firmware's system tables.
This is the same approach as gnu-efi's libefi.a, minus the ABI
translation layer (clang compiles to ms_abi natively).

### Files

```
include/uefi/
  axl-uefi.h              Umbrella: includes all below
  axl-uefi-types.h        Base types: EFI_STATUS, UINTN, UINT8-64, CHAR16, etc.
  axl-uefi-status.h       Status codes: EFI_SUCCESS, EFI_ERROR, EFI_NOT_READY, etc.
  axl-uefi-tables.h       EFI_SYSTEM_TABLE, EFI_BOOT_SERVICES, EFI_RUNTIME_SERVICES
  axl-uefi-protocols.h    Console, Shell, TCP4, IP4, DNS4, MP, SMBIOS,
                           ServiceBinding, DriverBinding
  axl-uefi-guids.h        All protocol GUIDs used by AXL
  axl-uefi-calling.h      EFIAPI, IN/OUT/OPTIONAL

src/backend/
  axl-backend-native.c    Native backend implementation

src/crt0/
  axl-crt0-native.c       App entry point: ST/BS/RT init + bridge to main()
                           (not used for drivers — user writes entry directly)

Makefile.native            Build with native backend (GCC or Clang, no external deps)
```

### What each header defines (from UEFI Spec 2.10)

**axl-uefi-calling.h** (~30 lines):
```c
#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#elif defined(__aarch64__)
#define EFIAPI
#else
#error "Unsupported architecture"
#endif
#define IN
#define OUT
#define OPTIONAL
#define VOID void
```

No `uefi_call_wrapper` needed — on the native backend, ALL code is
compiled with ms_abi on x86_64 so user functions and UEFI functions
use the same calling convention. Direct calls work.

**axl-uefi-types.h** (~180 lines):
- Scalar types: BOOLEAN, INT8-64, UINT8-64, CHAR8, CHAR16, UINTN, INTN
- Handle/event: EFI_HANDLE, EFI_EVENT, EFI_STATUS
- Constants: TRUE, FALSE, NULL, MAX_UINTN
- Composite: EFI_GUID, EFI_TIME, EFI_INPUT_KEY, EFI_IPv4_ADDRESS, EFI_MAC_ADDRESS
- Enums: EFI_MEMORY_TYPE, EFI_TIMER_DELAY, EFI_LOCATE_SEARCH_TYPE
- Event flags: EVT_TIMER, EVT_NOTIFY_WAIT, EVT_NOTIFY_SIGNAL
- TPL levels: TPL_APPLICATION, TPL_CALLBACK, TPL_NOTIFY
- Utility: `axl_guid_equal()` inline helper

**axl-uefi-status.h** (~50 lines):
- EFI_SUCCESS, EFI_ERROR() macro
- ~16 error codes actually used by AXL (NOT_FOUND, TIMEOUT, etc.)

**axl-uefi-tables.h** (~340 lines):
- `EFI_TABLE_HEADER`
- `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` (OutputString, SetAttribute, Mode)
- `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` (ReadKeyStroke, WaitForKey)
- `EFI_CONFIGURATION_TABLE` (for SMBIOS walk)
- `EFI_FILE_INFO` + file mode constants
- `EFI_BOOT_SERVICES` — all ~44 function pointer slots in UEFI 2.10
  spec order. Used slots have proper typedefs; unused slots are `void *`
  with comments naming each. Includes both library-used slots
  (AllocatePool, CreateEvent, LocateProtocol, etc.) and driver-used
  slots (InstallProtocolInterface, OpenProtocol, etc.)
- `EFI_RUNTIME_SERVICES` — all ~14 slots, GetTime used
- `EFI_SYSTEM_TABLE`

**axl-uefi-protocols.h** (~830 lines):
- Shell protocol (OpenFileByName, CloseFile, ReadFile, WriteFile, etc.)
- ServiceBinding protocol (CreateChild, DestroyChild)
- TCP4 protocol + all config/token/fragment structs
- IP4 protocol + config/header/token structs
- IP4Config2 protocol
- DNS4 protocol + config/token/completion structs
- MP Services protocol
- SMBIOS protocol + entry point tables (SMBIOS 2.x and 3.0)
- Driver Binding protocol (Supported, Start, Stop) — for DXE drivers
- Component Name 2 protocol — optional driver naming

**axl-uefi-guids.h** (~90 lines):
- 7 network GUIDs (TCP4, IP4, IP4Config2, DNS4 + service bindings)
- Shell, MP Services, SMBIOS protocol GUIDs
- SMBIOS/SMBIOS3 configuration table GUIDs
- Driver Binding GUID
- All as `static const EFI_GUID` (rodata, per-TU)

### CRT0 (axl-crt0-native.c)

Replaces the gnu-efi chain (_start → _relocate → _entry → InitializeLib → efi_main).

**Only used for applications.** Drivers provide their own entry point
and call `_axl_init()` directly.

**For Clang+lld-link:** No relocation handler needed — PE loader handles it.
Entry point is `_AxlEntry` directly. Just set global ST/BS/RT pointers.

```c
EFI_STATUS EFIAPI _AxlEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    _axl_init(image, systab);
    int argc; char **argv;
    _axl_get_args(&argc, &argv);
    int rc = main(argc, argv);
    _axl_cleanup();
    return rc == 0 ? EFI_SUCCESS : EFI_ABORTED;
}
```

**For GCC+axl-elf2efi:** Need a minimal relocation handler (ELF→PE
relocations are baked in by axl-elf2efi, but the PE loader applies
them at load time — no user-side handler needed). Same CRT0 as above.

### Backend implementation (axl-backend-native.c)

Almost identical to `axl-backend-edk2.c` but:
- Includes `<uefi/axl-uefi.h>` instead of `<Uefi.h>` + EDK2 libraries
- Uses global `gST`/`gBS`/`gRT` set by CRT0 or `_axl_init()`
- Self-implements string functions (already done in compat headers)
- Self-implements memory functions: `AllocatePool` → `gBS->AllocatePool`
- Shell protocol located at runtime (same as gnu-efi backend)
- No `uefi_call_wrapper` — direct calls (native ABI)

Works identically for applications and drivers — the backend doesn't
care what image type loaded it.

### Build system (Makefile.native)

Two toolchain options:

**GCC path:**
```makefile
CC = gcc
CFLAGS = -fpic -ffreestanding -fshort-wchar -fno-stack-protector \
         -fno-builtin -mno-red-zone -DAXL_BACKEND_NATIVE \
         -Iinclude -Iinclude/uefi -Isrc/backend
LDFLAGS = -shared -Bsymbolic --gc-sections -T axl-native.lds
# Then: axl-elf2efi app.so app.efi
```

**Clang path (preferred — no conversion step):**
```makefile
CC = clang
CFLAGS = -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
         -fno-builtin -DAXL_BACKEND_NATIVE \
         -Iinclude -Iinclude/uefi -Isrc/backend
LD = lld-link
LDFLAGS = /NODEFAULTLIB /ENTRY:$(ENTRY) /SUBSYSTEM:$(SUBSYSTEM) /DLL
# Direct .efi output
```

Image type controls two linker flags:

| `axl-cc --type` | `SUBSYSTEM` | `ENTRY` | Links CRT0 |
|-----------------|-------------|---------|------------|
| `app` (default) | `EFI_APPLICATION` | `_AxlEntry` | Yes |
| `driver` | `EFI_BOOT_SERVICE_DRIVER` | `DriverEntry` | No |
| `runtime` | `EFI_RUNTIME_DRIVER` | `DriverEntry` | No |

For drivers, the user's entry point name defaults to `DriverEntry`
but can be overridden with `--entry MyEntry`.

## Implementation Phases

### Phase N1: UEFI type headers — DONE
- Created `include/uefi/` with all type definitions from the spec
- Consolidated from: `axl-crt0.c` types, `axl-net-compat-gnuefi.h`,
  compat headers, UEFI 2.10 spec
- 7 headers, 1540 lines total
- Compiles clean on x86_64 and AARCH64 with -Wall -Wextra -Wpedantic
- **Test:** Include from a .c file and compile (types resolve) ✓

### Phase N2: Native backend + CRT0
- Create `axl-backend-native.c` (copy from edk2, change includes)
- Create `axl-crt0-native.c` (app only: set globals + bridge to main)
- Add `AXL_BACKEND_NATIVE` case to `axl-backend.h`
- Type remaining BS slots needed for drivers (InstallProtocolInterface,
  OpenProtocol, etc.)
- Add EFI_DRIVER_BINDING_PROTOCOL to protocols header
- **Test:** Build hello.c (app) with clang, run in QEMU
- **Test:** Build minimal driver with clang, load in QEMU

### Phase N3: Build system (Makefile.native)
- Wire up all library sources with native backend
- Build libaxl.a + CRT0 with no external deps
- Support `TYPE=app` (default) and `TYPE=driver` / `TYPE=runtime`
- **Test:** `make -f Makefile.native hello` → hello.efi runs in QEMU
- **Test:** `make -f Makefile.native TYPE=driver mydrv` → mydrv.efi loads

### Phase N4: Full test suite
- Run all 411 tests on native backend
- Fix any type mismatches or missing definitions
- **Test:** 411/411 on native

### Phase N5: SDK integration
- Update `install.sh` to support `--backend native`
- Generate `axl-cc` that uses native backend
- Add `axl-cc --type driver|runtime` support
- Add `axl-cc --entry <name>` for custom driver entry points
- **Test:** `axl-cc hello.c -o hello.efi` with zero external deps
- **Test:** `axl-cc --type driver mydrv.c -o mydrv.efi`

### Phase N6: UEFI header generation from spec HTML — DONE
- Manifest-driven generator extracts 275 definitions from spec HTML
- Sources: UEFI 2.11, PI 1.8, ACPI 6.5 (HTML); Shell 2.2 (PDF, hand-written)
- `--check` flag validates source coverage (integrated into build.sh)
- Add new protocols by adding entries to `scripts/uefi-manifest.json5`

### Phase N7: Deprecate EDK2/gnu-efi backends (optional)
- Make native the default
- Keep EDK2 backend for users who need EDK2 .inf integration
- Remove gnu-efi backend (native replaces it completely)

## UEFI Header Generation (Phase N6 — DONE)

UEFI type definitions are auto-generated from spec HTML using a
manifest-driven pipeline:

1. **`scripts/download-uefi-specs.py`** — downloads spec HTML from
   uefi.org (UEFI 2.11, PI 1.8, ACPI 6.5, Shell 2.2 PDF)
2. **`scripts/uefi-manifest.json5`** — declares 275 types to extract,
   each with name, kind (struct/enum/funcptr/define/typedef/table),
   and optional alias for spec naming mismatches
3. **`scripts/generate-uefi-headers.py`** — searches spec `<pre>`
   blocks and Table 2-4 for each manifest entry, extracts by C
   boundaries, emits into `include/uefi/generated/`

**Manifest order is dependency order** — no sorting or forward
declaration analysis needed. Unknown struct member types (from
protocols not in the manifest) are automatically replaced with
`void *` to maintain correct struct layout.

**Adding a new protocol:** add entries to `uefi-manifest.json5`
(funcptr typedefs before their protocol struct) and regenerate.
The `--check` flag (integrated into build.sh) warns about types
used in source but missing from the manifest.

**Hand-written items** (`include/uefi/axl-uefi-extra.h`):
- Shell protocol + Shell Parameters — Shell spec is PDF-only
- `SMBIOS_HANDLE_PI_RESERVED` — EDK2 convention (0xFFFE)
- 2 Shell GUIDs — not in any HTML spec
- `SHELL_FILE_HANDLE`, `TRUE`, `FALSE`, `NULL`, `MAX_UINTN`,
  `EFI_TEXT_ATTR` — not defined in any spec code block or table

## What This Eliminates

| Dependency | Current | After |
|-----------|---------|-------|
| EDK2 source tree | Required for library build | Optional (only for .inf builds) |
| gnu-efi-devel package | Required for gnu-efi build | Not needed |
| GenFw | Required for ELF→PE (EDK2 GCC) | Replaced by axl-elf2efi (WIP) or avoided via clang path |
| libgnuefi.a | Relocation handler | Not needed (PE loader handles it) |
| libefi.a | Runtime library | Not needed (self-implemented) |
| crt0-efi-x86_64.o | Entry point | Replaced by axl-crt0-native (apps) or user entry (drivers) |

The SDK becomes: **GCC or Clang + AXL headers + axl-elf2efi** = .efi files.
No other UEFI project required. Applications, drivers, and runtime
drivers from the same toolchain.

## Verification

Each phase is independently testable:
- N1: Type headers compile ✓
- N2-N4: 411/411 tests pass on native backend ✓
- N5: axl-cc works end-to-end for apps and drivers
- N6: 275 definitions extracted from spec, 0 compile errors ✓
- N7: gnu-efi backend removed, all tests still pass (optional)
