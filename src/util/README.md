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
extern const AxlGuid AXL_OEM_VENDOR_GUID;  // declared per-vendor
axl_nvstore_register_namespace("oem", &AXL_OEM_VENDOR_GUID);
axl_nvstore_get("oem", "AssetTag", buf, &sz);
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

For long-running services (cross-binary marshalling, structured
setup/teardown, foreground or driver-tick deployment), see
[AxlService](../service/README.md) — the lifecycle wrapper over
AxlLoop that composes axl-driver + axl-config + axl-loop.

### Driver Discovery

Where `axl-driver.h` *authors* and drives the driver lifecycle,
`<axl/axl-driver-info.h>` is the read-only **discovery** side — the
UEFI Shell `drivers` / "is this controller bound?" views as an API.
The recurring real-world question is "the network driver is on the
box, but it's not bound and I can't find it":

```c
AxlDriverInfo drivers[128];
size_t n = 0;
axl_driver_list_loaded(drivers, 128, &n);   // every DriverBinding driver
for (size_t i = 0; i < n; i++) {
    axl_printf("%-40s v%u  %u dev%s%s\n",
               drivers[i].name, drivers[i].version, drivers[i].num_devices,
               drivers[i].num_devices == 1 ? "" : "s",
               drivers[i].is_network ? "  [network]" : "");
}

// Is a specific PCI function bound, and by which driver?
AxlPciAddr nic = { .seg = 0, .bus = 2, .dev = 0, .func = 0 };
bool bound = false;
char drv[64];
axl_pci_driver_bound(nic, &bound, drv, sizeof drv);

// Bind a specific driver to a specific unbound controller.
AxlHandle controller = NULL;
axl_pci_to_handle(nic, &controller);
axl_driver_bind(controller, drivers[i].handle);   // NULL driver = any
```

`axl_driver_list_loaded` reads each driver's ComponentName2 name,
version, and managed-device count; `is_network` is true when the
driver manages a network controller, its name looks network-ish, or
(when idle) it claims an unbound network-class NIC. `axl_handle_name`
names any handle (ComponentName2, else device-path text).
`axl_pci_to_handle` maps a `AxlPciAddr` to its controller handle via
`EFI_PCI_IO_PROTOCOL.GetLocation`. The bare `connect` / `disconnect`
verbs already live in `axl-driver.h`
(`axl_driver_connect_handle` / `_disconnect_handle`).

