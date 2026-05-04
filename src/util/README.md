System operations, environment variables, time, NVRAM storage,
boot-option management, x86 I/O port access, driver lifecycle, hex
dump, configuration framework (including command-line parsing), and
path manipulation.

Headers:

- `<axl/axl-sys.h>` — System operations (reset, GUID, device map refresh)
- `<axl/axl-env.h>` — Environment variables and working directory
- `<axl/axl-time.h>` — Wall-clock time and monotonic timestamps
- `<axl/axl-nvstore.h>` — Portable NVRAM key-value storage
- `<axl/axl-boot.h>` — Boot-option management (Boot####/BootOrder/BootNext/BootCurrent)
- `<axl/axl-port.h>` — x86 I/O port access (`in`/`out`)
- `<axl/axl-driver.h>` — Driver binding and lifecycle
- `<axl/axl-image.h>` — Executable-image lifecycle (load/start/unload)
- `<axl/axl-mem-phys.h>` — Physical-memory map/unmap + one-shot read/write
- `<axl/axl-watchdog.h>` — Boot-services watchdog control
- `<axl/axl-rng.h>` — Cryptographic random bytes
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

Portable key-value storage backed by firmware variables, organized
by namespace. Built-in namespaces are `"global"` (spec UEFI Global
Variable GUID, e.g. SecureBoot, BootOrder, Boot####) and `"app"`
(per-app GUID for application settings).

```c
// Read
uint8_t secure_boot;
size_t sz = sizeof(secure_boot);
if (axl_nvstore_get("global", "SecureBoot", &secure_boot, &sz) == 0) {
    axl_printf("SecureBoot: %s\n", secure_boot ? "on" : "off");
}

// Write
axl_nvstore_set("app", "last-run", timestamp, timestamp_len,
                AXL_NV_PERSISTENT | AXL_NV_BOOT);
```

For variable-length values (NV strings, OEM blobs of unknown size),
`axl_nvstore_get_alloc` does the probe-allocate-read dance for you
and hands back a heap buffer the caller frees with `axl_free`. The
buffer is allocated `needed + 1` bytes with the trailing byte zeroed,
so a string-shaped variable can be dereferenced as NUL-terminated
even if the wire payload omitted the NUL:

```c
void   *buf;
size_t  sz;
if (axl_nvstore_get_alloc("global", "PlatformLang", &buf, &sz) == 0) {
    axl_printf("PlatformLang = '%s' (%zu bytes)\n", (char *)buf, sz);
    axl_free(buf);
}
```

`get_alloc` returns -1 for missing variables (and for backends that
allow 0-byte values, succeeds with `sz == 0` and a 1-byte NUL
allocation; UEFI's `SetVariable(size=0)` means "delete" so the
empty path doesn't surface there).

#### Vendor Namespaces

Vendor variables (Dell/HPE/Lenovo OEM keys) plug in via namespace
registration so consumer call sites stay UEFI-free — they reference
namespaces by name only. The backend token is opaque (a `const
AxlGuid *` on UEFI; on a future Linux backend it could be a path
prefix):

```c
extern const AxlGuid AXL_DELL_VENDOR_GUID;  // declared per-vendor
axl_nvstore_register_namespace("dell", &AXL_DELL_VENDOR_GUID);
axl_nvstore_get("dell", "SystemId", buf, &sz);
```

Other operations: `axl_nvstore_delete`, `axl_nvstore_iter` (walk
all keys in a namespace), `axl_nvstore_get_attrs` (read AXL_NV_*
flags without reading the value).

### Boot Options

Typed wrappers over the `Boot####`/`BootOrder`/`BootNext`/`BootCurrent`
firmware-variable family. The `EFI_LOAD_OPTION` wire codec stays
internal to AxlBoot — consumers operate on `AxlBootOption` structs:

```c
AxlBootOption opt;
if (axl_boot_option_get(0x0001, &opt) == 0) {
    axl_printf("Boot0001: %s\n  path: %s\n",
               opt.description, opt.device_path ?: "(unknown)");
    axl_boot_option_free(&opt);
}

uint16_t *order;
size_t    n;
if (axl_boot_order_get(&order, &n) == 0) {
    for (size_t i = 0; i < n; i++) {
        axl_printf("  %zu: Boot%04X\n", i, order[i]);
    }
    axl_free(order);
}
```

Set/delete options (`_option_set`, `_option_delete`), reorder boot
sequence (`_order_set`), or arm a one-shot (`_next_set` / `_next_clear`).
Encoding device paths to/from text uses the firmware's
`EFI_DEVICE_PATH_TO_TEXT_PROTOCOL` / `_FROM_TEXT_PROTOCOL` —
`_set` returns -1 if the from-text protocol isn't published.

### x86 I/O Ports

Public wrappers around `in`/`out` for legacy hardware that hasn't
moved to MMIO (CMOS, SuperIO, IPMI KCS, port-based ACPI PM blocks):

```c
#if defined(__x86_64__) || defined(__i386__)
uint8_t v = axl_io_port_read8(0x70);
axl_io_port_write8(0x71, v | 0x80);
#endif
```

Build-gated to x86 — calls compile out on AArch64, so wrong-arch
usage surfaces as a link error rather than a silent runtime no-op.
8/16/32-bit variants for read and write.

### Physical-Memory Access

For tools that scan ROM regions, peek at MMIO control registers,
or search firmware tables. The `_map`/`_unmap` pair is the held
abstraction; one-shot `_read{8,16,32,64}` / `_write{8,16,32,64}`
helpers cover the typical "I just want one byte" case without
boilerplate. UEFI is identity-mapped so map is effectively a
no-op; the abstraction exists for portability — a future Linux
backend would `mmap("/dev/mem")` on the way in.

```c
// Held mapping over multiple accesses.
void *va;
if (axl_mem_phys_map(0xFEE00000, 4096, &va) == 0) {
    uint32_t apic_id = *(volatile uint32_t *)((uint8_t *)va + 0x20);
    axl_mem_phys_unmap(va, 4096);
}

// One-shot read.
uint32_t signature;
axl_mem_phys_read32(0xE0000, &signature);
```

`axl_mem_phys_search` does a byte-by-byte scan for a needle
within a mapped region — useful for finding signatures inside
firmware blobs.

### Watchdog

UEFI starts every loaded image with a 5-minute boot-services
watchdog (UEFI 2.11 §7.5). Long-running diagnostics get killed
without warning unless they take action:

```c
// Disable entirely (typical for diagnostics that exceed 5 min).
axl_watchdog_disarm();

// Or extend without disabling protection.
axl_watchdog_set(900);  // 15 minutes
// ... long-running work ...
axl_watchdog_pet();     // re-arm to the same window
```

### Random Bytes

Thin wrapper over `EFI_RNG_PROTOCOL` (UEFI 2.11 §37.5). The
protocol is published by most modern firmware on platforms with
an entropy source (RDRAND on x86, an SBSA TRNG on aa64). Returns
-1 if the protocol isn't installed — consumers that need a
deterministic fallback layer their own.

```c
uint8_t nonce[16];
if (axl_rng_bytes(nonce, sizeof(nonce)) != 0) {
    // RNG not available — bail or fall back
}
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

### Image Lifecycle

For loading and running arbitrary EFI images (not DXE drivers),
use `axl_image_*`:

```c
AxlImage *img;
if (axl_image_load("fs0:\\boot\\hello.efi", &img) == 0) {
    int exit_code = 0;
    axl_image_start(img, &exit_code);
    axl_image_unload(img);
}
```

The handle is opaque — `EFI_HANDLE` and `EFI_LOADED_IMAGE_PROTOCOL`
never cross the public API. `axl_image_*` is a thin wrapper over
`axl_driver_*` (which already handles path-to-device-path
construction and the device-path / buffer load fallback); the only
distinct piece is `axl_image_start`, which captures the image's
exit status (`axl_driver_start` discards it because drivers aren't
expected to exit cleanly). Forward slashes in the path are
normalized to backslashes.

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

### Command-Line Parsing

CLI parsing moved to **AxlArgs** (`<axl/axl-args.h>`) — see the
*Command-Line Parsing (AxlArgs)* section below. AxlConfig stays
focused on the live property-bag use case (HTTP client/server
settings, future modules with tunable runtime properties).

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

## Command-Line Parsing (AxlArgs)

Declarative CLI parser — the tool declares a static `AxlArgsNode`
tree, calls `axl_args_run` from `main`, and the framework parses
argv, validates types and bounds, generates `--help`, and dispatches
to the matching leaf handler.

Header: `<axl/axl-args.h>`.

### One node type, three shapes

A single recursive node type (`AxlArgsNode`) describes the program
root, every inner branch ("category"), and every leaf verb. A node is
exactly one of:

- **Leaf** — `handler` set, `verbs` NULL. Optionally has
  `positionals`. Handler runs once parsing completes at this level.
- **Branch** — `verbs` set (NULL-terminated array of child nodes),
  `handler` NULL. Positionals MUST be NULL (the first non-flag
  argument is the verb name).
- **Single-handler app** — root happens to be a leaf (no verbs).
  The whole tool is one shape.

A node with both, or neither, is a configuration error and the
parser exits non-zero before invoking anything.

### Single-handler tool

```c
static int do_run(AxlArgs *a) {
    const char *path = axl_args_get_string(a, "path");
    return process(path);
}

int main(int argc, char **argv) {
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name = "mytool", .help = "Process a file",
        .positionals = (AxlArgDesc[]){
            { .name = "path", .type = AXL_ARG_STRING, .required = true,
              .help = "Input file" },
            {0}
        },
        .handler = do_run,
    });
}
```

### Multi-verb tool

```c
static const AxlArgsNode verbs[] = {
    { .name = "show", .handler = do_show, .positionals = slot_pos,
      .help = "Decoded fields for one slot" },
    { .name = "list", .handler = do_list,
      .help = "List populated slots" },
    {0}
};

int main(int argc, char **argv) {
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name = "memspd", .help = "Read JEDEC SPD content",
        .flags = flags, .verbs = verbs,
    });
}
```

### Argument types

`AxlArgDesc.type` selects the parser. Numeric types
(`AXL_ARG_U8`..`AXL_ARG_S64`) accept optional `min` / `max` bounds
and a `base` (0 = auto-detect, 10, or 16). String types get one
more knob:

- `AXL_ARG_BOOL` — presence flag, no value
- `AXL_ARG_STRING` — unconstrained string
- `AXL_ARG_MULTI` — repeatable string (variadic positional, or
  repeatable flag); accumulates into `axl_args_get_multi`
- `AXL_ARG_U8` / `AXL_ARG_U16` / `AXL_ARG_U32` / `AXL_ARG_U64` /
  `AXL_ARG_S64` — typed integers with bounds
- `AXL_ARG_CHOICE` — string restricted to a caller-supplied set:

```c
static const char *const fields[] = {
    "noHdds", "riserCfg", "delRiser", NULL
};
static const AxlArgDesc field_pos[] = {
    { .name = "field", .type = AXL_ARG_CHOICE, .required = false,
      .choices = fields,
      .default_value = "noHdds",
      .help = "field selector" },
    {0}
};
```

The framework rejects values not in `choices` with a
breadcrumb-prefixed error matching the out-of-range numeric format,
and lists the accepted values as `<noHdds|riserCfg|delRiser>` in
`--help` output. Comparison is case-sensitive by default. Setting
`choices` to NULL or an empty array degrades to `AXL_ARG_STRING`
(unconstrained); useful when the caller wants `<a|b|c>` help text
but custom validation in the handler.

For case-insensitive match — useful when migrating CLIs that
already accept mixed-case variants — set `.choices_case_insensitive
= true`:

```c
{ .name = "field", .type = AXL_ARG_CHOICE,
  .choices = fields,
  .choices_case_insensitive = true,
  .help = "field selector" }
