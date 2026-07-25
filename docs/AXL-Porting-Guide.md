# AXL Porting Guide

How to port UEFI applications and drivers from EDK2 to the AXL SDK.
Covers the dependency audit across AximCode consumer projects, AXL's
protocol abstraction strategy, and step-by-step porting instructions.

Last updated: April 2026

---

## What AXL Already Covers

These EDK2 dependencies have AXL equivalents and apps can port today:

| EDK2 | AXL | Used by |
|------|-----|---------|
| AllocatePool/FreePool | axl_malloc/free | Everything |
| Print(L"...") | axl_printf | Everything |
| CopyMem/SetMem/ZeroMem | axl_memcpy/memset | Everything |
| ShellOpenFileByName/ReadFile/CloseFile | axl_fopen/fread/fclose | Hexdump, Grep, Find, MkRd |
| ShellGetFileInfo | axl_file_info | Hexdump, Grep, Find |
| FileHandleFindFirst/Next | axl_dir_open/read/close | Grep, Find |
| ShellCommandLineParse | axl_config_parse_args | All apps |
| AsciiStr*/StrCmp/StrLen | axl_str* / standard C | Everything |
| utf8 to/from ucs2 | axl_utf8_to_ucs2/ucs2_to_utf8 | Everything |
| GOP->Blt | axl_gfx_* | SoftBMC Splash, RemoteKVM |
| TCP4 protocol | axl_tcp_* | SoftBMC HttpServer, axl-webfs |
| HTTP client/server | axl_http_* | Fetch, axl-webfs serve command, SoftBMC |
| SMBIOS tables | axl_smbios_* | SysInfo, SoftBMC HwInfo |
| Shell env/cwd | axl_getenv/setenv/chdir | Shell integration |
| Driver load/connect | axl_driver_* | axl-webfs mount command |
| System reset | axl_reset | SoftBMC PowerControl |
| Map refresh | axl_map_refresh | axl-webfs mount command |
| Event loop + timers | axl_loop_* | SoftBMC, axl-webfs |
| Deferred work | axl_defer | SoftBMC event handling |
| Pub/sub events | axl_pubsub_* | SoftBMC module decoupling |
| Buffer pool | axl_buf_pool_* | SoftBMC VNC tiles |
| Async AP work | axl_async_* | SoftBMC firmware update |
| Async-op cancellation | axl_cancellable_* / axl_event_* / axl_wait_* | Anything with async I/O or interruptible polls (see `src/event/README.md`) |
| `connect -r` + DHCP at startup | `axl_net_auto_init` | Any app touching TCP/UDP/HTTP/DNS — replaces the shell-level driver bind + DHCP wait that EDK2 apps assumed had already happened |
| Driver self-load on first use | `axl_driver_ensure(GUID, "DriverName.efi")` | Tools that need a DXE driver (e.g. `MkRd` loads `RamDiskDxe.efi`); `axl_driver_locate` for search-only |
| Ctrl-C / SIGINT handling | `axl_signal_install` / `axl_interrupted` / `axl_exit` | Long-running apps that want graceful shutdown (auto by default; install a handler for custom cleanup) |
| `atexit`-style cleanup | `axl_atexit` | Apps with long-lived resources that should free on any exit path |
| Default singleton main loop | `axl_loop_default` | Apps + library code that share one loop; lazy-allocated, freed automatically on `_axl_cleanup` |
| Cooperative yield | `axl_yield` | Tight CPU loops that need to stay Ctrl-C responsive (no preemption in UEFI BSP) |
| File seek/tell/eof | axl_fseek/ftell/feof | General file I/O |
| mkdir/rmdir | axl_dir_mkdir/rmdir | General |
| File delete/rename | axl_file_delete/rename | General |
| String search | axl_strstr_len/strrstr | General |
| Prefix/suffix test | axl_str_has_prefix/suffix | General |
| String comparison | axl_strcmp0/strcasecmp/strncasecmp | General |
| String array ops | axl_strv_contains/equal | General |

---

## What's Missing — By Priority

### Tier 1: Needed by 2+ projects (high value)