For the **Devices tab** and the per-NIC **protocol-stack view** ("the
drivers are present and the NIC is bound, but there's still no network
— where is the stack broken?") the header adds four generic
enumeration primitives, all sharing the fixed-buffer truncation
contract of `axl_driver_list_loaded` (pass `out == NULL` to count,
`*count` is the full total even when it exceeds `cap`):

```c
// Every handle (the shell `dh` list), or only those exposing a protocol.
axl_handle_list(NULL, handles, cap, &n);          // all handles
axl_handle_list(&snp_guid, handles, cap, &n);     // by-GUID subset

// The reverse of axl_protocol_enumerate: the GUIDs a handle exposes.
axl_handle_protocols(nic, guids, cap, &n);
for (size_t i = 0; i < n && i < cap; i++) {
    char name[24];
    if (axl_net_protocol_name(&guids[i], name, sizeof name) == AXL_OK)
        axl_printf("  %s\n", name);               // "Ip4Config2", ...
}

// Who manages this controller (BY_DRIVER); count 0 == unmanaged.
axl_handle_drivers(controller, drivers, cap, &n);

// The child controllers it produced (BY_CHILD_CONTROLLER) — for the
// instance-level stack and the `devtree` descent.
axl_handle_children(controller, kids, cap, &n);

// devtree also walks upward — the parent(s) that produced a controller.
axl_handle_parents(controller, parents, cap, &n);   // count 0 == a root
```

`axl_net_protocol_name` names just the networking stack (and rejects
non-net GUIDs, so a caller can decide "is this a net protocol?");
`axl_protocol_guid_name` is the broader Devices-tab namer — it consults
the net table first, then the common device / driver / bus / console
protocols (DevicePath, LoadedImage, DriverBinding, ComponentName2,
SimpleFileSystem, BlockIo, DiskIo, PciIo, GraphicsOutput, SerialIo,
UsbIo, NvmExpressPassThru, AtaPassThru, …), falling back to AXL_ERR so
the caller formats the raw GUID for anything unrecognised.

Where the stack lands is platform-dependent: on the QEMU/OVMF test
platform the per-controller config protocols (`Ip4Config2`) sit on the
**NIC/SNP controller handle itself**, while `ManagedNetwork` and the
upper service-binding *instances* live on its **child** handles — so a
complete stack view lists the controller's protocols *and* walks
`axl_handle_children`.

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

For the common "launch a blocking foreground app and get its exit
code" case, `axl_image_run` does load + (optional) args + start +
unload in one call:

```c
int exit_code = 0;
axl_image_run("fs0:\\tools\\diag.efi", "-v --quick", &exit_code);
// blocks until diag.efi returns; `args` is encoded to UCS-2 LoadOptions
```

`StartImage` blocks — the launched app owns the foreground until it
returns. This works for *any* blocking UEFI app (a diagnostic tool, a
vendor setup app, a recovery menu). To host a real **UEFI Shell**
specifically, `axl_shell_launch` (`<axl/axl-shell.h>`) is the thin
Shell wrapper: it locates `Shell.efi` and runs it with `-nostartup`
(so a child Shell launched from `startup.nsh` doesn't recurse). Pair
either with `AxlConsoleMirror` (`<axl/axl-console-mirror.h>`) to mirror
the launched app's console to a remote terminal.

### Image Signature Inspection

For "is this PE file signed and does its signature validate?"
checks without committing to launching the image, use
`<axl/axl-image-verify.h>`:

```c
AxlImageSignatureInfo info = {0};
if (axl_image_verify_signature("fs0:\\boot.efi",
                               /* consult_db = */ true,
                               &info) == 0) {
    if (!info.has_signature) {
        axl_print("UNSIGNED\n");
    } else if (info.consulted_db && !info.signature_valid) {
        axl_print("SIGNATURE INVALID against current Secure Boot db\n");
    } else {
        axl_print("SIGNED%s by '%s' (issued by '%s')\n",
                  info.consulted_db ? " (db-validated)" : " (presence only)",
                  info.subject_cn != NULL ? info.subject_cn : "(unknown)",
                  info.issuer_cn  != NULL ? info.issuer_cn  : "(unknown)");
    }
    axl_image_signature_info_free(&info);
}
```

The presence axis (`has_signature`) is a pure file-bytes parse of
the PE Certificate Table — no firmware dependencies. The validity
axis (`signature_valid` + `consulted_db`) opts into a firmware
dry-run via `LoadImage(SourceBuffer)` + immediate `UnloadImage`,
which fires `EFI_SECURITY2_ARCH_PROTOCOL` callbacks (audit logs,
PCR measurement, `dbx` notifications) as a side effect — pass
`consult_db = false` when those side effects matter. The
`subject_cn` / `issuer_cn` fields populate from the first
certificate in the PKCS#7 SignedData bundle via an in-tree
DER walker; they're best-effort diagnostic strings (the formal
way to identify the Authenticode signer is via SignerInfo's
IssuerAndSerial — out of scope for diagnostic CN output).

### Protocol Registry

#### What UEFI Means by "Protocol"

A UEFI **protocol** is *not* a wire protocol or a network spec — it's
the closest thing UEFI has to an object or a vtable. Concretely, a
protocol is:

- a **C struct of function pointers** (and sometimes inline state),
- identified by a **128-bit GUID**,
- **installed on a handle** (a firmware-allocated opaque token that
  represents some logical entity — a disk, a network port, a driver
  image, a service endpoint, etc.).

Consumers find a protocol by GUID via the `LocateProtocol` or
`LocateHandleBuffer` Boot Services calls; the firmware returns the
struct pointer; the consumer calls the function pointers it carries.

Mental-model translation if you come from elsewhere:

- **Java / C# / Swift**: a UEFI protocol is roughly an *interface*
  bound to a specific instance — except identity is a GUID instead
  of a class type, and instances are handles instead of objects.
- **COM**: very similar to a COM interface — IID-keyed vtable on a
  handle. UEFI's design lineage is COM-via-IntelBIOS.
- **POSIX**: there's no clean parallel. The closest analog is "a
  device-driver `struct file_operations` registered in a kobject
  hierarchy keyed by a UUID instead of a path."

The naming is awkward and we're stuck with it because that's what
the UEFI spec writes everywhere. AXL's protocol registry is a
name-keyed wrapper over this UEFI-native concept: instead of
shipping a GUID literal at every call site, consumers pass a string
name and the registry resolves it. Internally it still calls
`InstallProtocolInterface` / `LocateProtocol` — there is no extra
runtime cost, just less boilerplate and fewer GUIDs to copy-paste.

#### Using the Registry

Built-in well-known names cover the spec-defined protocols a
portable consumer typically reaches for: `"smbios"`, `"shell"`,
`"simple-network"`, `"simple-fs"`, `"device-path"`, `"loaded-image"`,
`"ram-disk"`, the IPv4 networking family (`"tcp4"`, `"tcp4-sb"`,
`"ip4"`, `"ip4-config2"`, `"dhcp4"`, `"dhcp4-sb"`, `"dns4"`,
`"dns4-sb"`), and `"tcg2"`.

```c
// Find a protocol (consumer side)
EFI_SMBIOS_PROTOCOL *smbios;
if (axl_protocol_find("smbios", (void **)&smbios) == AXL_OK) {
    // ...
}

// Enumerate all handles publishing a protocol
void   **handles;
size_t   count;
if (axl_protocol_enumerate("simple-fs", &handles, &count) == AXL_OK) {
    for (size_t i = 0; i < count; i++) { /* ... */ }
    axl_free(handles);
}
```

#### Custom Protocol Names

Drivers can publish their own protocols under a project-defined name.
By default `axl_protocol_register("my-protocol", &iface, &handle)`
synthesizes a deterministic GUID from the name string (FNV-1a).
That works for single-image use, but the GUID is unstable across
typos and not directly usable for cross-image discovery via raw
`LocateProtocol`. Pin a published vendor GUID once at startup:

```c
static const AxlGuid kMySvcGuid =
    AXL_GUID(0xdead0001, 0xbeef, 0xcafe,
             0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0);

// In DriverEntry
axl_protocol_register_name("my-protocol", &kMySvcGuid);
axl_protocol_register("my-protocol", &mInterface, &mHandle);
```

External consumers can either use `axl_protocol_find("my-protocol",
…)` (after a matching `register_name` of their own — names are
per-image) or `LocateProtocol(&kMySvcGuid, …)` directly against the
published GUID. `register_name` is idempotent for the same
`(name, guid)` pair, refuses to shadow built-in well-known names,
and returns `AXL_ERR` if the name is already pinned to a different
GUID.

#### Driver-Image Lifecycle

Registered protocols are NOT auto-released when a driver image is
unloaded — the AXL protocol registry never owned the install. Walk
your protocols in `axl_driver_set_unload`'s callback:

```c
static EFI_STATUS EFIAPI MyUnload(EFI_HANDLE image) {
    if (mHandle != NULL) {
        axl_protocol_unregister(mHandle, "my-protocol", &mInterface);
    }
    return EFI_SUCCESS;
}
```

`sdk/examples/driver.c` is the canonical reference. Forgetting to
unregister leaves dangling handle entries pointing at freed driver
memory; subsequent `LocateProtocol` calls hand consumers a stale
vtable and the next dispatch faults.

#### TPL Contract

All entry points (`_register`, `_register_name`, `_register_multiple`,
`_find`, `_enumerate`, `_unregister`) bottom out in UEFI Boot
Services calls (`InstallProtocolInterface`, `LocateProtocol`,
`LocateHandleBuffer`, `UninstallProtocolInterface`) plus
`axl_malloc`, all of which require TPL ≤ `TPL_NOTIFY` per UEFI 2.11
§7.3. Callers running from a timer handler at `TPL_NOTIFY` are
fine; callers at `TPL_HIGH_LEVEL` must lower first. Callbacks
dispatched through `AxlLoop` (defer-drain, pubsub delivery, source
handlers) all run at `TPL_APPLICATION` — the loop calls
`WaitForEvent`, which mandates that level — so consumers writing
protocol-event-driven code don't need to worry about TPL inside
handlers.

#### Protocol-Event Subscriptions

For protocols that need to publish events ("client connected",
"upload complete"), reuse `axl_pubsub_*` from `<axl/axl-pubsub.h>`
rather than rolling a protocol-internal callback list. Topics are
string-keyed; multiple consumers can subscribe; delivery is
deferred via the loop's defer queue, so handlers always fire at
`TPL_APPLICATION`. Pubsub is `AxlLoop`-scoped: subscribers must run
on the same loop instance as the publisher to receive events.

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
`EFI_SHELL_PARAMETERS_PROTOCOL` probe — some OEM firmware sometimes
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

### Standard option groups (group injection)

Networked tools repeat the same NIC / local-IP / port
descriptors over and over. `axl_config_descs_net` emits the
canonical entries into a consumer-owned accumulator, with
descriptor offsets shifted by the consumer's embedded
`AxlNetOpts` sub-struct offset:

```c
typedef struct {
    AxlNetOpts net;       // the standard sub-struct
    const char *url;
    bool        read_only;
} MountOpts;

static const AxlConfigDesc mount_consumer_descs[] = {
    { "url",       AXL_CFG_STRING, "",      "Server URL",
      offsetof(MountOpts, url),       sizeof(((MountOpts*)0)->url) },
    { "read-only", AXL_CFG_BOOL,   "false", "Mount read-only",
      offsetof(MountOpts, read_only), sizeof(bool), 'r' },
    { 0 }
};

static AxlConfigDesc mount_descs[16];
void mount_descs_init(void) {
    size_t n = axl_config_descs_net(mount_descs, ARRAY_SIZE(mount_descs),
                                    AXL_NET_OPT_SERVER,
                                    offsetof(MountOpts, net));
    n += axl_config_descs_append(mount_descs + n,
                                 ARRAY_SIZE(mount_descs) - n - 1,
                                 mount_consumer_descs);
    mount_descs[n] = (AxlConfigDesc){ 0 };
}
```

`AXL_NET_OPT_CLIENT` / `_SERVER` presets cover the common cases;
finer-grained bitmasks (`AXL_NET_OPT_NIC | AXL_NET_OPT_PORT`)
also work. `AXL_NET_OPT_SOURCE_IP` and `AXL_NET_OPT_LISTEN_IP`
both target the same `local_ip` field (same `bind(2)` syscall);
they differ only in CLI vocabulary — pick whichever matches your
tool's role. The emitted descriptors preserve `short_name` /
`choices` and route through AxlConfig's auto-apply machinery
exactly like the consumer's own table. See `<axl/axl-net-opts.h>`
for the option-bag types and the matching `axl_net_init_from_opts`
bring-up helper.

The companion `axl_config_descs_append` copies a consumer-owned
descriptor fragment (terminated by `{0}`) onto the accumulator;
the caller writes the final `{0}` terminator once, after all
fragments have been appended.

### Parent Inheritance

Create a child config that inherits defaults from a parent:

```c
AxlConfig *defaults = axl_config_new(opts);
axl_config_set(defaults, "port", "8080");

AxlConfig *override = axl_config_new_with_parent(opts, defaults);
// override inherits "port"="8080" until explicitly set
```

### Free-form config files (AxlConfigFile)

`AxlConfig` is descriptor-bound — it validates each key against a fixed
table and *rejects* unknown keys. For the opposite case — a free-form
`key=value` file whose keys aren't known at compile time (a module
config where features invent their own `prefix.key` names) —
`<axl/axl-config-file.h>` `AxlConfigFile` parses the file into a flat
string map with typed getters that fall back to a caller default:

