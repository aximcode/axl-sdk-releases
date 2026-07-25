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
- `<axl/axl-tls.h>` — TLS support (optional, requires `AXL_TLS=1`)

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
`FIRMWARE_ONLY`. `SWEEP_DIR` also honors `load_deps` (co-load a
candidate's declared dependencies from the sweep dir's
`netload-drivers.json5` sidecar first — a USB-RNDIS/CDC NIC whose driver
needs a companion) and `verify` (`REACHABLE` keeps trying drivers until
one both configures **and** passes a ping/gateway/DNS reachability
check, not just gets an address). An optional `on_driver` callback
reports each driver the sweep touches — a `TRYING` event before each
load, a result event after, and one per co-loaded dependency — so a tool
can render its own findings (and warn of a slow connect) without that
rendering living in the library.

**`netload -a` IS this engine.** netload runs its own firmware-first
probe and saved-config replay, then hands the staged sweep to
`axl_net_auto_init_opts` (with `skip_firmware_first`) and renders its
findings table from the `on_driver` callback — so netload's `-a`
integration suite is the engine's end-to-end coverage.

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
  doesn't support the notify falls back to a 1 s tick). On firmware
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

- `axl_net_get_driver_info(mac, &info)` — the bound driver name +
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

**Session-scoped cancellation:**

Every `axl_tcp_*_async` call accepts an optional `AxlCancellable *`.
Share one cancellable across all ops tied to a session — closing the
session cancels every in-flight op at once, each firing its callback
with `status == AXL_CANCELLED`.

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
// stops and its callback fires exactly once with AXL_CANCELLED.
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
AXL_TEARDOWN_GRACEFUL)` closes in-flight connections *gracefully* (FIN) and,
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
> unchanged (FIN).

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
close(aborted)). Per-mount single-in-flight PUT — concurrent
PUTs to the same mount are refused rather than silently
trampling each other's state.

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

HTTPS URLs are automatically detected when built with `AXL_TLS=1`.

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

Optional TLS 1.2 support using [mbedTLS](https://github.com/Mbed-TLS/mbedtls)
3.6. Provides HTTPS server/client, self-signed certificate generation,
and transparent TCP encryption.

**Build requirement**: `make AXL_TLS=1` (adds ~200KB to the binary).
Without this flag, all TLS functions return -1/NULL/false.

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

### Certificate Generation

`axl_tls_generate_self_signed` creates an ECDSA P-256 certificate
with SHA-256 signature:

- **Subject**: `CN=<name>,O=AximCode`
- **Validity**: current year to +10 years
- **SubjectAltName**: `DNS:localhost`, `IP:127.0.0.1`, plus any
  provided IP addresses
- **Output**: DER-encoded certificate and private key (caller frees)

### Entropy

mbedTLS needs random numbers for key generation and TLS handshakes.
AXL provides entropy via:

1. **EFI_RNG_PROTOCOL** (hardware RNG) — preferred, used when available
2. **Software fallback** — system time + monotonic counter mixing.
   A warning is logged when the fallback is used.

### Security Considerations

- Self-signed certificates are **not trusted** by browsers or standard
  TLS clients. Use `curl --insecure` or configure trust-on-first-use.
- Certificate verification is **disabled** for client connections
  (`MBEDTLS_SSL_VERIFY_NONE`). This is appropriate for BMC/embedded
  use but not for public internet TLS.
- The software entropy fallback is **not cryptographically strong**.
  For production use on hardware without an RNG, consider providing
  your own entropy source.