| Missing API | Needed by | What it does | Effort |
|-------------|-----------|-------------|--------|
| UEFI variable access | SoftBMC Config, SysInfo | gRT->GetVariable/SetVariable for persistent config, Secure Boot info | Medium |
| Protocol locate/enumerate | SoftBMC, axl-webfs, SysInfo, NetInfo | gBS->LocateProtocol, LocateHandleBuffer, HandleProtocol | Medium |
| Device path helpers | axl-webfs mount command, MkRd | FileDevicePath, IsDevicePathEnd, NextDevicePathNode | Medium |
| UDP socket | SoftBMC (SNMP, Syslog) | Send/receive UDP datagrams | Medium |

### Tier 2: Needed by 1 project, critical for it

| Missing API | Needed by | What it does | Effort |
|-------------|-----------|-------------|--------|
| Protocol install/uninstall | axl-webfs axl-webfs-dxe driver | gBS->InstallMultipleProtocolInterfaces | Medium |
| EFI_FILE_PROTOCOL types | axl-webfs driver | Type definitions for the protocol the driver implements | Low |
| EFI_SIMPLE_FILE_SYSTEM_PROTOCOL | axl-webfs driver | The protocol the driver installs | Low |
| Physical memory | MkRd | AllocatePages/FreePages | Low |
| RAM disk protocol | MkRd, SoftBMC VirtualMedia | Register memory as a disk | Low |
| Network init (DHCP/IP config) | SoftBMC Network, axl-webfs NetworkLib | Full NIC init with DHCP | High |

### Tier 3: Nice to have / can work around

| Missing API | Needed by | Workaround |
|-------------|-----------|------------|
| PCI/ACPI/USB/TPM enumeration | SoftBMC HwInfo | Limit to SMBIOS-only |
| HII database (UEFI Setup) | SoftBMC UefiSetup | Replace with web-based config |
| Ctrl-C / break flag | Grep, Find | Drop or poll stdin |
| gRT->GetTime (wall clock) | SoftBMC, axl-webfs cache | axl_time_* (partial) |
| Spinlocks | SoftBMC RemoteKVM | Refactor for single-threaded event model |

---

## App-Level Porting Readiness

### uefi-devkit

| App | Status | Blocker | Effort |
|-----|--------|---------|--------|
| **Hexdump** | DONE | None | Done |
| **Fetch** | Ready | None — just Print to printf + AXL_APP | Low |
| **Grep** | Ready | None — has axl_dir_read, just needs Print cleanup | Low |
| **Find** | Ready | Replace ShellCommandLineParse with axl_args | Low |
| **SysInfo** | Almost | Needs UEFI variable access (gRT->GetVariable) | Medium |
| **MkRd** | Blocked | Needs RAM disk + physical memory + device paths | Very High |
| **NetInfo** | Blocked | Needs raw network protocols (SNP, IP4, ICMP) | Very High |

### axl-webfs

| Component | Status | Blocker | Effort |
|-----------|--------|---------|--------|
| **CmdServe** | Ready | Already uses AXL HTTP server, minor cleanup | Low |
| **CmdMount** | Blocked | Protocol locate + device paths + driver loading | High |
| **axl-webfs-dxe driver** | Blocked | Protocol install, FILE_PROTOCOL implementation | Very High |
| **FileTransferLib** | Blocked | SimpleFileSystem protocol discovery | High |
| **NetworkLib** | Blocked | SNP, DHCP4, IP4Config2, ServiceBinding | Very High |
| **JsonLib** | Ready | Minimal UEFI dependency (string ops only) | Low |

### SoftBMC