```

`--help` then renders `<noHdds|riserCfg|delRiser> (case-insensitive)`
so users know `dd_cfg` and `DD_CFG` both work. The value the
handler sees retains the user's original casing — only validation
folds case. ASCII-only fold (per `axl_strcasecmp`); non-ASCII bytes
compare byte-equal.

### Nested verbs (`<top> <category> <verb>`)

```c
static const AxlArgsNode bios_verbs[] = {
    { .name = "test", .handler = bios_test, .help = "Run BIOS self-test" },
    { .name = "pci",  .handler = bios_pci,  .help = "List BIOS-PCI map" },
    {0}
};

static const AxlArgsNode top_verbs[] = {
    { .name = "bios", .verbs = bios_verbs,
      .help = "BIOS / SMBIOS subcommands" },
    { .name = "load", .handler = do_load, .positionals = load_args,
      .help = "Load and run a UEFI image" },
    {0}
};

int main(int argc, char **argv) {
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name = "do", .help = "Hardware diagnostic CLI",
        .flags = root_flags,
        .verbs = top_verbs,
    });
}
```

`do bios test` invokes the leaf with the breadcrumb in scope; if
the user types `do bios flarble`, the error reads
`do bios: unknown verb 'flarble'`. `do bios --help` recurses into
the bios subtree's auto-generated help.

### Branch with a default handler

A node can set BOTH `verbs` and `handler` — the handler runs only
when no sub-verb is supplied. Useful for the `do bios` → "print
summary" pattern where a category has subverbs but also wants a
default action:

```c
static const AxlArgsNode bios_verbs[] = {
    { .name = "info", .handler = bios_info, .help = "Type 0 summary" },
    { .name = "test", .handler = bios_test, .help = "Walk every record" },
    {0}
};