```c
AXL_AUTOPTR(AxlConfigFile) cf = axl_config_file_load("FS0:\\softbmc.cfg");
uint64_t timeout = axl_config_file_get_uint(cf, "session_timeout", 900);
const char *mode = axl_config_file_get(cf, "mode", "handoff");
bool        dbg  = axl_config_file_get_bool(cf, "log.debug", false);
axl_config_file_set(cf, "session_timeout", "1800");
axl_config_file_save(cf, "FS0:\\softbmc.cfg");
```

A missing file yields an empty map (not an error), so every lookup
returns its default. The format is ASCII `key=value`, one per line, with
`#` comments, blank lines ignored, and values trimmed of surrounding
whitespace. The `prefix.key` dot is just a naming convention — the map is
flat; the caller joins the prefix.

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
        .name = "mytool", .help = "Hardware diagnostic CLI",
        .flags = root_flags,
        .verbs = top_verbs,
    });
}
```

`mytool bios test` invokes the leaf with the breadcrumb in scope; if
the user types `mytool bios flarble`, the error reads
`mytool bios: unknown verb 'flarble'`. `mytool bios --help` recurses into
the bios subtree's auto-generated help.

### Branch with a default handler

A node can set BOTH `verbs` and `handler` — the handler runs only
when no sub-verb is supplied. Useful for the `mytool bios` → "print
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
    .handler = bios_info,    // fires on 'mytool bios' with no sub-verb
};
```