| Module | Status | Blocker | Effort |
|--------|--------|---------|--------|
| **Core logic** (JSON, crypto, strings) | Ready | None | Low |
| **HTTP/WebSocket server** | Ready | Event model needs axl_loop rework | Medium |
| **TCP utilities** | Ready | axl_tcp covers this | Low |
| **TLS/mbedtls** | Ready | Works with axl_tcp | Low |
| **Graphics/Splash** | Ready | axl_gfx covers GOP | Low |
| **RemoteKVM (VNC)** | Mostly ready | Needs spinlock rework | Medium |
| **Config** | Blocked | UEFI variable access | Medium |
| **Network init** | Blocked | DHCP, SimpleNetwork protocols | Very High |
| **File operations** | Ready | axl_io covers this | Low |
| **SMBIOS** | Partial | Read works, protocol lookup missing | Low |
| **Alert (SMTP, Webhook)** | Ready | TCP/HTTP based | Low |
| **Alert (SNMP, Syslog)** | Blocked | Needs UDP | Medium |
| **HwInfo (PCI, ACPI, USB, TPM)** | Blocked | Protocol enumeration | High |
| **UefiSetup (HII)** | Not portable | Replace with web-based config | N/A |
| **VirtualMedia** | Blocked | RAM disk protocol | High |
| **OsBoot** | Not portable | Firmware-specific | N/A |
| **UDP utilities** | Blocked | No axl_udp API | Medium |
| **Console wrapper** | Not portable | Simplify for serial | N/A |
| **Shell integration** | Not portable | Remove or externalize | N/A |

---

## Recommended Implementation Order

### Phase 1: Port the easy wins (unblocks 4 apps)

Fetch, Grep, Find, and axl-webfs serve command can all be ported today with
existing AXL APIs. Just mechanical rewrites (Print to printf, EDK2
types to C types, AXL_APP entry point).

### Phase 2: Non-volatile storage (unblocks SysInfo + SoftBMC Config) — DONE

```c
int axl_nvstore_get(const char *ns, const char *key, void *buf, size_t *size);
int axl_nvstore_set(const char *ns, const char *key, const void *buf, size_t size, uint32_t flags);
```

Platform-agnostic persistent storage. Namespaces: `"global"`, `"app"`.

### Phase 3: Protocol registry (unblocks axl-webfs mount command + driver) — DONE

```c
int axl_protocol_find(const char *name, void **interface);
int axl_protocol_enumerate(const char *name, void ***handles, size_t *count);
int axl_protocol_register(const char *name, void *interface, void **handle);
int axl_protocol_unregister(void *handle, const char *name, void *interface);
```

Platform-agnostic protocol discovery. Well-known names: `"smbios"`,
`"shell"`, `"simple-network"`, `"simple-fs"`, `"tcp4"`, etc.

### Phase 4: Network interface enumeration (unblocks NetInfo)

```c
int axl_net_list_interfaces(AxlNetInterface *out, size_t *count);
```

High-level NIC discovery — returns names, MACs, IPv4 config.

### Phase 5: UDP sockets (unblocks SoftBMC SNMP/Syslog)

```c
int  axl_udp_open(AxlUdp **sock, uint16_t local_port);
int  axl_udp_send(AxlUdp *sock, const char *host, uint16_t port, const void *data, size_t len);
int  axl_udp_recv(AxlUdp *sock, void *buf, size_t size, size_t *received);
void axl_udp_close(AxlUdp *sock);
```

### Phase 6: Network initialization (unblocks SoftBMC Network)

This is the hardest piece — DHCP, SimpleNetwork, IP4Config2. May be
better to assume the network is already configured (Shell or PXE did
it) and just discover the existing configuration.

---

## Projects That Will Never Fully Port

Some features are inherently UEFI-specific and can't be abstracted:

- **HII Setup Browser** (SoftBMC) — replace with web-based config
- **OS Boot management** (SoftBMC) — firmware-specific
- **PCI/ACPI/USB/TPM hardware enumeration** — protocol-specific
- **UEFI Shell dynamic commands** (SoftBMC) — Shell-specific
- **Console Extended Input** (SoftBMC) — replace with serial/TTY

These should be replaced with alternative implementations rather than
abstracted.

---

## ipmitool (uefi-ipmitool)

### Structure

- **Application** (`IpmiTool/`) — Shell app with subcommands (mc, chassis,
  raw, sdr, sensor, fru, sel, lan, sol, user, probe)
- **IpmiCommandLib** — IPMI command framing and response parsing
- **IpmiTransportLib** — Low-level transport (KCS via I/O ports, SSIF via
  SMBus/I2C)

### Dependencies

