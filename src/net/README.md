TCP sockets, UDP sockets, socket abstraction layer, URL parsing, HTTP
server, HTTP client, TLS, and network utilities (IPv4 address helpers,
interface enumeration, ping).

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

```c
// Auto-init: load drivers, run DHCP, wait for IP
if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
    axl_printf("Network not available\n");
    return -1;
}

// Or configure static IP
uint8_t ip[]      = {192, 168, 1, 100};
uint8_t netmask[] = {255, 255, 255, 0};
uint8_t gateway[] = {192, 168, 1, 1};
axl_net_set_static_ip(0, ip, netmask, gateway);
```

## Socket Layer

`AxlSocket` is the recommended socket API for new code — a
GLib-`GSocket`-shaped abstraction over both stream (TCP) and datagram
(UDP) transports, with rich address types and blocking and async
forms. It delegates to the low-level `AxlTcp` / `AxlUdpSocket`
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
bool on_client(AxlSocket *client, int status, void *data) {
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
> above. `AxlTcp` and `AxlUdpSocket` are the primitives underneath --
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
bool on_client(AxlTcp *client, int status, void *data) {
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

static bool on_connected(AxlTcp *sock, int status, void *data) {
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

Fire-and-forget datagram sending and request-response patterns.

```c
AXL_AUTOPTR(AxlUdpSocket) sock = NULL;
axl_udp_open(&sock, 0);  // ephemeral port

AxlIPv4Address dest;
axl_ipv4_parse("192.168.1.100", dest.addr);

// Fire-and-forget
axl_udp_send(sock, &dest, 514, msg, msg_len);

// Request-response (e.g., DNS query)
char reply[512];
size_t reply_len;
axl_udp_sendrecv(sock, &dest, 53, query, query_len,
                 reply, sizeof(reply), &reply_len, 3000);
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

The server supports middleware, WebSocket endpoints, authentication,
response caching, and streaming uploads. See the API reference for
details.

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