Dispatch is unambiguous: a verb argument that matches a child
recurses into it; a verb argument that matches none errors as
`mytool bios: unknown verb 'flarble'` (the handler is **not** a
catch-all); no verb argument at all invokes the handler with the
branch's parsed flags. Branch+handler nodes still cannot have
positionals — the first non-flag is structurally the verb name.

In `mytool bios --help` output, the verb whose handler matches the
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
mytool bios test: 'foo' for --slot is not a valid integer
mytool pci: unknown verb 'flarble'
mytool: unknown flag --verbosee
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

## Shared memory (AxlShm)

`<axl/axl-shm.h>` is the UEFI analog of POSIX `shm_open` + `mmap` (or
System V `shmget`/`shmat`): a **named, fixed-size memory region that
outlives the app that created it** and is reachable by any later app in
the same boot. UEFI is one flat, identity-mapped address space, so there
is no per-process mapping — `axl_shm_open` returns a pointer that *is* the
region (map/attach collapse to nothing). The region lives in boot-services
RAM, so it is volatile (gone at reboot / `ExitBootServices`) — not NVRAM,
no flash wear, and its capacity is system RAM rather than the tiny
firmware variable store.

```c
size_t sz;
// First app: create + write.
uint8_t *r = axl_shm_open("myapp/scratch", 4096, AXL_SHM_CREATE, &sz);
r[0] = 0x42;

// A later app in the same boot: open + read the same region.
uint8_t *r2 = axl_shm_open("myapp/scratch", 0, 0, &sz);   // r2[0] == 0x42

axl_shm_unlink("myapp/scratch");        // destroy when done
```