| Component | Status | Blocker |
|-----------|--------|---------|
| IpmiTool.c (entry) | Ready | None — AXL_APP |
| All Cmd*.c (subcommands) | Ready | Just Print to printf |
| IpmiCommandLib | Ready | Just memory/string ops |
| IpmiFormat.c | Ready | Just AsciiSPrint to snprintf |
| IpmiKcs.c (KCS transport) | Blocked | Needs I/O port access |
| IpmiSsif.c (SSIF transport) | Blocked | Needs I2C/SMBus protocol |
| IpmiDetect.c (BMC detection) | Partial | SMBIOS works, SMBus probe blocked |

### Unique missing APIs

| Missing | Used in | What it does |
|---------|---------|-------------|
| I/O port access (IoRead8/IoWrite8) | IpmiKcs.c | Direct x86 in/out instructions for KCS |
| I2C/SMBus protocol | IpmiSsif.c, CmdProbe.c | SMBus communication with BMC |

### Strategy: AxlIpmi module

Rather than porting ipmitool's transport layer as-is, AXL will absorb
IPMI as a first-class module (like SMBIOS). The consumer never sees
KCS registers or SMBus framing — just typed IPMI commands.

See "BMC Access Modules" section below for the full API design.

---

## BMC Access Modules (Planned)

AXL will provide two complementary modules for BMC access:

### AxlIpmi — Legacy BMC access (IPMI over KCS/SSIF)

**Header:** `axl/axl-ipmi.h`
**Source:** `src/ipmi/axl-ipmi.c`, `axl-ipmi-kcs.c`, `axl-ipmi-ssif.c`

Hides all transport complexity. BMC detection via SMBIOS Type 38
(IPMI Device Information) auto-selects KCS vs SSIF and discovers
the base address. The consumer never knows which transport is used.

```c
// Low-level: send raw IPMI command
int axl_ipmi_raw(
    uint8_t      netfn,       // network function
    uint8_t      cmd,         // command code
    const void  *req,         // request data (NULL OK)
    size_t       req_len,
    void        *resp,        // response buffer
    size_t      *resp_len     // [in/out] buffer size / actual size
);

// High-level convenience functions
int axl_ipmi_get_device_id(AxlIpmiDeviceId *info);
int axl_ipmi_get_sensor_reading(uint8_t sensor_num,
                                 AxlIpmiSensorReading *reading);
int axl_ipmi_get_sdr_entry(uint16_t record_id, void *buf,
                            size_t *len, uint16_t *next_id);
int axl_ipmi_get_sel_entry(uint16_t record_id, void *buf,
                            size_t *len, uint16_t *next_id);
int axl_ipmi_chassis_status(AxlIpmiChassisStatus *status);
int axl_ipmi_chassis_control(uint8_t action);
int axl_ipmi_get_fru_data(uint8_t fru_id, uint16_t offset,
                           void *buf, size_t len);
```

**Transport internals (hidden from consumer):**

- **KCS** — I/O port based (x86: `in`/`out` instructions at BMC base
  address, typically 0xCA2). Requires new backend function
  `axl_backend_io_read8/write8`. ARM uses MMIO instead.
- **SSIF** — SMBus/I2C based. Requires I2C protocol types in
  uefi-manifest.json5 and a backend wrapper.
- **Auto-detection** — SMBIOS Type 38 record specifies interface type
  (KCS=1, SSIF=4) and base address. `axl_smbios_find(38)` already works.

**Backend additions needed:**

```c
// I/O port access (x86 only, MMIO on ARM)
uint8_t  axl_backend_io_read8(uint16_t port);
void     axl_backend_io_write8(uint16_t port, uint8_t value);
```

SMBus access for the SSIF IPMI transport is no longer a backend hook
— it is provided by the `AxlSmbus` Platform Access Module. Use
`axl_smbus_new()` + `axl_smbus_read_block()` / `_write_block()` from
`<axl/axl-smbus.h>` instead. AxlSmbus auto-detects
`EFI_SMBUS_HC_PROTOCOL` with an `EFI_I2C_MASTER_PROTOCOL` fallback.

**ipmitool becomes a thin CLI wrapper:**

