# AxlIpmi — local BMC access

Local BMC access via IPMI transports (KCS, SSIF, EDKII vendor
protocol, Dell vendor protocol), auto-selected from SMBIOS Type 38
plus firmware protocol discovery. AxlIpmi gives UEFI apps a single
`axl_ipmi_raw()` entry point that abstracts away which physical
interface is attached.

## Transports

| Kind | Mechanism | Typical platform |
|---|---|---|
| `AXL_IPMI_TRANSPORT_KCS` | x86 I/O ports via `axl_backend_io_*` | x86 servers |
| `AXL_IPMI_TRANSPORT_SSIF` | SMBus / I2C via `axl_backend_smbus_*` | ARM servers |
| `AXL_IPMI_TRANSPORT_EDKII` | `IPMI_PROTOCOL` via `LocateProtocol` | firmware with MdeModulePkg IPMI stack |
| `AXL_IPMI_TRANSPORT_DELL` | `EFI_IPMI_TRANSPORT` via `LocateProtocol` | Dell platforms with iDRAC |

Auto-detect priority (highest → lowest): EDKII protocol → Dell
protocol → SMBIOS Type 38 (KCS/SSIF) → x86 default KCS at
`0x0CA2`/`0x0CA3`.

**Phase 2 implements KCS only.** The other transports attach to
the same vtable in subsequent commits.

## Usage

```c
#include <axl/axl-ipmi.h>

AXL_AUTOPTR(AxlIpmiSession) ipmi = axl_ipmi_session_new();
if (ipmi == NULL) {
    axl_error("No IPMI transport available");
    return -1;
}

// Get Device ID: NetFn=0x06 (App), Cmd=0x01
uint8_t resp[16];
size_t  resp_len = sizeof(resp);
if (axl_ipmi_raw(ipmi, 0x06, 0x01, NULL, 0, resp, &resp_len) != 0) {
    axl_error("IPMI send failed");
    return -1;
}

if (resp[0] != 0x00) {
    axl_warning("IPMI completion code 0x%02x", resp[0]);
    return -1;
}
// resp[1]..resp[resp_len-1] carry the response body.
```

`AXL_AUTOPTR` frees the session and closes the underlying transport
at scope exit. No explicit `axl_ipmi_session_free()` needed on the
happy path.

## Layout

```
src/ipmi/
├── axl-ipmi.c            core dispatcher + session + auto-detect
├── axl-ipmi-internal.h   transport vtable + session layout
├── axl-ipmi-kcs.c        KCS transport (Phase 2)
└── axl-ipmi-{ssif,edkii,dell,cmd,format}.c   upcoming phases
```

Typed command wrappers (get_device_id, chassis_status, sdr, sel,
fru, sensor) and the `IpmiFormat.c`-equivalent enum-to-string
helpers land in Phase 5, together with their unit tests.

## Transport hazards

These are documented upstream in the uefi-ipmitool port the
module is based on; recording them here as well since they shape
the module's internals:

- **KCS FSM** is blocking, polled, 5-second timeout per request.
  Safe in the current call sites (same pattern as AxlSmbios) but
  not usable inside AxlLoop without yielding.
- **SSIF 60 ms inter-command delay** is enforced at the transport
  layer. Bulk operations (listing 150+ SDR records on
  iDRAC/Nvidia Grace) hang without it.
- **SSIF multi-part framing** required for responses > 32 bytes.
- **Dell protocol** synthesizes a `CC=0x00` completion code byte
  because the vendor firmware drops it from the response buffer.