The name is hashed to a GUID (`axl_guid_v5`) and the region published as a
UEFI protocol, so it resolves across images with no shared handle. The
trick that makes it survive an image unload: the region is a *data-only*
pool allocation (it never holds function pointers, which would dangle once
the creating image is gone) from `EfiBootServicesData`, which the firmware
does not reclaim on unload. Names are one global namespace (like POSIX
`/name`) — prefix yours. Single-threaded: no locking; in the shell apps
run one at a time.

## Clipboard

UEFI has no system clipboard, so an editor that wants copy/paste needs
one. `AxlClipboard` is an owned byte buffer with an optional MIME type:
`axl_clipboard_set` copies bytes in (replacing the previous contents),
`axl_clipboard_get` borrows a pointer to them (valid until the next
set/clear), and `axl_clipboard_clear` empties it. Byte-oriented and
content-agnostic — store UTF-8 text, a UTF-16 region, a serialized
selection, or an image, and tag it with a MIME type the paste side can
check.

```c
axl_clipboard_set("hello", 5, "text/plain;charset=utf-8");

size_t len;
const char *mime;
const void *data = axl_clipboard_get(&len, &mime);   // borrowed, len bytes
// ... paste data[0..len) ...

axl_clipboard_clear();
```

Header: `<axl/axl-clipboard.h>`

The clipboard is backed by an [AxlShm](#shared-memory-axlshm) segment
(`"axl/clipboard"`), so it is **cross-app within a boot**: copy in one app
and paste in another, no driver. The `clip` and `paste` tools are the
command-line front end (`some-tool | clip`, `paste > file`) — pbcopy /
pbpaste for UEFI.

## Synchronization primitives

AxlCompletion / AxlCancellable / axl_wait_* and the foundational
`AxlEvent` moved out of AxlUtil into the dedicated
[`src/event/`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event) module. See
[src/event/README.md](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) for the current documentation.