```c
#include <axl.h>

int main(int argc, char **argv) {
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    axl_config_parse_args(cfg, argc, argv);
    const char *subcmd = axl_config_pos(cfg, 0);

    if (axl_strcmp(subcmd, "mc") == 0) {
        AxlIpmiDeviceId id;
        axl_ipmi_get_device_id(&id);
        axl_printf("Device ID: 0x%02x, FW: %d.%02d\n",
                   id.device_id, id.fw_major, id.fw_minor);
    } else if (axl_strcmp(subcmd, "sensor") == 0) {
        // iterate SDR, read each sensor...
    }
    // ...
}
```

### AxlRedfish — Modern BMC access (Redfish over HTTPS/JSON)

**Header:** `axl/axl-redfish.h`
**Source:** `src/redfish/axl-redfish.c`

Built on top of AXL's existing HTTP client + JSON parser. Adds
Redfish session management (X-Auth-Token), standard URI patterns,
and typed result structures.

```c
// Session management
AxlRedfishSession *axl_redfish_connect(
    const char *host,      // BMC hostname or IP
    const char *username,
    const char *password
);
void axl_redfish_close(AxlRedfishSession *session);

// Generic resource access (any Redfish URI)
int axl_redfish_get(AxlRedfishSession *s, const char *uri,
                     AxlJsonDoc *doc);
int axl_redfish_patch(AxlRedfishSession *s, const char *uri,
                       const char *json_body);
int axl_redfish_post(AxlRedfishSession *s, const char *uri,
                      const char *json_body,
                      char *location, size_t loc_size);
int axl_redfish_delete(AxlRedfishSession *s, const char *uri);

// High-level convenience (typed results)
int axl_redfish_get_system_info(AxlRedfishSession *s,
                                 AxlRedfishSystemInfo *info);
int axl_redfish_get_thermal(AxlRedfishSession *s,
                             AxlRedfishThermal *thermal);
int axl_redfish_get_power(AxlRedfishSession *s,
                           AxlRedfishPower *power);
int axl_redfish_get_bmc_info(AxlRedfishSession *s,
                              AxlRedfishManagerInfo *info);
int axl_redfish_chassis_reset(AxlRedfishSession *s,
                               const char *reset_type);
int axl_redfish_get_event_log(AxlRedfishSession *s,
                               AxlRedfishLogEntry *entries,
                               size_t *count);
```

**No new backend support needed** — Redfish is pure HTTP+JSON+TLS,
all of which AXL already provides:

- `axl_http_client_new` / `axl_http_request` — HTTP transport
- `axl_json_parse` / `axl_json_get_string` — JSON parsing
- TLS via mbedtls (SoftBMC already integrates this)

**Redfish standard URIs:**

| URI | What it returns |
|-----|----------------|
| `/redfish/v1/` | Service root |
| `/redfish/v1/Systems/1` | System info (model, serial, power state) |
| `/redfish/v1/Chassis/1/Thermal` | Temperatures, fans |
| `/redfish/v1/Chassis/1/Power` | Power supplies, consumption |
| `/redfish/v1/Managers/1` | BMC info (firmware version, network) |
| `/redfish/v1/Systems/1/LogServices/SEL/Entries` | Event log |
| `/redfish/v1/SessionService/Sessions` | Session management |

**SoftBMC integration:** SoftBMC currently implements the Redfish
*server* side. AxlRedfish provides the *client* side — useful for
tools that query a real or simulated BMC.

### Comparison

| Feature | AxlIpmi | AxlRedfish |
|---------|---------|------------|
| Protocol | Binary over KCS/SSIF | HTTP+JSON over TCP/TLS |
| Latency | Microseconds (local bus) | Milliseconds (network) |
| Scope | Local BMC only | Local or remote BMC |
| Features | Sensors, SEL, FRU, chassis | Full system management |
| Complexity | Hardware transport | HTTP client |
| New code | Transport layer + backend | Thin wrapper over existing HTTP+JSON |
| TLS needed | No | Yes (for production) |

---

## Protocol Abstraction Strategy

### The Problem

In UEFI, almost everything is accessed through protocols. Want to read
a file? Locate `EFI_SHELL_PROTOCOL`. Want to draw pixels? Locate
`EFI_GRAPHICS_OUTPUT_PROTOCOL`. Want network? Locate
`EFI_TCP4_SERVICE_BINDING_PROTOCOL`, create a child handle, configure
it, connect...

