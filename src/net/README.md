TCP sockets, UDP sockets, socket abstraction layer, URL parsing, HTTP
server, HTTP client, TLS, and network utilities (IPv4 address helpers,
interface enumeration; diagnostics — ICMP ping + `axl_net_ping_ex`
(traceroute / path-MTU), `axl_net_sntp_query` (SNTP time), `axl_net_arp_list`
(neighbor cache), `axl_net_get_link_stats`).

Individual headers can be included separately or use the umbrella
`<axl/axl-net.h>`.

Headers:

- `<axl/axl-net.h>` — Umbrella + network utilities
- `<axl/axl-inet-address.h>` — IP address and socket address types
- `<axl/axl-socket.h>` — Unified socket (stream/datagram)
- `<axl/axl-socket-client.h>` — High-level DNS + connect helper
- `<axl/axl-tcp.h>` — TCP sockets (low-level)
- `<axl/axl-udp.h>` — UDP sockets (low-level)
- `<axl/axl-url.h>` — URL parsing
- `<axl/axl-http-core.h>` — Low-level HTTP/1.1 parsing (shared by server + client)
- `<axl/axl-http-server.h>` — HTTP server
- `<axl/axl-http-client.h>` — HTTP client
- `<axl/axl-tls.h>` — TLS support (compiled into every build; call
  `axl_tls_init()` to activate it)

## Overview

AXL networking is built on UEFI's TCP4/UDP4 protocol stack. The stack
must be initialized before use — either by the UEFI Shell (`ifconfig`)
or by calling `axl_net_auto_init`.

```text
Application
├─ axl_http_server / axl_http_client
├─ axl_socket_client (DNS + connect)
├─ axl_socket (stream/datagram)
│   ├─ axl_tcp / axl_udp
│   └─ axl_socket_address / axl_inet_address
├─ axl_tls (optional, wraps TCP)
└─ UEFI TCP4 / UDP4 protocols
    └─ IP4 → ARP → SNP (NIC driver)
```

### Network Initialization

The recommended one-call shape is `axl_net_bring_up` — load drivers,
acquire an IP (DHCP or static), optionally read it back. Used by
HTTP services, REST tools, and one-shot fetch utilities — they all
open with the same preamble:

```c
// DHCP — most common case
if (axl_net_bring_up(SIZE_MAX, NULL, NULL, NULL, 10, NULL) != AXL_OK) {
    axl_printf("Network not available\n");
    return -1;
}

// Static IP — pass the address (NULL netmask = /24 default,
// NULL gateway = none); also reads the resolved address back.
uint8_t  ip[] = { 192, 168, 1, 100 };
AxlIPv4Address addr;
if (axl_net_bring_up(SIZE_MAX, ip, NULL, NULL, 0, &addr) != AXL_OK) {
    return -1;
}
```

### Options-driven bring-up (`axl_net_auto_init_opts`) — the crash-safe superset

When a consumer wants what `netload -a` does — firmware-first, then a
**crash-safe sweep** of a driver directory, then DHCP or static, with a
config a user can change later — use `axl_net_auto_init_opts`. A
resident service (SoftBMC and friends) comes up in auto mode, then
re-drives a specific NIC by MAC on user request, all through one call:

```c
// Auto: firmware-first, then crash-safe sweep of \drivers\<arch>, DHCP.
AxlNetAutoOpts opts = { 0 };            // zero-init == AUTO + DHCP + SWEEP_DIR
AxlNetBringUpResult res;
if (axl_net_auto_init_opts(&opts, &res) == AXL_OK) {
    // res.online, res.mac, res.ipv4, res.via ("firmware" or the driver that won)
}

// Later: the user picks a NIC by MAC and a static address.
AxlNetAutoOpts st = { 0 };
st.nic_select  = AXL_NET_NIC_SEL_MAC;   axl_memcpy(st.nic_mac, chosen_mac, 6);
st.ip_mode     = AXL_NET_IP_STATIC;     st.static_ipv4 = ip;  st.static_mask = mask;
axl_net_auto_init_opts(&st, NULL);
```