static const AxlArgsNode bios_node = {
    .name    = "bios",
    .help    = "BIOS / SMBIOS subcommands",
    .verbs   = bios_verbs,
    .handler = bios_info,    // fires on 'do bios' with no sub-verb
};
```

Dispatch is unambiguous: a verb argument that matches a child
recurses into it; a verb argument that matches none errors as
`do bios: unknown verb 'flarble'` (the handler is **not** a
catch-all); no verb argument at all invokes the handler with the
branch's parsed flags. Branch+handler nodes still cannot have
positionals — the first non-flag is structurally the verb name.

In `do bios --help` output, the verb whose handler matches the
default is annotated `(default)` so users see which sub-verb the
no-arg form is equivalent to.

### Parent-flag visibility

Flags declared on a parent node are visible to descendant handlers
via the same accessors. A `--verbose` declared on the root is
readable from a leaf two levels deep:

```c
static int bios_test(AxlArgs *a) {
    bool verbose = axl_args_get_bool(a, "verbose");   // root flag
    uint8_t slot = (uint8_t)axl_args_get_uint(a, "slot");  // leaf positional
    /* ... */
}
```

Same for `axl_args_user_data` — descendants inherit the nearest
non-NULL value walking up the chain.

### Error attribution

Errors are prefixed with the full breadcrumb path so users know
exactly which level rejected their input:

```
do bios test: 'foo' for --slot is not a valid integer
do pci: unknown verb 'flarble'
do: unknown flag --verbosee
```

### Lifetime

`AxlArgs` and accessor return values live until the leaf handler
returns. String values point into argv (program-lifetime); copy
numeric values, copy variadic-positional pointers if you need them
past handler return. Never call `axl_args_get_*` from a loop
callback that fires after the handler returns — extract everything
into local state inside the handler first.

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