This is the biggest barrier to writing UEFI apps — consumers shouldn't
need to know about GUIDs, handles, LocateProtocol, or service binding.

### AXL's Approach: Hide Protocols Behind Purpose-Built APIs

AXL already does this for several protocols:

| What the consumer writes | What AXL does behind the scenes |
|--------------------------|--------------------------------|
| `axl_fopen("file.txt", "r")` | Locates Shell protocol, calls OpenFileByName |
| `axl_gfx_fill_rect(...)` | Locates GOP protocol, calls Blt |
| `axl_tcp_connect(host, port, &sock)` | Locates TCP4 ServiceBinding, creates child, configures, connects |
| `axl_smbios_find(type)` | Locates SMBIOS protocol (or walks config table), iterates entries |
| `axl_getenv("PATH")` | Locates Shell protocol, calls GetEnv, converts UCS-2 to UTF-8 |
| `axl_printf("hello")` | Locates ConOut, converts UTF-8 to UCS-2, calls OutputString |

The pattern: the backend locates the protocol lazily on first use,
caches it in a static variable, and the consumer never sees a GUID,
handle, or EFI_STATUS.

### Three Categories of Protocol Usage

**Category 1: "I need a specific service"**

Examples: read a file, draw pixels, get an env var, query SMBIOS.

Strategy: **Purpose-built AXL API per service.** The consumer calls
`axl_gfx_*` or `axl_fopen` and the protocol is completely invisible.
This is what AXL does today. Each new service (UEFI variables, UDP
sockets, etc.) gets its own clean API.

```c
// Consumer code — no protocols visible:
char *val = axl_getenv("PATH");
AxlGfxInfo info; axl_gfx_get_info(&info);
int rc = axl_uefi_var_get("SecureBoot", &guid, buf, &size);
```

**Category 2: "I need to enumerate handles"**

Examples: list all mounted filesystems, find all NICs, discover
available block devices.

Strategy: **Higher-level query APIs** that return meaningful results,
not raw handles. Instead of exposing `LocateHandleBuffer` (which leaks
the handle/protocol model), provide task-specific queries:

```c
// Instead of:
//   gBS->LocateHandleBuffer(&gEfiSimpleFileSystemProtocolGuid, ...)
//   for each handle: gBS->OpenProtocol(...) → get volume label
//
// Provide:
size_t count;
AxlVolume *vols = axl_volume_list(&count);
for (size_t i = 0; i < count; i++) {
    printf("  %s: %s (%llu bytes)\n",
           vols[i].name, vols[i].label, vols[i].size);
}
axl_volume_list_free(vols);
```

The consumer gets names, paths, and sizes — not handles and GUIDs.

**Category 3: "I AM a protocol provider" (drivers)**

Examples: axl-webfs axl-webfs-dxe installs EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.

Strategy: **AXL provides types, helpers, and wrappers — but the driver
model is inherently visible.** A driver that installs a protocol IS the
protocol; there's no way to fully hide the UEFI driver model from its
author. But AXL can:

- Provide all protocol type definitions (via uefi-manifest.json5)
- Provide install/uninstall wrappers:
  ```c
  axl_protocol_install(&handle, &my_guid, &my_protocol_vtable);
  axl_protocol_uninstall(handle, &my_guid, &my_protocol_vtable);
  ```
- Provide helper macros for the boilerplate (signature validation,
  container-of derivation)
- Provide the CRT0 for driver entry points (`axl-cc --type driver`)

The driver author still understands the UEFI driver binding model,
but AXL handles the mechanical parts.

### Design Principle

> **For applications:** protocol access is invisible. AXL provides
> purpose-built APIs. Consumers never see GUIDs, handles, or
> LocateProtocol.
>
> **For drivers:** AXL provides the types and helpers, but the driver
> model is inherently visible. The driver author writes protocol
> implementation functions; AXL handles installation and boilerplate.

---

## How to Port an EDK2 App to axl-cc

### Step 1: Replace the entry point

