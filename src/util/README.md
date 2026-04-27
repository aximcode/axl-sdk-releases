System operations, environment variables, time, NVRAM storage, driver
lifecycle, hex dump, configuration framework (including command-line
parsing), and path manipulation.

Headers:

- `<axl/axl-sys.h>` — System operations (reset, GUID, device map refresh)
- `<axl/axl-env.h>` — Environment variables and working directory
- `<axl/axl-time.h>` — Wall-clock time and monotonic timestamps
- `<axl/axl-nvstore.h>` — UEFI NVRAM variable access
- `<axl/axl-driver.h>` — Driver binding and lifecycle
- `<axl/axl-diag.h>` — Tool diagnostic helpers (`-v` output)
- `<axl/axl-hexdump.h>` — Hex/ASCII dump formatting
- `<axl/axl-config.h>` — Unified configuration + command-line parsing
- `<axl/axl-path.h>` — Path manipulation

The event/cancellable/wait primitives previously listed here now
live in [`src/event/`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event) — see
[`src/event/README.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md).

## System Utilities

### GUIDs

UEFI identifies protocols, variables, and services by 128-bit GUIDs.
AXL provides `AxlGuid` (standard C types, no UEFI headers needed)
and the `AXL_GUID` macro for initialization:

```c
AxlGuid my_guid = AXL_GUID(0x12345678, 0xabcd, 0xef01,
    0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01);

if (axl_guid_cmp(&a, &b) == 0) {
    // GUIDs are equal
}
```

### Firmware Globals

After `AXL_APP` or `axl_driver_init`, these globals are available
(typed when `<uefi/axl-uefi.h>` is included):

- `gST` — System Table (`EFI_SYSTEM_TABLE *`)
- `gBS` — Boot Services (`EFI_BOOT_SERVICES *`)
- `gRT` — Runtime Services (`EFI_RUNTIME_SERVICES *`)
- `gImageHandle` — handle of the running application or driver

### NVRAM Variables

Read and write persistent UEFI variables (survive reboot):

```c
// Read
uint8_t secure_boot;
size_t sz = sizeof(secure_boot);
if (axl_nvstore_get("global", "SecureBoot", &secure_boot, &sz) == 0) {
    axl_printf("SecureBoot: %s\n", secure_boot ? "on" : "off");
}

// Write
axl_nvstore_set("app", "last-run", timestamp, timestamp_len,
                AXL_NVSTORE_BOOT | AXL_NVSTORE_NV);
```

### Driver Lifecycle

Build DXE drivers with `axl-cc --type driver`. The driver entry point
is `DriverEntry` (not `main`). Call `axl_driver_init` to set up
the AXL runtime:

```c
EFI_STATUS EFIAPI DriverEntry(EFI_HANDLE ImageHandle,
                               EFI_SYSTEM_TABLE *SystemTable) {
    axl_driver_init(ImageHandle, SystemTable);
    axl_printf("Driver loaded\n");
    // ...
}
```

See `sdk/examples/driver.c` for a complete example.

### Auto-Loading Driver Dependencies

Tools that need a protocol provided by a DXE driver (e.g. a RAM-disk
manager that needs `EFI_RAM_DISK_PROTOCOL` from `RamDiskDxe.efi`)
can call `axl_driver_ensure` to short-circuit when the protocol is
already registered, or to find and load the driver themselves
otherwise:

```c
if (axl_driver_ensure(&EfiRamDiskProtocolGuid,
                      "RamDiskDxe.efi") != 0) {
    axl_printf("RamDiskDxe.efi not available\n");
    return 1;
}
/* Protocol is now usable. */
```

The search walks `drivers/<arch>/<name>` on the running image's own
volume first, then the image's own directory, then the volume root,
and finally every other mounted FAT volume. The first match is
loaded and started; if it doesn't end up registering the requested
protocol, the image is unloaded and the search continues. This
lets tools work whether they're invoked from a bare UEFI shell, a
boot menu, or a `startup.nsh` that has already eager-loaded the
driver.

### Tool Diagnostics

When investigating "why doesn't my tool work on this firmware?", call
`axl_diag_startup(argc, argv)` from your `-v` / `--verbose` handler.
It prints six labelled sections in one block:

```
POSIX argc = 3
POSIX argv[0] = "mkrd.efi"
POSIX argv[1] = "-v"
POSIX argv[2] = "testrd"
LOADOPT: size = 38 bytes
LOADOPT: utf8 = "mkrd.efi -v testrd"
SHELL: protocol OK, Argc = 3
SHELL: Argv[0] = "FS0:\mkrd.efi"
...
IMG: path = \mkrd.efi
VOLUMES: 1 mounted
  fs0
```

POSIX argv shows what reached `main` after `axl-app.c` parsed
`EFI_LOADED_IMAGE_PROTOCOL.LoadOptions`. LOADOPT shows the raw
UCS-2 buffer the firmware passed in. SHELL is the optional
`EFI_SHELL_PARAMETERS_PROTOCOL` probe — Dell firmware sometimes
doesn't publish it for cross-volume invocations, which was the
original "argc=1" bug. IMG and VOLUMES are the search anchors
`axl_driver_ensure` / `axl_driver_locate` use.

For protocol-registration questions specifically, pair it with
`axl_diag_probe_protocol`:

```c
if (verbose) {
    axl_diag_startup(argc, argv);
    axl_diag_probe_protocol(
        (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
        "EFI_RAM_DISK_PROTOCOL");
}
/* ... call axl_driver_ensure ... */
if (verbose) {
    axl_diag_probe_protocol(
        (const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
        "EFI_RAM_DISK_PROTOCOL (post-ensure)");
}
```

The two probes around `axl_driver_ensure` show whether the firmware
already had the driver baked in (both `ALREADY REGISTERED`) or
whether ensure had to load it from disk (`NOT registered` → `REGISTERED`).

## Configuration (and Command-Line Parsing)

Unified configuration framework. One descriptor table drives defaults,
typed getters, auto-apply to caller structs via `offsetof`, callbacks
for custom logic, parent inheritance for cascading defaults, and
command-line argument parsing (short flags, long flags, repeatable
multi-values, positional args, and `--`).

**AxlConfig** replaces ad-hoc key-value parsing with a declarative
system. You define a table of option descriptors (name, type, default
value, optional short flag, help text), then populate from any source:
defaults, programmatic `set`, command-line `argv`, or a parent config.
Type validation happens automatically.

### Defining Options

```c
#include <axl.h>

typedef struct {
    size_t  port;
    bool    verbose;
    size_t  max_connections;
} ServerConfig;

static const AxlConfigDesc opts[] = {
    { "port",     AXL_CFG_UINT, "8080", 0, "Listen port",
      offsetof(ServerConfig, port), sizeof(size_t) },
    { "verbose",  AXL_CFG_BOOL, "false", 0, "Verbose output",
      offsetof(ServerConfig, verbose), sizeof(bool) },
    { "max.conn", AXL_CFG_UINT, "16", 0, "Max connections",
      offsetof(ServerConfig, max_connections), sizeof(size_t) },
    { 0 }
};
```

### Creating and Querying

```c
ServerConfig sc;
AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(opts);

// Set the auto-apply target -- values are written directly
// into the struct fields via offsetof
axl_config_set_target(cfg, &sc);

// Set values (type-validated)
axl_config_set(cfg, "port", "9090");       // sc.port = 9090
axl_config_set(cfg, "verbose", "true");    // sc.verbose = true

// Query values
size_t port = axl_config_get_uint(cfg, "port");
const char *port_str = axl_config_get(cfg, "port");  // "9090"
```

### Command-Line Integration

The same descriptor table that defines config options also drives
command-line parsing. Each descriptor's `short_flag` produces a short
option (e.g. `-p`) and its `key` produces the long form (e.g.
`--port`). Bool types toggle on without a value (`-v`), other types
take the next argv (`-p 8080` or `--port=8080`). Repeatable options
use `AXL_CFG_MULTI`. Positional arguments are available via
`axl_config_pos` / `axl_config_pos_count`; `--` terminates parsing.

```c
static const AxlConfigDesc descs[] = {
    { "port",    AXL_CFG_UINT, "8080",  'p', "Listen port",    0, 0 },
    { "verbose", AXL_CFG_BOOL, "false", 'v', "Verbose output", 0, 0 },
    { "header",  AXL_CFG_MULTI, NULL,   'H', "HTTP header (repeatable)", 0, 0 },
    { "help",    AXL_CFG_BOOL, "false", 'h', "Show this help", 0, 0 },
    { 0 }
};

int main(int argc, char **argv) {
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    if (cfg == NULL || axl_config_parse_args(cfg, argc, argv) != 0) {
        axl_config_usage(cfg, "myapp", "[options] <file>");
        return 1;
    }

    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "myapp", "[options] <file>");
        return 0;
    }

    size_t port    = axl_config_get_uint(cfg, "port");
    bool   verbose = axl_config_get_bool(cfg, "verbose");
    const char *file = axl_config_pos(cfg, 0);
    (void)port; (void)verbose; (void)file;
    return 0;
}
```

### Multi-Value Options

For options that can be specified multiple times (e.g., `-H "Name: Value"`):

```c
size_t count = axl_config_get_multi_count(cfg, "headers");
for (size_t i = 0; i < count; i++) {
    const char *hdr = axl_config_get_multi(cfg, "headers", i);
    axl_printf("  header: %s\n", hdr);
}
```

### Parent Inheritance

Create a child config that inherits defaults from a parent:

```c
AxlConfig *defaults = axl_config_new(opts);
axl_config_set(defaults, "port", "8080");

AxlConfig *override = axl_config_new_with_parent(opts, defaults);
// override inherits "port"="8080" until explicitly set
```

## Path Manipulation

Path manipulation: basename, dirname, extension, join, resolve. Handles
both `/` (Unix) and `\` (UEFI) path separators. All allocated results
are freed with `axl_free()`.

UEFI uses backslash (`\`) as the path separator, while most developers
are familiar with forward slash (`/`). AXL accepts both and normalizes
internally. Paths typically start with a volume name: `fs0:/path/to/file`.

```c
AXL_AUTO_FREE char *base = axl_path_get_basename("fs0:/logs/app.log");
// base = "app.log"

AXL_AUTO_FREE char *dir = axl_path_get_dirname("fs0:/logs/app.log");
// dir = "fs0:/logs"

AXL_AUTO_FREE char *ext = axl_path_get_extension("app.log");
// ext = "log"

AXL_AUTO_FREE char *full = axl_path_join("fs0:/data", "output.json");
// full = "fs0:/data/output.json"

// Resolve relative paths
char resolved[256];
axl_path_resolve("fs0:/app", "../config/app.cfg",
                 resolved, sizeof(resolved));
// resolved = "fs0:/config/app.cfg"
```

## Synchronization primitives

AxlCompletion / AxlCancellable / axl_wait_* and the foundational
`AxlEvent` moved out of AxlUtil into the dedicated
[`src/event/`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event) module. See
[src/event/README.md](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) for the current documentation.