`nic_select` is a mode (`AUTO` / `INDEX` / `MAC`), not a raw ordinal,
so a zero-initialized struct genuinely means AUTO (not "NIC 0"); pick
NICs by **MAC** in a UI, since ordinals shift as drivers load.
`driver_strategy` chooses what is loaded after firmware-first —
`SWEEP_DIR` (the default, `netload -a`'s crash-safe directory sweep),
`CURATED` (the built-in list `axl_net_auto_init` uses), or
`FIRMWARE_ONLY`. With `sweep_dir` NULL the sweep takes the directory
`axl_net_driver_dir()` names — `drivers\<arch>` beside the running image,
then `drivers` beside it, then `\drivers\<arch>` at its volume's root —
and, when none exists, every volume's `\drivers\<arch>`.

Two capabilities are **opt-in in code**: an image links them only by
naming them, so a zeroed struct carries neither the JSON5 sidecar reader
nor ping/DNS.

- `deps = axl_net_sweep_deps_sidecar()` co-loads a candidate's companion
  drivers from the sweep dir's `netload-drivers.json5` sidecar first — a
  USB-RNDIS/CDC NIC whose driver needs NetworkCommon. A companion is never
  swept as a candidate. Companions stay resident through the sweep (a later
  candidate reuses them); at its end those the winner does not DECLARE (its
  sidecar `requires`, followed transitively) are unloaded -- all of them
  when nothing came up, and one the winner relies on without declaring it
  too. A candidate whose own Unload refuses stays resident; its companions
  are unloaded all the same.
- `verifier = axl_net_verify_reachable` keeps trying drivers until one both
  configures **and** passes the ping (`ping_ipv4`), gateway
  (`ping_gateway`) and DNS (`resolve_host`) checks asked for, not just gets
  an address. Any function of that shape works.

`embedded` / `embedded_count` hand the sweep NIC drivers held in memory
(`AxlNetEmbeddedDriver`: a file name, the image -- raw or packed by
`axl-efi-pack` -- and its size), for a tool that carries its NIC driver
inside itself (`sshd-rtk.efi`). They are tried FIRST, in order, before any
file in the sweep directory (iPXE-named ones after every other candidate),
under the same crash guard, quarantine (keyed by the name), companions,
deadline and events -- each event carrying `is_embedded` -- and given the
sweep directory's volume as their DeviceHandle, as a file load there would,
else `device_handle` (the volume the tool was run from).

Three more fields bound and steer the bring-up:

- `timeout_sec` is ONE deadline for all of it: every wait (link, DHCP) is
  cut to what is left, and no driver is loaded once it has passed —
  `AXL_TIMEOUT`.
- `cancel` ends it — the running wait, or the sweep before its next driver
  — with `AXL_CANCELLED`. A cancellable that claims the Shell's break
  (`axl_cancellable_set_cancel_on_break`) makes a Ctrl-C do the same, even
  one pressed while a driver was loading.
- `link_wait_sec` turns on the **link gate**: each pass (the firmware's,
  then each swept driver's) waits that long for a link, reports a NIC with
  none as `LINK_DOWN` and gives it no DHCP, and announces each link-up NIC
  with `CONFIGURING` before its address step. A box whose firmware NICs are
  all uncabled reaches the sweep after the link wait instead of after a
  DHCP timeout. With DHCP, a pass runs it on ALL its link-up NICs at once,
  sharing one `dhcp_timeout_sec` wait (as Linux's `ip=dhcp`, systemd-networkd
  and dhcpcd do): the first to lease that the verifier accepts wins (a NIC
  it rejects leaves the race, which goes on waiting), and every other NIC is
  put back -- its IP4Config2 policy and, for a NIC that was static, its
  manual address, gateway and DNS servers -- so a pass costs the link wait plus one DHCP budget
  however many ports are cabled. The wait lists the NICs afresh as it goes, so one whose
  interface appears during it — a USB NIC still enumerating — is found;
  it runs even when there is no NIC yet.

A DHCP the bounded bring-up gave up on has the NIC's IP4Config2 policy put
back. A firmware call that blocks rather than waits — a driver load or
connect, the DHCP of the IP4Config2-free fallback — is not cut short; the
deadline and the cancellable are checked when it returns.

An optional `on_driver` callback reports each driver the sweep touches — a
`TRYING` event before each load (and before the firmware pass), a result
event after, one per co-loaded dependency, and the link gate's per-NIC
events — so a tool can render its own findings (and warn of a slow
connect) without that rendering living in the library.

**`netload -a` and `sshd` ARE this engine.** netload runs its own
firmware-first probe and saved-config replay, then hands the staged sweep
to `axl_net_auto_init_opts` (with `skip_firmware_first`, `deps` and, when
asked, `verifier`) and renders its findings table from the `on_driver`
callback. `sshd` runs the whole of it — firmware-first, the link gate, the
sweep of the drivers beside `sshd.efi` with their companions — under a
60 s deadline (`AxlNetworkTimeout`) and Ctrl-C, each DHCP capped at `AXL_NET_DHCP_ATTEMPT_SEC`
(15 s, netload's per-NIC wait) so a NIC no server answers falls through to
the sweep, naming each step from the callback. Their
integration suites are the engine's end-to-end coverage.

**Crash safety — design to the hardware.** On the target boxes a bad
driver **RSODs the machine and it does not auto-reboot**. The SWEEP_DIR
path breadcrumbs each driver to NVRAM before loading it, against ONE
shared `axl-net` driver-quarantine namespace: after a power-cycle the
next call quarantines the culprit and skips it, so the sweep advances
past it instead of dying in the same place every boot. A crash-safe
first run can therefore need one manual reboot per bad driver to
converge; the quarantine then persists. The namespace is shared across
every consumer of the engine — a driver that RSODs is bad for all of
them — and is reset by `axl_net_clear_driver_quarantine()` (the library
form of `netload --clear`). `netload` shares this exact namespace, so a
driver it quarantines is one the engine also skips, and vice versa.

### Standard option helpers

Most consumers (tools, services) take the same NIC / local-IP /
port options on the command line and run the same DHCP bring-up
preamble. `<axl/axl-net-opts.h>` ships a canonical option bag
plus a one-call init helper:

```c
typedef struct {
    AxlNetOpts net;          // embed as sub-struct
    const char *url;
    bool        verbose;
} MyOpts;

// DHCP bring-up driven by the bag — maps AXL_NET_NIC_AUTO to
// SIZE_MAX and runs axl_net_bring_up under the hood:
if (axl_net_init_from_opts(&opts.net, 10) != AXL_OK) {
    axl_printf("network unavailable\n");
    return 1;
}

// Use opts.net.local_ip for the local socket bind — outbound
// source for clients, listen address for servers (same bind(2)).
```

For a bring-up within ONE overall deadline, stoppable by a cancellable or
Ctrl-C, use `axl_net_auto_init_opts` with `timeout_sec` and `cancel` (see
above).

The bag carries three fields:

- `nic_index` — which NIC to DHCP on; `AXL_NET_NIC_AUTO` picks the
  first usable one.
- `local_ip` — IPv4 to `bind(2)` the local socket end to.
  Outbound source for clients (curl `--interface`-style),
  listen address for servers — same syscall, role implied by
  what the consumer does next.
- `port` — `uint16_t`; consumers define their own domain default.

**Out of scope by design**: installing a static IPv4 on the NIC.
That's a firmware-`ifconfig`-layer concern (UEFI Shell
`ifconfig`, or `axl_net_set_static_ip` for tools that genuinely
need to mutate `IP4Config2` policy). The options bag is for
stateless connection-side selectors only.

Pair with the descriptor-table composition helpers in
`<axl/axl-config.h>` (`axl_config_descs_net`,
`axl_config_descs_append`) to also inject the matching CLI / config
descriptors into a consumer's own table without copy-paste — see
the AxlConfig docs. The `AXL_NET_OPT_SOURCE_IP` and
`AXL_NET_OPT_LISTEN_IP` selector bits both target the same
`local_ip` field; they differ only in CLI vocabulary
(`--source-ip` vs `--listen-ip`).

Three layered primitives sit underneath if a consumer needs finer
control:

- `axl_net_drivers_up()` — load NIC drivers, connect SNP, wait for
  link-up (5 s budget). No DHCP, no IP assignment.
- `axl_net_auto_init(nic, dhcp_timeout)` — drivers_up + DHCP wait.
  Used by the DHCP path of `bring_up` internally. Event-driven
  via `EFI_IP4_CONFIG2_PROTOCOL.RegisterDataNotify` —
  sub-millisecond wakeup after DHCP completes (firmware that
  doesn't support the notify falls back to a 1 s tick). The wait is
  on the NIC being configured: another NIC's address does not end
  it. On firmware
  that lacks IP4Config2 entirely (some OEM laptops), it falls back
  to `Dhcp4ServiceBinding` then PXE Base Code DHCP, caching the
  lease so `axl_net_get_ip_address` / `axl_net_get_dhcp_lease` still
  report it; `axl_net_last_config_method()` says which path won.
  (Those fallbacks are real-hardware-only — OVMF always has
  IP4Config2.)
- `axl_net_set_static_ip(nic, ip, netmask, gateway)` — raw
  IP4Config2 setter; the static path of `bring_up` applies the same
  configuration after `drivers_up`.
  `axl_net_set_static_ip_by_mac(mac, ip, netmask, gateway)` is the
  MAC-keyed sibling, paired with it exactly as
  `axl_net_get_dhcp_lease_by_mac` is paired with
  `axl_net_get_dhcp_lease`: an ordinal is only stable while the NIC set
  is (a NIC appearing shifts later ordinals), a MAC never moves, so a
  consumer holding an `AxlNetInterface.mac` across a driver-load event
  should prefer the `_by_mac` spelling. The tradeoff is deliberate and
  worth knowing: the `_by_mac` variants require a real MAC match and
  error out otherwise, while the ordinal spellings can still fall back
  to the sole IP4Config2 handle on single-NIC firmware where no MAC
  correlates (OEM boxes that publish IP4Config2 on a child handle with
  no reachable SNP). Name the NIC by MAC for stability; use the ordinal
  spelling when you want that fallback.

## Static config / DNS / hostname (the `ifconfig` policy layer)

For an on-box network-setup UI (the "Configure" tab), the IP4Config2
policy layer is descriptor-driven like everything else — no
hand-authoring the form:

- `AxlNetStaticOpts` + `axl_config_descs_net_static(out, cap, base_off)`
  — the policy option group (mode / ip / netmask / gateway / dns / dns2 /
  hostname), the sibling of `axl_config_descs_net`. Fields are
  `const char *` (so AxlConfig's pointer-based string auto-apply
  populates them; an inline `char[]` would silently fail). A UI embeds
  the struct, emits the descriptors, and `axl_config_new` points each
  field at the form value.
- `axl_net_init_static(cfg, nic, timeout)` — one-call apply: dispatches
  on `cfg->mode` (`"static"` → set IP/mask/gw + DNS + hostname + settle;
  `"dhcp"` → DHCP + best-effort DNS/hostname). An unrecognized mode is
  rejected, not silently DHCP'd.
- `axl_net_set_dns(nic, dns, dns2)` — the missing resolver *setter*
  beside `axl_net_resolve` (a query). IP4Config2 makes the DNS list
  read-only under the DHCP policy, so this is a static-mode operation.
- `axl_net_set_hostname(name)` / `axl_net_get_hostname(buf, size)` —
  the box's hostname, persisted to a dedicated AXL non-volatile
  variable. **UEFI has no firmware-advertised hostname**, so this is a
  stored value (set + display + read by an AXL-aware consumer); it does
  not, by itself, make the firmware DHCP client send the name.
- `axl_net_wait_ip_settled(nic, expect_ipv4, timeout_ms)` — IP4Config2
  applies asynchronously; this polls until the address has taken
  (`expect_ipv4` non-NULL = wait for *that* address, so a stale prior
  address doesn't satisfy it) so a read-back is valid.

`tools/netinfo config` dogfoods the bring-up path
(`--mode static --ip … --dns … --hostname …`).

## Old tools: the pre-UEFI-2.4 IPv4 configuration protocols

UEFI 2.4 replaced `EFI_IP4_CONFIG_PROTOCOL` with `EFI_IP4_CONFIG2_PROTOCOL`,
and current firmware produces only the new one. The EFI 1.x shell's
`ifconfig` predates the change, so on current firmware it printed every NIC
as `Not configured.` -- even one `netload` had just leased.

Every bring-up above that leaves a NIC configured (`axl_net_auto_init`,
`axl_net_auto_init_opts`, `axl_net_bring_up`, `axl_net_set_static_ip*`, and
so `netload`, `sshd` and the tools built on `axl_net_init`) now also calls
`axl_net_legacy_ip4cfg_install()`. It loads, once per boot, a small resident
driver that puts the old protocols on every NIC handle with IP4Config2:

- `EFI_IP4_CONFIG_PROTOCOL` (UEFI 2.3.1): `GetData` returns the station
  address, mask and route table, the route table after the structure in the
  caller's buffer, as the old Ip4Config driver did.
- The NIC configuration protocol the pre-2.4 network stack installed beside
  it, which is what the old shell's `ifconfig` actually reads: the name
  (`eth0`, numbered as that shell numbers NICs itself), the link address, the
  policy and the addresses. An old `ifconfig -l` then prints:

```
eth0
  MAC        : 52-54-00-12-34-56
  Media State: Media present
  Source     : DHCP
  Perment    : TRUE

  IP address : 10.0.2.15
  Mask       : 255.255.255.0
  Gateway    : 10.0.2.2
  ...
```

What it promises:

- **It never shadows a real one.** A NIC that already has
  `EFI_IP4_CONFIG_PROTOCOL` (firmware from before UEFI 2.4) is left alone.
- **Read-only.** Everything answers from the NIC's live IP4Config2 data; the
  old ways of changing it (`ifconfig -s`, `ifconfig -c`) get
  `EFI_UNSUPPORTED`, and the old shell prints `Failed to set the NIC
  configure parameters, Unsupported`. The old protocol's "not permanent"
  flag cannot be honoured on IP4Config2, which always keeps its policy, so
  refusing is the honest answer. Configure with `netload` or the modern
  Shell's `ifconfig`. `Start`/`Stop` of the old protocol change only its
  own state, never the NIC.
- **It follows the NICs.** It is a UEFI driver-model driver holding each
  NIC's IP4Config2 `BY_DRIVER`, so a NIC whose network stack goes away
  loses the old protocols first, and a NIC that appears later gets them.
- **It outlives the caller.** `netload` exits before an old `ifconfig`
  runs, so the protocols cannot live in it: the driver is its own image,
  embedded in libaxl (built freestanding, about 10 KB) and loaded from
  memory. `axl_net_legacy_ip4cfg_uninstall()` unloads it.
- **One copy, the newest.** The driver marks its image with
  `AXL_NET_LEGACY_IP4CFG_MARKER_GUID` and its `AXL_NET_LEGACY_IP4CFG_REVISION`.
  A resident copy of this revision or newer is kept; an older one -- left by a
  tool built against an earlier SDK -- is unloaded and replaced.
- **Best effort.** A bring-up succeeds whether or not the driver loads --
  Secure Boot, for one, refuses its unsigned image.

The modern Shell's `ifconfig` reads IP4Config2 and is unaffected.

## NIC inventory + driver selection

`axl_net_list_interfaces(out, &count)` is the base enumeration --
query-then-fill, one row per physical NIC (deduped by MAC).
`axl_net_list_interfaces_alloc(&out, &count)` is the allocating
counterpart: it does the count/alloc/re-query dance for you and
hands back a heap array (`axl_free` it) instead of making every
caller repeat that dance by hand.

`axl_mac_format(mac, buf, size)` / `axl_mac_parse(str, mac)` are the
MAC-address counterpart to `axl_ipv4_format`/`axl_ipv4_parse`: format
an `AxlNetInterface.mac` as `"aa:bb:cc:dd:ee:ff"`, or parse that same
colon-separated form back into six bytes (case-insensitive, 1-2 hex
digits per octet).

For a local "pick the NIC / get an unknown box online / diagnose"
tool, five accessors layer on top of `axl_net_list_interfaces`:

- `axl_net_get_driver_info(mac, &info)` — the bound driver name
  (the driver of the deepest bus controller on the NIC's path, so a
  USB NIC names its USB NIC driver, not the USB host controller's) +
  binding layer (`NII3.1` / `NII` / `SNP`) and a stable
  **bus location** (the NIC's device-path topology, e.g.
  `PciRoot(0x0)/Pci(0x3,0x0)`, with the MAC/IP network tail
  trimmed) for the NIC carrying `mac`. The bus location is a
  reboot-stable selector that distinguishes two identical NICs,
  unlike the fragile enumeration index. Kept off
  `axl_net_list_interfaces` (which is polled during link bring-up)
  because the resolution is heavier — a UI fills the driver/bus
  columns per row.
- `axl_net_list_available_drivers(out, &count)` — the NIC-driver
  `.efi` / `.efidrv` files staged on `drivers/<arch>/` across mounted
  volumes, so a UI can offer "try X / Y / Z".
- `axl_net_ensure_drivers()` loads the known USB-network and vendor UNDI
  drivers it finds, and **never an iPXE driver**, not even as a fallback
  when they bring up no SNP: iPXE takes the USB host controller, so a BMC's
  virtual keyboard and virtual-media stick (both USB) vanish. An iPXE driver
  loads only when named (`axl_net_try_driver`, `netload -p`).
- `axl_net_driver_is_ipxe(path_or_name)` — the filename heuristic
  ("ipxe" substring, case-insensitive) that recognizes an iPXE
  driver. `axl_net_try_driver` applies this internally; it's exposed
  for a caller with its *own* load/start loop (a driver-picker UI, a
  diagnostic sweep like `netload`'s) that still has to order an iPXE
  candidate last and disarm the watchdog after starting it.
- `axl_net_try_driver(path_or_name, &result)` — load + connect **one**
  driver and report `{ snp_handles_added, link_up, bound_nic_macs,
  driver }`, unloading it again on failure so the next candidate starts
  clean. Every newly-bound NIC's MAC is recorded (the heap
  `bound_nic_macs` array is caller-owned — free with `axl_free`; no
  fixed cap), and on success the resident driver's handle is returned in
  `result.driver` so a sweep can `axl_driver_unload` a driver that bound
  a NIC but failed its own downstream check. Encapsulates the field
  hazards: iPXE's watchdog is disarmed (and iPXE must be tried last —
  its `LoadImage` hook breaks later loads), and `MediaPresent` is
  treated as advisory.
- `axl_net_connect_stack()` — the `ConnectController`-on-SNP step
  (for firmware that doesn't auto-connect), exposed so a "my NIC
  isn't showing up" action works without a full re-init.

`tools/netinfo` dogfoods four of the five (`list -v` driver/bus
columns, `list-bundle`, `try <driver>`); `tools/netload`'s driver
sweep drives `axl_net_try_driver` for its per-driver load/connect/diff
(breadcrumbed for crash recovery, ordered iPXE-last via
`axl_net_driver_is_ipxe`, and unloading each non-winning driver through
the returned `result.driver`).

## Socket Layer

`AxlSocket` is the recommended socket API for new code — a
GLib-`GSocket`-shaped abstraction over both stream (TCP) and datagram
(UDP) transports, with rich address types and blocking and async
forms. It delegates to the low-level `AxlTcp` / `AxlUdp`
primitives under the hood (see the [Low-Level TCP / UDP](#low-level-tcp--udp)
section below).

### Address Types

`AxlInetAddress` wraps an IPv4 address with parsing, formatting,
and comparison:

```c
// Create from string or bytes
AxlInetAddress *addr = axl_inet_address_new_from_string("192.168.1.1");
AxlInetAddress *lo   = axl_inet_address_new_loopback();

const char    *str   = axl_inet_address_to_string(addr);  // "192.168.1.1"
const uint8_t *bytes = axl_inet_address_to_bytes(addr);

axl_inet_address_free(addr);
axl_inet_address_free(lo);
```

`AxlSocketAddress` pairs an address with a port:

```c
// From address + port (takes ownership of the AxlInetAddress)
AxlSocketAddress *sa = axl_socket_address_new(
    axl_inet_address_new_from_string("10.0.0.1"), 8080);

// Or parse "host:port"
AxlSocketAddress *sa2 = axl_socket_address_new_from_string("10.0.0.1:8080", 0);

axl_socket_address_free(sa);
axl_socket_address_free(sa2);
```

### Unified Socket

**TCP client:**

```c
AXL_AUTOPTR(AxlSocket) sock = axl_socket_new(AXL_SOCKET_STREAM);
AxlSocketAddress *remote = axl_socket_address_new(
    axl_inet_address_new_from_string("192.168.1.1"), 8080);

if (axl_socket_connect(sock, remote) == 0) {
    axl_socket_send(sock, "hello", 5, 0);

    char buf[64];
    size_t len = sizeof(buf);
    axl_socket_receive(sock, buf, &len, 5000);
}
axl_socket_address_free(remote);
```

**TCP server (blocking):**

```c
AXL_AUTOPTR(AxlSocket) listener = axl_socket_new(AXL_SOCKET_STREAM);
axl_socket_listen(listener, 8080);

AxlSocket *client;
if (axl_socket_accept(listener, &client) == 0) {
    // handle client...
    axl_socket_free(client);
}
```

**UDP send:**

```c
AXL_AUTOPTR(AxlSocket) sock = axl_socket_new(AXL_SOCKET_DATAGRAM);
AXL_AUTOPTR(AxlSocketAddress) dest = axl_socket_address_new(
    axl_inet_address_new_from_string("192.168.1.100"), 514);
axl_socket_send_to(sock, msg, msg_len, dest);
```

### Socket Client

`AxlSocketClient` combines DNS resolution and TCP connection:

```c
AXL_AUTOPTR(AxlSocketClient) client = axl_socket_client_new();
AxlSocket *sock;

if (axl_socket_client_connect_to_host(client, "example.com", 80, &sock) == 0) {
    axl_socket_send(sock, request, req_len, 0);
    axl_socket_free(sock);
}
```

Or connect to a resolved address:

```c
AxlSocketAddress *addr = axl_socket_address_new(
    axl_inet_address_new_from_string("10.0.0.1"), 8080);
AxlSocket *sock;
axl_socket_client_connect(client, addr, &sock);
axl_socket_address_free(addr);
```

### Async Operations

The socket layer supports async operations via `AxlLoop`. Callbacks
return `bool` — `true` keeps the op armed (accept-the-next-client or
re-issue-recv-on-same-buffer), `false` tears down. Returning `false`
permits closing the socket inside the callback: the loop does not
touch the socket again after a false return.

```c
bool on_client(AxlSocket *client, AxlStatus status, void *data) {
    if (status != 0) return true;  // transient error; keep listening
    // handle client...
    axl_socket_free(client);
    return true;  // keep accepting more clients
}

AXL_AUTOPTR(AxlSocket) listener = axl_socket_new(AXL_SOCKET_STREAM);
axl_socket_listen(listener, 8080);
axl_socket_accept_async(listener, loop, on_client, NULL);
axl_loop_run(loop);
```

See `sdk/examples/echo-server.c` for a complete async echo server
built on this layer — it uses `axl_socket_receive_async` in
stays-armed mode (callback returns `true` to keep receiving).

## Low-Level TCP / UDP

> Most applications should use the [Socket Layer](#socket-layer)
> above. `AxlTcp` and `AxlUdp` are the primitives underneath --
> thin wrappers over UEFI's `TCP4_PROTOCOL` / `UDP4_PROTOCOL`. Reach
> for them only when you need raw access to UEFI tokens, want the
> session-scoped cancellation pattern shown below, or are minimizing
> wrapper overhead. See `sdk/examples/tcp-echo-server.c` for the
> low-level counterpart to the socket-based `echo-server.c`.

### TCP Sockets

Blocking and async TCP sockets. The blocking API is simpler; the async
API integrates with the event loop for non-blocking I/O.

**Client (blocking):**

```c
AxlTcp *sock;
if (axl_tcp_connect("192.168.1.1", 8080, &sock) == 0) {
    axl_tcp_send(sock, "GET / HTTP/1.0\r\n\r\n", 18, 5000);

    char buf[4096];
    size_t len = sizeof(buf);
    axl_tcp_recv(sock, buf, &len, 5000);

    axl_tcp_close(sock);
}
```

**Server (async with event loop):**

```c
bool on_client(AxlTcp *client, AxlStatus status, void *data) {
    if (status != 0) return true;  // transient error; keep listening
    // handle client connection...
    axl_tcp_close(client);
    return true;  // keep accepting more clients
}

AxlTcp *listener;
axl_tcp_listen(8080, &listener);
axl_tcp_accept_async(listener, loop, /*cancel=*/NULL, on_client, NULL);
axl_loop_run(loop);
```

**Sends are queued; their callbacks are deferred.**

`axl_tcp_send_async` never refuses a send and never asks the caller to
serialize. A send submitted while another is on the wire is accepted
and transmitted when its turn comes, FIFO; the return value is
`AXL_OK` for accepted and `AXL_ERR` only for a send that was rejected
outright (closed socket, bad argument, allocation failure), in which
case no callback fires. A transport that refuses the bytes reports
that the same way a transport that drops them does — through the
callback, with `AXL_ERR`.

Because nothing is refused, the queue is unbounded: a producer that
outruns its peer grows it without limit, and AxlTcp only logs a
warning, once per crossing, when the sends queued BEHIND the active one
pass 1 MiB (a different measure from the getter below, so one large
send alone never trips it). Backpressure is the caller's,
and `axl_tcp_send_queued_bytes(sock)` is the hook for it -- the bytes of
accepted sends not yet handed to the firmware (queued sends in full,
plus the unarmed remainder of the active one). Check it before
producing more and hold off above a bound that suits the application:

```c
if (axl_tcp_send_queued_bytes(sock) < 256 * 1024) {
    produce_and_send(sock);      /* otherwise retry from the send callback */
}
```

The value is a snapshot: exact when read, and until any loop driving a
send on the socket runs, or a send on it is submitted, cancelled or
closed. A socket busy only with an orphan piece (below) reads 0.

The callback itself never runs inside `axl_tcp_send_async` or inside
another send's completion handling: it is queued for the loop's next
iteration, which is how EDK2's socket layer signals its own tokens.
So a send callback may do anything to the socket, `axl_tcp_close`
included, without tripping over a transport operation still in
progress underneath it. Transmission is not delayed by this — the next
queued send is handed to the firmware immediately, and only the
notification waits for the tick.

**Sends are paced by polling, not by the firmware's timer.** Two
firmware limits sit below AxlTcp. EDK2's MnpDxe drains one received
frame per 10 ms tick unless polled, and a TcpDxe Transmit completes
only once TCP has segmented all of it, which the send window -- opened
by the peer's ACKs -- gates, so a send left
to the firmware advanced at ~10 ms per segment (a 1 MiB AxlHttp GET
took 7-10 s). And iPXE's SNP driver (QEMU's e1000 option ROM, and
real firmware wherever iPXE drives the NIC) refuses the 16th frame
since its last poll with an error MnpDxe does not retry, so a deep
burst lost a segment until TCP's retransmit timer. AxlTcp therefore:

- hands a large send to the firmware 8 KB at a time by default (about
  5 segments, well under the 15-frame ring);
- polls once before every Transmit, because that limit is per NIC and
  several sockets transmitting in one loop turn share it;
- while any send on a loop is on the wire, polls every sending socket
  from ONE idle source on that loop, each iteration. The loop keeps
  dispatching its other sources in between; it only stops sleeping.
  The source goes away with the loop's last pending send, and after
  the back-off (500 ms by default) in which no send made progress.

"Progress" is a Transmit handed to the firmware, so the back-off bounds
only a peer that stops reading: one poll burst of the back-off per
zero-window stall. A peer that reads slowly but accepts at least one
piece per back-off keeps its loop polling for the whole transfer -- on
a uniprocessor firmware, one busy core however slow the reader. And
each socket keeps one Transmit in flight, so a single send moves at
most about one piece per round trip: 8 KB per 100 ms is about 80 KB/s.

Measured on QEMU (RELEASE builds, sub-millisecond RTT): a 1 MiB
AxlHttp GET went from 7-10 s to 0.05-0.08 s on x64 and from 7.2 s to
0.12-0.14 s on aa64. The idle source counts against the loop's
`AXL_MAX_SOURCES`, once per loop however many sockets are sending. At
most eight loops pace at once. A send that cannot be paced (a full
source table, or a ninth loop) still completes at the firmware's pace;
each of those two fallbacks logs one warning the first time it happens
in the process. While a loop is polling it does not reach its blocking
wait, so a serial Ctrl-C keystroke is read only once polling stops:
delayed, not lost. The Shell's own break flag is checked on every pass
regardless.

**Tuning for a resident service.** Three settings:

| knob | default | what it trades |
|---|---|---|
| `axl_tcp_set_pacing()` `.chunk_bytes` | 8192 | one send moves about one piece per round trip, so beyond ~120 ms RTT 8 KB is slower than before pacing; 16 KB doubles that ceiling and suits a single distant client, but with several sockets sending large messages at once it measured multi-second stalls on iPXE (cause unknown). Keep chunk / MSS at most 15; smaller pieces are unmeasured with pacing and add a round trip per piece |
| `axl_tcp_set_pacing()` `.idle_backoff_ms` | 500 | must exceed the RTT to the peer, or sends to it get SLOWER than unpaced; each zero-window stall costs one poll burst this long; a trickle reader is not bounded by it. 0 turns polling off |
| `axl_tcp_set_pacing()` `.linger_us` | 0 (off) | after a socket's last send retires with a receive armed on the same loop, keep polling it this long, so a request/response peer's next request is answered at once instead of at the firmware's next 10 ms timer. NIC-dependent: on QEMU virtio-net 9000 us took a one-request-at-a-time SFTP download from 1.5 to 16.3 MB/s, on QEMU's e1000 it made uploads slower (5.5 to 0.9 MB/s). Costs a polling loop for up to this long after each reply |
| `axl_loop_set_driver_budget_ms()` | 5 | how long a driver-mode tick keeps polling while a send is on the wire; clamped to half the tick. Held at TPL_CALLBACK, so a foreground Shell loses about the budget plus one pass per tick |

A driver-mode service (`axl_loop_attach_driver`; AxlService attaches
its loop itself, after `setup`) gets the defaults with nothing to call.
The honest ceilings there: its loop polls only inside its tick. Measured
on QEMU (RELEASE, 50 ms tick, defaults, a real Shell in the foreground):
a 1 MiB GET takes about 0.35-0.4 s on x64 (~2.5 MB/s) and about
1.0-1.7 s on aa64 (~0.6-1 MB/s), against 6.4 s and 8.8 s with one pass
per tick; the Shell's
command latency was unchanged (~140 ms, typed over serial, idle or during
transfers). On a WAN the per-round-trip ceiling (chunk / RTT) is the
limit and the budget helps little. To change anything:

```c
/* In AxlService's setup callback, before anything listens: */
AxlTcpPacing pacing;
axl_tcp_get_pacing(&pacing);
pacing.idle_backoff_ms = 1000;               /* e.g. VPN clients at ~600 ms */
if (axl_tcp_set_pacing(&pacing) != AXL_OK) { /* out of range */ }
if (axl_loop_set_driver_budget_ms(loop, 10) != AXL_OK) { /* > 25 */ }
```

Which to turn when:

- **LAN transfers are slow in driver mode:** raise the budget (up to
  `AXL_LOOP_DRIVER_BUDGET_MAX_MS`, and never past half the tick), or
  shorten the tick. The budget is spent only while a send is on the
  wire.
- **A console or Shell in the foreground feels sluggish during a
  download:** lower the budget. Throughput falls; the loop still never
  blocks.
- **Clients over a VPN or WAN are slower than before pacing:** raise
  `idle_backoff_ms` above their RTT. If they are still slow, the
  per-round-trip ceiling is the limit: raise `chunk_bytes` (16 KB for a
  single distant client), within the iPXE bound if the NIC is
  iPXE-driven -- and not where several sockets send large messages at
  once, where 16 KB stalled for seconds on iPXE.
- **CPU matters more than throughput to peers that stall** (browser
  tabs that stop reading a stream): lower `idle_backoff_ms`, but keep
  it above the RTT of the peers you care about. It does nothing for a
  slow-but-steady reader.
- **An iPXE-driven NIC on a path with a small MSS:** lower
  `chunk_bytes` so chunk / MSS stays at most 15.
- **A request/response client is slow however fast the link** (one
  request in flight, about 10-20 ms each): that is what
  `axl_net_set_latency` (below) addresses -- opt-in, see its USB keyboard
  caveat. Without it, `linger_us` set to a little more than the round trip
  to the client helps -- after measuring on the NIC: on QEMU's e1000 it slowed
  uploads.

### Round-trip latency (`axl_net_set_latency`)

With no receive interrupt, a frame reaching an idle firmware waits for the
firmware's next 10 ms tick and then the network stack's own 10 ms receive
timer: an event-loop server on X64 measured a p90 of 10-20 ms per round
trip. `axl_net_set_latency` removes that by going **fast** -- a 1 ms
firmware tick (`axl_timer_resolution_begin`), a 1 ms poll of every live
AxlTcp socket from a firmware timer event, and every driver-attached
loop's tick shortened to match -- for as long as the policy says:

| mode | fast while | for |
|---|---|---|
| `AXL_NET_LATENCY_FIRMWARE` (default) | never | the behaviour before it existed |
| `AXL_NET_LATENCY_WINDOW` (250 ms) | the window after any AxlTcp activity | a service; idle costs nothing extra |
| `AXL_NET_LATENCY_ALWAYS` | any AxlTcp socket exists | a server whose FIRST request must be fast too (a test agent) |

Measured on QEMU (2026-11-03, `test-net-latency-qemu.sh`): p90 0.4-1.1 ms
under either policy on X64 and AARCH64, for an event-loop server, a
driver-attached one and the synchronous calls alike; a guest idling with
a listener costs 0.015 cores at the firmware's pace and 0.090 while fast.

**Opt-in, because a fast tick can break USB keyboards.** On QEMU (OVMF,
usb-kbd on qemu-xhci) ten key presses 0.5 s apart all arrived at a 10 ms or
5 ms tick, 8 at 2 ms, and 1 at 1 ms. Root cause and real-hardware behaviour
are not yet known; hold a fast tick only where nobody types on a USB
keyboard -- a test agent driven over the network, say.

```c
AxlNetLatency lat = { .mode = AXL_NET_LATENCY_ALWAYS };   /* zero fields: defaults */
if (axl_net_set_latency(&lat) != AXL_OK) { /* a field out of range */ }
```

`axl_tcp_close` retires every pending send whose bytes it drops -- the
queued ones, and an active one not yet fully handed to the firmware --
firing each callback with `AXL_CANCELLED` before it returns. It also
runs, before it returns, the callback of a send that had already
FINISHED, or whose last piece was already armed, but whose deferred
callback had not come up yet -- and that one carries its real status,
`AXL_OK` or `AXL_ERR`, not `AXL_CANCELLED`. So a status is not a
"torn down" signal: a callback that sends more on `AXL_OK`, or tears the
session down on `AXL_ERR`, needs the session marked closing BEFORE the
close. An accepted send therefore always gets exactly one callback,
which is what lets a caller free the buffer it lent the transport.
Closing an already-closed socket is a no-op, so a callback fired by a
close may itself close.

**A graceful close ends at the FIN exchange, not after TIME_WAIT.** EDK2's
TCP driver completes an active close only when TIME_WAIT (2 s) runs out, but
the close owes the wire nothing once both FINs are exchanged. So both ways a
graceful close is finished -- inline when no loop is running, or by the loop
it was deferred to (a server's closes) -- finalize on the TCP state instead:
at TIME_WAIT or CLOSED, and after about 500 ms in FIN_WAIT_2 (our FIN ACKed,
the peer's not in; counted in loop ticks, so longer under a slow driver tick). A server's closed connection is released a round trip after
its reply rather than 2 s later. `axl_tcp_closes_pending()` counts the
deferred closes not yet finalized -- each holds a few allocations, so a heap
reading is exact only when it is 0, and a program draining before
`axl_loop_free` runs its loop until it is.

**The firmware never holds caller memory.** Each piece of a send (at
most `chunk_bytes`) is copied into a buffer the socket owns just before
it is handed to TcpDxe, which reads it until it has segmented it into
its own send queue. So once a send's callback has run, nothing reads its
buffer again, whatever the status.

**Cancel stops a send at a piece boundary.** AxlTcp never cancels a
Transmit the firmware has accepted: on TcpDxe, `Cancel` on a Transmit
being processed unlinks the token while its data stays queued, and the
next segmentation then asserts (DEBUG) or signals a freed token (RELEASE).
So a cancelled send that is still QUEUED is dropped whole
(`AXL_CANCELLED`, 0 bytes); a cancelled ACTIVE send stops arming further
pieces and gets `AXL_CANCELLED` at once, while its armed piece -- the
socket's *orphan piece* -- goes out on its own and holds back the next
send until it completes; and an active send whose last piece is already
armed is reported `AXL_OK`, since nothing is left to stop.
`axl_tcp_send_get_sent(sock)`, read in the callback (or after a
synchronous `axl_tcp_send`, whose timeout takes the same path and
returns `AXL_TIMEOUT`), says how many bytes were committed, whole pieces
only -- BSD `send()`'s partial count. A byte stream resumes from
`buf + sent`; a framed protocol whose message was cut short closes.

**A close that would drop accepted bytes is abortive.** When a send is
still queued, the active send is only partly armed, or an earlier send
was retired with 0 < sent < size (the stream is truncated),
`axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL)` is promoted to
`AXL_TEARDOWN_RESET` (logged at DEBUG, `tcp` domain): a FIN would tell
the peer the stream ended cleanly, and the RST says it did not.
`Close(Abort)` discards the send queue, the socket is finalized
(`Configure(NULL)` + `DestroyChild`), and only THEN are the callbacks
delivered, so `AXL_CANCELLED` from a close means the bytes were not
delivered. A close with nothing dropped -- nothing pending, a finished
send, a fully-armed one, or only an orphan piece on an untruncated
stream (one left by a fully-armed send) -- stays graceful. In an
abortive close a fully-armed send's last piece is discarded by the RST,
so it gets `AXL_CANCELLED` with `axl_tcp_send_get_sent()` equal to its
size. To
deliver the last bytes and then FIN, close from the last send's
callback, as the HTTP server does for a `Connection: close` response.

That window is normally one loop iteration wide. A test holds it open
with `axl_tcp_fail_next_send_schedule(n)`: the Nth send RETIREMENT --
finished, cancelled or failed to arm, all counted -- then takes the path
it takes when the loop's defer queue and source table are both full, and
its callback runs only when the socket is closed. Never arm it around a
synchronous `axl_tcp_send`: that call's loop quits only from the held
callback, so it hangs.

**Session-scoped cancellation:**

Every `axl_tcp_*_async` call accepts an optional `AxlCancellable *`.
Share one cancellable across all ops tied to a session — closing the
session cancels every in-flight op at once, each firing its callback
once -- with `status == AXL_CANCELLED`, except a send that had already
finished or had its last piece armed, which reports `AXL_OK` (see the
piece-boundary rule above).

```c
typedef struct { AxlCancellable *cancel; AxlTcp *sock; } Session;

static bool on_connected(AxlTcp *sock, AxlStatus status, void *data) {
    Session *s = data;
    if (status == AXL_CANCELLED) return true;  // session closed before connect
    s->sock = sock;
    axl_tcp_recv_async(sock, s->rxbuf, sizeof(s->rxbuf),
                       loop, s->cancel, on_data, s);
    return true;  // connect fires once; return value ignored for connect
}

Session *s = axl_new0(Session);
s->cancel = axl_cancellable_new();
axl_tcp_connect_async(host, port, loop, s->cancel, on_connected, s);

// Later, from any handler -- user closes the tab, subsystem shuts
// down, a parent cancellable fires: every op tagged with s->cancel
// stops and its callback fires exactly once -- AXL_CANCELLED, or the
// real status of a send that was already fully handed to the firmware.
axl_cancellable_cancel(s->cancel);
```

### UDP Sockets

Fire-and-forget datagram sending, request-response patterns, plus
async receive / send and connection-style peer locking. Mirrors
`AxlTcp`'s async / cancellable / source-IP-pinning shape.

```c
AXL_AUTOPTR(AxlUdp) sock = NULL;
axl_udp_open(&sock, 0);                 // ephemeral local port
// or pin to a specific NIC:
// axl_udp_open_via(&sock, 0, &source_ip);

uint16_t bound;
char     bound_addr[16];
axl_udp_get_local_addr(sock, bound_addr, sizeof(bound_addr), &bound);

AxlIPv4Address dest;
axl_ipv4_parse("192.168.1.100", dest.addr);

// Fire-and-forget (sync, 2 s timeout)
axl_udp_send(sock, &dest, 514, msg, msg_len);

// Request-response (e.g., DNS query)
char reply[512];
size_t reply_len;
axl_udp_sendrecv(sock, &dest, 53, query, query_len,
                 reply, sizeof(reply), &reply_len, 3000);
```

**Async send + receive**, with optional `AxlCancellable`:

```c
bool on_recv(AxlUdp *s, AxlStatus status, const void *data, size_t len,
             const AxlIPv4Address *from, uint16_t from_port, void *udata) {
    if (status != AXL_OK) return false;   // err / cancel — stop
    process(data, len, from);
    return true;                          // re-arm for next datagram
}
axl_udp_recv_async(sock, loop, /*cancel=*/NULL, on_recv, NULL);

bool on_sent(AxlUdp *s, AxlStatus status, void *udata) {
    if (status != AXL_OK) log_warn("send failed");
    return true;                          // ignored for send
}
axl_udp_send_async(sock, &dest, 514, msg, msg_len,
                   loop, /*cancel=*/NULL, on_sent, NULL);
```

**Connection-style peer lock** — kernel-side recv filter plus
NULL-`dest` shorthand on send:

```c
axl_udp_connect(sock, &peer, 9999);
axl_udp_send(sock, NULL, 0, msg, msg_len);   // uses configured peer
axl_udp_disconnect(sock);                    // back to "send anywhere"
```

**Multicast / broadcast:**

```c
AxlIPv4Address mdns = { .addr = {224, 0, 0, 251} };
axl_udp_join_multicast(sock, &mdns);      // mDNS group
axl_udp_set_broadcast(sock, true);        // accept inbound broadcasts
// ... receive ...
axl_udp_leave_multicast(sock, NULL);      // leave all groups
```

## HTTP Server

Create an HTTP server with route handlers:

```c
void on_hello(AxlHttpRequest *req, AxlHttpResponse *resp, void *data) {
    axl_http_respond_text(resp, 200, "Hello from AXL!\n");
}

AXL_AUTOPTR(AxlHttpServer) s = axl_http_server_new(8080);
axl_http_server_add_route(s, "GET", "/hello", on_hello, NULL);
axl_http_server_run(s);  // blocks, serving requests
```

For multiple routes, the variadic batch form collapses the per-call
error checks into one:

```c
axl_http_server_add_routes(s,
    "GET",  "/version", on_version, NULL,
    "GET",  "/health",  on_health,  NULL,
    "POST", "/echo",    on_echo,    NULL,
    NULL);   // sentinel — required
```

#### Teardown: graceful vs. port-releasing

Teardown takes an `AxlTeardown` mode. `axl_http_server_free(s,
AXL_TEARDOWN_GRACEFUL)` closes idle connections *gracefully* (FIN) -- a
connection whose close would drop part of a response or WebSocket frame not
yet handed to the firmware is reset instead (see above) -- and,
when a loop is still running, defers the firmware teardown
(`Configure(NULL)` + `DestroyChild`) to a later loop tick — so the listen
port may stay bound until those closes finalize. That is fine for ordinary
shutdown, but not when the caller is about to **stop pumping the loop** (e.g.
block in `axl_image_run` for an in-place self-upgrade) and needs the port
back *now*.

> A graceful *connection* close that runs at a **raised TPL** (a driver-pump
> notify, `axl_loop_attach_driver` — not a foreground `axl_loop_run`) is
> promoted to an abortive **RST**: a graceful `EFI_TCP4.Close()` there flushes
> the send buffer + drives the FIN handshake, whose transmit completion needs the
> MNP timer to fire below `TPL_CALLBACK`, which the pump holds — so it would spin
> in firmware forever. A reset connection is an abandon anyway, so RST is the
> correct teardown at that level (see `tcp_close_impl`). The foreground path is
> unchanged (FIN), except that a close that would drop accepted bytes is
> abortive at any TPL.

`axl_http_server_free(s, AXL_TEARDOWN_RESET)` is the port-releasing teardown: it
RSTs the listener and every in-flight connection and finalizes **synchronously
and loop-free** before returning, so a fresh `axl_http_server_new(port)` +
`axl_http_server_start` on the same port succeeds **immediately with no loop
pumping**, even with connections in flight. The RST discards un-ACKed
in-flight bytes — the intended trade for a guaranteed, immediate port
release.

```c
axl_http_server_free(s, AXL_TEARDOWN_RESET);  // port :443 free on return
axl_image_run("fs0:\\new.efi", ...);           // child rebinds :443 immediately
```

`axl_tcp_close(listener, AXL_TEARDOWN_RESET)` is the transport-level primitive
that does the real work: it RSTs the listener, drains its firmware **accept
backlog**, and finalizes its connections' pending **deferred graceful closes** —
every place a PCB can linger on the port — synchronously and loop-free. Anything
that owns a TCP listener inherits the port-releasing teardown by tearing it down
through that call with `AXL_TEARDOWN_RESET`. The HTTP server does (above); the
BSD-style socket veneer exposes it as `axl_socket_free(sock, AXL_TEARDOWN_RESET)`
(both modes are identical for a datagram socket — UDP has no graceful close to
abort). UDP itself needs none of this: it is connectionless, so `axl_udp_close`
already releases the port synchronously. RAII (`AXL_AUTOPTR`) cleanup always uses
`AXL_TEARDOWN_GRACEFUL`.

### REST request helpers

For REST-shaped handlers, three helpers route through the
existing HTTP machinery so routes don't reinvent content
negotiation or JSON body parsing:

```c
int handle_request(AxlHttpRequest *req, AxlHttpResponse *resp, void *data) {
    if (axl_http_request_wants_json(req)) {
        // ...emit JSON
    }

    AxlJsonReader r;
    if (axl_http_request_get_json(req, &r)) {
        int64_t value;
        if (axl_json_get_int(&r, "key", &value)) { /* ... */ }
        axl_json_free(&r);
    }
    return 0;
}
```

`axl_http_request_accepts(req, mime)` is the underlying primitive
(case-insensitive, multi-type lists, wildcards, q-value tolerant).
`_wants_json` is the `application/json` shorthand. `_get_json`
parses `req->body` into a caller-owned `AxlJsonReader` (caller
frees with `axl_json_free`).

The server supports middleware, WebSocket endpoints, authentication,
response caching, streaming uploads, and WebDAV mounts. See the
API reference for details.

### WebDAV class-1 + MOVE/COPY

`axl_http_server_add_webdav(s, prefix, &ops, user_data)` mounts
a WebDAV handler at the given URL prefix. Verb scope:
OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY —
covers class-1 plus MOVE and COPY. PROPPATCH, LOCK, UNLOCK, and
If-header conditionals remain out of v1 scope (Windows Explorer,
macOS Finder, davfs2, cadaver work without them when the server
doesn't advertise the lock class).

The consumer fills in an `AxlWebDavOps` callback table mapped
onto its own filesystem; the SDK owns the protocol — verb
dispatch, PROPFIND 207 Multi-Status XML emit (driven by
`AxlXmlWriter`; see [`<axl/axl-xml.h>`](../data/README.md#axlxml--streaming-xml-writer--pull-token-reader)),
Depth / Destination / Overwrite header parsing, RFC 1123
Last-Modified date formatting, RFC 3230 Want-Digest /
Digest header negotiation (opt-in via the consumer's
optional `digest` callback), DAV: 1 advertisement on every
WebDAV-method response. GET inherits
`axl_http_response_set_streamer` (multi-GB safe, Range
requests via `axl_http_response_set_content_range`); PUT
inherits the upload-route chunk handler (write_open / chunk /
close(aborted)). `write_close(false)` commits a PUT's body and
`write_close(true)` discards it; a backend should make the commit the
moment the resource changes, so a discarded PUT -- the client hung up,
a chunk could not be written, the body did not match its
`Content-Digest` -- leaves the resource as it was. One PUT per mount at
a time: another PUT meanwhile, with a body or without one, is 503 with
`Retry-After: 1` and touches nothing. A PUT whose `write_open` fails is
403 when it returned `AXL_DENIED` (a read-only file), 507 for
`AXL_NO_RESOURCES` (a body the volume cannot hold, decided up front),
503 for `AXL_BUSY`, and 409 otherwise (a missing parent collection, an
existing file on a no-overwrite mount); a request path that does not
decode is 400, and one too long once decoded 414; a `write_chunk`
failure is 500. Either way the failure ends
with that PUT: the next one on the mount is unaffected. A
`Content-Digest` request header (RFC 9530 `sha-256=:<base64>:`, or bare
hex) is checked by the SDK over the body as it streams, before the
commit: every entry is read, the strongest the SDK hashes (sha-512,
else sha-256) is checked, and a mismatch is 400 with the write
discarded; a header naming only other algorithms checks nothing. A
commit can answer 409 (`AXL_IS_DIR`: a collection appeared at the target
while the body streamed; `AXL_EXISTS`: a file on a no-overwrite mount),
403 (`AXL_DENIED`) or 507 (`AXL_NO_RESOURCES`) as well as 500.

```c
static int my_stat(void *user, const char *path, AxlWebDavEntry *out);
static int my_list_dir(void *user, const char *path,
                       AxlWebDavEntry *out, size_t max, size_t *count);
static int my_read_open (void *user, const char *path,
                         uint64_t offset, void **out_ctx);
static int my_read_chunk(void *ctx, void *buf, size_t cap, size_t *got);
static void my_read_close(void *ctx);
/* ... write_open / write_chunk / write_close / mkdir / remove
       / move / copy / content_type ... */

static const AxlWebDavOps my_ops = {
    .stat         = my_stat,
    .list_dir     = my_list_dir,
    .read_open    = my_read_open,
    .read_chunk   = my_read_chunk,
    .read_close   = my_read_close,
    /* ... */
};

axl_http_server_add_webdav(server, "/dav", &my_ops, my_user_data);
```

MOVE and COPY answer RFC 4918's statuses before the op runs: 400 for
a missing or unparsable `Destination`, 502 for one on another server
(its authority is not the request's `Host`), 403 for a destination
that is the source itself or the mount root; with `stat` wired, 404
for a missing source and 412 for an existing destination under
`Overwrite: F`. A success is 204 when the destination existed and 201
when it did not. An op failure maps by its code: 403 for `AXL_DENIED`
or `AXL_INVALID`, 412 for `AXL_EXISTS`, 409 for `AXL_NOT_FOUND`,
`AXL_NOT_DIR`, `AXL_IS_DIR` or `AXL_NOT_EMPTY`, 502 for
`AXL_UNSUPPORTED`, 507 for `AXL_NO_RESOURCES`, and 500 for `AXL_ERR` and
anything else.

`axl_http_server_serve_fs` mounts a volume subtree with these ops
already written. Its MOVE is `axl_file_replace` (with
`AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE`) and its COPY that
replace's transaction with the source kept, so every refusal is decided
before anything changes, by the names the filesystem stores: onto the
source itself under its 8.3 alias is 403 with `Overwrite: T` (412 with
`Overwrite: F`, which the handler answers first), and onto a collection
holding the source or into its own subtree is 403; a MOVE that changes only the
name's letter case renames it on a case-insensitive volume (204, with
either Overwrite value) and is an overwrite of another entry on a
case-sensitive one; a volume without the room is 507. These decisions
are by identity, so the table sets `AXL_WEBDAV_CAP_IDENTITY`; a backend
of your own that does not keeps the default -- an existing destination
with `Overwrite: F` is 412, however its name is spelled. A copy of
`axl_fs_webdav_ops()` that overrides `move` or `copy` inherits the cap:
it must then refuse, with `overwrite` false, a destination that exists
and is a different entry (`AXL_EXISTS`), or clear `caps`.
An existing destination is renamed aside (`~AXLhhhh.TMP`), the source
moved or copied into its place, and only then the aside removed -- a
move that cannot happen costs nothing. An aside that cannot be removed
whole is left, and logged by name. `AXL_SERVE_FS_NO_OVERWRITE` refuses
a MOVE or COPY onto an existing destination with 412, a PUT with 409.
`AXL_SERVE_FS_NO_DELETE` refuses an overwrite that would remove
a collection (403), as it refuses DELETE; replacing a file stays
allowed. Its PUT is atomic: the body goes to a temp sibling
(`~APxxxxx.TMP`, an 8.3 name) that replaces the target -- by the same
aside -- only once it is complete and flushed, so a PUT that does not
complete leaves the old file exactly as it was. Replacing a file needs
room for both for a moment: a PUT whose `Content-Length` is more than
the volume's free space is 507 before a byte is written (a chunked one
finds out when a write fails: 500). The free space is what the volume
reports, and on FAT32 that is the FSInfo free-cluster count -- a hint,
which a volume another OS left inconsistent can misstate, so a false 507
(or a wrong in-place decision) is possible there until the count is
repaired (a `chkdsk`/`fsck` of the volume).

A mount that must take files too big for that opts into writing an
EXISTING file in place -- truncated, then the body streamed into it, no
temp. `AXL_SERVE_FS_PUT_IN_PLACE_IF_SHORT` does so only when the volume
cannot hold a second copy (the body's allocation units plus the
directory room the temp and the commit's replace take, counted as
`axl_file_replace` counts it -- a long name's entries included -- against
the free space; logged at info), and
stays atomic otherwise, for a chunked body (no length to decide by) and
on a volume that cannot say how much room it has.
`AXL_SERVE_FS_PUT_IN_PLACE` writes every overwrite in place. A NEW file
always goes through the temp: that costs no extra room and leaves
nothing behind a failed PUT. Every refusal still comes first -- a
read-only file, a collection, a no-overwrite mount, a live temp, and a
body that would not fit even in place (507). While it runs, the file is
busy for every WebDAV (serve-fs) mount in the process -- not for SFTP or
9P exports of the same volume, which do not consult it -- known by its stored names (an 8.3
alias or another letter case of it or of a directory on the way is the
same file): another PUT to it is 503 with `Retry-After: 1`, and a DELETE,
MOVE or COPY that would replace or remove it is 423 Locked. What is given up is the old
content of a PUT that does not complete: the file is left PARTIALLY
written and an error names it ("is partially written (N of M bytes); its
old content is gone"). A write failure answers 500 with the body `PUT
failed part-way: the resource is partially written`; a `Content-Digest`
mismatch -- which cannot be checked before the bytes land -- answers 400
with `Content-Digest mismatch: the resource now holds the body that did
not match`; a client that hangs up cannot be answered, so the log is
all there is. A hand-written backend reports the same by returning a
failure from `write_close(ctx, true)`, the discard -- after declaring the
write in place through `write_open`'s `*in_place`; for any other write the
discard's return is ignored. A write declared in place whose commit (the
final flush) fails answers 500 with the same "partially written" body.
`write_open` is told the body's length for such decisions: the
`Content-Length`, or `AXL_WEBDAV_LENGTH_UNKNOWN` for a chunked body.

A PUT replaces a file only: a collection
that appears at the target while the body streams is 409 at the
commit, never removed. A live PUT's temp is recognised by its name,
whatever spelling of its directory reaches it, and is hidden, not
copied, and refused (a PUT to it is 403); a temp-named file no PUT owns
(a power loss's leftover) is an ordinary file.

Paths are percent-decoded (RFC 3986 §2.1) before any op sees them --
the request path and `Destination` alike -- so the ops table is handed
names as they are stored (`/with space.txt`, `/été.txt`), and every
PROPFIND `href` is percent-encoded back (`/dav/with%20space.txt`). An
escape that decodes to a separator or a NUL (`%2F`, `%5C`, `%00`), a
malformed escape, or a raw `\` is 400: each would become a separator
inside what HTTP calls one segment. So is a component that starts with a
blank or ends with a blank or a period: EDK2 FAT trims those before it
looks a name up, so `x.TMP.` would open `x.TMP`, and it cannot store such
a name. `%2e%2e` is `..`, resolved like
any other -- inside the mount, never out of it. A PROPFIND with
`Depth: 1` lists every child, held on the heap: `list_dir` is asked again
with twice the room while a listing fills it (so a backend's `list_dir`
may be called several times per PROPFIND, and must return `count == max`
when it stopped short). There is no cap -- a partial 207 reads as the
whole directory to a sync client, which then deletes what it did not
see -- so a listing that cannot be held in memory is 507, and one that
fails is 500, never part of a directory.

Up to 4 WebDAV mounts per server. The `ops` struct is COPIED
into the server; the consumer may free or re-use it after
`add_webdav` returns. `user_data` is borrowed and must outlive
the server.

To gate a mount behind the server auth callback, use
`axl_http_server_add_webdav_auth(s, prefix, &ops, user_data, auth_flags)`
(or pass `auth_flags` to `axl_http_server_serve_fs`). The flags
(`AXL_ROUTE_AUTH` / `AXL_ROUTE_ADMIN`) apply to every verb route —
including the streaming PUT, which is enforced before any body byte.

### Streaming uploads

`axl_http_server_add_upload_route(server, method, path, handler, data)`
registers a route that streams the body to `handler` in chunks
instead of buffering — required for multi-GB uploads (the body never
materializes in RAM, bypasses `body.limit`).

The `AxlUploadHandler` callback distinguishes three terminating
shapes by the `chunk` and `aborted` arguments:

| chunk        | aborted | meaning                                                  |
| ------------ | ------- | -------------------------------------------------------- |
| `!= NULL`    | `false` | body chunk arrived; process it and return AXL_OK         |
| `NULL`, 0    | `false` | clean EOF — set `resp` fields, response is sent          |
| `NULL`, 0    | `true`  | TCP disconnect / recv error — release per-request state  |

The abort call is mutually exclusive with the clean-EOF call: a
handler that received the clean-EOF call will NOT also receive an
abort, even if the response send subsequently fails. On abort the
handler MUST NOT touch the connection or call any response setter
— it exists only to release per-request state (open file handles,
accumulators, allocations) accumulated across earlier chunk calls.
Without this signal, that state leaks across requests.

Middleware registered via `axl_http_server_use` runs before the
upload handler sees a single byte. On rejection the connection is
force-closed (clients almost always send body bytes before reading
the rejection — staying in keep-alive desyncs the next request).
Header-based gating only — the body isn't materialized so middleware
that needs the body can't apply to upload routes.

To auth-gate an upload route, register it with
`axl_http_server_add_upload_route_auth(server, method, path, handler,
data, auth_flags)`. Uploads bypass the normal dispatch auth check, so
this variant enforces the route's `auth_flags` directly — the server
auth callback runs before the first body byte (401 on failure, 403 for
an admin route presented a lesser role).

### Expect: 100-continue

A client that sends `Expect: 100-continue` holds its body back until the
server answers the headers (RFC 9110 10.1.1); curl does it for an upload of
unknown size (`curl -T -`) or of 1 MiB or more, and waits 1 s for an answer
before sending anyway. The server answers `HTTP/1.1 100 Continue` once it
knows it will read the body -- after an upload route's middleware and auth,
or, for any other route, once the route exists and its auth callback
accepts the headers -- and the final response follows on the same
connection. A request it would refuse without the body gets the final
status instead (404, 401/403, 413 for a Content-Length over `body.limit`, a
middleware's refusal), and the connection closes without waiting for the
body. No 100 goes to a request whose body arrived with its headers or to an
HTTP/1.0 request (which MUST ignore it); any other Expect value is 417.

The early check means an auth callback can run twice for one request --
once on the headers with `body` NULL, again with the body -- so it should
authenticate from the headers. A handler's own refusal (a WebDAV PUT that
finds its mount busy, say) still comes after the body, as without the field.

## HTTP Client

```c
AXL_AUTOPTR(AxlHttpClient) c = axl_http_client_new();
AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;

if (axl_http_get(c, "http://192.168.1.1:8080/api/status", &resp) == 0) {
    axl_printf("HTTP %zu\n", resp->status_code);
    if (resp->body != NULL) {
        axl_printf("%.*s\n", (int)resp->body_size, (char *)resp->body);
    }
}
```

HTTPS URLs are detected automatically; call `axl_tls_init()` once at
startup so the client has a TLS backend to use.

### How a response is read

The client reads a response as RFC 9112 asks of a user agent, and never
hands back a body it cannot vouch for:

- **Framing.** `Transfer-Encoding` wins over `Content-Length`: chunked
  when its last coding is chunked, otherwise the body runs until the
  server closes the connection. With neither header, the body also runs
  to the close — the usual `Connection: close` reply of embedded servers
  and BMCs. A response to `HEAD`, a 204 and a 304 have no body whatever
  their headers say. A connection whose response was framed by the close,
  or carried both headers, is not reused.
- **A body framed by the close ends only at an orderly close** — the
  server's FIN, or over TLS its `close_notify`. A reset part-way, or a TLS
  stream that ends without `close_notify`, fails the request with
  `AXL_INCOMPLETE`, and the error log says which. It is never returned
  as a short body.
- **Timeouts.** `timeout.ms` is an idle deadline, re-armed whenever bytes
  arrive. It passing while the response is awaited or part-way through a
  body fails the request with `AXL_TIMEOUT`, from the async client and
  `axl_http_request_streaming` alike.
- **Interim responses.** `100 Continue`, `103 Early Hints` and other 1xx
  heads are read past to the final response — at most 16 of them, then
  the request fails. A `101 Switching Protocols` fails the request: the
  client asks for no upgrade. A failed request's connection is never
  reused.
- **Header leniency.** Response headers follow a user agent's rules
  (`axl_http_parse_response_headers`): a folded line is unfolded,
  identical repeated `Content-Length` values merge, and bare-LF line ends
  are read; a bare CR, a NUL, or differing `Content-Length` values fail
  the response.

### Async HTTP client

`axl_http_get_async` / `axl_http_post_async` are the loop-integrated peers
of `axl_http_get` / `axl_http_post`. The whole request — DNS resolve, TCP
connect, TLS handshake, send, receive, redirects — runs as events on a
caller-supplied `AxlLoop` with **no nested loop**, so it is safe to issue
from inside a loop callback or a resident driver-pump tick at raised TPL
(`axl_loop_attach_driver`) — where the sync calls would nest an ephemeral
loop and warn.

```c
static void on_done(AxlHttpClientResponse *resp, AxlStatus st, void *user) {
    if (st == AXL_OK) {                 // non-2xx is still AXL_OK (inspect status_code)
        axl_printf("HTTP %zu\n", resp->status_code);
        axl_http_client_response_free(resp);   // the callback OWNS resp
    }
}

// Returns AXL_OK => on_done WILL fire later; AXL_BUSY if a request is already
// in flight on this client (one in flight per client — use separate clients
// for concurrency); any other error => on_done does NOT fire.
axl_http_post_async(c, loop, "https://host/webhook",
                    body, body_len, "application/json",
                    /*cancel=*/NULL, on_done, user);
```

The body is **borrowed** until the callback fires (not copied). Pass
`cb == NULL` for fire-and-forget (the response is freed internally). https
requires `axl_tls_init()` once at startup. The sync `axl_http_get/post`
remain available and unchanged.

## TLS (HTTPS)

TLS 1.2 support using [mbedTLS](https://github.com/Mbed-TLS/mbedtls) 3.6.
Provides HTTPS server/client, self-signed certificate generation, and
transparent TCP encryption.

**Compiled into every build.** There is no build flag: mbedTLS is an
unconditional dependency, and `--gc-sections` keeps the TLS stack out of
any image that never calls `axl_tls_init()`. What activates TLS is that
call, not a compile-time toggle — a client or server that never makes it
refuses `https://` at runtime.

Header: `<axl/axl-tls.h>`

AXL's TLS module wraps mbedTLS to provide:

- Self-signed ECDSA P-256 certificate generation
- TLS 1.2 server contexts (for HTTPS)
- TLS 1.2 client contexts (for HTTPS GET/POST)
- Transparent integration with **AxlHttpServer** and **AxlHttpClient**

### HTTPS Server

Generate a certificate and enable TLS on the HTTP server:

```c
#include <axl.h>

axl_tls_init();

// Generate self-signed cert (valid 10 years, ECDSA P-256)
void *cert, *key;
size_t cert_len, key_len;
axl_tls_generate_self_signed("MyServer", NULL, 0,
                             &cert, &cert_len, &key, &key_len);

// Create HTTPS server
AxlHttpServer *s = axl_http_server_new(8443);
axl_http_server_use_tls(s, cert, cert_len, key, key_len);
axl_http_server_add_route(s, "GET", "/", handler, NULL);

axl_free(cert);
axl_free(key);

axl_http_server_run(s);  // serves HTTPS
```

### HTTPS Client

HTTPS is automatic — just use an `https://` URL:

```c
AXL_AUTOPTR(AxlHttpClient) c = axl_http_client_new();
AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;

// TLS handshake happens automatically
axl_http_get(c, "https://192.168.1.1:8443/api/status", &resp);
```

A body framed by the connection's close ends in order over TLS only at the
server's `close_notify` alert; without it the request fails with
`AXL_INCOMPLETE`, logged as "TLS ended without close_notify after N
bytes" — truncation cannot be told from the end without it (RFC 8446
§6.1). That is the default, `tls.close_notify` = `"required"`. For a known
server that skips `close_notify`, opt in per client:

```c
axl_http_client_set(c, "tls.close_notify", "lenient");
```

A TCP FIN then ends such a body, with a WARNING naming the server; a reset
still fails with `AXL_INCOMPLETE`. `Content-Length` and chunked bodies end
by their own framing and are unaffected in either mode.

### Certificate Generation

`axl_tls_generate_self_signed` creates an ECDSA P-256 certificate
with SHA-256 signature:

- **Subject**: `CN=<name>,O=AximCode`
- **Validity**: current year to +10 years
- **Serial**: random positive 8-byte integer, minimal DER (RFC 5280
  4.1.2.2) -- a strict parser such as OpenSSL 3 rejects a certificate
  whose serial is not
- **SubjectAltName**: `DNS:localhost`, `IP:127.0.0.1`, plus any
  provided IP addresses
- **Output**: DER-encoded certificate and private key (caller frees)

### Entropy

mbedTLS needs random numbers for key generation and TLS handshakes.
There is exactly one entropy source, registered with a threshold of 32
and `MBEDTLS_ENTROPY_SOURCE_STRONG` (`MBEDTLS_NO_PLATFORM_ENTROPY`
means mbedTLS adds none of its own): **`EFI_RNG_PROTOCOL`**, reached
through `axl_rng_bytes()`.

If the firmware publishes no RNG, or `GetRNG` fails, the source
reports failure. The per-source threshold is then unmet,
`mbedtls_ctr_drbg_seed()` fails, and every call that needs randomness
returns its error or `NULL` sentinel — `axl_pk_key_new()`,
`axl_ecdh_new()`, `axl_tls_init()`, `axl_ssh_server_new()`. **No key
is produced.** This is deliberate: a key nobody can generate beats a
key an attacker can guess.

A platform that genuinely has no RNG, and whose operator accepts the
exposure, opts back in:

```c
axl_crypto_set_weak_entropy_allowed(true);   // BEFORE any crypto call
```

The seed then comes from the real-time clock and the firmware's
monotonic counter, and **keys become predictable to anyone who can
bound boot time**. Every draw that falls back logs at warning level.
Seeding is lazy and the seed is cached for the life of the process, so
the call must come before the first use of any crypto.

### Security Considerations

- Self-signed certificates are **not trusted** by browsers or standard
  TLS clients. Use `curl --insecure` or configure trust-on-first-use.
- Certificate verification is **disabled** for client connections
  (`MBEDTLS_SSL_VERIFY_NONE`). This is appropriate for BMC/embedded
  use but not for public internet TLS.
- The clock-seeded entropy fallback is **not cryptographically
  strong** and is **off by default**. Enabling it with
  `axl_crypto_set_weak_entropy_allowed(true)` makes every key this
  image generates predictable to anyone who can bound boot time; on
  hardware without an RNG, prefer supplying key material generated
  elsewhere over enabling it.
- **Any key generated by a build before `2787c468` (2026-09-07) is
  clock-derived and must be rotated, not trusted.** The entropy poll's
  hardware branch called the wrong `EFI_RNG_PROTOCOL` vtable member
  (`GetInfo` instead of `GetRNG`) and silently fell back to the clock
  seed above on every platform, including ones with a working RNG --
  the fallback's success return hid that the strong path had never
  worked. This affected every key this SDK produced through that
  commit: Ed25519 host keys, X25519 ephemerals, and TLS session keys
  alike.

## Public-Key Cryptography (AxlCrypto)

Header: `<axl/axl-crypto.h>`

`axl_pk_key_new` / `_sign` / `_verify` / `_load_private` / `_load_public`
generate, sign, and verify keys for ECDSA P-256 (SHA-256), ECDSA P-384
(SHA-384), and RSA (PKCS#1 v1.5 over SHA-256). ECDSA signatures use
either fixed-width raw `r||s` (what SSH/JWS/COSE expect) or ASN.1 DER;
RSA has a single encoding, so the format selector is ignored for it.
`axl_pk_verify` is a lower-level, one-shot form that verifies a
detached signature straight from a DER public key, with no key handle
involved.

### Provider seam

Internally, `AxlPkKey` dispatches through a per-algorithm vtable
(`AxlPkProvider`, `src/net/axl-pk-provider.h`) instead of assuming
every key is an mbedTLS one — mbedTLS 3.6.3 has no Ed25519 support at
all, so the Ed25519 provider cannot share the same
`mbedtls_pk_context` the ECDSA/RSA algorithms use.

One provider (`src/net/axl-pk-mbedtls.c`) serves ECDSA P-256, ECDSA
P-384, and RSA — they differ only by curve/key type, which the key
itself carries. A second (`src/net/axl-pk-ed25519.c`) serves Ed25519
over the vendored ref implementation in `deps/ed25519-ref10/`.
`axl_pk_alg_available` reports which algorithms are actually linked
into the current image.

### Every provider is opt-in, in code

There is no link flag. Each provider is reached through a **weak symbol**,
and an image opts in by **calling** its enable function — the same shape as
`axl_tls_init()`, where calling the function is what links the code:

| algorithms | call | who calls it for you |
|---|---|---|
| ECDSA P-256 / P-384, RSA | `axl_pk_enable_mbedtls()` | AxlJose; every ECDSA/RSA-specific function (`axl_pk_verify`, `axl_pk_key_from_ec_xy`, `axl_pk_key_get_ec_xy`, `axl_pk_key_from_rsa_ne`, `axl_pk_key_get_rsa_ne`) |
| Ed25519 | `axl_pk_enable_ed25519()` | AxlSsh (`axl_ssh_server_new()`) |

Each enable function is defined in its provider's own object and has an
empty body: a call to it makes the linker take that object, which defines
the provider and resolves the dispatcher's weak reference. There is no
run-time state, so the call is idempotent and order-free — the provider is
in the image from the first instruction or not at all. The functions that
"call it for you" do so the same way: AxlJose calls
`axl_pk_enable_mbedtls()`, AxlSsh calls `axl_pk_enable_ed25519()`, and the
ECDSA/RSA-specific functions live in `axl-pk-mbedtls.c` itself. TLS needs
nothing: it reaches mbedTLS's RSA and x509 code directly and never touches
AxlPk.

So only an image that reaches an algorithm through the generic
`axl_pk_key_new` / `axl_pk_key_load_private` / `axl_pk_key_load_public` /
`axl_pk_key_from_raw_public` alone must make the call. Without it those
calls return NULL and log one error line in the `pk` domain naming the
function:

```
ECDSA/RSA unavailable: call axl_pk_enable_mbedtls() before using ECDSA/RSA keys
Ed25519 unavailable: call axl_pk_enable_ed25519() before using Ed25519 keys
```

`axl_pk_alg_available` stays silent — it is how a caller asks first. A DER
import logs a line only when the bytes name the id-ecPublicKey,
rsaEncryption or id-Ed25519 OID and that algorithm's provider is absent, so
malformed DER is not blamed on a missing provider.

**What each costs.** Ed25519, measured x64 `-Os` 2026-09-05: **124,567
bytes** — 13,968 `.text` and 110,599 `.rodata`, almost all of it one array,
`ge25519_base_multiples_affine`, the precomputed base-point table, at
**108,800 bytes**. That is not the ~30 KB figure the design documents
assumed; the vendored code is SUPERCOP's `ref`, not `ref10`, and it stores a
field element as `crypto_uint32[32]` rather than `int32[10]` (see
`deps/ed25519-ref10/README.md`). The mbedTLS provider carries mbedTLS's PK
layer: `tools/sshd.efi`, x64 `BUILD=RELEASE`, went from **220,160 to
201,728 bytes shipped** (517,120 raw) when it stopped carrying it,
measured 2026-10-20.

**Why the whole mbedTLS provider, not just RSA.** mbedTLS's generic PK
layer reaches `rsa.o` on its own — `mbedtls_pk_info_from_type` returns
`&mbedtls_rsa_info`, `pkparse` and `pkwrite` call the RSA parsers and
writers — so with the ECDSA half still strongly referenced, removing every
direct RSA call from the provider left the Ed25519-only `sshd` driver with
37 of its 38 `mbedtls_rsa` symbols. With the provider weak, it carries none
of `pk` / `pkparse` / `pkwrite` / `ecdsa` / `rsa`.

**The "without" side is zero, checked by symbol rather than by size.**
`AxlTestJose` reaches `_axl_pk_provider_for()` and carries **0** `ge25519` /
`fe25519` / `sc25519` / `crypto_sign_ed25519` symbols; `AxlTestEd25519` and
`sshd-dxe` carry **0** `mbedtls_rsa` / `mbedtls_pk_` / `ecdsa` symbols.
(`AxlTestNet` and `fetch` carry 20-34 mbedTLS `pk` symbols, but through
TLS's x509 code: they never link the dispatcher at all — re-checked with
`nm` 2026-10-20.) `AxlTestCrypto` also carries the vendored Ed25519 code,
because it calls the vendored entry point directly to pin the vendoring
itself — but not the provider.

The tests are two deliberate pairs, because one binary cannot assert both
halves of a weak seam: `AxlTestCrypto` enables mbedTLS and not Ed25519, and
asserts ECDSA/RSA work while Ed25519 fails closed with its exact line;
`AxlTestEd25519` is the mirror image.

### Ed25519 refuses small-order public keys

`axl-pk-ed25519.c` rejects all fourteen encodings of the Curve25519
8-torsion before it calls the vendored verifier, which checks only for
the identity. The gap is forgeable: under a public key of order 2,
verification reduces to `[S]B == R + [h]A` with `[h]A` the identity
whenever `h` is even, so `S = 1` with `R = encode(B)` verifies with no
private key for about half of all messages. Measured against this
provider before the check existed — the identity forgery was refused
and the order-2 one was accepted.

## AxlSsh — an SSH server

`<axl/axl-ssh-server.h>`, `<axl/axl-ssh-channel.h>` and
`<axl/axl-ssh-core.h>`. An SSH server over
`AxlTcp` + `AxlLoop`, built here rather than ported: wolfSSH is GPLv3,
Dropbear's structure is `fork`/`forkpty`/`select`, and libssh's LGPL relink
clause sits badly with static firmware linking. See
`docs/superpowers/specs/2026-08-21-axl-ssh-design.md` §3.

**P3 ships password authentication alongside publickey.** A stock `ssh -i key`
completes version exchange, KEXINIT, curve25519 key exchange and NEWKEYS, then
authenticates by publickey against the keys given to
`axl_ssh_server_add_authorized_key()` or loaded in bulk with
`axl_ssh_server_load_authorized_keys()` — an OpenSSH-format `authorized_keys`
file, exactly what `ssh-keygen -t ed25519` writes — or by password, or both.
Password authentication is not built in: the application installs it through
`axl_ssh_server_set_auth_ops()`, and AxlSsh stores no credential of its own.
`axl_ssh_server_listen()` now refuses to bind unless it can authenticate
*somebody*: **an authorized key OR a password hook**, since either alone can
answer every login and an empty set of both could authenticate nobody, and
opening the port would offer network surface for nothing. It refuses on the
same grounds when the platform has **no readable monotonic clock** — an
unprogrammed `CNTFRQ_EL0`, a failed TSC calibration — because that clock is
what enforces the login grace, and without it peers that finish key
exchange and then stall would hold every pre-authentication slot for good
(admission, below, evicts only peers that have NOT finished it).

**The login grace is settable, not fixed at 60 s** (`axl_ssh_server_set_login_grace()`,
added after the P3 final review: the hostile-input suite used to spend
~140 s of its ~280 s waiting out the shipped default twice). It must be
called before `axl_ssh_server_listen()`, like `set_auth_ops()`; zero is
refused rather than treated as "no grace" — that would hand a peer back the
exact denial of service the grace exists to prevent — and so is anything
over one hour, a bound that exists to catch a units mistake rather than to
limit a real use. The reaper's own sweep is normally coarse (5 s), but
`listen()` sizes it down for a short grace so a 1 s grace does not silently
become a 6 s one; that finer sweep then runs for the server's whole life,
not just near any one connection's deadline. And because authentication
hooks run one at a time server-wide, a queued connection's grace keeps
running while it waits its turn — a grace shorter than (connections queued)
x (that hook's latency) reaps logins that never did anything wrong, so size
it for the slowest hook installed, not for the common case. When it happens
anyway the reap now SAYS so (`after waiting N ms behind another
connection's authentication hook`) instead of reading like a peer that sent
nothing, and the hook that held it up is reported in its own right —
`axl_ssh_server_set_auth_hook_budget_ms()`, whose default is derived from
this grace and the connection cap, so shortening the grace wants a smaller
budget with it.

**The password cap is settable too, and unlike the grace it is LIVE**
(`axl_ssh_server_set_max_password_failures()`, AxlSsh's counterpart of
sshd's `MaxAuthTries`; the default is
`AXL_SSH_PASSWORD_FAILURES_DEFAULT`, 3, and the ceiling
`AXL_SSH_PASSWORD_FAILURES_MAX`, 10). It may be called at any time from the
server's loop, including while it is listening and from an authentication
hook, because **nothing enforces it from the server**: the cap is copied
into each connection at accept and the reply path compares against that
copy. So a change reaches connections accepted afterwards and leaves open
ones alone — which is exactly what sshd does on a reload, since it forks a
process per connection with the configuration then in force. Zero is
refused: "no password authentication" is installing no password hook, not a
cap of nothing. The ceiling is 10 because a password is also one of the
twenty authentication requests a connection may send, so a cap above that
would be pre-empted by the request bound and the server would advertise one
number while enforcing another; a `_Static_assert` ties the two together.

**Admission is by eviction, and the accept never pauses for capacity**
(`axl_ssh_server_set_max_startups()`, AxlSsh's counterpart of sshd's
`MaxStartups`; default `AXL_SSH_MAX_STARTUPS_DEFAULT`, 10, ceiling
`AXL_SSH_MAX_STARTUPS_MAX`, 16 -- the connection cap). A new connection is
admitted while fewer than 16 are open and fewer than `max_startups` are
unauthenticated. Otherwise the server EVICTS the unauthenticated connection
least far along in key exchange — still waiting for the peer's
identification line, then its KEXINIT, its key exchange message, its
NEWKEYS — the oldest among equals, and admits the new one; only when every
unauthenticated connection is past key exchange is the new one REFUSED,
with an abortive close and nothing sent. Progress ranks first because age
alone gives a real client a clock: it would survive only about
`max_startups` further arrivals, so a sustained flood could evict it
mid-key-exchange or while its user answers ssh(1)'s host-key prompt. So a
flood that opens connections and says nothing cannot displace a client that
has sent its KEXINIT (one round trip after connecting), whether the flood
has stopped or is still arriving — tested to about sixty connections a
second through QEMU's user network, above which new connections stopped
reaching the guest at all, on the pre-P4a code too. What admission does NOT
stop: a flood whose peers send their own KEXINIT and key exchange message
(no cryptography needed on their side) ties with a client at the same step,
where age decides; and a flood that completes real key exchanges is past
eviction, so arrivals are refused until the login grace reaps it — sshd has
no defence against that either beyond per-source penalties and the grace,
both of which apply here. Both decisions are audit lines at INFO, each under its own rate-limit
window: `Evicted connection from A port P: past MaxStartups, key exchange
unfinished` and `Refused connection from A port P: past MaxStartups`. An
evicted connection accrues its source `noauth`, like any connection that
ends unauthenticated; a refused one accrues nothing. Every unauthenticated
connection counts toward the cap, in any state — including one being torn
down while its authentication hook runs, which counts until it is gone.
Like the password cap, the value is LIVE and read at each arrival --
lowering it evicts nothing by itself, and since an eviction swaps one
connection for another, only connections ending on their own bring a server
back under a lowered cap. The accept used to pause at the connection cap
instead, which let 16 silent peers hold every slot for a whole login grace,
and made every lost
re-arm a server gone deaf; now only `MAX_ACCEPT_ERRS` consecutive failed
accepts pause it. `test-ssh-admission-qemu.sh` proves the victim order by
age and by progress, the refusal when no victim exists, that an
authenticated connection is never touched, and liveness after a
70-connection flood; `test-ssh-live-flood-qemu.sh` proves logins, and a
client held at the host-key prompt, survive a flood that is still
running.

**Sends are coalesced, and a peer that stops reading is paused, not
buffered.** A connection has at most one TCP send in flight; every packet
sealed meanwhile leaves in the next one, so a burst of replies costs a few
sends rather than one each (x64 61.4 → 7.8 µs per pipelined round trip,
RELEASE). Every reply answers one of the peer's requests, so past 128 KiB
queued to send (sshd's figure) the server stops parsing and stops reading
that connection: TCP's window closes on the peer, nothing is dropped, and
reading resumes once the queue drains below 64 KiB. Each pause and its end
are one DEBUG line each.

**A pre-authentication banner is settable, and LIVE too**
(`axl_ssh_server_set_banner()`, sshd's `Banner`; up to
`AXL_SSH_BANNER_MAX`, 4096 bytes — sized so the DoD Notice and Consent
Banner, about 1.3 KB, fits, which the 1024 of `AXL_SSH_AUTH_BANNER_MAX`
would refuse). It goes out as `SSH_MSG_USERAUTH_BANNER` ahead of the reply
to a connection's FIRST authentication request, whatever user name or
method it asks with — AxlSsh has no user database, so it could not tell a
known name from an unknown one even if it wanted to. **Once per
connection**, decided by that first request rather than by a flag: a flag
would deliver a banner set mid-authentication between two password prompts.
It must be valid UTF-8, which is what RFC 4252 §5.4 specifies for the field
(`axl_utf8_validate()`); that is conformance caught at configuration time,
not a filter — the RFC puts control-character filtering on the client, and
`ssh(1)` does it (measured against 9.9p1: TAB, LF and CR pass through,
every other control byte becomes a four-character octal escape, non-ASCII
survives only under a UTF-8 locale, a banner with no final newline leaves
its last line unterminated, and both `-q` and `LogLevel=ERROR` suppress the
banner -- `-q` by printing nothing at all, `ERROR` by withholding the banner
and keeping the client's own error line). **If the banner cannot be sent the
connection is dropped**, because a mandated notice exists to be shown before
access. NULL
or `""` removes it, which is the default and sshd's `Banner none`.
Separate from the per-request banner an `axl_ssh_auth_complete()` verdict
may carry: a connection can be sent both, this one first.

**The verdict — password or the publickey principal — may complete later, not
just inline**, through `axl_ssh_auth_complete()`. That exists because a real
password check is not free: SoftBMC's PBKDF2 costs 15,000 iterations, and
running that inline on the server's own event loop would stall every other
connection for as long as the hash takes. A hook may instead hand the request
to `AxlAsync` and answer from its done callback on another core, or complete
synchronously inside the hook when the check really is cheap — both paths go
through the same call. RFC 4252 section 5.1 is enforced either way: a
connection reads nothing further once its request is sent, until a verdict
arrives. A verdict that arrives after `axl_ssh_server_free()` — which is what
freeing the server before `axl_async_free()` delivers, since that drain runs
pending done callbacks — returns `AXL_NOT_FOUND` without the freed server being
read, and request handles are unique across every server in the image, so a
restarted server cannot inherit an old verdict. Cancel outstanding work before
teardown anyway: it is wasted, and it may hold a copy of a password.
`axl_ssh_auth_complete()` itself calls no application code and dispatches no
loop: it applies the verdict and lets the loop resume the connection, so a
completion cannot re-enter the callback it was made from.

**The server knows when application code is on its stack**, because a hook and
`release_principal` may run the loop, and everything that dispatch runs sits on
top of server frames that still hold the server and a connection. Counting
those calls buys two properties (P3 security review MED-1 and MED-3). *One hook
at a time*: a connection that would read authentication input while a hook is
on the stack stops before decrypting and is queued and served strictly in
arrival order once the hook returns — so a hook that only ever answers from
`axl_ssh_auth_complete()` (never by blocking inline) costs the logins behind it
nothing worse than its own latency, where before each connection could start
its hook inside the one below it, sixteen deep, and hold a legitimate login
open past its grace. **A hook that DOES block, though, serialises every login
on the server for as long as it runs**: with sixteen connections available to
a peer, that cost is `(peer connections) x W` — answering from a completion
instead of blocking is immune to it entirely. That trade was settled by the
owner on 2026-09-26 (P3 security re-review NEW-1; spec §12): hooks stay
strictly serial, and the effort goes into making the stall VISIBLE rather
than into concurrency. So a hook that holds the slot past a budget is
reported — `axl_ssh_server_set_auth_hook_budget_ms()`, default 1 s, 0 to turn
the reports off, **logging only**, because a knob that changed enforcement
would invite an operator to set it and believe they were protected when the
library cannot take a hook off the stack. Two lines, and they are not one
event: one when a hook RETURNS over budget, carrying the method, the user,
the source, the hold and the queue depth behind it (that depth is what makes
the cost server-wide rather than local), and one BEST-EFFORT line while a
hook is still running, at most one per call. The second rides the reaper's
existing sweep rather than a timer of its own, which is not a cost decision:
`axl_loop_add_timeout` arms an `EVT_TIMER` at `TPL_APPLICATION` with no
notify, so a per-hook timer would fire exactly where the sweep does and
detect nothing extra, while competing for the loop's source table at the
moment the server is already degraded. **And there is a case it cannot see
at all**: a hook that blocks WITHOUT dispatching the loop produces no
mid-hook line, because the sweep that would write it is dispatched from the
stack that hook is holding — a hook that can genuinely wedge has to be
watched from outside the loop (`axl_watchdog_set()`, or a BMC watchdog
further out). The reaper says why too: a connection dropped while it was
still queued names what it was waiting on rather than reading as a silent
peer, which is the one state the line used to be silent about.

*And a free from anywhere*: `axl_ssh_server_free()` may be
called from a hook, from `release_principal`, or from an unrelated handler
their dispatch runs. The server stops at once — port released, the host key
and timing secret zeroised, and every connection dropped **except the one
whose own hook is running**, which is only marked for drop; that one is
closed, and its principal released if it held one, once the hook returns —
which can be after `axl_ssh_server_free()` has already returned to ITS
caller, so do not tear down the principal registry or a hook's `user_data`
on the strength of that call having returned (P3 security re-review NEW-2).
Freeing the loop before dispatching it again leaks the (zeroised) struct and
never makes that last release call at all, which is why the header asks for
server, loop run, loop. Once a server's own free has started, every other
public call on it — `listen()`, `set_auth_ops()`, `set_login_grace()`,
`set_penalties()`, `set_penalty_exempt()`,
`set_max_password_failures()`, `set_max_startups()`, `set_banner()`,
`set_auth_hook_budget_ms()`, the authorized-key loaders,
and (for a DIFFERENT reason: its handles are unique image-wide) a late
`axl_ssh_auth_complete()` — is refused rather than reaching a stale pointer;
the check is by address, though, so a server freed and then replaced by a new
one at the same address is acted on as if it were that new server, for
everything except a completion (NEW-4). **At most 8 servers may be live in
one image** (`AxlSshAuthRequest`'s handles and this cap both exist to make a
late completion safe without reading freed memory).

**The failure policy is asymmetric, and deliberately so.** A key not in the
authorized set is refused **without dropping the connection** — that is
`ssh` walking its key list one candidate at a time, not an attack — while a
bad signature under an AUTHORIZED key ends the connection immediately,
whether or not the client asked about the key first. A key that is
well-formed but cryptographically unusable (a non-canonical encoding, a
small-order point) loads successfully and is refused only at verify time,
because the Ed25519 provider validates at the one door every signature
passes through rather than at every import site (see "Ed25519 refuses
small-order public keys" above). A wrong password gets the same non-fatal
refusal as an unauthorized key, but only up to a point: **the wrong
password that reaches the connection's cap ends it** (three, with the
defaults); so does a request that changes the user name (RFC 4252 section 5
says flush or disconnect, and sshd disconnects too, since every count here
is per connection); and a
publickey login whose signature verifies but whose principal the
application's hook refuses is an ordinary failure, not forgery — `ssh` may
still try something else. A password that is correct but flagged for a
required change gets `AXL_SSH_AUTH_CHANGE_FIRST`: refused exactly like a
wrong password (it counts toward the same cap), but with a
`USERAUTH_BANNER` sent first so the client can say why — a per-request
banner, distinct from the pre-authentication one above, and both can reach
one connection. AxlSsh implements no change-password form itself.

**A failure is replied to no earlier than a per-user minimum**, borrowed from
OpenSSH: 5 ms plus 0-4.29 ms derived from `SHA-512(secret||user)`, so the same
user name always waits the same extra amount and different names differ, with
one server-wide reply timer rather than a timer per connection. **Its honest
limit:** this hides the reply's own timing only when the check the
application runs costs the same for every user — a hook that returns early
for an unknown user leaks exactly that difference, and nothing in AxlSsh can
undo it once the hook has already answered faster than the minimum. Measured
against `ssh(1)`: about 10 ms added per refused query, consistent with the
5-9.3 ms minimum plus rounding and timer latency.

**A source that keeps failing is refused for a while** — OpenSSH's
`PerSourcePenalties`, on by default at OpenSSH's own numbers, and
**measured** against a throwaway `sshd` 9.9p1 rather than recalled. The
password cap ends one connection and nothing more; this is what bounds
guessing across reconnects. Each connection that ends without
authenticating charges its source address once: `authfail` (5 s) if a real
attempt was refused by either method — including a forged signature, which
sends no failure reply at all — `grace-exceeded` (10 s) if the login grace
reaped it, `noauth` (1 s) otherwise; a successful login charges nothing and
clears nothing, a refused connection accrues nothing, and neither does a
connection the SERVER failed (a resource failure while setting it up, or a
login grace it could not time), since a peer may have induced that pressure
and legitimate operators would pay for it -- while a signature the server ran
out of memory CHECKING is no forgery, and is charged like any other failure
of the server's own mid-parse, by what the peer did before. **Nothing is
enforced until a source's penalty exceeds `min` (15 s)** — so an operator who
mistypes three times with the defaults, is disconnected and reconnects is
never refused — **and then it is enforced until it has burned down to
nothing**, capped at `max`
(10 min): `min` decides when refusing starts, not how long it lasts. sshd
logged `activating ipv4 penalty of 19 seconds` after four failed connections
and refused every connection for the next 20 s. A refused connection is
accepted and reset at once, before the identification line: no key exchange,
no hook, **no connection slot**, and the accept stays armed. sshd writes
`Not allowed at this time` first, visible only at `ssh -v`; AxlSsh writes
nothing, because text queued ahead of an RST can be lost with it. **The RST is
load-bearing, not a style choice:** a socket fresh from accept has had no
asynchronous operation, so a GRACEFUL close from inside the accept callback
cannot defer — it waits inline for the FIN handshake: a round trip, 500 ms
for a peer that ACKs the FIN without closing, up to 3 s for one that never
ACKs it, per refused connection, stalling every service on the loop. An
abortive close has no such wait: EDK2 completes it synchronously inside
`Close()`. A line before the refusal, if ever wanted, needs an
asynchronous send with the RST from its completion — never a graceful close.
An activation is logged at INFO; each refused connection only at DEBUG, so a
reconnect loop cannot flood a firmware console.

`axl_ssh_server_set_penalties()` tunes it, or turns it off with NULL — and
refuses the two configurations that would silently never refuse anything
(`min >= max`, and three zero accruals). `axl_ssh_server_set_penalty_exempt()`
names up to 8 CIDR ranges it never applies to, parsed with
`axl_ipv4_parse_cidr()`: a BMC behind a jump host, or on a flat management
LAN, sees every operator from one address, and without an exemption one
operator's typos throttle all of them (`AXL_SSH_PENALTY_EXEMPT_MAX`). Both
are before-`listen()` calls and independent of each other.
`axl_ssh_penalty_range_check()` judges one range by the exempt setter's own
rule without a server -- the same function decides both -- so a
configuration loader can refuse a bad range at the line it came from before
any server exists. The table holds **256 sources**, ranked for
retention active-first and then by time left, so a burst of scans cannot
evict a source that is being refused — and sized against the addresses a
guesser can field rather than the connection cap, because **a full table is
a bypass, not a slowdown**: a source whose accrual cannot be recorded is not
throttled at all. It was 32, which a flat management LAN can outnumber
trivially; 256 is about 4 KB per server and past what a /24 can field (sshd's
threshold for the same effect is 65536). The first dropped accrual in each
`max` window is logged at INFO. **Its honest limits:** a patient guesser is
throttled — about twelve guesses per twenty seconds per address with the
defaults — not locked out; it is keyed by address, so hosts behind one NAT
share a penalty; a guesser spread over many addresses is bounded per
address, not in total, and one that keeps more than 256 of them tracked can
leave its next address untracked entirely; and **SoftBMC's web login is not
covered** (spec §13.4).

**Session channels are opt-in** (`<axl/axl-ssh-channel.h>`). Until the
application installs channel hooks with `axl_ssh_server_set_channel_ops()`
(before listening), every `CHANNEL_OPEN` is refused with
`SSH_OPEN_UNKNOWN_CHANNEL_TYPE`, as it always was. With them, a `session`
channel opens — `ssh host`, `ssh -tt host`, `ssh host cmd`, and every
session multiplexed through `ssh -o ControlMaster` — and the application
sees it through hooks: `open` (where it can refuse, and attach state with
`axl_ssh_channel_set_user_data()`), `pty`, `shell`, `exec`, `subsystem`,
`resize`, `data`, `eof`, `writable` and `close`. Every other channel type
(`x11`, port forwarding) and every request but `pty-req`, `shell`, `exec`,
`subsystem` and `window-change` stays refused. **Subsystems are the
application's**: AxlSsh implements none, `sftp` included. The `subsystem`
hook receives the name the peer asked for and accepts or refuses it, exactly
as the `exec` hook answers a command; an accepted channel then carries the
subsystem's byte stream through the ordinary `data`, `eof`, `writable` and
`close` hooks. Without the hook, `subsystem` is refused unparsed, as before
the hook existed. A connection holds at most
`AXL_SSH_MAX_SESSIONS_DEFAULT` (10, sshd's `MaxSessions`) channels; the
eleventh open draws `SSH_OPEN_RESOURCE_SHORTAGE` and the connection stays
up, and `axl_ssh_server_set_max_sessions()` lowers the cap for connections
accepted afterwards. A channel is an integer handle (`AxlSshChannel`), so a
handle kept past its close — written to from the application's own timer,
say — is `AXL_NOT_FOUND`, never a use-after-free. Any other channel type draws
`SSH_OPEN_UNKNOWN_CHANNEL_TYPE` and the connection stays up, so `ssh -L`,
`-W` and `-J` fail cleanly instead of ending the session.

**Open-failure reasons differ from sshd's, on purpose.** sshd answers every
refused session open with `CONNECT_FAILED`; AxlSsh sends
`RESOURCE_SHORTAGE` past the cap (so the eleventh `ControlMaster` session
reports "resource shortage") and `ADMINISTRATIVELY_PROHIBITED` when the
`open` hook refuses. A refused open never gets a `close` hook.

**Writing is all-or-nothing, bounded and edge-signalled.**
`axl_ssh_channel_write()` (and `_write_stderr()`, which shares the queue and
its order) queues the whole buffer or returns `AXL_BUSY` and queues nothing,
past 256 KB on the channel or 4 MB across the server
(`axl_ssh_server_set_channel_write_limit()`); a small fair-share floor keeps
channels whose peers stopped reading from starving the rest of the total.
The channel's `writable` hook fires once per refusal, from the loop, when a
retry would fit. Output leaves from a deferred flush, never inside the call:
in write order, in packets of at most 32 KB, never past the peer's window —
and only while the connection's transmit queue is under 64 KiB, so the
output bound above still holds. A request's reply is sealed when its hook
returns, so anything the hook wrote reaches the peer after it.
`axl_ssh_channel_exit()` drains the queue and then sends `exit-status`, EOF
and CLOSE; `axl_ssh_channel_close()` discards the queue and sends CLOSE now.
The `close` hook runs exactly once per opened channel, whatever ended it —
never for an open the `open` hook refused, never while another hook of that
channel is on the stack, and always before the connection's
`release_principal`. While any channel hook of a connection runs, that
connection parses nothing: the `data` hook is handed a pointer into the
connection's one payload buffer, not a copy. `test-ssh-session-qemu.sh`
drives all of it with a real `ssh(1)`.

*Limits and the floor.* A write is judged against the channel's own limit
(`AXL_SSH_CHANNEL_WRITE_LIMIT_DEFAULT`, 256 KB) and then the server-wide
total (`AXL_SSH_CHANNEL_WRITE_TOTAL_DEFAULT`, 4 MB, at most
`AXL_SSH_CHANNEL_WRITE_TOTAL_MAX`, 64 MB). A write that leaves its channel at
or under the **fair-share floor** — the total divided by the most channels
the server can hold, 26,214 bytes at the defaults — is accepted even when
the total is reached, which is what stops channels whose peers never open a
window from starving the healthy ones; queued bytes can therefore reach twice
the total. A single write longer than the channel limit is `AXL_ERR`, not
`AXL_BUSY`, because no wait could help. Under pressure on the total, split
large output into floor-sized writes.

*The `writable` edge.* It fires once per refusal (any number of refusals
are owed one call), from the loop and never inside a write, and only when a
retry of the largest refused write would fit; "below half" alone is not
enough, since that can already be true at the refusal and the application
would retry in a loop. A channel that has exited or closed is owed nothing.
A channel refused on the total is woken by bytes leaving ANY connection of
the server, not only its own. `test-ssh-channel-budget-qemu.sh` drives the
total, the floor and that cross-connection wake in a running server, with
two connections and a lowered limit.

*Closing.* `axl_ssh_channel_close()` retires the handle at once, but the
channel's wire slot (and its place under `max_sessions`) stays until the
peer's own CLOSE; what the peer sent before it saw ours — data, EOF,
`WINDOW_ADJUST`, requests — is ignored, not fatal. On the peer's CLOSE the
server answers at once and drops whatever is still queued, including a
pending `axl_ssh_channel_exit()` drain, as sshd does. The `close` hook is
passed the channel's `chan_data` (the pointer from
`axl_ssh_channel_set_user_data()`) because every call on the retired handle
is `AXL_NOT_FOUND`.

*What a hook may call.* Every `axl_ssh_channel_*` call and every server
setter, from any channel hook, on any channel including its own; none runs
a hook of the same channel inside itself. A hook may dispatch the loop, but
then its connection reads nothing, so it must never wait for what that
connection's peer has to send (a window adjust, data, a CLOSE): write,
return, and continue from `writable`.

*Inbound.* A channel is confirmed with window 0 and gets its 2 MB window
when `shell` or `exec` accepts, as sshd does; data sent before then is
discarded. Up to 10% over the window is accepted, a packet over 32 KB is
dropped, and data after the peer's EOF is counted and discarded; extended
data after EOF, a NUL inside an `exec` command or terminal name, and a
second `pty-req` end the connection, as in sshd. `test-ssh-channel-hostile-qemu.sh`
drives these byte by byte.

**An authenticated connection rekeys, and either side may start it.** The
client starts one whenever it likes (`ssh -o RekeyLimit=...`); the server
starts one when either direction has carried more than
`axl_ssh_server_set_rekey_limit()` bytes under its current key (default
`AXL_SSH_REKEY_LIMIT_DEFAULT`, 1 GiB; 0 = never by bytes; under 16 refused,
sshd's floor) or more than 2^31
packets whatever the limit, since the wire's sequence number — the cipher's
nonce — is 32 bits. There is no time limit, as there is none in a stock
sshd. Every rule is OpenSSH 9.9p1's, read from its source
(`src/net/axl-ssh-rekey.h` cites each): the counters reset per direction at
that direction's NEWKEYS; from our KEXINIT to our NEWKEYS only key-exchange
messages leave (not even SERVICE_REQUEST, SERVICE_ACCEPT or EXT_INFO), so
channel data waits in the channel queues and
connection-layer replies made meanwhile are held as plaintext and sealed,
in order, under the new key right after it; the new keys are STAGED per
direction, each installed at its own NEWKEYS, so the peer's packets still in
flight under the old key open with the old key; strict KEX's sequence reset
holds at every NEWKEYS. Like sshd, channel messages the peer sends after its
own KEXINIT are still processed, and anything else out of place in the
transport range draws UNIMPLEMENTED rather than a disconnect. To an
application a rekey is a transient — `axl_ssh_channel_write()` goes on
accepting up to its limits, so at most a short `AXL_BUSY` — and it allocates
nothing that outlives it: the payload buffer is the connection's one, and
the held queue is allocated with the connection and kept, empty, for the
next exchange. That queue is capped at 128 KiB, which a conforming client
cannot reach and sshd's unbounded queue does not have. Before authentication
a KEXINIT still ends the connection. Every rekey line the server writes is
DEBUG, as sshd's are. `test-ssh-rekey-qemu.sh` drives client- and
server-started exchanges with a real `ssh(1)`, interactive and bulk, byte
for byte.

**An authenticated connection cannot hold a slot for ever.** Two of sshd's
idle rules are ON by default here, where sshd's defaults are off (spec
section 12a.7, deviation 2: sshd spends a process on an idle connection,
this server has 16 slots and never evicts an authenticated one).
`axl_ssh_server_set_unused_timeout()` is `UnusedConnectionTimeout`
(default 300 s): with no channel IN USE — a session whose shell or exec
was accepted, until its slot is freed; one opened and never started does
not count, as sshd skips a LARVAL channel — for longer than that, the
connection ends. `axl_ssh_server_set_client_alive()` is `ClientAliveInterval`
and `ClientAliveCountMax` (default 60 s x 3): after that much silence the
server sends `keepalive@openssh.com` with want_reply — a CHANNEL_REQUEST on
the peer's first channel, else a GLOBAL_REQUEST, as sshd does — and after
the count of them unanswered the connection ends; any packet from the peer
resets the count, any bytes the clock, and the answers (81/82/99/100) are
accepted silently. Both ends are sshd's: silent and abortive (no
DISCONNECT, a reset), nothing charged to the source, and one INFO line in
sshd's words, `terminating inactive connection from user U A port P` or
`Timeout, client not responding from user U A port P`. Both ride the
reaper's sweep, whose tick becomes the finest of 5 s, half the grace and
half of each idle period; a probe due while the peer is not reading is
counted but not queued, and one due while the server itself holds the
input (a channel hook running, or a parse turn queued behind another
connection's authentication hook) is neither (`src/net/axl-ssh-idle.h`
has every rule and its source). Both setters are called before `listen()`.
`test-ssh-idle-unused-qemu.sh`, `test-ssh-idle-alive-qemu.sh` and
`test-ssh-idle-probe-qemu.sh` (one body,
`test/integration/lib/ssh-idle-cases.sh`) prove them against a real
`ssh(1)`: a quiet
interactive session answers its probes and survives, `ssh -N` is ended and
its slot reused, and a peer that stops answering is ended within the drop
bound.
`test-ssh-idle-hostile-pause-qemu.sh`, `test-ssh-idle-hostile-control-qemu.sh`
and `test-ssh-idle-hostile-churn-qemu.sh` (one body,
`lib/ssh-idle-hostile-cases.sh`) prove the cases `ssh(1)` cannot make: a peer
that stops reading a stream, floods requests until the server pauses its
input, and half-closes is ended within the bound (and holds its slot for as
long as it likes with client-alive off); a keepalive due during our own
rekey is held and sent after our NEWKEYS; a peer's KEXINIT waiting behind an
input pause is parsed after the resume. Spec section 12a.5 "As built" has
the numbers and what no test can see.

**One algorithm per slot**, and that is a security decision: an algorithm
matrix is a downgrade surface, and there are no legacy peers to accommodate.
`curve25519-sha256` for kex, `ssh-ed25519` for the host key,
`chacha20-poly1305@openssh.com` for the cipher — and because the cipher is an
AEAD, the MAC name-lists are empty by design. A peer that does not offer our
name in a slot is refused rather than negotiated with.

**Ed25519 comes with the server.** `axl_ssh_server_new()` calls
`axl_pk_enable_ed25519()` before `axl_pk_key_new(AXL_PK_ED25519)` (see
"Every provider is opt-in, in code" above), so an AxlSsh application needs
no flag and no call. If the key still cannot be made the server fails at
construction rather than at key exchange, because there is no second host
key algorithm to fall back to.

### The host key: kept, provisioned, fingerprinted (`axl-ssh-host-key.h`)

`axl_ssh_server_new()` still generates a fresh, in-memory key, so a server
never given another works as before -- and changes identity at every start.
A server that keeps its identity is handed its key before it listens, with
`axl_ssh_server_set_host_key()`, which adopts it (and zeroises the one it
replaces). `axl_ssh_server_host_key_fingerprint()` and
`..._public_line()` report it.

Where the key comes from is `<axl/axl-ssh-host-key.h>`, in two halves, and
which half a function is in is part of its contract:

- **Provider-free** -- the fingerprint (`SHA256:...`, character for character
  what `ssh-keygen -lf` and ssh(1) print), the public line, the
  openssh-key-v1 parser, the file reader, and read / write / delete of the
  default store. SHA-256 and base64 only: a small launcher links them without
  the ~120 KB Ed25519 provider. `AxlTestSshHostKey` asserts the provider is
  absent from an image that calls all of them.
- **Provider-bound** -- `axl_ssh_host_key_load()` (which checks the seed
  derives the stored public key: a flipped seed bit otherwise loads cleanly
  as ANOTHER key), `axl_ssh_host_key_record_from_key()`, for an application
  with a configuration store of its own, and
  `axl_ssh_host_key_nv_load_or_create()`.

**The default store** is the UEFI variable `AxlSshHostKeyEd25519` under its
own vendor GUID (`AXL_SSH_HOST_KEY_GUID_STR`), non-volatile and
boot-services only. What that buys and what it does not:

- the key is not visible through UEFI variable services after
  ExitBootServices, and the OS cannot create, overwrite or delete it;
- it is NOT secret from a privileged OS: it sits unencrypted in the
  platform's variable flash, which many platforms let the OS read;
- clearing NVRAM -- "load defaults", many firmware updates -- removes it,
  and the next start makes a new key that every client sees as changed.

**What load-or-create will not do.** "Absent" is `EFI_NOT_FOUND` and nothing
else; a read error or a stored key that is present but unusable is refused
and NEVER regenerated -- regenerating would hide tampering and teach clients
to accept a changed key. A variable whose raw attributes are anything but
NV+BS is refused by name, runtime access above all (before the first start
the OS can plant one under the GUID), and so is every authenticated bit. A
key made while the random generator was ever seeded from a clock
(`axl_crypto_set_weak_entropy_allowed()`) is served but never stored
(`AXL_SSH_HOST_KEY_EPHEMERAL`), and so is one whose write fails or does not
read back. The stored value is a private, versioned format, not the public
struct.

**Files.** `axl_ssh_host_key_parse()` accepts the openssh-key-v1 file
`ssh-keygen -t ed25519 -N ''` writes, in UTF-8 or the Shell's UTF-16LE, LF
or CRLF, and checks it as OpenSSH's `sshkey.c` does, plus two stricter
checks. It refuses, each by name, a passphrase-protected key, every other key
type, and PKCS#8 -- with the command that converts it,
`ssh-keygen -p -N '' -f FILE` (a seed-only PKCS#8 cannot be fingerprinted
without the provider). FAT has no permissions, so a key file on an ESP or a
stick is readable by everything that can read the medium;
`axl_ssh_host_key_nv_write()` moves one into the variable store instead.

### How a connection ends: the DISCONNECT is delivered

Before P5 every SSH_MSG_DISCONNECT was followed by an abortive close on the
same stack, and the reset raced the packet. Now a DISCONNECT sent on a
healthy stream puts the connection in `ST_LINGER`: nothing more is sealed or
parsed, its channels, principal and hooks are released at once, the
transmit queue keeps flushing, and the server waits for the peer to hang up
-- ssh(1) prints the reason and closes -- then resets it. A peer that does
not hang up within `AXL_SSH_DISCONNECT_LINGER_MS` (1 s) is reset anyway, and
nothing it sends moves that deadline. One server-wide timer carries every
deadline; if it cannot be armed, every lingering connection is reset at
once. A lingering connection is the first eviction candidate, in any state,
and its source's penalty accrues when the DISCONNECT is sent, not when the
socket goes.

The texts are sshd's where sshd has one -- both authentication caps send
"Too many authentication failures" -- with RFC 4253's reason codes (sshd
sends 2 for everything). Abortive, with no DISCONNECT, as sshd's
`cleanup_exit` is: an input flood, the login grace, the idle timeouts,
accept-time refusals and eviction, and `axl_ssh_server_free()`. A stream
that already lost a run is reset at once, since the peer could not decrypt
the DISCONNECT. DEBUG lines tell the endings apart (`linger: peer closed
after N ms, disconnect sent` -- handed to TCP and its send completed, which
is not proof the peer read it; `linger: peer closed before the disconnect
was sent`; `linger: no close within 1000 ms; reset`);
`test-ssh-disconnect-qemu.sh` grades them.

### OpenSSH is the conformance oracle

Most protocols in this tree never get a second implementation to test
against, so a codec bug that is self-consistent passes both ends of our own
tests. SSH has OpenSSH, and `test/integration/test-ssh-transport-qemu.sh`
drives a real `ssh -vvv` from the host against the guest and grades **ssh's
own transcript**, never the guest's log.

That distinction is not theoretical. Two sabotages, measured:

| sabotage | unit tests | `ssh(1)` |
|---|---|---|
| the two ChaCha20 keys swapped | **green** | fails at the first *encrypted* message |
| `Q_C` and `Q_S` swapped in the exchange hash | **green** | fails at NEWKEYS |

Both produce a server that talks to itself perfectly. Only the oracle sees it.