EDK2:
```c
EFI_STATUS EFIAPI MyAppMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST) {
    // Manual ShellParameters protocol access for argc/argv
    gBS->OpenProtocol(ImageHandle, &gEfiShellParametersProtocolGuid, ...);
    ...
}
```

AXL:
```c
#include <axl.h>

int main(int argc, char **argv) {
    // argc/argv provided by the CRT0 entry stub
    ...
    return 0;   // 0 == EFI_SUCCESS, non-0 == EFI_ABORTED
}
```

The CRT0 entry stub at `src/crt0/axl-crt0-native.c` (~17 lines)
bridges UEFI's `_AxlEntry(ImageHandle, SystemTable)` to your
`int main(argc, argv)`. It calls `_axl_init` before `main` (default
loop singleton, atexit registry, Ctrl-C notify, tier-1 resource
registry) and `_axl_cleanup` after. You don't see any of that —
just write `main`. The full lifecycle is documented in
[`AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md).

### Step 1.5: If your app uses networking or needs a DXE driver

EDK2 apps typically assume the user already ran `connect -r` and
`ifconfig -s eth0 dhcp` from the UEFI shell before launching, and
that any DXE drivers they depend on (NIC drivers, RamDiskDxe, etc.)
are already loaded. AXL apps self-bootstrap so they Just Work
under `run-qemu.sh foo.efi` and on bare-metal images without a
hand-curated startup.nsh.

**Networking** — call `axl_net_auto_init` before any
`axl_tcp_*` / `axl_udp_*` / `axl_http_*` / `axl_dns_*` call:

```c
if (axl_net_auto_init(SIZE_MAX, /* dhcp_timeout_sec */ 10) != 0) {
    axl_printf("network bring-up failed\n");
    return 1;
}
```

It loads NIC drivers from the standard search path
(`drivers/<arch>/`), runs `ConnectController` globally
(equivalent to shell `connect -r`), and waits up to N seconds for
DHCP. Idempotent — short-circuits if SNP is already up.

**A specific DXE driver** — call `axl_driver_ensure(GUID, "Name.efi")`
before using a protocol the driver provides. `MkRd` is the
canonical example:

```c
if (axl_driver_ensure((const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
                      "RamDiskDxe.efi") != 0) {
    axl_printf("RamDiskDxe.efi not available\n");
    return 1;
}
```

The driver search path covers the image's own directory and
`drivers/<arch>/<name>` on every mounted FAT volume.

**TCP close lifetime** — `axl_tcp_close` returns immediately on
the async path (typical mid-loop case); the underlying `AxlTcp *`
pointer outlives the call by up to TIME_WAIT (~2 s) while the
firmware completes the close. Treat `sock` as freed once
`axl_tcp_close` returns, and always close TCP sockets *before*
freeing the loop they were registered with. The header doc in
`<axl/axl-tcp.h>` describes this explicitly.

### Step 2: Replace Print with axl_printf

EDK2: `Print(L"%s: %d bytes\n", FileName, Size);`

AXL: `axl_printf("%s: %d bytes\n", filename, size);`

Note: AXL uses UTF-8 everywhere. No `L""` wide strings, no `CHAR16`.

### Step 3: Replace memory functions

| EDK2 | AXL |
|------|-----|
| `AllocatePool(size)` | `axl_malloc(size)` |
| `AllocateZeroPool(size)` | `axl_calloc(1, size)` |
| `FreePool(ptr)` | `axl_free(ptr)` |
| `CopyMem(dst, src, n)` | `axl_memcpy(dst, src, n)` |
| `SetMem(dst, n, val)` | `axl_memset(dst, val, n)` (note: arg order differs) |
| `ZeroMem(dst, n)` | `axl_memset(dst, 0, n)` |

### Step 4: Replace string functions

| EDK2 | AXL / standard C |
|------|-------------------|
| `AsciiStrLen(s)` | `axl_strlen(s)` |
| `AsciiStrCmp(a, b)` | `axl_strcmp(a, b)` |
| `AsciiStrCpyS(dst, sz, src)` | `axl_strlcpy(dst, src, sz)` |
| `AsciiSPrint(buf, sz, fmt, ...)` | `axl_snprintf(buf, sz, fmt, ...)` |
| `AsciiStrDecimalToUint64(s)` | `axl_strtou64(s)` |
| `StrLen(s)` (wide) | Convert to UTF-8 first, then `axl_strlen` |
| `StrCmp(a, b)` (wide) | Convert to UTF-8 first, then `axl_strcmp` |

### Step 5: Replace file I/O

| EDK2 | AXL |
|------|-----|
| `ShellOpenFileByName(path, &h, mode, 0)` | `axl_fopen(utf8_path, "r")` |
| `ShellReadFile(h, &size, buf)` | `axl_fread(buf, 1, size, stream)` |
| `ShellWriteFile(h, &size, buf)` | `axl_fwrite(buf, 1, size, stream)` |
| `ShellCloseFile(&h)` | `axl_fclose(stream)` |
| `ShellGetFileInfo(h)` | `axl_file_info(path, &info)` |
| `ShellSetFilePosition(h, pos)` | `axl_fseek(stream, pos, AXL_SEEK_SET)` |
| `ShellIsDirectory(path)` | `axl_file_is_dir(path)` |
| `FileHandleFindFirstFile(h, &entry)` | `axl_dir_open(path)` + `axl_dir_read(dir, &entry)` |

### Step 6: Replace types

| EDK2 | Standard C |
|------|-----------|
| `VOID` | `void` |
| `BOOLEAN` | `bool` |
| `UINTN` | `size_t` |
| `UINT8/16/32/64` | `uint8_t/16/32/64_t` |
| `CHAR8` | `char` |
| `CHAR16` | `unsigned short` (or convert to UTF-8) |
| `EFI_STATUS` | `int` (0 = success, -1 = error) |
| `STATIC` | `static` |
| `IN/OUT/OPTIONAL` | Remove (documentation only) |
| `EFIAPI` | Remove (AXL handles calling convention) |

### Step 7: Build with axl-cc

```bash
# Install SDK (or install the .deb / .rpm from the releases page —
# https://github.com/aximcode/axl-sdk-releases — and skip this step)
./scripts/install.sh --prefix /opt/axl-sdk

# Build
/opt/axl-sdk/bin/axl-cc myapp.c -o myapp.efi

# Test in QEMU
./scripts/run-qemu.sh myapp.efi

# For network apps:
./scripts/run-qemu.sh --net --hostfwd 17000:7000 myapp.efi

# Better, for anything scripted: let the harness pick a host port that is
# free right now and hold it, instead of hard-coding one. The choice comes
# back as HOSTFWD_<guest-port>=<host-port>. This matters because QEMU
# refuses the ENTIRE -netdev if any single hostfwd cannot bind, so a port
# clash shows up as "the guest has no network" rather than "port busy".
./scripts/run-qemu.sh --net --hostfwd auto:7000 --background myapp.efi

# For apps that need a DXE driver staged on disk alongside:
./scripts/run-qemu.sh --extra MyDriverDxe.efi myapp.efi

# For "press a key to exit" stubs and other apps that need real
# user input — hands the host TTY to QEMU's serial console.
# Ctrl-A C drops to the QEMU monitor, Ctrl-A X quits.
./scripts/run-qemu.sh --interactive myapp.efi

# Drop into a UEFI shell with a host directory mounted as fsN:.
# Build apps on the host, run them from the UEFI shell without
# rebuilding the disk image each iteration. Requires virtiofsd
# and an OVMF that ships VirtioFsDxe (modern builds do).
./scripts/run-qemu.sh --interactive --mount ~/efi-apps
```

`run-qemu.sh` also samples QEMU's host CPU during the run and
emits a `WARN: CPU spike` line on stderr if a sustained spike
gets through (e.g. a loop lost its `axl_yield`). Silent on
healthy runs — see `--no-cpu-warn` / `--cpu-threshold` to tune.

### Reference: Hexdump port

See `uefi-devkit/DevKitPkg/Hexdump/Hexdump.c` for a complete example
of a ported application. Original: 202 lines with 8 EDK2 headers.
Ported: 158 lines with `#include <axl.h>`. Binary: 19KB.
